/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "calendareventplugin.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTime>

namespace
{
constexpr auto BusPath = "/org/kde/ThunderbirdCalendar";
constexpr auto BusInterface = "org.kde.ThunderbirdCalendar";
constexpr auto ConfigGroupName = "ThunderbirdCalendarPlugin";

QString snapshotPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/plasma-thunderbird-calendar/events.json");
}

QDateTime parseDateTime(const QString &value, bool allDay)
{
    if (allDay) {
        const QDate date = QDate::fromString(value.left(10), Qt::ISODate);
        return date.isValid() ? date.startOfDay() : QDateTime();
    }
    QDateTime result = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!result.isValid()) {
        result = QDateTime::fromString(value, Qt::ISODate);
    }
    // Timed values arrive as UTC instants. Convert so that date() is the local
    // calendar day the clock should place the event on.
    return result.isValid() ? result.toLocalTime() : result;
}

// Last calendar day an event occupies. Both iCal all-day DTEND values and
// timed events ending exactly at midnight are exclusive.
QDate lastDayOf(const QDateTime &start, const QDateTime &end, bool allDay)
{
    QDate last = end.date();
    if (end > start && (allDay || end.time() == QTime(0, 0))) {
        last = last.addDays(-1);
    }
    return last < start.date() ? start.date() : last;
}

QStringList enabledCalendars(bool *hasExplicitSelection)
{
    const KConfigGroup group = KSharedConfig::openConfig()->group(QString::fromLatin1(ConfigGroupName));
    *hasExplicitSelection = group.hasKey(QStringLiteral("calendars"));
    return group.readEntry(QStringLiteral("calendars"), QStringList());
}

}

ThunderbirdCalendarEventsPlugin::ThunderbirdCalendarEventsPlugin(QObject *parent)
    : CalendarEvents::CalendarEventsPlugin(parent)
{
    auto bus = QDBusConnection::sessionBus();
    bus.connect(QString(), QString::fromLatin1(BusPath), QString::fromLatin1(BusInterface),
                QStringLiteral("EventsChanged"), this, SLOT(onEventsChanged()));
    bus.connect(QString(), QString::fromLatin1(BusPath), QString::fromLatin1(BusInterface),
                QStringLiteral("ConfigurationChanged"), this, SLOT(onConfigurationChanged()));
}

void ThunderbirdCalendarEventsPlugin::loadEventsForDateRange(const QDate &startDate, const QDate &endDate)
{
    m_startDate = startDate;
    m_endDate = endDate;
    publishCurrentRange();
}

void ThunderbirdCalendarEventsPlugin::onEventsChanged()
{
    if (m_startDate.isValid() && m_endDate.isValid()) {
        publishCurrentRange();
    }
}

void ThunderbirdCalendarEventsPlugin::onConfigurationChanged()
{
    if (m_startDate.isValid() && m_endDate.isValid()) {
        publishCurrentRange();
    }
}

void ThunderbirdCalendarEventsPlugin::publishCurrentRange()
{
    QMultiHash<QDate, CalendarEvents::EventData> result;
    QFile file(snapshotPath());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (document.isObject()) {
            const QJsonArray calendars = document.object().value(QStringLiteral("calendars")).toArray();
            bool explicitSelection = false;
            const QStringList enabled = enabledCalendars(&explicitSelection);

            for (const QJsonValue &calendarValue : calendars) {
                const QJsonObject calendar = calendarValue.toObject();
                const QString calendarId = calendar.value(QStringLiteral("id")).toString();
                if (explicitSelection && !enabled.contains(calendarId)) {
                    continue;
                }
                const QString color = calendar.value(QStringLiteral("color")).toString();
                const QJsonArray events = calendar.value(QStringLiteral("events")).toArray();
                for (const QJsonValue &eventValue : events) {
                    const QJsonObject source = eventValue.toObject();
                    const bool allDay = source.value(QStringLiteral("allDay")).toBool(false);
                    const QDateTime start = parseDateTime(source.value(QStringLiteral("start")).toString(), allDay);
                    const QDateTime end = parseDateTime(source.value(QStringLiteral("end")).toString(), allDay);
                    if (!start.isValid() || !end.isValid()) {
                        continue;
                    }
                    const QDate firstDay = start.date();
                    const QDate lastDay = lastDayOf(start, end, allDay);
                    if (lastDay < m_startDate || firstDay > m_endDate) {
                        continue;
                    }

                    CalendarEvents::EventData event;
                    event.setUid(source.value(QStringLiteral("uid")).toString());
                    event.setTitle(source.value(QStringLiteral("title")).toString());
                    event.setDescription(source.value(QStringLiteral("description")).toString());
                    event.setStartDateTime(start);
                    event.setEndDateTime(end);
                    event.setIsAllDay(allDay);
                    event.setEventColor(source.value(QStringLiteral("color")).toString(color));
                    const QString type = source.value(QStringLiteral("kind")).toString();
                    event.setEventType(type == QStringLiteral("todo")
                                           ? CalendarEvents::EventData::Todo
                                           : CalendarEvents::EventData::Event);
                    // Publish the event on every visible day it spans.
                    const QDate from = qMax(firstDay, m_startDate);
                    const QDate to = qMin(lastDay, m_endDate);
                    for (QDate day = from; day <= to; day = day.addDays(1)) {
                        result.insert(day, event);
                    }
                }
            }
        }
    }
    Q_EMIT dataReady(result);
}
