/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/calendartimeutils.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QTime>
#include <QTimeZone>
#include <QString>

#include <cstdlib>
#include <iostream>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "Floating-time test failed: " << message << '\n';
        std::exit(1);
    }
}

void requireWallTime(const QDateTime &value, const QDate &date, const QTime &time, const char *message)
{
    require(value.isValid(), message);
    require(value.date() == date && value.time() == time, message);
    require(value.timeSpec() == Qt::TimeZone
                && value.timeZone().id() == QTimeZone::systemTimeZone().id(),
            "floating datetime must use the system timezone semantics");
}
}

int main()
{
    const QByteArray timezone = qgetenv("TZ");
    QTime expectedAbsoluteLocalTime;
    if (timezone == "Europe/London") {
        // 2026-06-01 is BST (UTC+1).
        expectedAbsoluteLocalTime = QTime(11, 34, 56, 125);
    } else if (timezone == "America/New_York") {
        // 2026-06-01 is EDT (UTC-4).
        expectedAbsoluteLocalTime = QTime(6, 34, 56, 125);
    } else if (timezone == "Asia/Tokyo") {
        // Japan has no daylight-saving transition (UTC+9).
        expectedAbsoluteLocalTime = QTime(19, 34, 56, 125);
    } else {
        std::cerr << "Floating-time test requires TZ=Europe/London, America/New_York or Asia/Tokyo\n";
        return 2;
    }

    // The same floating wall-clock value remains unchanged in different
    // viewer timezones, whereas an absolute instant is converted to a
    // different local clock time in each timezone.
    const QDateTime floatingInViewerZone = ThunderbirdCalendar::parseDateTime(
        QStringLiteral("2026-06-01T12:34:56.125"), false, true);
    requireWallTime(floatingInViewerZone, QDate(2026, 6, 1), QTime(12, 34, 56, 125),
                    "floating wall time changed in the viewer timezone");

    const QDateTime absolute = ThunderbirdCalendar::parseDateTime(
        QStringLiteral("2026-06-01T12:34:56.125+02:00"), false, false);
    require(absolute.isValid() && absolute.date() == QDate(2026, 6, 1)
                && absolute.time() == expectedAbsoluteLocalTime,
            "timezone-qualified instant did not convert to the viewer's local timezone");

    // The spring-forward transition occurs on different dates and skips a
    // different clock hour in London and New York. A floating time inside the
    // missing local hour must be rejected rather than silently shifted.
    QDate springDate;
    QTime beforeSpringTime;
    QTime skippedSpringTime;
    QTime afterSpringTime;
    QDate spanningStartDate;
    QTime spanningStartTime;
    QDate spanningEndDate;
    QTime spanningEndTime;
    QDate autumnDate;
    QTime repeatedAutumnTime;

    if (timezone == "Europe/London") {
        springDate = QDate(2026, 3, 29);
        beforeSpringTime = QTime(0, 30);
        skippedSpringTime = QTime(1, 30);
        afterSpringTime = QTime(3, 30);
        spanningStartDate = QDate(2026, 3, 28);
        spanningStartTime = QTime(23, 30);
        spanningEndDate = springDate;
        spanningEndTime = afterSpringTime;
        autumnDate = QDate(2026, 10, 25);
        repeatedAutumnTime = QTime(1, 30);
    } else if (timezone == "America/New_York") {
        springDate = QDate(2026, 3, 8);
        beforeSpringTime = QTime(1, 30);
        skippedSpringTime = QTime(2, 30);
        afterSpringTime = QTime(3, 30);
        spanningStartDate = QDate(2026, 3, 7);
        spanningStartTime = QTime(23, 30);
        spanningEndDate = springDate;
        spanningEndTime = afterSpringTime;
        autumnDate = QDate(2026, 11, 1);
        repeatedAutumnTime = QTime(1, 30);
    }

    if (springDate.isValid()) {
        const QDateTime beforeSpring = ThunderbirdCalendar::parseDateTime(
            QStringLiteral("%1T%2").arg(springDate.toString(Qt::ISODate), beforeSpringTime.toString(Qt::ISODate)),
            false, true);
        const QDateTime afterSpring = ThunderbirdCalendar::parseDateTime(
            QStringLiteral("%1T%2").arg(springDate.toString(Qt::ISODate), afterSpringTime.toString(Qt::ISODate)),
            false, true);
        const QDateTime skipped = ThunderbirdCalendar::parseDateTime(
            QStringLiteral("%1T%2").arg(springDate.toString(Qt::ISODate), skippedSpringTime.toString(Qt::ISODate)),
            false, true);
        requireWallTime(beforeSpring, springDate, beforeSpringTime, "pre-transition floating time shifted");
        requireWallTime(afterSpring, springDate, afterSpringTime, "post-transition floating time shifted");
        require(!skipped.isValid(), "nonexistent spring-forward wall time must be rejected, not shifted");

        const QDateTime spanningStart = ThunderbirdCalendar::parseDateTime(
            QStringLiteral("%1T%2").arg(spanningStartDate.toString(Qt::ISODate), spanningStartTime.toString(Qt::ISODate)),
            false, true);
        const QDateTime spanningEnd = ThunderbirdCalendar::parseDateTime(
            QStringLiteral("%1T%2").arg(spanningEndDate.toString(Qt::ISODate), spanningEndTime.toString(Qt::ISODate)),
            false, true);
        requireWallTime(spanningStart, spanningStartDate, spanningStartTime,
                        "multi-day floating event start shifted across DST");
        requireWallTime(spanningEnd, spanningEndDate, spanningEndTime,
                        "multi-day floating event end shifted across DST");

        // The autumn transition repeats an hour. The instant selected by Qt
        // may be either side of the transition, but the displayed clock fields
        // must remain the original floating wall-clock value.
        const QDateTime repeatedAutumn = ThunderbirdCalendar::parseDateTime(
            QStringLiteral("%1T%2").arg(autumnDate.toString(Qt::ISODate), repeatedAutumnTime.toString(Qt::ISODate)),
            false, true);
        requireWallTime(repeatedAutumn, autumnDate, repeatedAutumnTime,
                        "repeated autumn floating time changed its wall-clock fields");
    }

    const QDateTime fractional = ThunderbirdCalendar::parseDateTime(QStringLiteral("2026-06-01T12:34:56.125"), false, true);
    requireWallTime(fractional, QDate(2026, 6, 1), QTime(12, 34, 56, 125),
                    "fractional floating seconds were lost");

    require(!ThunderbirdCalendar::parseDateTime(QStringLiteral("not-a-date-time"), false, true).isValid(),
            "malformed floating date-time should be rejected");
    require(!ThunderbirdCalendar::parseDateTime(QStringLiteral("2026-02-30T09:00:00"), false, true).isValid(),
            "impossible floating calendar date should be rejected");
    require(!ThunderbirdCalendar::parseDateTime(QStringLiteral("2026-01-01T24:00:00"), false, true).isValid(),
            "24:00:00 end-of-day notation must not be accepted as a floating clock time");
    require(!ThunderbirdCalendar::parseDateTime(QStringLiteral("2026-01-01T24:00:01"), false, true).isValid(),
            "hour 24 with non-zero smaller fields must be rejected");
    require(!ThunderbirdCalendar::parseDateTime(QStringLiteral("2026-01-01T23:60:00"), false, true).isValid(),
            "out-of-range floating minute must be rejected");
    require(!ThunderbirdCalendar::parseDateTime(QStringLiteral("2026-01-01T23:59:60"), false, true).isValid(),
            "out-of-range floating second must be rejected");
    require(!ThunderbirdCalendar::parseDateTime(QStringLiteral("2026-01-01T12:34:56.1234"), false, true).isValid(),
            "floating fractional seconds longer than milliseconds must be rejected");
    require(!ThunderbirdCalendar::parseDateTime(QStringLiteral("2026-01-01T12:34:56Z"), false, true).isValid(),
            "floating date-time must not accept an explicit UTC suffix");
    require(!ThunderbirdCalendar::parseDateTime(QStringLiteral("2026-01-01T12:34:56+01:00"), false, true).isValid(),
            "floating date-time must not accept an explicit offset suffix");
    require(!ThunderbirdCalendar::parseDateTime(QStringLiteral("2026-01-01T12:34"), false, true).isValid(),
            "floating date-time without seconds must be rejected");

    const QDateTime finalClockSecond = ThunderbirdCalendar::parseDateTime(
        QStringLiteral("2026-01-01T23:59:59.999"), false, true);
    require(finalClockSecond.isValid() && finalClockSecond.date() == QDate(2026, 1, 1)
                && finalClockSecond.time() == QTime(23, 59, 59, 999),
            "last representable clock millisecond must remain valid");

    const QDateTime tenths = ThunderbirdCalendar::parseDateTime(
        QStringLiteral("2026-01-01T12:34:56.1"), false, true);
    requireWallTime(tenths, QDate(2026, 1, 1), QTime(12, 34, 56, 100),
                    "one fractional digit must be interpreted as tenths of a second");
    const QDateTime hundredths = ThunderbirdCalendar::parseDateTime(
        QStringLiteral("2026-01-01T12:34:56.12"), false, true);
    requireWallTime(hundredths, QDate(2026, 1, 1), QTime(12, 34, 56, 120),
                    "two fractional digits must be interpreted as hundredths of a second");
    const QDateTime wholeSecond = ThunderbirdCalendar::parseDateTime(
        QStringLiteral("2026-01-01T12:34:56"), false, true);
    requireWallTime(wholeSecond, QDate(2026, 1, 1), QTime(12, 34, 56, 0),
                    "floating whole-second value must remain valid");

    const QDateTime allDay = ThunderbirdCalendar::parseDateTime(QStringLiteral("2026-05-10"), true, false);
    require(allDay.isValid() && allDay.date() == QDate(2026, 5, 10) && allDay.time() == QTime(0, 0),
            "all-day DATE value changed during floating-time handling");

    std::cout << "Floating-time tests passed in " << timezone.constData()
              << ": wall-clock preservation, absolute instant conversion, DST gap/repetition, "
                 "multi-day transition, fractional seconds, invalid fields and all-day DATE.\n";
    return 0;
}
