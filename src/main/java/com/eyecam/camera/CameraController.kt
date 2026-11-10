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
import android.hardware.camera2.params.MeteringRectangle
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

    var backCameraId: String = "0"
        private set
    var frontCameraId: String? = null
        private set
    var ultraWideCameraId: String? = null
        private set
    var mainWideCameraId: String = "0"
        private set
    var telephotoCameraId: String? = null
        private set
    var periscopeCameraId: String? = null
        private set

    var isFrontCameraActive: Boolean = false
        private set

    private val _availableSensors = mutableListOf<CameraSensorInfo>()
    val availableSensors: List<CameraSensorInfo> get() = _availableSensors

    private var reusableNv21: ByteArray? = null
    private val reusableJpegOut = ByteArrayOutputStream(262144)
    private val encodeExecutor = Executors.newSingleThreadExecutor()
    private val isEncoding = AtomicBoolean(false)

    private var lastFrameSentTimeMs: Long = 0L
    var streamWidth: Int = 1920
        private set
    var streamHeight: Int = 1080
        private set
    var streamFps: Int = 60
        private set

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
                val focalLengths = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS) ?: floatArrayOf(0f)
                val minFocusDist = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0.0f
                val isoRange = chars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE) ?: Range(100, 3200)
                val expRange = chars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) ?: Range(100_000L, 1_000_000_000L)
                val sType = CameraUtils.classifySensor(chars)

                if (facing == CameraCharacteristics.LENS_FACING_FRONT) {
                    if (frontCameraId == null) frontCameraId = id
                } else if (facing == CameraCharacteristics.LENS_FACING_BACK) {
                    if (backCameraId == "0") backCameraId = id
                    when (sType) {
                        SensorType.ULTRA_WIDE -> {
                            if (ultraWideCameraId == null) ultraWideCameraId = id
                            foundUltraWide = true
                        }
                        SensorType.WIDE -> {
                            if (mainWideCameraId == "0" || mainWideCameraId.isEmpty()) mainWideCameraId = id
                        }
                        SensorType.TELEPHOTO -> {
                            foundTelephoto = true
                            val maxFocal = focalLengths.maxOrNull() ?: 0f
                            if (maxFocal > 12.0f) {
                                periscopeCameraId = id
                            } else {
                                if (telephotoCameraId == null) telephotoCameraId = id
                            }
                        }
                        else -> {}
                    }

                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        val zoomRange = chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
                        if (zoomRange != null) {
                            if (zoomRange.lower < detectedMinZoom) detectedMinZoom = zoomRange.lower
                            if (zoomRange.upper > detectedMaxZoom) detectedMaxZoom = zoomRange.upper
                        }
                    }
                    val maxDigZoom = chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1.0f
                    if (maxDigZoom > detectedMaxZoom) detectedMaxZoom = maxDigZoom

                    val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                    val sizes = map?.getOutputSizes(Surface::class.java)
                    if (sizes != null && sizes.any { it.width >= 3840 && it.height >= 2160 }) {
                        supports4K = true
                    }
                }

                val physicalIds = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && isLogical) {
                    chars.physicalCameraIds
                } else emptySet()

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
                            if (physType == SensorType.ULTRA_WIDE) {
                                if (ultraWideCameraId == null) ultraWideCameraId = physId
                                foundUltraWide = true
                            } else if (physType == SensorType.TELEPHOTO) {
                                foundTelephoto = true
                                val maxFocal = physFocal.maxOrNull() ?: 0f
                                if (maxFocal > 12.0f && periscopeCameraId == null) {
                                    periscopeCameraId = physId
                                } else if (telephotoCameraId == null) {
                                    telephotoCameraId = physId
                                }
                            }

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

        if (periscopeCameraId == null && telephotoCameraId != null) {
            periscopeCameraId = telephotoCameraId
        }

        if ((foundUltraWide || ultraWideCameraId != null) && detectedMinZoom >= 1.0f) {
            detectedMinZoom = 0.5f
        }
        if ((foundTelephoto || telephotoCameraId != null) && detectedMaxZoom < 10.0f) {
            detectedMaxZoom = 10.0f
        }

        minZoomRatio = if (detectedMinZoom <= 0.65f) 0.5f else 1.0f
        maxZoomRatio = detectedMaxZoom.coerceIn(1.0f, 10.0f)

        val candidatePresets = listOf(0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f)
        supportedZoomPresets = candidatePresets.filter { preset ->
            (preset < 1.0f && minZoomRatio < 1.0f) || (preset >= 1.0f && preset <= maxZoomRatio + 0.05f)
        }.ifEmpty { listOf(1.0f) }

        Log.i(TAG, "Modules: ultraWide=$ultraWideCameraId, main=$mainWideCameraId, tele=$telephotoCameraId, peri=$periscopeCameraId, front=$frontCameraId")
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

            val map = currentSensorCharacteristics?.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            val sizes = map?.getOutputSizes(ImageFormat.YUV_420_888)
            val bestSize = sizes?.minByOrNull {
                val dw = kotlin.math.abs(it.width - streamWidth)
                val dh = kotlin.math.abs(it.height - streamHeight)
                dw * dw + dh * dh
            }
            val actualW = bestSize?.width ?: streamWidth
            val actualH = bestSize?.height ?: streamHeight

            // Створюємо ImageReader для передачі живих кадрів на ПК через USB ADB / Wi-Fi
            imageReader = ImageReader.newInstance(actualW, actualH, ImageFormat.YUV_420_888, 2).apply {
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

    fun setResolution(width: Int, height: Int, fps: Int = 60) {
        if (streamWidth == width && streamHeight == height && streamFps == fps) return
        Log.i(TAG, "Updating stream resolution: ${width}x${height} @ ${fps}fps (from ${streamWidth}x${streamHeight})")
        streamWidth = width
        streamHeight = height
        streamFps = fps

        if (cameraDevice != null) {
            recreateCaptureSession()
        }
    }

    fun updatePreviewSurface(newSurface: Surface?) {
        this.previewSurface = newSurface
        if (cameraDevice != null) {
            recreateCaptureSession()
        } else if (isNetworkStreamActive && hasCameraPermission()) {
            openCamera(
                logicalCameraId = activeLogicalCameraId,
                previewSurface = newSurface,
                encoderSurface = encoderSurface
            )
        }
    }

    fun recreateCaptureSession() {
        val handler = cameraHandler ?: return
        try {
            try { captureSession?.stopRepeating() } catch (_: Exception) {}
            try { captureSession?.close() } catch (_: Exception) {}
            captureSession = null

            try { imageReader?.close() } catch (_: Exception) {}

            val map = currentSensorCharacteristics?.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            val sizes = map?.getOutputSizes(ImageFormat.YUV_420_888)
            val bestSize = sizes?.minByOrNull {
                val dw = kotlin.math.abs(it.width - streamWidth)
                val dh = kotlin.math.abs(it.height - streamHeight)
                dw * dw + dh * dh
            }
            val actualW = bestSize?.width ?: streamWidth
            val actualH = bestSize?.height ?: streamHeight

            imageReader = ImageReader.newInstance(actualW, actualH, ImageFormat.YUV_420_888, 2).apply {
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

            createCaptureSessionInternal()
        } catch (e: Exception) {
            Log.e(TAG, "Error recreating capture session", e)
        }
    }

    fun triggerAutoFocus() {
        val session = captureSession ?: return
        val builder = previewRequestBuilder ?: return
        val handler = cameraHandler ?: return
        try {
            builder.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_AUTO)
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_START)
            session.capture(builder.build(), null, handler)

            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
            builder.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
            session.setRepeatingRequest(builder.build(), null, handler)
            Log.i(TAG, "Refocus triggered successfully")
        } catch (e: Exception) {
            Log.e(TAG, "Error triggering auto-focus", e)
        }
    }

    fun triggerFocusAtPoint(xNorm: Float, yNorm: Float) {
        val chars = currentSensorCharacteristics ?: return
        val session = captureSession ?: return
        val builder = previewRequestBuilder ?: return
        val handler = cameraHandler ?: return
        val activeArray = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return

        try {
            val focusAreaSize = 150
            val centerX = (xNorm * activeArray.width()).toInt().coerceIn(0, activeArray.width() - 1)
            val centerY = (yNorm * activeArray.height()).toInt().coerceIn(0, activeArray.height() - 1)

            val left = (centerX - focusAreaSize).coerceAtLeast(0)
            val top = (centerY - focusAreaSize).coerceAtLeast(0)
            val right = (centerX + focusAreaSize).coerceAtMost(activeArray.width() - 1)
            val bottom = (centerY + focusAreaSize).coerceAtMost(activeArray.height() - 1)

            val meteringRect = MeteringRectangle(Rect(left, top, right, bottom), MeteringRectangle.METERING_WEIGHT_MAX)
            builder.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(meteringRect))
            builder.set(CaptureRequest.CONTROL_AE_REGIONS, arrayOf(meteringRect))
            builder.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_AUTO)
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_START)
            session.capture(builder.build(), null, handler)

            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
            builder.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
            session.setRepeatingRequest(builder.build(), null, handler)
            Log.i(TAG, "Focus triggered at point: ($xNorm, $yNorm)")
        } catch (e: Exception) {
            Log.e(TAG, "Error in tap-to-focus", e)
        }
    }

    private fun processImageForNetwork(image: Image) {
        val callback = onJpegFrameAvailable ?: return
        if (!isNetworkStreamActive) return

        if (!isEncoding.compareAndSet(false, true)) {
            // Попередній кадр ще стискається / відправляється — пропускаємо без накопичення затримок
            return
        }

        val width = image.width
        val height = image.height
        val requiredSize = width * height * 3 / 2
        var nv21 = reusableNv21
        if (nv21 == null || nv21.size != requiredSize) {
            nv21 = ByteArray(requiredSize)
            reusableNv21 = nv21
        }

        try {
            yuv420888ToNv21Fast(image, nv21)
        } catch (e: Exception) {
            isEncoding.set(false)
            return
        }

        encodeExecutor.execute {
            try {
                synchronized(reusableJpegOut) {
                    reusableJpegOut.reset()
                    val yuvImage = YuvImage(nv21, ImageFormat.NV21, width, height, null)
                    // Якість 75 забезпечує ідеальний баланс між деталізацією 1080p/4K та надшвидким кодуванням у 60 FPS
                    yuvImage.compressToJpeg(Rect(0, 0, width, height), 75, reusableJpegOut)
                    callback(reusableJpegOut.toByteArray(), width, height)
                }
            } catch (e: Exception) {
                Log.w(TAG, "Frame encode warning: ${e.message}")
            } finally {
                isEncoding.set(false)
            }
        }
    }

    private fun yuv420888ToNv21Fast(image: Image, outNv21: ByteArray) {
        val width = image.width
        val height = image.height
        val ySize = width * height

        val yPlane = image.planes[0]
        val uPlane = image.planes[1]
        val vPlane = image.planes[2]

        val yBuffer = yPlane.buffer
        val uBuffer = uPlane.buffer
        val vBuffer = vPlane.buffer

        val yRowStride = yPlane.rowStride
        if (yRowStride == width) {
            yBuffer.get(outNv21, 0, ySize)
        } else {
            var pos = 0
            for (row in 0 until height) {
                yBuffer.position(row * yRowStride)
                yBuffer.get(outNv21, pos, width)
                pos += width
            }
        }

        val uvRowStride = uPlane.rowStride
        val uvPixelStride = uPlane.pixelStride
        val uvHeight = height / 2
        val uvWidth = width / 2
        var pos = ySize

        // Швидке пряме копіювання, якщо U/V площини вже перемежовані у пам'яті HAL
        if (uvPixelStride == 2 && uvRowStride == width) {
            val vRem = vBuffer.remaining()
            val toCopy = minOf(vRem, outNv21.size - pos)
            vBuffer.get(outNv21, pos, toCopy)
        } else {
            for (row in 0 until uvHeight) {
                val rowOffsetV = row * uvRowStride
                for (col in 0 until uvWidth) {
                    val vuPos = rowOffsetV + col * uvPixelStride
                    outNv21[pos++] = vBuffer.get(vuPos)
                    outNv21[pos++] = uBuffer.get(vuPos)
                }
            }
        }
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
        if (isFrontCameraActive) {
            val chars = currentSensorCharacteristics
            val maxDigZoom = chars?.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 4.0f
            val clamped = zoomRatio.coerceIn(1.0f, maxDigZoom)
            currentZoomRatio = clamped
            val session = captureSession ?: return
            val builder = previewRequestBuilder ?: return
            val handler = cameraHandler ?: return
            try {
                applyZoomToBuilder(builder, clamped)
                session.setRepeatingRequest(builder.build(), null, handler)
                Log.i(TAG, "Applied front camera digital zoom: ${clamped}x")
            } catch (e: Exception) {
                Log.e(TAG, "Error setting front zoom", e)
            }
            return
        }

        // Тилова камера: 0.5x, 1x, 2x, 3x, 5x, 10x
        val clamped = zoomRatio.coerceIn(minZoomRatio, maxZoomRatio)
        currentZoomRatio = clamped

        // Перевіряємо, чи підтримує активна логічна камера (id "0" на Samsung Galaxy S26 Ultra)
        // плавний оптичний зум через CONTROL_ZOOM_RATIO_RANGE (включаючи оптичне перемикання лінз на рівні HAL)
        val chars = currentSensorCharacteristics
        val zoomRange = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            chars?.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
        } else null

        val isLogicalRear = (activeLogicalCameraId == backCameraId || activeLogicalCameraId == mainWideCameraId || activeLogicalCameraId == "0")

        // 1. Якщо це логічна тилова камера і вона підтримує оптичний зум через CONTROL_ZOOM_RATIO_RANGE:
        // На Samsung Galaxy Ultra (S20U..S26U) діапазон [0.6, 100.0] включає 0.5x/0.6x Ultra-Wide, 1x Wide, 3x Tele та 5x/10x Periscope.
        if (isLogicalRear && zoomRange != null) {
            val effectiveZoom = if (clamped <= 0.65f && zoomRange.lower <= 0.7f) {
                zoomRange.lower
            } else {
                clamped.coerceIn(zoomRange.lower, zoomRange.upper)
            }

            if (clamped >= 0.95f || zoomRange.lower <= 0.7f) {
                val session = captureSession ?: return
                val builder = previewRequestBuilder ?: return
                val handler = cameraHandler ?: return

                try {
                    applyZoomToBuilder(builder, effectiveZoom)
                    session.setRepeatingRequest(builder.build(), null, handler)
                    Log.i(TAG, "Applied zoom ratio: ${effectiveZoom}x on rear logical camera $activeLogicalCameraId")
                } catch (e: Exception) {
                    Log.e(TAG, "Error applying zoom ratio on rear camera", e)
                }
                return
            }
        }

        // 2. Якщо Ultra-Wide є окремим сенсором у cameraIdList і логічна камера не має < 1.0f:
        val targetModuleId = if (clamped < 1.0f && ultraWideCameraId != null && cameraManager.cameraIdList.contains(ultraWideCameraId)) {
            ultraWideCameraId
        } else {
            backCameraId
        }

        if (targetModuleId != null && targetModuleId != activeLogicalCameraId) {
            Log.i(TAG, "Switching camera device from $activeLogicalCameraId to $targetModuleId for zoom ${clamped}x")
            openCamera(
                logicalCameraId = targetModuleId,
                previewSurface = previewSurface,
                encoderSurface = encoderSurface
            )
        } else {
            val session = captureSession ?: return
            val builder = previewRequestBuilder ?: return
            val handler = cameraHandler ?: return
            try {
                applyZoomToBuilder(builder, clamped)
                session.setRepeatingRequest(builder.build(), null, handler)
            } catch (e: Exception) {
                Log.e(TAG, "Error setting zoom ratio fallback", e)
            }
        }
    }

    fun switchFrontBack(useFront: Boolean) {
        val targetId = if (useFront) (frontCameraId ?: "1") else backCameraId
        if (activeLogicalCameraId != targetId || cameraDevice == null || isFrontCameraActive != useFront) {
            isFrontCameraActive = useFront
            currentZoomRatio = 1.0f
            openCamera(
                logicalCameraId = targetId,
                previewSurface = previewSurface,
                encoderSurface = encoderSurface
            )
        }
    }

    private fun applyZoomToBuilder(builder: CaptureRequest.Builder, zoom: Float) {
        val chars = currentSensorCharacteristics ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val range = chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
            if (range != null) {
                val effectiveTarget = if (zoom <= 0.65f && range.lower <= 0.7f) range.lower else zoom.coerceIn(range.lower, range.upper)
                builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, effectiveTarget)
                return
            }
        }
        // Fallback через SCALER_CROP_REGION для старіших API
        val activeArray = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return
        val effZoom = zoom.coerceAtLeast(1.0f)
        val cropW = (activeArray.width() / effZoom).toInt()
        val cropH = (activeArray.height() / effZoom).toInt()
        val left = (activeArray.width() - cropW) / 2
        val top = (activeArray.height() - cropH) / 2
        builder.set(CaptureRequest.SCALER_CROP_REGION, Rect(left, top, left + cropW, top + cropH))
    }

    fun switchCameraModule(targetCameraId: String, targetZoom: Float) {
        // Запобігаємо виклику openCamera для фізичних суб-камер, які заборонено відкривати напряму
        val isSubCamera = !cameraManager.cameraIdList.contains(targetCameraId)
        if (isSubCamera) {
            Log.w(TAG, "Target camera ID $targetCameraId is a physical sub-camera. Applying zoom on logical camera instead.")
            if (isFrontCameraActive) switchFrontBack(false)
            setZoomRatio(targetZoom)
            return
        }

        this.isFrontCameraActive = (targetCameraId == frontCameraId)
        this.currentZoomRatio = targetZoom

        if (activeLogicalCameraId != targetCameraId || cameraDevice == null) {
            Log.i(TAG, "Switching camera: from $activeLogicalCameraId to $targetCameraId (zoom ${targetZoom}x)")
            openCamera(
                logicalCameraId = targetCameraId,
                previewSurface = previewSurface,
                encoderSurface = encoderSurface
            )
        } else {
            val session = captureSession ?: return
            val builder = previewRequestBuilder ?: return
            val handler = cameraHandler ?: return
            try {
                applyZoomToBuilder(builder, targetZoom)
                session.setRepeatingRequest(builder.build(), null, handler)
            } catch (e: Exception) {
                Log.e(TAG, "Error adjusting zoom on module $targetCameraId", e)
            }
        }
    }

    fun switchToSensor(moduleOrId: String) {
        when (moduleOrId.lowercase()) {
            "front", "self", "3_front" -> switchFrontBack(true)
            "0.5x", "0" -> {
                if (isFrontCameraActive) switchFrontBack(false)
                setZoomRatio(0.5f)
            }
            "1.0x", "1x", "1" -> {
                if (isFrontCameraActive) switchFrontBack(false)
                setZoomRatio(1.0f)
            }
            "2.0x", "2x" -> {
                if (isFrontCameraActive) switchFrontBack(false)
                setZoomRatio(2.0f)
            }
            "3.0x", "3x", "2" -> {
                if (isFrontCameraActive) switchFrontBack(false)
                setZoomRatio(3.0f)
            }
            "5.0x", "5x" -> {
                if (isFrontCameraActive) switchFrontBack(false)
                setZoomRatio(5.0f)
            }
            "10.0x", "10x" -> {
                if (isFrontCameraActive) switchFrontBack(false)
                setZoomRatio(10.0f)
            }
            else -> {
                val parsedZoom = moduleOrId.removeSuffix("x").toFloatOrNull()
                if (parsedZoom != null) {
                    if (isFrontCameraActive) switchFrontBack(false)
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
            SensorType.ULTRA_WIDE -> switchToSensor("0.5x")
            SensorType.WIDE -> switchToSensor("1.0x")
            SensorType.TELEPHOTO -> {
                val focal = targetSensor.focalLengths.maxOrNull() ?: 0f
                if (focal > 12.0f) switchToSensor("5.0x") else switchToSensor("3.0x")
            }
            else -> {
                if (cameraManager.cameraIdList.contains(physicalId)) {
                    switchCameraModule(physicalId, 1.0f)
                } else {
                    switchToSensor("1.0x")
                }
            }
        }
    }

    fun updateManualIso(iso: Int) {
        val oldMode = currentMode as? CameraMode.Manual ?: CameraMode.Manual()
        setMode(oldMode.copy(iso = iso))
    }

    fun updateManualExposure(exposureTimeNs: Long) {
        val oldMode = currentMode as? CameraMode.Manual ?: CameraMode.Manual()
        setMode(oldMode.copy(exposureTimeNs = exposureTimeNs))
    }

    fun updateManualFocus(distance: Float) {
        val oldMode = currentMode as? CameraMode.Manual ?: CameraMode.Manual()
        setMode(oldMode.copy(focusDistance = distance))
    }

    fun updateManualWhiteBalance(kelvin: Int) {
        val oldMode = currentMode as? CameraMode.Manual ?: CameraMode.Manual()
        setMode(oldMode.copy(whiteBalanceKelvin = kelvin))
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
                if (mode.focusDistance > 0.0f) {
                    builder.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_OFF)
                    builder.set(CaptureRequest.LENS_FOCUS_DISTANCE, CameraUtils.clampFocusDistance(mode.focusDistance, minFocusDist))
                } else {
                    builder.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                }

                val isoRange = chars?.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
                val expRange = chars?.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)

                if (mode.iso > 0 && mode.exposureTimeNs > 0L) {
                    builder.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF)
                    builder.set(CaptureRequest.SENSOR_SENSITIVITY, CameraUtils.clampIso(mode.iso, isoRange))
                    builder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, CameraUtils.clampExposureTime(mode.exposureTimeNs, expRange))
                } else if (mode.iso > 0) {
                    builder.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF)
                    builder.set(CaptureRequest.SENSOR_SENSITIVITY, CameraUtils.clampIso(mode.iso, isoRange))
                } else if (mode.exposureTimeNs > 0L) {
                    builder.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF)
                    builder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, CameraUtils.clampExposureTime(mode.exposureTimeNs, expRange))
                } else {
                    builder.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_ON)
                }

                if (mode.whiteBalanceKelvin > 0) {
                    builder.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_OFF)
                    builder.set(CaptureRequest.COLOR_CORRECTION_MODE, CameraMetadata.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX)
                    val rggb = CameraUtils.kelvinToRggbChannelVector(mode.whiteBalanceKelvin)
                    builder.set(CaptureRequest.COLOR_CORRECTION_GAINS, rggb)
                } else {
                    builder.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_AUTO)
                }
            }
        }
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
        try {
            cameraThread?.join(500)
            cameraThread = null
            cameraHandler = null
        } catch (e: InterruptedException) {
            Log.e(TAG, "Error stopping background thread", e)
        }
    }

    fun closeCamera() {
        try {
            captureSession?.stopRepeating()
        } catch (_: Exception) {}
        try {
            captureSession?.close()
        } catch (_: Exception) {}
        captureSession = null

        try {
            cameraDevice?.close()
        } catch (_: Exception) {}
        cameraDevice = null

        try {
            imageReader?.close()
        } catch (_: Exception) {}
        imageReader = null

        isSessionActive.set(false)
        // thread не зупиняємо тут для миттєвого перезапуску без лагів
    }

    fun release() {
        closeCamera()
        stopBackgroundThread()
        encodeExecutor.shutdown()
    }
}
