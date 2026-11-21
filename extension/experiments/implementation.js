/* SPDX-License-Identifier: GPL-2.0-or-later */
"use strict";

// Everything lives inside this closure so helper names cannot collide with
// globals from other experiments. Thunderbird looks up the API class below.
var tbCalendarBridge = (() => {
  const { ExtensionCommon } = ChromeUtils.importESModule(
    "resource://gre/modules/ExtensionCommon.sys.mjs"
  );
  const { cal } = ChromeUtils.importESModule(
    "resource:///modules/calendar/calUtils.sys.mjs"
  );
  const { setTimeout, clearTimeout } = ChromeUtils.importESModule(
    "resource://gre/modules/Timer.sys.mjs"
  );

  const SLICE_MS = 10;
  const CHUNK_DAYS = 30;
  const MAX_CONCURRENT_CALENDARS = 3;
  const DEFAULT_QUERY_TIMEOUT_SECONDS = 60;
  const WATCHED_PROPERTIES = new Set(["name", "color", "disabled"]);

  let debugLogsEnabled = false;

  function debugError(...args) {
    if (debugLogsEnabled) {
      console.error("Thunderbird Plasma Calendar:", ...args);
    }
  }

  function debugInfo(...args) {
    if (debugLogsEnabled) {
      console.log("Thunderbird Plasma Calendar:", ...args);
    }
  }

  // A shared yielder bounds synchronous work across the concurrent calendar
  // workers. Calls under the time budget do not allocate Promises or await.
  function createYielder() {
    const stats = { yields: 0, longestBlockMs: 0 };
    const now = () => typeof performance !== "undefined" && typeof performance.now === "function"
      ? performance.now() : Date.now();
    let sliceStart = now();
    let pendingYield = null;

    return {
      stats,
      resetAfterProviderWait() {
        if (!pendingYield) {
          sliceStart = now();
        }
      },
      shouldYield() {
        return Boolean(pendingYield) || now() - sliceStart >= SLICE_MS;
      },
      async yieldNow() {
        if (!pendingYield) {
          const elapsed = now() - sliceStart;
          if (elapsed < SLICE_MS) {
            return;
          }
          stats.longestBlockMs = Math.max(stats.longestBlockMs, elapsed);
          stats.yields++;
          pendingYield = new Promise(resolve => setTimeout(resolve, 0)).then(() => {
            sliceStart = now();
          }).finally(() => {
            pendingYield = null;
          });
        }
        await pendingYield;
      },
      finish() {
        if (!pendingYield) {
          stats.longestBlockMs = Math.max(stats.longestBlockMs, now() - sliceStart);
        }
        return stats;
      }
    };
  }

  function splitRange(start, end) {
    const chunkMs = CHUNK_DAYS * 86400000;
    const chunks = [];
    for (let from = start.getTime(); from < end.getTime(); from += chunkMs) {
      chunks.push([new Date(from), new Date(Math.min(from + chunkMs, end.getTime()))]);
    }
    return chunks;
  }

  function pad2(value) {
    return String(value).padStart(2, "0");
  }

  function serializeDateTime(dateTime) {
    if (!dateTime) {
      return null;
    }
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

  function itemFilter() {
    return Ci.calICalendar.ITEM_FILTER_TYPE_EVENT |
      Ci.calICalendar.ITEM_FILTER_TYPE_TODO |
      Ci.calICalendar.ITEM_FILTER_CLASS_OCCURRENCES |
      Ci.calICalendar.ITEM_FILTER_COMPLETED_ALL;
  }

  function truncate(text, maxChars, enabled) {
    const value = String(text ?? "");
    if (!enabled || value.length <= maxChars) {
      return value;
    }
    let cut = value.slice(0, Math.max(0, maxChars - 1));
    if (/[\uD800-\uDBFF]$/.test(cut)) {
      cut = cut.slice(0, -1);
    }
    return `${cut.trimEnd()}\u2026`;
  }

  function itemDescription(item, options) {
    let text = "";
    try {
      text = item.descriptionText;
    } catch (error) {
      // Older Thunderbird versions may not expose descriptionText.
    }
    if (typeof text !== "string") {
      try {
        text = item.getProperty("DESCRIPTION") || "";
      } catch (error) {
        text = "";
      }
    }
    return truncate(text, options.maxDescriptionChars, options.truncateDescriptions);
  }

  function itemLocation(item, options) {
    try {
      return truncate(
        item.getProperty("LOCATION") || "",
        options.maxLocationChars,
        options.truncateLocations
      );
    } catch (error) {
      return "";
    }
  }

  function serializeItem(item, calendarId, calendarColor, options, recurrence) {
    const isTodo = typeof item.isTodo === "function"
      ? item.isTodo()
      : Boolean(item.isTodo);
    const { start, end } = itemDateTimes(item, isTodo);
    if (!start || !end) {
      return null;
    }

    const description = options.includeDescriptions ? itemDescription(item, options) : "";
    const location = options.includeLocations ? itemLocation(item, options) : "";
    let itemColor = "";
    try {
      itemColor = item.getProperty("X-MOZ-COLOR") || "";
    } catch (error) {
      // Item-level colours are optional.
    }

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
  // Invalidation marks an entry dirty by advancing its version, but deliberately
  // retains the old successful entry. If the next provider read fails, that
  // last-known-good event list can still be exported as stale data.
  const cache = new Map(); // id -> { key, version, entry }
  const calendarVersions = new Map();
  let globalVersion = 0;

  function versionOf(calendarId) {
    return `${globalVersion}:${calendarVersions.get(calendarId) || 0}`;
  }

  function cacheKey(startIso, endIso, options) {
    return [
      startIso,
      endIso,
      options.includeDescriptions ? 1 : 0,
      options.includeLocations ? 1 : 0,
      options.truncateDescriptions ? 1 : 0,
      options.truncateLocations ? 1 : 0,
      options.maxDescriptionChars,
      options.maxLocationChars
    ].join("|");
  }

  function calendarIdOf(calendar) {
    try {
      return calendar && calendar.id !== undefined && calendar.id !== null
        ? String(calendar.id) : null;
    } catch (error) {
      return null;
    }
  }

  function invalidate(calendar) {
    const id = calendarIdOf(calendar);
    if (id === null) {
      globalVersion++;
      return;
    }
    calendarVersions.set(id, (calendarVersions.get(id) || 0) + 1);
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
    let batchDepth = 0;
    let batchDirty = false;
    const batchInvalidated = new Set();
    let batchGlobalInvalidated = false;

    const invalidateDuringBatch = calendar => {
      const id = calendarIdOf(calendar);
      if (id === null) {
        if (!batchGlobalInvalidated) {
          invalidateCalendar(null);
          batchGlobalInvalidated = true;
        }
      } else if (!batchInvalidated.has(id)) {
        invalidateCalendar(calendar);
        batchInvalidated.add(id);
      }
    };

    const changed = calendar => {
      if (batchDepth > 0) {
        batchDirty = true;
        invalidateDuringBatch(calendar);
        return;
      }
      invalidateCalendar(calendar);
      notify();
    };

    const calendarObserver = {
      QueryInterface: ChromeUtils.generateQI(["calIObserver"]),
      onStartBatch(calendar) {
        batchDepth++;
        batchDirty = true;
        invalidateDuringBatch(calendar);
      },
      onEndBatch(calendar) {
        if (batchDepth <= 0) {
          changed(calendar);
          return;
        }
        batchDepth--;
        if (batchDepth === 0) {
          if (batchDirty) {
            notify();
          }
          batchDirty = false;
          batchInvalidated.clear();
          batchGlobalInvalidated = false;
        } else {
          batchDirty = true;
        }
      },
      onLoad(calendar) { changed(calendar); },
      onAddItem(item) { changed(itemCalendar(item)); },
      onModifyItem(newItem) { changed(itemCalendar(newItem)); },
      onDeleteItem(item) { changed(itemCalendar(item)); },
      onError(calendar, code, message) {
        debugError("calendar provider error", calendarIdOf(calendar) || "unknown calendar", code, message || "");
        changed(calendar);
      },
      onPropertyChanged(calendar, name) {
        if (watched(name)) {
          changed(calendar);
        }
      },
      onPropertyDeleting(calendar, name) {
        if (watched(name)) {
          changed(calendar);
        }
      }
    };

    const managerObserver = {
      QueryInterface: ChromeUtils.generateQI(["calICalendarManagerObserver"]),
      onCalendarRegistered(calendar) { changed(calendar); },
      onCalendarUnregistering(calendar) { changed(calendar); },
      onCalendarDeleting(calendar) { changed(calendar); }
    };
    return { calendarObserver, managerObserver };
  }

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
        debugError("could not remove calendar observers:", error);
      }
      tracker = null;
    }
    listeners.clear();
    cache.clear();
    calendarVersions.clear();
    globalVersion++;
  }

  function cancelQuery(stream, iterator, reason) {
    try {
      const result = iterator && typeof iterator.return === "function"
        ? iterator.return() : null;
      if (result && typeof result.catch === "function") {
        result.catch(() => {});
      }
    } catch (error) {
      // Cancellation is best-effort; the query timeout still bounds the caller.
    }
    try {
      if (stream && typeof stream.cancel === "function") {
        const result = stream.cancel(reason);
        if (result && typeof result.catch === "function") {
          result.catch(() => {});
        }
      }
    } catch (error) {
      // A stream can be locked by its async iterator, making direct cancel invalid.
    }
  }

  async function readChunk(calendar, from, to, calendarEntry, exportOptions, seen, yielder, timing, logTiming, timeoutMs) {
    const stream = calendar.getItems(
      itemFilter(),
      0,
      cal.dtz.jsDateToDateTime(from),
      cal.dtz.jsDateToDateTime(to)
    );
    const iterator = cal.iterate.streamValues(stream)[Symbol.asyncIterator]();
    const queryStarted = Date.now();
    let timedOut = false;
    let timer = null;
    let rejectTimeout;

    const timeoutPromise = new Promise((resolve, reject) => {
      rejectTimeout = reject;
      timer = setTimeout(() => {
        timedOut = true;
        const error = new Error(`calendar query timed out after ${timeoutMs} ms`);
        cancelQuery(stream, iterator, error);
        rejectTimeout(error);
      }, timeoutMs);
    });

    const readPromise = (async () => {
      for (;;) {
        const waitStarted = logTiming ? Date.now() : 0;
        const next = await iterator.next();
        yielder.resetAfterProviderWait();
        if (logTiming) {
          timing.providerMs += Date.now() - waitStarted;
        }
        if (timedOut) {
          return;
        }
        if (next.done) {
          return;
        }

        for (const item of next.value) {
          const recurrence = recurrenceString(item);
          const dedupeKey = `${item.id}\u0000${recurrence}`;
          if (!seen.has(dedupeKey)) {
            seen.add(dedupeKey);
            const serialiseStart = logTiming ? Date.now() : 0;
            const serialized = serializeItem(
              item, calendarEntry.id, calendarEntry.color, exportOptions, recurrence
            );
            if (logTiming) {
              timing.serialiseMs += Date.now() - serialiseStart;
            }
            if (serialized) {
              calendarEntry.events.push(serialized);
              timing.items++;
            }
          }
          if (yielder.shouldYield()) {
            await yielder.yieldNow();
          }
        }
      }
    })();

    try {
      await Promise.race([readPromise, timeoutPromise]);
    } catch (error) {
      timedOut = true;
      cancelQuery(stream, iterator, error);
      throw error;
    } finally {
      if (timer !== null) {
        clearTimeout(timer);
      }
      yielder.resetAfterProviderWait();
      // Avoid an unhandled rejection if the provider finishes after a timeout.
      readPromise.catch(() => {});
      if (logTiming) {
        timing.queryMs += Date.now() - queryStarted;
      }
    }
  }

  async function mapWithConcurrency(items, limit, callback) {
    const results = new Array(items.length);
    let nextIndex = 0;
    const workerCount = Math.min(limit, items.length);
    const workers = Array.from({ length: workerCount }, async () => {
      for (;;) {
        const index = nextIndex++;
        if (index >= items.length) {
          return;
        }
        results[index] = await callback(items[index], index);
      }
    });
    await Promise.all(workers);
    return results;
  }

  function makeCalendarEntry(calendar) {
    return {
      id: String(calendar.id),
      name: String(calendar.name || calendar.id),
      color: calendarColour(calendar),
      type: String(calendar.type || "unknown"),
      events: []
    };
  }

  function staleEntry(current, previous, errorText) {
    if (previous && previous.entry && Array.isArray(previous.entry.events)) {
      return {
        ...previous.entry,
        name: current.name,
        color: current.color,
        type: current.type,
        stale: true,
        retryable: true,
        status: "stale",
        error: errorText
      };
    }
    return {
      ...current,
      events: [],
      stale: true,
      retryable: true,
      status: "error",
      error: errorText
    };
  }

  return class extends ExtensionCommon.ExtensionAPI {
    onShutdown(isAppShutdown) {
      removeTracker();
      if (!isAppShutdown) {
        try {
          Services.obs.notifyObservers(null, "startupcache-invalidate", null);
        } catch (error) {
          // Startup-cache invalidation is best-effort during extension upgrades.
        }
      }
    }

    getAPI(context) {
      ensureTracker();
      return {
        tbCalendarBridge: {
          setDebugLogging(enabled) {
            debugLogsEnabled = enabled === true;
          },

          async getSnapshot(startIso, endIso, options) {
            const range = parseRange(startIso, endIso);
            const chunks = splitRange(range.start, range.end);
            const exportOptions = {
              includeDescriptions: !options || options.includeDescriptions !== false,
              includeLocations: Boolean(options && options.includeLocations === true),
              // Preserve compatibility with callers using the former shared
              // switch; the two new settings otherwise operate independently.
              truncateDescriptions: options && typeof options.truncateDescriptions === "boolean"
                ? options.truncateDescriptions
                : (!options || options.truncateText !== false),
              truncateLocations: options && typeof options.truncateLocations === "boolean"
                ? options.truncateLocations
                : (options && typeof options.truncateText === "boolean"
                  ? options.truncateText
                  : false),
              maxDescriptionChars: Math.min(1000, Math.max(1, Number(options && options.maxDescriptionChars) || 300)),
              maxLocationChars: Math.min(1000, Math.max(1, Number(options && options.maxLocationChars) || 200)),
              queryTimeoutSeconds: Math.min(300, Math.max(5,
                Number(options && options.queryTimeoutSeconds) || DEFAULT_QUERY_TIMEOUT_SECONDS))
            };
            debugLogsEnabled = Boolean(options && options.enableDebugLogs === true);
            const useCache = !options || options.useCache !== false;
            const timeoutMs = exportOptions.queryTimeoutSeconds * 1000;
            const logTiming = debugLogsEnabled;
            const yielder = createYielder();
            const timing = {
              providerMs: 0,
              queryMs: 0,
              serialiseMs: 0,
              items: 0,
              queries: 0,
              read: 0,
              cached: 0,
              failed: 0,
              stale: 0
            };
            const key = cacheKey(startIso, endIso, exportOptions);
            const liveCalendars = cal.manager.getCalendars();

            const results = await mapWithConcurrency(
              liveCalendars,
              MAX_CONCURRENT_CALENDARS,
              async calendar => {
                let current;
                try {
                  current = makeCalendarEntry(calendar);
                } catch (error) {
                  debugError("could not read calendar metadata:", error);
                  return null;
                }
                const id = current.id;
                const versionBefore = versionOf(id);
                const previous = cache.get(id);

                try {
                  if (calendar.getProperty("disabled") === true) {
                    current.status = "disabled";
                    cache.set(id, { key, version: versionBefore, entry: current });
                    return current;
                  }

                  if (useCache && previous && previous.key === key && previous.version === versionBefore) {
                    timing.cached++;
                    return previous.entry;
                  }

                  timing.read++;
                  const seen = new Set();
                  for (const [from, to] of chunks) {
                    if (yielder.shouldYield()) {
                      await yielder.yieldNow();
                    }
                    timing.queries++;
                    await readChunk(
                      calendar, from, to, current, exportOptions, seen,
                      yielder, timing, logTiming, timeoutMs
                    );
                  }

                  if (versionOf(id) !== versionBefore) {
                    timing.stale++;
                    const stale = staleEntry(current, previous && previous.key === key ? previous : null,
                      "Calendar changed while it was being read; a follow-up refresh is pending.");
                    stale.retryable = false;
                    return stale;
                  }

                  current.status = "ok";
                  cache.set(id, { key, version: versionBefore, entry: current });
                  return current;
                } catch (error) {
                  timing.failed++;
                  const errorText = String(error && error.message ? error.message : error);
                  debugError(`unable to read calendar ${current.name} (${id}):`, error);
                  timing.stale += previous && previous.key === key ? 1 : 0;
                  return staleEntry(current, previous && previous.key === key ? previous : null, errorText);
                }
              }
            );

            const calendars = results.filter(Boolean);
            const liveIds = new Set(calendars.map(entry => entry.id));
            for (const id of Array.from(cache.keys())) {
              if (!liveIds.has(id)) {
                cache.delete(id);
                calendarVersions.delete(id);
              }
            }

            const stats = yielder.finish();
            if (logTiming) {
              const eventCount = calendars.reduce((count, entry) => count + entry.events.length, 0);
              debugInfo(
                `read ${eventCount} events in ${timing.queries} queries from ${timing.read} calendars ` +
                `(${timing.cached} cached, ${timing.failed} failed, ${timing.stale} stale); ` +
                `provider waits ${timing.providerMs} ms, queries ${timing.queryMs} ms, ` +
                `serialising ${timing.serialiseMs} ms, ${stats.yields} yields, ` +
                `longest measured synchronous slice ${stats.longestBlockMs} ms`
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
                  debugError("change notification failed:", error);
                }
              };
              ensureTracker();
              listeners.add(notify);
              return () => listeners.delete(notify);
            }
          }).api()
        }
      };
    }
  };
})();
