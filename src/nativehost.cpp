/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal Mozilla native-messaging host for the Thunderbird / Plasma bridge.
 * It accepts full snapshots and item-level calendar changes only; it cannot
 * execute commands or choose file paths. On changes it persists the updated
 * recovery snapshot and forwards event-level deltas
 * over D-Bus, keeping the Plasma plugin free of a second full event collection.
 */
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDevice>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QStandardPaths>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <unistd.h>
#include <utility>

namespace
{
constexpr quint32 MaxMessageBytes = 32U * 1024U * 1024U;
constexpr int MaxCalendars = 512;
constexpr int MaxEventsPerCalendar = 100000;
constexpr int MaxDeltasPerSignal = 64;
constexpr auto BusPath = "/org/kde/ThunderbirdCalendar";
constexpr auto BusInterface = "org.kde.ThunderbirdCalendar";

QString cacheDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/plasma-thunderbird-calendar");
}

QString formatMiB(quint32 bytes)
{
    return QStringLiteral("%1 MiB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 2);
}

// Phase timings are returned only when the extension's debug setting is on.
// Use QElapsedTimer so measurements are monotonic and unaffected by clock changes.
struct HostTimings {
    double messageWaitMs = 0.0;
    double frameReadMs = 0.0;
    double jsonParseMs = 0.0;
    double validationMs = 0.0;
    double contentHashMs = 0.0;
    double cacheDirectoryMs = 0.0;
    double existingCacheReadMs = 0.0;
    double cachePreparationMs = 0.0;
    double snapshotSerializationMs = 0.0;
    double diskCommitMs = 0.0;
    double dbusNotifyMs = 0.0;
    double hostProcessingMs = 0.0;
    double eventDiffMs = 0.0;
    qint64 eventDeltaCount = 0;
    qint64 cacheWriteBytes = 0;
    QString revision;
    bool unchanged = false;

    QJsonObject toJson() const
    {
        return {
            {QStringLiteral("messageWaitMs"), messageWaitMs},
            {QStringLiteral("frameReadMs"), frameReadMs},
            {QStringLiteral("jsonParseMs"), jsonParseMs},
            {QStringLiteral("validationMs"), validationMs},
            {QStringLiteral("contentHashMs"), contentHashMs},
            {QStringLiteral("cacheDirectoryMs"), cacheDirectoryMs},
            {QStringLiteral("existingCacheReadMs"), existingCacheReadMs},
            {QStringLiteral("cachePreparationMs"), cachePreparationMs},
            {QStringLiteral("snapshotSerializationMs"), snapshotSerializationMs},
            {QStringLiteral("diskCommitMs"), diskCommitMs},
            {QStringLiteral("dbusNotifyMs"), dbusNotifyMs},
            {QStringLiteral("hostProcessingMs"), hostProcessingMs},
            {QStringLiteral("eventDiffMs"), eventDiffMs},
            {QStringLiteral("eventDeltaCount"), static_cast<double>(eventDeltaCount)},
            {QStringLiteral("cacheWriteBytes"), static_cast<double>(cacheWriteBytes)},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("unchanged"), unchanged},
        };
    }
};

bool validateSnapshot(const QJsonObject &snapshot, QString *why)
{
    if (snapshot.value(QStringLiteral("schemaVersion")).toInt(-1) != 1) {
        *why = QStringLiteral("unsupported snapshot schemaVersion");
        return false;
    }
    const QJsonValue calendarsValue = snapshot.value(QStringLiteral("calendars"));
    if (!calendarsValue.isArray() || calendarsValue.toArray().size() > MaxCalendars) {
        *why = QStringLiteral("invalid calendars array: maximum is %1").arg(MaxCalendars);
        return false;
    }
    for (const QJsonValue &calendarValue : calendarsValue.toArray()) {
        if (!calendarValue.isObject()) {
            *why = QStringLiteral("calendar entry is not an object");
            return false;
        }
        const QJsonObject calendar = calendarValue.toObject();
        if (!calendar.value(QStringLiteral("id")).isString()
            || !calendar.value(QStringLiteral("events")).isArray()) {
            *why = QStringLiteral("invalid calendar entry: id must be a string and events must be an array");
            return false;
        }
        const QJsonArray events = calendar.value(QStringLiteral("events")).toArray();
        const int eventCount = events.size();
        if (eventCount > MaxEventsPerCalendar) {
            *why = QStringLiteral("calendar '%1' contains %2 events; host limit is %3 per calendar")
                .arg(calendar.value(QStringLiteral("name")).toString(calendar.value(QStringLiteral("id")).toString()))
                .arg(eventCount)
                .arg(MaxEventsPerCalendar);
            return false;
        }
        for (const QJsonValue &eventValue : events) {
            if (!eventValue.isObject()
                || !eventValue.toObject().value(QStringLiteral("uid")).isString()
                || eventValue.toObject().value(QStringLiteral("uid")).toString().isEmpty()) {
                *why = QStringLiteral("calendar '%1' contains an event without a stable UID")
                    .arg(calendar.value(QStringLiteral("id")).toString());
                return false;
            }
        }
    }
    return true;
}

