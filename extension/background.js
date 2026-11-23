/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

// CalendarSettings (settings.js) is loaded before this script.

const NATIVE_HOST = "org.kde.plasma.thunderbird_calendar";
const REFRESH_INTERVAL_MS = 15 * 60 * 1000;
const CHANGE_DEBOUNCE_MS = 2500;
const CHANGE_MAX_WAIT_MS = 30 * 1000;
const RECONNECT_BASE_MS = 15 * 1000;
const RECONNECT_MAX_MS = 15 * 60 * 1000;
const SNAPSHOT_RETRY_BASE_MS = 5 * 1000;
const SNAPSHOT_RETRY_MAX_MS = 5 * 60 * 1000;
const MAX_AUTOMATIC_SNAPSHOT_RETRIES = 5;
const PROFILE_LOCK_RETRY_BASE_MS = 60 * 1000;
const PROFILE_LOCK_RETRY_MAX_MS = 5 * 60 * 1000;
const NATIVE_ACK_TIMEOUT_MS = 60 * 1000;
const MAX_NATIVE_MESSAGE_BYTES = 32 * 1024 * 1024;
const ITEM_CHANGE_DEBOUNCE_MS = 120;
const MAX_QUEUED_ITEM_CHANGES = 5000;
const MAX_ITEM_CHANGES_PER_MESSAGE = 1000;
const ITEM_CHANGE_RETRY_MS = 1500;
const ITEM_CHANGE_RETRY_MAX_MS = 60 * 1000;
const MAX_AUTOMATIC_ITEM_CHANGE_RETRIES = 5;

const FULL_SNAPSHOT_FALLBACK_CODES = new Set([
  "FULL_SNAPSHOT_REQUIRED",
  "NATIVE_MESSAGE_TOO_LARGE",
  "JOURNAL_RECORD_TOO_LARGE",
  "JOURNAL_WRITE_ERROR",
  "JOURNAL_APPEND_FAILED",
  "JOURNAL_COMPACTION_FAILED"
]);
const PERMANENT_NATIVE_ERROR_CODES = new Set([
  "NATIVE_MESSAGE_TOO_LARGE",
  "EVENT_LIMIT_EXCEEDED",
  "INVALID_EVENT_CHANGES",
  "INVALID_MESSAGE_TYPE",
  "PROFILE_LOCK_ERROR"
]);

let nativePort = null;
let refreshInProgress = false;
let refreshQueued = false;
let fullRefreshNext = false;
let debounceTimer = null;
let firstPendingAt = 0;
let reconnectTimer = null;
let reconnectDelayMs = RECONNECT_BASE_MS;
let snapshotRetryTimer = null;
let snapshotRetryDelayMs = SNAPSHOT_RETRY_BASE_MS;
let snapshotRetryAttempts = 0;
let profileLockBlocked = false;
let profileLockRetryTimer = null;
let profileLockRetryDelayMs = PROFILE_LOCK_RETRY_BASE_MS;
let itemChangeRetryAttempts = 0;
let itemChangeRetriesExhausted = false;
let snapshotRecoveryPending = false;
let debugLogsEnabled = false;
let nextRequestId = 1;
const pendingAcks = new Map();
const pendingItemChanges = new Map();
let itemChangeFlushTimer = null;
let itemChangeRetryTimer = null;
let itemChangeFlushInProgress = false;

function debugLog(level, ...args) {
  if (debugLogsEnabled) {
    console[level]("Thunderbird Plasma Calendar:", ...args);
  }
}

function makeDateWindow(settings) {
  const today = new Date(Date.now());
  const year = today.getFullYear();
  const month = today.getMonth();
  const day = today.getDate();
  return {
    start: new Date(year, month, day - settings.lookBehindDays).toISOString(),
    end: new Date(year, month, day + settings.lookAheadDays + 1).toISOString()
  };
}

function settlePendingAcks(error) {
  for (const [requestId, pending] of pendingAcks) {
    clearTimeout(pending.timer);
    pendingAcks.delete(requestId);
    pending.reject(error);
  }
}

