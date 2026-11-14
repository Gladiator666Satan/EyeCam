package com.eyecam.camera

import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.params.ColorSpaceTransform
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
            val hFov = if (focalLength > 0.1f && sensorWidth > 0.1f) {
                Math.toDegrees(2.0 * atan((sensorWidth / (2.0 * focalLength)).toDouble())).toFloat()
            } else {
                75.0f
            }

            // Надширококутний сенсор (0.5x UW) має фокусну відстань < 2.6 мм (або FOV >= 92° при f < 3.0 мм).
            // Звичайні основні камери смартфонів (зокрема Redmi 8A з f ≈ 3.8 мм та minFocusDistance = 10 dpt)
            // завжди класифікуються як WIDE, щоб не створювати хибні модулі на 2-камерних пристроях.
            return when {
                focalLength < 2.6f || (hFov >= 92.0f && focalLength < 3.0f) -> SensorType.ULTRA_WIDE
                focalLength > 5.6f || (hFov < 52.0f && focalLength > 5.0f) -> SensorType.TELEPHOTO
                else -> SensorType.WIDE
            }
        }

        return SensorType.WIDE
    }

    /**
     * Computes clean, non-compounding Planckian RGGB channel gains for a given [kelvin]
     * temperature (`2000K..10000K`), preventing white balance overlap when switching camera modules.
     */
    fun kelvinToRggbChannelVector(kelvin: Int, @Suppress("UNUSED_PARAMETER") baseGains: RggbChannelVector? = null): RggbChannelVector {
        val k = kelvin.coerceIn(2000, 10000).toFloat()
        val rGain = if (k <= 5500f) {
            1.35f + (k - 2000f) / 3500f * 0.65f
        } else {
            2.00f + (k - 5500f) / 4500f * 0.75f
        }
        val bGain = if (k <= 5500f) {
            2.95f - (k - 2000f) / 3500f * 1.05f
        } else {
            1.90f - (k - 5500f) / 4500f * 0.65f
        }
        return RggbChannelVector(rGain.coerceIn(1.0f, 3.8f), 1.0f, 1.0f, bGain.coerceIn(1.0f, 3.8f))
    }

    /**
     * Estimates the live correlated color temperature in Kelvin (`2800..8500`) from the ISP's
     * auto white balance [RggbChannelVector] so the WB slider can track live AWB in AUTO mode.
     */
    fun rggbGainsToKelvin(gains: RggbChannelVector): Int {
        val r = gains.red.coerceAtLeast(0.1f)
        val b = gains.blue.coerceAtLeast(0.1f)
        val rbRatio = (r / b).coerceIn(0.45f, 2.25f)
        val est = if (rbRatio <= 1.05f) {
            2800f + ((rbRatio - 0.45f) / 0.60f) * 2700f
        } else {
            5500f + ((rbRatio - 1.05f) / 1.20f) * 3000f
        }
        return est.toInt().coerceIn(2800, 8500)
    }

    /**
     * Calibrated fallback sRGB color correction matrix (row sums equal 1.0 to preserve neutral grays)
     * used only if no live CCM has been captured from Auto AWB yet.
     */
    fun defaultColorCorrectionTransform(): ColorSpaceTransform {
        return ColorSpaceTransform(
            intArrayOf(
                140, 100,  -25, 100,  -15, 100,
                -15, 100,  130, 100,  -15, 100,
                 -5, 100,  -25, 100,  130, 100
            )
        )
    }

    fun clampIso(iso: Int, isoRange: Range<Int>?): Int = isoRange?.clamp(iso) ?: iso
    fun clampExposureTime(exposureTimeNs: Long, rangeNs: Range<Long>?): Long = rangeNs?.clamp(exposureTimeNs) ?: exposureTimeNs
    fun clampFocusDistance(distance: Float, minDistance: Float): Float = distance.coerceIn(0.0f, minDistance)
}
