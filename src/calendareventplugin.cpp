/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "calendareventplugin.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMultiHash>
#include <QStandardPaths>
#include <QStringList>
#include <QTime>

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

constexpr qint64 MaxJournalBytes = 16 * 1024 * 1024;
constexpr int MaxEventsPerCalendar = 100000;

QByteArray contentDigest(QJsonObject snapshot)
{
    snapshot.remove(QStringLiteral("generatedAt"));
    snapshot.remove(QStringLiteral("rangeStart"));
    snapshot.remove(QStringLiteral("rangeEnd"));
    snapshot.remove(QStringLiteral("contentRevision"));
    return QCryptographicHash::hash(
        QJsonDocument(snapshot).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
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
    if (header.value(QStringLiteral("type")).toString() != QStringLiteral("header")
        || header.value(QStringLiteral("schemaVersion")).toInt(-1) != 1
        || !header.value(QStringLiteral("baseRevision")).isString()) {
        *error = QStringLiteral("event journal header has an unsupported schema");
        return false;
    }

    const QString baseRevision = QString::fromLatin1(contentDigest(*snapshot));
    const QString storedBaseRevision = snapshot->value(QStringLiteral("contentRevision")).toString();
    if (!storedBaseRevision.isEmpty() && storedBaseRevision != baseRevision) {
        *error = QStringLiteral("base snapshot does not match its stored content revision");
        return false;
    }
    if (header.value(QStringLiteral("baseRevision")).toString() != baseRevision) {
        // A full snapshot is committed before the journal header is reset.
        // If a crash occurs between those operations, the newer snapshot wins.
        return true;
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
            || record.value(QStringLiteral("schemaVersion")).toInt(-1) != 1
            || previousRevision != currentRevision || revision.isEmpty() || !deltas.isArray()
            || record.value(QStringLiteral("checksum")).toString().toLatin1() != journalRecordChecksum(record)) {
            *error = QStringLiteral("event journal revision chain or checksum is invalid");
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

    const QString actualRevision = QString::fromLatin1(contentDigest(*snapshot));
    if (actualRevision != currentRevision) {
        *error = QStringLiteral("replayed event journal does not match its final content revision");
        return false;
    }
    return true;
}

QDateTime parseDateTime(const QString &value, bool allDay)
{
    if (allDay) {
        const QDate date = QDate::fromString(value.left(10), Qt::ISODate);
        return date.isValid() ? date.startOfDay() : QDateTime();
    }
    QDateTime result = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!result.isValid()) {
        result = QDateTime::fromString(value, Qt::ISODate);
    }
    // Timed values arrive as UTC instants. Convert so that date() is the local
    // calendar day the clock should place the event on.
    return result.isValid() ? result.toLocalTime() : result;
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
    const QDateTime start = parseDateTime(source.value(QStringLiteral("start")).toString(), allDay);
    const QDateTime end = parseDateTime(source.value(QStringLiteral("end")).toString(), allDay);
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
        qWarning() << "Thunderbird Calendar: ignoring invalid event journal:" << journalError;
    }
    return snapshot;
}