function disconnectNativePort() {
  const port = nativePort;
  nativePort = null;
  if (port) {
    try {
      port.disconnect();
    } catch (error) {
      debugLog("warn", "could not disconnect native host:", error);
    }
  }
}

function formatMiB(bytes) {
  return `${(bytes / (1024 * 1024)).toFixed(2)} MiB`;
}

function sendNativeMessageAndWait(message) {
  if (!nativePort) {
    return Promise.reject(new Error("native host is not connected"));
  }

  const requestId = String(nextRequestId++);
  const completeMessage = {
    ...message,
    requestId,
    debugLogs: debugLogsEnabled
  };
  // Native messaging uses UTF-8 JSON frames. Measure the complete envelope,
  // including request metadata, before handing it to the browser's native host.
  const bytes = new TextEncoder().encode(JSON.stringify(completeMessage)).byteLength;
  if (bytes > MAX_NATIVE_MESSAGE_BYTES) {
    const subject = completeMessage.type === "snapshot" ? "Calendar snapshot" : "Calendar event update";
    const error = new Error(
      `${subject} is too large for the native-messaging limit: ${formatMiB(bytes)} ` +
      `(${bytes.toLocaleString()} bytes; maximum ${formatMiB(MAX_NATIVE_MESSAGE_BYTES)}, ` +
      `${MAX_NATIVE_MESSAGE_BYTES.toLocaleString()} bytes). Reduce the look-back/look-ahead range, ` +
      `disable description/location export, or enable text truncation.`
    );
    error.name = "NativeMessageTooLargeError";
    error.code = "NATIVE_MESSAGE_TOO_LARGE";
    error.actualBytes = bytes;
    error.maxBytes = MAX_NATIVE_MESSAGE_BYTES;
    return Promise.reject(error);
  }

  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pendingAcks.delete(requestId);
      reject(new Error(`native host did not acknowledge ${completeMessage.type} ${requestId} within ${NATIVE_ACK_TIMEOUT_MS} ms`));
      // A timed-out host may be stuck processing an earlier frame. Restart it
      // rather than queueing more messages behind an unknown operation.
      disconnectNativePort();
    }, NATIVE_ACK_TIMEOUT_MS);
    pendingAcks.set(requestId, { resolve, reject, timer, bytes, type: completeMessage.type });

    try {
      nativePort.postMessage(completeMessage);
    } catch (error) {
      clearTimeout(timer);
      pendingAcks.delete(requestId);
      reject(error);
    }
  });
}

function sendSnapshotAndWait(snapshot) {
  return sendNativeMessageAndWait({ type: "snapshot", snapshot });
}

function eventChangeKey(change) {
  return `${String(change.calendarId)}\u0000${String(change.event.uid)}`;
}

function scheduleItemChangeFlush(delay = ITEM_CHANGE_DEBOUNCE_MS) {
  if (profileLockBlocked || itemChangeRetriesExhausted || snapshotRecoveryPending
      || itemChangeFlushTimer !== null || itemChangeRetryTimer !== null || pendingItemChanges.size === 0) {
    return;
  }
  itemChangeFlushTimer = setTimeout(() => {
    itemChangeFlushTimer = null;
    flushItemChanges();
  }, delay);
}

