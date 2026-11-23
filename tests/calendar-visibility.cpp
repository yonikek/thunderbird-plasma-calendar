/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/calendarvisibility.h"

#include <QJsonObject>

#include <cstdlib>
#include <iostream>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "Calendar visibility test failed: " << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    const QJsonObject fresh{{QStringLiteral("id"), QStringLiteral("fresh")},
                            {QStringLiteral("status"), QStringLiteral("ok")}};
    const QJsonObject staleFlag{{QStringLiteral("id"), QStringLiteral("stale-flag")},
                                {QStringLiteral("stale"), true}};
    const QJsonObject staleStatus{{QStringLiteral("id"), QStringLiteral("stale-status")},
                                  {QStringLiteral("status"), QStringLiteral("stale")}};

    require(shouldShowCalendarEvents(fresh, false), "fresh calendar should always be shown");
    require(shouldShowCalendarEvents(staleFlag, true), "stale events should be shown when the option is enabled");
    require(shouldShowCalendarEvents(staleStatus, true), "status-only stale metadata should still be shown by default");
    require(!shouldShowCalendarEvents(staleFlag, false), "stale flag should hide a failed calendar when disabled");
    require(!shouldShowCalendarEvents(staleStatus, false), "stale status should hide a failed calendar when disabled");
    require(!isStaleCalendar(fresh), "fresh calendar must not be classified as stale");

    const QSet<QString> selected{QStringLiteral("one")};
    const QSet<QString> stale;
    require(!calendarViewConfigurationChanged(selected, true, stale, true,
                                               selected, true, stale, true),
            "identical view configuration should not force a redundant rebuild");
    require(calendarViewConfigurationChanged(selected, true, stale, true,
                                              QSet<QString>{QStringLiteral("two")}, true, stale, true),
            "calendar selection changes should rebuild even when snapshot content is unchanged");
    require(calendarViewConfigurationChanged(selected, true, stale, true,
                                              selected, true, QSet<QString>{QStringLiteral("one")}, true),
            "stale-state changes should rebuild the visible calendar view");
    require(calendarViewConfigurationChanged(selected, false, stale, true,
                                              selected, true, stale, true),
            "explicit calendar-selection mode changes should rebuild the visible calendar view");
    require(calendarViewConfigurationChanged(selected, true, stale, true,
                                              selected, true, stale, false),
            "stale-visibility toggle should rebuild the visible calendar view");

    std::cout << "Calendar visibility tests passed: default visibility, filtering and config-only rebuilds.\n";
    return 0;
}
