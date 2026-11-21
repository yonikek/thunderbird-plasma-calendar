/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal Mozilla native-messaging host for the Thunderbird / Plasma bridge.
 * It accepts snapshots only; it cannot execute commands or choose file paths.
 */
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
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

namespace
{
constexpr quint32 MaxMessageBytes = 32U * 1024U * 1024U;
constexpr int MaxCalendars = 512;
constexpr int MaxEventsPerCalendar = 100000;
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
    qint64 cacheWriteBytes = 0;
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
            {QStringLiteral("cacheWriteBytes"), static_cast<double>(cacheWriteBytes)},
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
        const int eventCount = calendar.value(QStringLiteral("events")).toArray().size();
        if (eventCount > MaxEventsPerCalendar) {
            *why = QStringLiteral("calendar '%1' contains %2 events; host limit is %3 per calendar")
                .arg(calendar.value(QStringLiteral("name")).toString(calendar.value(QStringLiteral("id")).toString()))
                .arg(eventCount)
                .arg(MaxEventsPerCalendar);
            return false;
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
    QDBusConnection::sessionBus().send(signal);
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
    return QCryptographicHash::hash(QJsonDocument(snapshot).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}

QByteArray digestOfFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (document.isNull() || !document.isObject()) {
        return {};
    }
    return contentDigest(document.object());
}

QByteArray lastWrittenDigest;

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
    phase.restart();
    if (lastWrittenDigest.isEmpty()) {
        lastWrittenDigest = digestOfFile(cachePath);
    }
    timings->existingCacheReadMs = phase.nsecsElapsed() / 1000000.0;
    timings->cachePreparationMs = timings->cacheDirectoryMs + timings->existingCacheReadMs;

    // Skip the rewrite and D-Bus signal when only timestamps or the content is unchanged.
    // Plasma re-parses the whole file on every signal.
    if (digest == lastWrittenDigest && QFile::exists(cachePath)) {
        timings->unchanged = true;
        return true;
    }

    phase.restart();
    const QByteArray data = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
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

    phase.restart();
    notifyPlasma();
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
        const bool ok = acceptSnapshot(request, &error, &errorCode, &timings);
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
