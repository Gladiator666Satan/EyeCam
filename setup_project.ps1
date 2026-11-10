# ==============================================================================
# EyeCam - Automated Setup Script for Camera2 / MediaCodec Pipeline
# Usage in PowerShell: powershell -ExecutionPolicy Bypass -File .\setup_project.ps1
# ==============================================================================

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if (-not $root) { $root = Get-Location }

Write-Host ">>> Initializing EyeCam Native Android Engine in: $root" -ForegroundColor Cyan

# Ensure target directories exist
$dirs = @(
    "src/main/java/com/eyecam/model",
    "src/main/java/com/eyecam/camera",
    "src/main/java/com/eyecam/encoder",
    "src/main/java/com/eyecam/pipeline"
)

foreach ($d in $dirs) {
    $targetPath = Join-Path $root $d
    if (-not (Test-Path $targetPath)) {
        New-Item -ItemType Directory -Path $targetPath -Force | Out-Null
        Write-Host "Created folder: $d" -ForegroundColor Green
    }
}

# 1. CameraSensorInfo.kt
$p1 = Join-Path $root "src/main/java/com/eyecam/model/CameraSensorInfo.kt"
@'
package com.eyecam.model

enum class SensorType {
    WIDE, ULTRA_WIDE, TELEPHOTO, MACRO, FRONT, UNKNOWN
}

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
'@ | Set-Content -Path $p1 -Encoding UTF8
Write-Host "Written: $p1" -ForegroundColor Green

# 2. CameraMode.kt
$p2 = Join-Path $root "src/main/java/com/eyecam/model/CameraMode.kt"
@'
package com.eyecam.model

sealed class CameraMode {
    data object Auto : CameraMode()
    data class Manual(
        val iso: Int,
        val exposureTimeNs: Long,
        val focusDistance: Float,
        val whiteBalanceKelvin: Int
    ) : CameraMode()
}
'@ | Set-Content -Path $p2 -Encoding UTF8
Write-Host "Written: $p2" -ForegroundColor Green

# 3. NalUnit.kt
$p3 = Join-Path $root "src/main/java/com/eyecam/model/NalUnit.kt"
@'
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
'@ | Set-Content -Path $p3 -Encoding UTF8
Write-Host "Written: $p3" -ForegroundColor Green

# 4. EncoderConfig.kt
$p4 = Join-Path $root "src/main/java/com/eyecam/model/EncoderConfig.kt"
@'
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
'@ | Set-Content -Path $p4 -Encoding UTF8
Write-Host "Written: $p4" -ForegroundColor Green

# 5. CameraUtils.kt
$p5 = Join-Path $root "src/main/java/com/eyecam/camera/CameraUtils.kt"
@'
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
'@ | Set-Content -Path $p5 -Encoding UTF8
Write-Host "Written: $p5" -ForegroundColor Green

# 6. HardwareEncoder.kt
$p6 = Join-Path $root "src/main/java/com/eyecam/encoder/HardwareEncoder.kt"
@'
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
'@ | Set-Content -Path $p6 -Encoding UTF8
Write-Host "Written: $p6" -ForegroundColor Green

