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
const bridgeItemChangeListeners = [];
let initialSnapshotMessage = null;
let sentMessages = [];
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
    sentMessages.push(message);
    if (message.type === "snapshot") {
      initialSnapshotMessage = message;
    }
    queueMicrotask(() => onPortMessage({
      ok: true,
      requestId: message.requestId,
      appliedChanges: message.type === "eventChanges" ? message.changes.length : 0,
      hostTimingsMs: {
        frameReadMs: 2.1,
        jsonParseMs: 1.2,
        validationMs: 0.3,
        cacheValidationMs: 0.0,
        contentHashMs: 0.7,
        cacheDirectoryMs: 0.2,
        existingCacheReadMs: 0.0,
        cachePreparationMs: 0.2,
        calendarIndexMs: 0.1,
        changeValidationMs: 0.2,
        eventIndexMs: 0.4,
        eventApplyMs: 0.1,
        eventArrayRebuildMs: 0.2,
        eventDiffMs: 0.7,
        directChangeCount: message.type === "eventChanges" ? message.changes.length : 0,
        affectedCalendarCount: message.type === "eventChanges" ? 1 : 0,
        eventDeltaCount: message.type === "eventChanges" ? message.changes.length : 0,
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
    onItemChanges: { addListener(listener) { bridgeItemChangeListeners.push(listener); } },
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
  assert.ok(initialSnapshotMessage, "background script sent an initial snapshot");
  assert.equal(initialSnapshotMessage.type, "snapshot");
  assert.equal(initialSnapshotMessage.debugLogs, true, "debug preference is passed to the native host");
  assert.equal(typeof initialSnapshotMessage.requestId, "string");
  assert.equal(initialSnapshotMessage.snapshot.schemaVersion, 1);
  assert.equal(localChangeListeners.length, 1);
  assert.equal(bridgeChangeListeners.length, 1);
  assert.equal(bridgeItemChangeListeners.length, 1, "background script subscribes to direct calendar item changes");
  assert.ok(logLines.some(line => line.includes("native host phases (ms)")),
    "native host phase timings are written to the extension console when debugging is enabled");
  assert.ok(logLines.some(line => line.includes("JSON parse")),
    "the phase breakdown includes JSON parsing");

  bridgeItemChangeListeners[0]([{
    operation: "add",
    calendarId: "calendar-1",
    event: { uid: "calendar-1:event-1:", title: "Direct event", start: "2026-11-21T09:00:00.000Z" }
  }]);
  await new Promise(resolve => setTimeout(resolve, 250));
  const directMessage = sentMessages.find(message => message.type === "eventChanges");
  assert.ok(directMessage, "background script sends direct item changes without requesting a full snapshot");
  assert.equal(directMessage.changes.length, 1);
  assert.equal(directMessage.changes[0].operation, "add");
  assert.equal(directMessage.debugLogs, true);
  assert.ok(logLines.some(line => line.includes("native host direct-update phases (ms)")),
    "direct event updates log their own native-host phase breakdown");
  assert.ok(logLines.some(line => line.includes("event lookup/index")),
    "direct timing output distinguishes event indexing from applying changes");
  assert.ok(logLines.some(line => line.includes("event-array rebuild")),
    "direct timing output measures rebuilding the affected calendar event array");
  console.log("Background test passed: initial snapshot acknowledgement, direct item-change delivery and direct native-host phase timings.");
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
