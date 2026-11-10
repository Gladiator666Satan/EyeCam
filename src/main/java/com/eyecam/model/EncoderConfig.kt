package com.eyecam.model

import android.media.MediaCodecInfo
import android.media.MediaFormat

data class EncoderConfig(
    val mimeType: String = MediaFormat.MIMETYPE_VIDEO_AVC,
    val width: Int = 1920,
    val height: Int = 1080,
    val bitrate: Int = 4_000_000,
    val frameRate: Int = 30,
    val iFrameIntervalSeconds: Float = 1.0f,
    val bitrateMode: Int = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR,
    val latency: Int = 0,
    val priority: Int = 0,
    val maxBFrames: Int = 0
)
