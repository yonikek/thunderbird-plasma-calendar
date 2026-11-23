/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/revisionutils.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cstdlib>
#include <iostream>

namespace
{
void require(bool condition, const QString &message)
{
    if (!condition) {
        std::cerr << "Revision-chain test failed: " << message.toStdString() << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QJsonObject event{
        {QStringLiteral("uid"), QStringLiteral("calendar:event-1")},
        {QStringLiteral("title"), QStringLiteral("Initial title")},
    };
    const QJsonArray deltas{
        QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("modify")},
            {QStringLiteral("calendarId"), QStringLiteral("calendar")},
            {QStringLiteral("event"), event},
        },
    };

    const QString anchor = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    const QString first = ThunderbirdCalendar::nextDeltaRevision(anchor, deltas);
    require(first.size() == 64, QStringLiteral("revision must be a 256-bit lowercase hexadecimal digest"));
    require(first == QStringLiteral("8a03dcbf139d55c6d1c5274918a73fd810af79ecc49a04880beafcb355998ae5"),
            QStringLiteral("revision must match the documented delta-chain-v1 test vector"));
    require(ThunderbirdCalendar::isSha256Hex(first),
            QStringLiteral("revision must be a lowercase SHA-256 hexadecimal string"));
    require(!ThunderbirdCalendar::isSha256Hex(
                QStringLiteral("G000000000000000000000000000000000000000000000000000000000000000")),
            QStringLiteral("non-hexadecimal revisions must be rejected"));
    require(first != anchor, QStringLiteral("a delta must advance the revision"));
    require(first == ThunderbirdCalendar::nextDeltaRevision(anchor, deltas),
            QStringLiteral("the same base revision and delta payload must produce the same revision"));
    require(first != ThunderbirdCalendar::nextDeltaRevision(
                QStringLiteral("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"), deltas),
            QStringLiteral("revision must depend on the previous revision"));

    QJsonObject modifiedEvent = event;
    modifiedEvent.insert(QStringLiteral("title"), QStringLiteral("Changed title"));
    const QJsonArray changedDeltas{
        QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("modify")},
            {QStringLiteral("calendarId"), QStringLiteral("calendar")},
            {QStringLiteral("event"), modifiedEvent},
        },
    };
    require(first != ThunderbirdCalendar::nextDeltaRevision(anchor, changedDeltas),
            QStringLiteral("revision must depend on delta contents"));

    const QJsonArray reorderedDeltas{
        changedDeltas.at(0), deltas.at(0),
    };
    require(ThunderbirdCalendar::nextDeltaRevision(anchor, reorderedDeltas)
                != ThunderbirdCalendar::nextDeltaRevision(anchor, QJsonArray{deltas.at(0), changedDeltas.at(0)}),
            QStringLiteral("revision must preserve delta-array ordering"));

    QJsonObject snapshot{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("generatedAt"), QStringLiteral("2026-11-23T10:00:00.000Z")},
        {QStringLiteral("rangeStart"), QStringLiteral("2026-01-01T00:00:00.000Z")},
        {QStringLiteral("rangeEnd"), QStringLiteral("2027-01-01T00:00:00.000Z")},
        {QStringLiteral("calendars"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("calendar")}}}},
    };
    const QString snapshotRevision = ThunderbirdCalendar::snapshotContentRevision(snapshot);
    snapshot.insert(QStringLiteral("contentRevision"), first);
    snapshot.insert(QStringLiteral("revisionAlgorithm"), QLatin1String(ThunderbirdCalendar::DeltaRevisionAlgorithm));
    snapshot.insert(QStringLiteral("snapshotDigest"), snapshotRevision);
    const QString checkpoint = ThunderbirdCalendar::snapshotRevisionCheckpoint(snapshotRevision, first);
    snapshot.insert(QStringLiteral("revisionCheckpoint"), checkpoint);
    require(ThunderbirdCalendar::snapshotRevisionCheckpointMatches(snapshot, snapshotRevision),
            QStringLiteral("snapshot checkpoint must bind the content digest to the chain tip"));
    QJsonObject corruptedCheckpoint = snapshot;
    corruptedCheckpoint.insert(QStringLiteral("contentRevision"), anchor);
    require(!ThunderbirdCalendar::snapshotRevisionCheckpointMatches(corruptedCheckpoint, snapshotRevision),
            QStringLiteral("a changed chain tip must be detected after journal compaction"));
    snapshot.insert(QStringLiteral("generatedAt"), QStringLiteral("2026-11-24T10:00:00.000Z"));
    snapshot.insert(QStringLiteral("rangeStart"), QStringLiteral("2026-02-01T00:00:00.000Z"));
    require(ThunderbirdCalendar::snapshotContentRevision(snapshot) == snapshotRevision,
            QStringLiteral("snapshot digest must exclude volatile range and revision metadata"));
    QJsonObject changedSnapshot = snapshot;
    QJsonArray changedCalendars = changedSnapshot.value(QStringLiteral("calendars")).toArray();
    QJsonObject changedCalendar = changedCalendars.at(0).toObject();
    changedCalendar.insert(QStringLiteral("name"), QStringLiteral("Renamed calendar"));
    changedCalendars.replace(0, changedCalendar);
    changedSnapshot.insert(QStringLiteral("calendars"), changedCalendars);
    require(ThunderbirdCalendar::snapshotContentRevision(changedSnapshot) != snapshotRevision,
            QStringLiteral("snapshot digest must change when calendar content changes"));
    require(ThunderbirdCalendar::usesDeltaRevisionChain(snapshot),
            QStringLiteral("chain snapshot metadata must be recognised"));

    std::cout << "Revision-chain tests passed: deterministic delta revisions, previous-revision chaining,"
                 " payload sensitivity, order sensitivity, snapshot metadata exclusion, content sensitivity,"
                 " and checkpoint binding.\n";
    return EXIT_SUCCESS;
}
