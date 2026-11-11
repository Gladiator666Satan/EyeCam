package com.eyecam.camera

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
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
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.EncoderConfig
import com.eyecam.model.SensorType
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.max

/**
 * CameraX + OpenGL ES Tee + Hardware H.264 Encoder camera engine.
 *
 * Replaces raw Camera2 + JPEG image-reader capture with a single zero-copy CameraX [Preview]
 * stream fed into [EglVideoTee], which drives both the distortion-free UI preview surface and the
 * hardware [HardwareEncoder] simultaneously, uninterrupted across screen lock and background transitions.
 */
@OptIn(ExperimentalCamera2Interop::class)
class CameraController(private val context: Context) {

    companion object {
        private const val TAG = "EyeCam-CameraX"
        private const val FOCUS_DURATION_SEC = 5L

        fun defaultBitrateForResolution(width: Int, fps: Int): Int {
            val base = when {
                width >= 3840 -> 32_000_000
                width >= 2560 -> 20_000_000
                width >= 1920 -> 12_000_000
                else -> 6_000_000
            }
            return if (fps >= 60) (base * 1.25f).toInt() else base
        }
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val mainExecutor = ContextCompat.getMainExecutor(context)
    private val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    private val eglTee = EglVideoTee()

    // Fallback internal lifecycle if used outside LifecycleService (e.g. VideoPipelineCoordinator)
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

    // Stream & format configuration
    @Volatile var streamWidth: Int = 1920
        private set
    @Volatile var streamHeight: Int = 1080
        private set
    @Volatile var streamFps: Int = 60
        private set
    @Volatile var streamBitrate: Int = 12_000_000

    @Volatile var isNetworkStreamActive: Boolean = false
        set(value) {
            field = value
            runOnMain { syncEncoderState() }
        }

    // Sensor inventory & capabilities
    @Volatile var isFrontCameraActive: Boolean = false
        private set
    @Volatile var currentZoomRatio: Float = 1.0f
        private set
    @Volatile var minZoomRatio: Float = 1.0f
        private set
    @Volatile var maxZoomRatio: Float = 10.0f
        private set
    @Volatile var supports4K: Boolean = true
        private set
    val supportedZoomPresets: MutableList<Float> = mutableListOf(1.0f)

    @Volatile var activeCameraId: String = "0"
        private set
    @Volatile private var selectedPhysicalCameraId: String? = null

    private var discoveredSensors: List<CameraSensorInfo> = emptyList()
    private var rearLogicalCameraId: String = "0"
    private var rearUltraWideCameraId: String? = null
    private var rearTeleCameraIds: List<String> = emptyList()
    private var frontLogicalCameraId: String? = null
    private val physicalSubCameras = mutableMapOf<String, String>() // physicalId -> parentLogicalId

    // Pro / Manual parameters + live captured AWB calibration per lens
    @Volatile private var currentMode: CameraMode = CameraMode.Auto
    @Volatile private var lastNeutralGains: RggbChannelVector? = null
    @Volatile private var lastColorTransform: ColorSpaceTransform? = null
    @Volatile private var lastMeasuredIso: Int? = null
    @Volatile private var lastMeasuredExposureNs: Long? = null
    @Volatile private var activeFpsRange: Range<Int> = Range(30, 60)

    // Preview / orientation state
    @Volatile private var uiPreviewSurface: Surface? = null
    @Volatile private var uiPreviewWidth: Int = 0
    @Volatile private var uiPreviewHeight: Int = 0
    @Volatile private var uiDisplayRotation: Int = Surface.ROTATION_0
    @Volatile private var devicePhysicalRotation: Int = Surface.ROTATION_0
    @Volatile private var hasPhysicalOrientationSample: Boolean = false
    @Volatile private var currentInputHasCameraTransform: Boolean = true
    @Volatile var manualStreamRotationOffsetDeg: Int = -1 // -1 = auto physical orientation, or 0/90/180/270

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
     * Discovers all logical and physical camera sensors, 4K capability, and logical zoom presets
     * cleanly via Camera2 characteristics.
     */
    @Synchronized
    fun discoverAvailableSensors(): List<CameraSensorInfo> {
        val result = ArrayList<CameraSensorInfo>()
        val foundIds = HashSet<String>()
        physicalSubCameras.clear()

        var bestRearLogical = "0"
        var bestRearSpan = -1f
        var fallbackFront: String? = null
        var standaloneUw: String? = null
        val standaloneTeles = ArrayList<String>()
        var anySupports4k = false

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
                val zoomSpan = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    val zr = chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
                    if (zr != null) zr.upper - zr.lower else 1.0f
                } else {
                    (chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 4.0f) - 1.0f
                }

                if (supportsUhd4k(chars)) {
                    anySupports4k = true
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
                    }
                    if (sensorType == SensorType.ULTRA_WIDE && standaloneUw == null) {
                        standaloneUw = camId
                    } else if (sensorType == SensorType.TELEPHOTO) {
                        standaloneTeles.add(camId)
                    }
                }

