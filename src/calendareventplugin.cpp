/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "calendareventplugin.h"
#include "calendarvisibility.h"
#include "calendartimeutils.h"
#include "revisionutils.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMultiHash>
#include <QStandardPaths>
#include <QStringList>
#include <QTime>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace
{
constexpr auto BusPath = "/org/kde/ThunderbirdCalendar";
constexpr auto BusInterface = "org.kde.ThunderbirdCalendar";
constexpr auto ConfigGroupName = "ThunderbirdCalendarPlugin";

QString cacheDirectoryPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/plasma-thunderbird-calendar");
}

QString snapshotPath()
{
    return cacheDirectoryPath() + QStringLiteral("/events.json");
}

QString journalPath()
{
    return cacheDirectoryPath() + QStringLiteral("/events.journal");
}

QString fileFingerprint(const QString &path)
{
    QFileInfo info(path);
    info.refresh();
    if (!info.exists()) {
        return QStringLiteral("missing");
    }
    return QStringLiteral("%1:%2")
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch());
}

QString cacheFingerprint()
{
    return fileFingerprint(snapshotPath()) + QLatin1Char('|') + fileFingerprint(journalPath());
}

QSet<QString> eventUids(const QMultiHash<QDate, CalendarEvents::EventData> &events)
{
    QSet<QString> result;
    result.reserve(events.size());
    for (auto it = events.cbegin(); it != events.cend(); ++it) {
        result.insert(it.value().uid());
    }
    return result;
}

QSet<QString> staleCalendarIds(const QJsonArray &calendars)
{
    QSet<QString> result;
    for (const QJsonValue &value : calendars) {
        const QJsonObject calendar = value.toObject();
        const QString id = calendar.value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && isStaleCalendar(calendar)) {
            result.insert(id);
        }
    }
    return result;
}

bool showStaleEventsFromConfig()
{
    const KConfigGroup group = KSharedConfig::openConfig()->group(QString::fromLatin1(ConfigGroupName));
    return group.readEntry(QStringLiteral("showStaleEvents"), true);
}

constexpr qint64 MaxJournalBytes = 16 * 1024 * 1024;
constexpr int MaxEventsPerCalendar = 100000;

QByteArray contentDigest(QJsonObject snapshot)
{
    return ThunderbirdCalendar::snapshotContentDigest(std::move(snapshot)).toHex();
}

QByteArray journalRecordChecksum(QJsonObject record)
{
    record.remove(QStringLiteral("checksum"));
    return QCryptographicHash::hash(
        QJsonDocument(record).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
}

bool applyJournalDeltas(QJsonObject *snapshot, const QJsonArray &deltas, QString *error)
{
    QJsonArray calendars = snapshot->value(QStringLiteral("calendars")).toArray();
    QHash<QString, int> calendarIndexes;
    for (qsizetype index = 0; index < calendars.size(); ++index) {
        const QString id = calendars.at(index).toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            calendarIndexes.insert(id, static_cast<int>(index));
        }
    }

    QHash<QString, QHash<QString, QJsonObject>> grouped;
    for (const QJsonValue &value : deltas) {
        if (!value.isObject()) {
            *error = QStringLiteral("journal delta is not an object");
            return false;
        }
        const QJsonObject delta = value.toObject();
        const QString operation = delta.value(QStringLiteral("operation")).toString();
        const QString calendarId = delta.value(QStringLiteral("calendarId")).toString();
        const QJsonValue eventValue = delta.value(QStringLiteral("event"));
        if (!eventValue.isObject()) {
            *error = QStringLiteral("journal delta has no event object");
            return false;
        }
        const QJsonObject event = eventValue.toObject();
        const QString uid = event.value(QStringLiteral("uid")).toString();
        if ((operation != QStringLiteral("add") && operation != QStringLiteral("modify")
             && operation != QStringLiteral("remove"))
            || calendarId.isEmpty() || uid.isEmpty() || !calendarIndexes.contains(calendarId)) {
            *error = QStringLiteral("journal delta has an invalid operation, UID or calendar ID");
            return false;
        }
        grouped[calendarId].insert(uid, delta);
    }

    for (auto group = grouped.cbegin(); group != grouped.cend(); ++group) {
        const int calendarIndex = calendarIndexes.value(group.key());
        QJsonObject calendar = calendars.at(calendarIndex).toObject();
        const QJsonArray oldEvents = calendar.value(QStringLiteral("events")).toArray();
        QHash<QString, QJsonObject> eventMap;
        QStringList order;
        eventMap.reserve(oldEvents.size() + group.value().size());
        order.reserve(oldEvents.size() + group.value().size());
        for (const QJsonValue &eventValue : oldEvents) {
            const QJsonObject event = eventValue.toObject();
            const QString uid = event.value(QStringLiteral("uid")).toString();
            if (!uid.isEmpty() && !eventMap.contains(uid)) {
                eventMap.insert(uid, event);
                order.append(uid);
            }
        }

        QStringList orderedUids = group.value().keys();
        std::sort(orderedUids.begin(), orderedUids.end());
        for (const QString &uid : std::as_const(orderedUids)) {
            const QJsonObject delta = group.value().value(uid);
            const QString operation = delta.value(QStringLiteral("operation")).toString();
            const QJsonObject event = delta.value(QStringLiteral("event")).toObject();
            const auto existing = eventMap.constFind(uid);
            if (operation == QStringLiteral("remove")) {
                if (existing == eventMap.cend()) {
                    *error = QStringLiteral("journal removes missing event '%1'").arg(uid);
                    return false;
                }
                eventMap.remove(uid);
            } else if (operation == QStringLiteral("add")) {
                if (existing != eventMap.cend()) {
                    *error = QStringLiteral("journal adds existing event '%1'").arg(uid);
                    return false;
                }
                eventMap.insert(uid, event);
                order.append(uid);
            } else {
                if (existing == eventMap.cend()) {
                    *error = QStringLiteral("journal modifies missing event '%1'").arg(uid);
                    return false;
                }
                const QJsonValue previous = delta.value(QStringLiteral("previous"));
                if (previous.isObject() && existing.value() != previous.toObject()) {
                    *error = QStringLiteral("journal modification has an unexpected previous value for '%1'").arg(uid);
                    return false;
                }
                eventMap.insert(uid, event);
            }
        }

        QJsonArray updatedEvents;
        for (const QString &uid : std::as_const(order)) {
            const auto event = eventMap.constFind(uid);
            if (event != eventMap.cend()) {
                updatedEvents.append(event.value());
            }
        }
        if (updatedEvents.size() > MaxEventsPerCalendar) {
            *error = QStringLiteral("journal exceeds the event limit for calendar '%1'").arg(group.key());
            return false;
        }
        calendar.insert(QStringLiteral("events"), updatedEvents);
        calendars.replace(calendarIndex, calendar);
    }

    snapshot->insert(QStringLiteral("calendars"), calendars);
    return true;
}

