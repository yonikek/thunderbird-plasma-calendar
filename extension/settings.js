/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

// Shared by the background script and the options page.
const CalendarSettings = (() => {
  // How far around "now" calendar items are exported to Plasma, and which
  // optional text fields are included.
  const DEFAULTS = Object.freeze({
    lookBehindDays: 90,
    lookAheadDays: 360,
    exportDescriptions: true,
    exportLocations: false,
    // Log how long each refresh takes. Off by default: it serialises the
    // snapshot once more to report its size.
    logTiming: false
  });
  // Larger windows mean larger snapshots (the native host rejects snapshots
  // over 32 MiB) and more work for Thunderbird and the Plasma plugin.
  const LIMITS = Object.freeze({
    lookBehindDays: Object.freeze({ min: 0, max: 365 }),
    lookAheadDays: Object.freeze({ min: 1, max: 1095 })
  });

  function clampDays(value, key) {
    if (value === null || value === undefined || String(value).trim() === "") {
      return DEFAULTS[key];
    }
    const number = Math.round(Number(value));
    if (!Number.isFinite(number)) {
      return DEFAULTS[key];
    }
    return Math.min(LIMITS[key].max, Math.max(LIMITS[key].min, number));
  }

  function toBoolean(value, key) {
    return typeof value === "boolean" ? value : DEFAULTS[key];
  }

  function normalize(raw) {
    const source = raw || {};
    return {
      lookBehindDays: clampDays(source.lookBehindDays, "lookBehindDays"),
      lookAheadDays: clampDays(source.lookAheadDays, "lookAheadDays"),
      exportDescriptions: toBoolean(source.exportDescriptions, "exportDescriptions"),
      exportLocations: toBoolean(source.exportLocations, "exportLocations"),
      logTiming: toBoolean(source.logTiming, "logTiming")
    };
  }

  async function load() {
    try {
      return normalize(await browser.storage.local.get(Object.keys(DEFAULTS)));
    } catch (error) {
      console.warn("Thunderbird Plasma Calendar: could not read settings, using defaults:", error);
      return { ...DEFAULTS };
    }
  }

  async function save(values) {
    const normalized = normalize(values);
    await browser.storage.local.set(normalized);
    return normalized;
  }

  return { DEFAULTS, LIMITS, normalize, load, save };
})();
