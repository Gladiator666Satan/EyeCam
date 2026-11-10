package com.eyecam.model

sealed class CameraMode {
    data object Auto : CameraMode()
    data class Manual(
        val iso: Int = 100,
        val exposureTimeNs: Long = 20_000_000L,
        val focusDistance: Float = 0.0f,
        val whiteBalanceKelvin: Int = 5500
    ) : CameraMode()
}
