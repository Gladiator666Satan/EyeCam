package com.eyecam.encoder

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.util.Log
import android.view.Surface
import com.eyecam.model.EncoderConfig
import com.eyecam.model.NalType
import com.eyecam.model.NalUnit
import java.io.IOException
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean

fun interface NalUnitListener {
    fun onNalUnitAvailable(nalUnit: NalUnit)
}

class HardwareEncoder(
    private val config: EncoderConfig,
    private val listener: NalUnitListener
) {
    companion object {
        private const val TAG = "HardwareEncoder"
        private const val START_CODE_4_BYTES = 4
        private const val START_CODE_3_BYTES = 3
    }

    private var mediaCodec: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var encoderThread: HandlerThread? = null
    private var encoderHandler: Handler? = null

    private val isRunning = AtomicBoolean(false)
    private val isHevc = config.mimeType == MediaFormat.MIMETYPE_VIDEO_HEVC

    fun getInputSurface(): Surface {
        return inputSurface ?: throw IllegalStateException("Encoder is not running.")
    }

    @Synchronized
    fun start() {
        if (isRunning.get()) return

        val thread = HandlerThread("HardwareEncoderThread", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply {
            start()
        }
        encoderThread = thread
        val handler = Handler(thread.looper)
        encoderHandler = handler

        try {
            val codec = MediaCodec.createEncoderByType(config.mimeType)
            val format = createLowLatencyFormat(config)

            codec.setCallback(object : MediaCodec.Callback() {
                override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {}

                override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                    handleOutputBuffer(codec, index, info)
                }

                override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                    Log.e(TAG, "MediaCodec error: diagnosticInfo=${e.diagnosticInfo}", e)
                }

                override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                    extractCodecSpecificData(format)
                }
            }, handler)

            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            inputSurface = codec.createInputSurface()
            codec.start()

            mediaCodec = codec
            isRunning.set(true)
            Log.i(TAG, "HardwareEncoder started: ${config.mimeType}")
        } catch (e: IOException) {
            Log.e(TAG, "Failed to initialize MediaCodec", e)
            stop()
            throw e
        }
    }

    private fun createLowLatencyFormat(cfg: EncoderConfig): MediaFormat {
        return MediaFormat.createVideoFormat(cfg.mimeType, cfg.width, cfg.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, cfg.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, cfg.frameRate)
            setInteger(MediaFormat.KEY_CAPTURE_RATE, cfg.frameRate)
            setFloat(MediaFormat.KEY_I_FRAME_INTERVAL, cfg.iFrameIntervalSeconds)
            setInteger(MediaFormat.KEY_BITRATE_MODE, cfg.bitrateMode)

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                setInteger(MediaFormat.KEY_LATENCY, cfg.latency)
            }
            setInteger("low-latency", 1)
            setInteger("vendor.qcom-ext-enc-low-latency.enable", 1)
            setInteger(MediaFormat.KEY_PRIORITY, cfg.priority)
            setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE.toInt())
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, cfg.maxBFrames)

            if (cfg.mimeType == MediaFormat.MIMETYPE_VIDEO_AVC) {
                setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
                setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel41)
            } else if (cfg.mimeType == MediaFormat.MIMETYPE_VIDEO_HEVC) {
                setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.HEVCProfileMain)
            }
        }
    }

    private fun handleOutputBuffer(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
        try {
            if (info.size > 0) {
                val outputBuffer = codec.getOutputBuffer(index)
                if (outputBuffer != null) {
                    outputBuffer.position(info.offset)
                    outputBuffer.limit(info.offset + info.size)

                    val rawBuffer = ByteArray(info.size)
                    outputBuffer.get(rawBuffer)

                    val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                    val isKeyFrame = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0

                    parseAndEmitAnnexBNalUnits(rawBuffer, info.presentationTimeUs, isKeyFrame || isConfig)
                }
            }
        } finally {
            codec.releaseOutputBuffer(index, false)
        }
    }

    private fun extractCodecSpecificData(format: MediaFormat) {
        format.getByteBuffer("csd-0")?.let { emitCodecBuffer(it, if (isHevc) NalType.VPS else NalType.SPS) }
        format.getByteBuffer("csd-1")?.let { emitCodecBuffer(it, if (isHevc) NalType.SPS else NalType.PPS) }
        if (isHevc) {
            format.getByteBuffer("csd-2")?.let { emitCodecBuffer(it, NalType.PPS) }
        }
    }

    private fun emitCodecBuffer(buffer: ByteBuffer, type: NalType) {
        val bytes = ByteArray(buffer.remaining())
        buffer.get(bytes)
        buffer.rewind()
        listener.onNalUnitAvailable(NalUnit(type, true, 0L, bytes))
    }

    private fun parseAndEmitAnnexBNalUnits(rawBytes: ByteArray, ptsUs: Long, isKeyFrame: Boolean) {
        val startOffsets = ArrayList<Int>()
        var i = 0
        val length = rawBytes.size

        while (i < length - 3) {
            if (rawBytes[i] == 0.toByte() && rawBytes[i + 1] == 0.toByte()) {
                if (rawBytes[i + 2] == 1.toByte()) {
                    startOffsets.add(i)
                    i += 3
                    continue
                } else if (rawBytes[i + 2] == 0.toByte() && i < length - 3 && rawBytes[i + 3] == 1.toByte()) {
                    startOffsets.add(i)
                    i += 4
                    continue
                }
            }
            i++
        }

        if (startOffsets.isEmpty()) {
            val type = detectNalType(rawBytes, 0)
            listener.onNalUnitAvailable(NalUnit(type, isKeyFrame, ptsUs, rawBytes))
            return
        }

        for (idx in 0 until startOffsets.size) {
            val start = startOffsets[idx]
            val end = if (idx + 1 < startOffsets.size) startOffsets[idx + 1] else length
            val nalLength = end - start

            val nalBytes = ByteArray(nalLength)
            System.arraycopy(rawBytes, start, nalBytes, 0, nalLength)

            val headerOffset = if (nalBytes[2] == 1.toByte()) START_CODE_3_BYTES else START_CODE_4_BYTES
            val type = detectNalType(nalBytes, headerOffset)

            val unit = NalUnit(
                type = type,
                isKeyFrame = isKeyFrame || type == NalType.IDR_FRAME || type == NalType.SPS || type == NalType.VPS,
                presentationTimestampUs = ptsUs,
                data = nalBytes
            )
            listener.onNalUnitAvailable(unit)
        }
    }

    private fun detectNalType(nalBytes: ByteArray, headerOffset: Int): NalType {
        if (headerOffset >= nalBytes.size) return NalType.OTHER
        val headerByte = nalBytes[headerOffset].toInt()

        return if (!isHevc) {
            when (headerByte and 0x1F) {
                7 -> NalType.SPS
                8 -> NalType.PPS
                5 -> NalType.IDR_FRAME
                1 -> NalType.P_FRAME
                6 -> NalType.SEI
                else -> NalType.OTHER
            }
        } else {
            when ((headerByte shr 1) and 0x3F) {
                32 -> NalType.VPS
                33 -> NalType.SPS
                34 -> NalType.PPS
                19, 20 -> NalType.IDR_FRAME
                1 -> NalType.P_FRAME
                39, 40 -> NalType.SEI
                else -> NalType.OTHER
            }
        }
    }

    fun requestSyncFrame() {
        if (!isRunning.get()) return
        encoderHandler?.post {
            val params = Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            }
            mediaCodec?.setParameters(params)
        }
    }

    fun setBitrate(bitrateBps: Int) {
        if (!isRunning.get()) return
        encoderHandler?.post {
            val params = Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrateBps)
            }
            mediaCodec?.setParameters(params)
        }
    }

    @Synchronized
    fun stop() {
        if (!isRunning.getAndSet(false)) return
        try {
            mediaCodec?.stop()
            mediaCodec?.release()
        } finally {
            mediaCodec = null
            inputSurface?.release()
            inputSurface = null
            encoderThread?.quitSafely()
            encoderThread = null
            encoderHandler = null
        }
    }
}