function queueItemChanges(changes) {
  if (!Array.isArray(changes) || changes.length === 0) {
    return;
  }

  // Reject oversized batches before iterating or growing the pending map.
  // The Experiment API normally enforces this limit too, but this boundary
  // must remain safe if a future caller or a malformed event bypasses it.
  if (changes.length > MAX_QUEUED_ITEM_CHANGES) {
    pendingItemChanges.clear();
    fullRefreshNext = true;
    debugLog("warn", "direct item-change batch exceeded its safety limit; falling back to a full snapshot");
    requestRefresh();
    return;
  }

  const validChanges = [];
  const incomingKeys = new Set();
  for (const change of changes) {
    if (!change || !["add", "modify", "remove"].includes(change.operation)
        || typeof change.calendarId !== "string"
        || !change.event || typeof change.event.uid !== "string" || !change.event.uid) {
      debugLog("warn", "ignored malformed direct calendar item change:", change);
      continue;
    }
    validChanges.push(change);
    incomingKeys.add(eventChangeKey(change));
  }

  let additionalKeys = 0;
  for (const key of incomingKeys) {
    if (!pendingItemChanges.has(key)) {
      additionalKeys++;
    }
  }
  if (pendingItemChanges.size + additionalKeys > MAX_QUEUED_ITEM_CHANGES) {
    pendingItemChanges.clear();
    fullRefreshNext = true;
    debugLog("warn", "direct item-change queue would exceed its safety limit; falling back to a full snapshot");
    requestRefresh();
    return;
  }

  for (const change of validChanges) {
    // Keep the final desired state for each calendar/UID during the short
    // debounce window. The host resolves add-vs-modify against its own snapshot.
    pendingItemChanges.set(eventChangeKey(change), change);
  }

  if (itemChangeRetriesExhausted) {
    // A new calendar notification is a new opportunity to retry a previously
    // paused transient failure. Old queued changes are kept and coalesced with it.
    itemChangeRetriesExhausted = false;
    itemChangeRetryAttempts = 0;
  }
  scheduleItemChangeFlush();
}

async function flushItemChanges() {
  if (!nativePort || profileLockBlocked || itemChangeRetriesExhausted || snapshotRecoveryPending
      || pendingItemChanges.size === 0 || itemChangeFlushInProgress) {
    return;
  }
  if (refreshInProgress) {
    return; // refreshCalendarCache() schedules this again in its finally block.
  }

  itemChangeFlushInProgress = true;
  const batchEntries = Array.from(pendingItemChanges.entries()).slice(0, MAX_ITEM_CHANGES_PER_MESSAGE);
  for (const [key] of batchEntries) {
    pendingItemChanges.delete(key);
  }
  const changes = batchEntries.map(([, change]) => change);

  try {
    const directUpdateStarted = performance.now();
    const response = await sendNativeMessageAndWait({ type: "eventChanges", changes });
    const directUpdateAcknowledged = performance.now();
    if (debugLogsEnabled) {
      debugLog("log", `sent ${changes.length} direct item changes; host applied ${Number(response.appliedChanges || 0)}; revision ${response.revision || "unchanged"}`);
      logDirectUpdateTiming(changes.length, directUpdateStarted, directUpdateAcknowledged, response);
    }
    reconnectDelayMs = RECONNECT_BASE_MS;
    itemChangeRetryAttempts = 0;
    itemChangeRetriesExhausted = false;
    clearProfileLockBlocked();
  } catch (error) {
    const code = error && error.code;
    const restoreFailedBatch = () => {
      for (const [key, change] of batchEntries) {
        if (!pendingItemChanges.has(key)) {
          pendingItemChanges.set(key, change);
        }
      }
    };

    if (code === "ACTIVE_PROFILE_LOCKED") {
      restoreFailedBatch();
      enterProfileLockBlocked(error);
    } else if (FULL_SNAPSHOT_FALLBACK_CODES.has(code)) {
      // A compacted/full snapshot is a safe fallback for deltas that cannot be
      // represented or appended to the current journal. Keep queued updates:
      // the refresh will absorb them where possible, and any remaining direct
      // changes are harmlessly coalesced against the new baseline.
      restoreFailedBatch();
      snapshotRecoveryPending = true;
      fullRefreshNext = true;
      debugLog("warn", `native host requested snapshot recovery (${code}); pausing direct updates until the full snapshot is committed`);
      requestRefresh();
    } else if (PERMANENT_NATIVE_ERROR_CODES.has(code)) {
      // Do not retry the same invalid or unsupported update indefinitely.
      // Report this even when verbose debug logging is disabled.
      console.error("Thunderbird Plasma Calendar:", error.message || error);
      itemChangeRetriesExhausted = true;
    } else {
      restoreFailedBatch();
      itemChangeRetryAttempts++;
      if (itemChangeRetryAttempts > MAX_AUTOMATIC_ITEM_CHANGE_RETRIES) {
        itemChangeRetriesExhausted = true;
        console.error(
          "Thunderbird Plasma Calendar: direct calendar updates have been paused after " +
          `${MAX_AUTOMATIC_ITEM_CHANGE_RETRIES} failed attempts. ${pendingItemChanges.size} change(s) remain queued; ` +
          "a new calendar change or manual refresh will retry them. Last error: " + (error.message || error)
        );
      } else {
        debugLog("error", "direct calendar update failed; will retry with bounded backoff:", error);
        if (itemChangeRetryTimer === null) {
          const delay = Math.min(
            ITEM_CHANGE_RETRY_MS * (2 ** (itemChangeRetryAttempts - 1)),
            ITEM_CHANGE_RETRY_MAX_MS
          );
          itemChangeRetryTimer = setTimeout(() => {
            itemChangeRetryTimer = null;
            scheduleItemChangeFlush(0);
          }, delay);
        }
      }
    }
  } finally {
    itemChangeFlushInProgress = false;
    if (pendingItemChanges.size > 0 && itemChangeRetryTimer === null
        && !profileLockBlocked && !itemChangeRetriesExhausted && !snapshotRecoveryPending) {
      scheduleItemChangeFlush(0);
    }
    if (refreshQueued) {
      refreshQueued = false;
      requestRefresh();
    }
  }
}

