/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <CalendarEvents/CalendarEventsPlugin>
#include "cachefingerprinttracker.h"

#include <QDate>
#include <QElapsedTimer>
#include <QMultiHash>
#include <QSet>
#include <QString>
#include <QTimer>

class ThunderbirdCalendarEventsPlugin final : public CalendarEvents::CalendarEventsPlugin
{
    Q_OBJECT
    Q_INTERFACES(CalendarEvents::CalendarEventsPlugin)
    Q_PLUGIN_METADATA(IID "org.kde.CalendarEventsPlugin" FILE "thunderbirdcalendarplugin.json")

public:
    explicit ThunderbirdCalendarEventsPlugin(QObject *parent = nullptr);
    void loadEventsForDateRange(const QDate &startDate, const QDate &endDate) override;

private Q_SLOTS:
    void onEventsChanged();
    void onEventsDelta(const QString &json);
    void onConfigurationChanged();
    void onCachePoll();

private:
    // Measures time spent inside each synchronous Qt signal emission. This
    // includes work performed by direct-connected receivers (not work queued
    // for later delivery).
    struct SignalEmissionTimings
    {
        quint64 eventRemovedCount = 0;
        qint64 eventRemovedTotalNs = 0;
        qint64 eventRemovedMaxNs = 0;
        quint64 eventRemovedMaxIndex = 0;

        quint64 eventModifiedCount = 0;
        quint64 eventModifiedStartedCount = 0;
        qint64 eventModifiedTotalNs = 0;
        qint64 eventModifiedMaxNs = 0;
        quint64 eventModifiedMaxIndex = 0;

        quint64 dataReadyCalls = 0;
        quint64 dataReadyStartedCalls = 0;
        quint64 dataReadyEvents = 0;
        qint64 dataReadyTotalNs = 0;
        qint64 dataReadyMaxNs = 0;
        quint64 dataReadyMaxCallIndex = 0;
    };

    void publishCurrentRange();
    void applyCalendarSelectionChanges();
    void reconcileFromSnapshot(const QString &reason);
    bool isCalendarSelected(const QString &calendarId) const;
    void emitEventRemoved(const QString &uid, const QString &source,
                          quint64 &operationCount, SignalEmissionTimings &timings,
                          quint64 expectedCount = 0);
    void emitEventModified(const CalendarEvents::EventData &event,
                           SignalEmissionTimings &timings);
    void emitDataReady(const QMultiHash<QDate, CalendarEvents::EventData> &events,
                       SignalEmissionTimings &timings);
    void logSignalEmissionTimings(const QString &source,
                                  const SignalEmissionTimings &timings) const;

    QDate m_startDate;
    QDate m_endDate;
    QSet<QString> m_selectedCalendarIds;
    QSet<QString> m_staleCalendarIds;
    // Keep only the UIDs currently published to Plasma, not a second copy of
    // EventData. This small index allows an authoritative snapshot/journal
    // reconciliation after a missed D-Bus revision without duplicating events.
    QSet<QString> m_publishedEventUids;
    CacheFingerprintTracker m_cacheFingerprintTracker;
    QElapsedTimer m_cacheRetryClock;
    QTimer m_cachePollTimer;
    bool m_hasExplicitSelection = false;
    bool m_selectionInitialized = false;
    bool m_showStaleEvents = true;
    QString m_loadedRevision;
    QString m_pendingRevision;
    QString m_pendingPreviousRevision;
    QSet<int> m_receivedDeltaBatches;
    int m_pendingBatchCount = 0;
    quint64 m_pendingRevisionEventRemovedSignals = 0;
    quint64 m_pendingRevisionEventModifiedSignals = 0;
    quint64 m_pendingRevisionDataReadyEvents = 0;
};
