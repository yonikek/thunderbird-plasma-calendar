/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QJsonObject>
#include <QSet>

inline bool isStaleCalendar(const QJsonObject &calendar)
{
    return calendar.value(QStringLiteral("stale")).toBool(false)
        || calendar.value(QStringLiteral("status")).toString() == QStringLiteral("stale");
}

inline bool shouldShowCalendarEvents(const QJsonObject &calendar, bool showStaleEvents)
{
    return showStaleEvents || !isStaleCalendar(calendar);
}

inline bool calendarViewConfigurationChanged(
    const QSet<QString> &oldSelected,
    bool oldHasExplicitSelection,
    const QSet<QString> &oldStaleIds,
    bool oldShowStaleEvents,
    const QSet<QString> &newSelected,
    bool newHasExplicitSelection,
    const QSet<QString> &newStaleIds,
    bool newShowStaleEvents)
{
    return oldSelected != newSelected
        || oldHasExplicitSelection != newHasExplicitSelection
        || oldStaleIds != newStaleIds
        || oldShowStaleEvents != newShowStaleEvents;
}
