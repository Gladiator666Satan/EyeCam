import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "views"
import "components"
import "."

// Головне вікно EyeCam: модульний макет із трьох панелей у темному стилі гласморфізму
ApplicationWindow {
    id: window
    visible: true
    width: 1380
    height: 860
    minimumWidth: 1080
    minimumHeight: 680
    title: "EyeCam Studio — Ultra Low-Latency Virtual Camera & Hardware Pipeline"
    color: Theme.bgBase

    // Фонове амбієнтне градієнтне світіння
    Rectangle {
        anchors.fill: parent
        color: Theme.bgBase

        // Радіальні світлові плями для глибини матового скла
        Rectangle {
            x: -150
            y: -150
            width: 600
            height: 600
            radius: 300
            color: Qt.rgba(0.0, 0.94, 1.0, 0.04)
        }

        Rectangle {
            x: parent.width - 450
            y: parent.height - 450
            width: 700
            height: 700
            radius: 350
            color: Qt.rgba(0.23, 0.51, 0.96, 0.04)
        }
    }

    // Верхня панель заголовка (Custom Title Bar)
    Item {
        id: titleBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 48

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 12

            // Логотип та назва
            Row {
                spacing: 8
                Rectangle {
                    width: 24
                    height: 24
                    radius: 6
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Theme.accentCyan }
                        GradientStop { position: 1.0; color: Theme.accentCobalt }
                    }
                    Text {
                        anchors.centerIn: parent
                        text: "👁"
                        font.pixelSize: 13
                    }
                }

                Text {
                    text: "EYECAM"
                    font.pixelSize: 14
                    font.weight: Font.Black
                    font.letterSpacing: 1.5
                    color: Theme.textPrimary
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    text: "PRO STUDIO"
                    font.pixelSize: 10
                    font.weight: Font.Bold
                    color: Theme.accentCyan
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Item { Layout.fillWidth: true }

            // Швидкі статуси системи
            Rectangle {
                height: 26
                width: 190
                radius: Theme.radiusPill
                color: Qt.rgba(1, 1, 1, 0.04)
                border.width: 1
                border.color: Theme.glassBorder

                Row {
                    anchors.centerIn: parent
                    spacing: 8
                    Text {
                        text: "Апаратне декодування:"
                        font.pixelSize: 10
                        color: Theme.textSecondary
                    }
                    Text {
                        text: appState.activeHwDecoderName
                        font.pixelSize: 10
                        font.weight: Font.Bold
                        color: Theme.accentCyan
                    }
                }
            }

            // Повноекранний режим
            Button {
                text: window.visibility === Window.FullScreen ? "🗗 Вікно" : "⛶ Повний екран"
                implicitWidth: 105
                implicitHeight: 28
                onClicked: {
                    if (window.visibility === Window.FullScreen) {
                        window.showNormal()
                    } else {
                        window.showFullScreen()
                    }
                }
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: parent.down ? Qt.rgba(1, 1, 1, 0.15) : Qt.rgba(1, 1, 1, 0.06)
                    border.width: 1
                    border.color: Theme.glassBorder
                }
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 10
                    font.weight: Font.Medium
                    color: Theme.textPrimary
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    // Головний робочий простір з трьома секціями (Ліва, Центр, Права)
    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 14
        spacing: 14

        // 1. Ліва панель: Менеджер підключень та телеметрія
        LeftSidebar {
            id: leftSidebar
        }

        // 2. Центральна область: Відео-в'юпорт низької затримки з трансформаціями
        CenterViewport {
            id: centerViewport
        }

        // 3. Права панель: Інспектор параметрів камери (Auto / Pro)
        RightInspector {
            id: rightInspector
        }
    }

    // Гарячі клавіші для швидкого керування
    Shortcut {
        sequence: "F"
        onActivated: {
            if (window.visibility === Window.FullScreen) {
                window.showNormal()
            } else {
                window.showFullScreen()
            }
        }
    }
    Shortcut {
        sequence: "H"
        onActivated: appState.flipH = !appState.flipH
    }
    Shortcut {
        sequence: "V"
        onActivated: appState.flipV = !appState.flipV
    }
    Shortcut {
        sequence: "R"
        onActivated: appState.rotationAngle = (appState.rotationAngle + 90) % 360
    }
}
