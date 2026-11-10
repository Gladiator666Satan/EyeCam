pragma Singleton
import QtQuick

// Централізована дизайн-система EyeCam: палітра кольорів, радіуси, тіні та типографіка
QtObject {
    id: theme

    // Основна темна палітра (Deep Minimalist Dark Palette)
    readonly property color bgBase: "#090B0E"            // Глибокий фоновий колір
    readonly property color bgSurface: "#11141A"         // Поверхня базових панелей
    readonly property color bgSurfaceElevated: "#181D26" // Підвищена поверхня (hover, картки)
    readonly property color bgGlass: "#141923"           // Базовий шар для ефекту матового скла
    readonly property color bgGlassAccent: "#1E2538"     // Акцентний скляний шар

    // Напівпрозорі альфа-канали для гласморфізму
    readonly property color glassBackground: Qt.rgba(0.08, 0.10, 0.15, 0.65)
    readonly property color glassBorder: Qt.rgba(1.0, 1.0, 1.0, 0.08)
    readonly property color glassBorderHover: Qt.rgba(0.0, 0.94, 1.0, 0.35)
    readonly property color glassBorderActive: Qt.rgba(0.0, 0.94, 1.0, 0.70)

    // Акцентні кольори (Неоновий кіберпанк / Студійний монітор)
    readonly property color accentCyan: "#00F0FF"        // Головний акцентний неоновий колір
    readonly property color accentCobalt: "#3B82F6"      // Допоміжний акцент (синій індиго)
    readonly property color accentPurple: "#8B5CF6"      // Додатковий градієнт
    readonly property color accentSuccess: "#10B981"     // Зелений (Оптимальний пінг, стабільний потік)
    readonly property color accentWarning: "#F59E0B"     // Жовтий (Підвищений джитер, втрата пакетів)
    readonly property color accentDanger: "#EF4444"      // Червоний (Критичні помилки, обрив зв'язку)

    // Текстова ієрархія
    readonly property color textPrimary: "#FFFFFF"       // Основний контрастний текст
    readonly property color textSecondary: "#94A3B8"     // Вторинний текст (підписи, одиниці виміру)
    readonly property color textTertiary: "#64748B"      // Третинний текст (неактивні лейбли)
    readonly property color textMuted: "#475569"         // Приглушений колір

    // Геометрія та радіуси скруглення
    readonly property real radiusSmall: 6.0
    readonly property real radiusMedium: 10.0
    readonly property real radiusLarge: 16.0
    readonly property real radiusPill: 999.0

    // Відступи та розміри сітки
    readonly property int spacingSmall: 8
    readonly property int spacingMedium: 14
    readonly property int spacingLarge: 20
    readonly property int spacingXLarge: 28

    // Тривалість анімацій
    readonly property int animFast: 150
    readonly property int animNormal: 250
    readonly property int animSlow: 400
}