async function refreshCalendarCache() {
  if (!nativePort) {
    return;
  }
  if (profileLockBlocked) {
    scheduleProfileLockRetry();
    return;
  }
  if (refreshInProgress || itemChangeFlushInProgress) {
    refreshQueued = true;
    return;
  }

  refreshInProgress = true;
  let snapshotBuilt = false;
  let requestedFullRefresh = false;
  try {
    const started = performance.now();
    requestedFullRefresh = fullRefreshNext;
    const useCache = !requestedFullRefresh;
    fullRefreshNext = false;
    const settings = await CalendarSettings.load();
    debugLogsEnabled = settings.enableDebugLogs;
    browser.tbCalendarBridge.setDebugLogging(debugLogsEnabled);
    const range = makeDateWindow(settings);
    const snapshot = await browser.tbCalendarBridge.getSnapshot(range.start, range.end, {
      includeDescriptions: settings.exportDescriptions,
      includeLocations: settings.exportLocations,
      truncateDescriptions: settings.truncateDescriptions,
      truncateLocations: settings.truncateLocations,
      maxDescriptionChars: settings.maxDescriptionChars,
      maxLocationChars: settings.maxLocationChars,
      queryTimeoutSeconds: settings.queryTimeoutSeconds,
      enableDebugLogs: settings.enableDebugLogs,
      useCache
    });
    snapshotBuilt = true;
    const readFinished = performance.now();
    const delivery = await sendSnapshotAndWait(snapshot);
    const acknowledged = performance.now();
    clearProfileLockBlocked();
    snapshotRecoveryPending = false;
    // A successful authoritative snapshot repairs any earlier direct-update
    // error state, because it now represents the provider's current contents.
    itemChangeRetryAttempts = 0;
    itemChangeRetriesExhausted = false;
    if (!snapshot.calendars.some(calendar => calendar.retryable === true)) {
      snapshotRetryAttempts = 0;
    }
    if (snapshot.calendars.some(calendar => calendar.retryable === true)) {
      scheduleSnapshotRetry();
    } else {
      clearSnapshotRetry();
    }

    if (settings.enableDebugLogs) {
      logTiming(snapshot, started, readFinished, acknowledged, delivery);
    }
  } catch (error) {
    if (requestedFullRefresh && !snapshotBuilt) {
      fullRefreshNext = true;
    }
    const code = error && error.code;
    if (code === "ACTIVE_PROFILE_LOCKED") {
      enterProfileLockBlocked(error);
    } else if (PERMANENT_NATIVE_ERROR_CODES.has(code)
        || (error && error.name === "SnapshotTooLargeError")
        || String(error && (error.message || error) || "").includes("snapshot too large")) {
      // Unsupported, invalid or oversized snapshots cannot succeed by retrying
      // the same payload. Keep this visible without requiring debug logging.
      console.error("Thunderbird Plasma Calendar:", error.message || error);
      clearSnapshotRetry();
    } else {
      debugLog("error", "calendar refresh/delivery failed:", error);
      scheduleSnapshotRetry(error);
    }
  } finally {
    refreshInProgress = false;
    if (pendingItemChanges.size > 0) {
      scheduleItemChangeFlush(0);
    }
    if (refreshQueued) {
      refreshQueued = false;
      requestRefresh();
    }
  }
}

