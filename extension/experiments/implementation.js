/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

// Everything lives inside this closure so helper names cannot collide with
// globals from other experiments. The one global is the API class itself,
// which Thunderbird looks up by the namespace name in manifest.json.
var tbCalendarBridge = (() => {
  const { ExtensionCommon } = ChromeUtils.importESModule(
    "resource://gre/modules/ExtensionCommon.sys.mjs"
  );
  const { cal } = ChromeUtils.importESModule(
    "resource:///modules/calendar/calUtils.sys.mjs"
  );

  // Calendar properties that appear in the exported snapshot. Other property
  // changes (sync tokens, last-refresh times, ...) happen constantly and are
  // not worth a refresh.
  const WATCHED_PROPERTIES = new Set(["name", "color", "disabled"]);

  function pad2(value) {
    return String(value).padStart(2, "0");
  }

  function serializeDateTime(dateTime) {
    if (!dateTime) {
      return null;
    }
    // All-day dates are civil dates, not instants. Preserve them as
    // YYYY-MM-DD rather than converting them through UTC and risking a date
    // shift.
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

  function recurrenceString(item) {
    try {
      if (item.recurrenceId) {
        return item.recurrenceId.icalString || "";
      }
    } catch (error) {
      // Some providers expose recurrence IDs through a different wrapper.
    }
    return "";
  }

  // Items of every type, with recurring items expanded into occurrences for
  // the requested time window.
  function itemFilter() {
    return Ci.calICalendar.ITEM_FILTER_TYPE_EVENT |
      Ci.calICalendar.ITEM_FILTER_TYPE_TODO |
      Ci.calICalendar.ITEM_FILTER_CLASS_OCCURRENCES |
      Ci.calICalendar.ITEM_FILTER_COMPLETED_ALL;
  }

  // Long text is truncated to keep the snapshot and the plaintext cache small
  // when invitations carry long boilerplate. Line breaks and spacing are kept.
  const MAX_DESCRIPTION_CHARS = 300;
  const MAX_LOCATION_CHARS = 200;

  function truncate(text, maxChars) {
    const value = String(text);
    if (value.length <= maxChars) {
      return value;
    }
    let cut = value.slice(0, maxChars - 1);
    if (/[\uD800-\uDBFF]$/.test(cut)) {
      cut = cut.slice(0, -1); // do not split a surrogate pair
    }
    return `${cut.trimEnd()}\u2026`;
  }

  function itemDescription(item) {
    let text = "";
    try {
      text = item.descriptionText;
    } catch (error) {
      // Older Thunderbird versions have no descriptionText.
    }
    if (typeof text !== "string" || text === "") {
      try {
        text = item.getProperty("DESCRIPTION") || "";
      } catch (error) {
        text = "";
      }
    }
    return truncate(text, MAX_DESCRIPTION_CHARS);
  }

  function itemLocation(item) {
    try {
      return truncate(item.getProperty("LOCATION") || "", MAX_LOCATION_CHARS);
    } catch (error) {
      return "";
    }
  }

  function serializeItem(item, calendar, calendarId, options) {
    const isTodo = typeof item.isTodo === "function"
      ? item.isTodo()
      : Boolean(item.isTodo);
    const { start, end } = itemDateTimes(item, isTodo);
    if (!start || !end) {
      return null;
    }

    const recurrence = recurrenceString(item);

    // Skipped entirely when disabled: no calls into Thunderbird, no payload.
    const description = options.includeDescriptions ? itemDescription(item) : "";
    const location = options.includeLocations ? itemLocation(item) : "";

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

  function parseRange(startIso, endIso) {
    const start = new Date(startIso);
    const end = new Date(endIso);
    if (Number.isNaN(start.getTime()) || Number.isNaN(end.getTime()) || end <= start) {
      throw new Error("Invalid snapshot date range");
    }
    return {
      rangeStart: cal.dtz.jsDateToDateTime(start),
      rangeEnd: cal.dtz.jsDateToDateTime(end)
    };
  }

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

  return class extends ExtensionCommon.ExtensionAPI {
    getAPI(context) {
      return {
        tbCalendarBridge: {
          async getSnapshot(startIso, endIso, options) {
            const { rangeStart, rangeEnd } = parseRange(startIso, endIso);
            const exportOptions = {
              includeDescriptions: !options || options.includeDescriptions !== false,
              includeLocations: Boolean(options && options.includeLocations === true)
            };
            const filter = itemFilter();
            const calendars = [];

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
                    const serialized = serializeItem(item, calendar, calendarEntry.id, exportOptions);
                    if (serialized) {
                      calendarEntry.events.push(serialized);
                    }
                  }
                }
              } catch (error) {
                // Keep the calendar visible in settings even when a provider
                // is temporarily unavailable. Its events are retried on the
                // next refresh.
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
})();
