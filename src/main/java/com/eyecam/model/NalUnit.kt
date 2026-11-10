package com.eyecam.model

enum class NalType {
    SPS, PPS, VPS, SEI, IDR_FRAME, P_FRAME, B_FRAME, OTHER
}

data class NalUnit(
    val type: NalType,
    val isKeyFrame: Boolean,
    val presentationTimestampUs: Long,
    val data: ByteArray
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as NalUnit
        return type == other.type && isKeyFrame == other.isKeyFrame &&
               presentationTimestampUs == other.presentationTimestampUs && data.contentEquals(other.data)
    }

    override fun hashCode(): Int {
        var result = type.hashCode()
        result = 31 * result + isKeyFrame.hashCode()
        result = 31 * result + presentationTimestampUs.hashCode()
        result = 31 * result + data.contentHashCode()
        return result
    }
}
