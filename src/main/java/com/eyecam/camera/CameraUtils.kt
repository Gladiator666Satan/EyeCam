package com.eyecam.camera

import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.params.RggbChannelVector
import android.util.Range
import com.eyecam.model.SensorType
import kotlin.math.atan

object CameraUtils {

    fun classifySensor(characteristics: CameraCharacteristics): SensorType {
        val facing = characteristics.get(CameraCharacteristics.LENS_FACING)
        if (facing == CameraCharacteristics.LENS_FACING_FRONT) {
            return SensorType.FRONT
        }

        val focalLengths = characteristics.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
        val sensorSize = characteristics.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
        val minFocusDistance = characteristics.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0.0f

        if (focalLengths != null && focalLengths.isNotEmpty() && sensorSize != null) {
            val focalLength = focalLengths[0]
            val sensorWidth = sensorSize.width
            val hFov = Math.toDegrees(2.0 * atan((sensorWidth / (2.0 * focalLength)).toDouble())).toFloat()

            // Будь-який сенсор з фокусною відстанню < 2.8 мм або FOV >= 85° є ULTRA_WIDE
            // (на Samsung Galaxy Ultra надширококутний сенсор також має макро-автофокус з minFocusDistance >= 10.0f)
            return when {
                hFov >= 85.0f || focalLength < 2.8f -> SensorType.ULTRA_WIDE
                minFocusDistance >= 10.0f && focalLength in 2.8f..4.0f -> SensorType.MACRO
                hFov in 60.0f..84.9f || focalLength in 2.8f..5.5f -> SensorType.WIDE
                hFov < 60.0f || focalLength > 5.5f -> SensorType.TELEPHOTO
                else -> SensorType.WIDE
            }
        }

        if (minFocusDistance >= 10.0f) {
            return SensorType.MACRO
        }
        return SensorType.WIDE
    }

    fun kelvinToRggbChannelVector(kelvin: Int): RggbChannelVector {
        val k = kelvin.coerceIn(2000, 10000).toFloat()
        // Природний баланс білого без зеленого спотворення для сенсорів Bayer:
        // Нейтральне денне світло (5500K): R ≈ 2.0f, G = 1.0f, B ≈ 1.9f
        // Тепліше світло (2000K, лампа розжарювання): R ≈ 1.25f, B ≈ 3.2f
        // Холодніше світло (10000K, тінь): R ≈ 2.85f, B ≈ 1.2f
        val rGain = if (k <= 5500f) {
            1.25f + (k - 2000f) / 3500f * 0.75f
        } else {
            2.0f + (k - 5500f) / 4500f * 0.85f
        }
        val bGain = if (k <= 5500f) {
            3.2f - (k - 2000f) / 3500f * 1.3f
        } else {
            1.9f - (k - 5500f) / 4500f * 0.7f
        }
        return RggbChannelVector(rGain, 1.0f, 1.0f, bGain)
    }

    fun clampIso(iso: Int, range: Range<Int>?): Int {
        if (range == null) return iso.coerceIn(50, 3200)
        return iso.coerceIn(range.lower, range.upper)
    }

    fun clampExposureTime(exposureNs: Long, range: Range<Long>?): Long {
        if (range == null) return exposureNs.coerceIn(100_000L, 1_000_000_000L)
        return exposureNs.coerceIn(range.lower, range.upper)
    }
}
