/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.ThunderbirdCalendarPlugin

Item {
    id: root

    signal configurationChanged()

    implicitWidth: 460
    implicitHeight: 470

    function saveConfig() {
        calendarModel.saveConfig()
    }

    CalendarConfigModel {
        id: calendarModel
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Choose which Thunderbird calendars appear in the Plasma calendar.")
        }

        GroupBox {
            title: qsTr("Failed calendar refreshes")
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 4

                CheckBox {
                    Layout.fillWidth: true
                    text: qsTr("Show the last synchronised events")
                    checked: calendarModel.showStaleEvents
                    onToggled: {
                        calendarModel.showStaleEvents = checked
                        root.configurationChanged()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.75
                    text: qsTr("When enabled, events from a calendar that failed to refresh remain visible using its last synchronised data. Disable this to hide those potentially outdated events. This is enabled by default.")
                }
            }
        }

        GroupBox {
            title: qsTr("Thunderbird calendars")
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    visible: calendarModel.count === 0
                    wrapMode: Text.WordWrap
                    opacity: 0.75
                    text: qsTr("No calendars have been received yet. Start Thunderbird and wait for its calendar list to synchronise, then select Refresh list.")
                }

                ListView {
                    id: calendarList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: calendarModel
                    spacing: 2

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
