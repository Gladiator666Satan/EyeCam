import QtQuick
import QtQuick.Effects
import ".."

// Універсальний контейнер у стилі гласморфізму (Frosted Glass Container)
Item {
    id: root

    default property alias content: contentContainer.data
    property alias radius: backgroundRect.radius
    property color backgroundColor: Theme.glassBackground
    property color borderColor: Theme.glassBorder
    property bool hoverEffect: true
    property bool activeGlow: false
    property alias contentPadding: contentContainer.anchors.margins

    implicitWidth: 300
    implicitHeight: 200

    // Фоновий шар матового скла з градієнтною рамкою
    Rectangle {
        id: backgroundRect
        anchors.fill: parent
        radius: Theme.radiusLarge
        color: root.backgroundColor

        // Градієнтне фонове підсвічування для глибини
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(1.0, 1.0, 1.0, 0.04) }
            GradientStop { position: 1.0; color: Qt.rgba(0.0, 0.0, 0.0, 0.12) }
        }

        // Тонка напівпрозора рамка зі світловим відблиском
        border.width: 1
        border.color: mouseArea.containsMouse && root.hoverEffect 
                      ? Theme.glassBorderHover 
                      : (root.activeGlow ? Theme.glassBorderActive : root.borderColor)

        Behavior on border.color {
            ColorAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic }
        }

        // Внутрішнє світіння при активному стані
        Rectangle {
            anchors.fill: parent
            radius: backgroundRect.radius
            color: "transparent"
            border.width: 1
            border.color: Qt.rgba(0, 0.94, 1.0, 0.15)
            visible: root.activeGlow || (mouseArea.containsMouse && root.hoverEffect)
            opacity: mouseArea.containsMouse ? 1.0 : 0.4

            Behavior on opacity {
                NumberAnimation { duration: Theme.animNormal }
            }
        }
    }

    // Зона обробки наведення миші
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: root.hoverEffect
        acceptedButtons: Qt.NoButton
        propagateComposedEvents: true
    }

    // Контейнер внутрішнього вмісту з безпечними відступами
    Item {
        id: contentContainer
        anchors.fill: parent
        anchors.margins: Theme.spacingMedium
    }
}
