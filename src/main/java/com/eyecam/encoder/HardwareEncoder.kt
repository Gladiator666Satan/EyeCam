package com.eyecam.encoder

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
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
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.min

fun interface NalUnitListener {
    fun onNalUnitAvailable(nalUnit: NalUnit)
}

/**
 * Hardware capabilities summary for the device's primary H.264/AVC hardware encoder.
 */
data class AvcEncoderCapabilities(
    val maxWidth: Int = 1920,
    val maxHeight: Int = 1080,
    val maxFpsAt1080p: Int = 30,
    val maxFpsAt720p: Int = 30,
    val maxBitrateBps: Int = 12_000_000,
    val minBitrateBps: Int = 500_000,
    val widthAlignment: Int = 2,
    val heightAlignment: Int = 2,
    val supports4K: Boolean = false,
    val supports2K: Boolean = false,
    val supports60Fps1080p: Boolean = false
)

/**
 * Low-latency hardware Surface-to-Annex-B H.264/HEVC encoder.
 *
 * Automatically clamps resolution, frame rate, and bitrate to the device's hardware encoder
 * limits (preventing crashes and CPU/GPU overload on budget/legacy devices such as Redmi 8A),
 * and avoids per-frame NAL byte-scanning allocations when only Access Units are needed.
 */
