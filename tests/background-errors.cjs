/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const source = fs.readFileSync(path.join(__dirname, "../extension/background.js"), "utf8");
const timers = new Map();
const responsePlans = [];
const sentMessages = [];
const errors = [];
const snapshotOptions = [];
let nextTimerId = 1;
let onPortMessage;
let onPortDisconnect;
let itemChangeListener;
let bridgeRefreshListener;
let onStorageChanged;

function fakeSetTimeout(callback, delay = 0) {
  const id = nextTimerId++;
  timers.set(id, { callback, delay: Number(delay) || 0 });
  return id;
}

function fakeClearTimeout(id) {
  timers.delete(id);
}

async function settle() {
  await Promise.resolve();
  await Promise.resolve();
  await new Promise(resolve => setImmediate(resolve));
  await Promise.resolve();
}

async function runTimer(delay) {
  const entry = [...timers.entries()].find(([, timer]) => timer.delay === delay);
  assert.ok(entry, `expected a pending timer with delay ${delay} ms; pending: ${[...timers.values()].map(t => t.delay).join(", ")}`);
  const [id, timer] = entry;
  timers.delete(id);
  timer.callback();
  await settle();
}

const nativePort = {
  onMessage: { addListener(callback) { onPortMessage = callback; } },
  onDisconnect: { addListener(callback) { onPortDisconnect = callback; } },
  postMessage(message) {
    const plain = JSON.parse(JSON.stringify(message));
    sentMessages.push(plain);
    const planned = responsePlans.shift() || { ok: true };
    queueMicrotask(() => onPortMessage({ ...planned, requestId: plain.requestId }));
  },
  disconnect() { onPortDisconnect?.(); }
};

const settings = {
  lookBehindDays: 90,
  lookAheadDays: 360,
  exportDescriptions: true,
  exportLocations: false,
  truncateDescriptions: true,
  truncateLocations: false,
  maxDescriptionChars: 300,
  maxLocationChars: 200,
  queryTimeoutSeconds: 60,
  enableDebugLogs: false
};

