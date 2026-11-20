/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

const KEYS = Object.keys(CalendarSettings.DEFAULTS);
const inputs = Object.fromEntries(KEYS.map(key => [key, document.getElementById(key)]));
const ranges = Object.fromEntries(CalendarSettings.NUMERIC_KEYS.map(key => [
  key,
  document.getElementById(`${key}Range`)
]));
const isCheckbox = key => CalendarSettings.BOOLEAN_KEYS.includes(key);
const status = document.getElementById("status");
const advanced = document.getElementById("advancedSettings");
const advancedButton = document.getElementById("toggleAdvanced");

for (const key of CalendarSettings.NUMERIC_KEYS) {
  const { min, max } = CalendarSettings.LIMITS[key];
  for (const control of [inputs[key], ranges[key]]) {
    control.min = String(min);
    control.max = String(max);
    control.step = "1";
    control.title = `${min} to ${max}`;
  }
}

function updateDependentControls() {
  const truncationEnabled = inputs.truncateText.checked;
  const descriptionsEnabled = truncationEnabled && inputs.exportDescriptions.checked;
  const locationsEnabled = truncationEnabled && inputs.exportLocations.checked;
  ranges.maxDescriptionChars.disabled = !descriptionsEnabled;
  inputs.maxDescriptionChars.disabled = !descriptionsEnabled;
  ranges.maxLocationChars.disabled = !locationsEnabled;
  inputs.maxLocationChars.disabled = !locationsEnabled;
}

function show(values) {
  for (const key of KEYS) {
    if (isCheckbox(key)) {
      inputs[key].checked = Boolean(values[key]);
    } else {
      inputs[key].value = String(values[key]);
      ranges[key].value = String(values[key]);
    }
  }
  updateDependentControls();
}

function rawValues() {
  return Object.fromEntries(KEYS.map(key => [
    key,
    isCheckbox(key) ? inputs[key].checked : inputs[key].value
  ]));
}

let saveQueue = Promise.resolve();

function saveFromInputs() {
  const raw = rawValues();
  saveQueue = saveQueue.then(async () => {
    try {
      const saved = await CalendarSettings.save(raw);
      const adjusted = CalendarSettings.NUMERIC_KEYS.some(key =>
        String(raw[key]).trim() !== String(saved[key])
      );
      show(saved);
      status.textContent = adjusted
        ? "Saved. A value was outside the allowed range or was adjusted."
        : "Saved.";
    } catch (error) {
      status.textContent = `Could not save: ${error}`;
    }
  });
  return saveQueue;
}

for (const key of CalendarSettings.NUMERIC_KEYS) {
  const numberInput = inputs[key];
  const rangeInput = ranges[key];

  rangeInput.addEventListener("input", () => {
    numberInput.value = rangeInput.value;
  });
  rangeInput.addEventListener("change", saveFromInputs);

  numberInput.addEventListener("input", () => {
    const value = Number(numberInput.value);
    if (numberInput.value !== "" && Number.isFinite(value)) {
      const { min, max } = CalendarSettings.LIMITS[key];
      rangeInput.value = String(Math.min(max, Math.max(min, value)));
    }
  });
  numberInput.addEventListener("change", saveFromInputs);
}

for (const key of CalendarSettings.BOOLEAN_KEYS) {
  inputs[key].addEventListener("change", () => {
    updateDependentControls();
    saveFromInputs();
  });
}

advancedButton.addEventListener("click", () => {
  advanced.hidden = !advanced.hidden;
  const expanded = !advanced.hidden;
  advancedButton.setAttribute("aria-expanded", String(expanded));
  advancedButton.textContent = expanded ? "Hide advanced settings" : "Show advanced settings";
});

document.getElementById("form").addEventListener("submit", event => {
  event.preventDefault();
  saveFromInputs();
});

document.getElementById("reset").addEventListener("click", async () => {
  show(CalendarSettings.DEFAULTS);
  await saveFromInputs();
});

CalendarSettings.load().then(show);
