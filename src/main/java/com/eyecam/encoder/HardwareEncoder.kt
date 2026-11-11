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

/**
 * Low-latency hardware Surface-to-Annex-B H.264/HEVC encoder.
 *
 * Emits both individual [NalUnit] slices (legacy coordinator compatibility) and complete,
 * self-contained Annex-B Access Units with SPS/PPS prepended before every IDR keyframe so
 * remote decoders can recover instantaneously after connect or frame drops.
 */
class HardwareEncoder(
    @Volatile var config: EncoderConfig,
    private val listener: NalUnitListener = NalUnitListener { },
    private val onAccessUnit: ((data: ByteArray, isKeyFrame: Boolean, ptsUs: Long) -> Unit)? = null
) {
    companion object {
        private const val TAG = "EyeCam-Encoder"
        private const val START_CODE_4_BYTES = 4
        private const val START_CODE_3_BYTES = 3
    }

    private var mediaCodec: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var encoderThread: HandlerThread? = null
    private var encoderHandler: Handler? = null

    private val isRunning = AtomicBoolean(false)
    @Volatile
    private var cachedCsd: ByteArray? = null

    private val isHevc: Boolean
        get() = config.mimeType == MediaFormat.MIMETYPE_VIDEO_HEVC

    fun getInputSurface(): Surface {
        return inputSurface ?: throw IllegalStateException("Encoder is not running.")
    }

    fun isActive(): Boolean = isRunning.get()

    @Synchronized
    fun start() {
        if (isRunning.get()) return
        cachedCsd = null

        val thread = HandlerThread("HardwareEncoderThread", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply {
            start()
        }
        encoderThread = thread
        val handler = Handler(thread.looper)
        encoderHandler = handler

        val cfg = config
        try {
            val codec = configureWithFallback(cfg, handler)
            inputSurface = codec.createInputSurface()
            codec.start()

            mediaCodec = codec
            isRunning.set(true)
            Log.i(
                TAG,
                "HardwareEncoder started: ${cfg.mimeType} ${cfg.width}x${cfg.height}@${cfg.frameRate} " +
                    "bitrate=${cfg.bitrate / 1_000_000}Mbps"
            )
        } catch (e: Exception) {
            Log.e(TAG, "Failed to initialize MediaCodec for ${cfg.width}x${cfg.height}@${cfg.frameRate}", e)
            stop()
            throw if (e is IOException) e else IOException("MediaCodec init failed", e)
        }
    }

    private fun configureWithFallback(cfg: EncoderConfig, handler: Handler): MediaCodec {
        val attempts = listOf(
            createFormat(cfg, lowLatency = true, includeProfile = true, bitrateMode = cfg.bitrateMode),
            createFormat(cfg, lowLatency = true, includeProfile = false, bitrateMode = cfg.bitrateMode),
            createFormat(
                cfg,
                lowLatency = false,
                includeProfile = false,
                bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR
            )
        )

        var lastError: Exception? = null
        for ((idx, format) in attempts.withIndex()) {
            var codec: MediaCodec? = null
            try {
                codec = MediaCodec.createEncoderByType(cfg.mimeType)
                codec.setCallback(object : MediaCodec.Callback() {
                    override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {}

                    override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                        handleOutputBuffer(codec, index, info)
                    }

                    override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                        Log.e(TAG, "MediaCodec error: ${e.diagnosticInfo}", e)
                    }

                    override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                        extractCodecSpecificData(format)
                    }
                }, handler)
                codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                return codec
            } catch (e: Exception) {
                lastError = e
                Log.w(TAG, "Encoder format attempt #${idx + 1} failed: ${e.message}")
                try {
                    codec?.release()
                } catch (_: Exception) {
                }
            }
        }
        throw lastError ?: IOException("All MediaCodec configurations failed")
    }

    private fun createFormat(
        cfg: EncoderConfig,
        lowLatency: Boolean,
        includeProfile: Boolean,
        bitrateMode: Int
    ): MediaFormat {
        return MediaFormat.createVideoFormat(cfg.mimeType, cfg.width, cfg.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, cfg.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, cfg.frameRate)
            setInteger(MediaFormat.KEY_CAPTURE_RATE, cfg.frameRate)
            setFloat(MediaFormat.KEY_I_FRAME_INTERVAL, cfg.iFrameIntervalSeconds)
            setInteger(MediaFormat.KEY_BITRATE_MODE, bitrateMode)
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, cfg.maxBFrames)

            if (lowLatency) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    setInteger(MediaFormat.KEY_LATENCY, cfg.latency)
                }
                setInteger("low-latency", 1)
                setInteger("vendor.qcom-ext-enc-low-latency.enable", 1)
                setInteger(MediaFormat.KEY_PRIORITY, cfg.priority)
                setInteger(MediaFormat.KEY_OPERATING_RATE, cfg.frameRate.coerceAtLeast(30))
            }

            if (includeProfile) {
                if (cfg.mimeType == MediaFormat.MIMETYPE_VIDEO_AVC) {
                    setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
                    // Do NOT hardcode AVCLevel41 — let MediaCodec select level automatically for 2K/4K60
                } else if (cfg.mimeType == MediaFormat.MIMETYPE_VIDEO_HEVC) {
                    setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.HEVCProfileMain)
                }
            }
        }
    }

    private fun handleOutputBuffer(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
        try {
            if (!isRunning.get()) return
            if (info.size > 0) {
                val outputBuffer = codec.getOutputBuffer(index)
                if (outputBuffer != null) {
                    outputBuffer.position(info.offset)
                    outputBuffer.limit(info.offset + info.size)

                    val rawBuffer = ByteArray(info.size)
                    outputBuffer.get(rawBuffer)

                    val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                    if (isConfig) {
                        cachedCsd = rawBuffer.copyOf()
                        parseAndEmitAnnexBNalUnits(rawBuffer, info.presentationTimeUs, isKeyFrame = true)
                        return
                    }

                    val rawHasIdr = containsIdrSlice(rawBuffer)
                    val isKeyFrame = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0 || rawHasIdr

                    val accessUnit = if (isKeyFrame) {
                        ensureSpsPpsPrepended(rawBuffer)
                    } else {
                        rawBuffer
                    }

                    onAccessUnit?.invoke(accessUnit, isKeyFrame, info.presentationTimeUs)
                    parseAndEmitAnnexBNalUnits(accessUnit, info.presentationTimeUs, isKeyFrame)
                }
            }
        } catch (e: IllegalStateException) {
            Log.w(TAG, "Output buffer handled during codec shutdown: ${e.message}")
        } finally {
            try {
                codec.releaseOutputBuffer(index, false)
            } catch (_: IllegalStateException) {
            }
        }
    }

    private fun extractCodecSpecificData(format: MediaFormat) {
        val parts = ArrayList<ByteArray>(3)
        format.getByteBuffer("csd-0")?.let { buf ->
            val bytes = ByteArray(buf.remaining())
            buf.get(bytes)
            buf.rewind()
            parts.add(bytes)
            listener.onNalUnitAvailable(NalUnit(if (isHevc) NalType.VPS else NalType.SPS, true, 0L, bytes))
        }
        format.getByteBuffer("csd-1")?.let { buf ->
            val bytes = ByteArray(buf.remaining())
            buf.get(bytes)
            buf.rewind()
            parts.add(bytes)
            listener.onNalUnitAvailable(NalUnit(if (isHevc) NalType.SPS else NalType.PPS, true, 0L, bytes))
        }
        if (isHevc) {
            format.getByteBuffer("csd-2")?.let { buf ->
                val bytes = ByteArray(buf.remaining())
                buf.get(bytes)
                buf.rewind()
                parts.add(bytes)
                listener.onNalUnitAvailable(NalUnit(NalType.PPS, true, 0L, bytes))
            }
        }
        if (parts.isNotEmpty()) {
            val total = parts.sumOf { it.size }
            val merged = ByteArray(total)
            var pos = 0
            for (p in parts) {
                System.arraycopy(p, 0, merged, pos, p.size)
                pos += p.size
            }
            cachedCsd = merged
        }
    }

    private fun ensureSpsPpsPrepended(frameBytes: ByteArray): ByteArray {
        val csd = cachedCsd ?: return frameBytes
        if (containsParameterSet(frameBytes)) return frameBytes
        val combined = ByteArray(csd.size + frameBytes.size)
        System.arraycopy(csd, 0, combined, 0, csd.size)
        System.arraycopy(frameBytes, 0, combined, csd.size, frameBytes.size)
        return combined
    }

    private fun containsParameterSet(data: ByteArray): Boolean {
        var i = 0
        while (i < data.size - 4) {
            if (data[i] == 0.toByte() && data[i + 1] == 0.toByte()) {
                val offset = when {
                    data[i + 2] == 1.toByte() -> i + 3
                    data[i + 2] == 0.toByte() && data[i + 3] == 1.toByte() -> i + 4
                    else -> -1
                }
                if (offset in data.indices) {
                    val nalHeader = data[offset].toInt()
                    val isParam = if (!isHevc) {
                        (nalHeader and 0x1F) == 7
                    } else {
                        ((nalHeader shr 1) and 0x3F) == 32
                    }
                    if (isParam) return true
                    i = offset + 1
                    continue
                }
            }
            i++
        }
        return false
    }

    private fun containsIdrSlice(data: ByteArray): Boolean {
        var i = 0
        while (i < data.size - 4) {
            if (data[i] == 0.toByte() && data[i + 1] == 0.toByte()) {
                val offset = when {
                    data[i + 2] == 1.toByte() -> i + 3
                    data[i + 2] == 0.toByte() && data[i + 3] == 1.toByte() -> i + 4
                    else -> -1
                }
                if (offset in data.indices) {
                    val nalHeader = data[offset].toInt()
                    val isIdr = if (!isHevc) {
                        (nalHeader and 0x1F) == 5
                    } else {
                        val t = (nalHeader shr 1) and 0x3F
                        t == 19 || t == 20
                    }
                    if (isIdr) return true
                    i = offset + 1
                    continue
                }
            }
            i++
        }
        return false
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
            try {
                val params = Bundle().apply {
                    putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
                }
                mediaCodec?.setParameters(params)
            } catch (e: Exception) {
                Log.w(TAG, "requestSyncFrame failed: ${e.message}")
            }
        }
    }

    fun setBitrate(bitrateBps: Int) {
        if (!isRunning.get()) return
        encoderHandler?.post {
            try {
                val params = Bundle().apply {
                    putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrateBps)
                }
                mediaCodec?.setParameters(params)
            } catch (e: Exception) {
                Log.w(TAG, "setBitrate failed: ${e.message}")
            }
        }
    }

    @Synchronized
    fun stop() {
        val wasRunning = isRunning.getAndSet(false)
        try {
            if (wasRunning) {
                mediaCodec?.stop()
            }
        } catch (e: Exception) {
            Log.w(TAG, "MediaCodec stop warning: ${e.message}")
        } finally {
            try {
                mediaCodec?.release()
            } catch (_: Exception) {
            }
            mediaCodec = null
            try {
                inputSurface?.release()
            } catch (_: Exception) {
            }
            inputSurface = null
            encoderThread?.quitSafely()
            encoderThread = null
            encoderHandler = null
        }
    }
}
