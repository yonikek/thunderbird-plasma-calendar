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
  // Direct observer messages are deliberately much smaller than the native
  // messaging frame limit. Larger recurrence edits must use a full snapshot.
  const MAX_DIRECT_OBSERVER_CHANGES = 5000;
  const RECENT_ITEM_LOAD_SUPPRESSION_MS = 5000;
  const WATCHED_PROPERTIES = new Set(["name", "color", "disabled"]);

  let debugLogsEnabled = false;
  let liveObserverConfig = null;

  // Snapshot reads and provider batches share a lightweight generation guard.
  // A read that overlaps a batch is never cached as a complete calendar view;
  // after the batch closes, a full refresh is requested to repair any partial
  // result. This is deliberately metadata-only, not another event collection.
  const activeCalendarBatchDepth = new Map();
  let activeGlobalBatchDepth = 0;
  const snapshotReads = new Set();
  const snapshotBatchConflicts = new Set();

  function isCalendarBatchActive(calendarId) {
    return activeGlobalBatchDepth > 0 || (activeCalendarBatchDepth.get(calendarId) || 0) > 0;
  }

  function beginTrackedBatch(calendar) {
    const id = calendarIdOf(calendar);
    if (id === null) {
      activeGlobalBatchDepth++;
      for (const readingId of snapshotReads) {
        snapshotBatchConflicts.add(readingId);
      }
      return;
    }
    activeCalendarBatchDepth.set(id, (activeCalendarBatchDepth.get(id) || 0) + 1);
    if (snapshotReads.has(id) || activeGlobalBatchDepth > 0) {
      snapshotBatchConflicts.add(id);
    }
  }

  function endTrackedBatch(calendar) {
    const id = calendarIdOf(calendar);
    if (id === null) {
      activeGlobalBatchDepth = Math.max(0, activeGlobalBatchDepth - 1);
    } else {
      const depth = activeCalendarBatchDepth.get(id) || 0;
      if (depth <= 1) {
        activeCalendarBatchDepth.delete(id);
      } else {
        activeCalendarBatchDepth.set(id, depth - 1);
      }
    }

    let refreshNeeded = false;
    for (const conflictId of Array.from(snapshotBatchConflicts)) {
      if (!isCalendarBatchActive(conflictId)) {
        snapshotBatchConflicts.delete(conflictId);
        refreshNeeded = true;
      }
    }
    return refreshNeeded;
  }

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

  function isFloatingDateTime(dateTime) {
    try {
      const timezone = dateTime && dateTime.timezone;
      return Boolean(timezone && (timezone.isFloating === true || timezone.tzid === "floating"));
    } catch (error) {
      return false;
    }
  }

  function serializeDateTime(dateTime) {
    if (!dateTime) {
      return null;
    }
    if (dateTime.isDate) {
      return {
        value: `${dateTime.year}-${pad2(dateTime.month + 1)}-${pad2(dateTime.day)}`,
        floating: false
      };
    }
    if (isFloatingDateTime(dateTime)) {
      // A floating iCalendar DATE-TIME is a wall-clock value, not an instant.
      // getInTimezone(UTC) does not convert it; dateTimeToJsDate() then
      // interprets its fields using JavaScript Date semantics and can attach
      // the wrong offset. Preserve the fields and explicitly mark the value
      // so Plasma parses it as local time instead of UTC.
      const milliseconds = Number(dateTime.millisecond) || 0;
      const fraction = milliseconds === 0 ? "" : `.${String(milliseconds).padStart(3, "0")}`;
      return {
        value: `${dateTime.year}-${pad2(dateTime.month + 1)}-${pad2(dateTime.day)}` +
          `T${pad2(dateTime.hour)}:${pad2(dateTime.minute)}:${pad2(dateTime.second)}${fraction}`,
        floating: true
      };
    }
    return {
      value: cal.dtz.dateTimeToJsDate(dateTime.getInTimezone(cal.dtz.UTC)).toISOString(),
      floating: false
    };
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

    const serializedStart = serializeDateTime(start);
    const serializedEnd = serializeDateTime(end);
    if (!serializedStart || !serializedEnd) {
      return null;
    }

    return {
      uid: `${calendarId}:${item.id || "unknown"}:${recurrence}`,
      title: item.title || "(untitled)",
      description,
      location,
      start: serializedStart.value,
      end: serializedEnd.value,
      startFloating: serializedStart.floating,
      endFloating: serializedEnd.floating,
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

  function cloneDateTime(value) {
    return value && typeof value.clone === "function" ? value.clone() : value;
  }

  function itemOverlapsLiveRange(item) {
    if (!item || !liveObserverConfig) {
      return false;
    }
    const isTodo = typeof item.isTodo === "function" ? item.isTodo() : Boolean(item.isTodo);
    const { start, end } = itemDateTimes(item, isTodo);
    // Match getItems() range semantics: include an event if it overlaps the
    // requested interval, even when a multi-day event began before rangeStart.
    return Boolean(start && end
      && start.compare(liveObserverConfig.rangeEnd) < 0
      && end.compare(liveObserverConfig.rangeStart) > 0);
  }

  // Observer notifications carry the changed item, unlike onChanged which
  // merely invalidates a calendar cache. Recurring masters must be expanded
  // into the same occurrence objects getItems(ITEM_FILTER_CLASS_OCCURRENCES)
  // returns for the currently exported range.
  function observerOccurrences(item) {
    if (!item || !liveObserverConfig) {
      return null;
    }
    // A recurrence occurrence/exception already identifies one instance by
    // recurrenceId (and points back to its master through parentItem). Expanding
    // its parent's recurrenceInfo would turn one edited occurrence into changes
    // for the entire series.
    if (item.recurrenceId || (item.parentItem && item.parentItem !== item)) {
      return itemOverlapsLiveRange(item) ? [item] : [];
    }
    const recurrenceInfo = item.recurrenceInfo;
    if (recurrenceInfo) {
      // Ask Thunderbird for only one more than the direct-message budget. The
      // extra occurrence is a sentinel: seeing it proves that the complete
      // series cannot be delivered incrementally. This bounds expansion and
      // serialisation work instead of materialising up to 100,000 occurrences
      // only to reject the resulting message later.
      const occurrenceLimit = MAX_DIRECT_OBSERVER_CHANGES + 1;
      const occurrences = recurrenceInfo.getOccurrences(
        cloneDateTime(liveObserverConfig.rangeStart),
        cloneDateTime(liveObserverConfig.rangeEnd),
        occurrenceLimit
      );
      // Reaching the sentinel may mean additional occurrences were truncated;
      // use the authoritative full-snapshot path rather than silently omitting
      // any part of the recurring series.
      if (occurrences.length >= occurrenceLimit) {
        return null;
      }
      return occurrences.filter(itemOverlapsLiveRange);
    }
    return itemOverlapsLiveRange(item) ? [item] : [];
  }

  function serializeObservedItems(item) {
    if (!item || !liveObserverConfig) {
      return null;
    }
    const calendar = itemCalendar(item);
    const calendarId = calendarIdOf(calendar);
    if (calendarId === null) {
      return null;
    }
    try {
      if (calendar.getProperty("disabled") === true) {
        return [];
      }
    } catch (error) {
      // A provider may not expose the disabled property; treat it as enabled.
    }

    const occurrences = observerOccurrences(item);
    if (!Array.isArray(occurrences)) {
      return null;
    }
    const color = calendarColour(calendar);
    const result = [];
    for (const occurrence of occurrences) {
      const recurrence = recurrenceString(occurrence);
      const serialized = serializeItem(
        occurrence, calendarId, color, liveObserverConfig.options, recurrence
      );
      if (serialized) {
        result.push({ calendarId, event: serialized });
      }
    }
    return result;
  }

  function observerChangeList(operation, newItem, oldItem) {
    if (!liveObserverConfig) {
      return null;
    }
    if (operation === "add") {
      const current = serializeObservedItems(newItem);
      return current === null ? null : current.map(value => ({ operation: "add", ...value }));
    }
    if (operation === "remove") {
      const previous = serializeObservedItems(oldItem);
      return previous === null ? null : previous.map(value => ({ operation: "remove", ...value }));
    }

    const previous = serializeObservedItems(oldItem);
    const current = serializeObservedItems(newItem);
    if (previous === null || current === null) {
      return null;
    }
    // A modification may require serialising both the old and new recurrence
    // sets. Bound their combined size before building maps or a diff array; a
    // pair of individually acceptable series can still exceed the protocol
    // budget when considered together.
    if (previous.length + current.length > MAX_DIRECT_OBSERVER_CHANGES) {
      return null;
    }
    const previousByKey = new Map(previous.map(value => [`${value.calendarId}\u0000${value.event.uid}`, value]));
    const currentByKey = new Map(current.map(value => [`${value.calendarId}\u0000${value.event.uid}`, value]));
    const changes = [];
    for (const [key, oldValue] of previousByKey) {
      const newValue = currentByKey.get(key);
      if (!newValue) {
        changes.push({ operation: "remove", ...oldValue });
      } else {
        currentByKey.delete(key);
        if (JSON.stringify(oldValue.event) !== JSON.stringify(newValue.event)) {
          changes.push({ operation: "modify", ...newValue });
        }
      }
    }
    for (const value of currentByKey.values()) {
      changes.push({ operation: "add", ...value });
    }
    return changes;
  }

  function createObservers(notify, invalidateCalendar, notifyItemChanges) {
    const watched = name => WATCHED_PROPERTIES.has(String(name).toLowerCase());
    let batchDepth = 0;
    let batchNeedsRefresh = false;
    let batchHadItemChanges = false;
    let batchOverflow = false;
    const batchChanges = [];
    const batchInvalidated = new Set();
    const batchCalendarIds = new Set();
    const recentItemLoads = new Map();
    let batchGlobalInvalidated = false;

    const markRecentItemChange = calendar => {
      const id = calendarIdOf(calendar);
      if (id === null) {
        return;
      }
      const expiry = Date.now() + RECENT_ITEM_LOAD_SUPPRESSION_MS;
      recentItemLoads.set(id, expiry);
      setTimeout(() => {
        if (recentItemLoads.get(id) === expiry) {
          recentItemLoads.delete(id);
        }
      }, RECENT_ITEM_LOAD_SUPPRESSION_MS);
    };

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
        batchCalendarIds.add(id);
      }
    };

    const changed = calendar => {
      if (batchDepth > 0) {
        batchNeedsRefresh = true;
        invalidateDuringBatch(calendar);
        return;
      }
      invalidateCalendar(calendar);
      notify();
    };

    const itemChanged = (operation, newItem, oldItem) => {
      // Thunderbird can report a move between calendars through one
      // onModifyItem(newItem, oldItem) callback. Invalidate both sides: the old
      // calendar's cached copy must not be reused by the next full snapshot.
      const affectedCalendars = [];
      const seenCalendarIds = new Set();
      for (const item of [oldItem, newItem]) {
        if (!item) {
          continue;
        }
        const calendar = itemCalendar(item);
        const id = calendarIdOf(calendar);
        if (id === null) {
          if (calendar === null && !seenCalendarIds.has("<unknown>")) {
            seenCalendarIds.add("<unknown>");
            affectedCalendars.push({ calendar: null, id: null });
          }
          continue;
        }
        if (!seenCalendarIds.has(id)) {
          seenCalendarIds.add(id);
          affectedCalendars.push({ calendar, id });
        }
      }

      if (affectedCalendars.length === 0) {
        affectedCalendars.push({ calendar: null, id: null });
      }
      for (const affected of affectedCalendars) {
        if (batchDepth > 0) {
          invalidateDuringBatch(affected.calendar);
        } else {
          invalidateCalendar(affected.calendar);
        }
      }

      // Once a provider batch has overflowed, the full snapshot is the only
      // authoritative result. Continue invalidating affected calendars, but
      // do not expand or serialise more recurrence instances from this batch.
      if (batchDepth > 0 && batchOverflow) {
        batchHadItemChanges = true;
        batchNeedsRefresh = true;
        return;
      }

      let changes;
      try {
        changes = observerChangeList(operation, newItem, oldItem);
      } catch (error) {
        debugError("could not serialise observed calendar change; scheduling a snapshot refresh:", error);
        changes = null;
      }
      if (changes === null) {
        if (batchDepth > 0) {
          // A partially serialisable batch cannot be safely applied as deltas.
          // Discard any queued prefix and recover from the complete snapshot.
          batchHadItemChanges = true;
          batchChanges.length = 0;
          batchOverflow = true;
          batchNeedsRefresh = true;
        } else {
          notify();
        }
        return;
      }
      if (changes.length === 0) {
        return;
      }

      // Check a single observer callback before retaining or forwarding its
      // occurrence list. A large recurring master can expand to thousands of
      // concrete changes even outside a provider batch.
      if (changes.length > MAX_DIRECT_OBSERVER_CHANGES) {
        if (batchDepth > 0) {
          batchHadItemChanges = true;
          batchChanges.length = 0;
          batchOverflow = true;
          batchNeedsRefresh = true;
        }
        debugInfo("calendar observer change exceeded the direct-update limit; falling back to a snapshot refresh",
          changes.length);
        if (batchDepth === 0) {
          notify();
        }
        return;
      }

      for (const affected of affectedCalendars) {
        if (affected.id !== null) {
          markRecentItemChange({ id: affected.id });
        }
      }
      if (batchDepth > 0) {
        batchHadItemChanges = true;
        if (!batchOverflow) {
          if (batchChanges.length + changes.length > MAX_DIRECT_OBSERVER_CHANGES) {
            batchChanges.length = 0;
            batchOverflow = true;
            batchNeedsRefresh = true;
            debugInfo("calendar batch exceeded the direct-update limit; falling back to a snapshot refresh",
              affectedCalendars.map(value => value.id || "unknown calendar").join(", "));
          } else {
            // Avoid argument spreading even for bounded batches. A loop also
            // makes this path independent of the engine's argument-count cap.
            for (const change of changes) {
              batchChanges.push(change);
            }
          }
        }
      } else {
        notifyItemChanges(changes);
      }
    };

    const calendarObserver = {
      QueryInterface: ChromeUtils.generateQI(["calIObserver"]),
      onStartBatch(calendar) {
        beginTrackedBatch(calendar);
        batchDepth++;
        invalidateDuringBatch(calendar);
      },
      onEndBatch(calendar) {
        const snapshotConflictResolved = endTrackedBatch(calendar);
        if (batchDepth <= 0) {
          changed(calendar);
          if (snapshotConflictResolved) {
            notify();
          }
          return;
        }
        batchDepth--;
        if (batchDepth === 0) {
          if (batchHadItemChanges && !batchOverflow && batchChanges.length > 0) {
            notifyItemChanges(batchChanges.splice(0));
          }
          if (batchNeedsRefresh || batchOverflow || snapshotConflictResolved) {
            notify();
          }
          for (const id of batchCalendarIds) {
            markRecentItemChange({ id });
          }
          batchNeedsRefresh = false;
          batchHadItemChanges = false;
          batchOverflow = false;
          batchChanges.length = 0;
          batchInvalidated.clear();
          batchCalendarIds.clear();
          batchGlobalInvalidated = false;
        } else if (snapshotConflictResolved) {
          notify();
        }
      },
      onLoad(calendar) {
        const id = calendarIdOf(calendar);
        const expiry = id === null ? 0 : (recentItemLoads.get(id) || 0);
        if (expiry > Date.now()) {
          recentItemLoads.delete(id);
          debugInfo("ignored redundant onLoad following direct item notifications", id);
          return;
        }
        changed(calendar);
      },
      onAddItem(item) { itemChanged("add", item, null); },
      onModifyItem(newItem, oldItem) { itemChanged("modify", newItem, oldItem); },
      onDeleteItem(item) { itemChanged("remove", null, item); },
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
  const itemChangeListeners = new Set();
  let tracker = null;

  function ensureTracker() {
    if (tracker) {
      return;
    }
    tracker = createObservers(() => {
      for (const notify of listeners) {
        notify();
      }
    }, invalidate, changes => {
      for (const notify of itemChangeListeners) {
        notify(changes);
      }
    });
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
    itemChangeListeners.clear();
    liveObserverConfig = null;
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
            liveObserverConfig = {
              rangeStart: cal.dtz.jsDateToDateTime(range.start),
              rangeEnd: cal.dtz.jsDateToDateTime(range.end),
              options: exportOptions
            };
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
                  if (isCalendarBatchActive(id)) {
                    snapshotBatchConflicts.add(id);
                    timing.stale++;
                    const stale = staleEntry(current, previous && previous.key === key ? previous : null,
                      "Snapshot deferred because the calendar provider is inside a batch operation.");
                    stale.retryable = false;
                    return stale;
                  }

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
                  snapshotReads.add(id);
                  try {
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
                  } finally {
                    snapshotReads.delete(id);
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
          }).api(),

          onItemChanges: new ExtensionCommon.EventManager({
            context,
            name: "tbCalendarBridge.onItemChanges",
            register(fire) {
              const notify = changes => {
                try {
                  if (Array.isArray(changes) && changes.length > 0) {
                    fire.async(changes);
                  }
                } catch (error) {
                  debugError("direct item-change notification failed:", error);
                }
              };
              ensureTracker();
              itemChangeListeners.add(notify);
              return () => itemChangeListeners.delete(notify);
            }
          }).api()
        }
      };
    }
  };
})();
