/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <CalendarEvents/CalendarEventsPlugin>
#include "cachefingerprinttracker.h"

#include <QElapsedTimer>
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
    void publishCurrentRange();
    void applyCalendarSelectionChanges();
    void reconcileFromSnapshot(const QString &reason);
    bool isCalendarSelected(const QString &calendarId) const;

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
};