bool replayJournal(QJsonObject *snapshot, QString *error)
{
    const QString path = journalPath();
    if (!QFile::exists(path)) {
        return true;
    }
    QFile journal(path);
    if (!journal.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("could not open event journal: %1").arg(journal.errorString());
        return false;
    }
    if (journal.size() > MaxJournalBytes) {
        *error = QStringLiteral("event journal exceeds the %1 MiB safety limit")
            .arg(MaxJournalBytes / (1024 * 1024));
        return false;
    }
    const QByteArray contents = journal.readAll();
    const qsizetype headerEnd = contents.indexOf('\n');
    if (headerEnd < 0) {
        *error = QStringLiteral("event journal has no complete header");
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument headerDocument = QJsonDocument::fromJson(contents.left(headerEnd), &parseError);
    if (parseError.error != QJsonParseError::NoError || !headerDocument.isObject()) {
        *error = QStringLiteral("event journal header is invalid");
        return false;
    }
    const QJsonObject header = headerDocument.object();
    const int headerSchemaVersion = header.value(QStringLiteral("schemaVersion")).toInt(-1);
    if (header.value(QStringLiteral("type")).toString() != QStringLiteral("header")
        || (headerSchemaVersion != 1
            && headerSchemaVersion != ThunderbirdCalendar::DeltaJournalSchemaVersion)
        || !header.value(QStringLiteral("baseRevision")).isString()) {
        *error = QStringLiteral("event journal header has an unsupported schema");
        return false;
    }

    const QString storedBaseRevision = snapshot->value(QStringLiteral("contentRevision")).toString();
    const QString baseContentDigest = QString::fromLatin1(contentDigest(*snapshot));
    const bool chainSnapshot = ThunderbirdCalendar::usesDeltaRevisionChain(*snapshot);
    QString baseRevision = storedBaseRevision;
    if (chainSnapshot) {
        if (storedBaseRevision.isEmpty()
            || !ThunderbirdCalendar::snapshotRevisionCheckpointMatches(*snapshot, baseContentDigest)) {
            *error = QStringLiteral("base snapshot content or revision checkpoint is invalid");
            return false;
        }
    } else {
        if (!storedBaseRevision.isEmpty() && storedBaseRevision != baseContentDigest) {
            *error = QStringLiteral("base snapshot does not match its stored content revision");
            return false;
        }
        baseRevision = baseContentDigest;
        snapshot->insert(QStringLiteral("contentRevision"), baseRevision);
    }
    if (header.value(QStringLiteral("baseRevision")).toString() != baseRevision
        || (chainSnapshot && headerSchemaVersion != ThunderbirdCalendar::DeltaJournalSchemaVersion)) {
        // A full snapshot is committed before the journal header is reset.
        // If a crash occurs between those operations, the newer snapshot wins.
        return true;
    }

    if (headerSchemaVersion == ThunderbirdCalendar::DeltaJournalSchemaVersion && !chainSnapshot) {
        // A legacy snapshot may have an empty v2 journal header if the host was
        // upgraded but no direct update has yet triggered snapshot migration.
        snapshot->insert(QStringLiteral("revisionAlgorithm"),
                         QLatin1String(ThunderbirdCalendar::DeltaRevisionAlgorithm));
        snapshot->insert(QStringLiteral("snapshotDigest"), baseContentDigest);
        snapshot->insert(QStringLiteral("revisionCheckpoint"),
                         ThunderbirdCalendar::snapshotRevisionCheckpoint(baseContentDigest, baseRevision));
    }
    snapshot->insert(QStringLiteral("contentRevision"), baseRevision);
    QString currentRevision = baseRevision;
    qsizetype offset = headerEnd + 1;
    while (offset < contents.size()) {
        const qsizetype lineEnd = contents.indexOf('\n', offset);
        if (lineEnd < 0) {
            // Ignore only an incomplete trailing append. The native host will
            // truncate it when it next opens the journal for recovery.
            break;
        }
        const QByteArray line = contents.mid(offset, lineEnd - offset);
        const QJsonDocument recordDocument = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !recordDocument.isObject()) {
            *error = QStringLiteral("event journal contains an invalid complete record");
            return false;
        }
        const QJsonObject record = recordDocument.object();
        const QString previousRevision = record.value(QStringLiteral("previousRevision")).toString();
        const QString revision = record.value(QStringLiteral("revision")).toString();
        const QJsonValue deltas = record.value(QStringLiteral("deltas"));
        if (record.value(QStringLiteral("type")).toString() != QStringLiteral("delta")
            || record.value(QStringLiteral("schemaVersion")).toInt(-1) != headerSchemaVersion
            || previousRevision != currentRevision || revision.isEmpty() || !deltas.isArray()
            || record.value(QStringLiteral("checksum")).toString().toLatin1() != journalRecordChecksum(record)) {
            *error = QStringLiteral("event journal revision chain or checksum is invalid");
            return false;
        }
        if (headerSchemaVersion == ThunderbirdCalendar::DeltaJournalSchemaVersion
            && revision != ThunderbirdCalendar::nextDeltaRevision(currentRevision, deltas.toArray())) {
            *error = QStringLiteral("event journal delta revision is invalid");
            return false;
        }
        QString applyError;
        if (!applyJournalDeltas(snapshot, deltas.toArray(), &applyError)) {
            *error = QStringLiteral("could not replay event journal: %1").arg(applyError);
            return false;
        }
        const QString generatedAt = record.value(QStringLiteral("generatedAt")).toString();
        if (!generatedAt.isEmpty()) {
            snapshot->insert(QStringLiteral("generatedAt"), generatedAt);
        }
        snapshot->insert(QStringLiteral("contentRevision"), revision);
        currentRevision = revision;
        offset = lineEnd + 1;
    }

    if (headerSchemaVersion == 1) {
        const QString actualRevision = QString::fromLatin1(contentDigest(*snapshot));
        if (actualRevision != currentRevision) {
            *error = QStringLiteral("replayed event journal does not match its final content revision");
            return false;
        }
    }
    return true;
}