function logTiming(snapshot, started, readFinished, acknowledged, delivery) {
  try {
    let items = 0;
    for (const calendar of snapshot.calendars) {
      items += Array.isArray(calendar.events) ? calendar.events.length : 0;
    }
    const bytes = delivery && Number.isFinite(delivery.clientPayloadBytes)
      ? delivery.clientPayloadBytes
      : 0;
    console.log(
      "Thunderbird Plasma Calendar: refresh took " + Math.round(acknowledged - started) +
      " ms (read " + Math.round(readFinished - started) + " ms, native host acknowledgement " +
      Math.round(acknowledged - readFinished) + " ms), " + snapshot.calendars.length +
      " calendars, " + items + " items, " + formatMiB(bytes)
    );

    const phases = delivery && delivery.hostTimingsMs;
    if (phases && typeof phases === "object") {
      const phase = (name) => Number(phases[name] || 0).toFixed(1);
      console.log(
        "Thunderbird Plasma Calendar: native host phases (ms): " +
        "wait before message " + phase("messageWaitMs") + ", frame read " + phase("frameReadMs") +
        ", JSON parse " + phase("jsonParseMs") +
        ", validation " + phase("validationMs") + ", content hash " + phase("contentHashMs") +
        ", event diff " + phase("eventDiffMs") + " (" + Number(phases.eventDeltaCount || 0) + " event changes)" +
        ", cache directory " + phase("cacheDirectoryMs") +
        ", existing cache read " + phase("existingCacheReadMs") +
        ", cache preparation total " + phase("cachePreparationMs") +
        ", snapshot serialisation " + phase("snapshotSerializationMs") +
        ", disk commit " + phase("diskCommitMs") + ", D-Bus notification " + phase("dbusNotifyMs") +
        ", host processing before response write " + phase("hostProcessingMs") +
        ", remaining round-trip overhead ~" + Math.max(0,
          (acknowledged - readFinished) - Number(phases.frameReadMs || 0) - Number(phases.hostProcessingMs || 0)
        ).toFixed(1) + " ms (includes response write; host logs exact total)" +
        ", cache write " + Number(phases.cacheWriteBytes || 0) + " bytes" +
        (phases.unchanged === true ? ", cache unchanged" : ", cache updated")
      );
    }
  } catch (error) {
    debugLog("warn", "could not log timing:", error);
  }
}

