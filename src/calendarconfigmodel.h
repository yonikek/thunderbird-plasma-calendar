/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QAbstractListModel>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class CalendarConfigModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool showStaleEvents READ showStaleEvents WRITE setShowStaleEvents NOTIFY showStaleEventsChanged)

public:
    enum Roles {
        CalendarIdRole = Qt::UserRole + 1,
        NameRole,
        CheckedRole,
    };

    explicit CalendarConfigModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool showStaleEvents() const;
    void setShowStaleEvents(bool show);
    Q_INVOKABLE void setEnabled(const QString &calendarId, bool enabled);
    Q_INVOKABLE void saveConfig();
    Q_INVOKABLE void reload();

Q_SIGNALS:
    void countChanged();
    void showStaleEventsChanged();

private:
    struct Calendar {
        QString id;
        QString name;
        bool checked = true;
    };
    QVector<Calendar> m_calendars;
    bool m_showStaleEvents = true;
};