const browser = {
  runtime: {
    connectNative(name) {
      assert.equal(name, "org.kde.plasma.thunderbird_calendar");
      return nativePort;
    },
    lastError: null
  },
  storage: {
    onChanged: { addListener(callback) { onStorageChanged = callback; } }
  },
  tbCalendarBridge: {
    onChanged: { addListener(callback) { bridgeRefreshListener = callback; } },
    onItemChanges: { addListener(callback) { itemChangeListener = callback; } },
    setDebugLogging() {},
    async getSnapshot(start, end, options) {
      snapshotOptions.push({ ...options });
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
    lookBehindDays: 0, lookAheadDays: 0,
    exportDescriptions: true, exportLocations: false,
    truncateDescriptions: true, truncateLocations: false,
    maxDescriptionChars: 300, maxLocationChars: 200,
    queryTimeoutSeconds: 60, enableDebugLogs: false
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
  Date, Map, Set, Promise, Object, String, Number, Math, Error, Array, JSON,
  TextEncoder, performance, queueMicrotask,
  setTimeout: fakeSetTimeout,
  clearTimeout: fakeClearTimeout,
  setInterval() { return -1; },
  clearInterval() {}
};
vm.createContext(context);
vm.runInContext(source, context);

const change = uid => ({
  operation: "add",
  calendarId: "calendar-1",
  event: { uid, title: uid, start: "2026-11-22T09:00:00.000Z", end: "2026-11-22T10:00:00.000Z" }
});

(async () => {
  await settle();
  assert.equal(sentMessages[0]?.type, "snapshot", "initial snapshot should establish the native-host connection");
  assert.equal(typeof itemChangeListener, "function", "direct item-change listener should be registered");

  // The queue boundary rejects an oversized incoming message before it
  // iterates the entries or grows the pending map, then requests a snapshot.
  const snapshotsBeforeOversizedBatch = sentMessages.filter(message => message.type === "snapshot").length;
  itemChangeListener(Array.from({ length: 5001 }, (_, index) => change(`oversized-${index}`)));
  assert.equal(vm.runInContext("pendingItemChanges.size", context), 0,
    "oversized observer input must not populate the pending-change map");
  assert.equal(vm.runInContext("fullRefreshNext", context), true,
    "oversized observer input should request an authoritative full snapshot");
  await runTimer(2500);
  assert.ok(sentMessages.filter(message => message.type === "snapshot").length > snapshotsBeforeOversizedBatch,
    "oversized observer input should trigger a full-snapshot recovery");

  // Journal errors which can be repaired by a new full snapshot must pause
  // direct delivery until that snapshot is acknowledged. This catches a bug
  // where flushItemChanges() would immediately retry from its finally block.
  responsePlans.push({ ok: false, code: "JOURNAL_RECORD_TOO_LARGE", error: "test journal record too large" });
  itemChangeListener([change("fallback-event")]);
  await runTimer(120);
  assert.equal(vm.runInContext("snapshotRecoveryPending", context), true);
  assert.ok([...timers.values()].some(timer => timer.delay === 2500),
    "journal fallback should schedule a full snapshot");
  assert.ok(![...timers.values()].some(timer => timer.delay === 0),
    "failed direct batch must not be retried before snapshot recovery");

  await runTimer(2500);
  const snapshotAfterFallback = sentMessages.findIndex((message, index) => index > 0 && message.type === "snapshot");
  assert.ok(snapshotAfterFallback > 0, "journal error should cause a full-snapshot fallback");
  assert.equal(snapshotOptions.at(-1).useCache, false, "fallback snapshot must bypass the extension cache");
  assert.equal(vm.runInContext("snapshotRecoveryPending", context), false,
    "successful full snapshot should release the direct-update gate");
  await runTimer(0);
  assert.equal(sentMessages.at(-1).type, "eventChanges",
    "queued direct changes should resume after the recovery snapshot is acknowledged");

  // Permanent validation/capacity errors are reported and never retried.
  responsePlans.push({ ok: false, code: "EVENT_LIMIT_EXCEEDED", error: "test event limit exceeded" });
  itemChangeListener([change("permanent-error")]);
  await runTimer(120);
  assert.equal(vm.runInContext("itemChangeRetriesExhausted", context), true,
    "permanent error should pause direct updates");
  assert.ok(errors.some(line => line.includes("test event limit exceeded")),
    "permanent error must be visible even with debug logging disabled");
  const afterPermanentError = sentMessages.length;
  assert.ok(![...timers.values()].some(timer => timer.delay === 1500),
    "permanent error must not schedule an automatic retry");
  assert.equal(sentMessages.length, afterPermanentError);

  // A competing native-host profile lock pauses writes but retries a full
  // snapshot at a controlled interval so the extension resumes when available.
  responsePlans.push({ ok: false, code: "ACTIVE_PROFILE_LOCKED", error: "test lock owner" });
  itemChangeListener([change("locked-event")]);
  await runTimer(120);
  assert.equal(vm.runInContext("profileLockBlocked", context), true,
    "active profile lock should block writes");
  assert.ok(errors.some(line => line.includes("Only one Thunderbird profile")),
    "profile lock conflict should be visible");
  assert.ok([...timers.values()].some(timer => timer.delay === 60000),
    "profile lock should schedule a delayed retry rather than rapid polling");
  await runTimer(60000);
  assert.equal(vm.runInContext("profileLockBlocked", context), false,
    "successful full snapshot after lock release should unblock updates");
  await runTimer(0);
  assert.equal(sentMessages.at(-1).type, "eventChanges",
    "queued changes should resume after lock release and full snapshot");

  // Ordinary transient errors receive five automatic retries, then pause with
  // a visible diagnostic instead of retrying forever.
  for (let i = 0; i < 6; i++) {
    responsePlans.push({ ok: false, code: "TEMPORARY_STORAGE_ERROR", error: `transient-${i + 1}` });
  }
  itemChangeListener([change("transient-error")]);
  await runTimer(120);
  const retryDelays = [1500, 3000, 6000, 12000, 24000];
  for (const delay of retryDelays) {
    await runTimer(delay);
    await runTimer(0);
  }
  assert.equal(vm.runInContext("itemChangeRetriesExhausted", context), true,
    "sixth failed attempt should pause automatic direct-update retries");
  assert.ok(errors.some(line => line.includes("direct calendar updates have been paused")),
    "retry exhaustion should produce an actionable error");
  assert.ok(![...timers.values()].some(timer => timer.delay === 48000),
    "no further retry should be scheduled after the retry budget is exhausted");

  // Full-snapshot delivery failures also have a finite retry budget. Trigger a
  // fresh observer refresh after the previous direct error has paused its queue.
  for (let i = 0; i < 6; i++) {
    responsePlans.push({ ok: false, code: "CACHE_WRITE_ERROR", error: `snapshot-storage-${i + 1}` });
  }
  bridgeRefreshListener();
  await runTimer(2500);
  for (const delay of [5000, 10000, 20000, 40000, 80000]) {
    await runTimer(delay);
  }
  assert.ok(errors.some(line => line.includes("automatic snapshot retries paused after 5 failed attempts")),
    "snapshot retry exhaustion should produce an actionable error");
  assert.ok(![...timers.values()].some(timer => timer.delay === 160000),
    "snapshot errors must not retry forever after the configured retry budget");

  console.log("Background error tests passed: snapshot fallback gates direct retries, permanent errors stop, profile locks back off and recover, direct and snapshot retries are bounded.");
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
