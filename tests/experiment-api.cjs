const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../extension/experiments/implementation.js'), 'utf8');
const observers = {};
let activeReads = 0;
let maxActiveReads = 0;
let readFailure = false;
let failNever = false;
let consoleErrors = 0;
const mockConsole = {
  error: (...args) => { consoleErrors++; },
  log: (...args) => realConsole.log(...args),
};
function dt(value) {
  const result = new Date(value);
  result.isDate = false;
  result.getInTimezone = () => result;
  result.clone = () => dt(value);
  result.compare = other => result.getTime() - other.getTime();
  return result;
}
const event = {
  id: 'evt-1', title: 'Test event', isTodo: false, recurrenceInfo: null,
  startDate: dt('2026-11-20T10:00:00Z'),
  endDate: dt('2026-11-20T11:00:00Z'),
  descriptionText: 'abcdefghijk',
  getProperty(name) { return name === 'LOCATION' ? 'Somewhere' : ''; },
};
function makeCalendar(id, delayMs = 10) {
  return {
    id, name: id, type: 'local',
    getProperty(name) { return name === 'disabled' ? false : name === 'color' ? '#123456' : ''; },
    getItems() {
      if (readFailure) throw new Error('provider offline');
      return {
        async *[Symbol.asyncIterator]() {
          activeReads++;
          maxActiveReads = Math.max(maxActiveReads, activeReads);
          try {
            if (failNever) await new Promise(() => {});
            await new Promise(resolve => setTimeout(resolve, delayMs));
            yield [event];
          } finally {
            activeReads--;
          }
        }
      };
    }
  };
}
const calendar = makeCalendar('calendar-1');
event.calendar = calendar;
const manager = {
  getCalendars: () => [calendar],
  addCalendarObserver(observer) { observers.calendar = observer; },
  removeCalendarObserver() {},
  addObserver(observer) { observers.manager = observer; },
  removeObserver() {}
};
const cal = {
  manager,
  iterate: { streamValues(stream) { return stream; } },
  dtz: {
    UTC: 'UTC',
    jsDateToDateTime(date) { return date; },
    dateTimeToJsDate(date) { return date; }
  }
};
class ExtensionAPI {}
class EventManager {
  constructor({ register }) { this.register = register; }
  api() {
    return { addListener: listener => this.register({ async: (...args) => listener(...args) }) };
  }
}
const ExtensionCommon = { ExtensionAPI, EventManager };
const context = {
  ChromeUtils: {
    importESModule(url) {
      if (url.includes('ExtensionCommon')) return { ExtensionCommon };
      if (url.includes('calUtils')) return { cal };
      if (url.includes('Timer')) return { setTimeout, clearTimeout };
      throw new Error(`unexpected import ${url}`);
    },
    generateQI: () => () => ({}),
  },
  Ci: { calICalendar: {
    ITEM_FILTER_TYPE_EVENT: 1,
    ITEM_FILTER_TYPE_TODO: 2,
    ITEM_FILTER_CLASS_OCCURRENCES: 4,
    ITEM_FILTER_COMPLETED_ALL: 8,
  } },
  Services: { obs: { notifyObservers() {} } },
  console: mockConsole,
  Date, Set, Map, Promise, String, Number, Boolean, Math, Error, Array,
  TextEncoder, performance,
  setTimeout, clearTimeout,
};
vm.createContext(context);
vm.runInContext(source, context);
const ApiClass = context.tbCalendarBridge;
const api = new ApiClass().getAPI({}).tbCalendarBridge;

