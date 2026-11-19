/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

const KEYS = Object.keys(CalendarSettings.DEFAULTS);
const inputs = Object.fromEntries(KEYS.map(key => [key, document.getElementById(key)]));
const status = document.getElementById("status");

for (const key of KEYS) {
  const { min, max } = CalendarSettings.LIMITS[key];
  inputs[key].min = String(min);
  inputs[key].max = String(max);
  inputs[key].title = `${min} to ${max} days`;
}

function show(values) {
  for (const key of KEYS) {
    inputs[key].value = String(values[key]);
  }
}

async function saveFromInputs() {
  const raw = Object.fromEntries(KEYS.map(key => [key, inputs[key].value]));
  try {
    const saved = await CalendarSettings.save(raw);
    const adjusted = KEYS.some(key => raw[key].trim() !== String(saved[key]));
    show(saved);
    status.textContent = adjusted
      ? "Saved. A value was outside the allowed range or not a number and was adjusted."
      : "Saved.";
  } catch (error) {
    status.textContent = `Could not save: ${error}`;
  }
}

for (const key of KEYS) {
  inputs[key].addEventListener("change", saveFromInputs);
}

document.getElementById("form").addEventListener("submit", event => {
  event.preventDefault();
  saveFromInputs();
});

document.getElementById("reset").addEventListener("click", async () => {
  show(CalendarSettings.DEFAULTS);
  await saveFromInputs();
});

CalendarSettings.load().then(show);