enum class ReadStatus { Complete, EndOfStream, Error };

// Use the file descriptor directly rather than std::cin. The latter may buffer
// bytes belonging to the next frame, which would make poll() on stdin miss data
// already held in the C++ stream buffer.
ReadStatus readExact(void *destination, std::size_t length)
{
    auto *bytes = static_cast<char *>(destination);
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t count = ::read(STDIN_FILENO, bytes + offset, length - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            return offset == 0 ? ReadStatus::EndOfStream : ReadStatus::Error;
        }
        if (errno == EINTR) {
            continue;
        }
        return ReadStatus::Error;
    }
    return ReadStatus::Complete;
}

bool writeAll(int descriptor, const char *data, std::size_t length)
{
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t count = ::write(descriptor, data + offset, length - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool waitForInput(double *waitMs)
{
    QElapsedTimer waitTimer;
    waitTimer.start();
    pollfd input{};
    input.fd = STDIN_FILENO;
    input.events = POLLIN;

    int result = 0;
    do {
        result = ::poll(&input, 1, -1);
    } while (result < 0 && errno == EINTR);

    *waitMs = waitTimer.nsecsElapsed() / 1000000.0;
    if (result < 0 || (input.revents & POLLNVAL) != 0) {
        return false;
    }
    // POLLHUP may accompany the final readable bytes. Let readExact() decide
    // whether this is a clean EOF or a truncated frame.
    return result > 0 && (input.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}

bool writeFrame(const QJsonObject &object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (static_cast<quint64>(payload.size()) > MaxMessageBytes) {
        return false;
    }
    const quint32 length = qToLittleEndian(static_cast<quint32>(payload.size()));
    std::array<char, sizeof(length)> header{};
    std::memcpy(header.data(), &length, sizeof(length));
    return writeAll(STDOUT_FILENO, header.data(), header.size())
        && writeAll(STDOUT_FILENO, payload.constData(), static_cast<std::size_t>(payload.size()));
}

void notifyPlasma()
{
    QDBusMessage signal = QDBusMessage::createSignal(
        QString::fromLatin1(BusPath), QString::fromLatin1(BusInterface), QStringLiteral("EventsChanged"));
    if (!QDBusConnection::sessionBus().send(signal)) {
        std::cerr << "Thunderbird Plasma Calendar: could not send initial EventsChanged signal\n";
    }
    QCoreApplication::processEvents();
}

void notifyEventDeltas(const QJsonArray &deltas, const QString &previousRevision, const QString &revision)
{
    // Emit an empty batch too when only non-event snapshot metadata changed.
    // It advances the plugin's revision so the next real delta has a valid base.
    const qsizetype batchCount = std::max<qsizetype>(1,
        (deltas.size() + MaxDeltasPerSignal - 1) / MaxDeltasPerSignal);
    const QDBusConnection bus = QDBusConnection::sessionBus();
    for (qsizetype batchIndex = 0; batchIndex < batchCount; ++batchIndex) {
        QJsonArray batch;
        const qsizetype offset = batchIndex * MaxDeltasPerSignal;
        const qsizetype end = std::min<qsizetype>(offset + MaxDeltasPerSignal, deltas.size());
        for (qsizetype index = offset; index < end; ++index) {
            batch.append(deltas.at(index));
        }
        const QJsonObject envelope{
            {QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("previousRevision"), previousRevision},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("batchIndex"), static_cast<int>(batchIndex)},
            {QStringLiteral("batchCount"), static_cast<int>(batchCount)},
            {QStringLiteral("deltas"), batch},
        };
        QDBusMessage signal = QDBusMessage::createSignal(
            QString::fromLatin1(BusPath), QString::fromLatin1(BusInterface), QStringLiteral("EventsDelta"));
        signal << QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
        if (!bus.send(signal)) {
            std::cerr << "Thunderbird Plasma Calendar: could not send EventsDelta D-Bus signal batch\n";
        }
    }
    QCoreApplication::processEvents();
}

// Digest of a snapshot's calendar content. generatedAt and the query range are
// regenerated on every refresh, so they are excluded; QJsonObject keeps its
// keys sorted, which makes the compact serialisation deterministic.
QByteArray contentDigest(QJsonObject snapshot)
{
    snapshot.remove(QStringLiteral("generatedAt"));
    snapshot.remove(QStringLiteral("rangeStart"));
    snapshot.remove(QStringLiteral("rangeEnd"));
    snapshot.remove(QStringLiteral("contentRevision"));
    return QCryptographicHash::hash(QJsonDocument(snapshot).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}

bool readSnapshotFile(const QString &path, QJsonObject *snapshot)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    *snapshot = document.object();
    return true;
}

struct IndexedEvent {
    QString calendarId;
    QJsonObject event;
};

QHash<QString, IndexedEvent> indexEvents(const QJsonObject &snapshot)
{
    QHash<QString, IndexedEvent> result;
    const QJsonArray calendars = snapshot.value(QStringLiteral("calendars")).toArray();
    for (const QJsonValue &calendarValue : calendars) {
        const QJsonObject calendar = calendarValue.toObject();
        const QString calendarId = calendar.value(QStringLiteral("id")).toString();
        for (const QJsonValue &eventValue : calendar.value(QStringLiteral("events")).toArray()) {
            const QJsonObject event = eventValue.toObject();
            const QString uid = event.value(QStringLiteral("uid")).toString();
            if (uid.isEmpty()) {
                continue;
            }
            // Include the calendar ID in the key even though the exporter also
            // namespaces UIDs, so malformed/migrated data cannot collide.
            const QString key = QString::number(calendarId.size()) + QLatin1Char(':')
                + calendarId + uid;
            result.insert(key, IndexedEvent{calendarId, event});
        }
    }
    return result;
}

QJsonArray eventDeltas(QHash<QString, IndexedEvent> &previous, const QJsonObject &nextSnapshot)
{
    // Keep only one UID index: consume old entries as their replacements are
    // encountered in the new snapshot. Leftovers are removals. This avoids
    // allocating a second full event index for the new snapshot.
    QJsonArray deltas;
    const QJsonArray calendars = nextSnapshot.value(QStringLiteral("calendars")).toArray();
    for (const QJsonValue &calendarValue : calendars) {
        const QJsonObject calendar = calendarValue.toObject();
        const QString calendarId = calendar.value(QStringLiteral("id")).toString();
        for (const QJsonValue &eventValue : calendar.value(QStringLiteral("events")).toArray()) {
            const QJsonObject event = eventValue.toObject();
            const QString uid = event.value(QStringLiteral("uid")).toString();
            if (uid.isEmpty()) {
                continue;
            }
            const QString key = QString::number(calendarId.size()) + QLatin1Char(':')
                + calendarId + uid;
            auto old = previous.find(key);
            if (old == previous.end()) {
                deltas.append(QJsonObject{
                    {QStringLiteral("operation"), QStringLiteral("add")},
                    {QStringLiteral("calendarId"), calendarId},
                    {QStringLiteral("event"), event},
                });
            } else {
                if (old->event != event) {
                    deltas.append(QJsonObject{
                        {QStringLiteral("operation"), QStringLiteral("modify")},
                        {QStringLiteral("calendarId"), calendarId},
                        {QStringLiteral("previous"), old->event},
                        {QStringLiteral("event"), event},
                    });
                }
                previous.erase(old);
            }
        }
    }

    for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
        deltas.append(QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("remove")},
            {QStringLiteral("calendarId"), it->calendarId},
            {QStringLiteral("event"), it->event},
        });
    }
    return deltas;
}

