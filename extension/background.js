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
const NATIVE_ACK_TIMEOUT_MS = 60 * 1000;
const MAX_NATIVE_MESSAGE_BYTES = 32 * 1024 * 1024;

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
let debugLogsEnabled = false;
let nextRequestId = 1;
const pendingAcks = new Map();

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

function sendSnapshotAndWait(snapshot) {
  if (!nativePort) {
    return Promise.reject(new Error("native host is not connected"));
  }

  const requestId = String(nextRequestId++);
  const message = { type: "snapshot", requestId, snapshot };
  // Native messaging uses UTF-8 JSON frames; reject oversized payloads before
  // sending so that a host-side size error can never lose its request ID.
  const bytes = new TextEncoder().encode(JSON.stringify(message)).byteLength;
  if (bytes > MAX_NATIVE_MESSAGE_BYTES) {
    const error = new Error(`snapshot exceeds the ${MAX_NATIVE_MESSAGE_BYTES} byte native-messaging limit`);
    error.name = "SnapshotTooLargeError";
    return Promise.reject(error);
  }

  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pendingAcks.delete(requestId);
      reject(new Error(`native host did not acknowledge snapshot ${requestId} within ${NATIVE_ACK_TIMEOUT_MS} ms`));
      // A timed-out host may be stuck processing an earlier frame. Restart it
      // rather than queueing more snapshots behind an unknown operation.
      disconnectNativePort();
    }, NATIVE_ACK_TIMEOUT_MS);
    pendingAcks.set(requestId, { resolve, reject, timer });

    try {
      nativePort.postMessage(message);
    } catch (error) {
      clearTimeout(timer);
      pendingAcks.delete(requestId);
      reject(error);
    }
  });
}

async function refreshCalendarCache() {
  if (!nativePort) {
    return;
  }
  if (refreshInProgress) {
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
      truncateText: settings.truncateText,
      maxDescriptionChars: settings.maxDescriptionChars,
      maxLocationChars: settings.maxLocationChars,
      queryTimeoutSeconds: settings.queryTimeoutSeconds,
      enableDebugLogs: settings.enableDebugLogs,
      useCache
    });
    snapshotBuilt = true;
    const readFinished = performance.now();
    await sendSnapshotAndWait(snapshot);
    const acknowledged = performance.now();
    if (snapshot.calendars.some(calendar => calendar.retryable === true)) {
      scheduleSnapshotRetry();
    } else {
      clearSnapshotRetry();
    }

    if (settings.enableDebugLogs) {
      logTiming(snapshot, started, readFinished, acknowledged);
    }
  } catch (error) {
    if (requestedFullRefresh && !snapshotBuilt) {
      fullRefreshNext = true;
    }
    debugLog("error", "calendar refresh/delivery failed:", error);
    if (error && error.name !== "SnapshotTooLargeError" && !String(error.message || error).includes("snapshot too large")) {
      scheduleSnapshotRetry();
    }
  } finally {
    refreshInProgress = false;
    if (refreshQueued) {
      refreshQueued = false;
      requestRefresh();
    }
  }
}

function logTiming(snapshot, started, readFinished, acknowledged) {
  try {
    let items = 0;
    for (const calendar of snapshot.calendars) {
      items += Array.isArray(calendar.events) ? calendar.events.length : 0;
    }
    const json = JSON.stringify(snapshot);
    const kib = (new TextEncoder().encode(json).byteLength / 1024).toFixed(1);
    console.log(
      "Thunderbird Plasma Calendar: refresh took " + Math.round(acknowledged - started) +
      " ms (read " + Math.round(readFinished - started) + " ms, native host acknowledgement " +
      Math.round(acknowledged - readFinished) + " ms), " + snapshot.calendars.length +
      " calendars, " + items + " items, " + kib + " KiB"
    );
  } catch (error) {
    debugLog("warn", "could not log timing:", error);
  }
}

function requestRefresh() {
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
}

function scheduleSnapshotRetry() {
  if (snapshotRetryTimer !== null) {
    return;
  }
  snapshotRetryTimer = setTimeout(() => {
    snapshotRetryTimer = null;
    // A disconnected port already has its own reconnect timer. Its successful
    // connection performs an initial refresh, so do not reset that backoff.
    if (nativePort) {
      refreshCalendarCache();
    }
  }, snapshotRetryDelayMs);
  snapshotRetryDelayMs = Math.min(snapshotRetryDelayMs * 2, SNAPSHOT_RETRY_MAX_MS);
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
    pending.resolve(message);
  } else {
    pending.reject(new Error(message.error || "native host rejected the snapshot"));
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
