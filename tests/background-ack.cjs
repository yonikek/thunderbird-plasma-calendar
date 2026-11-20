/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

let onPortMessage;
let onPortDisconnect;
let sentMessage = null;
let connectedHost = null;
const localChangeListeners = [];
const bridgeChangeListeners = [];
const settings = {
  lookBehindDays: 90,
  lookAheadDays: 360,
  exportDescriptions: true,
  exportLocations: false,
  truncateText: true,
  maxDescriptionChars: 300,
  maxLocationChars: 200,
  queryTimeoutSeconds: 60,
  enableDebugLogs: false
};

const port = {
  onMessage: { addListener(listener) { onPortMessage = listener; } },
  onDisconnect: { addListener(listener) { onPortDisconnect = listener; } },
  postMessage(message) {
    sentMessage = message;
    queueMicrotask(() => onPortMessage({ ok: true, requestId: message.requestId }));
  },
  disconnect() {
    if (onPortDisconnect) onPortDisconnect();
  }
};

const browser = {
  runtime: {
    connectNative(host) {
      connectedHost = host;
      return port;
    },
    lastError: null
  },
  storage: {
    onChanged: { addListener(listener) { localChangeListeners.push(listener); } }
  },
  tbCalendarBridge: {
    onChanged: { addListener(listener) { bridgeChangeListeners.push(listener); } },
    setDebugLogging() {},
    async getSnapshot(start, end, options) {
      return {
        schemaVersion: 1,
        generatedAt: new Date().toISOString(),
        rangeStart: start,
        rangeEnd: end,
        calendars: []
      };
    }
  }
};

const CalendarSettings = {
  DEFAULTS: {
    lookBehindDays: 0,
    lookAheadDays: 0,
    exportDescriptions: true,
    exportLocations: true,
    truncateText: true,
    maxDescriptionChars: 1,
    maxLocationChars: 1,
    queryTimeoutSeconds: 1,
    enableDebugLogs: true
  },
  async load() { return settings; }
};

const context = {
  browser,
  CalendarSettings,
  console: { log() {}, warn() {}, error() {} },
  Date,
  Map,
  Promise,
  Object,
  String,
  Number,
  Math,
  Error,
  TextEncoder,
  performance,
  setTimeout,
  clearTimeout,
  setInterval() { return 1; },
  clearInterval() {}
};
vm.createContext(context);
vm.runInContext(
  fs.readFileSync(path.join(__dirname, "../extension/background.js"), "utf8"),
  context
);

(async () => {
  await new Promise(resolve => setTimeout(resolve, 30));
  assert.equal(connectedHost, "org.kde.plasma.thunderbird_calendar");
  assert.ok(sentMessage, "background script sent an initial snapshot");
  assert.equal(sentMessage.type, "snapshot");
  assert.equal(typeof sentMessage.requestId, "string");
  assert.equal(sentMessage.snapshot.schemaVersion, 1);
  assert.equal(localChangeListeners.length, 1);
  assert.equal(bridgeChangeListeners.length, 1);
  console.log("Background test passed: native snapshot carries a request ID and receives an acknowledgement.");
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