QByteArray lastWrittenDigest;
QJsonObject cachedSnapshot;
bool hasCachedSnapshot = false;

bool acceptSnapshot(const QJsonObject &message,
                    QString *error,
                    QString *errorCode,
                    HostTimings *timings)
{
    if (message.value(QStringLiteral("type")).toString() != QStringLiteral("snapshot")) {
        *errorCode = QStringLiteral("INVALID_MESSAGE_TYPE");
        *error = QStringLiteral("only snapshot messages are supported");
        return false;
    }
    const QJsonValue snapshotValue = message.value(QStringLiteral("snapshot"));
    if (!snapshotValue.isObject()) {
        *errorCode = QStringLiteral("INVALID_SNAPSHOT");
        *error = QStringLiteral("snapshot must be an object");
        return false;
    }
    const QJsonObject snapshot = snapshotValue.toObject();

    QElapsedTimer phase;
    phase.start();
    if (!validateSnapshot(snapshot, error)) {
        timings->validationMs = phase.nsecsElapsed() / 1000000.0;
        *errorCode = QStringLiteral("INVALID_SNAPSHOT");
        return false;
    }
    timings->validationMs = phase.nsecsElapsed() / 1000000.0;

    phase.restart();
    const QByteArray digest = contentDigest(snapshot);
    timings->contentHashMs = phase.nsecsElapsed() / 1000000.0;
    timings->revision = QString::fromLatin1(digest.toHex());

    phase.restart();
    const QString directory = cacheDirectory();
    if (!QDir().mkpath(directory)) {
        *errorCode = QStringLiteral("CACHE_DIRECTORY_ERROR");
        *error = QStringLiteral("could not create cache directory: %1").arg(directory);
        timings->cacheDirectoryMs = phase.nsecsElapsed() / 1000000.0;
        timings->cachePreparationMs = timings->cacheDirectoryMs;
        return false;
    }
    const auto privateDirectory = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!QFile::setPermissions(directory, privateDirectory)) {
        *errorCode = QStringLiteral("CACHE_PERMISSION_ERROR");
        *error = QStringLiteral("could not restrict cache directory permissions: %1").arg(directory);
        timings->cacheDirectoryMs = phase.nsecsElapsed() / 1000000.0;
        timings->cachePreparationMs = timings->cacheDirectoryMs;
        return false;
    }
    timings->cacheDirectoryMs = phase.nsecsElapsed() / 1000000.0;

    const QString cachePath = directory + QStringLiteral("/events.json");
    QJsonObject previousSnapshot;
    bool hasPreviousSnapshot = false;
    phase.restart();
    if (lastWrittenDigest.isEmpty()) {
        hasPreviousSnapshot = readSnapshotFile(cachePath, &previousSnapshot);
        if (hasPreviousSnapshot) {
            lastWrittenDigest = contentDigest(previousSnapshot);
        }
    }
    timings->existingCacheReadMs = phase.nsecsElapsed() / 1000000.0;
    timings->cachePreparationMs = timings->cacheDirectoryMs + timings->existingCacheReadMs;

    // Unchanged refreshes stay cheap: once the host knows the cache digest it
    // does not reparse the on-disk snapshot just to confirm the same content.
    if (digest == lastWrittenDigest && QFile::exists(cachePath)) {
        if (!hasCachedSnapshot && hasPreviousSnapshot) {
            cachedSnapshot = previousSnapshot;
            hasCachedSnapshot = true;
        }
        timings->unchanged = true;
        return true;
    }

    QHash<QString, IndexedEvent> previousEvents;
    QString previousRevision;
    if (QFile::exists(cachePath)) {
        if (!hasPreviousSnapshot) {
            phase.restart();
            hasPreviousSnapshot = readSnapshotFile(cachePath, &previousSnapshot);
            timings->existingCacheReadMs += phase.nsecsElapsed() / 1000000.0;
        }
        if (hasPreviousSnapshot) {
            previousRevision = previousSnapshot.value(QStringLiteral("contentRevision")).toString();
            if (previousRevision.isEmpty()) {
                previousRevision = QString::fromLatin1(contentDigest(previousSnapshot).toHex());
            }
            phase.restart();
            previousEvents = indexEvents(previousSnapshot);
            timings->eventDiffMs = phase.nsecsElapsed() / 1000000.0;
            previousSnapshot = {}; // Release the old JSON tree; keep only the UID index.
        }
    }
    const bool incrementalUpdate = hasPreviousSnapshot;
    QJsonArray deltas;
    if (incrementalUpdate) {
        phase.restart();
        deltas = eventDeltas(previousEvents, snapshot);
        timings->eventDiffMs += phase.nsecsElapsed() / 1000000.0;
        timings->eventDeltaCount = deltas.size();
    }

    QJsonObject persistedSnapshot = snapshot;
    const QString revision = QString::fromLatin1(digest.toHex());
    persistedSnapshot.insert(QStringLiteral("contentRevision"), revision);

    phase.restart();
    const QByteArray data = QJsonDocument(persistedSnapshot).toJson(QJsonDocument::Compact);
    timings->snapshotSerializationMs = phase.nsecsElapsed() / 1000000.0;
    timings->cacheWriteBytes = data.size();

    phase.restart();
    QSaveFile file(cachePath);
    if (!file.open(QIODevice::WriteOnly)) {
        *errorCode = QStringLiteral("CACHE_WRITE_ERROR");
        *error = QStringLiteral("could not open cache file: %1").arg(file.errorString());
        timings->diskCommitMs = phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    const auto privateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!file.setPermissions(privateFile)) {
        *errorCode = QStringLiteral("CACHE_PERMISSION_ERROR");
        *error = QStringLiteral("could not restrict cache file permissions: %1").arg(cachePath);
        file.cancelWriting();
        timings->diskCommitMs = phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        *errorCode = QStringLiteral("CACHE_WRITE_ERROR");
        *error = QStringLiteral("could not commit cache file: %1").arg(file.errorString());
        timings->diskCommitMs = phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    timings->diskCommitMs = phase.nsecsElapsed() / 1000000.0;
    lastWrittenDigest = digest;
    cachedSnapshot = persistedSnapshot;
    hasCachedSnapshot = true;

    phase.restart();
    if (incrementalUpdate) {
        notifyEventDeltas(deltas, previousRevision, revision);
    } else {
        // The cache is being created for the first time. The CalendarEvents
        // provider must load the full range once; later commits are incremental.
        notifyPlasma();
    }
    timings->dbusNotifyMs = phase.nsecsElapsed() / 1000000.0;
    return true;
}

