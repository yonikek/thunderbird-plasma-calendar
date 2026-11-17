/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <CalendarEvents/CalendarEventsPlugin>

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
    void onConfigurationChanged();

private:
    void publishCurrentRange();
    QDate m_startDate;
    QDate m_endDate;
};
