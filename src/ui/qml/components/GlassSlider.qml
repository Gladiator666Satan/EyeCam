import QtQuick
import QtQuick.Controls.Basic
import ".."

// Високоточний слайдер у стилі гласморфізму для Pro-керування параметрами камери
Control {
    id: root

    property string title: "Параметр"
    property string displayValue: value.toFixed(1)
    property string unit: ""
    property real from: 0.0
    property real to: 100.0
    property real stepSize: 1.0
    property real value: 0.0
    property bool enabled: true

    // Сигнал зміни значення користувачем
    signal userValueChanged(real newValue)

    implicitWidth: 260
    implicitHeight: 64

    contentItem: Item {
        anchors.fill: parent

        // Заголовок та поточне цифрове значення
        Row {
            id: headerRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 22

            Text {
                text: root.title
                font.pixelSize: 12
                font.weight: Font.DemiBold
                color: root.enabled ? Theme.textSecondary : Theme.textMuted
                elide: Text.ElideRight
                width: parent.width - valBadge.width - Theme.spacingSmall
            }

            Rectangle {
                id: valBadge
                anchors.verticalCenter: parent.verticalCenter
                width: valText.implicitWidth + 14
                height: 20
                radius: Theme.radiusSmall
                color: Qt.rgba(0.0, 0.94, 1.0, 0.12)
                border.width: 1
                border.color: Qt.rgba(0.0, 0.94, 1.0, 0.3)

                Text {
                    id: valText
                    anchors.centerIn: parent
                    text: root.displayValue + (root.unit !== "" ? " " + root.unit : "")
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    font.family: "Monospace"
                    color: root.enabled ? Theme.accentCyan : Theme.textMuted
                }
            }
        }

        // Інтерактивна область слайдера
        Slider {
            id: sliderInternal
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 32
            from: root.from
            to: root.to
            stepSize: root.stepSize
            value: root.value
            enabled: root.enabled

            onMoved: {
                root.value = value
                root.userValueChanged(value)
            }

            // Фонова доріжка (Track)
            background: Item {
                x: sliderInternal.leftPadding
                y: sliderInternal.topPadding + sliderInternal.availableHeight / 2 - height / 2
                implicitWidth: 200
                implicitHeight: 6
                width: sliderInternal.availableWidth
                height: implicitHeight

                // Неактивна частина доріжки
                Rectangle {
                    anchors.fill: parent
                    radius: 3
                    color: Qt.rgba(1.0, 1.0, 1.0, 0.10)
                    border.width: 1
                    border.color: Qt.rgba(255, 255, 255, 0.05)
                }

                // Заповнена активна частина доріжки з градієнтом
                Rectangle {
                    width: sliderInternal.visualPosition * parent.width
                    height: parent.height
                    radius: 3
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: Theme.accentCobalt }
                        GradientStop { position: 1.0; color: Theme.accentCyan }
                    }
                }
            }

            // Повзунок (Thumb)
            handle: Rectangle {
                x: sliderInternal.leftPadding + sliderInternal.visualPosition * (sliderInternal.availableWidth - width)
                y: sliderInternal.topPadding + sliderInternal.availableHeight / 2 - height / 2
                implicitWidth: sliderInternal.pressed ? 20 : 16
                implicitHeight: sliderInternal.pressed ? 20 : 16
                radius: Theme.radiusPill
                color: Theme.textPrimary
                border.width: 2
                border.color: Theme.accentCyan

                // Ефект пульсуючого неонового підсвічування
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width + 10
                    height: parent.height + 10
                    radius: Theme.radiusPill
                    color: Qt.rgba(0.0, 0.94, 1.0, sliderInternal.pressed ? 0.4 : 0.15)
                    z: -1

                    Behavior on color {
                        ColorAnimation { duration: Theme.animFast }
                    }
                }

                Behavior on implicitWidth {
                    NumberAnimation { duration: Theme.animFast; easing.type: Easing.OutBack }
                }
                Behavior on implicitHeight {
                    NumberAnimation { duration: Theme.animFast; easing.type: Easing.OutBack }
                }
            }
        }
    }
}
