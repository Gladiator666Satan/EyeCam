package com.eyecam.camera

import android.Manifest
import android.annotation.SuppressLint
import android.app.ActivityManager
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Rect
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.ColorSpaceTransform
import android.hardware.camera2.params.RggbChannelVector
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.OrientationEventListener
import android.view.Surface
import androidx.annotation.OptIn
import androidx.camera.camera2.interop.Camera2CameraControl
import androidx.camera.camera2.interop.Camera2CameraInfo
import androidx.camera.camera2.interop.Camera2Interop
import androidx.camera.camera2.interop.CaptureRequestOptions
import androidx.camera.camera2.interop.ExperimentalCamera2Interop
import androidx.camera.core.Camera
import androidx.camera.core.CameraFilter
import androidx.camera.core.CameraInfo
import androidx.camera.core.CameraSelector
import androidx.camera.core.FocusMeteringAction
import androidx.camera.core.Preview
import androidx.camera.core.SurfaceOrientedMeteringPointFactory
import androidx.camera.core.resolutionselector.AspectRatioStrategy
import androidx.camera.core.resolutionselector.ResolutionFilter
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import com.eyecam.encoder.HardwareEncoder
import com.eyecam.model.CameraMode
import com.eyecam.model.CameraModuleInfo
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.EncoderConfig
import com.eyecam.model.SensorType
import java.io.InputStream
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.abs
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * CameraX + OpenGL ES Tee + Hardware H.264 Encoder camera engine.
 *
 * Features:
 * - Automatic hardware capability & budget/legacy device detection (e.g., Redmi 8A)
 * - Explicit physical camera module binding (UW, Main, Tele, Super-Tele/Macro, Front)
 * - Strictly intra-module zoom (1.0x .. maxIntraZoom) without unwanted multi-cam lens jumping
 * - Independent per-parameter Auto/Manual controls (ISO, Shutter, Focus, WB) + Exposure Lock (AE Lock)
 * - Flashlight (Torch) control (disabled on Front camera)
 * - Real-time OpenGL ES `.cube` 3D LUT color grading
 */
@OptIn(ExperimentalCamera2Interop::class)
class CameraController(private val context: Context) {

    companion object {
        private const val TAG = "EyeCam-CameraX"
        private const val FOCUS_DURATION_SEC = 5L

        fun defaultBitrateForResolution(width: Int, fps: Int, isLowEnd: Boolean = false): Int {
            val base = when {
                width >= 3840 -> 28_000_000
                width >= 2560 -> 18_000_000
                width >= 1920 -> if (isLowEnd) 7_000_000 else 12_000_000
                else -> if (isLowEnd) 4_000_000 else 6_000_000
            }
            return if (fps >= 60) (base * 1.25f).toInt() else base
        }
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val mainExecutor = ContextCompat.getMainExecutor(context)
    private val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    private val eglTee = EglVideoTee()

    // Fallback internal lifecycle if used outside LifecycleService
    private val fallbackLifecycleOwner = object : LifecycleOwner {
        val registry = LifecycleRegistry(this)
        override val lifecycle: Lifecycle get() = registry
    }

    @Volatile
    private var attachedLifecycleOwner: LifecycleOwner? = null
    @Volatile
    private var cameraProvider: ProcessCameraProvider? = null
    @Volatile
    private var boundCamera: Camera? = null
    @Volatile
    private var previewUseCase: Preview? = null
    @Volatile
    private var cameraOpenRequested = false

    private var hardwareEncoder: HardwareEncoder? = null
    @Volatile
    private var externalEncoderSurface: Surface? = null

    // Hardware & budget device flags
    @Volatile var isLowEndDevice: Boolean = false
        private set
    @Volatile var supports4K: Boolean = false
        private set
    @Volatile var supports2K: Boolean = false
        private set
    @Volatile var supports60Fps: Boolean = true
        private set

    // Stream & format configuration
    @Volatile var streamWidth: Int = 1920
        private set
    @Volatile var streamHeight: Int = 1080
        private set
    @Volatile var streamFps: Int = 30
        private set
    @Volatile var streamBitrate: Int = 8_000_000

    @Volatile var isNetworkStreamActive: Boolean = false
        set(value) {
            field = value
            runOnMain { syncEncoderState() }
        }

    // Sensor & Module inventory
    @Volatile var isFrontCameraActive: Boolean = false
        private set
    @Volatile var activeModuleKey: String = "main"
        private set
    @Volatile var intraModuleZoom: Float = 1.0f
        private set
    @Volatile var maxIntraModuleZoom: Float = 6.0f
        private set
    @Volatile var currentZoomRatio: Float = 1.0f
        private set
    @Volatile var minZoomRatio: Float = 1.0f
        private set
    @Volatile var maxZoomRatio: Float = 6.0f
        private set

    val supportedZoomPresets: MutableList<Float> = mutableListOf(1.0f)
    val availableModules: MutableList<CameraModuleInfo> = mutableListOf()

    @Volatile var activeCameraId: String = "0"
        private set
    @Volatile private var selectedPhysicalCameraId: String? = null
    @Volatile private var isPhysicalCameraPinned: Boolean = false

    private var discoveredSensors: List<CameraSensorInfo> = emptyList()
    private var rearLogicalCameraId: String = "0"
    private var frontLogicalCameraId: String? = null
    private val physicalSubCameras = mutableMapOf<String, String>() // physicalId -> parentLogicalId

    // Torch & Exposure Lock & LUT state
    @Volatile var isTorchOn: Boolean = false
        private set
    @Volatile var hasRearFlashUnit: Boolean = false
        private set
    val isTorchAvailable: Boolean
        get() = !isFrontCameraActive && hasRearFlashUnit

    @Volatile var isAeLocked: Boolean = false
        private set
    @Volatile var activeLutName: String = ""
        private set

    // Visual feedback pulse counters (incremented when AF or Auto mode is triggered)
    private val afPulseSeqGen = AtomicLong(0L)
    private val autoModePulseSeqGen = AtomicLong(0L)
    val afTriggerPulseSeq: Long get() = afPulseSeqGen.get()
    val autoModePulseSeq: Long get() = autoModePulseSeqGen.get()

    // Pro / Manual parameters + live captured AWB calibration per lens
    @Volatile var currentMode: CameraMode = CameraMode.Auto
        private set
    @Volatile private var lastNeutralGains: RggbChannelVector? = null
    @Volatile private var lastColorTransform: ColorSpaceTransform? = null
    @Volatile private var lastMeasuredIso: Int? = null
    @Volatile private var lastMeasuredExposureNs: Long? = null
    @Volatile private var activeFpsRange: Range<Int> = Range(15, 30)
    private val captureSampleCounter = AtomicInteger(0)

    // Preview / orientation state
    @Volatile private var uiPreviewSurface: Surface? = null
    @Volatile private var uiPreviewWidth: Int = 0
    @Volatile private var uiPreviewHeight: Int = 0
    @Volatile private var uiDisplayRotation: Int = Surface.ROTATION_0
    @Volatile private var devicePhysicalRotation: Int = Surface.ROTATION_0
    @Volatile private var hasPhysicalOrientationSample: Boolean = false
    @Volatile private var currentInputHasCameraTransform: Boolean = true
    @Volatile var manualStreamRotationOffsetDeg: Int = -1

    // Telemetry counters
    private val frameCounter = AtomicInteger(0)
    private val byteCounter = AtomicLong(0L)
    @Volatile var measuredFps: Float = 0f
        private set
    @Volatile var measuredBitrateKbps: Int = 0
        private set

    // Callbacks
    var onH264FrameAvailable: ((data: ByteArray, isKeyFrame: Boolean, ptsUs: Long) -> Unit)? = null
    var onJpegFrameAvailable: ((ByteArray, Int, Int) -> Unit)? = null
    var onStateChanged: (() -> Unit)? = null
    var onTelemetryMeasured: ((fps: Float, bitrateKbps: Int) -> Unit)? = null

    private val telemetryRunnable = object : Runnable {
        override fun run() {
            val fps = frameCounter.getAndSet(0).toFloat()
            val bytes = byteCounter.getAndSet(0L)
            measuredFps = fps
            measuredBitrateKbps = ((bytes * 8L) / 1000L).toInt()
            onTelemetryMeasured?.invoke(measuredFps, measuredBitrateKbps)
            mainHandler.postDelayed(this, 1000L)
        }
    }

    private val orientationListener = object : OrientationEventListener(context) {
        override fun onOrientationChanged(orientation: Int) {
            if (orientation == ORIENTATION_UNKNOWN) return
            val nextRot = when {
                orientation >= 335 || orientation <= 25 -> Surface.ROTATION_0
                orientation in 65..115 -> Surface.ROTATION_270
                orientation in 155..205 -> Surface.ROTATION_180
                orientation in 245..295 -> Surface.ROTATION_90
                else -> return
            }
            val changed = !hasPhysicalOrientationSample || nextRot != devicePhysicalRotation
            hasPhysicalOrientationSample = true
            if (nextRot != devicePhysicalRotation) {
                devicePhysicalRotation = nextRot
            }
            if (changed) {
                updateTeeTransforms()
            }
        }
    }

    init {
        mainHandler.post {
            fallbackLifecycleOwner.registry.currentState = Lifecycle.State.CREATED
            mainHandler.postDelayed(telemetryRunnable, 1000L)
        }
        eglTee.setOnFrameRenderedListener {
            frameCounter.incrementAndGet()
        }
        if (orientationListener.canDetectOrientation()) {
            orientationListener.enable()
        }
        discoverAvailableSensors()
    }

    fun hasCameraPermission(): Boolean {
        return ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
            PackageManager.PERMISSION_GRANTED
    }

    fun isCameraRunning(): Boolean = cameraOpenRequested

    fun attachLifecycleOwner(owner: LifecycleOwner) {
        attachedLifecycleOwner = owner
        runOnMain {
            if (cameraOpenRequested && hasCameraPermission()) {
                bindOrRebindCamera()
            }
        }
    }

    fun getCurrentCharacteristics(): CameraCharacteristics? {
        return try {
            val targetId = selectedPhysicalCameraId ?: activeCameraId
            cameraManager.getCameraCharacteristics(targetId)
        } catch (_: Exception) {
            try {
                cameraManager.getCameraCharacteristics(activeCameraId)
            } catch (_: Exception) {
                null
            }
        }
    }

    /**
     * Discovers all logical and physical camera sensors, hardware encoder limits, budget/legacy
     * status, and builds explicit selectable [CameraModuleInfo] descriptors.
     */
    @Synchronized
    fun discoverAvailableSensors(): List<CameraSensorInfo> {
        val avcCaps = HardwareEncoder.probeAvcCapabilities()
        val actMgr = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
        val memInfo = ActivityManager.MemoryInfo()
        actMgr?.getMemoryInfo(memInfo)
        val totalRamMb = if (memInfo.totalMem > 0L) (memInfo.totalMem / (1024L * 1024L)).toInt() else 4096
        val isLowRam = (actMgr?.isLowRamDevice == true) || totalRamMb in 1..3500

        val result = ArrayList<CameraSensorInfo>()
        val foundIds = HashSet<String>()
        physicalSubCameras.clear()

        var bestRearLogical = "0"
        var bestRearSpan = -1f
        var fallbackFront: String? = null
        var anyCam4K = false
        var anyCam2K = false
        var anyCam60Fps = false
        var rearFlashFound = false
        var primaryHwLevel = CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED

        data class RawCandidate(
            val id: String,
            val parentLogicalId: String?,
            val chars: CameraCharacteristics,
            val sensorType: SensorType,
            val facing: Int,
            val focalLength: Float,
            val maxDigitalZoom: Float
        )
        val rearCandidates = ArrayList<RawCandidate>()

        try {
            for (camId in cameraManager.cameraIdList) {
                val chars = cameraManager.getCameraCharacteristics(camId)
                val facing = chars.get(CameraCharacteristics.LENS_FACING)
                    ?: CameraCharacteristics.LENS_FACING_BACK
                val caps = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: IntArray(0)
                val isLogical = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P &&
                    caps.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA)
                val physIds: Set<String> = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                    chars.physicalCameraIds
                } else {
                    emptySet()
                }
                val sensorType = CameraUtils.classifySensor(chars)
                val focalLengths = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
                    ?: floatArrayOf(4.0f)
                val minFocus = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
                val isoRangeRaw = chars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
                val expRangeRaw = chars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)
                val maxDigZoom = (chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 4.0f)
                    .coerceIn(2.0f, 10.0f)

                val zoomSpan = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    val zr = chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
                    if (zr != null) zr.upper - zr.lower else maxDigZoom - 1.0f
                } else {
                    maxDigZoom - 1.0f
                }