(async () => {
  const start = '2026-11-20T00:00:00.000Z';
  const end = '2026-11-22T00:00:00.000Z';
  const options = {
    includeDescriptions: true, includeLocations: true,
    truncateDescriptions: true, truncateLocations: true,
    maxDescriptionChars: 6, maxLocationChars: 5,
    queryTimeoutSeconds: 5, enableDebugLogs: false, useCache: true
  };
  const first = await api.getSnapshot(start, end, options);
  if (first.calendars[0].events[0].description !== 'abcde…') throw new Error('description truncation failed');
  if (first.calendars[0].events[0].location !== 'Some…') throw new Error('location truncation failed');
  if (first.calendars[0].status !== 'ok') throw new Error('successful status missing');

  readFailure = true;
  observers.calendar.onError(calendar, 'TEST', 'offline');
  const second = await api.getSnapshot(start, end, options);
  if (!second.calendars[0].stale) throw new Error('failed read was not marked stale');
  if (second.calendars[0].events.length !== 1) throw new Error('last-known-good events were not preserved');
  if (second.calendars[0].error !== 'provider offline') throw new Error('provider error not recorded');
  if (consoleErrors !== 0) throw new Error('error was logged while debug logging was disabled');

  readFailure = false;
  observers.calendar.onAddItem(event);
  api.setDebugLogging(true);
  observers.calendar.onError(calendar, 'TEST', 'debug-visible');
  if (consoleErrors === 0) throw new Error('provider errors not logged when debug logging enabled');

  const descriptionOnly = await api.getSnapshot(start, end, {
    ...options, truncateDescriptions: true, truncateLocations: false, useCache: false
  });
  if (descriptionOnly.calendars[0].events[0].description !== 'abcde…') throw new Error('independent description truncation failed');
  if (descriptionOnly.calendars[0].events[0].location !== 'Somewhere') throw new Error('location truncation should be independently disabled');

  const locationOnly = await api.getSnapshot(start, end, {
    ...options, truncateDescriptions: false, truncateLocations: true, useCache: false
  });
  if (locationOnly.calendars[0].events[0].description !== 'abcdefghijk') throw new Error('description truncation should be independently disabled');
  if (locationOnly.calendars[0].events[0].location !== 'Some…') throw new Error('independent location truncation failed');

  const newInstallDefaults = await api.getSnapshot('2026-11-23T00:00:00.000Z', '2026-11-25T00:00:00.000Z', {
    includeDescriptions: true, includeLocations: true,
    maxDescriptionChars: 6, maxLocationChars: 5, useCache: false
  });
  if (newInstallDefaults.calendars[0].events[0].description !== 'abcde…') throw new Error('description truncation should default to enabled');
  if (newInstallDefaults.calendars[0].events[0].location !== 'Somewhere') throw new Error('location truncation should default to disabled');

  // Restore the event's test range before exercising live notifications.
  await api.getSnapshot(start, end, { ...options, enableDebugLogs: false, useCache: false });

  // Item observers produce direct serialized changes without requesting a full snapshot.
  const directChanges = [];
  let fallbackRefreshes = 0;
  api.onChanged.addListener(() => { fallbackRefreshes++; });
  api.onItemChanges.addListener(changes => directChanges.push(changes));
  observers.calendar.onAddItem(event);
  if (directChanges.length !== 1 || directChanges[0][0].operation !== 'add') {
    throw new Error('onAddItem did not emit a direct add change');
  }
  const oldEvent = {
    ...event,
    title: 'Old title',
    startDate: dt('2026-11-20T10:00:00Z'),
    endDate: dt('2026-11-20T11:00:00Z'),
    calendar
  };
  event.title = 'Updated title';
  observers.calendar.onModifyItem(event, oldEvent);
  if (directChanges[1]?.[0]?.operation !== 'modify'
      || directChanges[1][0].event.title !== 'Updated title') {
    throw new Error('onModifyItem did not emit a direct modify change');
  }
  const movedOldEvent = {
    ...event,
    startDate: dt('2026-11-20T10:00:00Z'),
    endDate: dt('2026-11-20T11:00:00Z'),
    calendar
  };
  const movedNewEvent = {
    ...event,
    startDate: dt('2026-11-21T10:00:00Z'),
    endDate: dt('2026-11-21T11:00:00Z'),
    calendar
  };
  observers.calendar.onModifyItem(movedNewEvent, movedOldEvent);
  if (directChanges[2]?.[0]?.operation !== 'modify'
      || directChanges[2][0].event.start !== '2026-11-21T10:00:00.000Z') {
    throw new Error('date move was not forwarded as a direct modification with the new start date');
  }
  observers.calendar.onDeleteItem(event);
  if (directChanges[3]?.[0]?.operation !== 'remove') {
    throw new Error('onDeleteItem did not emit a direct remove change');
  }

  // A recurrence exception is already one concrete occurrence; direct observer
  // serialization must still exclude it when it lies outside the exported range.
  const outsideOccurrence = {
    ...event,
    id: 'evt-recurring',
    recurrenceId: { icalString: '20261220T100000Z' },
    startDate: dt('2026-12-20T10:00:00Z'),
    endDate: dt('2026-12-20T11:00:00Z'),
    calendar
  };
  const changesBeforeOutsideOccurrence = directChanges.length;
  observers.calendar.onAddItem(outsideOccurrence);
  if (directChanges.length !== changesBeforeOutsideOccurrence) {
    throw new Error('out-of-range recurrence occurrence leaked into direct changes');
  }
  const inRangeOccurrence = {
    ...event,
    id: 'evt-recurring',
    recurrenceId: { icalString: '20261120T100000Z' },
    startDate: dt('2026-11-20T10:00:00Z'),
    endDate: dt('2026-11-20T11:00:00Z'),
    calendar
  };
  observers.calendar.onAddItem(inRangeOccurrence);
  if (directChanges.length !== changesBeforeOutsideOccurrence + 1
      || directChanges.at(-1)?.[0]?.event?.uid !== 'calendar-1:evt-recurring:20261120T100000Z') {
    throw new Error('in-range recurrence occurrence was not represented as one direct item change');
  }
  const recurringMaster = {
    ...event,
    id: 'evt-series',
    recurrenceInfo: {
      getOccurrences() { return [inRangeOccurrence]; }
    },
    calendar
  };
  observers.calendar.onAddItem(recurringMaster);
  if (directChanges.at(-1)?.[0]?.event?.uid !== 'calendar-1:evt-recurring:20261120T100000Z') {
    throw new Error('recurring master was not expanded into its in-range occurrence');
  }

  // Batch notifications are coalesced into one direct extension event, not a full refresh.
  directChanges.length = 0;
  observers.calendar.onStartBatch(calendar);
  observers.calendar.onAddItem(event);
  observers.calendar.onModifyItem(event, oldEvent);
  observers.calendar.onDeleteItem(event);
  observers.calendar.onEndBatch(calendar);
  if (directChanges.length !== 1 || directChanges[0].length !== 3) {
    throw new Error(`batch emitted ${directChanges.length} direct notifications containing ${directChanges[0]?.length || 0} changes`);
  }
  if (fallbackRefreshes !== 0) throw new Error('ordinary item changes unexpectedly requested a full calendar refresh');

  // Five calendars are used to verify that at most three providers are active.
  manager.getCalendars = () => [
    makeCalendar('a', 25), makeCalendar('b', 25), makeCalendar('c', 25),
    makeCalendar('d', 25), makeCalendar('e', 25)
  ];
  maxActiveReads = 0;
  await api.getSnapshot(start, end, { ...options, enableDebugLogs: false, useCache: false });
  if (maxActiveReads > 3) throw new Error(`calendar concurrency exceeded three: ${maxActiveReads}`);

  // A stuck provider must time out instead of leaving the snapshot pending.
  manager.getCalendars = () => [makeCalendar('stuck', 0)];
  failNever = true;
  const timeoutStarted = Date.now();
  const timedOut = await api.getSnapshot(start, end, { ...options, enableDebugLogs: false, queryTimeoutSeconds: 5, useCache: false });
  const timeoutElapsed = Date.now() - timeoutStarted;
  if (timedOut.calendars[0].status !== 'error') throw new Error('timed-out calendar did not report an error state');
  if (timeoutElapsed < 4900 || timeoutElapsed > 9000) throw new Error(`query timeout duration unexpected: ${timeoutElapsed} ms`);

  console.log('Experiment API mock tests passed: independent truncation, last-good fallback, debug gating, direct add/modify/move/remove notifications, batch coalescing, concurrency <= 3, query timeout.');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