bool acceptEventChanges(const QJsonObject &message,
                        QString *error,
                        QString *errorCode,
                        HostTimings *timings)
{
    if (message.value(QStringLiteral("type")).toString() != QStringLiteral("eventChanges")) {
        *errorCode = QStringLiteral("INVALID_MESSAGE_TYPE");
        *error = QStringLiteral("unsupported native-host message type");
        return false;
    }

    const QJsonValue changesValue = message.value(QStringLiteral("changes"));
    if (!changesValue.isArray() || changesValue.toArray().isEmpty()
        || changesValue.toArray().size() > 5000) {
        *errorCode = QStringLiteral("INVALID_EVENT_CHANGES");
        *error = QStringLiteral("changes must be a non-empty array containing at most 5000 entries");
        return false;
    }

    QElapsedTimer phase;
    phase.start();
    const QString directory = cacheDirectory();
    if (!QDir().mkpath(directory)) {
        *errorCode = QStringLiteral("CACHE_DIRECTORY_ERROR");
        *error = QStringLiteral("could not create cache directory: %1").arg(directory);
        timings->cacheDirectoryMs = phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    const auto privateDirectory = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!QFile::setPermissions(directory, privateDirectory)) {
        *errorCode = QStringLiteral("CACHE_PERMISSION_ERROR");
        *error = QStringLiteral("could not restrict cache directory permissions: %1").arg(directory);
        timings->cacheDirectoryMs = phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    timings->cacheDirectoryMs = phase.nsecsElapsed() / 1000000.0;

    const QString cachePath = directory + QStringLiteral("/events.json");
    if (!hasCachedSnapshot) {
        phase.restart();
        QString cacheValidationError;
        if (!readSnapshotFile(cachePath, &cachedSnapshot)
            || !validateSnapshot(cachedSnapshot, &cacheValidationError)) {
            timings->existingCacheReadMs = phase.nsecsElapsed() / 1000000.0;
            *errorCode = QStringLiteral("FULL_SNAPSHOT_REQUIRED");
            *error = QStringLiteral("the native host has no valid recovery snapshot (%1); send a full calendar snapshot before event updates")
                .arg(cacheValidationError.isEmpty() ? QStringLiteral("cache file is missing or unreadable") : cacheValidationError);
            cachedSnapshot = {};
            return false;
        }
        hasCachedSnapshot = true;
        if (lastWrittenDigest.isEmpty()) {
            lastWrittenDigest = contentDigest(cachedSnapshot);
        }
        timings->existingCacheReadMs = phase.nsecsElapsed() / 1000000.0;
    }
    timings->revision = cachedSnapshot.value(QStringLiteral("contentRevision")).toString();
    if (timings->revision.isEmpty()) {
        timings->revision = QString::fromLatin1(contentDigest(cachedSnapshot).toHex());
    }

    QJsonArray calendars = cachedSnapshot.value(QStringLiteral("calendars")).toArray();
    if (calendars.isEmpty()) {
        *errorCode = QStringLiteral("FULL_SNAPSHOT_REQUIRED");
        *error = QStringLiteral("the recovery snapshot contains no calendars; send a full calendar snapshot first");
        return false;
    }

    QHash<QString, int> calendarIndex;
    for (qsizetype index = 0; index < calendars.size(); ++index) {
        const QString id = calendars.at(index).toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            calendarIndex.insert(id, static_cast<int>(index));
        }
    }

    // Validate all changes and group them before mutating the snapshot. A
    // missing calendar means the host's base revision is out of date; a full
    // snapshot is safer than silently losing a change from a newly added one.
    QHash<QString, QHash<QString, QJsonObject>> changesByCalendar;
    const QJsonArray changes = changesValue.toArray();
    QElapsedTimer validationTimer;
    validationTimer.start();
    for (const QJsonValue &changeValue : changes) {
        if (!changeValue.isObject()) {
            *errorCode = QStringLiteral("INVALID_EVENT_CHANGES");
            *error = QStringLiteral("each event change must be an object");
            return false;
        }
        const QJsonObject change = changeValue.toObject();
        const QString operation = change.value(QStringLiteral("operation")).toString();
        const QString calendarId = change.value(QStringLiteral("calendarId")).toString();
        const QJsonValue eventValue = change.value(QStringLiteral("event"));
        if ((operation != QStringLiteral("add") && operation != QStringLiteral("modify")
             && operation != QStringLiteral("remove"))
            || calendarId.isEmpty() || !eventValue.isObject()) {
            *errorCode = QStringLiteral("INVALID_EVENT_CHANGES");
            *error = QStringLiteral("each change requires operation add/modify/remove, a calendarId and an event object");
            return false;
        }
        const QJsonObject event = eventValue.toObject();
        const QString uid = event.value(QStringLiteral("uid")).toString();
        if (uid.isEmpty()
            || !event.value(QStringLiteral("start")).isString()
            || !event.value(QStringLiteral("end")).isString()) {
            *errorCode = QStringLiteral("INVALID_EVENT_CHANGES");
            *error = QStringLiteral("event change requires a stable UID and string start/end values");
            return false;
        }
        if (!calendarIndex.contains(calendarId)) {
            *errorCode = QStringLiteral("FULL_SNAPSHOT_REQUIRED");
            *error = QStringLiteral("calendar '%1' is not present in the cached snapshot; send a full snapshot to synchronise calendar metadata").arg(calendarId);
            return false;
        }
        // Last observer state wins for a UID. The host compares it to the
        // snapshot's authoritative state to decide add/modify/remove.
        changesByCalendar[calendarId].insert(uid, change);
    }
    timings->validationMs = validationTimer.nsecsElapsed() / 1000000.0;

    QElapsedTimer diffTimer;
    diffTimer.start();
    QJsonArray deltas;
    for (auto calendarIt = changesByCalendar.cbegin(); calendarIt != changesByCalendar.cend(); ++calendarIt) {
        const QString calendarId = calendarIt.key();
        const int index = calendarIndex.value(calendarId);
        QJsonObject calendar = calendars.at(index).toObject();
        const QJsonArray oldEvents = calendar.value(QStringLiteral("events")).toArray();
        QHash<QString, QJsonObject> eventMap;
        QStringList eventOrder;
        eventMap.reserve(oldEvents.size() + calendarIt.value().size());
        eventOrder.reserve(oldEvents.size() + calendarIt.value().size());
        for (const QJsonValue &eventValue : oldEvents) {
            const QJsonObject event = eventValue.toObject();
            const QString uid = event.value(QStringLiteral("uid")).toString();
            if (uid.isEmpty()) {
                continue;
            }
            if (!eventMap.contains(uid)) {
                eventOrder.append(uid);
            }
            eventMap.insert(uid, event);
        }

        for (auto changeIt = calendarIt.value().cbegin(); changeIt != calendarIt.value().cend(); ++changeIt) {
            const QString uid = changeIt.key();
            const QJsonObject change = changeIt.value();
            const QString operation = change.value(QStringLiteral("operation")).toString();
            const QJsonObject event = change.value(QStringLiteral("event")).toObject();
            const auto oldIt = eventMap.constFind(uid);
            const bool exists = oldIt != eventMap.cend();

            if (operation == QStringLiteral("remove")) {
                if (exists) {
                    deltas.append(QJsonObject{
                        {QStringLiteral("operation"), QStringLiteral("remove")},
                        {QStringLiteral("calendarId"), calendarId},
                        {QStringLiteral("event"), oldIt.value()},
                    });
                    eventMap.remove(uid);
                }
                continue;
            }

            if (!exists) {
                eventMap.insert(uid, event);
                eventOrder.append(uid);
                deltas.append(QJsonObject{
                    {QStringLiteral("operation"), QStringLiteral("add")},
                    {QStringLiteral("calendarId"), calendarId},
                    {QStringLiteral("event"), event},
                });
            } else if (oldIt.value() != event) {
                deltas.append(QJsonObject{
                    {QStringLiteral("operation"), QStringLiteral("modify")},
                    {QStringLiteral("calendarId"), calendarId},
                    {QStringLiteral("previous"), oldIt.value()},
                    {QStringLiteral("event"), event},
                });
                eventMap.insert(uid, event);
            }
        }

        QJsonArray updatedEvents;
        for (const QString &uid : std::as_const(eventOrder)) {
            const auto it = eventMap.constFind(uid);
            if (it != eventMap.cend()) {
                updatedEvents.append(it.value());
            }
        }
        if (updatedEvents.size() > MaxEventsPerCalendar) {
            *errorCode = QStringLiteral("EVENT_LIMIT_EXCEEDED");
            *error = QStringLiteral("calendar '%1' would contain %2 events; host limit is %3 per calendar")
                .arg(calendarId).arg(updatedEvents.size()).arg(MaxEventsPerCalendar);
            return false;
        }
        calendar.insert(QStringLiteral("events"), updatedEvents);
        calendars.replace(index, calendar);
    }
    timings->eventDiffMs = diffTimer.nsecsElapsed() / 1000000.0;

    if (deltas.isEmpty()) {
        timings->unchanged = true;
        timings->eventDeltaCount = 0;
        return true;
    }

    timings->eventDeltaCount = deltas.size();
    QJsonObject updatedSnapshot = cachedSnapshot;
    updatedSnapshot.insert(QStringLiteral("calendars"), calendars);
    updatedSnapshot.insert(QStringLiteral("generatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

    phase.restart();
    const QByteArray digest = contentDigest(updatedSnapshot);
    timings->contentHashMs = phase.nsecsElapsed() / 1000000.0;
    const QString previousRevision = cachedSnapshot.value(QStringLiteral("contentRevision")).toString().isEmpty()
        ? QString::fromLatin1(lastWrittenDigest.toHex())
        : cachedSnapshot.value(QStringLiteral("contentRevision")).toString();
    const QString revision = QString::fromLatin1(digest.toHex());
    timings->revision = revision;
    updatedSnapshot.insert(QStringLiteral("contentRevision"), revision);

    phase.restart();
    const QByteArray data = QJsonDocument(updatedSnapshot).toJson(QJsonDocument::Compact);
    timings->snapshotSerializationMs = phase.nsecsElapsed() / 1000000.0;
    timings->cacheWriteBytes = data.size();

    phase.restart();
    QSaveFile file(cachePath);
    if (!file.open(QIODevice::WriteOnly)) {
        *errorCode = QStringLiteral("CACHE_WRITE_ERROR");
        *error = QStringLiteral("could not open cache file for direct event update: %1").arg(file.errorString());
        timings->diskCommitMs = phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    const auto privateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!file.setPermissions(privateFile)) {
        *errorCode = QStringLiteral("CACHE_PERMISSION_ERROR");
        *error = QStringLiteral("could not restrict cache file permissions: %1").arg(cachePath);
        file.cancelWriting();
        timings->diskCommitMs = phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        *errorCode = QStringLiteral("CACHE_WRITE_ERROR");
        *error = QStringLiteral("could not commit direct event update: %1").arg(file.errorString());
        timings->diskCommitMs = phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    timings->diskCommitMs = phase.nsecsElapsed() / 1000000.0;
    lastWrittenDigest = digest;
    cachedSnapshot = updatedSnapshot;
    hasCachedSnapshot = true;

    phase.restart();
    notifyEventDeltas(deltas, previousRevision, revision);
    timings->dbusNotifyMs = phase.nsecsElapsed() / 1000000.0;
    return true;
}

QJsonObject makeResponse(bool ok,
                         const QString &error,
                         const QString &errorCode,
                         const QJsonValue &requestId,
                         const HostTimings *timings,
                         bool includeTimings)
{
    QJsonObject response{
        {QStringLiteral("ok"), ok},
        {QStringLiteral("error"), error},
    };
    if (!errorCode.isEmpty()) {
        response.insert(QStringLiteral("code"), errorCode);
    }
    if (!requestId.isUndefined() && !requestId.isNull()) {
        response.insert(QStringLiteral("requestId"), requestId);
    }
    if (ok && timings && timings->eventDeltaCount > 0) {
        response.insert(QStringLiteral("appliedChanges"), static_cast<double>(timings->eventDeltaCount));
    }
    if (ok && timings && !timings->revision.isEmpty()) {
        response.insert(QStringLiteral("revision"), timings->revision);
    }
    if (includeTimings && timings) {
        response.insert(QStringLiteral("hostTimingsMs"), timings->toJson());
    }
    return response;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("thunderbird-calendar-native-host"));

    while (true) {
        // Time waiting for Thunderbird separately. This interval is idle time
        // between requests, not part of the current request's processing time.
        double messageWaitMs = 0.0;
        if (!waitForInput(&messageWaitMs)) {
            std::cerr << "Thunderbird Plasma Calendar: error waiting for native-messaging input\n";
            return 2;
        }

        // Start the per-message timer only after poll() reports readable input.
        // It includes reading the frame header and complete payload, unlike the
        // old timer which also included arbitrary idle time between messages.
        QElapsedTimer messageTimer;
        messageTimer.start();
        QElapsedTimer frameTimer;
        frameTimer.start();

        std::array<unsigned char, sizeof(quint32)> header{};
        const ReadStatus headerStatus = readExact(header.data(), header.size());
        if (headerStatus == ReadStatus::EndOfStream) {
            return 0;
        }
        if (headerStatus != ReadStatus::Complete) {
            std::cerr << "Thunderbird Plasma Calendar: incomplete native-messaging frame header\n";
            return 2;
        }

        quint32 length = 0;
        std::memcpy(&length, header.data(), sizeof(length));
        length = qFromLittleEndian(length);
        if (length == 0) {
            std::cerr << "Thunderbird Plasma Calendar: invalid native-messaging frame size (zero bytes)\n";
            return 2;
        }
        if (length > MaxMessageBytes) {
            // Drain the frame so that the protocol remains synchronised, then return
            // a structured error. The extension normally catches this before send.
            std::array<char, 64 * 1024> scratch{};
            quint32 remaining = length;
            while (remaining > 0) {
                const auto chunk = static_cast<std::size_t>(std::min(remaining, static_cast<quint32>(scratch.size())));
                if (readExact(scratch.data(), chunk) != ReadStatus::Complete) {
                    std::cerr << "Thunderbird Plasma Calendar: incomplete oversized native-messaging frame payload\n";
                    return 2;
                }
                remaining -= static_cast<quint32>(chunk);
            }

            const QString error = QStringLiteral(
                "Native messaging payload is too large: received %1 (%2 bytes); the host limit is %3 (%4 bytes). "
                "Reduce the look-back/look-ahead range, disable description/location export, or enable text truncation.")
                .arg(formatMiB(length))
                .arg(length)
                .arg(formatMiB(MaxMessageBytes))
                .arg(MaxMessageBytes);
            QJsonObject response{
                {QStringLiteral("ok"), false},
                {QStringLiteral("code"), QStringLiteral("NATIVE_MESSAGE_TOO_LARGE")},
                {QStringLiteral("error"), error},
                {QStringLiteral("payloadBytes"), static_cast<double>(length)},
                {QStringLiteral("maxBytes"), static_cast<double>(MaxMessageBytes)},
            };
            if (!writeFrame(response)) {
                std::cerr << "Thunderbird Plasma Calendar: could not return oversized-message error to extension\n";
                return 3;
            }
            // The extension reports this actionable size error even when debug
            // logging is disabled, so do not emit optional phase timings here.
            continue;
        }

        QByteArray payload(static_cast<qsizetype>(length), Qt::Uninitialized);
        if (readExact(payload.data(), static_cast<std::size_t>(length)) != ReadStatus::Complete) {
            std::cerr << "Thunderbird Plasma Calendar: incomplete native-messaging frame payload\n";
            return 2;
        }
        const double frameReadMs = frameTimer.nsecsElapsed() / 1000000.0;

        QElapsedTimer processingTimer;
        processingTimer.start();
        HostTimings timings;
        timings.messageWaitMs = messageWaitMs;
        timings.frameReadMs = frameReadMs;

        QElapsedTimer parseTimer;
        parseTimer.start();
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
        timings.jsonParseMs = parseTimer.nsecsElapsed() / 1000000.0;
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            QJsonObject response = makeResponse(
                false, QStringLiteral("invalid JSON object: %1").arg(parseError.errorString()),
                QStringLiteral("INVALID_JSON"), QJsonValue(), &timings, false);
            QElapsedTimer responseWriteTimer;
            responseWriteTimer.start();
            if (!writeFrame(response)) {
                return 3;
            }
            continue;
        }

        const QJsonObject request = document.object();
        const bool includeTimings = request.value(QStringLiteral("debugLogs")).toBool(false);
        const QJsonValue requestId = request.value(QStringLiteral("requestId"));
        QString error;
        QString errorCode;
        const QString messageType = request.value(QStringLiteral("type")).toString();
        const bool ok = messageType == QStringLiteral("snapshot")
            ? acceptSnapshot(request, &error, &errorCode, &timings)
            : messageType == QStringLiteral("eventChanges")
                ? acceptEventChanges(request, &error, &errorCode, &timings)
                : false;
        if (messageType != QStringLiteral("snapshot") && messageType != QStringLiteral("eventChanges")) {
            errorCode = QStringLiteral("INVALID_MESSAGE_TYPE");
            error = QStringLiteral("supported message types are snapshot and eventChanges");
        }
        timings.hostProcessingMs = processingTimer.nsecsElapsed() / 1000000.0;
        QJsonObject response = makeResponse(ok, error, errorCode, requestId, &timings, includeTimings);

        QElapsedTimer responseWriteTimer;
        responseWriteTimer.start();
        const bool responseWritten = writeFrame(response);
        const double responseWriteMs = responseWriteTimer.nsecsElapsed() / 1000000.0;
        const double totalMessageMs = messageTimer.nsecsElapsed() / 1000000.0;
        if (includeTimings) {
            // The total is emitted on stderr because it includes writing the
            // response; that duration cannot be embedded in the response itself.
            std::cerr << "Thunderbird Plasma Calendar: native-host message timing (ms): "
                      << "wait before message " << messageWaitMs
                      << ", frame read " << frameReadMs
                      << ", host processing " << timings.hostProcessingMs
                      << ", response frame write " << responseWriteMs
                      << ", total message handling " << totalMessageMs << "\n";
        }
        if (!responseWritten) {
            std::cerr << "Thunderbird Plasma Calendar: could not write native-host response\n";
            return 3;
        }
    }
}