                if (facing == CameraCharacteristics.LENS_FACING_BACK) {
                    if (chars.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true) {
                        rearFlashFound = true
                    }
                    if (supportsOutputSize(chars, 3840, 2160)) anyCam4K = true
                    if (supportsOutputSize(chars, 2560, 1440)) anyCam2K = true
                    val fpsRanges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
                    if (fpsRanges?.any { it.upper >= 60 } == true) {
                        anyCam60Fps = true
                    }
                }

                if (foundIds.add(camId)) {
                    result.add(
                        CameraSensorInfo(
                            id = camId,
                            isLogical = isLogical,
                            physicalCameraIds = physIds,
                            sensorType = sensorType,
                            lensFacing = facing,
                            focalLengths = focalLengths,
                            minFocusDistance = minFocus,
                            isoRange = (isoRangeRaw?.lower ?: 50)..(isoRangeRaw?.upper ?: 3200),
                            exposureTimeRangeNs = (expRangeRaw?.lower ?: 100_000L)..(expRangeRaw?.upper ?: 1_000_000_000L)
                        )
                    )
                }

                if (facing == CameraCharacteristics.LENS_FACING_FRONT) {
                    if (fallbackFront == null) fallbackFront = camId
                } else {
                    val score = zoomSpan + (if (isLogical) physIds.size * 10f else 0f)
                    if (score > bestRearSpan) {
                        bestRearSpan = score
                        bestRearLogical = camId
                        primaryHwLevel = chars.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)
                            ?: CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED
                    }
                    if (!isLogical || physIds.isEmpty()) {
                        rearCandidates.add(
                            RawCandidate(
                                id = camId,
                                parentLogicalId = null,
                                chars = chars,
                                sensorType = sensorType,
                                facing = facing,
                                focalLength = focalLengths.firstOrNull() ?: 4.0f,
                                maxDigitalZoom = maxDigZoom
                            )
                        )
                    }
                }

                for (physId in physIds) {
                    physicalSubCameras[physId] = camId
                    try {
                        val pChars = cameraManager.getCameraCharacteristics(physId)
                        val pType = CameraUtils.classifySensor(pChars)
                        val pFocal = pChars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
                            ?: floatArrayOf(4.0f)
                        val pMinFocus = pChars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
                        val pIso = pChars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
                        val pExp = pChars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)
                        val pMaxDig = (pChars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 4.0f)
                            .coerceIn(2.0f, 10.0f)

                        if (foundIds.add(physId)) {
                            result.add(
                                CameraSensorInfo(
                                    id = physId,
                                    isLogical = false,
                                    physicalCameraIds = emptySet(),
                                    sensorType = pType,
                                    lensFacing = facing,
                                    focalLengths = pFocal,
                                    minFocusDistance = pMinFocus,
                                    isoRange = (pIso?.lower ?: 50)..(pIso?.upper ?: 3200),
                                    exposureTimeRangeNs = (pExp?.lower ?: 100_000L)..(pExp?.upper ?: 1_000_000_000L)
                                )
                            )
                        }
                        if (facing == CameraCharacteristics.LENS_FACING_BACK) {
                            rearCandidates.add(
                                RawCandidate(
                                    id = physId,
                                    parentLogicalId = camId,
                                    chars = pChars,
                                    sensorType = pType,
                                    facing = facing,
                                    focalLength = pFocal.firstOrNull() ?: 4.0f,
                                    maxDigitalZoom = pMaxDig
                                )
                            )
                        }
                    } catch (_: Exception) {
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed discovering sensors", e)
        }

        rearLogicalCameraId = bestRearLogical
        frontLogicalCameraId = fallbackFront
        hasRearFlashUnit = rearFlashFound
        discoveredSensors = result

        val isLegacyOrLimited = primaryHwLevel == CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY ||
            primaryHwLevel == CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED
        isLowEndDevice = isLowRam || isLegacyOrLimited || !avcCaps.supports60Fps1080p
        supports4K = avcCaps.supports4K && anyCam4K && !isLowEndDevice
        supports2K = avcCaps.supports2K && (anyCam2K || anyCam4K) && !isLowEndDevice
        supports60Fps = avcCaps.supports60Fps1080p && anyCam60Fps && !isLowEndDevice

        eglTee.lowEndPreviewThrottle = isLowEndDevice
        if (isLowEndDevice && streamFps > 30) {
            streamFps = 30
            streamBitrate = defaultBitrateForResolution(streamWidth, streamFps, isLowEnd = true)
        } else if (!isLowEndDevice && supports60Fps && streamFps == 30 && !cameraOpenRequested) {
            streamFps = 60
            streamBitrate = defaultBitrateForResolution(streamWidth, streamFps, isLowEnd = false)
        }

        // Build structured CameraModuleInfo list
        buildCameraModules(rearCandidates)
        return result
    }

    private fun buildCameraModules(
        rearCandidates: List<Any>
    ) {
        // Check logical zoom range of primary rear camera
        var rearLogicalMinZ = 1.0f
        var rearLogicalMaxZ = 4.0f
        try {
            val rChars = cameraManager.getCameraCharacteristics(rearLogicalCameraId)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                rChars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)?.let {
                    rearLogicalMinZ = it.lower
                    rearLogicalMaxZ = it.upper
                }
            } else {
                rearLogicalMaxZ = rChars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 4.0f
            }
        } catch (_: Exception) {
        }

        val rearSensorInfos = discoveredSensors.filter {
            it.lensFacing == CameraCharacteristics.LENS_FACING_BACK && (!it.isLogical || it.physicalCameraIds.isEmpty())
        }

        val uwSensor = rearSensorInfos
            .filter { it.sensorType == SensorType.ULTRA_WIDE }
            .minByOrNull { it.focalLengths.firstOrNull() ?: 2.0f }

        val mainSensor = rearSensorInfos
            .filter { it.sensorType == SensorType.WIDE }
            .minByOrNull { abs((it.focalLengths.firstOrNull() ?: 4.5f) - 4.5f) }
            ?: rearSensorInfos.firstOrNull { it.id == rearLogicalCameraId }

        val teleSensors = rearSensorInfos
            .filter { it.sensorType == SensorType.TELEPHOTO }
            .sortedBy { it.focalLengths.firstOrNull() ?: 6.0f }

        val macroSensor = rearSensorInfos
            .firstOrNull { it.sensorType == SensorType.MACRO && it.id != uwSensor?.id && it.id != mainSensor?.id }

        val modules = ArrayList<CameraModuleInfo>()

        // 1. Ultra-Wide (0.5x) — only added if physical/standalone UW exists OR logical zoom < 0.85x
        if (uwSensor != null || rearLogicalMinZ <= 0.85f) {
            val parentLog = uwSensor?.let { physicalSubCameras[it.id] }
            val logId = parentLog ?: uwSensor?.id ?: rearLogicalCameraId
            val physId = if (parentLog != null) uwSensor.id else null
            modules.add(
                CameraModuleInfo(
                    key = "uw",
                    label = "0.5x UW",
                    shortLabel = "0.5x",
                    logicalCameraId = logId,
                    physicalCameraId = physId,
                    sensorType = SensorType.ULTRA_WIDE,
                    isFront = false,
                    baseOpticalZoom = 0.5f,
                    maxIntraZoom = 3.0f,
                    logicalBandMin = rearLogicalMinZ.coerceAtMost(0.6f),
                    logicalBandMax = 0.96f
                )
            )
        }

        // 2. Main Wide (1x) — always present on rear camera
        run {
            val parentLog = mainSensor?.let { physicalSubCameras[it.id] }
            val logId = parentLog ?: mainSensor?.id ?: rearLogicalCameraId
            val physId = if (parentLog != null) mainSensor.id else null
            val hasTele = teleSensors.isNotEmpty()
            modules.add(
                CameraModuleInfo(
                    key = "main",
                    label = "1x Main",
                    shortLabel = "1x",
                    logicalCameraId = logId,
                    physicalCameraId = physId,
                    sensorType = SensorType.WIDE,
                    isFront = false,
                    baseOpticalZoom = 1.0f,
                    maxIntraZoom = if (isLowEndDevice) 4.0f else 6.0f,
                    logicalBandMin = 1.0f,
                    logicalBandMax = if (hasTele) 1.95f else rearLogicalMaxZ.coerceIn(2.0f, 8.0f)
                )
            )
        }

        // 3. Primary Telephoto (2x / 3x) — only if hardware telephoto sensor exists
        val firstTele = teleSensors.firstOrNull()
        if (firstTele != null) {
            val parentLog = physicalSubCameras[firstTele.id]
            val logId = parentLog ?: firstTele.id
            val physId = if (parentLog != null) firstTele.id else null
            val mainFocal = mainSensor?.focalLengths?.firstOrNull() ?: 4.3f
            val teleFocal = firstTele.focalLengths.firstOrNull() ?: 9.0f
            val optRatio = (teleFocal / mainFocal).coerceIn(1.8f, 4.0f)
            val displayRatio = if (optRatio >= 2.6f) 3 else 2
            val hasSecondTele = teleSensors.size >= 2
            modules.add(
                CameraModuleInfo(
                    key = "tele",
                    label = "${displayRatio}x Tele",
                    shortLabel = "${displayRatio}x",
                    logicalCameraId = logId,
                    physicalCameraId = physId,
                    sensorType = SensorType.TELEPHOTO,
                    isFront = false,
                    baseOpticalZoom = displayRatio.toFloat(),
                    maxIntraZoom = 6.0f,
                    logicalBandMin = displayRatio.toFloat(),
                    logicalBandMax = if (hasSecondTele) 4.9f else rearLogicalMaxZ.coerceAtLeast(displayRatio * 3f)
                )
            )
        }

        // 4. Secondary Super-Telephoto (5x/10x) OR Dedicated Macro
        val secondTele = teleSensors.getOrNull(1)
        if (secondTele != null) {
            val parentLog = physicalSubCameras[secondTele.id]
            val logId = parentLog ?: secondTele.id
            val physId = if (parentLog != null) secondTele.id else null
            modules.add(
                CameraModuleInfo(
                    key = "tele5x",
                    label = "5x Tele",
                    shortLabel = "5x",
                    logicalCameraId = logId,
                    physicalCameraId = physId,
                    sensorType = SensorType.TELEPHOTO,
                    isFront = false,
                    baseOpticalZoom = 5.0f,
                    maxIntraZoom = 8.0f,
                    logicalBandMin = 5.0f,
                    logicalBandMax = rearLogicalMaxZ.coerceAtLeast(10.0f)
                )
            )
        } else if (macroSensor != null) {
            val parentLog = physicalSubCameras[macroSensor.id]
            val logId = parentLog ?: macroSensor.id
            val physId = if (parentLog != null) macroSensor.id else null
            modules.add(
                CameraModuleInfo(
                    key = "macro",
                    label = "Macro",
                    shortLabel = "MC",
                    logicalCameraId = logId,
                    physicalCameraId = physId,
                    sensorType = SensorType.MACRO,
                    isFront = false,
                    baseOpticalZoom = 1.0f,
                    maxIntraZoom = 3.0f,
                    logicalBandMin = 1.0f,
                    logicalBandMax = 3.0f
                )
            )
        }

        // 5. Front Camera (SELF / Front)
        frontLogicalCameraId?.let { frontId ->
            modules.add(
                CameraModuleInfo(
                    key = "front",
                    label = "Front",
                    shortLabel = "SELF",
                    logicalCameraId = frontId,
                    physicalCameraId = null,
                    sensorType = SensorType.FRONT,
                    isFront = true,
                    baseOpticalZoom = 1.0f,
                    maxIntraZoom = 3.0f,
                    logicalBandMin = 1.0f,
                    logicalBandMax = 3.0f
                )
            )
        }

        availableModules.clear()
        availableModules.addAll(modules)

        supportedZoomPresets.clear()
        for (m in modules) {
            if (!m.isFront) {
                supportedZoomPresets.add(m.baseOpticalZoom)
            }
        }
        if (supportedZoomPresets.isEmpty()) {
            supportedZoomPresets.add(1.0f)
        }

        val currentMod = availableModules.find { it.key == activeModuleKey }
            ?: availableModules.find { it.key == "main" }
            ?: availableModules.firstOrNull()
        if (currentMod != null) {
            activeModuleKey = currentMod.key
            maxIntraModuleZoom = currentMod.maxIntraZoom
            minZoomRatio = 1.0f
            maxZoomRatio = currentMod.maxIntraZoom
        }
    }

    private fun supportsOutputSize(chars: CameraCharacteristics, reqW: Int, reqH: Int): Boolean {
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return false
        val sizes = map.getOutputSizes(SurfaceTexture::class.java) ?: return false
        return sizes.any { (it.width >= reqW && it.height >= reqH) || (it.height >= reqW && it.width >= reqH) }
    }

    fun openCamera(
        logicalCameraId: String = rearLogicalCameraId,
        previewSurface: Surface? = null,
        encoderSurface: Surface? = null
    ) {
        if (!hasCameraPermission()) {
            Log.w(TAG, "openCamera ignored: CAMERA permission missing")
            return
        }
        if (discoveredSensors.isEmpty()) discoverAvailableSensors()

        cameraOpenRequested = true
        val initialModule = if (logicalCameraId == frontLogicalCameraId) {
            availableModules.find { it.isFront }
        } else {
            availableModules.find { it.key == activeModuleKey } ?: availableModules.find { it.key == "main" }
        }

        if (initialModule != null) {
            activeModuleKey = initialModule.key
            activeCameraId = initialModule.logicalCameraId
            selectedPhysicalCameraId = initialModule.physicalCameraId
            isFrontCameraActive = initialModule.isFront
            maxIntraModuleZoom = initialModule.maxIntraZoom
            minZoomRatio = 1.0f
            maxZoomRatio = initialModule.maxIntraZoom
        } else {
            activeCameraId = logicalCameraId
            isFrontCameraActive = logicalCameraId == frontLogicalCameraId
            selectedPhysicalCameraId = null
        }

        if (isFrontCameraActive) {
            isTorchOn = false
        }

        if (previewSurface != null) {
            uiPreviewSurface = previewSurface
        }
        externalEncoderSurface = encoderSurface
        if (encoderSurface != null && encoderSurface.isValid) {
            eglTee.setEncoderSurface(encoderSurface, streamWidth, streamHeight)
        }

        runOnMain {
            if (attachedLifecycleOwner == null) {
                fallbackLifecycleOwner.registry.currentState = Lifecycle.State.RESUMED
            }
            syncEncoderState()
            bindOrRebindCamera()
        }
    }

    fun updatePreviewSurface(
        surface: Surface?,
        width: Int = 0,
        height: Int = 0,
        displayRotation: Int = uiDisplayRotation
    ) {
        uiPreviewSurface = surface
        if (width > 0 && height > 0) {
            uiPreviewWidth = width
            uiPreviewHeight = height
        }
        uiDisplayRotation = displayRotation
        if (!hasPhysicalOrientationSample) {
            devicePhysicalRotation = displayRotation
        }

        val targetW = if (width > 0) width else uiPreviewWidth.coerceAtLeast(720)
        val targetH = if (height > 0) height else uiPreviewHeight.coerceAtLeast(1280)
        eglTee.setPreviewSurface(surface, targetW, targetH, await = (surface == null))
        updateTeeTransforms()
    }

    fun setStreamRotationOffset(degrees: Int) {
        manualStreamRotationOffsetDeg = if (degrees < 0) -1 else ((degrees % 360) + 360) % 360
        updateTeeTransforms()
    }

    fun switchFrontBack(useFront: Boolean) {
        val targetKey = if (useFront) "front" else "main"
        selectCameraModule(targetKey)
    }

    /**
     * Switches directly to a specific camera module ("uw", "main", "tele", "tele5x", "macro", "front",
     * or preset strings like "0.5x", "1x", "2x", "3x", "5x", "10x") and resets intra-module zoom to 1.0x.
     */
    fun selectCameraModule(moduleOrPreset: String) {
        val norm = moduleOrPreset.trim().lowercase()
        if (availableModules.isEmpty()) discoverAvailableSensors()

        val matched = availableModules.find {
            it.key.equals(norm, ignoreCase = true) ||
                it.shortLabel.equals(norm, ignoreCase = true) ||
                it.label.equals(norm, ignoreCase = true) ||
                (it.physicalCameraId != null && it.physicalCameraId == moduleOrPreset.trim()) ||
                (it.physicalCameraId == null && it.logicalCameraId == moduleOrPreset.trim())
        } ?: when {
            norm == "front" || norm == "self" -> availableModules.find { it.isFront }
            norm.startsWith("0.5") || norm == "uw" || norm == "ultrawide" -> availableModules.find { it.key == "uw" }
            norm.startsWith("1.0") || norm == "1x" || norm == "1" || norm == "main" || norm == "wide" ->
                availableModules.find { it.key == "main" }
            norm.startsWith("2") || norm.startsWith("3") || norm == "tele" ->
                availableModules.find { it.key == "tele" }
            norm.startsWith("5") || norm.startsWith("10") || norm == "tele5x" || norm == "periscope" ->
                availableModules.find { it.key == "tele5x" } ?: availableModules.find { it.key == "tele" }
            norm == "macro" || norm == "mc" -> availableModules.find { it.key == "macro" }
            else -> null
        }

        if (matched == null) {
            Log.w(TAG, "Requested camera module '$moduleOrPreset' is not available on this device")
            return
        }

        val changed = activeModuleKey != matched.key ||
            activeCameraId != matched.logicalCameraId ||
            selectedPhysicalCameraId != matched.physicalCameraId ||
            isFrontCameraActive != matched.isFront ||
            boundCamera == null

        activeModuleKey = matched.key
        activeCameraId = matched.logicalCameraId
        selectedPhysicalCameraId = matched.physicalCameraId
        isFrontCameraActive = matched.isFront
        if (isFrontCameraActive) {
            isTorchOn = false
        }
        intraModuleZoom = 1.0f
        currentZoomRatio = 1.0f
        minZoomRatio = 1.0f
        maxZoomRatio = matched.maxIntraZoom
        maxIntraModuleZoom = matched.maxIntraZoom
        lastNeutralGains = null
        lastColorTransform = null

        runOnMain {
            if (cameraOpenRequested) {
                if (changed) {
                    bindOrRebindCamera()
                } else {
                    applyIntraModuleZoomInternal(1.0f)
                }
            }
            onStateChanged?.invoke()
        }
    }

    fun switchToSensor(moduleOrId: String) {
        selectCameraModule(moduleOrId)
    }

    fun switchPhysicalCameraSeamless(physicalId: String) {
        selectCameraModule(physicalId)
    }

    /**
     * Adjusts zoom strictly inside the currently selected camera module (`1.0x .. maxIntraModuleZoom`)
     * WITHOUT triggering automatic multi-camera lens switching.
     */
    fun setZoomRatio(zoom: Float) {
        val clamped = zoom.coerceIn(1.0f, maxIntraModuleZoom.coerceAtLeast(1.0f))
        intraModuleZoom = clamped
        currentZoomRatio = clamped

        runOnMain {
            applyIntraModuleZoomInternal(clamped)
            onStateChanged?.invoke()
        }
    }

    private fun applyIntraModuleZoomInternal(intraZoom: Float) {
        val cam = boundCamera ?: return
        val mod = availableModules.find { it.key == activeModuleKey }
        val clampedIntra = intraZoom.coerceIn(1.0f, (mod?.maxIntraZoom ?: maxIntraModuleZoom).coerceAtLeast(1.0f))
        intraModuleZoom = clampedIntra
        currentZoomRatio = clampedIntra

        if (isPhysicalCameraPinned || selectedPhysicalCameraId == null && mod?.baseOpticalZoom == 1.0f && availableModules.count { !it.isFront } <= 1) {
            // Single physical sensor bound: apply crop/zoom directly on this sensor
            val zoomState = cam.cameraInfo.zoomState.value
            val minZ = zoomState?.minZoomRatio ?: 1.0f
            val maxZ = zoomState?.maxZoomRatio ?: maxIntraModuleZoom
            cam.cameraControl.setZoomRatio(clampedIntra.coerceIn(minZ.coerceAtLeast(1.0f), maxZ))
            applyCurrentCameraControls()
        } else if (mod != null && !mod.isFront) {
            // Bound to logical multi-camera: stay strictly within this module's optical band so HAL never switches lenses
            val bandMin = mod.logicalBandMin
            val bandMax = mod.logicalBandMax
            val norm = ((clampedIntra - 1.0f) / (mod.maxIntraZoom - 1.0f).coerceAtLeast(0.1f)).coerceIn(0f, 1f)
            val targetLogicalZoom = (bandMin + norm * (bandMax - bandMin)).coerceIn(bandMin, bandMax)
            cam.cameraControl.setZoomRatio(targetLogicalZoom)
        } else {
            val zoomState = cam.cameraInfo.zoomState.value
            val maxZ = zoomState?.maxZoomRatio ?: 3.0f
            cam.cameraControl.setZoomRatio(clampedIntra.coerceIn(1.0f, maxZ))
        }
    }

    /**
     * Enables or disables the smartphone rear flashlight (torch).
     * Automatically disabled when Front camera is active.
     */
    fun setTorchEnabled(enabled: Boolean) {
        if (isFrontCameraActive || !hasRearFlashUnit) {
            isTorchOn = false
            runOnMain {
                try {
                    boundCamera?.cameraControl?.enableTorch(false)
                } catch (_: Exception) {}
                onStateChanged?.invoke()
            }
            return
        }
        isTorchOn = enabled
        runOnMain {
            try {
                boundCamera?.cameraControl?.enableTorch(enabled)
            } catch (e: Exception) {
                Log.w(TAG, "enableTorch($enabled) failed: ${e.message}")
            }
            onStateChanged?.invoke()
        }
    }

    fun toggleTorch() {
        setTorchEnabled(!isTorchOn)
    }

    /**
     * Locks or unlocks the current Auto Exposure (AE Lock).
     */
    fun setAeLocked(locked: Boolean) {
        isAeLocked = locked
        runOnMain {
            applyCurrentCameraControls()
            onStateChanged?.invoke()
        }
    }

    fun toggleAeLock() {
        setAeLocked(!isAeLocked)
    }

    /**
     * Loads and applies a `.cube` 3D LUT from [inputStream] into the OpenGL ES shader pipeline.
     */
    fun loadCubeLut(name: String, inputStream: InputStream): Boolean {
        return try {
            val rgbaBuf = EglVideoTee.parseCubeLutToRgba32(inputStream)
            if (rgbaBuf != null) {
                eglTee.setLutRgba32(rgbaBuf)
                activeLutName = name
                runOnMain { onStateChanged?.invoke() }
                true
            } else {
                false
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed loading .cube LUT '$name'", e)
            false
        }
    }

    fun clearCubeLut() {
        eglTee.setLutRgba32(null)
        activeLutName = ""
        runOnMain { onStateChanged?.invoke() }
    }

    fun setResolution(width: Int, height: Int, fps: Int) {
        val rawCfg = EncoderConfig(
            width = width.coerceAtLeast(320),
            height = height.coerceAtLeast(240),
            frameRate = if (isLowEndDevice) fps.coerceIn(15, 30) else fps.coerceIn(15, 60),
            bitrate = defaultBitrateForResolution(width, fps, isLowEndDevice)
        )
        val sanitized = HardwareEncoder.sanitizeEncoderConfig(rawCfg)
        val w = sanitized.width
        val h = sanitized.height
        val f = if (isLowEndDevice) sanitized.frameRate.coerceAtMost(30) else sanitized.frameRate
        val changed = (w != streamWidth || h != streamHeight || f != streamFps)

        streamWidth = w
        streamHeight = h
        streamFps = f
        streamBitrate = defaultBitrateForResolution(w, f, isLowEndDevice).coerceAtMost(sanitized.bitrate)

        if (!changed) {
            runOnMain { onStateChanged?.invoke() }
            return
        }
        runOnMain {
            if (isNetworkStreamActive) {
                syncEncoderState(forceRecreate = true)
            }
            if (cameraOpenRequested) {
                bindOrRebindCamera()
            }
            onStateChanged?.invoke()
        }
    }

    fun updateManualIso(iso: Int) {
        val base = currentMode as? CameraMode.Manual ?: CameraMode.Manual()
        if (iso > 0) isAeLocked = false
        setMode(base.copy(iso = iso.coerceAtLeast(0)).normalized())
    }

    fun updateManualExposure(exposureTimeNs: Long) {
        val base = currentMode as? CameraMode.Manual ?: CameraMode.Manual()
        if (exposureTimeNs > 0L) isAeLocked = false
        setMode(base.copy(exposureTimeNs = exposureTimeNs.coerceAtLeast(0L)).normalized())
    }

    fun updateManualFocus(focusDistanceDiopters: Float) {
        val base = currentMode as? CameraMode.Manual ?: CameraMode.Manual()
        setMode(base.copy(focusDistance = focusDistanceDiopters.coerceAtLeast(0f)).normalized())
    }

    fun updateManualWhiteBalance(kelvin: Int) {
        val base = currentMode as? CameraMode.Manual ?: CameraMode.Manual()
        setMode(base.copy(whiteBalanceKelvin = kelvin.coerceAtLeast(0)).normalized())
    }

    fun setMode(mode: CameraMode) {
        val norm = mode.normalized()
        if (norm is CameraMode.Auto) {
            autoModePulseSeqGen.incrementAndGet()
        }
        currentMode = norm
        runOnMain {
            applyCurrentCameraControls()
            onStateChanged?.invoke()
        }
    }

    fun resetAllToAuto() {
        isAeLocked = false
        autoModePulseSeqGen.incrementAndGet()
        currentMode = CameraMode.Auto
        runOnMain {
            applyCurrentCameraControls()
            onStateChanged?.invoke()
        }
    }

    fun triggerAutoFocus() {
        afPulseSeqGen.incrementAndGet()
        // If focus was manually locked, unlock focus so autofocus actually engages
        val mode = currentMode
        if (mode is CameraMode.Manual && mode.focusDistance > 0f) {
            currentMode = mode.copy(focusDistance = 0f).normalized()
        }
        runOnMain {
            applyCurrentCameraControls()
            val cam = boundCamera
            if (cam != null) {
                val factory = SurfaceOrientedMeteringPointFactory(1f, 1f)
                val point = factory.createPoint(0.5f, 0.5f)
                val action = FocusMeteringAction.Builder(point, FocusMeteringAction.FLAG_AF or FocusMeteringAction.FLAG_AE)
                    .setAutoCancelDuration(FOCUS_DURATION_SEC, TimeUnit.SECONDS)
                    .build()
                cam.cameraControl.startFocusAndMetering(action)
            }
            onStateChanged?.invoke()
        }
    }

    fun triggerFocusAtPoint(normX: Float, normY: Float) {
        val x = normX.coerceIn(0f, 1f)
        val y = normY.coerceIn(0f, 1f)
        afPulseSeqGen.incrementAndGet()
        val mode = currentMode
        if (mode is CameraMode.Manual && mode.focusDistance > 0f) {
            currentMode = mode.copy(focusDistance = 0f).normalized()
        }
        runOnMain {
            applyCurrentCameraControls()
            val cam = boundCamera
            if (cam != null) {
                val factory = SurfaceOrientedMeteringPointFactory(1f, 1f)
                val point = factory.createPoint(x, y)
                val action = FocusMeteringAction.Builder(point, FocusMeteringAction.FLAG_AF or FocusMeteringAction.FLAG_AE)
                    .setAutoCancelDuration(FOCUS_DURATION_SEC, TimeUnit.SECONDS)
                    .build()
                cam.cameraControl.startFocusAndMetering(action)
            }
            onStateChanged?.invoke()
        }
    }

    fun requestKeyFrame() {
        hardwareEncoder?.requestSyncFrame()
    }

    fun closeCamera() {
        cameraOpenRequested = false
        runOnMain {
            try {
                cameraProvider?.unbindAll()
            } catch (_: Exception) {
            }
            boundCamera = null
            previewUseCase = null
            if (attachedLifecycleOwner == null) {
                fallbackLifecycleOwner.registry.currentState = Lifecycle.State.CREATED
            }
            stopInternalEncoder()
        }
    }

    fun release() {
        mainHandler.removeCallbacks(telemetryRunnable)
        orientationListener.disable()
        closeCamera()
        eglTee.release()
    }

    // ---------------------------------------------------------------------------------------------
    // Internal CameraX binding & hardware H.264 lifecycle (always runs on Main Thread)
    // ---------------------------------------------------------------------------------------------

    private fun bindOrRebindCamera() {
        val provider = cameraProvider
        if (provider == null) {
            val future = ProcessCameraProvider.getInstance(context)
            future.addListener({
                try {
                    cameraProvider = future.get()
                    if (cameraOpenRequested) {
                        bindOrRebindCamera()
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "ProcessCameraProvider initialization failed", e)
                }
            }, mainExecutor)
            return
        }

        val owner = attachedLifecycleOwner ?: fallbackLifecycleOwner
        if (owner.lifecycle.currentState == Lifecycle.State.DESTROYED) return

        val attempts = buildBindAttempts()

        for ((index, attempt) in attempts.withIndex()) {
            try {
                provider.unbindAll()

                val previewBuilder = Preview.Builder()
                    .setTargetRotation(Surface.ROTATION_90)
                    .setResolutionSelector(buildResolutionSelector(streamWidth, streamHeight))

                val interop = Camera2Interop.Extender(previewBuilder)
                activeFpsRange = attempt.fpsRange
                interop.setCaptureRequestOption(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, attempt.fpsRange)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && !attempt.physicalId.isNullOrBlank()) {
                    interop.setPhysicalCameraId(attempt.physicalId)
                }
                interop.setSessionCaptureCallback(object : CameraCaptureSession.CaptureCallback() {
                    override fun onCaptureCompleted(
                        session: CameraCaptureSession,
                        request: CaptureRequest,
                        result: TotalCaptureResult
                    ) {
                        // Sample once every 16 frames to avoid UI thread overhead on budget devices
                        if ((captureSampleCounter.incrementAndGet() and 0x0F) != 0) return
                        val mode = currentMode
                        if (!mode.isWbManual) {
                            result.get(CaptureResult.COLOR_CORRECTION_GAINS)?.let { lastNeutralGains = it }
                            result.get(CaptureResult.COLOR_CORRECTION_TRANSFORM)?.let { lastColorTransform = it }
                        }
                        if (!mode.isIsoManual) {
                            result.get(CaptureResult.SENSOR_SENSITIVITY)?.let { lastMeasuredIso = it }
                        }
                        if (!mode.isShutterManual) {
                            result.get(CaptureResult.SENSOR_EXPOSURE_TIME)?.let { lastMeasuredExposureNs = it }
                        }
                    }
                })

                val preview = previewBuilder.build()
                preview.setSurfaceProvider(mainExecutor) { request ->
                    val size = request.resolution
                    request.setTransformationInfoListener(mainExecutor) { info ->
                        val hasCamTransform = info.hasCameraTransform()
                        if (currentInputHasCameraTransform != hasCamTransform) {
                            currentInputHasCameraTransform = hasCamTransform
                            updateTeeTransforms()
                        }
                    }
                    eglTee.acquireInputSurface(size.width, size.height) { inputSurface ->
                        request.provideSurface(inputSurface, mainExecutor) {
                            eglTee.releaseInputSurface(inputSurface)
                        }
                    }
                }

                val camera = provider.bindToLifecycle(owner, attempt.selector, preview)
                boundCamera = camera
                previewUseCase = preview
                isPhysicalCameraPinned = !attempt.physicalId.isNullOrBlank()

                applyIntraModuleZoomInternal(intraModuleZoom)
                if (isTorchOn && !isFrontCameraActive && hasRearFlashUnit) {
                    try {
                        camera.cameraControl.enableTorch(true)
                    } catch (_: Exception) {}
                }
                updateTeeTransforms()
                applyCurrentCameraControls()
                onStateChanged?.invoke()
                Log.i(
                    TAG,
                    "Bound CameraX (module=$activeModuleKey cam=$activeCameraId phys=${attempt.physicalId} " +
                        "target=${streamWidth}x${streamHeight}@${attempt.fpsRange}, intraZoom=$intraModuleZoom)"
                )
                return
            } catch (e: Exception) {
                Log.w(TAG, "CameraX bind attempt #${index + 1} failed: ${e.message}")
            }
        }
        Log.e(TAG, "All CameraX bind attempts failed")
    }

    private data class BindAttempt(
        val selector: CameraSelector,
        val fpsRange: Range<Int>,
        val physicalId: String?
    )

    private fun buildBindAttempts(): List<BindAttempt> {
        val targetId = selectedPhysicalCameraId ?: activeCameraId
        val preferredFps = selectBestFpsRange(targetId, streamFps)
        val safe30Fps = Range(15, 30)

        val attempts = ArrayList<BindAttempt>()

        // 1. Exact CameraX selector (including physical ID if requested)
        val primarySelector = buildCameraSelector(activeCameraId, selectedPhysicalCameraId)
        attempts.add(BindAttempt(primarySelector, preferredFps, selectedPhysicalCameraId))
        if (preferredFps.upper > 30) {
            attempts.add(BindAttempt(primarySelector, Range(30, 30), selectedPhysicalCameraId))
            attempts.add(BindAttempt(primarySelector, safe30Fps, selectedPhysicalCameraId))
        }

        // 2. Fallback to parent logical camera without physical pinning (if OEM HAL rejects physical pin)
        if (selectedPhysicalCameraId != null) {
            val logicalSelector = buildCameraSelector(activeCameraId, null)
            attempts.add(BindAttempt(logicalSelector, preferredFps, null))
            attempts.add(BindAttempt(logicalSelector, Range(30, 30), null))
            attempts.add(BindAttempt(logicalSelector, safe30Fps, null))
        }

        // 3. Fallback to standard CameraSelector lens facing
        val fallbackSelector = if (isFrontCameraActive) {
            CameraSelector.DEFAULT_FRONT_CAMERA
        } else {
            CameraSelector.DEFAULT_BACK_CAMERA
        }
        attempts.add(BindAttempt(fallbackSelector, Range(30, 30), null))
        attempts.add(BindAttempt(fallbackSelector, safe30Fps, null))
        return attempts
    }

    @SuppressLint("RestrictedApi")
    private fun buildCameraSelector(logicalId: String, physicalId: String?): CameraSelector {
        val builder = CameraSelector.Builder()
            .addCameraFilter(CameraFilter { cameraInfos ->
                val filtered = cameraInfos.filter { info ->
                    try {
                        Camera2CameraInfo.from(info).cameraId == logicalId
                    } catch (_: Exception) {
                        false
                    }
                }
                filtered.ifEmpty { cameraInfos }
            })
        if (!physicalId.isNullOrBlank()) {
            builder.setPhysicalCameraId(physicalId)
        }
        return builder.build()
    }

    private fun buildResolutionSelector(targetW: Int, targetH: Int): ResolutionSelector {
        val targetSize = Size(targetW, targetH)
        return ResolutionSelector.Builder()
            .setAspectRatioStrategy(AspectRatioStrategy.RATIO_16_9_FALLBACK_AUTO_STRATEGY)
            .setResolutionStrategy(
                ResolutionStrategy(targetSize, ResolutionStrategy.FALLBACK_RULE_CLOSEST_HIGHER_THEN_LOWER)
            )
            .setResolutionFilter(ResolutionFilter { supportedSizes, _ ->
                if (supportedSizes.isEmpty()) return@ResolutionFilter supportedSizes
                // On budget/low-end devices, never pick a camera preview buffer larger than targetW x targetH
                val cappedSizes = if (isLowEndDevice) {
                    val withinTarget = supportedSizes.filter {
                        maxOf(it.width, it.height) <= maxOf(targetW, targetH)
                    }
                    withinTarget.ifEmpty { supportedSizes }
                } else {
                    supportedSizes
                }
                val sorted = cappedSizes.sortedWith(
                    compareBy<Size>(
                        {
                            val longSide = maxOf(it.width, it.height)
                            val shortSide = minOf(it.width, it.height)
                            if (longSide == targetW && shortSide == targetH) 0 else 1
                        },
                        {
                            val longSide = maxOf(it.width, it.height)
                            val shortSide = minOf(it.width, it.height)
                            if (abs(longSide * 9 - shortSide * 16) <= 32) 0 else 1
                        },
                        {
                            val longSide = maxOf(it.width, it.height)
                            val shortSide = minOf(it.width, it.height)
                            abs(longSide - targetW) + abs(shortSide - targetH)
                        }
                    )
                )
                sorted
            })
            .build()
    }

    private fun selectBestFpsRange(cameraId: String, desiredFps: Int): Range<Int> {
        val effectiveDesired = if (isLowEndDevice) desiredFps.coerceAtMost(30) else desiredFps
        return try {
            val chars = cameraManager.getCameraCharacteristics(cameraId)
            val ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
                ?: return Range(15, effectiveDesired.coerceAtMost(30))
            if (effectiveDesired >= 60) {
                ranges.firstOrNull { it.lower in 24..30 && it.upper >= 60 }
                    ?: ranges.firstOrNull { it.lower == 60 && it.upper == 60 }
                    ?: ranges.firstOrNull { it.upper >= 60 }
                    ?: ranges.maxByOrNull { it.upper * 100 + it.lower }
                    ?: Range(15, 30)
            } else {
                // On legacy/budget devices, [15, 30] allows proper auto-exposure in indoor lighting without frame drops
                if (isLowEndDevice) {
                    ranges.firstOrNull { it.lower in 15..24 && it.upper == 30 }
                        ?: ranges.firstOrNull { it.upper == 30 }
                        ?: Range(15, 30)
                } else {
                    ranges.firstOrNull { it.lower == 30 && it.upper == 30 }
                        ?: ranges.firstOrNull { it.lower >= 20 && it.upper == 30 }
                        ?: ranges.firstOrNull { it.upper >= 30 }
                        ?: Range(15, 30)
                }
            }
        } catch (_: Exception) {
            Range(15, effectiveDesired.coerceAtMost(30))
        }
    }

    private fun applyCurrentCameraControls() {
        val cam = boundCamera ?: return
        val chars = getCurrentCharacteristics()
        val caps = chars?.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: IntArray(0)
        val supportsManualSensor = caps.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)

        val builder = CaptureRequestOptions.Builder()
            .setCaptureRequestOption(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)

        // If a physical sub-camera is pinned and intraModuleZoom > 1.0f, also apply SCALER_CROP_REGION
        if (isPhysicalCameraPinned && intraModuleZoom > 1.01f) {
            val activeRect = chars?.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
            if (activeRect != null && activeRect.width() > 0 && activeRect.height() > 0) {
                val cropW = (activeRect.width() / intraModuleZoom).roundToInt().coerceAtLeast(64)
                val cropH = (activeRect.height() / intraModuleZoom).roundToInt().coerceAtLeast(64)
                val left = activeRect.left + (activeRect.width() - cropW) / 2
                val top = activeRect.top + (activeRect.height() - cropH) / 2
                builder.setCaptureRequestOption(
                    CaptureRequest.SCALER_CROP_REGION,
                    Rect(left, top, left + cropW, top + cropH)
                )
            }
        }

        val mode = currentMode
        val man = mode as? CameraMode.Manual

        // 1. Exposure (ISO + Shutter + AE Lock)
        val manualIso = man?.iso ?: 0
        val manualExpNs = man?.exposureTimeNs ?: 0L
        if (manualIso > 0 || manualExpNs > 0L) {
            val isoRange = chars?.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
            val expRange = chars?.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)

            if (supportsManualSensor && isoRange != null && expRange != null) {
                val targetIso = CameraUtils.clampIso(
                    if (manualIso > 0) manualIso else (lastMeasuredIso ?: 250),
                    isoRange
                )
                val defaultExpNs = (1_000_000_000L / streamFps.coerceAtLeast(24)).coerceIn(8_000_000L, 33_333_333L)
                val targetExp = CameraUtils.clampExposureTime(
                    if (manualExpNs > 0L) manualExpNs else (lastMeasuredExposureNs ?: defaultExpNs),
                    expRange
                )
                val frameDuration = max(1_000_000_000L / streamFps.coerceAtLeast(1), targetExp)
                builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_LOCK, false)
                builder.setCaptureRequestOption(CaptureRequest.SENSOR_SENSITIVITY, targetIso)
                builder.setCaptureRequestOption(CaptureRequest.SENSOR_EXPOSURE_TIME, targetExp)
                builder.setCaptureRequestOption(CaptureRequest.SENSOR_FRAME_DURATION, frameDuration)
            } else {
                // Fallback for LIMITED/LEGACY devices (e.g. Redmi 8A): translate manual ISO/Shutter into AE Exposure Compensation
                builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_LOCK, false)
                builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, activeFpsRange)
                val compRange = chars?.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
                if (compRange != null && compRange.upper > compRange.lower) {
                    val isoEv = if (manualIso > 0) (ln(manualIso / 400.0) / ln(2.0)).toFloat() else 0f
                    val expEv = if (manualExpNs > 0L) (ln(manualExpNs / 16_666_666.0) / ln(2.0)).toFloat() else 0f
                    val totalEv = (isoEv + expEv).coerceIn(-3.0f, 3.0f)
                    val span = (compRange.upper - compRange.lower) / 2.0f
                    val mid = (compRange.upper + compRange.lower) / 2.0f
                    val step = (mid + (totalEv / 3.0f) * span).roundToInt().coerceIn(compRange.lower, compRange.upper)
                    builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, step)
                }
            }
        } else {
            builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
            builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, activeFpsRange)
            builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, 0)
            builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_LOCK, isAeLocked)
        }

        // 2. Focus (Manual vs Continuous Video AF)
        val manualFocus = man?.focusDistance ?: 0f
        if (manualFocus > 0f) {
            val minFocusDist = chars?.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 10f
            if (minFocusDist > 0f) {
                builder.setCaptureRequestOption(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
                builder.setCaptureRequestOption(
                    CaptureRequest.LENS_FOCUS_DISTANCE,
                    CameraUtils.clampFocusDistance(manualFocus, minFocusDist)
                )
            } else {
                builder.setCaptureRequestOption(
                    CaptureRequest.CONTROL_AF_MODE,
                    CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO
                )
            }
        } else {
            builder.setCaptureRequestOption(
                CaptureRequest.CONTROL_AF_MODE,
                CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO
            )
        }

        // 3. White Balance (Manual Kelvin vs Auto AWB)
        val manualWb = man?.whiteBalanceKelvin ?: 0
        if (manualWb > 0) {
            builder.setCaptureRequestOption(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_OFF)
            builder.setCaptureRequestOption(
                CaptureRequest.COLOR_CORRECTION_MODE,
                CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX
            )
            builder.setCaptureRequestOption(
                CaptureRequest.COLOR_CORRECTION_TRANSFORM,
                lastColorTransform ?: CameraUtils.defaultColorCorrectionTransform()
            )
            builder.setCaptureRequestOption(
                CaptureRequest.COLOR_CORRECTION_GAINS,
                CameraUtils.kelvinToRggbChannelVector(manualWb, lastNeutralGains)
            )
        } else {
            builder.setCaptureRequestOption(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
            builder.setCaptureRequestOption(
                CaptureRequest.COLOR_CORRECTION_MODE,
                CaptureRequest.COLOR_CORRECTION_MODE_FAST
            )
        }

        try {
            Camera2CameraControl.from(cam.cameraControl).setCaptureRequestOptions(builder.build())
        } catch (e: Exception) {
            Log.w(TAG, "applyCurrentCameraControls failed: ${e.message}")
        }
    }

    private fun surfaceRotationToDegrees(rotation: Int): Int = when (rotation) {
        Surface.ROTATION_90 -> 90
        Surface.ROTATION_180 -> 180
        Surface.ROTATION_270 -> 270
        else -> 0
    }

    private fun computeGlCcwDegrees(camInfo: CameraInfo?, targetRot: Int): Int {
        return if (currentInputHasCameraTransform) {
            surfaceRotationToDegrees(targetRot)
        } else {
            val cwFromSensor = camInfo?.getSensorRotationDegrees(targetRot)
                ?: if (isFrontCameraActive) 270 else 90
            (360 - (cwFromSensor % 360)) % 360
        }
    }

    private fun updateTeeTransforms() {
        val camInfo: CameraInfo? = boundCamera?.cameraInfo
        val uiGlCcwDeg = computeGlCcwDegrees(camInfo, uiDisplayRotation)

        val encTargetRot = if (manualStreamRotationOffsetDeg >= 0) {
            when (((manualStreamRotationOffsetDeg % 360) + 360) % 360) {
                0 -> Surface.ROTATION_90
                90 -> Surface.ROTATION_0
                180 -> Surface.ROTATION_270
                270 -> Surface.ROTATION_180
                else -> Surface.ROTATION_90
            }
        } else {
            devicePhysicalRotation
        }
        val encGlCcwDeg = computeGlCcwDegrees(camInfo, encTargetRot)

        eglTee.updatePreviewTransform(uiGlCcwDeg, mirrorHorizontal = isFrontCameraActive)
        eglTee.updateEncoderTransform(encGlCcwDeg, mirrorHorizontal = false)
    }

    private fun syncEncoderState(forceRecreate: Boolean = false) {
        if (externalEncoderSurface != null) return
        if (!isNetworkStreamActive) {
            stopInternalEncoder()
            return
        }

        val current = hardwareEncoder
        val cfg = current?.config
        val needsStart = current == null || !current.isActive() || forceRecreate ||
            cfg == null || cfg.width != streamWidth || cfg.height != streamHeight || cfg.frameRate != streamFps

        if (!needsStart) return

        stopInternalEncoder()

        val rawCfg = EncoderConfig(
            width = streamWidth,
            height = streamHeight,
            bitrate = streamBitrate,
            frameRate = streamFps,
            iFrameIntervalSeconds = 1.0f
        )
        try {
            val enc = HardwareEncoder(
                config = rawCfg,
                onAccessUnit = { data, isKey, ptsUs ->
                    byteCounter.addAndGet(data.size.toLong())
                    onH264FrameAvailable?.invoke(data, isKey, ptsUs)
                }
            )
            enc.start()
            // Sync back actual hardware-negotiated resolution & FPS (e.g. if fallback occurred)
            val negotiated = enc.config
            streamWidth = negotiated.width
            streamHeight = negotiated.height
            streamFps = negotiated.frameRate
            streamBitrate = negotiated.bitrate
            hardwareEncoder = enc
            eglTee.setEncoderSurface(enc.getInputSurface(), streamWidth, streamHeight, await = false)
        } catch (e: Exception) {
            Log.e(TAG, "Failed starting H.264 HardwareEncoder", e)
            stopInternalEncoder()
        }
    }

    private fun stopInternalEncoder() {
        val enc = hardwareEncoder ?: return
        hardwareEncoder = null
        eglTee.setEncoderSurface(null, streamWidth, streamHeight, await = true)
        enc.stop()
    }

    private inline fun runOnMain(crossinline block: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            block()
        } else {
            mainHandler.post { block() }
        }
    }
}
