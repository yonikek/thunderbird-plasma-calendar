/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

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
const LOOK_BEHIND_DAYS = 180;
const LOOK_AHEAD_DAYS = 730;

let nativePort = null;
let refreshInProgress = false;
let refreshQueued = false;
let debounceTimer = null;
let firstPendingAt = 0;
let reconnectTimer = null;
let reconnectDelayMs = RECONNECT_BASE_MS;

function makeDateWindow() {
  const now = Date.now();
  return {
    start: new Date(now - LOOK_BEHIND_DAYS * 86400000).toISOString(),
    end: new Date(now + LOOK_AHEAD_DAYS * 86400000).toISOString()
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
    const range = makeDateWindow();
    const snapshot = await browser.tbCalendarBridge.getSnapshot(range.start, range.end);
    if (nativePort) {
      nativePort.postMessage({ type: "snapshot", snapshot });
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
connectNativeHost();
setInterval(refreshCalendarCache, REFRESH_INTERVAL_MS);