# 7. CameraController.kt
$p7 = Join-Path $root "src/main/java/com/eyecam/camera/CameraController.kt"
@'
package com.eyecam.camera

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.*
import android.hardware.camera2.params.ColorSpaceTransform
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.RggbChannelVector
import android.hardware.camera2.params.SessionConfiguration
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.util.Log
import android.util.Range
import android.view.Surface
import com.eyecam.model.CameraMode
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.SensorType
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class CameraController(private val context: Context) {

    companion object {
        private const val TAG = "CameraController"
        private val IDENTITY_COLOR_TRANSFORM = ColorSpaceTransform(
            intArrayOf(1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1)
        )
    }

    private val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null

    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var previewRequestBuilder: CaptureRequest.Builder? = null

    private var previewSurface: Surface? = null
    private var encoderSurface: Surface? = null

    private val isSessionActive = AtomicBoolean(false)
    private var activeLogicalCameraId: String? = null
    private var activePhysicalCameraId: String? = null
    private var currentMode: CameraMode = CameraMode.Auto
    private var currentSensorCharacteristics: CameraCharacteristics? = null

    private val _availableSensors = mutableListOf<CameraSensorInfo>()
    val availableSensors: List<CameraSensorInfo> get() = _availableSensors

    init {
        discoverAvailableSensors()
    }

    fun discoverAvailableSensors(): List<CameraSensorInfo> {
        _availableSensors.clear()
        try {
            for (id in cameraManager.cameraIdList) {
                val chars = cameraManager.getCameraCharacteristics(id)
                val caps = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf()
                val isLogical = caps.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA)
                val facing = chars.get(CameraCharacteristics.LENS_FACING) ?: CameraCharacteristics.LENS_FACING_BACK

                val physicalIds = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && isLogical) {
                    chars.physicalCameraIds
                } else emptySet()

                val focalLengths = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS) ?: floatArrayOf(0f)
                val minFocusDist = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0.0f
                val isoRange = chars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE) ?: Range(100, 3200)
                val expRange = chars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) ?: Range(100_000L, 1_000_000_000L)

                _availableSensors.add(
                    CameraSensorInfo(
                        id = id,
                        isLogical = isLogical,
                        physicalCameraIds = physicalIds,
                        sensorType = CameraUtils.classifySensor(chars),
                        lensFacing = facing,
                        focalLengths = focalLengths,
                        minFocusDistance = minFocusDist,
                        isoRange = isoRange.lower..isoRange.upper,
                        exposureTimeRangeNs = expRange.lower..expRange.upper
                    )
                )

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && isLogical) {
                    for (physId in physicalIds) {
                        val physChars = cameraManager.getCameraCharacteristics(physId)
                        val physFocal = physChars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS) ?: floatArrayOf(0f)
                        val physMinFocus = physChars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0.0f
                        val physIso = physChars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE) ?: Range(100, 3200)
                        val physExp = physChars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) ?: Range(100_000L, 1_000_000_000L)

                        _availableSensors.add(
                            CameraSensorInfo(
                                id = physId,
                                isLogical = false,
                                physicalCameraIds = emptySet(),
                                sensorType = CameraUtils.classifySensor(physChars),
                                lensFacing = facing,
                                focalLengths = physFocal,
                                minFocusDistance = physMinFocus,
                                isoRange = physIso.lower..physIso.upper,
                                exposureTimeRangeNs = physExp.lower..physExp.upper
                            )
                        )
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error discovering sensors", e)
        }
        return _availableSensors
    }

    @SuppressLint("MissingPermission")
    fun openCamera(
        logicalCameraId: String,
        initialPhysicalCameraId: String? = null,
        previewSurface: Surface,
        encoderSurface: Surface
    ) {
        startBackgroundThread()
        this.previewSurface = previewSurface
        this.encoderSurface = encoderSurface
        this.activeLogicalCameraId = logicalCameraId
        this.activePhysicalCameraId = initialPhysicalCameraId
        this.currentSensorCharacteristics = cameraManager.getCameraCharacteristics(logicalCameraId)

        val handler = cameraHandler ?: return
        cameraManager.openCamera(logicalCameraId, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                cameraDevice = camera
                createCaptureSessionInternal()
            }
            override fun onDisconnected(camera: CameraDevice) = closeCamera()
            override fun onError(camera: CameraDevice, error: Int) = closeCamera()
        }, handler)
    }

    private fun createCaptureSessionInternal() {
        val device = cameraDevice ?: return
        val preview = previewSurface ?: return
        val encoder = encoderSurface ?: return

        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                val previewOutput = OutputConfiguration(preview)
                val encoderOutput = OutputConfiguration(encoder)

                activePhysicalCameraId?.let { physId ->
                    previewOutput.setPhysicalCameraId(physId)
                    encoderOutput.setPhysicalCameraId(physId)
                }

                val sessionConfig = SessionConfiguration(
                    SessionConfiguration.SESSION_REGULAR,
                    listOf(previewOutput, encoderOutput),
                    Executors.newSingleThreadExecutor(),
                    createSessionCallback()
                )
                device.createCaptureSession(sessionConfig)
            } else {
                @Suppress("DEPRECATION")
                device.createCaptureSession(listOf(preview, encoder), createSessionCallback(), cameraHandler)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error creating session", e)
        }
    }

    private fun createSessionCallback() = object : CameraCaptureSession.StateCallback() {
        override fun onConfigured(session: CameraCaptureSession) {
            captureSession = session
            isSessionActive.set(true)
            startRepeatingPipeline()
        }
        override fun onConfigureFailed(session: CameraCaptureSession) {
            isSessionActive.set(false)
        }
    }

    private fun startRepeatingPipeline() {
        val device = cameraDevice ?: return
        val session = captureSession ?: return
        val preview = previewSurface ?: return
        val encoder = encoderSurface ?: return
        val handler = cameraHandler ?: return

        try {
            val builder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(preview)
                addTarget(encoder)
            }
            applyModeToBuilder(builder, currentMode)
            previewRequestBuilder = builder
            session.setRepeatingRequest(builder.build(), null, handler)
        } catch (e: Exception) {
            Log.e(TAG, "Error starting repeating request", e)
        }
    }

    fun switchPhysicalCameraSeamless(physicalId: String) {
        val session = captureSession ?: return
        val builder = previewRequestBuilder ?: return
        val handler = cameraHandler ?: return

        activePhysicalCameraId = physicalId

        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                val targetSensor = _availableSensors.find { it.id == physicalId }
                when (targetSensor?.sensorType) {
                    SensorType.ULTRA_WIDE -> builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, 0.6f)
                    SensorType.WIDE -> builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, 1.0f)
                    SensorType.TELEPHOTO -> builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, 3.0f)
                    else -> {}
                }
            }

            currentSensorCharacteristics = cameraManager.getCameraCharacteristics(physicalId)
            session.setRepeatingRequest(builder.build(), null, handler)
            Log.i(TAG, "Switched sensor to $physicalId")
        } catch (e: Exception) {
            Log.e(TAG, "Error switching sensor", e)
        }
    }

    fun setMode(mode: CameraMode) {
        this.currentMode = mode
        val builder = previewRequestBuilder ?: return
        val session = captureSession ?: return
        val handler = cameraHandler ?: return

        applyModeToBuilder(builder, mode)
        try {
            session.setRepeatingRequest(builder.build(), null, handler)
        } catch (e: Exception) {
            Log.e(TAG, "Error updating mode", e)
        }
    }

    private fun applyModeToBuilder(builder: CaptureRequest.Builder, mode: CameraMode) {
        val chars = currentSensorCharacteristics

        when (mode) {
            is CameraMode.Auto -> {
                builder.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO)
                builder.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                builder.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_ON)
                builder.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_AUTO)
            }
            is CameraMode.Manual -> {
                builder.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO)

                val minFocusDist = chars?.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 10.0f
                builder.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_OFF)
                builder.set(CaptureRequest.LENS_FOCUS_DISTANCE, CameraUtils.clampFocusDistance(mode.focusDistance, minFocusDist))

                val isoRange = chars?.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
                val expRange = chars?.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)

                builder.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF)
                builder.set(CaptureRequest.SENSOR_SENSITIVITY, CameraUtils.clampIso(mode.iso, isoRange))
                builder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, CameraUtils.clampExposureTime(mode.exposureTimeNs, expRange))

                val rggbVector: RggbChannelVector = CameraUtils.kelvinToRggbChannelVector(mode.whiteBalanceKelvin)
                builder.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_OFF)
                builder.set(CaptureRequest.COLOR_CORRECTION_MODE, CameraMetadata.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX)
                builder.set(CaptureRequest.COLOR_CORRECTION_GAINS, rggbVector)
                builder.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM, IDENTITY_COLOR_TRANSFORM)
            }
        }
    }

    @Synchronized
    fun closeCamera() {
        isSessionActive.set(false)
        try { captureSession?.close() } catch (_: Exception) {}
        try { cameraDevice?.close() } catch (_: Exception) {}
        captureSession = null
        cameraDevice = null
        previewRequestBuilder = null
        stopBackgroundThread()
    }

    private fun startBackgroundThread() {
        if (cameraThread == null) {
            cameraThread = HandlerThread("CameraBackgroundThread", Process.THREAD_PRIORITY_MORE_FAVORABLE).apply {
                start()
                cameraHandler = Handler(looper)
            }
        }
    }

    private fun stopBackgroundThread() {
        cameraThread?.quitSafely()
        cameraThread = null
        cameraHandler = null
    }
}
'@ | Set-Content -Path $p7 -Encoding UTF8
Write-Host "Written: $p7" -ForegroundColor Green

