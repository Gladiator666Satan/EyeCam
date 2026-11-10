package com.eyecam.model

sealed class CameraMode {
    data object Auto : CameraMode()
    data class Manual(
        val iso: Int,
        val exposureTimeNs: Long,
        val focusDistance: Float,
        val whiteBalanceKelvin: Int
    ) : CameraMode()
}
