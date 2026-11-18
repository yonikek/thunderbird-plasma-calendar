/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

var { ExtensionCommon } = ChromeUtils.importESModule(
  "resource://gre/modules/ExtensionCommon.sys.mjs"
);
var { cal } = ChromeUtils.importESModule(
  "resource:///modules/calendar/calUtils.sys.mjs"
);

function pad2(value) {
  return String(value).padStart(2, "0");
}

function serializeDateTime(dateTime) {
  if (!dateTime) {
    return null;
  }
  // All-day dates are civil dates, not instants. Preserve them as YYYY-MM-DD
  // rather than converting them through UTC and risking a date shift.
  if (dateTime.isDate) {
    return `${dateTime.year}-${pad2(dateTime.month + 1)}-${pad2(dateTime.day)}`;
  }
  return cal.dtz.dateTimeToJsDate(dateTime.getInTimezone(cal.dtz.UTC)).toISOString();
}

function calendarColour(calendar) {
  try {
    return calendar.getProperty("color") || "";
  } catch (error) {
    return "";
  }
}

function itemDateTimes(item, isTodo) {
  if (isTodo) {
    const start = item.entryDate || item.dueDate || item.creationDate;
    const end = item.dueDate || start;
    return { start, end };
  }
  return { start: item.startDate, end: item.endDate };
}

function serializeItem(item, calendar, calendarId) {
  const isTodo = typeof item.isTodo === "function"
    ? item.isTodo()
    : Boolean(item.isTodo);
  const { start, end } = itemDateTimes(item, isTodo);
  if (!start || !end) {
    return null;
  }

  let recurrence = "";
  try {
    if (item.recurrenceId) {
      recurrence = item.recurrenceId.icalString || "";
    }
  } catch (error) {
    // Some providers expose recurrence IDs through a different wrapper.
  }

  let description = "";
  let location = "";
  try { description = item.description || ""; } catch (error) {}
  try { location = item.getProperty("LOCATION") || ""; } catch (error) {}

  const calendarColor = calendarColour(calendar);
  let itemColor = "";
  try { itemColor = item.getProperty("X-MOZ-COLOR") || ""; } catch (error) {}

  return {
    uid: `${calendarId}:${item.id || "unknown"}:${recurrence}`,
    title: item.title || "(untitled)",
    description,
    location,
    start: serializeDateTime(start),
    end: serializeDateTime(end),
    allDay: Boolean(start.isDate),
    kind: isTodo ? "todo" : "event",
    color: itemColor || calendarColor
  };
}

// Calendar properties that appear in the exported snapshot. Other property
// changes (sync tokens, last-refresh times, ...) happen constantly and are
// not worth a refresh.
const WATCHED_PROPERTIES = new Set(["name", "color", "disabled"]);

function createObservers(notify) {
  const watched = name => WATCHED_PROPERTIES.has(String(name).toLowerCase());
  const calendarObserver = {
    QueryInterface: ChromeUtils.generateQI(["calIObserver"]),
    onStartBatch() {},
    onEndBatch() { notify(); },
    onLoad() { notify(); },
    onAddItem() { notify(); },
    onModifyItem() { notify(); },
    onDeleteItem() { notify(); },
    onError() {},
    onPropertyChanged(calendar, name) { if (watched(name)) { notify(); } },
    onPropertyDeleting(calendar, name) { if (watched(name)) { notify(); } }
  };
  const managerObserver = {
    QueryInterface: ChromeUtils.generateQI(["calICalendarManagerObserver"]),
    onCalendarRegistered() { notify(); },
    onCalendarUnregistering() { notify(); },
    onCalendarDeleting() { notify(); }
  };
  return { calendarObserver, managerObserver };
}

var tbCalendarBridge = class extends ExtensionCommon.ExtensionAPI {
  getAPI(context) {
    return {
      tbCalendarBridge: {
        async getSnapshot(startIso, endIso) {
          const rangeStart = cal.dtz.jsDateToDateTime(new Date(startIso));
          const rangeEnd = cal.dtz.jsDateToDateTime(new Date(endIso));
          const calendars = [];

          // All-items + occurrences means recurring appointments are expanded
          // into individual occurrences for the requested time window.
          const filter =
            Ci.calICalendar.ITEM_FILTER_TYPE_EVENT |
            Ci.calICalendar.ITEM_FILTER_TYPE_TODO |
            Ci.calICalendar.ITEM_FILTER_CLASS_OCCURRENCES |
            Ci.calICalendar.ITEM_FILTER_COMPLETED_ALL;

          for (const calendar of cal.manager.getCalendars()) {
            const calendarEntry = {
              id: String(calendar.id),
              name: String(calendar.name || calendar.id),
              color: calendarColour(calendar),
              type: String(calendar.type || "unknown"),
              events: []
            };

            try {
              if (calendar.getProperty("disabled") === true) {
                calendars.push(calendarEntry);
                continue;
              }

              const stream = calendar.getItems(filter, 0, rangeStart, rangeEnd);
              for await (const batch of cal.iterate.streamValues(stream)) {
                for (const item of batch) {
                  const serialized = serializeItem(item, calendar, calendarEntry.id);
                  if (serialized) {
                    calendarEntry.events.push(serialized);
                  }
                }
              }
            } catch (error) {
              // Keep the calendar visible in settings even when a provider is
              // temporarily unavailable. Its events will be retried next poll.
              console.error(`Unable to read Thunderbird calendar ${calendarEntry.name}:`, error);
              calendarEntry.error = String(error);
            }
            calendars.push(calendarEntry);
          }

          return {
            schemaVersion: 1,
            generatedAt: new Date().toISOString(),
            rangeStart: startIso,
            rangeEnd: endIso,
            calendars
          };
        },

        onChanged: new ExtensionCommon.EventManager({
          context,
          name: "tbCalendarBridge.onChanged",
          register(fire) {
            const notify = () => {
              try {
                fire.async();
              } catch (error) {
                console.error("Thunderbird Plasma Calendar: change notification failed:", error);
              }
            };
            const { calendarObserver, managerObserver } = createObservers(notify);
            cal.manager.addCalendarObserver(calendarObserver);
            cal.manager.addObserver(managerObserver);
            return () => {
              cal.manager.removeCalendarObserver(calendarObserver);
              cal.manager.removeObserver(managerObserver);
            };
          }
        }).api()
      }
    };
  }
};
