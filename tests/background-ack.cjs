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
  truncateDescriptions: true,
  truncateLocations: true,
  maxDescriptionChars: 300,
  maxLocationChars: 200,
  queryTimeoutSeconds: 60,
  enableDebugLogs: true
};
const logLines = [];

const port = {
  onMessage: { addListener(listener) { onPortMessage = listener; } },
  onDisconnect: { addListener(listener) { onPortDisconnect = listener; } },
  postMessage(message) {
    sentMessage = message;
    queueMicrotask(() => onPortMessage({
      ok: true,
      requestId: message.requestId,
      hostTimingsMs: {
        frameReadMs: 2.1,
        jsonParseMs: 1.2,
        validationMs: 0.3,
        contentHashMs: 0.7,
        cacheDirectoryMs: 0.2,
        existingCacheReadMs: 0.0,
        cachePreparationMs: 0.2,
        snapshotSerializationMs: 0.8,
        diskCommitMs: 1.4,
        dbusNotifyMs: 0.1,
        hostProcessingMs: 4.7,
        cacheWriteBytes: 123,
        unchanged: false
      }
    }));
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
    truncateDescriptions: true,
    truncateLocations: true,
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
  console: {
    log(...args) { logLines.push(args.join(" ")); },
    warn() {},
    error() {}
  },
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
  assert.equal(sentMessage.debugLogs, true, "debug preference is passed to the native host");
  assert.equal(typeof sentMessage.requestId, "string");
  assert.equal(sentMessage.snapshot.schemaVersion, 1);
  assert.equal(localChangeListeners.length, 1);
  assert.equal(bridgeChangeListeners.length, 1);
  assert.ok(logLines.some(line => line.includes("native host phases (ms)")),
    "native host phase timings are written to the extension console when debugging is enabled");
  assert.ok(logLines.some(line => line.includes("JSON parse")),
    "the phase breakdown includes JSON parsing");
  console.log("Background test passed: request-ID acknowledgement and debug-gated native-host phase logging.");
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
