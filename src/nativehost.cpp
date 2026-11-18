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
#include <cstring>
#include <iostream>

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

bool validateSnapshot(const QJsonObject &snapshot, QString *why)
{
    if (snapshot.value(QStringLiteral("schemaVersion")).toInt(-1) != 1) {
        *why = QStringLiteral("unsupported snapshot schemaVersion");
        return false;
    }
    const QJsonValue calendarsValue = snapshot.value(QStringLiteral("calendars"));
    if (!calendarsValue.isArray() || calendarsValue.toArray().size() > MaxCalendars) {
        *why = QStringLiteral("invalid calendars array");
        return false;
    }
    for (const QJsonValue &calendarValue : calendarsValue.toArray()) {
        if (!calendarValue.isObject()) {
            *why = QStringLiteral("calendar entry is not an object");
            return false;
        }
        const QJsonObject calendar = calendarValue.toObject();
        if (!calendar.value(QStringLiteral("id")).isString()
            || !calendar.value(QStringLiteral("events")).isArray()
            || calendar.value(QStringLiteral("events")).toArray().size() > MaxEventsPerCalendar) {
            *why = QStringLiteral("invalid calendar entry");
            return false;
        }
    }
    return true;
}

bool writeFrame(const QJsonObject &object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (static_cast<quint64>(payload.size()) > MaxMessageBytes) {
        return false;
    }
    const quint32 length = qToLittleEndian(static_cast<quint32>(payload.size()));
    std::cout.write(reinterpret_cast<const char *>(&length), sizeof(length));
    std::cout.write(payload.constData(), payload.size());
    std::cout.flush();
    return std::cout.good();
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
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    return contentDigest(document.object());
}

QByteArray lastWrittenDigest;

bool acceptSnapshot(const QJsonObject &message, QString *error)
{
    if (message.value(QStringLiteral("type")).toString() != QStringLiteral("snapshot")) {
        *error = QStringLiteral("only snapshot messages are supported");
        return false;
    }
    const QJsonValue snapshotValue = message.value(QStringLiteral("snapshot"));
    if (!snapshotValue.isObject()) {
        *error = QStringLiteral("snapshot must be an object");
        return false;
    }
    const QJsonObject snapshot = snapshotValue.toObject();
    if (!validateSnapshot(snapshot, error)) {
        return false;
    }

    const QString directory = cacheDirectory();
    if (!QDir().mkpath(directory)) {
        *error = QStringLiteral("could not create cache directory");
        return false;
    }
    const auto privateDirectory = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!QFile::setPermissions(directory, privateDirectory)) {
        *error = QStringLiteral("could not restrict cache directory permissions");
        return false;
    }

    // Skip the rewrite and the D-Bus signal when nothing but timestamps changed.
    // Plasma re-parses the whole file on every signal.
    const QString cachePath = directory + QStringLiteral("/events.json");
    const QByteArray digest = contentDigest(snapshot);
    if (lastWrittenDigest.isEmpty()) {
        lastWrittenDigest = digestOfFile(cachePath);
    }
    if (digest == lastWrittenDigest && QFile::exists(cachePath)) {
        return true;
    }

    QSaveFile file(cachePath);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }
    const auto privateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!file.setPermissions(privateFile)) {
        *error = QStringLiteral("could not restrict cache file permissions");
        file.cancelWriting();
        return false;
    }
    const QByteArray data = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    lastWrittenDigest = digest;
    notifyPlasma();
    return true;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("thunderbird-calendar-native-host"));

    while (true) {
        std::array<unsigned char, sizeof(quint32)> header{};
        std::cin.read(reinterpret_cast<char *>(header.data()), header.size());
        const std::streamsize got = std::cin.gcount();
        if (got == 0 && std::cin.eof()) {
            return 0;
        }
        if (got != static_cast<std::streamsize>(header.size())) {
            std::cerr << "Incomplete native-messaging frame header\n";
            return 2;
        }

        quint32 length = 0;
        std::memcpy(&length, header.data(), sizeof(length));
        length = qFromLittleEndian(length);
        if (length == 0) {
            std::cerr << "Invalid native-messaging frame size\n";
            return 2;
        }
        if (length > MaxMessageBytes) {
            // Discard the oversized payload so the stream stays in sync and
            // report the problem to the extension. Exiting here would make the
            // extension reconnect and resend the same oversized snapshot.
            std::array<char, 64 * 1024> scratch{};
            quint32 remaining = length;
            while (remaining > 0) {
                const auto chunk = static_cast<std::streamsize>(std::min(remaining, static_cast<quint32>(scratch.size())));
                std::cin.read(scratch.data(), chunk);
                if (std::cin.gcount() != chunk) {
                    std::cerr << "Incomplete native-messaging frame payload\n";
                    return 2;
                }
                remaining -= static_cast<quint32>(chunk);
            }
            if (!writeFrame({{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("snapshot too large")}})) {
                return 3;
            }
            continue;
        }

        QByteArray payload(static_cast<qsizetype>(length), Qt::Uninitialized);
        std::cin.read(payload.data(), static_cast<std::streamsize>(length));
        if (std::cin.gcount() != static_cast<std::streamsize>(length)) {
            std::cerr << "Incomplete native-messaging frame payload\n";
            return 2;
        }

        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            writeFrame({{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("invalid JSON object")}});
            continue;
        }

        QString error;
        const bool ok = acceptSnapshot(document.object(), &error);
        if (!writeFrame({{QStringLiteral("ok"), ok}, {QStringLiteral("error"), error}})) {
            return 3;
        }
    }
}