// iCalendar DTEND is exclusive for DATE values. A timed event ending exactly
// at midnight likewise does not occupy the following calendar day.
QDate lastDayOf(const QDateTime &start, const QDateTime &end, bool allDay)
{
    QDate last = end.date();
    if (end > start && (allDay || end.time() == QTime(0, 0))) {
        last = last.addDays(-1);
    }
    return last < start.date() ? start.date() : last;
}

bool eventOverlapsRange(const QDateTime &start, const QDateTime &end, bool allDay,
                        const QDate &rangeStart, const QDate &rangeEnd)
{
    if (!start.isValid() || !end.isValid() || !rangeStart.isValid() || !rangeEnd.isValid()) {
        return false;
    }
    const QDate first = start.date();
    const QDate last = lastDayOf(start, end, allDay);
    return last >= rangeStart && first <= rangeEnd;
}

// Plasma may process eventRemoved() asynchronously/batched. Give every
// displayed start-date incarnation its own UID, so a queued removal for the
// previous day can never remove the newly inserted event after it moves.
QString plasmaEventUid(const QString &sourceUid, const QDate &startDay)
{
    return QStringLiteral("thunderbird-calendar:")
        + QString::number(sourceUid.size())
        + QLatin1Char(':') + sourceUid
        + QLatin1Char(':') + startDay.toString(Qt::ISODate);
}

bool toEventData(const QJsonObject &source, const QString &calendarColor,
                 CalendarEvents::EventData *event, QDate *startDay = nullptr)
{
    const bool allDay = source.value(QStringLiteral("allDay")).toBool(false);
    const QDateTime start = ThunderbirdCalendar::parseDateTime(
        source.value(QStringLiteral("start")).toString(), allDay,
        source.value(QStringLiteral("startFloating")).toBool(false));
    const QDateTime end = ThunderbirdCalendar::parseDateTime(
        source.value(QStringLiteral("end")).toString(), allDay,
        source.value(QStringLiteral("endFloating")).toBool(false));
    if (!start.isValid() || !end.isValid()) {
        return false;
    }

    const QDate firstDay = start.date();
    const QDate lastDay = lastDayOf(start, end, allDay);

    const QString sourceUid = source.value(QStringLiteral("uid")).toString();
    if (sourceUid.isEmpty()) {
        return false;
    }
    event->setUid(plasmaEventUid(sourceUid, firstDay));
    event->setTitle(source.value(QStringLiteral("title")).toString());
    event->setDescription(source.value(QStringLiteral("description")).toString());
    event->setStartDateTime(start);
    event->setEndDateTime(end);
    // Plasma's EventData documentation says isAllDay is for single-day events.
    // Multi-day events keep the default false value and are entered only once
    // under their actual start date (see eventsForCalendars()).
    event->setIsAllDay(allDay && firstDay == lastDay);
    event->setEventColor(source.value(QStringLiteral("color")).toString(calendarColor));
    const QString type = source.value(QStringLiteral("kind")).toString();
    event->setEventType(type == QStringLiteral("todo")
                            ? CalendarEvents::EventData::Todo
                            : CalendarEvents::EventData::Event);
    if (startDay) {
        *startDay = firstDay;
    }
    return true;
}

QSet<QString> selectionFromConfig(const QJsonArray &calendars, bool *hasExplicitSelection)
{
    const KConfigGroup group = KSharedConfig::openConfig()->group(QString::fromLatin1(ConfigGroupName));
    *hasExplicitSelection = group.hasKey(QStringLiteral("calendars"));
    QSet<QString> selected;
    if (*hasExplicitSelection) {
        const QStringList configured = group.readEntry(QStringLiteral("calendars"), QStringList());
        for (const QString &id : configured) {
            selected.insert(id);
        }
    } else {
        for (const QJsonValue &value : calendars) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                selected.insert(id);
            }
        }
    }
    return selected;
}

QJsonObject readSnapshot(QString *error = nullptr)
{
    QFile file(snapshotPath());
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return {};
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = parseError.errorString();
        }
        return {};
    }
    QJsonObject snapshot = document.object();
    QJsonObject replayedSnapshot = snapshot;
    QString journalError;
    if (replayJournal(&replayedSnapshot, &journalError)) {
        snapshot = replayedSnapshot;
    } else {
        if (error) {
            *error = journalError;
        }
        qWarning() << "Thunderbird Calendar [Plasma]: refusing to use a snapshot with an invalid event journal:" << journalError;
        return {};
    }
    return snapshot;
}

QString revisionForSnapshot(QJsonObject snapshot)
{
    const QString revision = snapshot.value(QStringLiteral("contentRevision")).toString();
    if (!revision.isEmpty()) {
        return revision;
    }
    return ThunderbirdCalendar::snapshotContentRevision(snapshot);
}

QMultiHash<QDate, CalendarEvents::EventData> eventsForCalendars(
    const QJsonArray &calendars,
    const QSet<QString> &selected,
    const QDate &rangeStart,
    const QDate &rangeEnd,
    const QSet<QString> *onlyCalendarIds = nullptr,
    bool showStale = true)
{
    QMultiHash<QDate, CalendarEvents::EventData> result;
    for (const QJsonValue &calendarValue : calendars) {
        const QJsonObject calendar = calendarValue.toObject();
        const QString calendarId = calendar.value(QStringLiteral("id")).toString();
        if (!selected.contains(calendarId)
            || (onlyCalendarIds && !onlyCalendarIds->contains(calendarId))
            || !shouldShowCalendarEvents(calendar, showStale)) {
            continue;
        }
        const QString calendarColor = calendar.value(QStringLiteral("color")).toString();
        const QJsonArray events = calendar.value(QStringLiteral("events")).toArray();
        for (const QJsonValue &eventValue : events) {
            CalendarEvents::EventData event;
            QDate startDay;
            const QJsonObject source = eventValue.toObject();
            if (!toEventData(source, calendarColor, &event, &startDay)
                || !eventOverlapsRange(event.startDateTime(), event.endDateTime(),
                                       source.value(QStringLiteral("allDay")).toBool(false),
                                       rangeStart, rangeEnd)) {
                continue;
            }
            // CalendarEvents::CalendarEventsPlugin explicitly requires a
            // multi-day event to be inserted once, keyed by its start date.
            result.insert(startDay, event);
        }
    }
    return result;
}

}

