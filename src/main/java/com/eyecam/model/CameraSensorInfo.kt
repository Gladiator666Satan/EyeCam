package com.eyecam.model

enum class SensorType {
    WIDE, ULTRA_WIDE, TELEPHOTO, MACRO, FRONT, UNKNOWN
}

/**
 * Опис окремого фізичного або логічного модуля камери для прямого вибору
 * без автоматичного перестрибування між об'єктивами під час зумування.
 */
data class CameraModuleInfo(
    val key: String,                 // "uw", "main", "tele", "tele5x", "macro", "front"
    val label: String,               // "0.5x UW", "1x Main", "2x Tele", "3x Tele", "5x Tele", "Front"
    val shortLabel: String,          // "0.5x", "1x", "2x", "3x", "5x", "SELF"
    val logicalCameraId: String,
    val physicalCameraId: String? = null,
    val sensorType: SensorType = SensorType.WIDE,
    val isFront: Boolean = false,
    val baseOpticalZoom: Float = 1.0f,
    val maxIntraZoom: Float = 6.0f,
    val logicalBandMin: Float = 1.0f,
    val logicalBandMax: Float = 1.95f,
    val maxFocusDiopters: Float = 5.0f
)

data class CameraSensorInfo(
    val id: String,
    val isLogical: Boolean,
    val physicalCameraIds: Set<String> = emptySet(),
    val sensorType: SensorType,
    val lensFacing: Int,
    val focalLengths: FloatArray,
    val minFocusDistance: Float,
    val isoRange: ClosedRange<Int>,
    val exposureTimeRangeNs: ClosedRange<Long>
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as CameraSensorInfo
        return id == other.id && isLogical == other.isLogical &&
               physicalCameraIds == other.physicalCameraIds && sensorType == other.sensorType &&
               lensFacing == other.lensFacing && focalLengths.contentEquals(other.focalLengths) &&
               minFocusDistance == other.minFocusDistance && isoRange == other.isoRange &&
               exposureTimeRangeNs == other.exposureTimeRangeNs
    }

    override fun hashCode(): Int {
        var result = id.hashCode()
        result = 31 * result + isLogical.hashCode()
        result = 31 * result + physicalCameraIds.hashCode()
        result = 31 * result + sensorType.hashCode()
        result = 31 * result + lensFacing
        result = 31 * result + focalLengths.contentHashCode()
        result = 31 * result + minFocusDistance.hashCode()
        result = 31 * result + isoRange.hashCode()
        result = 31 * result + exposureTimeRangeNs.hashCode()
        return result
    }
}