function logDirectUpdateTiming(changeCount, started, acknowledged, response) {
  const phases = response && response.hostTimingsMs;
  if (!phases || typeof phases !== "object") {
    return;
  }

  const phase = name => Number(phases[name] || 0).toFixed(1);
  const acknowledgementMs = acknowledged - started;
  const hostFrameReadMs = Number(phases.frameReadMs || 0);
  const hostProcessingMs = Number(phases.hostProcessingMs || 0);
  const transportOverheadMs = Math.max(0, acknowledgementMs - hostFrameReadMs - hostProcessingMs);

  debugLog("log",
    `direct update acknowledgement took ${Math.round(acknowledgementMs)} ms; ` +
    `${changeCount} queued changes, ${Number(phases.eventDeltaCount || 0)} applied deltas, ` +
    `${Number(phases.affectedCalendarCount || 0)} affected calendars`
  );
  debugLog("log",
    "native host direct-update phases (ms): " +
    "frame read " + phase("frameReadMs") +
    ", JSON parse " + phase("jsonParseMs") +
    ", cache directory " + phase("cacheDirectoryMs") +
    ", recovery cache read " + phase("existingCacheReadMs") +
    ", recovery cache validation " + phase("cacheValidationMs") +
    ", calendar index " + phase("calendarIndexMs") +
    ", change validation/grouping " + phase("changeValidationMs") +
    ", event lookup/index " + phase("eventIndexMs") +
    ", event application " + phase("eventApplyMs") +
    ", event-array rebuild " + phase("eventArrayRebuildMs") +
    ", event diff total " + phase("eventDiffMs") +
    ", revision hash " + phase("contentHashMs") +
    ", snapshot serialisation " + phase("snapshotSerializationMs") +
    ", disk commit " + phase("diskCommitMs")
    + ", D-Bus notification " + phase("dbusNotifyMs")
    + ", journal serialisation " + phase("journalSerializationMs")
    + ", journal append " + phase("journalAppendMs")
    + ", journal fsync " + phase("journalFsyncMs")
    + ", journal compaction " + phase("journalCompactionMs")
    + ", journal records " + Number(phases.journalRecordCount || 0)
    + ", journal size " + Number(phases.journalFileBytes || 0) + " bytes"
    + ", journal write " + Number(phases.journalWriteBytes || 0) + " bytes"
    + (phases.journalCompacted === true ? ", journal compacted" : ", journal not compacted")
    + ", host processing " + phase("hostProcessingMs")
    + ", transport/extension overhead ~" + transportOverheadMs.toFixed(1) +
    " ms, input changes " + Number(phases.directChangeCount || changeCount) +
    ", affected calendars " + Number(phases.affectedCalendarCount || 0) +
    ", applied deltas " + Number(phases.eventDeltaCount || 0) +
    ", cache write " + Number(phases.cacheWriteBytes || 0) + " bytes" +
    (phases.unchanged === true ? ", cache unchanged" : ", cache updated")
  );
}

function requestRefresh() {
  if (profileLockBlocked) {
    scheduleProfileLockRetry();
    return;
  }
  clearSnapshotRetry();
  const now = Date.now();
  if (debounceTimer === null) {
    firstPendingAt = now;
  } else {
    clearTimeout(debounceTimer);
  }
  const wait = Math.max(0, Math.min(
    CHANGE_DEBOUNCE_MS,
    firstPendingAt + CHANGE_MAX_WAIT_MS - now
  ));
  debounceTimer = setTimeout(() => {
    debounceTimer = null;
    refreshCalendarCache();
  }, wait);
}

function clearSnapshotRetry() {
  if (snapshotRetryTimer !== null) {
    clearTimeout(snapshotRetryTimer);
    snapshotRetryTimer = null;
  }
  snapshotRetryDelayMs = SNAPSHOT_RETRY_BASE_MS;
  snapshotRetryAttempts = 0;
}

function scheduleSnapshotRetry(error) {
  if (snapshotRetryTimer !== null) {
    return;
  }
  if (snapshotRetryAttempts >= MAX_AUTOMATIC_SNAPSHOT_RETRIES) {
    console.error(
      "Thunderbird Plasma Calendar: automatic snapshot retries paused after " +
      `${MAX_AUTOMATIC_SNAPSHOT_RETRIES} failed attempts. A new calendar change or manual refresh will retry. ` +
      "Last error: " + (error && (error.message || error) || "unknown error")
    );
    return;
  }
  snapshotRetryAttempts++;
  const delay = snapshotRetryDelayMs;
  snapshotRetryTimer = setTimeout(() => {
    snapshotRetryTimer = null;
    if (nativePort && !profileLockBlocked) {
      refreshCalendarCache();
    }
  }, delay);
  snapshotRetryDelayMs = Math.min(snapshotRetryDelayMs * 2, SNAPSHOT_RETRY_MAX_MS);
}

function clearProfileLockBlocked() {
  profileLockBlocked = false;
  profileLockRetryDelayMs = PROFILE_LOCK_RETRY_BASE_MS;
  if (profileLockRetryTimer !== null) {
    clearTimeout(profileLockRetryTimer);
    profileLockRetryTimer = null;
  }
}

function enterProfileLockBlocked(error) {
  if (!profileLockBlocked) {
    console.error(
      "Thunderbird Plasma Calendar: another Thunderbird profile appears to be using this calendar cache. " +
      "Only one Thunderbird profile can use the integration at a time. Close the other Thunderbird instance. " +
      "The extension will check again automatically. " + (error && error.message || "")
    );
  }
  profileLockBlocked = true;
  scheduleProfileLockRetry();
}