ThunderbirdCalendarEventsPlugin::ThunderbirdCalendarEventsPlugin(QObject *parent)
    : CalendarEvents::CalendarEventsPlugin(parent)
{
    auto bus = QDBusConnection::sessionBus();
    bus.connect(QString(), QString::fromLatin1(BusPath), QString::fromLatin1(BusInterface),
                QStringLiteral("EventsChanged"), this, SLOT(onEventsChanged()));
    bus.connect(QString(), QString::fromLatin1(BusPath), QString::fromLatin1(BusInterface),
                QStringLiteral("EventsDelta"), this, SLOT(onEventsDelta(QString)));
    bus.connect(QString(), QString::fromLatin1(BusPath), QString::fromLatin1(BusInterface),
                QStringLiteral("ConfigurationChanged"), this, SLOT(onConfigurationChanged()));

    m_cacheRetryClock.start();
    m_dateRangeRequestClock.start();

    // D-Bus signals are normally immediate, but a plugin can be starting,
    // reloading or briefly disconnected when a delta is emitted. Poll only
    // cheap file metadata; read/reconcile the snapshot and journal only when
    // their size or mtime changes.
    m_cachePollTimer.setInterval(2000);
    connect(&m_cachePollTimer, &QTimer::timeout, this, &ThunderbirdCalendarEventsPlugin::onCachePoll);
    m_cachePollTimer.start();
    qInfo() << "Thunderbird Calendar [Plasma]: provider initialised; cache polling interval"
            << m_cachePollTimer.interval() << "ms";
}

void ThunderbirdCalendarEventsPlugin::loadEventsForDateRange(const QDate &startDate, const QDate &endDate)
{
    const quint64 requestNumber = ++m_dateRangeRequestCount;
    const qint64 nowElapsedMs = m_dateRangeRequestClock.elapsed();
    const qint64 msSincePreviousRequest = m_hasPreviousDateRangeRequest
        ? nowElapsedMs - m_lastDateRangeRequestElapsedMs
        : -1;
    const bool sameRangeAsPrevious = m_hasPreviousDateRangeRequest
        && startDate == m_lastRequestedStartDate
        && endDate == m_lastRequestedEndDate;

    if (sameRangeAsPrevious) {
        ++m_consecutiveRepeatedDateRangeRequests;
    } else {
        m_consecutiveRepeatedDateRangeRequests = 0;
    }

    qInfo() << "Thunderbird Calendar [Plasma]: date-range request"
            << requestNumber
            << "start" << startDate
            << "end" << endDate
            << "has previous request" << m_hasPreviousDateRangeRequest
            << "previous start" << m_lastRequestedStartDate
            << "previous end" << m_lastRequestedEndDate
            << "same range as previous" << sameRangeAsPrevious
            << "consecutive repeated-range requests" << m_consecutiveRepeatedDateRangeRequests
            << "ms since previous request" << msSincePreviousRequest
            << "previous loaded revision" << m_loadedRevision
            << "previous published events" << m_publishedEventUids.size();

    m_lastRequestedStartDate = startDate;
    m_lastRequestedEndDate = endDate;
    m_lastDateRangeRequestElapsedMs = nowElapsedMs;
    m_hasPreviousDateRangeRequest = true;
    m_startDate = startDate;
    m_endDate = endDate;
    publishCurrentRange(QStringLiteral("date-range request #%1").arg(requestNumber));
}

bool ThunderbirdCalendarEventsPlugin::isCalendarSelected(const QString &calendarId) const
{
    const bool selected = !m_hasExplicitSelection || m_selectedCalendarIds.contains(calendarId);
    return selected && (m_showStaleEvents || !m_staleCalendarIds.contains(calendarId));
}

void ThunderbirdCalendarEventsPlugin::emitEventRemoved(const QString &uid, const QString &source,
                                                        quint64 &operationCount,
                                                        SignalEmissionTimings &timings,
                                                        quint64 expectedCount)
{
    ++operationCount;

    // Log before emitting: the receiving Plasma slot may be the code that is
    // slow or stuck. Progress messages therefore remain useful even if a
    // large removal batch never reaches its completion summary.
    if (operationCount == 1 || operationCount % 50 == 0
        || (expectedCount > 0 && operationCount == expectedCount)) {
        if (expectedCount > 0) {
            qInfo() << "Thunderbird Calendar [Plasma]: emitting eventRemoved" << operationCount
                    << "of" << expectedCount << "from" << source;
        } else {
            qInfo() << "Thunderbird Calendar [Plasma]: emitted eventRemoved progress" << operationCount
                    << "from" << source;
        }
    }

    QElapsedTimer timer;
    timer.start();
    Q_EMIT eventRemoved(uid);
    const qint64 elapsedNs = timer.nsecsElapsed();
    ++timings.eventRemovedCount;
    timings.eventRemovedTotalNs += elapsedNs;
    if (elapsedNs > timings.eventRemovedMaxNs) {
        timings.eventRemovedMaxNs = elapsedNs;
        timings.eventRemovedMaxIndex = operationCount;
    }
}

void ThunderbirdCalendarEventsPlugin::emitEventModified(
    const CalendarEvents::EventData &event, SignalEmissionTimings &timings)
{
    ++timings.eventModifiedStartedCount;
    if (timings.eventModifiedStartedCount == 1 || timings.eventModifiedStartedCount % 50 == 0) {
        qInfo() << "Thunderbird Calendar [Plasma]: emitting eventModified"
                << timings.eventModifiedStartedCount;
    }
    QElapsedTimer timer;
    timer.start();
    Q_EMIT eventModified(event);
    const qint64 elapsedNs = timer.nsecsElapsed();
    ++timings.eventModifiedCount;
    timings.eventModifiedTotalNs += elapsedNs;
    if (elapsedNs > timings.eventModifiedMaxNs) {
        timings.eventModifiedMaxNs = elapsedNs;
        timings.eventModifiedMaxIndex = timings.eventModifiedCount;
    }
}

void ThunderbirdCalendarEventsPlugin::emitDataReady(
    const QMultiHash<QDate, CalendarEvents::EventData> &events,
    SignalEmissionTimings &timings)
{
    ++timings.dataReadyStartedCalls;
    qInfo() << "Thunderbird Calendar [Plasma]: emitting dataReady call"
            << timings.dataReadyStartedCalls << "with" << events.size() << "events";
    QElapsedTimer timer;
    timer.start();
    Q_EMIT dataReady(events);
    const qint64 elapsedNs = timer.nsecsElapsed();
    ++timings.dataReadyCalls;
    timings.dataReadyEvents += static_cast<quint64>(events.size());
    timings.dataReadyTotalNs += elapsedNs;
    if (elapsedNs > timings.dataReadyMaxNs) {
        timings.dataReadyMaxNs = elapsedNs;
        timings.dataReadyMaxCallIndex = timings.dataReadyCalls;
    }
}

