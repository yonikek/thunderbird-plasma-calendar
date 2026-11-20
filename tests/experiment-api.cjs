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
  return result;
}
const event = {
  id: 'evt-1', title: 'Test event', isTodo: false,
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
    return { addListener: listener => this.register({ async: () => listener() }) };
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
    truncateText: true, maxDescriptionChars: 6, maxLocationChars: 5,
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

  // Batch notifications are coalesced into one extension event.
  let changeEvents = 0;
  api.onChanged.addListener(() => { changeEvents++; });
  observers.calendar.onStartBatch(calendar);
  observers.calendar.onAddItem(event);
  observers.calendar.onModifyItem(event);
  observers.calendar.onDeleteItem(event);
  observers.calendar.onEndBatch(calendar);
  if (changeEvents !== 1) throw new Error(`batch emitted ${changeEvents} change notifications instead of one`);

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

  console.log('Experiment API mock tests passed: truncation, last-good fallback, debug gating, batch coalescing, concurrency <= 3, query timeout.');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
