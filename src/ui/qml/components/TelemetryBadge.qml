import QtQuick
import ".."

// Картка показника телеметрії реального часу зі статусною кольоровою диференціацією
Item {
    id: root

    property string label: "RTT / Ping"
    property string value: "0"
    property string unit: "ms"
    property int statusLevel: 0 // 0: Норма (Зелений), 1: Попередження (Жовтий), 2: Критично (Червоний)
    property string iconSymbol: "⚡"

    implicitWidth: 120
    implicitHeight: 64

    // Визначення акцентного кольору стану
    readonly property color statusColor: {
        switch (root.statusLevel) {
            case 0: return Theme.accentSuccess
            case 1: return Theme.accentWarning
            case 2: return Theme.accentDanger
            default: return Theme.textSecondary
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusMedium
        color: Qt.rgba(0.09, 0.12, 0.17, 0.7)
        border.width: 1
        border.color: Qt.rgba(root.statusColor.r, root.statusColor.g, root.statusColor.b, 0.25)

        Column {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 3

            // Верхній рядок: іконка та назва метрики
            Row {
                width: parent.width
                spacing: 6

                Text {
                    text: root.iconSymbol
                    font.pixelSize: 11
                    color: root.statusColor
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    text: root.label
                    font.pixelSize: 10
                    font.weight: Font.Medium
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Нижній рядок: поточне числове значення та розмірність
            Row {
                spacing: 3
                anchors.left: parent.left
                anchors.bottom: parent.bottom

                Text {
                    text: root.value
                    font.pixelSize: 15
                    font.weight: Font.Bold
                    font.family: "Monospace"
                    color: Theme.textPrimary
                    anchors.baseline: unitText.baseline
                }

                Text {
                    id: unitText
                    text: root.unit
                    font.pixelSize: 10
                    color: Theme.textTertiary
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 1
                }
            }
        }
    }
}
