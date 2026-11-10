import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"
import ".."

// Центральний в'юпорт низької затримки з апаратними трансформаціями та HUD
Item {
    id: root

    Layout.fillWidth: true
    Layout.fillHeight: true

    // Контейнер відеообласті з темною підкладкою
    Rectangle {
        id: viewportBg
        anchors.fill: parent
        color: "#050608"
        radius: Theme.radiusLarge
        clip: true

        // Область рендерингу відеопотоку
        Item {
            id: videoRenderContainer
            anchors.centerIn: parent
            width: parent.width - 24
            height: parent.height - 24

            // Симуляція відеопотоку / Апаратний QSGTextureProvider елемент
            Rectangle {
                id: videoSurface
                anchors.centerIn: parent
                width: parent.width
                height: parent.height
                color: "#0c1017"

                // Апаратні трансформації без додаткового кодування на GPU
                transform: [
                    Scale {
                        origin.x: videoSurface.width / 2
                        origin.y: videoSurface.height / 2
                        xScale: appState.flipH ? -1.0 : 1.0
                        yScale: appState.flipV ? -1.0 : 1.0
                    },
                    Rotation {
                        origin.x: videoSurface.width / 2
                        origin.y: videoSurface.height / 2
                        angle: appState.rotationAngle
                        Behavior on angle {
                            NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic }
                        }
                    }
                ]

                // Внутрішня градієнтна сітка та прев'ю-заглушка для рендеру
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#141b2b" }
                        GradientStop { position: 0.5; color: "#0c121d" }
                        GradientStop { position: 1.0; color: "#070a10" }
                    }

                    // Центральна графічна мітка при відсутності активного потоку
                    Column {
                        anchors.centerIn: parent
                        spacing: 12
                        visible: !appState.isConnected

                        Text {
                            text: "📷"
                            font.pixelSize: 48
                            anchors.horizontalCenter: parent.horizontalCenter
                            opacity: 0.5
                        }

                        Text {
                            text: "Очікування відеопотоку низької затримки..."
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            color: Theme.textSecondary
                            anchors.horizontalCenter: parent.horizontalCenter
                        }

                        Text {
                            text: "D3D11VA / NVDEC / VA-API нульове копіювання активне"
                            font.pixelSize: 11
                            font.family: "Monospace"
                            color: Theme.textTertiary
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                    }

                    // Анімована тестова мішень низької затримки при активному зв'язку
                    Item {
                        anchors.fill: parent
                        visible: appState.isConnected

                        // Сітка правила третин (Rule of Thirds)
                        Item {
                            anchors.fill: parent
                            visible: appState.showGridOverlay
                            opacity: 0.25

                            // Горизонтальні лінії
                            Rectangle { y: parent.height / 3; width: parent.width; height: 1; color: "#FFFFFF" }
                            Rectangle { y: (parent.height / 3) * 2; width: parent.width; height: 1; color: "#FFFFFF" }

                            // Вертикальні лінії
                            Rectangle { x: parent.width / 3; width: 1; height: parent.height; color: "#FFFFFF" }
                            Rectangle { x: (parent.width / 3) * 2; width: 1; height: parent.height; color: "#FFFFFF" }
                        }

                        // Центральне візирне перехрестя фокусу
                        Rectangle {
                            anchors.centerIn: parent
                            width: 60
                            height: 60
                            radius: 30
                            color: "transparent"
                            border.width: 1
                            border.color: Qt.rgba(0, 0.94, 1.0, 0.4)

                            Rectangle {
                                anchors.centerIn: parent
                                width: 4
                                height: 4
                                radius: 2
                                color: Theme.accentCyan
                            }
                        }
                    }
                }
            }
        }

        // Верхній плаваючий HUD (Специфікація потоку та статус Virtual Camera)
        RowLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 18
            spacing: 10

            // Плашка розширення та кодека
            Rectangle {
                height: 28
                width: streamBadgeText.implicitWidth + 20
                radius: Theme.radiusSmall
                color: Qt.rgba(0.05, 0.07, 0.1, 0.85)
                border.width: 1
                border.color: Theme.glassBorder

                Row {
                    anchors.centerIn: parent
                    spacing: 6
                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: appState.isConnected ? Theme.accentSuccess : Theme.textTertiary
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        id: streamBadgeText
                        text: appState.isConnected ? "4K UHD (3840x2160) @ 60 FPS • D3D11VA NV12" : "НЕМАЄ СИГНАЛУ"
                        font.pixelSize: 11
                        font.weight: Font.Bold
                        font.family: "Monospace"
                        color: Theme.textPrimary
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }

            Item { Layout.fillWidth: true }

            // Плашка статусу віртуальної камери Stage 3
            Rectangle {
                height: 28
                width: vcamText.implicitWidth + 24
                radius: Theme.radiusSmall
                color: appState.virtualCameraActive ? Qt.rgba(0.0, 0.94, 1.0, 0.15) : Qt.rgba(0.05, 0.07, 0.1, 0.85)
                border.width: 1
                border.color: appState.virtualCameraActive ? Theme.accentCyan : Theme.glassBorder

                Row {
                    anchors.centerIn: parent
                    spacing: 6
                    Text {
                        text: "🎥"
                        font.pixelSize: 11
                    }
                    Text {
                        id: vcamText
                        text: appState.virtualCameraActive ? "VIRTUAL CAM: АКТИВНО" : "VIRTUAL CAM: ОЧІКУВАННЯ"
                        font.pixelSize: 10
                        font.weight: Font.Bold
                        color: appState.virtualCameraActive ? Theme.accentCyan : Theme.textTertiary
                    }
                }
            }
        }

        // Нижня панель швидкого керування орієнтацією та геометрією
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            height: 44
            width: controlsRow.implicitWidth + 24
            radius: Theme.radiusPill
            color: Qt.rgba(0.08, 0.10, 0.15, 0.85)
            border.width: 1
            border.color: Theme.glassBorder

            Row {
                id: controlsRow
                anchors.centerIn: parent
                spacing: 8

                // Кнопка: Віддзеркалення по горизонталі (Flip H)
                Button {
                    text: "⇋ Flip H"
                    implicitWidth: 80
                    implicitHeight: 30
                    checkable: true
                    checked: appState.flipH
                    onClicked: appState.flipH = !appState.flipH
                    background: Rectangle {
                        radius: Theme.radiusPill
                        color: parent.checked ? Qt.rgba(0, 0.94, 1.0, 0.25) : (parent.hovered ? Qt.rgba(1, 1, 1, 0.1) : "transparent")
                        border.width: parent.checked ? 1 : 0
                        border.color: Theme.accentCyan
                    }
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        color: parent.checked ? Theme.accentCyan : Theme.textPrimary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                // Кнопка: Віддзеркалення по вертикалі (Flip V)
                Button {
                    text: "⇅ Flip V"
                    implicitWidth: 80
                    implicitHeight: 30
                    checkable: true
                    checked: appState.flipV
                    onClicked: appState.flipV = !appState.flipV
                    background: Rectangle {
                        radius: Theme.radiusPill
                        color: parent.checked ? Qt.rgba(0, 0.94, 1.0, 0.25) : (parent.hovered ? Qt.rgba(1, 1, 1, 0.1) : "transparent")
                        border.width: parent.checked ? 1 : 0
                        border.color: Theme.accentCyan
                    }
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        color: parent.checked ? Theme.accentCyan : Theme.textPrimary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                // Роздільник
                Rectangle {
                    width: 1
                    height: 18
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.glassBorder
                }

                // Кнопка: Поворот на 90 градусів за годинниковою стрілкою
                Button {
                    text: "↻ 90°"
                    implicitWidth: 64
                    implicitHeight: 30
                    onClicked: {
                        appState.rotationAngle = (appState.rotationAngle + 90) % 360
                    }
                    background: Rectangle {
                        radius: Theme.radiusPill
                        color: parent.down ? Qt.rgba(0, 0.94, 1.0, 0.3) : (parent.hovered ? Qt.rgba(1, 1, 1, 0.1) : "transparent")
                    }
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        color: Theme.textPrimary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                // Кнопка перемикання сітки
                Button {
                    text: "# Сітка"
                    implicitWidth: 70
                    implicitHeight: 30
                    checkable: true
                    checked: appState.showGridOverlay
                    onClicked: appState.showGridOverlay = !appState.showGridOverlay
                    background: Rectangle {
                        radius: Theme.radiusPill
                        color: parent.checked ? Qt.rgba(0, 0.94, 1.0, 0.25) : (parent.hovered ? Qt.rgba(1, 1, 1, 0.1) : "transparent")
                        border.width: parent.checked ? 1 : 0
                        border.color: Theme.accentCyan
                    }
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        color: parent.checked ? Theme.accentCyan : Theme.textPrimary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }
    }
}