function scheduleProfileLockRetry() {
  if (profileLockRetryTimer !== null) {
    return;
  }
  const delay = profileLockRetryDelayMs;
  profileLockRetryTimer = setTimeout(() => {
    profileLockRetryTimer = null;
    profileLockBlocked = false;
    fullRefreshNext = true;
    refreshCalendarCache();
  }, delay);
  profileLockRetryDelayMs = Math.min(profileLockRetryDelayMs * 2, PROFILE_LOCK_RETRY_MAX_MS);
}

function scheduleReconnect() {
  if (reconnectTimer !== null) {
    clearTimeout(reconnectTimer);
  }
  reconnectTimer = setTimeout(connectNativeHost, reconnectDelayMs);
  reconnectDelayMs = Math.min(reconnectDelayMs * 2, RECONNECT_MAX_MS);
}

function handleNativeMessage(message) {
  if (!message || typeof message !== "object") {
    debugLog("warn", "native host returned a non-object response");
    return;
  }

  let requestId = message.requestId === undefined || message.requestId === null
    ? null : String(message.requestId);
  // Accept acknowledgements from older installed host versions, which did not
  // echo requestId. There is only ever one snapshot in flight.
  if (requestId === null && pendingAcks.size === 1) {
    requestId = pendingAcks.keys().next().value;
  }
  const pending = requestId === null ? null : pendingAcks.get(requestId);
  if (!pending) {
    debugLog("warn", "received an unmatched native-host response:", message);
    return;
  }

  clearTimeout(pending.timer);
  pendingAcks.delete(requestId);
  if (message.ok === true) {
    reconnectDelayMs = RECONNECT_BASE_MS;
    pending.resolve({
      ...message,
      clientPayloadBytes: pending.bytes
    });
  } else {
    const error = new Error(message.error || `native host rejected ${pending.type || "message"}`);
    if (typeof message.code === "string") {
      error.code = message.code;
    }
    if (Number.isFinite(message.payloadBytes)) {
      error.actualBytes = message.payloadBytes;
    }
    if (Number.isFinite(message.maxBytes)) {
      error.maxBytes = message.maxBytes;
    }
    pending.reject(error);
  }
}

function connectNativeHost() {
  reconnectTimer = null;
  try {
    nativePort = browser.runtime.connectNative(NATIVE_HOST);
    nativePort.onMessage.addListener(handleNativeMessage);
    nativePort.onDisconnect.addListener(() => {
      const reason = browser.runtime.lastError && browser.runtime.lastError.message;
      nativePort = null;
      settlePendingAcks(new Error(reason || "native host disconnected"));
      debugLog("warn", "native host disconnected:", reason || "connection closed");
      scheduleReconnect();
    });
    refreshCalendarCache();
  } catch (error) {
    nativePort = null;
    debugLog("error", "could not connect to native host:", error);
    scheduleReconnect();
  }
}

browser.tbCalendarBridge.onChanged.addListener(requestRefresh);
browser.tbCalendarBridge.onItemChanges.addListener(queueItemChanges);
browser.storage.onChanged.addListener((changes, area) => {
  if (area !== "local") {
    return;
  }
  if (changes.enableDebugLogs) {
    debugLogsEnabled = changes.enableDebugLogs.newValue === true;
    browser.tbCalendarBridge.setDebugLogging(debugLogsEnabled);
  }
  if (Object.keys(changes).some(key => key in CalendarSettings.DEFAULTS || key === "logTiming")) {
    requestRefresh();
  }
});

// Load the persisted diagnostic preference before connecting, so errors during
// initial observer registration follow the user's chosen logging setting.
CalendarSettings.load().then(settings => {
  debugLogsEnabled = settings.enableDebugLogs;
  browser.tbCalendarBridge.setDebugLogging(debugLogsEnabled);
  connectNativeHost();
});

setInterval(() => {
  fullRefreshNext = true;
  refreshCalendarCache();
}, REFRESH_INTERVAL_MS);
