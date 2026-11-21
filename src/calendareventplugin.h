/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <CalendarEvents/CalendarEventsPlugin>

#include <QSet>

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

private:
    void publishCurrentRange();
    void applyCalendarSelectionChanges();
    bool isCalendarSelected(const QString &calendarId) const;

    QDate m_startDate;
    QDate m_endDate;
    QSet<QString> m_selectedCalendarIds;
    bool m_hasExplicitSelection = false;
    bool m_selectionInitialized = false;
    QString m_loadedRevision;
    QString m_pendingRevision;
    QString m_pendingPreviousRevision;
    QSet<int> m_receivedDeltaBatches;
    int m_pendingBatchCount = 0;
};
