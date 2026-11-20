/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

// Shared by the background script and the options page.
const CalendarSettings = (() => {
  const DEFAULTS = Object.freeze({
    lookBehindDays: 90,
    lookAheadDays: 360,
    exportDescriptions: true,
    exportLocations: false,
    truncateText: true,
    maxDescriptionChars: 300,
    maxLocationChars: 200,
    queryTimeoutSeconds: 60,
    enableDebugLogs: false
  });

  const LIMITS = Object.freeze({
    lookBehindDays: Object.freeze({ min: 0, max: 365 }),
    lookAheadDays: Object.freeze({ min: 1, max: 1095 }),
    maxDescriptionChars: Object.freeze({ min: 1, max: 1000 }),
    maxLocationChars: Object.freeze({ min: 1, max: 1000 }),
    queryTimeoutSeconds: Object.freeze({ min: 5, max: 300 })
  });

  const BOOLEAN_KEYS = Object.freeze([
    "exportDescriptions",
    "exportLocations",
    "truncateText",
    "enableDebugLogs"
  ]);
  const NUMERIC_KEYS = Object.freeze(Object.keys(LIMITS));
  const STORAGE_KEYS = Object.freeze([...Object.keys(DEFAULTS), "logTiming"]);

  function clampNumber(value, key) {
    if (value === null || value === undefined || String(value).trim() === "") {
      return DEFAULTS[key];
    }
    const number = Math.round(Number(value));
    if (!Number.isFinite(number)) {
      return DEFAULTS[key];
    }
    return Math.min(LIMITS[key].max, Math.max(LIMITS[key].min, number));
  }

  function normalize(raw) {
    const source = raw || {};
    // Migrate the old timing-only preference without losing the user's choice.
    const debugValue = typeof source.enableDebugLogs === "boolean"
      ? source.enableDebugLogs
      : source.logTiming;

    return {
      lookBehindDays: clampNumber(source.lookBehindDays, "lookBehindDays"),
      lookAheadDays: clampNumber(source.lookAheadDays, "lookAheadDays"),
      exportDescriptions: typeof source.exportDescriptions === "boolean"
        ? source.exportDescriptions : DEFAULTS.exportDescriptions,
      exportLocations: typeof source.exportLocations === "boolean"
        ? source.exportLocations : DEFAULTS.exportLocations,
      truncateText: typeof source.truncateText === "boolean"
        ? source.truncateText : DEFAULTS.truncateText,
      maxDescriptionChars: clampNumber(source.maxDescriptionChars, "maxDescriptionChars"),
      maxLocationChars: clampNumber(source.maxLocationChars, "maxLocationChars"),
      queryTimeoutSeconds: clampNumber(source.queryTimeoutSeconds, "queryTimeoutSeconds"),
      enableDebugLogs: typeof debugValue === "boolean" ? debugValue : DEFAULTS.enableDebugLogs
    };
  }

  async function load() {
    try {
      return normalize(await browser.storage.local.get(STORAGE_KEYS));
    } catch (error) {
      // A settings failure should not prevent the calendar bridge from starting.
      return { ...DEFAULTS };
    }
  }

  async function save(values) {
    const normalized = normalize(values);
    await browser.storage.local.set(normalized);
    // Remove the previous key after the new preference has been persisted.
    try {
      await browser.storage.local.remove("logTiming");
    } catch (error) {
      // Older storage implementations may not support remove; the new key wins.
    }
    return normalized;
  }

  return {
    DEFAULTS,
    LIMITS,
    BOOLEAN_KEYS,
    NUMERIC_KEYS,
    normalize,
    load,
    save
  };
})();