                for (physId in physIds) {
                    physicalSubCameras[physId] = camId
                    if (!foundIds.contains(physId)) {
                        try {
                            val pChars = cameraManager.getCameraCharacteristics(physId)
                            val pType = CameraUtils.classifySensor(pChars)
                            val pFocal = pChars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
                                ?: floatArrayOf(4.0f)
                            val pMinFocus = pChars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
                            val pIso = pChars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
                            val pExp = pChars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)
                            foundIds.add(physId)
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
                        } catch (_: Exception) {
                        }
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed discovering sensors", e)
        }

        rearLogicalCameraId = bestRearLogical
        rearUltraWideCameraId = standaloneUw
        rearTeleCameraIds = standaloneTeles
        frontLogicalCameraId = fallbackFront
        supports4K = anySupports4k || supports4K
        discoveredSensors = result
        recomputeCapabilitiesFromChars(activeCameraId)
        return result
    }

    private fun supportsUhd4k(chars: CameraCharacteristics): Boolean {
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return false
        val sizes = map.getOutputSizes(SurfaceTexture::class.java) ?: return false
        return sizes.any { (it.width >= 3840 && it.height >= 2160) || (it.height >= 3840 && it.width >= 2160) }
    }

    private fun recomputeCapabilitiesFromChars(cameraId: String) {
        try {
            val chars = cameraManager.getCameraCharacteristics(cameraId)
            val facing = chars.get(CameraCharacteristics.LENS_FACING) ?: CameraCharacteristics.LENS_FACING_BACK
            if (facing == CameraCharacteristics.LENS_FACING_BACK) {
                supports4K = supportsUhd4k(chars) || supports4K
            }
            var minZ = 1.0f
            var maxZ = 4.0f
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)?.let { range ->
                    minZ = range.lower
                    maxZ = range.upper
                }
            } else {
                maxZ = chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 4.0f
            }
            // If rear is active (or probing initial rear presets), also inspect CameraX ZoomState if bound
            val camZoom = boundCamera?.cameraInfo?.zoomState?.value
            if (camZoom != null && activeCameraId == cameraId) {
                minZ = camZoom.minZoomRatio
                maxZ = camZoom.maxZoomRatio
            }
            updateZoomPresetsFromRange(minZ, maxZ, facing == CameraCharacteristics.LENS_FACING_FRONT)
        } catch (_: Exception) {
        }
    }

    private fun updateZoomPresetsFromRange(minZ: Float, maxZ: Float, isFront: Boolean) {
        minZoomRatio = minZ
        maxZoomRatio = maxZ

        // Query rear logical range so zoom preset list matches available rear lenses too
        var rearMin = minZ
        var rearMax = maxZ
        if (isFront) {
            try {
                val rChars = cameraManager.getCameraCharacteristics(rearLogicalCameraId)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    rChars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)?.let {
                        rearMin = it.lower
                        rearMax = it.upper
                    }
                } else {
                    rearMax = rChars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 8.0f
                }
            } catch (_: Exception) {
            }
        }

        val presets = mutableListOf<Float>()
        if (rearMin <= 0.85f || rearUltraWideCameraId != null) {
            presets.add(if (rearMin in 0.55f..0.75f) 0.5f else 0.5f)
        }
        presets.add(1.0f)
        if (rearMax >= 2.0f) presets.add(2.0f)
        if (rearMax >= 3.0f) presets.add(3.0f)
        if (rearMax >= 5.0f) presets.add(5.0f)
        if (rearMax >= 10.0f) presets.add(10.0f)

        supportedZoomPresets.clear()
        supportedZoomPresets.addAll(presets.distinct().sorted())
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
        activeCameraId = logicalCameraId
        isFrontCameraActive = logicalCameraId == frontLogicalCameraId
        selectedPhysicalCameraId = null

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

        val targetW = if (width > 0) width else uiPreviewWidth.coerceAtLeast(1080)
        val targetH = if (height > 0) height else uiPreviewHeight.coerceAtLeast(1920)
        // Synchronous flush when detaching (surface == null) prevents drawing to destroyed TextureView
        eglTee.setPreviewSurface(surface, targetW, targetH, await = (surface == null))
        updateTeeTransforms()
    }

    fun setStreamRotationOffset(degrees: Int) {
        manualStreamRotationOffsetDeg = if (degrees < 0) -1 else ((degrees % 360) + 360) % 360
        updateTeeTransforms()
    }

    fun switchFrontBack(useFront: Boolean) {
        val targetId = if (useFront) {
            frontLogicalCameraId ?: run {
                Log.w(TAG, "No front camera discovered")
                return
            }
        } else {
            rearLogicalCameraId
        }
        if (isFrontCameraActive == useFront && activeCameraId == targetId && selectedPhysicalCameraId == null && boundCamera != null) {
            return
        }
        isFrontCameraActive = useFront
        activeCameraId = targetId
        selectedPhysicalCameraId = null
        currentZoomRatio = 1.0f
        lastNeutralGains = null
        lastColorTransform = null

        runOnMain {
            if (cameraOpenRequested) bindOrRebindCamera()
            onStateChanged?.invoke()
        }
    }

    /**
     * Switches either by logical zoom preset ("0.5", "1", "2", "3", "5", "10") — which moves off
     * the front camera if needed and engages CameraX logical multi-cam lens handoff — or by direct
     * camera/physical ID.
     */
    fun switchToSensor(moduleOrId: String) {
        val trimmed = moduleOrId.trim()
        if (trimmed.equals("front", ignoreCase = true)) {
            switchFrontBack(true)
            return
        }
        val parsedZoom = trimmed.removeSuffix("x").removeSuffix("X").toFloatOrNull()
        if (parsedZoom != null) {
            switchRearToPresetZoom(parsedZoom)
            return
        }
        switchPhysicalCameraSeamless(trimmed)
    }

    fun switchPhysicalCameraSeamless(physicalId: String) {
        val target = physicalId.trim()
        if (target.isEmpty()) return

        // Check if target corresponds to a known front camera
        val sensorInfo = discoveredSensors.find { it.id == target }
        if (sensorInfo?.sensorType == SensorType.FRONT) {
            switchFrontBack(true)
            return
        }

        val parentLogical = physicalSubCameras[target]
        if (parentLogical != null) {
            activeCameraId = parentLogical
            selectedPhysicalCameraId = target
            isFrontCameraActive = parentLogical == frontLogicalCameraId
        } else {
            activeCameraId = target
            selectedPhysicalCameraId = null
            isFrontCameraActive = target == frontLogicalCameraId
        }
        currentZoomRatio = 1.0f
        lastNeutralGains = null
        lastColorTransform = null

        runOnMain {
            if (cameraOpenRequested) bindOrRebindCamera()
            onStateChanged?.invoke()
        }
    }

    private fun switchRearToPresetZoom(targetZoom: Float) {
        val needRebindToRear = isFrontCameraActive || selectedPhysicalCameraId != null ||
            activeCameraId != rearLogicalCameraId

        if (needRebindToRear) {
            // Check if rear logical doesn't support < 1.0x and standalone ultrawide ID exists
            isFrontCameraActive = false
            val rearChars = try {
                cameraManager.getCameraCharacteristics(rearLogicalCameraId)
            } catch (_: Exception) {
                null
            }
            val rearMinZ = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && rearChars != null) {
                rearChars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)?.lower ?: 1.0f
            } else {
                1.0f
            }
            if (targetZoom < 0.9f && rearMinZ >= 0.95f && rearUltraWideCameraId != null &&
                rearUltraWideCameraId != rearLogicalCameraId
            ) {
                activeCameraId = rearUltraWideCameraId!!
                selectedPhysicalCameraId = null
                currentZoomRatio = 1.0f
            } else {
                activeCameraId = rearLogicalCameraId
                selectedPhysicalCameraId = null
                currentZoomRatio = targetZoom
            }
            lastNeutralGains = null
            lastColorTransform = null

            runOnMain {
                if (cameraOpenRequested) {
                    bindOrRebindCamera(desiredZoomAfterBind = currentZoomRatio)
                }
                onStateChanged?.invoke()
            }
        } else {
            // Already on rear camera: check standalone ultrawide case vs logical zoom
            if (targetZoom < 0.9f && minZoomRatio >= 0.95f && rearUltraWideCameraId != null &&
                rearUltraWideCameraId != activeCameraId
            ) {
                switchPhysicalCameraSeamless(rearUltraWideCameraId!!)
                return
            }
            if (activeCameraId != rearLogicalCameraId && targetZoom >= 1.0f) {
                activeCameraId = rearLogicalCameraId
                selectedPhysicalCameraId = null
                currentZoomRatio = targetZoom
                runOnMain {
                    if (cameraOpenRequested) bindOrRebindCamera(desiredZoomAfterBind = currentZoomRatio)
                    onStateChanged?.invoke()
                }
                return
            }
            setZoomRatio(targetZoom)
        }
    }

    /**
     * Adjusts optical/hybrid zoom strictly on the CURRENTLY active camera without jumping from
     * Front camera back to Rear when sliding zoom.
     */
    fun setZoomRatio(zoom: Float) {
        val minZ = minZoomRatio
        val maxZ = maxZoomRatio
        val clamped = zoom.coerceIn(minZ, maxZ)
        currentZoomRatio = clamped

        runOnMain {
            val cam = boundCamera
            if (cam != null) {
                cam.cameraControl.setZoomRatio(clamped)
            }
            onStateChanged?.invoke()
        }
    }

    fun setResolution(width: Int, height: Int, fps: Int) {
        val w = width.coerceAtLeast(320)
        val h = height.coerceAtLeast(240)
        val f = fps.coerceIn(15, 60)
        val changed = (w != streamWidth || h != streamHeight || f != streamFps)
        streamWidth = w
        streamHeight = h
        streamFps = f
        streamBitrate = defaultBitrateForResolution(w, f)

        if (!changed) return
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
        setMode(base.copy(iso = iso))
    }

    fun updateManualExposure(exposureTimeNs: Long) {
        val base = currentMode as? CameraMode.Manual ?: CameraMode.Manual()
        setMode(base.copy(exposureTimeNs = exposureTimeNs))
    }

    fun updateManualFocus(focusDistanceDiopters: Float) {
        val base = currentMode as? CameraMode.Manual ?: CameraMode.Manual()
        setMode(base.copy(focusDistance = focusDistanceDiopters))
    }

    fun updateManualWhiteBalance(kelvin: Int) {
        val base = currentMode as? CameraMode.Manual ?: CameraMode.Manual()
        setMode(base.copy(whiteBalanceKelvin = kelvin))
    }

    fun setMode(mode: CameraMode) {
        currentMode = mode
        runOnMain { applyCurrentCameraControls() }
    }

    fun triggerAutoFocus() {
        runOnMain {
            val cam = boundCamera ?: return@runOnMain
            val factory = SurfaceOrientedMeteringPointFactory(1f, 1f)
            val point = factory.createPoint(0.5f, 0.5f)
            val action = FocusMeteringAction.Builder(point, FocusMeteringAction.FLAG_AF or FocusMeteringAction.FLAG_AE)
                .setAutoCancelDuration(FOCUS_DURATION_SEC, TimeUnit.SECONDS)
                .build()
            cam.cameraControl.startFocusAndMetering(action)
        }
    }

    fun triggerFocusAtPoint(normX: Float, normY: Float) {
        val x = normX.coerceIn(0f, 1f)
        val y = normY.coerceIn(0f, 1f)
        runOnMain {
            val cam = boundCamera ?: return@runOnMain
            val factory = SurfaceOrientedMeteringPointFactory(1f, 1f)
            val point = factory.createPoint(x, y)
            val action = FocusMeteringAction.Builder(point, FocusMeteringAction.FLAG_AF or FocusMeteringAction.FLAG_AE)
                .setAutoCancelDuration(FOCUS_DURATION_SEC, TimeUnit.SECONDS)
                .build()
            cam.cameraControl.startFocusAndMetering(action)
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

    private fun bindOrRebindCamera(desiredZoomAfterBind: Float? = null) {
        val provider = cameraProvider
        if (provider == null) {
            val future = ProcessCameraProvider.getInstance(context)
            future.addListener({
                try {
                    cameraProvider = future.get()
                    if (cameraOpenRequested) {
                        bindOrRebindCamera(desiredZoomAfterBind)
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "ProcessCameraProvider initialization failed", e)
                }
            }, mainExecutor)
            return
        }

        val owner = attachedLifecycleOwner ?: fallbackLifecycleOwner
        if (owner.lifecycle.currentState == Lifecycle.State.DESTROYED) return

        val zoomToRestore = desiredZoomAfterBind ?: currentZoomRatio
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
                interop.setSessionCaptureCallback(object : CameraCaptureSession.CaptureCallback() {
                    override fun onCaptureCompleted(
                        session: CameraCaptureSession,
                        request: CaptureRequest,
                        result: TotalCaptureResult
                    ) {
                        if (currentMode is CameraMode.Auto) {
                            result.get(CaptureResult.COLOR_CORRECTION_GAINS)?.let { lastNeutralGains = it }
                            result.get(CaptureResult.COLOR_CORRECTION_TRANSFORM)?.let { lastColorTransform = it }
                            result.get(CaptureResult.SENSOR_SENSITIVITY)?.let { lastMeasuredIso = it }
                            result.get(CaptureResult.SENSOR_EXPOSURE_TIME)?.let { lastMeasuredExposureNs = it }
                        }
                    }
                })

                val preview = previewBuilder.build()
                preview.setSurfaceProvider(mainExecutor) { request ->
                    val size = request.resolution
                    request.setTransformationInfoListener(mainExecutor) { info ->
                        if (currentInputHasCameraTransform != info.hasCameraTransform) {
                            currentInputHasCameraTransform = info.hasCameraTransform
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

                val zoomState = camera.cameraInfo.zoomState.value
                if (zoomState != null) {
                    updateZoomPresetsFromRange(zoomState.minZoomRatio, zoomState.maxZoomRatio, isFrontCameraActive)
                } else {
                    recomputeCapabilitiesFromChars(activeCameraId)
                }

                val clampedZoom = zoomToRestore.coerceIn(minZoomRatio, maxZoomRatio)
                currentZoomRatio = clampedZoom
                if (clampedZoom != 1.0f || minZoomRatio < 1.0f) {
                    camera.cameraControl.setZoomRatio(clampedZoom)
                }

                updateTeeTransforms()
                applyCurrentCameraControls()
                onStateChanged?.invoke()
                Log.i(
                    TAG,
                    "Bound CameraX (cam=$activeCameraId phys=$selectedPhysicalCameraId " +
                        "target=${streamWidth}x${streamHeight}@${attempt.fpsRange}, zoom=$clampedZoom)"
                )
                return
            } catch (e: Exception) {
                Log.w(TAG, "CameraX bind attempt #${index + 1} failed: ${e.message}")
            }
        }
        Log.e(TAG, "All CameraX bind attempts failed")
    }

    private data class BindAttempt(val selector: CameraSelector, val fpsRange: Range<Int>)

    private fun buildBindAttempts(): List<BindAttempt> {
        val targetId = selectedPhysicalCameraId ?: activeCameraId
        val preferredFps = selectBestFpsRange(targetId, streamFps)
        val safe30Fps = Range(15, 30)

        val attempts = ArrayList<BindAttempt>()

        // 1. Exact CameraX selector (including physical ID if requested)
        val primarySelector = buildCameraSelector(activeCameraId, selectedPhysicalCameraId)
        attempts.add(BindAttempt(primarySelector, preferredFps))
        if (preferredFps.upper > 30) {
            attempts.add(BindAttempt(primarySelector, Range(30, 30)))
            attempts.add(BindAttempt(primarySelector, safe30Fps))
        }

        // 2. Fallback to parent logical camera without physical pinning (if physical pin failed)
        if (selectedPhysicalCameraId != null) {
            val logicalSelector = buildCameraSelector(activeCameraId, null)
            attempts.add(BindAttempt(logicalSelector, preferredFps))
            attempts.add(BindAttempt(logicalSelector, Range(30, 30)))
        }

        // 3. Fallback to standard CameraSelector lens facing
        val fallbackSelector = if (isFrontCameraActive) {
            CameraSelector.DEFAULT_FRONT_CAMERA
        } else {
            CameraSelector.DEFAULT_BACK_CAMERA
        }
        attempts.add(BindAttempt(fallbackSelector, Range(30, 30)))
        attempts.add(BindAttempt(fallbackSelector, safe30Fps))
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
                val sorted = supportedSizes.sortedWith(
                    compareBy<Size>(
                        {
                            val longSide = maxOf(it.width, it.height)
                            val shortSide = minOf(it.width, it.height)
                            if (longSide == targetW && shortSide == targetH) 0 else 1
                        },
                        {
                            val longSide = maxOf(it.width, it.height)
                            val shortSide = minOf(it.width, it.height)
                            if (kotlin.math.abs(longSide * 9 - shortSide * 16) <= 32) 0 else 1
                        },
                        {
                            val longSide = maxOf(it.width, it.height)
                            val shortSide = minOf(it.width, it.height)
                            kotlin.math.abs(longSide - targetW) + kotlin.math.abs(shortSide - targetH)
                        }
                    )
                )
                sorted
            })
            .build()
    }

    private fun selectBestFpsRange(cameraId: String, desiredFps: Int): Range<Int> {
        return try {
            val chars = cameraManager.getCameraCharacteristics(cameraId)
            val ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
                ?: return Range(desiredFps.coerceAtMost(30), desiredFps.coerceAtMost(60))
            if (desiredFps >= 60) {
                // Prefer [30, 60] / [24, 60] over rigid [60, 60] so multi-cam HAL can smoothly hand off to ultrawide/tele lenses
                ranges.firstOrNull { it.lower in 24..30 && it.upper >= 60 }
                    ?: ranges.firstOrNull { it.lower == 60 && it.upper == 60 }
                    ?: ranges.firstOrNull { it.upper >= 60 }
                    ?: ranges.maxByOrNull { it.upper * 100 + it.lower }
                    ?: Range(30, 30)
            } else {
                ranges.firstOrNull { it.lower == 30 && it.upper == 30 }
                    ?: ranges.firstOrNull { it.lower >= 24 && it.upper == 30 }
                    ?: ranges.firstOrNull { it.upper >= 30 }
                    ?: Range(15, 30)
            }
        } catch (_: Exception) {
            if (desiredFps >= 60) Range(30, 60) else Range(30, 30)
        }
    }

    private fun applyCurrentCameraControls() {
        val cam = boundCamera ?: return
        val chars = getCurrentCharacteristics()
        val builder = CaptureRequestOptions.Builder()
            .setCaptureRequestOption(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)

        when (val mode = currentMode) {
            is CameraMode.Auto -> {
                builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                builder.setCaptureRequestOption(
                    CaptureRequest.CONTROL_AF_MODE,
                    CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO
                )
                builder.setCaptureRequestOption(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
                builder.setCaptureRequestOption(
                    CaptureRequest.COLOR_CORRECTION_MODE,
                    CaptureRequest.COLOR_CORRECTION_MODE_FAST
                )
                builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, activeFpsRange)
            }

            is CameraMode.Manual -> {
                val isoRange = chars?.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
                val expRange = chars?.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)
                val minFocusDist = chars?.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 10f

                if (mode.iso > 0 || mode.exposureTimeNs > 0L) {
                    val targetIso = CameraUtils.clampIso(
                        if (mode.iso > 0) mode.iso else (lastMeasuredIso ?: 200),
                        isoRange
                    )
                    val targetExp = CameraUtils.clampExposureTime(
                        if (mode.exposureTimeNs > 0L) mode.exposureTimeNs else (lastMeasuredExposureNs ?: 16_666_666L),
                        expRange
                    )
                    val frameDuration = max(1_000_000_000L / streamFps.coerceAtLeast(1), targetExp)
                    builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                    builder.setCaptureRequestOption(CaptureRequest.SENSOR_SENSITIVITY, targetIso)
                    builder.setCaptureRequestOption(CaptureRequest.SENSOR_EXPOSURE_TIME, targetExp)
                    builder.setCaptureRequestOption(CaptureRequest.SENSOR_FRAME_DURATION, frameDuration)
                } else {
                    builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                    builder.setCaptureRequestOption(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, activeFpsRange)
                }

                if (mode.focusDistance > 0f) {
                    builder.setCaptureRequestOption(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
                    builder.setCaptureRequestOption(
                        CaptureRequest.LENS_FOCUS_DISTANCE,
                        CameraUtils.clampFocusDistance(mode.focusDistance, minFocusDist)
                    )
                } else {
                    builder.setCaptureRequestOption(
                        CaptureRequest.CONTROL_AF_MODE,
                        CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO
                    )
                }

                if (mode.whiteBalanceKelvin > 0) {
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
                        CameraUtils.kelvinToRggbChannelVector(mode.whiteBalanceKelvin, lastNeutralGains)
                    )
                } else {
                    builder.setCaptureRequestOption(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
                    builder.setCaptureRequestOption(
                        CaptureRequest.COLOR_CORRECTION_MODE,
                        CaptureRequest.COLOR_CORRECTION_MODE_FAST
                    )
                }
            }
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

        val newCfg = EncoderConfig(
            width = streamWidth,
            height = streamHeight,
            bitrate = streamBitrate,
            frameRate = streamFps,
            iFrameIntervalSeconds = 1.0f
        )
        try {
            val enc = HardwareEncoder(
                config = newCfg,
                onAccessUnit = { data, isKey, ptsUs ->
                    byteCounter.addAndGet(data.size.toLong())
                    onH264FrameAvailable?.invoke(data, isKey, ptsUs)
                }
            )
            enc.start()
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
        // Detach EGLSurface synchronously BEFORE destroying MediaCodec input surface
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