void ThunderbirdCalendarEventsPlugin::logSignalEmissionTimings(
    const QString &source, const SignalEmissionTimings &timings) const
{
    constexpr double NanosecondsPerMillisecond = 1000000.0;
    qInfo() << "Thunderbird Calendar [Plasma]: signal delivery timings for" << source
            << "eventRemoved count" << timings.eventRemovedCount
            << "total ms" << (static_cast<double>(timings.eventRemovedTotalNs) / NanosecondsPerMillisecond)
            << "average ms" << (timings.eventRemovedCount
                                    ? static_cast<double>(timings.eventRemovedTotalNs)
                                        / NanosecondsPerMillisecond / timings.eventRemovedCount
                                    : 0.0)
            << "max ms" << (static_cast<double>(timings.eventRemovedMaxNs) / NanosecondsPerMillisecond)
            << "max at removal" << timings.eventRemovedMaxIndex
            << "eventModified count" << timings.eventModifiedCount
            << "total ms" << (static_cast<double>(timings.eventModifiedTotalNs) / NanosecondsPerMillisecond)
            << "average ms" << (timings.eventModifiedCount
                                    ? static_cast<double>(timings.eventModifiedTotalNs)
                                        / NanosecondsPerMillisecond / timings.eventModifiedCount
                                    : 0.0)
            << "max ms" << (static_cast<double>(timings.eventModifiedMaxNs) / NanosecondsPerMillisecond)
            << "max at modification" << timings.eventModifiedMaxIndex
            << "dataReady calls" << timings.dataReadyCalls
            << "events" << timings.dataReadyEvents
            << "total ms" << (static_cast<double>(timings.dataReadyTotalNs) / NanosecondsPerMillisecond)
            << "average ms" << (timings.dataReadyCalls
                                    ? static_cast<double>(timings.dataReadyTotalNs)
                                        / NanosecondsPerMillisecond / timings.dataReadyCalls
                                    : 0.0)
            << "max ms" << (static_cast<double>(timings.dataReadyMaxNs) / NanosecondsPerMillisecond)
            << "max at dataReady call" << timings.dataReadyMaxCallIndex;
}

void ThunderbirdCalendarEventsPlugin::onEventsChanged()
{
    if (!m_startDate.isValid() || !m_endDate.isValid()) {
        return;
    }
    if (m_selectionInitialized && !m_loadedRevision.isEmpty()) {
        qInfo() << "Thunderbird Calendar [Plasma]: received full-snapshot notification; loaded revision"
                << m_loadedRevision;
        reconcileFromSnapshot(QStringLiteral("full-snapshot notification"));
    } else {
        publishCurrentRange(QStringLiteral("initial publication after full-snapshot notification"));
    }
}

void ThunderbirdCalendarEventsPlugin::onCachePoll()
{
    if (!m_startDate.isValid() || !m_endDate.isValid()) {
        return;
    }
    const QString fingerprint = cacheFingerprint();
    if (!m_cacheFingerprintTracker.needsReconciliation(fingerprint, m_cacheRetryClock.elapsed())) {
        return;
    }
    reconcileFromSnapshot(QStringLiteral("cache changed without a matching D-Bus delta"));
}

void ThunderbirdCalendarEventsPlugin::onConfigurationChanged()
{
    if (m_startDate.isValid() && m_endDate.isValid()) {
        applyCalendarSelectionChanges();
    }
}

void ThunderbirdCalendarEventsPlugin::publishCurrentRange(const QString &source)
{
    constexpr double NanosecondsPerMillisecond = 1000000.0;
    QElapsedTimer totalTimer;
    totalTimer.start();
    QElapsedTimer stageTimer;
    stageTimer.start();

    QString error;
    const QJsonObject snapshot = readSnapshot(&error);
    const qint64 snapshotReadNs = stageTimer.nsecsElapsed();
    if (snapshot.isEmpty() && !error.isEmpty()) {
        qWarning() << "Thunderbird Calendar [Plasma]: date-range publication for" << source
                   << "cannot read the calendar snapshot:" << error;
        // Keep the previous fingerprint. onCachePoll() must retry this failed
        // read even when the files do not change again.
        m_cacheFingerprintTracker.markFailed(cacheFingerprint(), m_cacheRetryClock.elapsed());
        return;
    }
    stageTimer.start();
    const QJsonArray calendars = snapshot.value(QStringLiteral("calendars")).toArray();
    m_selectedCalendarIds = selectionFromConfig(calendars, &m_hasExplicitSelection);
    m_staleCalendarIds = staleCalendarIds(calendars);
    m_showStaleEvents = showStaleEventsFromConfig();
    m_selectionInitialized = true;
    m_loadedRevision = snapshot.isEmpty() ? QString() : revisionForSnapshot(snapshot);
    const qint64 selectionAndRevisionNs = stageTimer.nsecsElapsed();
    m_pendingRevision.clear();
    m_pendingPreviousRevision.clear();
    m_receivedDeltaBatches.clear();
    m_pendingBatchCount = 0;
    m_pendingRevisionEventRemovedSignals = 0;
    m_pendingRevisionEventModifiedSignals = 0;
    m_pendingRevisionDataReadyEvents = 0;

    stageTimer.start();
    const auto events = eventsForCalendars(calendars, m_selectedCalendarIds, m_startDate, m_endDate,
                                           nullptr, m_showStaleEvents);
    const qint64 eventSelectionAndConversionNs = stageTimer.nsecsElapsed();

    stageTimer.start();
    m_publishedEventUids = eventUids(events);
    const qint64 publishedUidIndexNs = stageTimer.nsecsElapsed();
    m_cacheFingerprintTracker.markReconciled(cacheFingerprint());

    qInfo() << "Thunderbird Calendar [Plasma]: date-range publication preparation for" << source
            << "snapshot read ms" << (static_cast<double>(snapshotReadNs) / NanosecondsPerMillisecond)
            << "selection and revision setup ms" << (static_cast<double>(selectionAndRevisionNs) / NanosecondsPerMillisecond)
            << "event filtering and conversion ms" << (static_cast<double>(eventSelectionAndConversionNs) / NanosecondsPerMillisecond)
            << "published UID indexing ms" << (static_cast<double>(publishedUidIndexNs) / NanosecondsPerMillisecond)
            << "events" << events.size()
            << "revision" << m_loadedRevision;

    SignalEmissionTimings signalTimings;
    emitDataReady(events, signalTimings);
    logSignalEmissionTimings(source, signalTimings);

    qInfo() << "Thunderbird Calendar [Plasma]: date-range publication completion for" << source
            << "dataReady delivery ms" << (static_cast<double>(signalTimings.dataReadyTotalNs) / NanosecondsPerMillisecond)
            << "total ms" << (static_cast<double>(totalTimer.nsecsElapsed()) / NanosecondsPerMillisecond)
            << "events" << events.size()
            << "revision" << m_loadedRevision;

    qInfo() << "Thunderbird Calendar [Plasma]: published date range for" << source
            << m_startDate << "through" << m_endDate
            << "calendars" << calendars.size() << "stale calendars" << m_staleCalendarIds.size()
            << "show stale" << m_showStaleEvents << "events" << events.size()
            << "revision" << m_loadedRevision;
}

