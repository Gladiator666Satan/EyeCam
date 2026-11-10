package com.eyecam.camera

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.os.Build
import android.util.Range
import com.eyecam.model.CameraSensorInfo

object CameraUtils {

    fun classifySensor(focalLength: Float, minZoom: Float, isFront: Boolean): String {
        if (isFront) return "Селфі-камера"
        return when {
            minZoom < 1.0f || focalLength < 20.0f -> "Ультраширококутна (0.5x)"
            focalLength in 20.0f..35.0f -> "Основний модуль (1x)"
            focalLength in 35.0f..85.0f -> "Телеоб'єктив (3x)"
            else -> "Перископ (5x - 10x)"
        }
    }

    fun getSupportedZoomPresets(minZoom: Float, maxZoom: Float): List<Float> {
        val standardPresets = listOf(0.5f, 0.6f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f)
        val available = standardPresets.filter { it in minZoom..maxZoom }.toMutableList()
        if (!available.contains(1.0f) && 1.0f in minZoom..maxZoom) {
            available.add(1.0f)
        }
        return available.sorted()
    }

    fun queryDeviceSensors(context: Context): List<CameraSensorInfo> {
        val list = mutableListOf<CameraSensorInfo>()
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as? CameraManager ?: return list

        try {
            for (id in manager.cameraIdList) {
                val chars = manager.getCameraCharacteristics(id)
                val facing = chars.get(CameraCharacteristics.LENS_FACING)
                val isFront = facing == CameraCharacteristics.LENS_FACING_FRONT

                val focalLengths = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
                val focalLength = focalLengths?.firstOrNull() ?: 24.0f

                val zoomRange: Range<Float>? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
                } else null

                val minZoom = zoomRange?.lower ?: 1.0f
                val maxZoom = zoomRange?.upper ?: (chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 10.0f)

                val name = classifySensor(focalLength, minZoom, isFront)

                list.add(
                    CameraSensorInfo(
                        id = id,
                        name = name,
                        focalLengthMm = focalLength,
                        minZoomRatio = minZoom,
                        maxZoomRatio = maxZoom,
                        isFront = isFront
                    )
                )
            }
        } catch (_: Exception) {}

        return list
    }
}
