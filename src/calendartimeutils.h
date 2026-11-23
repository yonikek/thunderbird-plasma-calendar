/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QDate>
#include <QDateTime>
#include <QTime>
#include <QRegularExpression>
#include <QString>
#include <QTimeZone>

namespace ThunderbirdCalendar
{
// Thunderbird serialises floating iCalendar DATE-TIME values without a zone
// suffix and marks them separately. They represent local wall-clock fields,
// not UTC instants, so keep their fields and attach the machine's local time
// specification. Values with an explicit timezone are instants and are
// converted to local time for Plasma's calendar view.
inline QDateTime parseDateTime(const QString &value, bool allDay, bool floating)
{
    if (allDay) {
        const QDate date = QDate::fromString(value.left(10), Qt::ISODate);
        return date.isValid() ? date.startOfDay() : QDateTime();
    }

    if (floating) {
        // Parse floating values strictly. QTime's ISO parser accepts 24:00:00
        // as the ISO 8601 end-of-day notation on some Qt versions, but a
        // floating DATE-TIME emitted by Thunderbird must contain a clock
        // time in the range 00:00:00..23:59:59.999. Accepting 24:00 here can
        // silently roll the value into the next day and change the event.
        //
        // Thunderbird's serializer emits YYYY-MM-DDTHH:mm:ss with an optional
        // 1-3 digit fractional second. Match that grammar before constructing
        // QDate/QTime so Qt's intentionally permissive ISO parser cannot
        // normalise out-of-range fields or accept trailing timezone syntax.
        static const QRegularExpression floatingDateTimePattern(
            QStringLiteral(R"(^([0-9]{4}-[0-9]{2}-[0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})(?:\.([0-9]{1,3}))?$)"));
        const auto match = floatingDateTimePattern.match(value);
        if (!match.hasMatch()) {
            return {};
        }

        const QDate date = QDate::fromString(match.captured(1), Qt::ISODate);
        const int hour = match.captured(2).toInt();
        const int minute = match.captured(3).toInt();
        const int second = match.captured(4).toInt();
        const int millisecond = match.captured(5).isEmpty()
            ? 0
            : match.captured(5).leftJustified(3, QLatin1Char('0')).toInt();
        if (!date.isValid() || hour < 0 || hour > 23 || minute < 0 || minute > 59
            || second < 0 || second > 59 || millisecond < 0 || millisecond > 999) {
            return {};
        }

        const QTime time(hour, minute, second, millisecond);
        if (!time.isValid()) {
            return {};
        }

        // A floating DATE-TIME is a wall-clock value, not a UTC instant.
        // Attach the system timezone so Plasma receives the same clock fields
        // as Thunderbird. Reject nonexistent local times in a DST spring gap
        // rather than allowing QDateTime to silently shift them forward.
        const QTimeZone systemZone = QTimeZone::systemTimeZone();
        if (!systemZone.isValid()) {
            return {};
        }
        const QDateTime local(date, time, systemZone);
        if (!local.isValid() || local.date() != date || local.time() != time) {
            return {};
        }
        return local;
    }

    QDateTime result = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!result.isValid()) {
        result = QDateTime::fromString(value, Qt::ISODate);
    }
    return result.toLocalTime();
}
}