QString revisionForSnapshot(QJsonObject snapshot)
{
    const QString revision = snapshot.value(QStringLiteral("contentRevision")).toString();
    if (!revision.isEmpty()) {
        return revision;
    }
    snapshot.remove(QStringLiteral("generatedAt"));
    snapshot.remove(QStringLiteral("rangeStart"));
    snapshot.remove(QStringLiteral("rangeEnd"));
    snapshot.remove(QStringLiteral("contentRevision"));
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(snapshot).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

QMultiHash<QDate, CalendarEvents::EventData> eventsForCalendars(
    const QJsonArray &calendars,
    const QSet<QString> &selected,
    const QDate &rangeStart,
    const QDate &rangeEnd,
    const QSet<QString> *onlyCalendarIds = nullptr)
{
    QMultiHash<QDate, CalendarEvents::EventData> result;
    for (const QJsonValue &calendarValue : calendars) {
        const QJsonObject calendar = calendarValue.toObject();
        const QString calendarId = calendar.value(QStringLiteral("id")).toString();
        if (!selected.contains(calendarId)
            || (onlyCalendarIds && !onlyCalendarIds->contains(calendarId))) {
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

QSet<QString> calendarIds(const QJsonArray &calendars)
{
    QSet<QString> result;
    for (const QJsonValue &value : calendars) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            result.insert(id);
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
}

void ThunderbirdCalendarEventsPlugin::loadEventsForDateRange(const QDate &startDate, const QDate &endDate)
{
    m_startDate = startDate;
    m_endDate = endDate;
    publishCurrentRange();
}

bool ThunderbirdCalendarEventsPlugin::isCalendarSelected(const QString &calendarId) const
{
    return !m_hasExplicitSelection || m_selectedCalendarIds.contains(calendarId);
}

void ThunderbirdCalendarEventsPlugin::onEventsChanged()
{
    // EventsChanged is reserved for the first snapshot (or recovery when no
    // readable snapshot existed). Normal updates use EventsDelta and never
    // republish the complete event collection.
    if (m_startDate.isValid() && m_endDate.isValid()) {
        publishCurrentRange();
    }
}

void ThunderbirdCalendarEventsPlugin::onConfigurationChanged()
{
    if (m_startDate.isValid() && m_endDate.isValid()) {
        applyCalendarSelectionChanges();
    }
}

void ThunderbirdCalendarEventsPlugin::publishCurrentRange()
{
    const QJsonObject snapshot = readSnapshot();
    const QJsonArray calendars = snapshot.value(QStringLiteral("calendars")).toArray();
    m_selectedCalendarIds = selectionFromConfig(calendars, &m_hasExplicitSelection);
    m_selectionInitialized = true;
    m_loadedRevision = snapshot.isEmpty() ? QString() : revisionForSnapshot(snapshot);
    m_pendingRevision.clear();
    m_pendingPreviousRevision.clear();
    m_receivedDeltaBatches.clear();
    m_pendingBatchCount = 0;
    Q_EMIT dataReady(eventsForCalendars(calendars, m_selectedCalendarIds, m_startDate, m_endDate));
}

void ThunderbirdCalendarEventsPlugin::applyCalendarSelectionChanges()
{
    const QJsonObject snapshot = readSnapshot();
    if (snapshot.isEmpty() || !snapshot.value(QStringLiteral("calendars")).isArray()) {
        // Do not discard the current selection/revision when the cache is
        // temporarily unavailable. A later configuration notification or
        // normal range load can retry once the host has restored the snapshot.
        qWarning() << "Thunderbird Calendar: cannot reconcile calendar selection; snapshot is unavailable";
        return;
    }
    const QJsonArray calendars = snapshot.value(QStringLiteral("calendars")).toArray();
    bool newHasExplicitSelection = false;
    const QSet<QString> newSelected = selectionFromConfig(calendars, &newHasExplicitSelection);

    // A settings change is rare. Reconcile the displayed range by removing the
    // old selected calendars' visible UIDs, then inserting the newly selected
    // view. This avoids calling dataReady() with a full collection on top of
    // Plasma's existing entries while retaining no per-event plugin cache.
    if (m_selectionInitialized) {
        const QSet<QString> previousSelected = m_hasExplicitSelection
            ? m_selectedCalendarIds : calendarIds(calendars);
        for (const QJsonValue &calendarValue : calendars) {
            const QJsonObject calendar = calendarValue.toObject();
            const QString calendarId = calendar.value(QStringLiteral("id")).toString();
            if (!previousSelected.contains(calendarId)) {
                continue;
            }
            for (const QJsonValue &eventValue : calendar.value(QStringLiteral("events")).toArray()) {
                const QJsonObject source = eventValue.toObject();
                CalendarEvents::EventData event;
                if (toEventData(source, calendar.value(QStringLiteral("color")).toString(), &event)
                    && eventOverlapsRange(event.startDateTime(), event.endDateTime(),
                                          source.value(QStringLiteral("allDay")).toBool(false),
                                          m_startDate, m_endDate)) {
                    Q_EMIT eventRemoved(event.uid());
                }
            }
        }
    }

    m_selectedCalendarIds = newSelected;
    m_hasExplicitSelection = newHasExplicitSelection;
    m_selectionInitialized = true;
    m_loadedRevision = snapshot.isEmpty() ? QString() : revisionForSnapshot(snapshot);
    m_pendingRevision.clear();
    m_pendingPreviousRevision.clear();
    m_receivedDeltaBatches.clear();
    m_pendingBatchCount = 0;

    Q_EMIT dataReady(eventsForCalendars(calendars, m_selectedCalendarIds, m_startDate, m_endDate));
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
    // A range load can race with a D-Bus delta: if it read the already-updated
    // snapshot, its content revision matches and this delta must be ignored.
    if (revision == m_loadedRevision) {
        return;
    }
    if (m_pendingRevision.isEmpty()) {
        if (previousRevision != m_loadedRevision) {
            qWarning() << "Thunderbird Calendar: ignoring out-of-sequence event delta; expected base"
                       << m_loadedRevision << "received" << previousRevision;
            return;
        }
        m_pendingPreviousRevision = previousRevision;
        m_pendingRevision = revision;
        m_pendingBatchCount = batchCount;
        m_receivedDeltaBatches.clear();
    } else if (m_pendingPreviousRevision != previousRevision
               || m_pendingRevision != revision || m_pendingBatchCount != batchCount) {
        qWarning() << "Thunderbird Calendar: ignoring overlapping event-delta batch";
        return;
    }
    if (m_receivedDeltaBatches.contains(batchIndex)) {
        return;
    }

    const QJsonArray deltas = envelope.value(QStringLiteral("deltas")).toArray();
    QMultiHash<QDate, CalendarEvents::EventData> additions;

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
                additions.insert(startDay, event);
            }
            continue;
        }

        if (operation == QStringLiteral("remove")) {
            if (selected && newVisible) {
                Q_EMIT eventRemoved(event.uid());
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

        if (previousVisible && newVisible) {
            if (previousEvent.uid() != event.uid()) {
                // The Plasma-facing UID includes the start date. A move therefore
                // becomes an identity replacement, not an in-place modification.
                // This is safe even when DaysModel queues removals: the queued old
                // UID cannot remove the new date's incarnation of the event.
                Q_EMIT eventRemoved(previousEvent.uid());
                additions.insert(startDay, event);
            } else {
                Q_EMIT eventModified(event);
            }
        } else if (previousVisible) {
            Q_EMIT eventRemoved(previousEvent.uid());
        } else if (newVisible) {
            additions.insert(startDay, event);
        }
    }

    if (!additions.isEmpty()) {
        Q_EMIT dataReady(additions);

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
            Q_EMIT eventModified(it.value());
        }
    }

    m_receivedDeltaBatches.insert(batchIndex);
    if (m_receivedDeltaBatches.size() == m_pendingBatchCount) {
        m_loadedRevision = m_pendingRevision;
        m_pendingRevision.clear();
        m_pendingPreviousRevision.clear();
        m_receivedDeltaBatches.clear();
        m_pendingBatchCount = 0;
    }
}
