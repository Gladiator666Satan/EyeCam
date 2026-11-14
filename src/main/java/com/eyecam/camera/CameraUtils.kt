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

        return RggbChannelVector(rGain.coerceIn(1.0f, 4.0f), 1.0f, 1.0f, bGain.coerceIn(1.0f, 4.0f))
    }

    /**
     * Scales a captured neutral Bayer [baseGains] (from live AWB) by the requested [kelvin]
     * temperature relative to daylight (5500K), avoiding the strong green cast that occurs when
     * raw hardcoded gains override sensor-specific G channel calibration.
     */
    fun kelvinToRggbChannelVector(kelvin: Int, baseGains: RggbChannelVector?): RggbChannelVector {
        if (baseGains == null) return kelvinToRggbChannelVector(kelvin)
        val ref = kelvinToRggbChannelVector(5500)
        val target = kelvinToRggbChannelVector(kelvin)
        val rScale = target.red / ref.red
        val bScale = target.blue / ref.blue
        val gEven = baseGains.greenEven.coerceAtLeast(0.5f)
        val gOdd = baseGains.greenOdd.coerceAtLeast(0.5f)
        return RggbChannelVector(
            (baseGains.red * rScale).coerceIn(0.8f, 6.0f),
            gEven,
            gOdd,
            (baseGains.blue * bScale).coerceIn(0.8f, 6.0f)
        )
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
