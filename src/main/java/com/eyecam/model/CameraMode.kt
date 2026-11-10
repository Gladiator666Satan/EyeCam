package com.eyecam.model

sealed class CameraMode {
    data object Auto : CameraMode()
    data class Manual(
        val iso: Int = 0,               // 0 = Auto ISO
        val exposureTimeNs: Long = 0L,  // 0L = Auto Exposure time
        val focusDistance: Float = 0.0f, // 0.0f = Auto Focus
        val whiteBalanceKelvin: Int = 0  // 0 = Auto White Balance (не спотворює колірний баланс)
    ) : CameraMode()
}
