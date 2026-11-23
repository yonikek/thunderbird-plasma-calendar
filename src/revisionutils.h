/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace ThunderbirdCalendar
{
// Version 2 journal revisions are a hash chain. A full snapshot digest anchors
// the chain; each later revision hashes only the previous revision and the
// compact JSON delta array. The domain separator prevents this digest from
// being confused with a snapshot-content digest or journal-record checksum.
inline constexpr auto DeltaRevisionAlgorithm = "delta-chain-v1";
inline constexpr int DeltaJournalSchemaVersion = 2;

inline QByteArray snapshotContentDigest(QJsonObject snapshot)
{
    snapshot.remove(QStringLiteral("generatedAt"));
    snapshot.remove(QStringLiteral("rangeStart"));
    snapshot.remove(QStringLiteral("rangeEnd"));
    snapshot.remove(QStringLiteral("contentRevision"));
    snapshot.remove(QStringLiteral("revisionAlgorithm"));
    snapshot.remove(QStringLiteral("snapshotDigest"));
    snapshot.remove(QStringLiteral("revisionCheckpoint"));
    return QCryptographicHash::hash(
        QJsonDocument(snapshot).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}

inline QString snapshotContentRevision(const QJsonObject &snapshot)
{
    return QString::fromLatin1(snapshotContentDigest(snapshot).toHex());
}

inline QString nextDeltaRevision(const QString &previousRevision, const QJsonArray &deltas)
{
    QByteArray input = QByteArrayLiteral("thunderbird-plasma-calendar:delta-chain:v1\n");
    input += previousRevision.toLatin1();
    input += '\n';
    input += QJsonDocument(deltas).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

inline bool isSha256Hex(const QString &value)
{
    if (value.size() != 64) {
        return false;
    }
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline QString snapshotRevisionCheckpoint(const QString &snapshotDigest, const QString &revision)
{
    QByteArray input = QByteArrayLiteral("thunderbird-plasma-calendar:snapshot-checkpoint:v1\n");
    input += snapshotDigest.toLatin1();
    input += '\n';
    input += revision.toLatin1();
    return QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

inline bool snapshotRevisionCheckpointMatches(const QJsonObject &snapshot, const QString &actualSnapshotDigest)
{
    const QString storedSnapshotDigest = snapshot.value(QStringLiteral("snapshotDigest")).toString();
    const QString revision = snapshot.value(QStringLiteral("contentRevision")).toString();
    return storedSnapshotDigest == actualSnapshotDigest
        && isSha256Hex(revision)
        && snapshot.value(QStringLiteral("revisionCheckpoint")).toString()
            == snapshotRevisionCheckpoint(storedSnapshotDigest, revision);
}

inline bool usesDeltaRevisionChain(const QJsonObject &snapshot)
{
    return snapshot.value(QStringLiteral("revisionAlgorithm")).toString()
        == QLatin1String(DeltaRevisionAlgorithm);
}

} // namespace ThunderbirdCalendar
