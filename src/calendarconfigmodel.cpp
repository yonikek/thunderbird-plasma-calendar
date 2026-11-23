/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "calendarconfigmodel.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <utility>

namespace
{
QString snapshotPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/plasma-thunderbird-calendar/events.json");
}

void notifyConfigurationChanged()
{
    QDBusConnection::sessionBus().send(QDBusMessage::createSignal(
        QStringLiteral("/org/kde/ThunderbirdCalendar"), QStringLiteral("org.kde.ThunderbirdCalendar"),
        QStringLiteral("ConfigurationChanged")));
}
}

CalendarConfigModel::CalendarConfigModel(QObject *parent)
    : QAbstractListModel(parent)
{
    reload();
}

int CalendarConfigModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_calendars.size();
}

int CalendarConfigModel::count() const
{
    return m_calendars.size();
}

bool CalendarConfigModel::showStaleEvents() const
{
    return m_showStaleEvents;
}

void CalendarConfigModel::setShowStaleEvents(bool show)
{
    if (m_showStaleEvents == show) {
        return;
    }
    m_showStaleEvents = show;
    Q_EMIT showStaleEventsChanged();
}

QVariant CalendarConfigModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_calendars.size()) {
        return {};
    }
    const Calendar &calendar = m_calendars.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return calendar.name;
    case CalendarIdRole:
        return calendar.id;
    case CheckedRole:
        return calendar.checked;
    default:
        return {};
    }
}

QHash<int, QByteArray> CalendarConfigModel::roleNames() const
{
    return {
        {CalendarIdRole, "calendarId"},
        {NameRole, "calendarName"},
        {CheckedRole, "calendarEnabled"},
    };
}

void CalendarConfigModel::setEnabled(const QString &calendarId, bool enabled)
{
    for (int i = 0; i < m_calendars.size(); ++i) {
        if (m_calendars[i].id == calendarId && m_calendars[i].checked != enabled) {
            m_calendars[i].checked = enabled;
            Q_EMIT dataChanged(index(i), index(i), {CheckedRole});
            return;
        }
    }
}

void CalendarConfigModel::saveConfig()
{
    QStringList selected;
    for (const Calendar &calendar : std::as_const(m_calendars)) {
        if (calendar.checked) {
            selected.push_back(calendar.id);
        }
    }
    KConfigGroup group = KSharedConfig::openConfig()->group(QStringLiteral("ThunderbirdCalendarPlugin"));
    group.writeEntry(QStringLiteral("calendars"), selected);
    group.writeEntry(QStringLiteral("showStaleEvents"), m_showStaleEvents);
    group.sync();
    notifyConfigurationChanged();
}

void CalendarConfigModel::reload()
{
    beginResetModel();
    m_calendars.clear();

    const KConfigGroup group = KSharedConfig::openConfig()->group(QStringLiteral("ThunderbirdCalendarPlugin"));
    const bool showStaleEvents = group.readEntry(QStringLiteral("showStaleEvents"), true);
    const bool staleSettingChanged = m_showStaleEvents != showStaleEvents;
    m_showStaleEvents = showStaleEvents;
    const bool explicitSelection = group.hasKey(QStringLiteral("calendars"));
    const QStringList selected = group.readEntry(QStringLiteral("calendars"), QStringList());

    QFile file(snapshotPath());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (document.isObject()) {
            const QJsonArray calendars = document.object().value(QStringLiteral("calendars")).toArray();
            for (const QJsonValue &value : calendars) {
                const QJsonObject object = value.toObject();
                Calendar calendar;
                calendar.id = object.value(QStringLiteral("id")).toString();
                calendar.name = object.value(QStringLiteral("name")).toString(calendar.id);
                calendar.checked = !explicitSelection || selected.contains(calendar.id);
                if (!calendar.id.isEmpty()) {
                    m_calendars.push_back(calendar);
                }
            }
        }
    }
    endResetModel();
    Q_EMIT countChanged();
    if (staleSettingChanged) {
        Q_EMIT showStaleEventsChanged();
    }
}