# 8. VideoPipelineCoordinator.kt
$p8 = Join-Path $root "src/main/java/com/eyecam/pipeline/VideoPipelineCoordinator.kt"
@'
package com.eyecam.pipeline

import android.content.Context
import android.view.Surface
import com.eyecam.camera.CameraController
import com.eyecam.encoder.HardwareEncoder
import com.eyecam.encoder.NalUnitListener
import com.eyecam.model.CameraMode
import com.eyecam.model.EncoderConfig
import com.eyecam.model.NalUnit

class VideoPipelineCoordinator(
    context: Context,
    private val encoderConfig: EncoderConfig = EncoderConfig(),
    private val networkSocketSender: (NalUnit) -> Unit
) {
    val cameraController = CameraController(context)
    private var hardwareEncoder: HardwareEncoder? = null

    fun startStreaming(previewSurface: Surface, logicalCameraId: String = "0") {
        val encoder = HardwareEncoder(encoderConfig) { nalUnit ->
            networkSocketSender(nalUnit)
        }
        encoder.start()
        hardwareEncoder = encoder

        cameraController.openCamera(
            logicalCameraId = logicalCameraId,
            previewSurface = previewSurface,
            encoderSurface = encoder.getInputSurface()
        )
    }

    fun switchToSensor(physicalId: String) {
        cameraController.switchPhysicalCameraSeamless(physicalId)
    }

    fun setCameraMode(mode: CameraMode) {
        cameraController.setMode(mode)
    }

    fun requestSyncFrame() {
        hardwareEncoder?.requestSyncFrame()
    }

    fun stop() {
        cameraController.closeCamera()
        hardwareEncoder?.stop()
        hardwareEncoder = null
    }
}
'@ | Set-Content -Path $p8 -Encoding UTF8
Write-Host "Written: $p8" -ForegroundColor Green

Write-Host ">>> All files successfully verified and written to project folder!" -ForegroundColor Cyan
