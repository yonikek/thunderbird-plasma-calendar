/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.ThunderbirdCalendarPlugin

Item {
    id: root

    signal configurationChanged()

    implicitWidth: 440
    implicitHeight: 340

    function saveConfig() {
        calendarModel.saveConfig()
    }

    CalendarConfigModel {
        id: calendarModel
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Choose which Thunderbird calendars appear in the Plasma calendar.")
        }

        Label {
            Layout.fillWidth: true
            visible: calendarModel.count === 0
            wrapMode: Text.WordWrap
            text: qsTr("No calendars have been received yet. Start Thunderbird and wait for its calendar list to synchronise, then reopen these settings.")
        }

        ListView {
            id: calendarList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: calendarModel
            delegate: CheckBox {
                required property string calendarId
                required property string calendarName
                required property bool calendarEnabled
                width: ListView.view.width
                text: calendarName
                checked: calendarEnabled
                onToggled: {
                    calendarModel.setEnabled(calendarId, checked)
                    root.configurationChanged()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Button {
                text: qsTr("Refresh list")
                icon.name: "view-refresh"
                onClicked: calendarModel.reload()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: qsTr("Changes apply when you press Apply.")
                opacity: 0.7
            }
        }
    }
}
