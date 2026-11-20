/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const values = {
  lookBehindDays: 999,
  lookAheadDays: 42,
  exportDescriptions: true,
  exportLocations: false,
  logTiming: true,
  maxDescriptionChars: 4000,
  queryTimeoutSeconds: 1
};
const browser = {
  storage: {
    local: {
      async get(keys) {
        return Object.fromEntries(keys.filter(key => key in values).map(key => [key, values[key]]));
      },
      async set(update) {
        Object.assign(values, update);
      },
      async remove(key) {
        delete values[key];
      }
    }
  }
};
const context = { browser, Object, Number, String, Math, Boolean };
vm.createContext(context);
vm.runInContext(
  fs.readFileSync(path.join(__dirname, "../extension/settings.js"), "utf8"),
  context
);
const CalendarSettings = vm.runInContext("CalendarSettings", context);

(async () => {
  const settings = await CalendarSettings.load();
  assert.equal(settings.lookBehindDays, 365, "days-back value is clamped");
  assert.equal(settings.lookAheadDays, 42);
  assert.equal(settings.maxDescriptionChars, 1000, "description limit is clamped");
  assert.equal(settings.queryTimeoutSeconds, 5, "timeout is clamped");
  assert.equal(settings.truncateText, true, "truncation defaults to enabled");
  assert.equal(settings.enableDebugLogs, true, "legacy logTiming value is migrated");

  await CalendarSettings.save({ ...settings, enableDebugLogs: false });
  assert.equal(values.enableDebugLogs, false);
  assert.equal("logTiming" in values, false, "legacy preference is removed after migration");
  console.log("Settings tests passed: legacy migration, defaults, clamping and save.");
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
