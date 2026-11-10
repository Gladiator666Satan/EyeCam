import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"
import ".."

// Права панель інспектора: Керування камерою, перемикач режимів Auto / Pro та повзунки
GlassCard {
    id: root

    implicitWidth: 340
    Layout.fillHeight: true

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacingMedium

        // Заголовок панелі та перемикач режимів (Auto / Pro Toggle)
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Text {
                text: "ПАРАМЕТРИ КАМЕРИ"
                font.pixelSize: 11
                font.weight: Font.Bold
                color: Theme.textSecondary
                Layout.fillWidth: true
            }

            // Перемикач режимів Auto / Pro у стилі "Pill Selector"
            Rectangle {
                id: modeSwitchBg
                width: 140
                height: 32
                radius: Theme.radiusPill
                color: Qt.rgba(0.05, 0.07, 0.1, 0.9)
                border.width: 1
                border.color: Theme.glassBorder

                // Анімована підкладка активного режиму
                Rectangle {
                    x: appState.isProMode ? modeSwitchBg.width / 2 : 2
                    y: 2
                    width: modeSwitchBg.width / 2 - 2
                    height: modeSwitchBg.height - 4
                    radius: Theme.radiusPill
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: Theme.accentCobalt }
                        GradientStop { position: 1.0; color: Theme.accentCyan }
                    }

                    Behavior on x {
                        NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic }
                    }
                }

                Row {
                    anchors.fill: parent
                    Item {
                        width: parent.width / 2
                        height: parent.height
                        MouseArea {
                            anchors.fill: parent
                            onClicked: appState.isProMode = false
                        }
                        Text {
                            anchors.centerIn: parent
                            text: "AUTO"
                            font.pixelSize: 11
                            font.weight: Font.Bold
                            color: !appState.isProMode ? Theme.textPrimary : Theme.textTertiary
                        }
                    }
                    Item {
                        width: parent.width / 2
                        height: parent.height
                        MouseArea {
                            anchors.fill: parent
                            onClicked: appState.isProMode = true
                        }
                        Text {
                            anchors.centerIn: parent
                            text: "PRO"
                            font.pixelSize: 11
                            font.weight: Font.Bold
                            color: appState.isProMode ? Theme.textPrimary : Theme.textTertiary
                        }
                    }
                }
            }
        }

        // Роздільник
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.glassBorder
        }

        // Область прокрутки параметрів
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            ColumnLayout {
                width: parent.width
                spacing: 16

                // --- РЕЖИМ AUTO ---
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 14
                    visible: !appState.isProMode

                    Rectangle {
                        Layout.fillWidth: true
                        height: 54
                        radius: Theme.radiusMedium
                        color: Qt.rgba(0.0, 0.94, 1.0, 0.05)
                        border.width: 1
                        border.color: Qt.rgba(0.0, 0.94, 1.0, 0.15)

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 8
                            Text {
                                text: "✨"
                                font.pixelSize: 16
                            }
                            ColumnLayout {
                                Text {
                                    text: "Інтелектуальна авто-експозиція"
                                    font.pixelSize: 11
                                    font.weight: Font.Bold
                                    color: Theme.textPrimary
                                }
                                Text {
                                    text: "Сенсор автоматично адаптує ISO, витримку та баланс білого"
                                    font.pixelSize: 10
                                    color: Theme.textSecondary
                                }
                            }
                        }
                    }

                    // Компенсація експозиції (Exposure Value)
                    GlassSlider {
                        Layout.fillWidth: true
                        title: "Компенсація експозиції (EV)"
                        from: -3.0
                        to: 3.0
                        stepSize: 0.1
                        value: appState.exposureCompensation
                        displayValue: (appState.exposureCompensation > 0 ? "+" : "") + appState.exposureCompensation.toFixed(1)
                        unit: "EV"
                        onUserValueChanged: function(newVal) { appState.exposureCompensation = newVal }
                    }

                    // Оптичний / Цифровий зум для режиму Auto
                    GlassSlider {
                        Layout.fillWidth: true
                        title: "Масштабування (Zoom)"
                        from: 1.0
                        to: 10.0
                        stepSize: 0.1
                        value: appState.zoom
                        displayValue: appState.zoom.toFixed(1)
                        unit: "x"
                        onUserValueChanged: function(newVal) { appState.zoom = newVal }
                    }
                }

                // --- РЕЖИМ PRO: Гранулярні слайдери ---
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 14
                    visible: appState.isProMode

                    // 1. Повзунок ISO (Світлочутливість)
                    GlassSlider {
                        Layout.fillWidth: true
                        title: "Світлочутливість (ISO)"
                        from: 100
                        to: 12800
                        stepSize: 50
                        value: appState.iso
                        displayValue: Math.round(appState.iso).toString()
                        unit: "ISO"
                        onUserValueChanged: function(newVal) { appState.iso = newVal }
                    }

                    // 2. Повзунок витримки (Shutter Speed)
                    GlassSlider {
                        Layout.fillWidth: true
                        title: "Витримка (Shutter Speed)"
                        from: 24
                        to: 8000
                        stepSize: 10
                        value: appState.shutterDenominator
                        displayValue: "1/" + Math.round(appState.shutterDenominator)
                        unit: "s"
                        onUserValueChanged: function(newVal) { appState.shutterDenominator = newVal }
                    }

                    // 3. Ручне фокусування (Manual Focus)
                    GlassSlider {
                        Layout.fillWidth: true
                        title: "Ручне фокусування (Focus)"
                        from: 0.0
                        to: 1.0
                        stepSize: 0.01
                        value: appState.manualFocus
                        displayValue: appState.manualFocus === 0.0 ? "Макро" : (appState.manualFocus >= 0.99 ? "Нескінченність" : appState.manualFocus.toFixed(2))
                        unit: ""
                        onUserValueChanged: function(newVal) { appState.manualFocus = newVal }
                    }

                    // 4. Оптичний / Цифровий зум (Zoom)
                    GlassSlider {
                        Layout.fillWidth: true
                        title: "Плавний зум (Zoom)"
                        from: 1.0
                        to: 10.0
                        stepSize: 0.05
                        value: appState.zoom
                        displayValue: appState.zoom.toFixed(2)
                        unit: "x"
                        onUserValueChanged: function(newVal) { appState.zoom = newVal }
                    }

                    // 5. Баланс білого в Кельвінах (White Balance)
                    GlassSlider {
                        Layout.fillWidth: true
                        title: "Баланс білого (WB Kelvin)"
                        from: 2000
                        to: 10000
                        stepSize: 50
                        value: appState.whiteBalanceKelvin
                        displayValue: Math.round(appState.whiteBalanceKelvin).toString()
                        unit: "K"
                        onUserValueChanged: function(newVal) { appState.whiteBalanceKelvin = newVal }
                    }

                    // Швидкі пресети балансу білого
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Repeater {
                            model: [
                                { name: "3200K", val: 3200, label: "Tungsten" },
                                { name: "4000K", val: 4000, label: "Fluoresc" },
                                { name: "5600K", val: 5600, label: "Daylight" },
                                { name: "7000K", val: 7000, label: "Shade" }
                            ]
                            delegate: Button {
                                Layout.fillWidth: true
                                implicitHeight: 26
                                text: modelData.name
                                onClicked: appState.whiteBalanceKelvin = modelData.val
                                background: Rectangle {
                                    radius: Theme.radiusSmall
                                    color: Math.abs(appState.whiteBalanceKelvin - modelData.val) < 100
                                           ? Qt.rgba(0.0, 0.94, 1.0, 0.25)
                                           : Qt.rgba(1, 1, 1, 0.06)
                                    border.width: 1
                                    border.color: Math.abs(appState.whiteBalanceKelvin - modelData.val) < 100
                                                  ? Theme.accentCyan
                                                  : Theme.glassBorder
                                }
                                contentItem: Text {
                                    text: parent.text
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    color: Theme.textPrimary
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }
                    }

                    // 6. Відтінок (Tint: Magenta / Green)
                    GlassSlider {
                        Layout.fillWidth: true
                        title: "Відтінок (Tint Magenta / Green)"
                        from: -50
                        to: 50
                        stepSize: 1
                        value: appState.tint
                        displayValue: (appState.tint > 0 ? "+" : "") + Math.round(appState.tint).toString()
                        unit: ""
                        onUserValueChanged: function(newVal) { appState.tint = newVal }
                    }
                }
            }
        }

        // Кнопка скидання налаштувань
        Button {
            Layout.fillWidth: true
            implicitHeight: 36
            text: "Скинути до стандартних налаштувань"
            onClicked: appState.resetCameraDefaults()
            background: Rectangle {
                radius: Theme.radiusMedium
                color: parent.down ? Qt.rgba(1, 1, 1, 0.1) : Qt.rgba(1, 1, 1, 0.04)
                border.width: 1
                border.color: Theme.glassBorder
            }
            contentItem: Text {
                text: parent.text
                font.pixelSize: 11
                font.weight: Font.Medium
                color: Theme.textSecondary
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
}