class HardwareEncoder(
    @Volatile var config: EncoderConfig,
    private val onAccessUnit: ((data: ByteArray, isKeyFrame: Boolean, ptsUs: Long) -> Unit)? = null,
    private val listener: NalUnitListener? = null
) {
    companion object {
        private const val TAG = "EyeCam-Encoder"
        private const val START_CODE_4_BYTES = 4
        private const val START_CODE_3_BYTES = 3
        private const val HEADER_SCAN_LIMIT_BYTES = 768

        @Volatile
        private var cachedAvcCaps: AvcEncoderCapabilities? = null

        fun probeAvcCapabilities(): AvcEncoderCapabilities {
            cachedAvcCaps?.let { return it }
            synchronized(this) {
                cachedAvcCaps?.let { return it }
                val probed = querySystemAvcEncoder()
                cachedAvcCaps = probed
                Log.i(
                    TAG,
                    "Probed AVC hardware capabilities: max=${probed.maxWidth}x${probed.maxHeight}, " +
                        "1080pMaxFps=${probed.maxFpsAt1080p}, 4K=${probed.supports4K}, 2K=${probed.supports2K}, " +
                        "maxBitrate=${probed.maxBitrateBps / 1_000_000}Mbps"
                )
                return probed
            }
        }

        private fun querySystemAvcEncoder(): AvcEncoderCapabilities {
            return try {
                val list = MediaCodecList(MediaCodecList.REGULAR_CODECS)
                var bestVideoCaps: MediaCodecInfo.VideoCapabilities? = null
                var bestScore = -1

                for (info in list.codecInfos) {
                    if (!info.isEncoder) continue
                    val types = info.supportedTypes
                    if (types.none { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, ignoreCase = true) }) continue

                    val nameLower = info.name.lowercase()
                    val isSw = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        info.isSoftwareOnly
                    } else {
                        nameLower.startsWith("omx.google.") ||
                            nameLower.startsWith("c2.android.") ||
                            nameLower.contains(".sw.")
                    }

                    val caps = try {
                        info.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
                    } catch (_: Exception) {
                        null
                    } ?: continue

                    val vCaps = caps.videoCapabilities ?: continue
                    val maxW = vCaps.supportedWidths.upper
                    val score = (if (!isSw) 100_000 else 0) + maxW
                    if (score > bestScore) {
                        bestScore = score
                        bestVideoCaps = vCaps
                    }
                }

                val vc = bestVideoCaps ?: return AvcEncoderCapabilities()
                val maxW = vc.supportedWidths.upper
                val maxH = vc.supportedHeights.upper
                val sup4K = isSizeSupportedSafe(vc, 3840, 2160)
                val sup2K = isSizeSupportedSafe(vc, 2560, 1440)
                val fps1080 = maxFpsForSizeSafe(vc, 1920, 1080, defaultFps = 30)
                val fps720 = maxFpsForSizeSafe(vc, 1280, 720, defaultFps = 30)

                AvcEncoderCapabilities(
                    maxWidth = maxW,
                    maxHeight = maxH,
                    maxFpsAt1080p = fps1080,
                    maxFpsAt720p = fps720,
                    maxBitrateBps = vc.bitrateRange.upper.coerceIn(2_000_000, 60_000_000),
                    minBitrateBps = vc.bitrateRange.lower.coerceIn(200_000, 2_000_000),
                    widthAlignment = vc.widthAlignment.coerceAtLeast(2),
                    heightAlignment = vc.heightAlignment.coerceAtLeast(2),
                    supports4K = sup4K,
                    supports2K = sup2K,
                    supports60Fps1080p = fps1080 >= 55
                )
            } catch (e: Exception) {
                Log.w(TAG, "Failed probing MediaCodecList: ${e.message}")
                AvcEncoderCapabilities()
            }
        }

        private fun isSizeSupportedSafe(vc: MediaCodecInfo.VideoCapabilities, w: Int, h: Int): Boolean {
            return try {
                vc.isSizeSupported(w, h) || vc.isSizeSupported(h, w)
            } catch (_: Exception) {
                false
            }
        }

        private fun maxFpsForSizeSafe(vc: MediaCodecInfo.VideoCapabilities, w: Int, h: Int, defaultFps: Int): Int {
            return try {
                val range = when {
                    vc.isSizeSupported(w, h) -> vc.getSupportedFrameRatesFor(w, h)
                    vc.isSizeSupported(h, w) -> vc.getSupportedFrameRatesFor(h, w)
                    else -> null
                }
                val upper = range?.upper?.toInt() ?: defaultFps
                if (upper >= 55) 60 else 30
            } catch (_: Exception) {
                defaultFps
            }
        }

        fun sanitizeEncoderConfig(raw: EncoderConfig): EncoderConfig {
            val caps = probeAvcCapabilities()
            var w = raw.width
            var h = raw.height

            val longSide = maxOf(w, h)
            if (longSide >= 3840 && !caps.supports4K) {
                if (caps.supports2K) {
                    w = 2560; h = 1440
                } else {
                    w = 1920; h = 1080
                }
            } else if (longSide >= 2560 && !caps.supports2K) {
                w = 1920; h = 1080
            }

            if (w > caps.maxWidth || h > caps.maxHeight) {
                w = min(w, caps.maxWidth)
                h = min(h, caps.maxHeight)
            }
            w = (w / caps.widthAlignment).coerceAtLeast(16) * caps.widthAlignment
            h = (h / caps.heightAlignment).coerceAtLeast(16) * caps.heightAlignment

            val maxHardwareFps = when {
                maxOf(w, h) >= 3840 -> 30
                maxOf(w, h) >= 1920 -> caps.maxFpsAt1080p
                else -> caps.maxFpsAt720p
            }
            val safeFps = raw.frameRate.coerceIn(15, maxHardwareFps.coerceAtLeast(30))
            val safeBitrate = raw.bitrate.coerceIn(caps.minBitrateBps, caps.maxBitrateBps)

            return raw.copy(
                width = w,
                height = h,
                frameRate = safeFps,
                bitrate = safeBitrate
            )
        }
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

        val thread = HandlerThread("HardwareEncoderThread", Process.THREAD_PRIORITY_DISPLAY).apply {
            start()
        }
        encoderThread = thread
        val handler = Handler(thread.looper)
        encoderHandler = handler

        val sanitized = sanitizeEncoderConfig(config)
        config = sanitized
        try {
            val codec = configureWithFallback(sanitized, handler)
            inputSurface = codec.createInputSurface()
            codec.start()

            mediaCodec = codec
            isRunning.set(true)
            Log.i(
                TAG,
                "HardwareEncoder started: ${config.mimeType} ${config.width}x${config.height}@${config.frameRate} " +
                    "bitrate=${config.bitrate / 1000}Kbps"
            )
        } catch (e: Exception) {
            Log.e(TAG, "Failed to initialize MediaCodec for ${sanitized.width}x${sanitized.height}@${sanitized.frameRate}", e)
            stop()
            throw if (e is IOException) e else IOException("MediaCodec init failed", e)
        }
    }

    private fun configureWithFallback(cfg: EncoderConfig, handler: Handler): MediaCodec {
        val attempts = ArrayList<Pair<EncoderConfig, MediaFormat>>()
        val vbr = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR

        // Primary attempts at requested sanitized config
        val useBaseline = cfg.width <= 1920 && cfg.height <= 1088
        attempts.add(cfg to createFormat(cfg, lowLatency = true, includeProfile = useBaseline, bitrateMode = cfg.bitrateMode))
        attempts.add(cfg to createFormat(cfg, lowLatency = true, includeProfile = false, bitrateMode = vbr))
        attempts.add(cfg to createFormat(cfg, lowLatency = false, includeProfile = false, bitrateMode = vbr))

        // If 60 FPS failed on this hardware, fall back to 30 FPS at the SAME resolution
        if (cfg.frameRate > 30) {
            val cfg30 = cfg.copy(frameRate = 30, bitrate = (cfg.bitrate * 0.75f).toInt().coerceAtLeast(2_000_000))
            attempts.add(cfg30 to createFormat(cfg30, lowLatency = false, includeProfile = false, bitrateMode = vbr))
        }

        // Progressive resolution fallbacks (4K -> 2K -> 1080p -> 720p)
        if (cfg.width > 2560 || cfg.height > 1440) {
            val cfg2k = cfg.copy(width = 2560, height = 1440, frameRate = 30, bitrate = 12_000_000)
            attempts.add(cfg2k to createFormat(cfg2k, lowLatency = false, includeProfile = false, bitrateMode = vbr))
        }
        if (cfg.width > 1920 || cfg.height > 1080) {
            val cfg1080 = cfg.copy(width = 1920, height = 1080, frameRate = 30, bitrate = 7_500_000)
            attempts.add(cfg1080 to createFormat(cfg1080, lowLatency = false, includeProfile = false, bitrateMode = vbr))
        }
        if (cfg.width > 1280 || cfg.height > 720) {
            val cfg720 = cfg.copy(width = 1280, height = 720, frameRate = 30, bitrate = 4_500_000)
            attempts.add(cfg720 to createFormat(cfg720, lowLatency = false, includeProfile = false, bitrateMode = vbr))
        }

        var lastError: Exception? = null
        for ((idx, pair) in attempts.withIndex()) {
            val (attemptCfg, format) = pair
            var codec: MediaCodec? = null
            try {
                codec = MediaCodec.createEncoderByType(attemptCfg.mimeType)
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
                config = attemptCfg
                return codec
            } catch (e: Exception) {
                lastError = e
                Log.w(TAG, "Encoder format attempt #${idx + 1} (${attemptCfg.width}x${attemptCfg.height}@${attemptCfg.frameRate}) failed: ${e.message}")
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
            setFloat(MediaFormat.KEY_I_FRAME_INTERVAL, cfg.iFrameIntervalSeconds)
            setInteger(MediaFormat.KEY_BITRATE_MODE, bitrateMode)

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                setInteger(MediaFormat.KEY_MAX_B_FRAMES, cfg.maxBFrames)
            }

            if (lowLatency) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    setInteger(MediaFormat.KEY_LATENCY, cfg.latency)
                }
                setInteger("low-latency", 1)
                setInteger("vendor.qcom-ext-enc-low-latency.enable", 1)
                if (cfg.frameRate >= 60) {
                    setInteger(MediaFormat.KEY_PRIORITY, cfg.priority)
                    setInteger(MediaFormat.KEY_OPERATING_RATE, cfg.frameRate)
                }
            }

            if (includeProfile) {
                if (cfg.mimeType == MediaFormat.MIMETYPE_VIDEO_AVC) {
                    setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
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
                        if (listener != null) {
                            parseAndEmitAnnexBNalUnits(rawBuffer, info.presentationTimeUs, isKeyFrame = true)
                        }
                        return
                    }

                    val isKeyFrame = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0 ||
                        containsIdrSlice(rawBuffer)

                    val accessUnit = if (isKeyFrame) {
                        ensureSpsPpsPrepended(rawBuffer)
                    } else {
                        rawBuffer
                    }

                    onAccessUnit?.invoke(accessUnit, isKeyFrame, info.presentationTimeUs)
                    if (listener != null) {
                        parseAndEmitAnnexBNalUnits(accessUnit, info.presentationTimeUs, isKeyFrame)
                    }
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
            listener?.onNalUnitAvailable(NalUnit(if (isHevc) NalType.VPS else NalType.SPS, true, 0L, bytes))
        }
        format.getByteBuffer("csd-1")?.let { buf ->
            val bytes = ByteArray(buf.remaining())
            buf.get(bytes)
            buf.rewind()
            parts.add(bytes)
            listener?.onNalUnitAvailable(NalUnit(if (isHevc) NalType.SPS else NalType.PPS, true, 0L, bytes))
        }
        if (isHevc) {
            format.getByteBuffer("csd-2")?.let { buf ->
                val bytes = ByteArray(buf.remaining())
                buf.get(bytes)
                buf.rewind()
                parts.add(bytes)
                listener?.onNalUnitAvailable(NalUnit(NalType.PPS, true, 0L, bytes))
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
        val limit = min(data.size - 4, HEADER_SCAN_LIMIT_BYTES)
        var i = 0
        while (i < limit) {
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
        val limit = min(data.size - 4, HEADER_SCAN_LIMIT_BYTES)
        var i = 0
        while (i < limit) {
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
        val sink = listener ?: return
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
            sink.onNalUnitAvailable(NalUnit(type, isKeyFrame, ptsUs, rawBytes))
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
            sink.onNalUnitAvailable(unit)
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
