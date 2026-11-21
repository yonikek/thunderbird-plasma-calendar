/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

// Exercises the extension-side guard without needing to transmit a 32 MiB
// message to a real native host. The host-side framing guard has a separate
// integration test: tests/native-host-limit.py.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const MAX_BYTES = 32 * 1024 * 1024;
let onPortMessage;
let sent = false;
const errors = [];
const settings = {
  lookBehindDays: 90,
  lookAheadDays: 360,
  exportDescriptions: true,
  exportLocations: false,
  truncateDescriptions: false,
  truncateLocations: false,
  maxDescriptionChars: 300,
  maxLocationChars: 200,
  queryTimeoutSeconds: 60,
  enableDebugLogs: false
};

const browser = {
  runtime: {
    connectNative() {
      return {
        onMessage: { addListener(listener) { onPortMessage = listener; } },
        onDisconnect: { addListener() {} },
        postMessage() { sent = true; }
      };
    },
    lastError: null
  },
  storage: { onChanged: { addListener() {} } },
  tbCalendarBridge: {
    onChanged: { addListener() {} },
    setDebugLogging() {},
    async getSnapshot(start, end) {
      return {
        schemaVersion: 1,
        generatedAt: new Date().toISOString(),
        rangeStart: start,
        rangeEnd: end,
        calendars: [{
          id: "oversized-test",
          name: "Oversized test calendar",
          events: [{ title: "Large description", description: "x".repeat(MAX_BYTES + 128) }]
        }]
      };
    }
  }
};

const CalendarSettings = {
  DEFAULTS: {
    lookBehindDays: 90,
    lookAheadDays: 360,
    exportDescriptions: true,
    exportLocations: true,
    truncateDescriptions: true,
    truncateLocations: true,
    maxDescriptionChars: 300,
    maxLocationChars: 200,
    queryTimeoutSeconds: 60,
    enableDebugLogs: false
  },
  async load() { return settings; }
};

const context = {
  browser,
  CalendarSettings,
  console: {
    log() {},
    warn() {},
    error(...args) { errors.push(args.join(" ")); }
  },
  Date, Map, Promise, Object, String, Number, Math, Error, TextEncoder,
  performance, setTimeout, clearTimeout,
  setInterval() { return 1; },
  clearInterval() {}
};
vm.createContext(context);
vm.runInContext(
  fs.readFileSync(path.join(__dirname, "../extension/background.js"), "utf8"),
  context
);

(async () => {
  await new Promise(resolve => setTimeout(resolve, 1500));
  assert.equal(typeof onPortMessage, "function", "native host connection was established");
  assert.equal(sent, false, "oversized payload is rejected before postMessage");
  assert.ok(errors.some(message => message.includes("Calendar snapshot is too large")),
    "an actionable size error is logged even when debug logging is disabled");
  assert.ok(errors.some(message => message.includes("32.00 MiB")),
    "the error includes the configured native-host limit");
  assert.ok(errors.some(message => message.includes("bytes; maximum")),
    "the error includes exact actual and maximum byte counts");
  console.log("Native message limit test passed: oversized snapshots are blocked and reported clearly.");
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
