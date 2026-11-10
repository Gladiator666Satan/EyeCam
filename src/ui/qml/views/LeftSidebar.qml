import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"
import ".."

// Ліва панель: Менеджер підключень (Wi-Fi/USB через ADB) та телеметрія в реальному часі
GlassCard {
    id: root

    implicitWidth: 320
    Layout.fillHeight: true

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacingMedium

        // Заголовок панелі та статус додатку
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Rectangle {
                width: 10
                height: 10
                radius: 5
                color: appState.isConnected ? Theme.accentSuccess : Theme.accentDanger

                // Пульсуюче коло підключення
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    running: appState.isConnected
                    NumberAnimation { from: 1.0; to: 0.3; duration: 800; easing.type: Easing.InOutQuad }
                    NumberAnimation { from: 0.3; to: 1.0; duration: 800; easing.type: Easing.InOutQuad }
                }
            }

            Text {
                text: "МЕНЕДЖЕР ПІДКЛЮЧЕНЬ"
                font.pixelSize: 11
                font.weight: Font.Bold
                color: Theme.textSecondary
                Layout.fillWidth: true
            }

            // Індикатор опитування ADB
            Rectangle {
                width: 70
                height: 20
                radius: Theme.radiusSmall
                color: Qt.rgba(1, 1, 1, 0.05)
                border.width: 1
                border.color: Qt.rgba(255, 255, 255, 0.1)

                Row {
                    anchors.centerIn: parent
                    spacing: 4
                    Rectangle {
                        width: 6
                        height: 6
                        radius: 3
                        color: appState.adbPollingActive ? Theme.accentCyan : Theme.textTertiary
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: "ADB ДЕМОН"
                        font.pixelSize: 8
                        font.weight: Font.Bold
                        color: Theme.textTertiary
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }

        // Блок 1: Статус USB підключення (ADB Polling)
        Rectangle {
            Layout.fillWidth: true
            height: 74
            radius: Theme.radiusMedium
            color: appState.usbConnected ? Qt.rgba(0.0, 0.94, 1.0, 0.08) : Qt.rgba(1, 1, 1, 0.03)
            border.width: 1
            border.color: appState.usbConnected ? Qt.rgba(0.0, 0.94, 1.0, 0.4) : Qt.rgba(255, 255, 255, 0.07)

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "🔌 Прямий USB (ADB)"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                    }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        width: badgeText.implicitWidth + 10
                        height: 18
                        radius: Theme.radiusSmall
                        color: appState.usbConnected ? Qt.rgba(0.06, 0.72, 0.5, 0.2) : Qt.rgba(1, 1, 1, 0.1)
                        Text {
                            id: badgeText
                            anchors.centerIn: parent
                            text: appState.usbConnected ? "АКТИВНО (3.0)" : "ПОШУК..."
                            font.pixelSize: 9
                            font.weight: Font.Bold
                            color: appState.usbConnected ? Theme.accentSuccess : Theme.textMuted
                        }
                    }
                }

                Text {
                    text: appState.usbConnected 
                          ? appState.usbDeviceName + " • " + appState.usbTransportMode
                          : "Підключіть пристрій через кабель з увімкненим USB Debugging"
                    font.pixelSize: 11
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
        }

        // Блок 2: Бездротові пристрої Wi-Fi (mDNS / SSDP Discovery)
        Text {
            text: "ДОСТУПНІ WI-FI КАМЕРИ (mDNS)"
            font.pixelSize: 10
            font.weight: Font.Bold
            color: Theme.textTertiary
            Layout.topMargin: 4
        }

        ListView {
            id: wifiDeviceList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: appState.discoveredDevicesModel

            delegate: Rectangle {
                width: wifiDeviceList.width
                height: 52
                radius: Theme.radiusMedium
                color: model.ipAddress === appState.activeIp ? Qt.rgba(0.0, 0.94, 1.0, 0.12) : Qt.rgba(1, 1, 1, 0.03)
                border.width: 1
                border.color: model.ipAddress === appState.activeIp ? Theme.accentCyan : Qt.rgba(255, 255, 255, 0.06)

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 8

                    Text {
                        text: "📶"
                        font.pixelSize: 14
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Text {
                            text: model.deviceName
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Text {
                            text: model.ipAddress + ":" + model.port + " • " + model.rssi + " dBm"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: Theme.textSecondary
                        }
                    }

                    Button {
                        text: model.ipAddress === appState.activeIp ? "Вимкнути" : "З'єднати"
                        implicitWidth: 78
                        implicitHeight: 28
                        onClicked: {
                            if (model.ipAddress === appState.activeIp) {
                                appState.disconnectCamera()
                            } else {
                                appState.connectCamera(model.ipAddress, model.port)
                            }
                        }
                        background: Rectangle {
                            radius: Theme.radiusSmall
                            color: parent.down ? Theme.accentCobalt : (model.ipAddress === appState.activeIp ? Qt.rgba(1, 0.2, 0.2, 0.2) : Qt.rgba(0, 0.94, 1.0, 0.2))
                            border.width: 1
                            border.color: model.ipAddress === appState.activeIp ? Theme.accentDanger : Theme.accentCyan
                        }
                        contentItem: Text {
                            text: parent.text
                            font.pixelSize: 10
                            font.weight: Font.Bold
                            color: Theme.textPrimary
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }

            // Повідомлення при відсутності пристроїв
            Rectangle {
                anchors.centerIn: parent
                width: parent.width - 20
                height: 60
                color: "transparent"
                visible: wifiDeviceList.count === 0

                Column {
                    anchors.centerIn: parent
                    spacing: 4
                    Text {
                        text: "Сканування локальної мережі..."
                        font.pixelSize: 11
                        color: Theme.textTertiary
                        anchors.horizontalCenter: parent.horizontalCenter
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

        // Блок 3: Телеметрія потоку в реальному часі (Real-time Telemetry)
        Text {
            text: "ТЕЛЕМЕТРІЯ ПОТОКУ (LOW-LATENCY)"
            font.pixelSize: 10
            font.weight: Font.Bold
            color: Theme.textTertiary
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            rowSpacing: 8
            columnSpacing: 8

            TelemetryBadge {
                Layout.fillWidth: true
                label: "Затримка RTT"
                value: appState.telemetryPing.toFixed(1)
                unit: "ms"
                statusLevel: appState.telemetryPing < 15 ? 0 : (appState.telemetryPing < 40 ? 1 : 2)
                iconSymbol: "⏱"
            }

            TelemetryBadge {
                Layout.fillWidth: true
                label: "Рендер / Потік"
                value: appState.telemetryFps.toFixed(1)
                unit: "FPS"
                statusLevel: appState.telemetryFps >= 58.0 ? 0 : (appState.telemetryFps >= 45.0 ? 1 : 2)
                iconSymbol: "🎬"
            }

            TelemetryBadge {
                Layout.fillWidth: true
                label: "Бітрейт H.264"
                value: appState.telemetryBitrateMbps.toFixed(2)
                unit: "Mbps"
                statusLevel: 0
                iconSymbol: "📊"
            }

            TelemetryBadge {
                Layout.fillWidth: true
                label: "Втрата пакетів"
                value: appState.telemetryPacketLoss.toFixed(2)
                unit: "%"
                statusLevel: appState.telemetryPacketLoss < 0.1 ? 0 : (appState.telemetryPacketLoss < 1.0 ? 1 : 2)
                iconSymbol: "📡"
            }
        }
    }
}
