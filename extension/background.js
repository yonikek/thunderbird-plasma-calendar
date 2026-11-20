/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

// CalendarSettings (settings.js) is loaded before this script.

const NATIVE_HOST = "org.kde.plasma.thunderbird_calendar";
// Changes are pushed by the Experiment API; the interval is only a safety net.
const REFRESH_INTERVAL_MS = 15 * 60 * 1000;
// A burst of notifications (for example a CalDAV sync) is collapsed into one
// refresh once it has been quiet for CHANGE_DEBOUNCE_MS, but never delayed
// longer than CHANGE_MAX_WAIT_MS so a long sync still shows progress.
const CHANGE_DEBOUNCE_MS = 2500;
const CHANGE_MAX_WAIT_MS = 30 * 1000;
const RECONNECT_BASE_MS = 15 * 1000;
const RECONNECT_MAX_MS = 15 * 60 * 1000;

let nativePort = null;
let refreshInProgress = false;
let refreshQueued = false;
// Set by the safety-net poll: re-read every calendar instead of trusting the
// cache, in case a change notification was missed.
let fullRefreshNext = false;
let debounceTimer = null;
let firstPendingAt = 0;
let reconnectTimer = null;
let reconnectDelayMs = RECONNECT_BASE_MS;

// The window starts and ends at local midnight, so it stays the same for a whole
// day. That lets the Experiment API reuse cached calendars between refreshes;
// the window moves, and everything is read again, once a day.
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

async function refreshCalendarCache() {
  if (!nativePort) {
    return;
  }
  if (refreshInProgress) {
    // Something changed while a snapshot was being built; run once more after.
    refreshQueued = true;
    return;
  }
  refreshInProgress = true;
  try {
    const started = performance.now();
    const useCache = !fullRefreshNext;
    fullRefreshNext = false;
    const settings = await CalendarSettings.load();
    const range = makeDateWindow(settings);
    const snapshot = await browser.tbCalendarBridge.getSnapshot(range.start, range.end, {
      includeDescriptions: settings.exportDescriptions,
      includeLocations: settings.exportLocations,
      logTiming: settings.logTiming,
      useCache
    });
    const read = performance.now();
    if (nativePort) {
      nativePort.postMessage({ type: "snapshot", snapshot });
    }
    if (settings.logTiming) {
      logTiming(snapshot, started, read, performance.now());
    }
  } catch (error) {
    console.error("Thunderbird Plasma Calendar: calendar refresh failed:", error);
  } finally {
    refreshInProgress = false;
    if (refreshQueued) {
      refreshQueued = false;
      requestRefresh();
    }
  }
}

// Runs after the snapshot has been sent, so its extra serialisation never
// delays an update.
function logTiming(snapshot, started, read, sent) {
  try {
    let items = 0;
    for (const calendar of snapshot.calendars) {
      items += calendar.events.length;
    }
    const kib = (JSON.stringify(snapshot).length / 1024).toFixed(1);
    console.log(
      `Thunderbird Plasma Calendar: refresh took ${Math.round(sent - started)} ms ` +
      `(read ${Math.round(read - started)} ms), ${snapshot.calendars.length} calendars, ` +
      `${items} items, about ${kib} KiB`
    );
  } catch (error) {
    console.warn("Thunderbird Plasma Calendar: could not log timing:", error);
  }
}

function requestRefresh() {
  const now = Date.now();
  if (debounceTimer === null) {
    firstPendingAt = now;
  } else {
    clearTimeout(debounceTimer);
  }
  const wait = Math.max(0, Math.min(CHANGE_DEBOUNCE_MS, firstPendingAt + CHANGE_MAX_WAIT_MS - now));
  debounceTimer = setTimeout(() => {
    debounceTimer = null;
    refreshCalendarCache();
  }, wait);
}

function scheduleReconnect() {
  if (reconnectTimer !== null) {
    clearTimeout(reconnectTimer);
  }
  // Back off exponentially so a host that keeps failing does not trigger a
  // full calendar snapshot every few seconds.
  reconnectTimer = setTimeout(connectNativeHost, reconnectDelayMs);
  reconnectDelayMs = Math.min(reconnectDelayMs * 2, RECONNECT_MAX_MS);
}

function connectNativeHost() {
  reconnectTimer = null;
  try {
    nativePort = browser.runtime.connectNative(NATIVE_HOST);
    nativePort.onMessage.addListener(message => {
      if (message && message.ok === true) {
        reconnectDelayMs = RECONNECT_BASE_MS;
      } else if (message && message.ok === false) {
        console.error("Thunderbird Plasma Calendar native host:", message.error || "unknown error");
      }
    });
    nativePort.onDisconnect.addListener(() => {
      const reason = browser.runtime.lastError && browser.runtime.lastError.message;
      console.warn("Thunderbird Plasma Calendar native host disconnected:", reason || "connection closed");
      nativePort = null;
      scheduleReconnect();
    });
    refreshCalendarCache();
  } catch (error) {
    nativePort = null;
    console.error("Thunderbird Plasma Calendar could not connect to its native host:", error);
    scheduleReconnect();
  }
}

browser.tbCalendarBridge.onChanged.addListener(requestRefresh);
// The options page changes the exported date range.
browser.storage.onChanged.addListener((changes, area) => {
  if (area === "local" && Object.keys(changes).some(key => key in CalendarSettings.DEFAULTS)) {
    requestRefresh();
  }
});
connectNativeHost();
setInterval(() => {
  fullRefreshNext = true;
  refreshCalendarCache();
}, REFRESH_INTERVAL_MS);
