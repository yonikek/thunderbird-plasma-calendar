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
  const { setTimeout } = ChromeUtils.importESModule(
    "resource://gre/modules/Timer.sys.mjs"
  );

  // Reading items runs in Thunderbird's parent process. Work in slices of about
  // this many milliseconds and give the event loop a turn in between, so the
  // user interface and other add-ons are not blocked by a large refresh.
  const SLICE_MS = 10;

  // A provider expands recurring events inside a single getItems() call, which
  // cannot be interrupted. Query the window in chunks of this many days so each
  // call stays short and the event loop gets a turn between calls.
  const CHUNK_DAYS = 30;

  function createYielder() {
    const stats = { yields: 0, longestBlockMs: 0 };
    let sliceStart = Date.now();
    return {
      stats,
      async yieldIfNeeded() {
        const elapsed = Date.now() - sliceStart;
        if (elapsed >= SLICE_MS) {
          stats.longestBlockMs = Math.max(stats.longestBlockMs, elapsed);
          stats.yields++;
          await new Promise(resolve => setTimeout(resolve, 0));
          sliceStart = Date.now();
        }
      },
      finish() {
        stats.longestBlockMs = Math.max(stats.longestBlockMs, Date.now() - sliceStart);
        return stats;
      }
    };
  }

  // Contiguous [from, to) pairs of JS Dates covering [start, end).
  function splitRange(start, end) {
    const chunkMs = CHUNK_DAYS * 86400000;
    const chunks = [];
    for (let from = start.getTime(); from < end.getTime(); from += chunkMs) {
      chunks.push([new Date(from), new Date(Math.min(from + chunkMs, end.getTime()))]);
    }
    return chunks;
  }

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
    return { start, end };
  }

  // ---- Per-calendar cache -------------------------------------------------
  // The serialised events of each calendar are kept until that calendar
  // changes, so a refresh only re-reads calendars that were touched. Every
  // observer callback names the calendar it concerns; if it cannot be
  // determined, everything is dropped to be safe.
  const cache = new Map(); // calendar id -> { key, entry }
  const calendarVersions = new Map(); // calendar id -> change counter
  let globalVersion = 0;

  function versionOf(calendarId) {
    return `${globalVersion}:${calendarVersions.get(calendarId) || 0}`;
  }

  function cacheKey(startIso, endIso, exportOptions) {
    return `${startIso}|${endIso}|${exportOptions.includeDescriptions ? 1 : 0}${exportOptions.includeLocations ? 1 : 0}`;
  }

  function invalidate(calendar) {
    let id = null;
    try {
      id = calendar && calendar.id !== undefined && calendar.id !== null ? String(calendar.id) : null;
    } catch (error) {
      id = null;
    }
    if (id === null) {
      globalVersion++;
      cache.clear();
      return;
    }
    // The counter also stops a read that is in progress from being cached.
    calendarVersions.set(id, (calendarVersions.get(id) || 0) + 1);
    cache.delete(id);
  }

  function itemCalendar(item) {
    try {
      return item ? item.calendar : null;
    } catch (error) {
      return null;
    }
  }

  function createObservers(notify, invalidateCalendar) {
    const watched = name => WATCHED_PROPERTIES.has(String(name).toLowerCase());
    const changed = calendar => {
      invalidateCalendar(calendar);
      notify();
    };
    const calendarObserver = {
      QueryInterface: ChromeUtils.generateQI(["calIObserver"]),
      onStartBatch() {},
      onEndBatch(calendar) { changed(calendar); },
      onLoad(calendar) { changed(calendar); },
      onAddItem(item) { changed(itemCalendar(item)); },
      onModifyItem(newItem) { changed(itemCalendar(newItem)); },
      onDeleteItem(item) { changed(itemCalendar(item)); },
      onError() {},
      onPropertyChanged(calendar, name) { if (watched(name)) { changed(calendar); } },
      onPropertyDeleting(calendar, name) { if (watched(name)) { changed(calendar); } }
    };
    const managerObserver = {
      QueryInterface: ChromeUtils.generateQI(["calICalendarManagerObserver"]),
      onCalendarRegistered(calendar) { changed(calendar); },
      onCalendarUnregistering(calendar) { changed(calendar); },
      onCalendarDeleting(calendar) { changed(calendar); }
    };
    return { calendarObserver, managerObserver };
  }

  // One pair of observers serves both cache invalidation and the onChanged
  // event, and is registered whether or not anything listens to the event.
  const listeners = new Set();
  let tracker = null;

  function ensureTracker() {
    if (tracker) {
      return;
    }
    tracker = createObservers(() => {
      for (const notify of listeners) {
        notify();
      }
    }, invalidate);
    cal.manager.addCalendarObserver(tracker.calendarObserver);
    cal.manager.addObserver(tracker.managerObserver);
  }

  function removeTracker() {
    if (tracker) {
      try {
        cal.manager.removeCalendarObserver(tracker.calendarObserver);
        cal.manager.removeObserver(tracker.managerObserver);
      } catch (error) {
        console.warn("Thunderbird Plasma Calendar: could not remove observers:", error);
      }
      tracker = null;
    }
    listeners.clear();
    cache.clear();
  }

  return class extends ExtensionCommon.ExtensionAPI {
    onShutdown() {
      removeTracker();
    }

    getAPI(context) {
      ensureTracker();
      return {
        tbCalendarBridge: {
          async getSnapshot(startIso, endIso, options) {
            const range = parseRange(startIso, endIso);
            const chunks = splitRange(range.start, range.end);
            const exportOptions = {
              includeDescriptions: !options || options.includeDescriptions !== false,
              includeLocations: Boolean(options && options.includeLocations === true)
            };
            const logTiming = Boolean(options && options.logTiming === true);
            const filter = itemFilter();
            const yielder = createYielder();
            const useCache = !options || options.useCache !== false;
            const timing = { providerMs: 0, serialiseMs: 0, items: 0, queries: 0, read: 0, cached: 0 };
            const calendars = [];

            for (const calendar of cal.manager.getCalendars()) {
              const calendarEntry = {
                id: String(calendar.id),
                name: String(calendar.name || calendar.id),
                color: calendarColour(calendar),
                type: String(calendar.type || "unknown"),
                events: []
              };

              const key = cacheKey(startIso, endIso, exportOptions);
              const versionBefore = versionOf(calendarEntry.id);

              try {
                if (calendar.getProperty("disabled") === true) {
                  calendars.push(calendarEntry);
                  continue;
                }

                const hit = useCache ? cache.get(calendarEntry.id) : undefined;
                if (hit && hit.key === key) {
                  calendars.push(hit.entry);
                  timing.cached++;
                  continue;
                }
                timing.read++;

                // Items overlapping a chunk boundary are returned for both
                // chunks, and undated tasks for every chunk; keep each once.
                const seen = new Set();
                for (const [from, to] of chunks) {
                  await yielder.yieldIfNeeded();
                  let started = logTiming ? Date.now() : 0;
                  const stream = calendar.getItems(
                    filter, 0, cal.dtz.jsDateToDateTime(from), cal.dtz.jsDateToDateTime(to)
                  );
                  timing.queries++;
                  const iterator = cal.iterate.streamValues(stream)[Symbol.asyncIterator]();
                  for (;;) {
                    const next = await iterator.next();
                    if (logTiming) {
                      timing.providerMs += Date.now() - started;
                    }
                    if (next.done) {
                      break;
                    }
                    for (const item of next.value) {
                      const key = `${item.id}\u0000${recurrenceString(item)}`;
                      if (seen.has(key)) {
                        continue;
                      }
                      seen.add(key);
                      const serialiseStart = logTiming ? Date.now() : 0;
                      const serialized = serializeItem(item, calendar, calendarEntry.id, exportOptions);
                      if (logTiming) {
                        timing.serialiseMs += Date.now() - serialiseStart;
                      }
                      if (serialized) {
                        calendarEntry.events.push(serialized);
                        timing.items++;
                      }
                      await yielder.yieldIfNeeded();
                    }
                    started = logTiming ? Date.now() : 0;
                  }
                }
              } catch (error) {
                // Keep the calendar visible in settings even when a provider
                // is temporarily unavailable. Its events are retried on the
                // next refresh.
                console.error(`Unable to read Thunderbird calendar ${calendarEntry.name}:`, error);
                calendarEntry.error = String(error);
              }
              // Cache only complete reads of calendars that did not change
              // while they were being read; otherwise read again next time.
              if (!calendarEntry.error && versionOf(calendarEntry.id) === versionBefore) {
                cache.set(calendarEntry.id, { key, entry: calendarEntry });
              }
              calendars.push(calendarEntry);
            }

            // Forget calendars that no longer exist.
            const liveIds = new Set(calendars.map(entry => entry.id));
            for (const id of Array.from(cache.keys())) {
              if (!liveIds.has(id)) {
                cache.delete(id);
              }
            }

            const stats = yielder.finish();
            if (logTiming) {
              console.log(
                `Thunderbird Plasma Calendar: read ${timing.items} items in ${timing.queries} queries ` +
                `from ${timing.read} calendars (${timing.cached} from cache); ` +
                `provider ${timing.providerMs} ms, serialising ${timing.serialiseMs} ms, ` +
                `${stats.yields} yields, longest uninterrupted block ${stats.longestBlockMs} ms`
              );
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
              ensureTracker();
              listeners.add(notify);
              return () => {
                listeners.delete(notify);
              };
            }
          }).api()
        }
      };
    }
  };
})();
