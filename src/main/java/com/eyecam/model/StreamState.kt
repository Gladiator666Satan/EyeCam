package com.eyecam.model

/**
 * Тип активного з'єднання з десктопним клієнтом.
 */
enum class ConnectionType {
    NONE,
    USB,
    WIFI
}

/**
 * Рівень термального тротлінгу пристрою (адаптація під PowerManager.THERMAL_STATUS_*).
 */
enum class ThermalState(val severityLevel: Int, val descriptionUa: String) {
    NORMAL(0, "Нормальна"),
    LIGHT(1, "Помірне нагрівання"),
    MODERATE(2, "Попередження тротлінгу"),
    SEVERE(3, "Критичний тротлінг (FPS/бітрейт обмежено)"),
    CRITICAL(4, "Аварійний перегрів"),
    UNKNOWN(-1, "Невідомо")
}

/**
 * Поточна телеметрія та стан відеотрансляції.
 */
data class StreamTelemetry(
    val fps: Int = 0,
    val targetFps: Int = 60,
    val bitrateKbps: Int = 0,
    val targetBitrateKbps: Int = 12000,
    val resolutionWidth: Int = 1920,
    val resolutionHeight: Int = 1080,
    val batteryPercentage: Int = 100,
    val isCharging: Boolean = false,
    val batteryTemperatureC: Float = 25.0f,
    val thermalState: ThermalState = ThermalState.NORMAL,
    val connectionType: ConnectionType = ConnectionType.NONE,
    val connectionDetails: String = "Очікування підключення...",
    val droppedFramesCount: Long = 0L
)

/**
 * Загальний стан системи для UI та бізнес-логіки.
 */
data class StreamUiState(
    val isStreaming: Boolean = false,
    val isMicMuted: Boolean = false,
    val isTorchOn: Boolean = false,
    val activeSensor: CameraSensorInfo? = null,
    val availableSensors: List<CameraSensorInfo> = emptyList(),
    val isDisplayProtectionActive: Boolean = false,
    val telemetry: StreamTelemetry = StreamTelemetry()
)
