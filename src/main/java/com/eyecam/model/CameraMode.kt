package com.eyecam.model

sealed class CameraMode {
    data object Auto : CameraMode()
    data class Manual(
        val iso: Int = 0,                 // 0 = Auto ISO
        val exposureTimeNs: Long = 0L,    // 0L = Auto Exposure time (Shutter)
        val focusDistance: Float = -1.0f, // -1.0f = Auto Focus (>= 0.0f = Manual Focus, 0.0f = ∞)
        val whiteBalanceKelvin: Int = 0   // 0 = Auto White Balance
    ) : CameraMode()

    val isIsoManual: Boolean
        get() = (this as? Manual)?.let { it.iso > 0 } ?: false

    val isShutterManual: Boolean
        get() = (this as? Manual)?.let { it.exposureTimeNs > 0L } ?: false

    val isFocusManual: Boolean
        get() = (this as? Manual)?.let { it.focusDistance >= 0f } ?: false

    val isWbManual: Boolean
        get() = (this as? Manual)?.let { it.whiteBalanceKelvin > 0 } ?: false

    val hasAnyManualLock: Boolean
        get() = isIsoManual || isShutterManual || isFocusManual || isWbManual

    fun normalized(): CameraMode {
        return if (this is Manual && !hasAnyManualLock) Auto else this
    }
}
