/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

const NATIVE_HOST = "org.kde.plasma.thunderbird_calendar";
const REFRESH_INTERVAL_MS = 3 * 60 * 1000;
const LOOK_BEHIND_DAYS = 180;
const LOOK_AHEAD_DAYS = 730;

let nativePort = null;
let refreshInProgress = false;
let reconnectTimer = null;

function makeDateWindow() {
  const now = Date.now();
  return {
    start: new Date(now - LOOK_BEHIND_DAYS * 86400000).toISOString(),
    end: new Date(now + LOOK_AHEAD_DAYS * 86400000).toISOString()
  };
}

async function refreshCalendarCache() {
  if (!nativePort || refreshInProgress) {
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
  }
}

function scheduleReconnect() {
  if (reconnectTimer !== null) {
    clearTimeout(reconnectTimer);
  }
  reconnectTimer = setTimeout(connectNativeHost, 15000);
}

function connectNativeHost() {
  reconnectTimer = null;
  try {
    nativePort = browser.runtime.connectNative(NATIVE_HOST);
    nativePort.onMessage.addListener(message => {
      if (message && message.ok === false) {
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

connectNativeHost();
setInterval(refreshCalendarCache, REFRESH_INTERVAL_MS);
