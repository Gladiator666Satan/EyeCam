package com.eyecam.camera

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.hardware.camera2.*
import android.hardware.camera2.params.ColorSpaceTransform
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.RggbChannelVector
import android.hardware.camera2.params.SessionConfiguration
import android.media.Image
import android.media.ImageReader
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.util.Log
import android.util.Range
import android.view.Surface
import androidx.core.content.ContextCompat
import com.eyecam.model.CameraMode
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.SensorType
import java.io.ByteArrayOutputStream
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
    private var imageReader: ImageReader? = null

    private var previewSurface: Surface? = null
    private var encoderSurface: Surface? = null

    var onJpegFrameAvailable: ((ByteArray, Int, Int) -> Unit)? = null
    var isNetworkStreamActive: Boolean = false

    private val isSessionActive = AtomicBoolean(false)
    private var activeLogicalCameraId: String = "0"
    private var activePhysicalCameraId: String? = null
    private var currentMode: CameraMode = CameraMode.Auto
    private var currentZoomRatio: Float = 1.0f
    private var currentSensorCharacteristics: CameraCharacteristics? = null

    var minZoomRatio: Float = 0.5f
        private set
    var maxZoomRatio: Float = 10.0f
        private set
    var supports4K: Boolean = true
        private set
    var supportedZoomPresets: List<Float> = listOf(0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f)
        private set

    private var backCameraId: String = "0"
    private var frontCameraId: String? = null
    var isFrontCameraActive: Boolean = false
        private set

    private val _availableSensors = mutableListOf<CameraSensorInfo>()
    val availableSensors: List<CameraSensorInfo> get() = _availableSensors

    private var lastFrameSentTimeMs: Long = 0L
    private var streamWidth: Int = 1280
    private var streamHeight: Int = 720

    init {
        discoverAvailableSensors()
    }

    fun hasCameraPermission(): Boolean {
        return ContextCompat.checkSelfPermission(
            context,
            Manifest.permission.CAMERA
        ) == PackageManager.PERMISSION_GRANTED
    }

    fun discoverAvailableSensors(): List<CameraSensorInfo> {
        _availableSensors.clear()
        var detectedMinZoom = 1.0f
        var detectedMaxZoom = 1.0f
        var foundUltraWide = false
        var foundTelephoto = false

        try {
            for (id in cameraManager.cameraIdList) {
                val chars = cameraManager.getCameraCharacteristics(id)
                val caps = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf()
                val isLogical = caps.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA)
                val facing = chars.get(CameraCharacteristics.LENS_FACING) ?: CameraCharacteristics.LENS_FACING_BACK

                if (facing == CameraCharacteristics.LENS_FACING_BACK && backCameraId == "0") {
                    backCameraId = id
                } else if (facing == CameraCharacteristics.LENS_FACING_FRONT && frontCameraId == null) {
                    frontCameraId = id
                }

                // Визначення реального діапазону зуму телефону (наприклад, Samsung Galaxy S26 Ultra підтримує 0.5x..100x)
                if (facing == CameraCharacteristics.LENS_FACING_BACK) {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        val zoomRange = chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
                        if (zoomRange != null) {
                            if (zoomRange.lower < detectedMinZoom) detectedMinZoom = zoomRange.lower
                            if (zoomRange.upper > detectedMaxZoom) detectedMaxZoom = zoomRange.upper
                        }
                    }
                    val maxDigZoom = chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1.0f
                    if (maxDigZoom > detectedMaxZoom) detectedMaxZoom = maxDigZoom

                    // Перевірка підтримки 4K (3840x2160)
                    val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                    val sizes = map?.getOutputSizes(Surface::class.java)
                    if (sizes != null && sizes.any { it.width >= 3840 && it.height >= 2160 }) {
                        supports4K = true
                    }
                }

                val physicalIds = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && isLogical) {
                    chars.physicalCameraIds
                } else emptySet()

                val focalLengths = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS) ?: floatArrayOf(0f)
                val minFocusDist = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0.0f
                val isoRange = chars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE) ?: Range(100, 3200)
                val expRange = chars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) ?: Range(100_000L, 1_000_000_000L)

                val sType = CameraUtils.classifySensor(chars)
                if (sType == SensorType.ULTRA_WIDE) foundUltraWide = true
                if (sType == SensorType.TELEPHOTO) foundTelephoto = true

                _availableSensors.add(
                    CameraSensorInfo(
                        id = id,
                        isLogical = isLogical,
                        physicalCameraIds = physicalIds,
                        sensorType = sType,
                        lensFacing = facing,
                        focalLengths = focalLengths,
                        minFocusDistance = minFocusDist,
                        isoRange = isoRange.lower..isoRange.upper,
                        exposureTimeRangeNs = expRange.lower..expRange.upper
                    )
                )

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && isLogical) {
                    for (physId in physicalIds) {
                        try {
                            val physChars = cameraManager.getCameraCharacteristics(physId)
                            val physFocal = physChars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS) ?: floatArrayOf(0f)
                            val physMinFocus = physChars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0.0f
                            val physIso = physChars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE) ?: Range(100, 3200)
                            val physExp = physChars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) ?: Range(100_000L, 1_000_000_000L)
                            val physType = CameraUtils.classifySensor(physChars)
                            if (physType == SensorType.ULTRA_WIDE) foundUltraWide = true
                            if (physType == SensorType.TELEPHOTO) foundTelephoto = true

                            _availableSensors.add(
                                CameraSensorInfo(
                                    id = physId,
                                    isLogical = false,
                                    physicalCameraIds = emptySet(),
                                    sensorType = physType,
                                    lensFacing = facing,
                                    focalLengths = physFocal,
                                    minFocusDistance = physMinFocus,
                                    isoRange = physIso.lower..physIso.upper,
                                    exposureTimeRangeNs = physExp.lower..physExp.upper
                                )
                            )
                        } catch (_: Exception) {}
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error discovering sensors", e)
        }

        if (foundUltraWide && detectedMinZoom >= 1.0f) {
            detectedMinZoom = 0.5f
        }
        if (foundTelephoto && detectedMaxZoom < 10.0f) {
            detectedMaxZoom = 10.0f
        }

        minZoomRatio = if (detectedMinZoom <= 0.65f) 0.5f else 1.0f
        maxZoomRatio = detectedMaxZoom.coerceIn(1.0f, 10.0f)

        val candidatePresets = listOf(0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f)
        supportedZoomPresets = candidatePresets.filter { preset ->
            (preset < 1.0f && minZoomRatio < 1.0f) || (preset >= 1.0f && preset <= maxZoomRatio + 0.05f)
        }.ifEmpty { listOf(1.0f) }

        Log.i(TAG, "Detected Zoom Range: ${minZoomRatio}x .. ${maxZoomRatio}x, Presets: $supportedZoomPresets")
        return _availableSensors
    }

    @SuppressLint("MissingPermission")
    fun openCamera(
        logicalCameraId: String = backCameraId,
        initialPhysicalCameraId: String? = null,
        previewSurface: Surface?,
        encoderSurface: Surface? = null
    ) {
        if (!hasCameraPermission()) {
            Log.w(TAG, "Cannot open camera: CAMERA permission not granted yet")
            return
        }

        closeCamera()
        startBackgroundThread()

        this.previewSurface = previewSurface
        this.encoderSurface = encoderSurface
        this.activeLogicalCameraId = logicalCameraId
        this.activePhysicalCameraId = initialPhysicalCameraId
        this.isFrontCameraActive = (logicalCameraId == frontCameraId)

        val handler = cameraHandler ?: return

        try {
            this.currentSensorCharacteristics = cameraManager.getCameraCharacteristics(logicalCameraId)

            imageReader = ImageReader.newInstance(streamWidth, streamHeight, ImageFormat.YUV_420_888, 2).apply {
                setOnImageAvailableListener({ reader ->
                    val image = try { reader.acquireLatestImage() } catch (_: Exception) { null }
                    if (image != null) {
                        try {
                            processImageForNetwork(image)
                        } finally {
                            image.close()
                        }
                    }
                }, handler)
            }

            cameraManager.openCamera(logicalCameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    cameraDevice = camera
                    createCaptureSessionInternal()
                }
                override fun onDisconnected(camera: CameraDevice) = closeCamera()
                override fun onError(camera: CameraDevice, error: Int) {
                    Log.e(TAG, "CameraDevice error: $error")
                    closeCamera()
                }
            }, handler)
        } catch (e: Exception) {
            Log.e(TAG, "Error opening camera $logicalCameraId", e)
        }
    }

    private fun processImageForNetwork(image: Image) {
        val callback = onJpegFrameAvailable ?: return
        if (!isNetworkStreamActive) return

        val now = System.currentTimeMillis()
        if (now - lastFrameSentTimeMs < 28) return
        lastFrameSentTimeMs = now

        try {
            val nv21 = yuv420888ToNv21(image)
            val yuvImage = YuvImage(nv21, ImageFormat.NV21, image.width, image.height, null)
            val out = ByteArrayOutputStream(65536)
            yuvImage.compressToJpeg(Rect(0, 0, image.width, image.height), 78, out)
            callback(out.toByteArray(), image.width, image.height)
        } catch (e: Exception) {
            Log.w(TAG, "Frame encode warning: ${e.message}")
        }
    }

    private fun yuv420888ToNv21(image: Image): ByteArray {
        val width = image.width
        val height = image.height
        val ySize = width * height
        val uvSize = width * height / 2
        val nv21 = ByteArray(ySize + uvSize)

        val yPlane = image.planes[0]
        val uPlane = image.planes[1]
        val vPlane = image.planes[2]

        val yBuffer = yPlane.buffer
        val uBuffer = uPlane.buffer
        val vBuffer = vPlane.buffer

        val yRowStride = yPlane.rowStride
        var pos = 0

        if (yRowStride == width) {
            yBuffer.get(nv21, 0, ySize)
            pos = ySize
        } else {
            for (row in 0 until height) {
                yBuffer.position(row * yRowStride)
                yBuffer.get(nv21, pos, width)
                pos += width
            }
        }

        val uvRowStride = uPlane.rowStride
        val uvPixelStride = uPlane.pixelStride
        val uvHeight = height / 2
        val uvWidth = width / 2

        for (row in 0 until uvHeight) {
            for (col in 0 until uvWidth) {
                val vuPos = row * uvRowStride + col * uvPixelStride
                nv21[pos++] = vBuffer.get(vuPos)
                nv21[pos++] = uBuffer.get(vuPos)
            }
        }
        return nv21
    }

    private fun createCaptureSessionInternal() {
        val device = cameraDevice ?: return
        val targets = mutableListOf<Surface>()
        previewSurface?.let { if (it.isValid) targets.add(it) }
        encoderSurface?.let { if (it.isValid) targets.add(it) }
        imageReader?.surface?.let { if (it.isValid) targets.add(it) }

        if (targets.isEmpty()) return

        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                val outputConfigs = targets.map { OutputConfiguration(it) }
                val sessionConfig = SessionConfiguration(
                    SessionConfiguration.SESSION_REGULAR,
                    outputConfigs,
                    Executors.newSingleThreadExecutor(),
                    createSessionCallback(targets)
                )
                device.createCaptureSession(sessionConfig)
            } else {
                @Suppress("DEPRECATION")
                device.createCaptureSession(targets, createSessionCallback(targets), cameraHandler)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error creating capture session", e)
        }
    }

    private fun createSessionCallback(targets: List<Surface>) = object : CameraCaptureSession.StateCallback() {
        override fun onConfigured(session: CameraCaptureSession) {
            captureSession = session
            isSessionActive.set(true)
            startRepeatingPipeline(targets)
        }
        override fun onConfigureFailed(session: CameraCaptureSession) {
            isSessionActive.set(false)
        }
    }

    private fun startRepeatingPipeline(targets: List<Surface>) {
        val device = cameraDevice ?: return
        val session = captureSession ?: return
        val handler = cameraHandler ?: return

        try {
            val builder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                targets.forEach { addTarget(it) }
            }
            applyModeToBuilder(builder, currentMode)
            applyZoomToBuilder(builder, currentZoomRatio)
            previewRequestBuilder = builder
            session.setRepeatingRequest(builder.build(), null, handler)
        } catch (e: Exception) {
            Log.e(TAG, "Error starting repeating request", e)
        }
    }

    fun setZoomRatio(zoomRatio: Float) {
        val clamped = zoomRatio.coerceIn(minZoomRatio, maxZoomRatio)
        currentZoomRatio = clamped

        if (isFrontCameraActive) {
            openCamera(
                logicalCameraId = backCameraId,
                previewSurface = previewSurface,
                encoderSurface = encoderSurface
            )
            return
        }

        val session = captureSession ?: return
        val builder = previewRequestBuilder ?: return
        val handler = cameraHandler ?: return

        try {
            applyZoomToBuilder(builder, clamped)
            session.setRepeatingRequest(builder.build(), null, handler)
            Log.i(TAG, "Applied zoom ratio: ${clamped}x")
        } catch (e: Exception) {
            Log.e(TAG, "Error setting zoom ratio", e)
        }
    }

    fun switchFrontBack(useFront: Boolean) {
        val targetId = if (useFront) (frontCameraId ?: backCameraId) else backCameraId
        if (targetId == activeLogicalCameraId && isSessionActive.get()) return
        openCamera(
            logicalCameraId = targetId,
            previewSurface = previewSurface,
            encoderSurface = encoderSurface
        )
    }

    private fun applyZoomToBuilder(builder: CaptureRequest.Builder, zoom: Float) {
        val chars = currentSensorCharacteristics ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val range = chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
            if (range != null) {
                builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom.coerceIn(range.lower, range.upper))
                return
            }
        }
        val activeArray = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return
        val effZoom = zoom.coerceAtLeast(1.0f)
        val cropW = (activeArray.width() / effZoom).toInt()
        val cropH = (activeArray.height() / effZoom).toInt()
        val left = (activeArray.width() - cropW) / 2
        val top = (activeArray.height() - cropH) / 2
        builder.set(CaptureRequest.SCALER_CROP_REGION, Rect(left, top, left + cropW, top + cropH))
    }

    fun switchToSensor(moduleOrId: String) {
        when (moduleOrId.lowercase()) {
            "front", "self", "3_front" -> switchFrontBack(true)
            "0.5x", "0" -> setZoomRatio(minZoomRatio)
            "1.0x", "1x", "1" -> setZoomRatio(1.0f)
            "2.0x", "2x" -> setZoomRatio(2.0f)
            "3.0x", "3x", "2" -> setZoomRatio(3.0f)
            "5.0x", "5x" -> setZoomRatio(5.0f)
            "10.0x", "10x" -> setZoomRatio(10.0f)
            else -> {
                val parsedZoom = moduleOrId.removeSuffix("x").toFloatOrNull()
                if (parsedZoom != null) {
                    setZoomRatio(parsedZoom)
                } else {
                    switchPhysicalCameraSeamless(moduleOrId)
                }
            }
        }
    }

    fun switchPhysicalCameraSeamless(physicalId: String) {
        val targetSensor = _availableSensors.find { it.id == physicalId }
        if (targetSensor?.sensorType == SensorType.FRONT) {
            switchFrontBack(true)
            return
        }
        when (targetSensor?.sensorType) {
            SensorType.ULTRA_WIDE -> setZoomRatio(minZoomRatio)
            SensorType.WIDE -> setZoomRatio(1.0f)
            SensorType.TELEPHOTO -> setZoomRatio(3.0f)
            else -> setZoomRatio(1.0f)
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
        try { imageReader?.close() } catch (_: Exception) {}
        captureSession = null
        cameraDevice = null
        imageReader = null
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
