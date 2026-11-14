package com.eyecam.model

/**
 * Мова інтерфейсу додатку (за замовчуванням при першому встановленні — English).
 */
enum class AppLanguage(val code: String) {
    EN("en"),
    UA("ua");

    companion object {
        fun fromCode(code: String?): AppLanguage =
            if (code?.lowercase() == "ua" || code?.lowercase() == "uk") UA else EN
    }
}

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
enum class ThermalState(
    val severityLevel: Int,
    val descriptionUa: String,
    val descriptionEn: String
) {
    NORMAL(0, "Нормальна", "Normal"),
    LIGHT(1, "Помірне нагрівання", "Warm"),
    MODERATE(2, "Попередження тротлінгу", "Throttling Warning"),
    SEVERE(3, "Критичний тротлінг", "Severe Throttling"),
    CRITICAL(4, "Аварійний перегрів", "Critical Overheat"),
    UNKNOWN(-1, "Невідомо", "Unknown")
}

/**
 * Поточна телеметрія та стан відеотрансляції.
 */
data class StreamTelemetry(
    val fps: Int = 0,
    val targetFps: Int = 30,
    val bitrateKbps: Int = 0,
    val targetBitrateKbps: Int = 8000,
    val resolutionWidth: Int = 1920,
    val resolutionHeight: Int = 1080,
    val batteryPercentage: Int = 100,
    val isCharging: Boolean = false,
    val batteryTemperatureC: Float = 25.0f,
    val thermalState: ThermalState = ThermalState.NORMAL,
    val connectionType: ConnectionType = ConnectionType.NONE,
    val connectionDetails: String = "",
    val isPcConnected: Boolean = false,
    val localIpAddress: String = "",
    val droppedFramesCount: Long = 0L
)

/**
 * Загальний стан системи для UI та бізнес-логіки.
 */
data class StreamUiState(
    val language: AppLanguage = AppLanguage.EN,
    val shouldExitApp: Boolean = false,
    val isStreaming: Boolean = false,
    val hasCameraPermission: Boolean = false,
    val isMicMuted: Boolean = false,
    val isTorchOn: Boolean = false,
    val isTorchAvailable: Boolean = true,
    val isAeLocked: Boolean = false,
    val cameraMode: CameraMode = CameraMode.Auto,
    val activeSensor: CameraSensorInfo? = null,
    val availableSensors: List<CameraSensorInfo> = emptyList(),
    val availableModules: List<CameraModuleInfo> = emptyList(),
    val activeModuleKey: String = "main",
    val intraModuleZoom: Float = 1.0f,
    val maxIntraModuleZoom: Float = 6.0f,
    val minZoomRatio: Float = 1.0f,
    val maxZoomRatio: Float = 6.0f,
    val currentZoomRatio: Float = 1.0f,
    val supportedZoomPresets: List<Float> = listOf(1.0f),
    val isFrontCamera: Boolean = false,
    val supports4K: Boolean = false,
    val supports2K: Boolean = false,
    val supports60Fps: Boolean = true,
    val isLowEndDevice: Boolean = false,
    val activeLutName: String = "",
    val liveIso: Int = 200,
    val liveExposureNs: Long = 16_666_666L,
    val liveFocusDiopters: Float = 0.5f,
    val liveFocusNorm: Float = 0.2f,
    val liveWbKelvin: Int = 5200,
    val afTriggerPulseSeq: Long = 0L,
    val autoModePulseSeq: Long = 0L,
    val deviceModelName: String = "",
    val isDisplayProtectionActive: Boolean = false,
    val telemetry: StreamTelemetry = StreamTelemetry()
)
