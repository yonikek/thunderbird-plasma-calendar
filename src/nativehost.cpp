/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal Mozilla native-messaging host for the Thunderbird / Plasma bridge.
 * It accepts full snapshots and item-level calendar changes only; it cannot
 * execute commands or choose file paths. Direct event changes are appended to a
 * checksummed journal and fsync'd before D-Bus delivery. The complete recovery
 * snapshot is atomically compacted periodically, keeping routine updates small.
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
#include <QFileInfo>
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
#include <fcntl.h>
#include <sys/stat.h>
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
    QString messageType;
    double messageWaitMs = 0.0;
    double frameReadMs = 0.0;
    double jsonParseMs = 0.0;
    double validationMs = 0.0;
    double cacheValidationMs = 0.0;
    double contentHashMs = 0.0;
    double cacheDirectoryMs = 0.0;
    double existingCacheReadMs = 0.0;
    double cachePreparationMs = 0.0;
    double calendarIndexMs = 0.0;
    double changeValidationMs = 0.0;
    double eventIndexMs = 0.0;
    double eventApplyMs = 0.0;
    double eventArrayRebuildMs = 0.0;
    double snapshotSerializationMs = 0.0;
    double diskCommitMs = 0.0;
    double dbusNotifyMs = 0.0;
    double hostProcessingMs = 0.0;
    double eventDiffMs = 0.0;
    double journalReplayMs = 0.0;
    double journalSerializationMs = 0.0;
    double journalAppendMs = 0.0;
    double journalFsyncMs = 0.0;
    double journalCompactionMs = 0.0;
    qint64 journalRecordsReplayed = 0;
    qint64 journalRecordCount = 0;
    qint64 journalFileBytes = 0;
    qint64 journalWriteBytes = 0;
    bool journalCompacted = false;
    qint64 directChangeCount = 0;
    qint64 affectedCalendarCount = 0;
    qint64 eventDeltaCount = 0;
    qint64 cacheWriteBytes = 0;
    QString revision;
    bool unchanged = false;

    QJsonObject toJson() const
    {
        return {
            {QStringLiteral("messageType"), messageType},
            {QStringLiteral("messageWaitMs"), messageWaitMs},
            {QStringLiteral("frameReadMs"), frameReadMs},
            {QStringLiteral("jsonParseMs"), jsonParseMs},
            {QStringLiteral("validationMs"), validationMs},
            {QStringLiteral("cacheValidationMs"), cacheValidationMs},
            {QStringLiteral("contentHashMs"), contentHashMs},
            {QStringLiteral("cacheDirectoryMs"), cacheDirectoryMs},
            {QStringLiteral("existingCacheReadMs"), existingCacheReadMs},
            {QStringLiteral("cachePreparationMs"), cachePreparationMs},
            {QStringLiteral("calendarIndexMs"), calendarIndexMs},
            {QStringLiteral("changeValidationMs"), changeValidationMs},
            {QStringLiteral("eventIndexMs"), eventIndexMs},
            {QStringLiteral("eventApplyMs"), eventApplyMs},
            {QStringLiteral("eventArrayRebuildMs"), eventArrayRebuildMs},
            {QStringLiteral("snapshotSerializationMs"), snapshotSerializationMs},
            {QStringLiteral("diskCommitMs"), diskCommitMs},
            {QStringLiteral("dbusNotifyMs"), dbusNotifyMs},
            {QStringLiteral("hostProcessingMs"), hostProcessingMs},
            {QStringLiteral("eventDiffMs"), eventDiffMs},
            {QStringLiteral("journalReplayMs"), journalReplayMs},
            {QStringLiteral("journalSerializationMs"), journalSerializationMs},
            {QStringLiteral("journalAppendMs"), journalAppendMs},
            {QStringLiteral("journalFsyncMs"), journalFsyncMs},
            {QStringLiteral("journalCompactionMs"), journalCompactionMs},
            {QStringLiteral("journalRecordsReplayed"), static_cast<double>(journalRecordsReplayed)},
            {QStringLiteral("journalRecordCount"), static_cast<double>(journalRecordCount)},
            {QStringLiteral("journalFileBytes"), static_cast<double>(journalFileBytes)},
            {QStringLiteral("journalWriteBytes"), static_cast<double>(journalWriteBytes)},
            {QStringLiteral("journalCompacted"), journalCompacted},
            {QStringLiteral("directChangeCount"), static_cast<double>(directChangeCount)},
            {QStringLiteral("affectedCalendarCount"), static_cast<double>(affectedCalendarCount)},
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

QString snapshotCachePath()
{
    return cacheDirectory() + QStringLiteral("/events.json");
}

QString journalCachePath()
{
    return cacheDirectory() + QStringLiteral("/events.journal");
}

qint64 journalRecordCount = 0;
qint64 journalBytes = 0;
bool journalReady = false;

constexpr qint64 CompactJournalAfterRecords = 128;
constexpr qint64 CompactJournalAfterBytes = 1024 * 1024;
constexpr qint64 MaxJournalBytes = 16 * 1024 * 1024;

bool syncFileDescriptor(int descriptor);
bool syncParentDirectory(const QString &filePath, QString *error);

bool writeAtomicFile(const QString &path, const QByteArray &data, QString *error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("could not open '%1': %2").arg(path, file.errorString());
        return false;
    }
    const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!file.setPermissions(permissions)) {
        *error = QStringLiteral("could not restrict permissions for '%1': %2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (file.write(data) != data.size() || !file.flush()) {
        *error = QStringLiteral("could not write temporary file for '%1': %2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!syncFileDescriptor(file.handle())) {
        *error = QStringLiteral("could not fsync temporary file for '%1': %2")
            .arg(path, QString::fromLocal8Bit(std::strerror(errno)));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        *error = QStringLiteral("could not atomically commit '%1': %2").arg(path, file.errorString());
        return false;
    }
    // Persist the rename itself, not only the file contents. This matters when
    // the journal is first created or atomically replaced during compaction.
    return syncParentDirectory(path, error);
}

QByteArray journalHeaderLine(const QString &baseRevision)
{
    const QJsonObject header{
        {QStringLiteral("type"), QStringLiteral("header")},
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("baseRevision"), baseRevision},
    };
    return QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n';
}

bool resetJournal(const QString &path, const QString &baseRevision, QString *error)
{
    const QByteArray header = journalHeaderLine(baseRevision);
    if (!writeAtomicFile(path, header, error)) {
        journalReady = false;
        return false;
    }
    journalRecordCount = 0;
    journalBytes = header.size();
    journalReady = true;
    return true;
}

bool syncFileDescriptor(int descriptor)
{
    while (::fsync(descriptor) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool syncParentDirectory(const QString &filePath, QString *error)
{
    const QByteArray encodedDirectory = QFile::encodeName(QFileInfo(filePath).absolutePath());
    const int descriptor = ::open(encodedDirectory.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        *error = QStringLiteral("could not open parent directory for '%1': %2")
            .arg(filePath, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    const bool synced = syncFileDescriptor(descriptor);
    const int savedErrno = errno;
    ::close(descriptor);
    if (!synced) {
        *error = QStringLiteral("could not sync parent directory for '%1': %2")
            .arg(filePath, QString::fromLocal8Bit(std::strerror(savedErrno)));
        return false;
    }
    return true;
}

bool appendJournalRecord(const QString &path,
                         const QByteArray &line,
                         double *appendMs,
                         double *fsyncMs,
                         QString *error)
{
    QElapsedTimer appendTimer;
    appendTimer.start();
    const QByteArray encodedPath = QFile::encodeName(path);
    const int descriptor = ::open(encodedPath.constData(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        *appendMs = appendTimer.nsecsElapsed() / 1000000.0;
        *error = QStringLiteral("could not open event journal '%1': %2")
            .arg(path, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }

    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        const int savedErrno = errno;
        ::close(descriptor);
        *appendMs = appendTimer.nsecsElapsed() / 1000000.0;
        *error = QStringLiteral("could not inspect event journal '%1' before append: %2")
            .arg(path, QString::fromLocal8Bit(std::strerror(savedErrno)));
        return false;
    }
    const off_t originalSize = status.st_size;
    bool writeSucceeded = (::fchmod(descriptor, S_IRUSR | S_IWUSR) == 0)
        && writeAll(descriptor, line.constData(), static_cast<std::size_t>(line.size()));
    *appendMs = appendTimer.nsecsElapsed() / 1000000.0;

    QElapsedTimer syncTimer;
    syncTimer.start();
    const bool syncSucceeded = writeSucceeded && syncFileDescriptor(descriptor);
    *fsyncMs = syncTimer.nsecsElapsed() / 1000000.0;

    if (!writeSucceeded || !syncSucceeded) {
        const int savedErrno = errno;
        // Do not leave a partial JSONL record that a later append could turn
        // into an apparently complete but invalid record. A failed rollback
        // is recovered by re-reading the journal and dropping only its torn tail.
        ::ftruncate(descriptor, originalSize);
        syncFileDescriptor(descriptor);
        ::close(descriptor);
        *error = QStringLiteral("could not durably append event journal '%1': %2")
            .arg(path, QString::fromLocal8Bit(std::strerror(savedErrno)));
        return false;
    }

    if (::close(descriptor) != 0) {
        // The preceding fsync succeeded, so the record is already durable.
        // A close error must not turn a committed append into an apparent
        // failure: the caller would retry an operation whose journal entry is
        // already present, potentially suppressing its D-Bus update.
        std::cerr << "Thunderbird Plasma Calendar: close reported an error after a successful journal fsync: "
                  << std::strerror(errno) << "\n";
    }
    return true;
}

bool truncateJournal(const QString &path, qint64 size, QString *error)
{
    const QByteArray encodedPath = QFile::encodeName(path);
    const int descriptor = ::open(encodedPath.constData(), O_WRONLY | O_CLOEXEC);
    if (descriptor < 0) {
        *error = QStringLiteral("could not open event journal for tail recovery: %1")
            .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    const bool truncated = (::ftruncate(descriptor, static_cast<off_t>(size)) == 0)
        && syncFileDescriptor(descriptor);
    const int savedErrno = errno;
    ::close(descriptor);
    if (!truncated) {
        *error = QStringLiteral("could not truncate incomplete event-journal tail: %1")
            .arg(QString::fromLocal8Bit(std::strerror(savedErrno)));
        return false;
    }
    return true;
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
        const QJsonObject event = delta.value(QStringLiteral("event")).toObject();
        const QString uid = event.value(QStringLiteral("uid")).toString();
        if ((operation != QStringLiteral("add") && operation != QStringLiteral("modify")
             && operation != QStringLiteral("remove"))
            || calendarId.isEmpty() || uid.isEmpty() || !calendarIndexes.contains(calendarId)) {
            *error = QStringLiteral("journal contains an invalid delta or references an unknown calendar");
            return false;
        }
        grouped[calendarId].insert(uid, delta);
    }

    for (auto group = grouped.cbegin(); group != grouped.cend(); ++group) {
        const QString calendarId = group.key();
        const int calendarIndex = calendarIndexes.value(calendarId);
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
                    *error = QStringLiteral("journal removal references missing event '%1' in calendar '%2'")
                        .arg(uid, calendarId);
                    return false;
                }
                eventMap.remove(uid);
            } else if (operation == QStringLiteral("add")) {
                if (existing != eventMap.cend()) {
                    *error = QStringLiteral("journal addition duplicates event '%1' in calendar '%2'")
                        .arg(uid, calendarId);
                    return false;
                }
                eventMap.insert(uid, event);
                order.append(uid);
            } else {
                if (existing == eventMap.cend()) {
                    *error = QStringLiteral("journal modification references missing event '%1' in calendar '%2'")
                        .arg(uid, calendarId);
                    return false;
                }
                const QJsonValue previousValue = delta.value(QStringLiteral("previous"));
                if (previousValue.isObject() && existing.value() != previousValue.toObject()) {
                    *error = QStringLiteral("journal modification has an unexpected previous value for event '%1'")
                        .arg(uid);
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
            *error = QStringLiteral("replayed journal would exceed the event limit for calendar '%1'").arg(calendarId);
            return false;
        }
        calendar.insert(QStringLiteral("events"), updatedEvents);
        calendars.replace(calendarIndex, calendar);
    }

    snapshot->insert(QStringLiteral("calendars"), calendars);
    return true;
}

bool writeSnapshotAtomically(const QString &path,
                             const QJsonObject &snapshot,
                             double *serializationMs,
                             double *commitMs,
                             qint64 *bytesWritten,
                             QString *error)
{
    QElapsedTimer phase;
    phase.start();
    const QByteArray data = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    *serializationMs = phase.nsecsElapsed() / 1000000.0;
    *bytesWritten = data.size();

    phase.restart();
    if (!writeAtomicFile(path, data, error)) {
        *commitMs = phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    *commitMs = phase.nsecsElapsed() / 1000000.0;
    return true;
}

bool loadRecoveryState(const QString &cachePath,
                       const QString &journalPath,
                       QJsonObject *snapshot,
                       QString *error,
                       HostTimings *timings)
{
    QElapsedTimer phase;
    phase.start();
    if (!readSnapshotFile(cachePath, snapshot)) {
        timings->existingCacheReadMs += phase.nsecsElapsed() / 1000000.0;
        *error = QStringLiteral("recovery snapshot is missing or unreadable");
        return false;
    }
    timings->existingCacheReadMs += phase.nsecsElapsed() / 1000000.0;

    phase.restart();
    if (!validateSnapshot(*snapshot, error)) {
        timings->cacheValidationMs += phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    timings->cacheValidationMs += phase.nsecsElapsed() / 1000000.0;

    phase.restart();
    const QString storedBaseRevision = snapshot->value(QStringLiteral("contentRevision")).toString();
    const QString baseRevision = QString::fromLatin1(contentDigest(*snapshot).toHex());
    timings->contentHashMs += phase.nsecsElapsed() / 1000000.0;
    if (!storedBaseRevision.isEmpty() && storedBaseRevision != baseRevision) {
        *error = QStringLiteral("recovery snapshot content does not match its stored revision");
        return false;
    }
    snapshot->insert(QStringLiteral("contentRevision"), baseRevision);
    QElapsedTimer journalTimer;
    journalTimer.start();

    if (!QFile::exists(journalPath)) {
        QString journalError;
        if (!resetJournal(journalPath, baseRevision, &journalError)) {
            timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
            *error = journalError;
            return false;
        }
        snapshot->insert(QStringLiteral("contentRevision"), baseRevision);
        lastWrittenDigest = QByteArray::fromHex(baseRevision.toLatin1());
        timings->journalFileBytes = journalBytes;
        timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
        return true;
    }

    QFile journal(journalPath);
    if (!journal.open(QIODevice::ReadOnly)) {
        timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
        *error = QStringLiteral("could not read recovery journal: %1").arg(journal.errorString());
        return false;
    }
    if (journal.size() > MaxJournalBytes) {
        timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
        *error = QStringLiteral("recovery journal exceeds the %1 MiB safety limit; a full snapshot is required")
            .arg(MaxJournalBytes / (1024 * 1024));
        return false;
    }
    const QByteArray contents = journal.readAll();
    journal.close();
    timings->journalFileBytes = contents.size();

    const qsizetype headerEnd = contents.indexOf('\n');
    if (headerEnd < 0) {
        timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
        *error = QStringLiteral("recovery journal has no complete header");
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument headerDocument = QJsonDocument::fromJson(contents.left(headerEnd), &parseError);
    if (parseError.error != QJsonParseError::NoError || !headerDocument.isObject()) {
        timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
        *error = QStringLiteral("recovery journal header is invalid");
        return false;
    }
    const QJsonObject header = headerDocument.object();
    if (header.value(QStringLiteral("type")).toString() != QStringLiteral("header")
        || header.value(QStringLiteral("schemaVersion")).toInt(-1) != 1
        || !header.value(QStringLiteral("baseRevision")).isString()) {
        timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
        *error = QStringLiteral("recovery journal header has an unsupported schema");
        return false;
    }

    // A snapshot is atomically committed before a journal is reset. If a crash
    // occurs between those operations, its revision no longer matches the old
    // journal header. The snapshot therefore supersedes that entire old journal.
    if (header.value(QStringLiteral("baseRevision")).toString() != baseRevision) {
        QString journalError;
        if (!resetJournal(journalPath, baseRevision, &journalError)) {
            timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
            *error = journalError;
            return false;
        }
        snapshot->insert(QStringLiteral("contentRevision"), baseRevision);
        lastWrittenDigest = QByteArray::fromHex(baseRevision.toLatin1());
        timings->journalFileBytes = journalBytes;
        timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
        return true;
    }

    QString currentRevision = baseRevision;
    qsizetype validBytes = headerEnd + 1;
    qsizetype offset = validBytes;
    qint64 recordsReplayed = 0;
    while (offset < contents.size()) {
        const qsizetype lineEnd = contents.indexOf('\n', offset);
        if (lineEnd < 0) {
            // An append interrupted by a process crash is unacknowledged. Keep
            // all complete preceding records and discard only this torn tail.
            QString truncateError;
            if (!truncateJournal(journalPath, validBytes, &truncateError)) {
                timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
                *error = truncateError;
                return false;
            }
            break;
        }
        const QByteArray line = contents.mid(offset, lineEnd - offset);
        const QJsonDocument recordDocument = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !recordDocument.isObject()) {
            timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
            *error = QStringLiteral("recovery journal contains an invalid complete record at byte %1").arg(offset);
            return false;
        }
        const QJsonObject record = recordDocument.object();
        const QString recordRevision = record.value(QStringLiteral("revision")).toString();
        const QString previousRevision = record.value(QStringLiteral("previousRevision")).toString();
        const QJsonValue deltasValue = record.value(QStringLiteral("deltas"));
        if (record.value(QStringLiteral("type")).toString() != QStringLiteral("delta")
            || record.value(QStringLiteral("schemaVersion")).toInt(-1) != 1
            || previousRevision != currentRevision || recordRevision.isEmpty()
            || !deltasValue.isArray()
            || record.value(QStringLiteral("checksum")).toString().toLatin1() != journalRecordChecksum(record)) {
            timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
            *error = QStringLiteral("recovery journal chain or checksum is invalid at byte %1").arg(offset);
            return false;
        }
        QString applyError;
        if (!applyJournalDeltas(snapshot, deltasValue.toArray(), &applyError)) {
            timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
            *error = QStringLiteral("could not replay recovery journal record: %1").arg(applyError);
            return false;
        }
        const QString generatedAt = record.value(QStringLiteral("generatedAt")).toString();
        if (!generatedAt.isEmpty()) {
            snapshot->insert(QStringLiteral("generatedAt"), generatedAt);
        }
        snapshot->insert(QStringLiteral("contentRevision"), recordRevision);
        currentRevision = recordRevision;
        ++recordsReplayed;
        validBytes = lineEnd + 1;
        offset = validBytes;
    }

    QElapsedTimer hashTimer;
    hashTimer.start();
    const QString recoveredDigest = QString::fromLatin1(contentDigest(*snapshot).toHex());
    timings->contentHashMs += hashTimer.nsecsElapsed() / 1000000.0;
    if (recoveredDigest != currentRevision) {
        timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
        *error = QStringLiteral("replayed recovery journal does not match its final content revision");
        return false;
    }

    journalRecordCount = recordsReplayed;
    journalBytes = validBytes;
    journalReady = true;
    timings->journalRecordsReplayed = recordsReplayed;
    timings->journalFileBytes = validBytes;
    timings->journalReplayMs += journalTimer.nsecsElapsed() / 1000000.0;
    lastWrittenDigest = QByteArray::fromHex(currentRevision.toLatin1());
    return true;
}

bool compactJournal(const QString &cachePath,
                    const QString &journalPath,
                    const QJsonObject &snapshot,
                    const QString &revision,
                    HostTimings *timings,
                    QString *error)
{
    QElapsedTimer phase;
    phase.start();
    qint64 bytesWritten = 0;
    double serializationMs = 0.0;
    double commitMs = 0.0;
    if (!writeSnapshotAtomically(cachePath, snapshot, &serializationMs, &commitMs, &bytesWritten, error)) {
        timings->snapshotSerializationMs += serializationMs;
        timings->diskCommitMs += commitMs;
        timings->journalCompactionMs += phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    timings->snapshotSerializationMs += serializationMs;
    timings->diskCommitMs += commitMs;
    timings->cacheWriteBytes += bytesWritten;

    // Commit the new base snapshot first. If this header replacement fails,
    // the old journal remains intact but has a different base revision; the
    // next request will safely reset it before appending another record.
    QString journalError;
    if (!resetJournal(journalPath, revision, &journalError)) {
        journalReady = false;
        *error = journalError;
        timings->journalCompactionMs += phase.nsecsElapsed() / 1000000.0;
        return false;
    }
    timings->journalCompactionMs += phase.nsecsElapsed() / 1000000.0;
    timings->journalRecordCount = 0;
    timings->journalFileBytes = journalBytes;
    return true;
}

bool ensureJournalReady(const QString &journalPath, const QString &currentRevision, QString *error)
{
    if (journalReady) {
        return true;
    }
    return resetJournal(journalPath, currentRevision, error);
}

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
    const QString revision = QString::fromLatin1(digest.toHex());
    timings->revision = revision;

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

    const QString cachePath = snapshotCachePath();
    const QString journalPath = journalCachePath();
    if (!hasCachedSnapshot && QFile::exists(cachePath)) {
        QString recoveryError;
        if (loadRecoveryState(cachePath, journalPath, &cachedSnapshot, &recoveryError, timings)) {
            hasCachedSnapshot = true;
        } else {
            std::cerr << "Thunderbird Plasma Calendar: existing recovery cache could not be loaded; replacing it with the incoming full snapshot: "
                      << recoveryError.toStdString() << "\n";
            cachedSnapshot = {};
            hasCachedSnapshot = false;
            lastWrittenDigest.clear();
            journalReady = false;
        }
    }
    timings->cachePreparationMs = timings->cacheDirectoryMs + timings->existingCacheReadMs
        + timings->cacheValidationMs + timings->journalReplayMs;

    if (hasCachedSnapshot && digest == lastWrittenDigest && QFile::exists(cachePath)) {
        timings->unchanged = true;
        timings->revision = QString::fromLatin1(lastWrittenDigest.toHex());
        if (journalRecordCount >= CompactJournalAfterRecords || journalBytes >= CompactJournalAfterBytes) {
            QString compactError;
            if (!compactJournal(cachePath, journalPath, cachedSnapshot, timings->revision, timings, &compactError)) {
                std::cerr << "Thunderbird Plasma Calendar: could not compact event journal: "
                          << compactError.toStdString() << "\n";
            }
        }
        return true;
    }

    QJsonArray deltas;
    QString previousRevision;
    const bool incrementalUpdate = hasCachedSnapshot;
    if (incrementalUpdate) {
        previousRevision = cachedSnapshot.value(QStringLiteral("contentRevision")).toString();
        if (previousRevision.isEmpty()) {
            previousRevision = QString::fromLatin1(lastWrittenDigest.toHex());
        }
        QHash<QString, IndexedEvent> previousEvents;
        phase.restart();
        previousEvents = indexEvents(cachedSnapshot);
        deltas = eventDeltas(previousEvents, snapshot);
        timings->eventDiffMs = phase.nsecsElapsed() / 1000000.0;
        timings->eventDeltaCount = deltas.size();
    }

    QJsonObject persistedSnapshot = snapshot;
    persistedSnapshot.insert(QStringLiteral("contentRevision"), revision);
    phase.restart();
    qint64 bytesWritten = 0;
    double serializationMs = 0.0;
    double commitMs = 0.0;
    if (!writeSnapshotAtomically(cachePath, persistedSnapshot, &serializationMs, &commitMs,
                                 &bytesWritten, error)) {
        *errorCode = QStringLiteral("CACHE_WRITE_ERROR");
        timings->snapshotSerializationMs += serializationMs;
        timings->diskCommitMs += commitMs;
        return false;
    }
    timings->snapshotSerializationMs += serializationMs;
    timings->diskCommitMs += commitMs;
    timings->cacheWriteBytes = bytesWritten;

    lastWrittenDigest = digest;
    cachedSnapshot = persistedSnapshot;
    hasCachedSnapshot = true;

    QString journalError;
    if (!resetJournal(journalPath, revision, &journalError)) {
        // The snapshot itself is safely committed. The old journal is ignored
        // on the next load because its header base revision will not match.
            journalReady = false;
        std::cerr << "Thunderbird Plasma Calendar: snapshot committed but journal reset failed: "
                  << journalError.toStdString() << "\n";
    }
    timings->journalRecordCount = journalRecordCount;
    timings->journalFileBytes = journalBytes;

    phase.restart();
    if (incrementalUpdate) {
        notifyEventDeltas(deltas, previousRevision, revision);
    } else {
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
    const QJsonArray changes = changesValue.toArray();
    timings->directChangeCount = changes.size();

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

    const QString cachePath = snapshotCachePath();
    const QString journalPath = journalCachePath();
    if (!hasCachedSnapshot) {
        QString recoveryError;
        if (!loadRecoveryState(cachePath, journalPath, &cachedSnapshot, &recoveryError, timings)) {
            *errorCode = QStringLiteral("FULL_SNAPSHOT_REQUIRED");
            *error = QStringLiteral("the native host could not restore its recovery snapshot (%1); send a full snapshot before event updates")
                .arg(recoveryError);
            cachedSnapshot = {};
            journalReady = false;
            timings->cachePreparationMs = timings->cacheDirectoryMs + timings->existingCacheReadMs
                + timings->cacheValidationMs + timings->journalReplayMs;
            return false;
        }
        hasCachedSnapshot = true;
        lastWrittenDigest = contentDigest(cachedSnapshot);
    }
    timings->cachePreparationMs = timings->cacheDirectoryMs + timings->existingCacheReadMs
        + timings->cacheValidationMs + timings->journalReplayMs;

    QString currentRevision = cachedSnapshot.value(QStringLiteral("contentRevision")).toString();
    if (currentRevision.isEmpty()) {
        phase.restart();
        currentRevision = QString::fromLatin1(contentDigest(cachedSnapshot).toHex());
        timings->contentHashMs += phase.nsecsElapsed() / 1000000.0;
        cachedSnapshot.insert(QStringLiteral("contentRevision"), currentRevision);
        lastWrittenDigest = QByteArray::fromHex(currentRevision.toLatin1());
    }
    if (!ensureJournalReady(journalPath, currentRevision, error)) {
        *errorCode = QStringLiteral("JOURNAL_WRITE_ERROR");
        return false;
    }
    timings->revision = currentRevision;

    QJsonArray calendars = cachedSnapshot.value(QStringLiteral("calendars")).toArray();
    if (calendars.isEmpty()) {
        *errorCode = QStringLiteral("FULL_SNAPSHOT_REQUIRED");
        *error = QStringLiteral("the recovery snapshot contains no calendars; send a full calendar snapshot first");
        return false;
    }

    QHash<QString, int> calendarIndex;
    phase.restart();
    for (qsizetype index = 0; index < calendars.size(); ++index) {
        const QString id = calendars.at(index).toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            calendarIndex.insert(id, static_cast<int>(index));
        }
    }
    timings->calendarIndexMs = phase.nsecsElapsed() / 1000000.0;

    QHash<QString, QHash<QString, QJsonObject>> changesByCalendar;
    QElapsedTimer validationTimer;
    validationTimer.start();
    for (const QJsonValue &changeValue : changes) {
        if (!changeValue.isObject()) {
            *errorCode = QStringLiteral("INVALID_EVENT_CHANGES");
            *error = QStringLiteral("each event change must be an object");
            timings->changeValidationMs = validationTimer.nsecsElapsed() / 1000000.0;
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
            timings->changeValidationMs = validationTimer.nsecsElapsed() / 1000000.0;
            return false;
        }
        const QJsonObject event = eventValue.toObject();
        const QString uid = event.value(QStringLiteral("uid")).toString();
        if (uid.isEmpty()
            || !event.value(QStringLiteral("start")).isString()
            || !event.value(QStringLiteral("end")).isString()) {
            *errorCode = QStringLiteral("INVALID_EVENT_CHANGES");
            *error = QStringLiteral("event change requires a stable UID and string start/end values");
            timings->changeValidationMs = validationTimer.nsecsElapsed() / 1000000.0;
            return false;
        }
        if (!calendarIndex.contains(calendarId)) {
            *errorCode = QStringLiteral("FULL_SNAPSHOT_REQUIRED");
            *error = QStringLiteral("calendar '%1' is not present in the cached snapshot; send a full snapshot to synchronise calendar metadata")
                .arg(calendarId);
            timings->changeValidationMs = validationTimer.nsecsElapsed() / 1000000.0;
            return false;
        }
        changesByCalendar[calendarId].insert(uid, change);
    }
    timings->changeValidationMs = validationTimer.nsecsElapsed() / 1000000.0;
    timings->affectedCalendarCount = changesByCalendar.size();

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

        phase.restart();
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
        timings->eventIndexMs += phase.nsecsElapsed() / 1000000.0;

        phase.restart();
        QStringList orderedUids = calendarIt.value().keys();
        std::sort(orderedUids.begin(), orderedUids.end());
        for (const QString &uid : std::as_const(orderedUids)) {
            const QJsonObject change = calendarIt.value().value(uid);
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
        timings->eventApplyMs += phase.nsecsElapsed() / 1000000.0;

        phase.restart();
        QJsonArray updatedEvents;
        for (const QString &uid : std::as_const(eventOrder)) {
            const auto it = eventMap.constFind(uid);
            if (it != eventMap.cend()) {
                updatedEvents.append(it.value());
            }
        }
        timings->eventArrayRebuildMs += phase.nsecsElapsed() / 1000000.0;

        if (updatedEvents.size() > MaxEventsPerCalendar) {
            *errorCode = QStringLiteral("EVENT_LIMIT_EXCEEDED");
            *error = QStringLiteral("calendar '%1' would contain %2 events; host limit is %3 per calendar")
                .arg(calendarId).arg(updatedEvents.size()).arg(MaxEventsPerCalendar);
            return false;
        }
        calendar.insert(QStringLiteral("events"), updatedEvents);
        calendars.replace(index, calendar);
    }
    timings->eventDiffMs = timings->eventIndexMs + timings->eventApplyMs + timings->eventArrayRebuildMs;

    if (deltas.isEmpty()) {
        timings->unchanged = true;
        timings->eventDeltaCount = 0;
        timings->journalRecordCount = journalRecordCount;
        timings->journalFileBytes = journalBytes;
        return true;
    }

    timings->eventDeltaCount = deltas.size();
    QJsonObject updatedSnapshot = cachedSnapshot;
    updatedSnapshot.insert(QStringLiteral("calendars"), calendars);
    const QString generatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    updatedSnapshot.insert(QStringLiteral("generatedAt"), generatedAt);

    phase.restart();
    const QByteArray digest = contentDigest(updatedSnapshot);
    timings->contentHashMs += phase.nsecsElapsed() / 1000000.0;
    const QString revision = QString::fromLatin1(digest.toHex());
    const QString previousRevision = currentRevision;
    timings->revision = revision;
    updatedSnapshot.insert(QStringLiteral("contentRevision"), revision);

    QJsonObject record{
        {QStringLiteral("type"), QStringLiteral("delta")},
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("previousRevision"), previousRevision},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("generatedAt"), generatedAt},
        {QStringLiteral("deltas"), deltas},
    };
    record.insert(QStringLiteral("checksum"), QString::fromLatin1(journalRecordChecksum(record)));

    phase.restart();
    const QByteArray recordLine = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
    timings->journalSerializationMs = phase.nsecsElapsed() / 1000000.0;
    timings->journalWriteBytes = recordLine.size();

    if (recordLine.size() > MaxJournalBytes) {
        *errorCode = QStringLiteral("JOURNAL_RECORD_TOO_LARGE");
        *error = QStringLiteral("event-change journal record exceeds the %1 MiB safety limit")
            .arg(MaxJournalBytes / (1024 * 1024));
        return false;
    }

    if (journalBytes + recordLine.size() > MaxJournalBytes) {
        if (journalRecordCount > 0) {
            QString compactError;
            if (!compactJournal(cachePath, journalPath, cachedSnapshot, currentRevision, timings, &compactError)) {
                *errorCode = QStringLiteral("JOURNAL_COMPACTION_FAILED");
                *error = QStringLiteral("event journal reached its safety limit and could not be compacted: %1")
                    .arg(compactError);
                return false;
            }
        }
        if (journalBytes + recordLine.size() > MaxJournalBytes) {
            *errorCode = QStringLiteral("JOURNAL_RECORD_TOO_LARGE");
            *error = QStringLiteral("event-change record plus journal header exceeds the %1 MiB recovery limit")
                .arg(MaxJournalBytes / (1024 * 1024));
            return false;
        }
    }

    phase.restart();
    double appendMs = 0.0;
    double fsyncMs = 0.0;
    QString appendError;
    if (!appendJournalRecord(journalPath, recordLine, &appendMs, &fsyncMs, &appendError)) {
        // A failed append may leave an incomplete tail. Reloading from disk on
        // the next request will replay complete records and truncate only that
        // unacknowledged tail before accepting another direct change.
        hasCachedSnapshot = false;
        cachedSnapshot = {};
        lastWrittenDigest.clear();
        journalReady = false;
        *errorCode = QStringLiteral("JOURNAL_APPEND_FAILED");
        *error = appendError;
        timings->journalAppendMs = appendMs;
        timings->journalFsyncMs = fsyncMs;
        return false;
    }
    timings->journalAppendMs = appendMs;
    timings->journalFsyncMs = fsyncMs;

    ++journalRecordCount;
    journalBytes += recordLine.size();
    timings->journalRecordCount = journalRecordCount;
    timings->journalFileBytes = journalBytes;
    timings->cacheWriteBytes = recordLine.size();

    // The journal record has been fsync'd, so it is durable before D-Bus
    // delivery and before the native-messaging acknowledgement is returned.
    lastWrittenDigest = digest;
    cachedSnapshot = updatedSnapshot;
    hasCachedSnapshot = true;

    phase.restart();
    notifyEventDeltas(deltas, previousRevision, revision);
    timings->dbusNotifyMs = phase.nsecsElapsed() / 1000000.0;

    if (journalRecordCount >= CompactJournalAfterRecords || journalBytes >= CompactJournalAfterBytes) {
        QString compactError;
        if (!compactJournal(cachePath, journalPath, cachedSnapshot, revision, timings, &compactError)) {
            // The delta is already durable in the journal and has been sent to
            // Plasma. Keep it as the recovery source and retry compaction later.
            std::cerr << "Thunderbird Plasma Calendar: journal compaction deferred: "
                      << compactError.toStdString() << "\n";
        }
    }
    timings->journalRecordCount = journalRecordCount;
    timings->journalFileBytes = journalBytes;
    timings->journalCompacted = (journalRecordCount == 0);
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
        timings.messageType = messageType;
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