void ThunderbirdCalendarEventsPlugin::applyCalendarSelectionChanges()
{
    QString error;
    const QJsonObject snapshot = readSnapshot(&error);
    if (snapshot.isEmpty() || !snapshot.value(QStringLiteral("calendars")).isArray()) {
        // Do not discard the current selection/revision or cache fingerprint
        // when the cache is temporarily unavailable. The unchanged fingerprint
        // must be retried by the next poll rather than suppressing recovery.
        qWarning() << "Thunderbird Calendar [Plasma]: cannot reconcile calendar selection; snapshot is unavailable:" << error;
        m_cacheFingerprintTracker.markFailed(cacheFingerprint(), m_cacheRetryClock.elapsed());
        return;
    }

    const QJsonArray calendars = snapshot.value(QStringLiteral("calendars")).toArray();
    bool newHasExplicitSelection = false;
    const QSet<QString> newSelected = selectionFromConfig(calendars, &newHasExplicitSelection);
    const QSet<QString> newStaleIds = staleCalendarIds(calendars);
    const bool newShowStale = showStaleEventsFromConfig();
    const auto newEvents = eventsForCalendars(calendars, newSelected, m_startDate, m_endDate,
                                               nullptr, newShowStale);
    const QSet<QString> newUids = eventUids(newEvents);
    QMultiHash<QDate, CalendarEvents::EventData> additions;
    QList<CalendarEvents::EventData> existingEvents;

    quint64 expectedRemovalCount = 0;
    for (const QString &oldUid : std::as_const(m_publishedEventUids)) {
        if (!newUids.contains(oldUid)) {
            ++expectedRemovalCount;
        }
    }
    quint64 removalSignalCount = 0;
    SignalEmissionTimings signalTimings;

    for (const QString &oldUid : std::as_const(m_publishedEventUids)) {
        if (!newUids.contains(oldUid)) {
            emitEventRemoved(oldUid, QStringLiteral("calendar configuration change"),
                             removalSignalCount, signalTimings, expectedRemovalCount);
        }
    }
    for (auto it = newEvents.cbegin(); it != newEvents.cend(); ++it) {
        if (m_publishedEventUids.contains(it.value().uid())) {
            existingEvents.append(it.value());
        } else {
            additions.insert(it.key(), it.value());
        }
    }

    m_selectedCalendarIds = newSelected;
    m_staleCalendarIds = newStaleIds;
    m_showStaleEvents = newShowStale;
    m_hasExplicitSelection = newHasExplicitSelection;
    m_selectionInitialized = true;
    const QString newRevision = revisionForSnapshot(snapshot);
    const bool snapshotChanged = !m_loadedRevision.isEmpty() && newRevision != m_loadedRevision;
    m_loadedRevision = newRevision;
    m_publishedEventUids = newUids;
    m_pendingRevision.clear();
    m_pendingPreviousRevision.clear();
    m_receivedDeltaBatches.clear();
    m_pendingBatchCount = 0;
    m_pendingRevisionEventRemovedSignals = 0;
    m_pendingRevisionEventModifiedSignals = 0;
    m_pendingRevisionDataReadyEvents = 0;
    m_cacheFingerprintTracker.markReconciled(cacheFingerprint());

    qInfo() << "Thunderbird Calendar [Plasma]: applied calendar configuration; selected calendars"
            << m_selectedCalendarIds.size() << "stale calendars" << m_staleCalendarIds.size()
            << "show stale" << m_showStaleEvents << "visible events" << newUids.size();

    if (!additions.isEmpty()) {
        emitDataReady(additions, signalTimings);
        for (auto it = additions.cbegin(); it != additions.cend(); ++it) {
            emitEventModified(it.value(), signalTimings);
        }
    }
    if (snapshotChanged) {
        for (const auto &event : std::as_const(existingEvents)) {
            emitEventModified(event, signalTimings);
        }
    }
    logSignalEmissionTimings(QStringLiteral("calendar configuration change"), signalTimings);
    qInfo() << "Thunderbird Calendar [Plasma]: calendar-configuration signal counts:"
            << "eventRemoved" << signalTimings.eventRemovedCount
            << "dataReady events" << signalTimings.dataReadyEvents
            << "eventModified" << signalTimings.eventModifiedCount;
}

