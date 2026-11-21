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
  truncateText: false,
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
      async remove(keys) {
        for (const key of Array.isArray(keys) ? keys : [keys]) {
          delete values[key];
        }
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
const optionsHtml = fs.readFileSync(path.join(__dirname, "../extension/options/options.html"), "utf8");
for (const key of Object.keys(CalendarSettings.DEFAULTS)) {
  assert.match(optionsHtml, new RegExp(`id=["']${key}["']`), `options page is missing the ${key} control`);
}
assert.match(optionsHtml, /Locations are exported for future Plasma support, current version does not display them\./);
assert.match(optionsHtml, /Truncate event details to keep snapshots small\. Each limit can be set independently up to 1000 characters\./);
assert.doesNotMatch(optionsHtml, /Enabled by default to keep snapshots small/);

(async () => {
  const defaults = CalendarSettings.normalize({});
  assert.equal(defaults.truncateDescriptions, true, "description truncation defaults to enabled");
  assert.equal(defaults.truncateLocations, false, "location truncation defaults to disabled");

  const settings = await CalendarSettings.load();
  assert.equal(settings.lookBehindDays, 365, "days-back value is clamped");
  assert.equal(settings.lookAheadDays, 42);
  assert.equal(settings.maxDescriptionChars, 1000, "description limit is clamped");
  assert.equal(settings.queryTimeoutSeconds, 5, "timeout is clamped");
  assert.equal(settings.truncateDescriptions, false, "legacy shared truncation value migrates to descriptions");
  assert.equal(settings.truncateLocations, false, "legacy shared truncation value migrates to locations");
  assert.equal(settings.enableDebugLogs, true, "legacy logTiming value is migrated");

  await CalendarSettings.save({ ...settings, enableDebugLogs: false });
  assert.equal(values.enableDebugLogs, false);
  assert.equal("logTiming" in values, false, "legacy debug preference is removed after migration");
  assert.equal("truncateText" in values, false, "legacy truncation preference is removed after migration");

  const independentlySaved = await CalendarSettings.save({
    ...settings, truncateDescriptions: false, truncateLocations: true
  });
  assert.equal(independentlySaved.truncateDescriptions, false);
  assert.equal(independentlySaved.truncateLocations, true);
  console.log("Settings tests passed: legacy migrations, independent truncation, defaults, clamping and save.");
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
