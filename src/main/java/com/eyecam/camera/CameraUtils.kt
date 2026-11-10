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

        if (minFocusDistance >= 10.0f && (focalLengths == null || focalLengths.firstOrNull() ?: 0f < 3.5f)) {
            return SensorType.MACRO
        }

        if (focalLengths != null && focalLengths.isNotEmpty() && sensorSize != null) {
            val focalLength = focalLengths[0]
            val sensorWidth = sensorSize.width
            val hFov = Math.toDegrees(2.0 * atan((sensorWidth / (2.0 * focalLength)).toDouble())).toFloat()

            return when {
                hFov >= 85.0f || focalLength < 2.8f -> SensorType.ULTRA_WIDE
                hFov in 60.0f..84.9f || focalLength in 2.8f..5.5f -> SensorType.WIDE
                hFov < 60.0f || focalLength > 5.5f -> SensorType.TELEPHOTO
                else -> SensorType.WIDE
            }
        }
        return SensorType.WIDE
    }

    fun kelvinToRggbChannelVector(kelvin: Int): RggbChannelVector {
        val clampedKelvin = kelvin.coerceIn(2000, 10000)
        val temp = clampedKelvin / 100.0

        val red: Double = if (temp <= 66.0) 255.0 else 329.698727446 * Math.pow(temp - 60.0, -0.1332047592)
        val green: Double = if (temp <= 66.0) {
            99.4708025861 * Math.log(temp) - 161.1195681661
        } else {
            288.1221695283 * Math.pow(temp - 60.0, -0.0755148492)
        }
        val blue: Double = if (temp >= 66.0) 255.0 else if (temp <= 19.0) 0.0 else 138.5177312231 * Math.log(temp - 10.0) - 305.0447927307

        val rGain = (green.coerceIn(1.0, 255.0) / red.coerceIn(1.0, 255.0)).toFloat().coerceIn(1.0f, 4.0f)
        val bGain = (green.coerceIn(1.0, 255.0) / blue.coerceIn(1.0, 255.0)).toFloat().coerceIn(1.0f, 4.0f)

        return RggbChannelVector(rGain, 1.0f, 1.0f, bGain)
    }

    fun clampIso(iso: Int, isoRange: Range<Int>?): Int = isoRange?.clamp(iso) ?: iso
    fun clampExposureTime(exposureTimeNs: Long, rangeNs: Range<Long>?): Long = rangeNs?.clamp(exposureTimeNs) ?: exposureTimeNs
    fun clampFocusDistance(distance: Float, minDistance: Float): Float = distance.coerceIn(0.0f, minDistance)
}