void ThunderbirdCalendarEventsPlugin::reconcileFromSnapshot(const QString &reason)
{
    QString error;
    const QJsonObject snapshot = readSnapshot(&error);
    if (snapshot.isEmpty() || !snapshot.value(QStringLiteral("calendars")).isArray()) {
        qWarning() << "Thunderbird Calendar [Plasma]: could not recover a missed event revision (" << reason << "): " << error;
        m_cacheFingerprintTracker.markFailed(cacheFingerprint(), m_cacheRetryClock.elapsed());
        return;
    }

    const QString newRevision = revisionForSnapshot(snapshot);
    if (newRevision.isEmpty()) {
        qWarning() << "Thunderbird Calendar [Plasma]: recovered snapshot has no content revision";
        m_cacheFingerprintTracker.markFailed(cacheFingerprint(), m_cacheRetryClock.elapsed());
        return;
    }

    const QJsonArray calendars = snapshot.value(QStringLiteral("calendars")).toArray();
    bool hasExplicitSelection = false;
    const QSet<QString> selected = selectionFromConfig(calendars, &hasExplicitSelection);
    const QSet<QString> newStaleIds = staleCalendarIds(calendars);
    const bool newShowStale = showStaleEventsFromConfig();
    const bool viewConfigurationChanged = !m_selectionInitialized
        || calendarViewConfigurationChanged(
            m_selectedCalendarIds, m_hasExplicitSelection, m_staleCalendarIds, m_showStaleEvents,
            selected, hasExplicitSelection, newStaleIds, newShowStale);
    if (newRevision == m_loadedRevision && !viewConfigurationChanged) {
        m_cacheFingerprintTracker.markReconciled(cacheFingerprint());
        return;
    }

    // A cache read may have failed while applying a calendar-selection or
    // stale-visibility change. The next polling retry can see the same content
    // revision, but must still rebuild the view when configuration differs.
    const auto currentEvents = eventsForCalendars(calendars, selected, m_startDate, m_endDate,
                                                   nullptr, newShowStale);
    const QSet<QString> currentUids = eventUids(currentEvents);
    QMultiHash<QDate, CalendarEvents::EventData> additions;
    QList<CalendarEvents::EventData> updates;
    const qsizetype previousPublishedEventCount = m_publishedEventUids.size();

    quint64 expectedRemovalCount = 0;
    for (const QString &oldUid : std::as_const(m_publishedEventUids)) {
        if (!currentUids.contains(oldUid)) {
            ++expectedRemovalCount;
        }
    }
    quint64 removalSignalCount = 0;
    SignalEmissionTimings signalTimings;

    // We retain only the UID index for the previously published view. Event
    // payloads are read from the authoritative snapshot/journal when recovery
    // is needed, avoiding a second full event collection in the plugin.
    for (const QString &oldUid : std::as_const(m_publishedEventUids)) {
        if (!currentUids.contains(oldUid)) {
            emitEventRemoved(oldUid, QStringLiteral("snapshot reconciliation: ") + reason,
                             removalSignalCount, signalTimings, expectedRemovalCount);
        }
    }
    for (auto it = currentEvents.cbegin(); it != currentEvents.cend(); ++it) {
        if (m_publishedEventUids.contains(it.value().uid())) {
            updates.append(it.value());
        } else {
            additions.insert(it.key(), it.value());
        }
    }

    m_selectedCalendarIds = selected;
    m_staleCalendarIds = newStaleIds;
    m_showStaleEvents = newShowStale;
    m_hasExplicitSelection = hasExplicitSelection;
    m_selectionInitialized = true;
    m_publishedEventUids = currentUids;
    m_loadedRevision = newRevision;
    m_pendingRevision.clear();
    m_pendingPreviousRevision.clear();
    m_receivedDeltaBatches.clear();
    m_pendingBatchCount = 0;
    m_pendingRevisionEventRemovedSignals = 0;
    m_pendingRevisionEventModifiedSignals = 0;
    m_pendingRevisionDataReadyEvents = 0;
    m_cacheFingerprintTracker.markReconciled(cacheFingerprint());

    if (!additions.isEmpty()) {
        emitDataReady(additions, signalTimings);
        // dataReady inserts the item; eventModified then invalidates the
        // agenda cache for the event's actual date in Plasma's DaysModel.
        for (auto it = additions.cbegin(); it != additions.cend(); ++it) {
            emitEventModified(it.value(), signalTimings);
        }
    }
    for (const auto &event : std::as_const(updates)) {
        emitEventModified(event, signalTimings);
    }
    logSignalEmissionTimings(QStringLiteral("snapshot reconciliation: ") + reason, signalTimings);
    qInfo() << "Thunderbird Calendar [Plasma]: reconciled missed updates from the recovery snapshot; reason"
            << reason << "revision" << newRevision << "calendars" << calendars.size()
            << "stale calendars" << m_staleCalendarIds.size() << "show stale" << m_showStaleEvents
            << "previous visible events" << previousPublishedEventCount
            << "visible events" << currentUids.size()
            << "signal counts: eventRemoved" << signalTimings.eventRemovedCount
            << "dataReady events" << signalTimings.dataReadyEvents
            << "eventModified" << signalTimings.eventModifiedCount;
}

