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
const calendarItems = new Map();
const calendarReadCounts = new Map();
const calendarReadGates = new Map();
const mockConsole = {
  error: (...args) => { consoleErrors++; },
  log: (...args) => realConsole.log(...args),
};
function dt(value) {
  const result = new Date(value);
  result.isDate = false;
  result.timezone = { tzid: 'UTC', isFloating: false };
  result.getInTimezone = () => result;
  result.clone = () => dt(value);
  result.compare = other => result.getTime() - other.getTime();
  return result;
}
function floatingDt(value, timezone = { tzid: 'floating', isFloating: true }) {
  const match = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,3}))?$/.exec(value);
  if (!match) throw new Error(`invalid floating test datetime: ${value}`);
  const [, year, month, day, hour, minute, second, fraction = '0'] = match;
  const fields = {
    year: Number(year), month: Number(month) - 1, day: Number(day),
    hour: Number(hour), minute: Number(minute), second: Number(second),
    millisecond: Number(fraction.padEnd(3, '0'))
  };
  // The Date's epoch is only used by the test provider's range-overlap
  // comparison. Serialization must use the explicit fields above and must
  // never ask Thunderbird to convert this floating value to UTC.
  const result = new Date(Date.UTC(fields.year, fields.month, fields.day,
    fields.hour, fields.minute, fields.second, fields.millisecond));
  Object.assign(result, fields, {
    isDate: false,
    timezone,
    getInTimezone() { throw new Error('floating date-time must not be converted to another timezone'); },
    clone() { return floatingDt(value); },
    compare(other) { return this.getTime() - other.getTime(); }
  });
  return result;
}
function dateOnly(value) {
  const [year, month, day] = value.split('-').map(Number);
  const result = new Date(Date.UTC(year, month - 1, day));
  Object.assign(result, {
    year, month: month - 1, day, hour: 0, minute: 0, second: 0, millisecond: 0,
    isDate: true,
    timezone: { tzid: 'floating', isFloating: true },
    getInTimezone() { throw new Error('DATE value must not be converted to a timezone'); },
    clone() { return dateOnly(value); },
    compare(other) { return this.getTime() - other.getTime(); }
  });
  return result;
}
function makeTestEvent(id, startDate, endDate) {
  return {
    id, title: id, isTodo: false, recurrenceInfo: null,
    startDate, endDate, calendar,
    descriptionText: '',
    getProperty() { return ''; }
  };
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
      const gate = calendarReadGates.get(id);
      const items = calendarItems.has(id) ? calendarItems.get(id) : [event];
      return {
        async *[Symbol.asyncIterator]() {
          activeReads++;
          maxActiveReads = Math.max(maxActiveReads, activeReads);
          calendarReadCounts.set(id, (calendarReadCounts.get(id) || 0) + 1);
          try {
            if (gate) {
              gate.signalStarted();
              await gate.promise;
            }
            if (failNever) await new Promise(() => {});
            await new Promise(resolve => setTimeout(resolve, delayMs));
            yield items;
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

  // Floating DATE-TIME values are wall-clock fields, not UTC instants. Cover
  // ordinary winter time, the spring-forward boundary, the repeated autumn
  // hour, a multi-day event spanning the spring change, and fractional seconds.
  // getInTimezone() intentionally throws for these mocks: accidental UTC
  // conversion must fail this test rather than silently shift their times.
  const floatingEvents = [
    makeTestEvent('floating-winter', floatingDt('2026-01-15T09:15:00'), floatingDt('2026-01-15T09:45:00')),
    makeTestEvent('floating-spring-after-transition', floatingDt('2026-03-29T03:30:00'), floatingDt('2026-03-29T04:00:00')),
    makeTestEvent('floating-autumn-repeated-hour', floatingDt('2026-10-25T01:30:00'), floatingDt('2026-10-25T02:15:00')),
    makeTestEvent('floating-crosses-spring-transition', floatingDt('2026-03-28T23:30:00'), floatingDt('2026-03-29T03:30:00')),
    makeTestEvent('floating-fractional-seconds', floatingDt('2026-06-01T12:34:56.125'), floatingDt('2026-06-01T12:35:56.875')),
    // Thunderbird exposes floating semantics through timezone.isFloating and
    // the special `floating` tzid across different wrappers/versions.
    makeTestEvent('floating-isFloating-only',
      floatingDt('2026-06-02T10:00:00', { tzid: 'Europe/London', isFloating: true }),
      floatingDt('2026-06-02T11:00:00', { tzid: 'Europe/London', isFloating: true })),
    makeTestEvent('floating-tzid-only',
      floatingDt('2026-06-03T10:00:00', { tzid: 'floating' }),
      floatingDt('2026-06-03T11:00:00', { tzid: 'floating' })),
    // Start and end metadata are evaluated independently, so a provider
    // exposing mixed timezone semantics cannot accidentally mark both alike.
    makeTestEvent('mixed-endpoint-semantics',
      floatingDt('2026-06-04T09:00:00'), dt('2026-06-04T10:00:00Z')),
    // The same display time can also be represented as an absolute instant.
    // This one must retain the existing UTC-normalisation behaviour.
    makeTestEvent('timezone-qualified', dt('2026-07-20T09:00:00+02:00'), dt('2026-07-20T10:00:00+02:00')),
    makeTestEvent('all-day-date', dateOnly('2026-05-10'), dateOnly('2026-05-11'))
  ];
  calendarItems.set('calendar-1', floatingEvents);
  const floatingSnapshot = await api.getSnapshot(
    '2026-01-01T00:00:00.000Z', '2027-01-01T00:00:00.000Z',
    { ...options, enableDebugLogs: false, useCache: false }
  );
  const floatingById = new Map(floatingSnapshot.calendars[0].events.map(value => [value.uid.split(':')[1], value]));
  const expectedFloating = [
    ['floating-winter', '2026-01-15T09:15:00', '2026-01-15T09:45:00'],
    ['floating-spring-after-transition', '2026-03-29T03:30:00', '2026-03-29T04:00:00'],
    ['floating-autumn-repeated-hour', '2026-10-25T01:30:00', '2026-10-25T02:15:00'],
    ['floating-crosses-spring-transition', '2026-03-28T23:30:00', '2026-03-29T03:30:00'],
    ['floating-fractional-seconds', '2026-06-01T12:34:56.125', '2026-06-01T12:35:56.875'],
    ['floating-isFloating-only', '2026-06-02T10:00:00', '2026-06-02T11:00:00'],
    ['floating-tzid-only', '2026-06-03T10:00:00', '2026-06-03T11:00:00']
  ];
  for (const [id, expectedStart, expectedEnd] of expectedFloating) {
    const serialized = floatingById.get(id);
    if (!serialized) throw new Error(`floating event ${id} was not exported`);
    if (serialized.start !== expectedStart || serialized.end !== expectedEnd) {
      throw new Error(`${id} wall-clock time changed: ${serialized.start} – ${serialized.end}`);
    }
    if (serialized.startFloating !== true || serialized.endFloating !== true) {
      throw new Error(`${id} did not preserve its floating-time markers`);
    }
    if (serialized.start.endsWith('Z') || /[+-]\d{2}:?\d{2}$/.test(serialized.start)) {
      throw new Error(`${id} was incorrectly assigned an explicit timezone`);
    }
  }
  const mixedEndpoint = floatingById.get('mixed-endpoint-semantics');
  if (mixedEndpoint.start !== '2026-06-04T09:00:00'
      || mixedEndpoint.end !== '2026-06-04T10:00:00.000Z'
      || mixedEndpoint.startFloating !== true || mixedEndpoint.endFloating !== false) {
    throw new Error('start and end floating-time semantics were not serialised independently');
  }
  const qualified = floatingById.get('timezone-qualified');
  if (qualified.start !== '2026-07-20T07:00:00.000Z'
      || qualified.end !== '2026-07-20T08:00:00.000Z'
      || qualified.startFloating !== false || qualified.endFloating !== false) {
    throw new Error('timezone-qualified event no longer normalises to UTC correctly');
  }
  const allDay = floatingById.get('all-day-date');
  if (allDay.start !== '2026-05-10' || allDay.end !== '2026-05-11'
      || allDay.allDay !== true || allDay.startFloating !== false) {
    throw new Error('all-day DATE values were changed by floating-time handling');
  }
  // Restore the ordinary fixture for the rest of the observer/cache tests.
  calendarItems.delete('calendar-1');
  await api.getSnapshot(start, end, { ...options, enableDebugLogs: false, useCache: false });

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

  const largeRecurrenceMaxCounts = [];
  function largeRecurringMaster(id, count) {
    return {
      ...event,
      id,
      recurrenceInfo: {
        getOccurrences(_rangeStart, _rangeEnd, maxCount) {
          // Mirror Thunderbird's maxCount contract and record the requested
          // cap so the regression test can prove the expansion is bounded.
          largeRecurrenceMaxCounts.push(maxCount);
          return Array.from({ length: Math.min(count, maxCount) }, (_, index) => ({
            ...event,
            id,
            recurrenceId: { icalString: `20261120T${String(index % 24).padStart(2, '0')}0000Z-${index}` },
            startDate: dt('2026-11-20T10:00:00Z'),
            endDate: dt('2026-11-20T11:00:00Z'),
            calendar
          }));
        }
      },
      calendar
    };
  }

  // A single recurring-series edit must be size-checked before forwarding its
  // expanded occurrence list to the background script.
  directChanges.length = 0;
  let refreshesBeforeLargeDirectChange = fallbackRefreshes;
  observers.calendar.onAddItem(largeRecurringMaster('large-direct-series', 5001));
  if (directChanges.length !== 0 || fallbackRefreshes !== refreshesBeforeLargeDirectChange + 1) {
    throw new Error('oversized direct recurrence change was not replaced with one full-snapshot refresh');
  }
  if (largeRecurrenceMaxCounts.at(-1) !== 5001) {
    throw new Error(`recurrence expansion was not bounded to the 5,001-item sentinel: ${largeRecurrenceMaxCounts.at(-1)}`);
  }

  // A series modification must count both old and new expansions before
  // allocating its keyed diff maps. Each side is below the per-series cap,
  // but their combined six thousand occurrences exceed the direct budget.
  directChanges.length = 0;
  refreshesBeforeLargeDirectChange = fallbackRefreshes;
  observers.calendar.onModifyItem(
    largeRecurringMaster('large-modified-series', 3000),
    largeRecurringMaster('large-modified-series', 3000)
  );
  if (directChanges.length !== 0 || fallbackRefreshes !== refreshesBeforeLargeDirectChange + 1) {
    throw new Error('oversized old-plus-new recurrence modification did not fall back to a full snapshot');
  }

  // A batch's cumulative size must be checked before appending the second
  // recurrence expansion. This exercises the overflow branch without relying
  // on the JavaScript engine's maximum function-argument count.
  directChanges.length = 0;
  refreshesBeforeLargeDirectChange = fallbackRefreshes;
  observers.calendar.onStartBatch(calendar);
  observers.calendar.onAddItem(largeRecurringMaster('large-batch-series-a', 3000));
  observers.calendar.onAddItem(largeRecurringMaster('large-batch-series-b', 3000));
  const expansionsAtOverflow = largeRecurrenceMaxCounts.length;
  observers.calendar.onAddItem(largeRecurringMaster('large-batch-series-c', 3000));
  if (largeRecurrenceMaxCounts.length !== expansionsAtOverflow) {
    throw new Error('provider batch continued expanding recurrence items after its safety limit was exceeded');
  }
  observers.calendar.onEndBatch(calendar);
  if (directChanges.length !== 0 || fallbackRefreshes !== refreshesBeforeLargeDirectChange + 1) {
    throw new Error('oversized cumulative batch did not fall back to one full-snapshot refresh');
  }

  // Moving an event between calendars invalidates both cached calendar entries.
  // The next cached snapshot must reread both providers and reflect the move.
  const secondCalendar = makeCalendar('calendar-2');
  const crossCalendarOld = { ...event, id: 'cross-calendar-move', title: 'Before move', calendar };
  const crossCalendarNew = { ...crossCalendarOld, title: 'After move', calendar: secondCalendar };
  calendarItems.set('calendar-1', [event, crossCalendarOld]);
  calendarItems.set('calendar-2', []);
  manager.getCalendars = () => [calendar, secondCalendar];
  await api.getSnapshot(start, end, { ...options, enableDebugLogs: false, useCache: false });
  const sourceReadsBeforeMove = calendarReadCounts.get('calendar-1') || 0;
  const destinationReadsBeforeMove = calendarReadCounts.get('calendar-2') || 0;
  directChanges.length = 0;
  observers.calendar.onModifyItem(crossCalendarNew, crossCalendarOld);
  const crossCalendarChanges = directChanges.at(-1) || [];
  if (crossCalendarChanges.length !== 2
      || crossCalendarChanges[0].operation !== 'remove'
      || crossCalendarChanges[0].calendarId !== 'calendar-1'
      || crossCalendarChanges[1].operation !== 'add'
      || crossCalendarChanges[1].calendarId !== 'calendar-2') {
    throw new Error('cross-calendar move was not represented as removal from the source and addition to the destination');
  }
  calendarItems.set('calendar-1', [event]);
  calendarItems.set('calendar-2', [crossCalendarNew]);
  const afterCrossCalendarMove = await api.getSnapshot(start, end, { ...options, enableDebugLogs: false, useCache: true });
  if ((calendarReadCounts.get('calendar-1') || 0) !== sourceReadsBeforeMove + 1
      || (calendarReadCounts.get('calendar-2') || 0) !== destinationReadsBeforeMove + 1) {
    throw new Error('cross-calendar move did not invalidate both source and destination calendar caches');
  }
  const sourceEntry = afterCrossCalendarMove.calendars.find(entry => entry.id === 'calendar-1');
  const destinationEntry = afterCrossCalendarMove.calendars.find(entry => entry.id === 'calendar-2');
  if (sourceEntry.events.some(value => value.uid.includes('cross-calendar-move'))
      || !destinationEntry.events.some(value => value.uid.includes('cross-calendar-move'))) {
    throw new Error('cross-calendar move was not reflected in the next full snapshot');
  }
  calendarItems.delete('calendar-1');
  calendarItems.delete('calendar-2');
  manager.getCalendars = () => [calendar];
  await api.getSnapshot(start, end, { ...options, enableDebugLogs: false, useCache: false });

  // A snapshot requested while a provider is already inside a batch must defer
  // the provider query, retain last-good data, and request a refresh on batch end.
  let refreshesBeforeBatch = fallbackRefreshes;
  const readsBeforeActiveBatch = calendarReadCounts.get('calendar-1') || 0;
  observers.calendar.onStartBatch(calendar);
  const duringActiveBatch = await api.getSnapshot(start, end, { ...options, enableDebugLogs: false, useCache: true });
  if (duringActiveBatch.calendars[0].status !== 'stale'
      || duringActiveBatch.calendars[0].events.length !== 1) {
    throw new Error('snapshot during active batch did not preserve the last-known-good events as stale');
  }
  if ((calendarReadCounts.get('calendar-1') || 0) !== readsBeforeActiveBatch) {
    throw new Error('snapshot queried a provider while its batch was active');
  }
  observers.calendar.onEndBatch(calendar);
  if (fallbackRefreshes <= refreshesBeforeBatch) {
    throw new Error('batch completion did not request a follow-up snapshot after an active-batch deferral');
  }
  await api.getSnapshot(start, end, { ...options, enableDebugLogs: false, useCache: true });

  // A snapshot already reading from a provider must be invalidated if a batch
  // starts before the read completes. This prevents caching a partial batch.
  let releaseRead;
  let markReadStarted;
  const readStarted = new Promise(resolve => { markReadStarted = resolve; });
  const readGate = {
    promise: new Promise(resolve => { releaseRead = resolve; }),
    signalStarted: markReadStarted
  };
  calendarReadGates.set('calendar-1', readGate);
  const overlappingSnapshotPromise = api.getSnapshot(start, end, {
    ...options, enableDebugLogs: false, useCache: false
  });
  await readStarted;
  observers.calendar.onStartBatch(calendar);
  releaseRead();
  const overlappingSnapshot = await overlappingSnapshotPromise;
  if (overlappingSnapshot.calendars[0].status !== 'stale'
      || overlappingSnapshot.calendars[0].events.length !== 1) {
    throw new Error('snapshot overlapping a provider batch was incorrectly accepted as fresh');
  }
  calendarReadGates.delete('calendar-1');
  refreshesBeforeBatch = fallbackRefreshes;
  observers.calendar.onEndBatch(calendar);
  if (fallbackRefreshes <= refreshesBeforeBatch) {
    throw new Error('ending a batch overlapping a provider read did not schedule a repair refresh');
  }
  await api.getSnapshot(start, end, { ...options, enableDebugLogs: false, useCache: true });

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

  console.log('Experiment API mock tests passed: independent truncation, floating-time/DST serialization, last-good fallback, debug gating, direct add/modify/move/remove notifications, oversized recurrence fallback, cross-calendar cache invalidation, batch/snapshot race guards, batch coalescing, concurrency <= 3, query timeout.');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