void ThunderbirdCalendarEventsPlugin::onEventsDelta(const QString &json)
{
    if (!m_startDate.isValid() || !m_endDate.isValid()) {
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return;
    }
    const QJsonObject envelope = document.object();
    if (envelope.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        return;
    }
    const QString previousRevision = envelope.value(QStringLiteral("previousRevision")).toString();
    const QString revision = envelope.value(QStringLiteral("revision")).toString();
    const int batchIndex = envelope.value(QStringLiteral("batchIndex")).toInt(-1);
    const int batchCount = envelope.value(QStringLiteral("batchCount")).toInt(0);
    if (revision.isEmpty() || previousRevision.isEmpty()
        || batchIndex < 0 || batchCount < 1 || batchIndex >= batchCount) {
        return;
    }
    qInfo() << "Thunderbird Calendar [Plasma]: received event-delta batch" << batchIndex + 1 << "of" << batchCount
            << "deltas" << envelope.value(QStringLiteral("deltas")).toArray().size()
            << "from" << previousRevision << "to" << revision;
    // A range load can race with a D-Bus delta: if it read the already-updated
    // snapshot, its content revision matches and this delta must be ignored.
    if (revision == m_loadedRevision) {
        return;
    }
    if (m_pendingRevision.isEmpty()) {
        if (previousRevision != m_loadedRevision) {
            qWarning() << "Thunderbird Calendar [Plasma]: received an out-of-sequence event delta; reconciling from the durable journal. Expected base"
                       << m_loadedRevision << "received" << previousRevision;
            reconcileFromSnapshot(QStringLiteral("D-Bus revision gap"));
            return;
        }
        m_pendingPreviousRevision = previousRevision;
        m_pendingRevision = revision;
        m_pendingBatchCount = batchCount;
        m_receivedDeltaBatches.clear();
        m_pendingRevisionEventRemovedSignals = 0;
        m_pendingRevisionEventModifiedSignals = 0;
        m_pendingRevisionDataReadyEvents = 0;
    } else if (m_pendingPreviousRevision != previousRevision
               || m_pendingRevision != revision || m_pendingBatchCount != batchCount) {
        qWarning() << "Thunderbird Calendar [Plasma]: overlapping event-delta batch; reconciling from the durable journal";
        reconcileFromSnapshot(QStringLiteral("overlapping D-Bus delta batches"));
        return;
    }
    if (m_receivedDeltaBatches.contains(batchIndex)) {
        return;
    }

    const QJsonArray deltas = envelope.value(QStringLiteral("deltas")).toArray();
    QMultiHash<QDate, CalendarEvents::EventData> additions;
    quint64 batchRemovalProgress = 0;
    SignalEmissionTimings signalTimings;

    auto makeVisibleEvent = [this](const QJsonObject &source, CalendarEvents::EventData *event,
                                   QDate *startDay) {
        const QString calendarColor = source.value(QStringLiteral("color")).toString();
        if (!toEventData(source, calendarColor, event, startDay)) {
            return false;
        }
        return eventOverlapsRange(event->startDateTime(), event->endDateTime(),
                                  source.value(QStringLiteral("allDay")).toBool(false),
                                  m_startDate, m_endDate);
    };

    for (const QJsonValue &deltaValue : deltas) {
        const QJsonObject delta = deltaValue.toObject();
        const QString operation = delta.value(QStringLiteral("operation")).toString();
        const QString calendarId = delta.value(QStringLiteral("calendarId")).toString();
        const bool selected = isCalendarSelected(calendarId);

        const QJsonObject source = delta.value(QStringLiteral("event")).toObject();
        CalendarEvents::EventData event;
        QDate startDay;
        const bool newVisible = selected && makeVisibleEvent(source, &event, &startDay);

        if (operation == QStringLiteral("add")) {
            if (newVisible) {
                if (m_publishedEventUids.contains(event.uid())) {
                    // Treat a repeated add as an upsert. This makes a replayed
                    // or duplicated D-Bus batch idempotent in Plasma's model.
                    emitEventModified(event, signalTimings);
                } else {
                    m_publishedEventUids.insert(event.uid());
                    additions.insert(startDay, event);
                }
            }
            continue;
        }

        if (operation == QStringLiteral("remove")) {
            if (selected && newVisible && m_publishedEventUids.remove(event.uid()) > 0) {
                emitEventRemoved(event.uid(),
                                 QStringLiteral("event-delta batch %1/%2")
                                     .arg(batchIndex + 1).arg(batchCount),
                                 batchRemovalProgress, signalTimings);
            }
            continue;
        }

        if (operation != QStringLiteral("modify")) {
            continue;
        }

        const QJsonObject previousSource = delta.value(QStringLiteral("previous")).toObject();
        CalendarEvents::EventData previousEvent;
        const bool previousVisible = selected
            && makeVisibleEvent(previousSource, &previousEvent, nullptr);
        const bool previousWasPublished = previousVisible
            && m_publishedEventUids.contains(previousEvent.uid());

        if (previousVisible && newVisible) {
            if (previousEvent.uid() != event.uid()) {
                // A move changes the date-scoped Plasma UID. Remove the old
                // incarnation, then add the new one under its destination date.
                if (previousWasPublished) {
                    m_publishedEventUids.remove(previousEvent.uid());
                    emitEventRemoved(previousEvent.uid(),
                                     QStringLiteral("event-delta batch %1/%2")
                                         .arg(batchIndex + 1).arg(batchCount),
                                     batchRemovalProgress, signalTimings);
                }
                if (m_publishedEventUids.contains(event.uid())) {
                    emitEventModified(event, signalTimings);
                } else {
                    m_publishedEventUids.insert(event.uid());
                    additions.insert(startDay, event);
                }
            } else if (previousWasPublished) {
                emitEventModified(event, signalTimings);
            } else {
                m_publishedEventUids.insert(event.uid());
                additions.insert(startDay, event);
            }
        } else if (previousVisible) {
            if (previousWasPublished) {
                m_publishedEventUids.remove(previousEvent.uid());
                emitEventRemoved(previousEvent.uid(),
                                 QStringLiteral("event-delta batch %1/%2")
                                     .arg(batchIndex + 1).arg(batchCount),
                                 batchRemovalProgress, signalTimings);
            }
        } else if (newVisible) {
            if (m_publishedEventUids.contains(event.uid())) {
                emitEventModified(event, signalTimings);
            } else {
                m_publishedEventUids.insert(event.uid());
                additions.insert(startDay, event);
            }
        }
    }

    if (!additions.isEmpty()) {
        emitDataReady(additions, signalTimings);

        // DaysModel::onDataReady() inserts the events, but only marks its
        // cached agenda data dirty when the additions include today's date.
        // The agenda can currently be showing another date (for example, the
        // destination of a moved event), in which case its cached list would
        // otherwise remain stale until the month is changed. Once dataReady()
        // has synchronously inserted each event, eventModified() is a valid
        // follow-up notification: DaysModel finds the just-inserted UID,
        // invalidates the agenda cache, and emits agendaUpdated() for the
        // event's actual date. Our date-scoped UIDs also keep this safe when a
        // previous-date removal is queued by a newer Plasma implementation.
        for (auto it = additions.cbegin(); it != additions.cend(); ++it) {
            emitEventModified(it.value(), signalTimings);
        }
    }

    m_pendingRevisionEventRemovedSignals += signalTimings.eventRemovedCount;
    m_pendingRevisionEventModifiedSignals += signalTimings.eventModifiedCount;
    m_pendingRevisionDataReadyEvents += signalTimings.dataReadyEvents;
    logSignalEmissionTimings(QStringLiteral("event-delta batch %1/%2")
                                 .arg(batchIndex + 1).arg(batchCount),
                             signalTimings);
    qInfo() << "Thunderbird Calendar [Plasma]: event-delta signal counts for batch"
            << batchIndex + 1 << "of" << batchCount
            << "eventRemoved" << signalTimings.eventRemovedCount
            << "eventModified" << signalTimings.eventModifiedCount
            << "dataReady events" << signalTimings.dataReadyEvents
            << "revision totals so far: eventRemoved" << m_pendingRevisionEventRemovedSignals
            << "eventModified" << m_pendingRevisionEventModifiedSignals
            << "dataReady events" << m_pendingRevisionDataReadyEvents;

    m_receivedDeltaBatches.insert(batchIndex);
    if (m_receivedDeltaBatches.size() == m_pendingBatchCount) {
        m_loadedRevision = m_pendingRevision;
        m_cacheFingerprintTracker.markReconciled(cacheFingerprint());
        qInfo() << "Thunderbird Calendar [Plasma]: completed event-delta revision" << m_loadedRevision
                << "published events" << m_publishedEventUids.size()
                << "signal totals: eventRemoved" << m_pendingRevisionEventRemovedSignals
                << "eventModified" << m_pendingRevisionEventModifiedSignals
                << "dataReady events" << m_pendingRevisionDataReadyEvents;
        m_pendingRevision.clear();
        m_pendingPreviousRevision.clear();
        m_receivedDeltaBatches.clear();
        m_pendingBatchCount = 0;
        m_pendingRevisionEventRemovedSignals = 0;
        m_pendingRevisionEventModifiedSignals = 0;
        m_pendingRevisionDataReadyEvents = 0;
    }
}
