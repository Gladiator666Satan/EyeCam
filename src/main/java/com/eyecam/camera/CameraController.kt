package com.eyecam.camera

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.SurfaceTexture
import android.hardware.camera2.*
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.Image
import android.media.ImageReader
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import com.eyecam.model.CameraMode
import com.eyecam.model.CameraSensorInfo
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.concurrent.Executors
import kotlin.math.roundToInt

class CameraController(
    private val context: Context,
    private val onFrameAvailable: (ByteArray, Int, Int, Long) -> Unit
) {
    companion object {
        private const val TAG = "CameraController"
    }

    private val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager

    private var backgroundThread: HandlerThread? = null
    private var backgroundHandler: Handler? = null

    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null
    private var previewSurface: Surface? = null

    private var currentCameraId: String = "0"
    private var currentMode: CameraMode = CameraMode.Auto
    private var isTorchEnabled: Boolean = false
    private var zoomRatio: Float = 1.0f

    private var targetFps: Int = 60
    private var streamWidth: Int = 1920
    private var streamHeight: Int = 1080

    private val _availableSensors = MutableStateFlow<List<CameraSensorInfo>>(emptyList())
    val availableSensors: StateFlow<List<CameraSensorInfo>> = _availableSensors.asStateFlow()

    private val _activeSensor = MutableStateFlow<CameraSensorInfo?>(null)
    val activeSensor: StateFlow<CameraSensorInfo?> = _activeSensor.asStateFlow()

    private var sensorCharacteristics: CameraCharacteristics? = null

    init {
        startBackgroundThread()
        queryCameraSensors()
    }

    private fun startBackgroundThread() {
        if (backgroundThread == null) {
            backgroundThread = HandlerThread("CameraBackgroundThread").apply {
                start()
                backgroundHandler = Handler(looper)
            }
        }
    }

    private fun stopBackgroundThread() {
        backgroundThread?.quitSafely()
        try {
            backgroundThread?.join()
            backgroundThread = null
            backgroundHandler = null
        } catch (e: InterruptedException) {
            Log.e(TAG, "Error stopping background thread", e)
        }
    }

    fun queryCameraSensors() {
        try {
            val sensorList = mutableListOf<CameraSensorInfo>()
            val cameraIds = cameraManager.cameraIdList

            for (id in cameraIds) {
                val chars = cameraManager.getCameraCharacteristics(id)
                val facing = chars.get(CameraCharacteristics.LENS_FACING)
                val isFront = facing == CameraCharacteristics.LENS_FACING_FRONT

                val focalLengths = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
                val primaryFocalLength = focalLengths?.firstOrNull() ?: 24.0f

                val zoomRange = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
                } else null
                val minZoom = zoomRange?.lower ?: 1.0f
                val maxZoom = zoomRange?.upper ?: (chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 10.0f)

                val name = CameraUtils.classifySensor(primaryFocalLength, minZoom, isFront)

                sensorList.add(
                    CameraSensorInfo(
                        id = id,
                        name = name,
                        focalLengthMm = primaryFocalLength,
                        minZoomRatio = minZoom,
                        maxZoomRatio = maxZoom,
                        isFront = isFront
                    )
                )
            }

            _availableSensors.value = sensorList
            if (_activeSensor.value == null && sensorList.isNotEmpty()) {
                val defaultSensor = sensorList.firstOrNull { !it.isFront && it.minZoomRatio <= 1.0f }
                    ?: sensorList.first()
                _activeSensor.value = defaultSensor
                currentCameraId = defaultSensor.id
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to query camera sensors", e)
        }
    }

    fun setPreviewSurface(surface: Surface?) {
        this.previewSurface = surface
        if (cameraDevice != null && captureSession != null) {
            updateCaptureRequest()
        }
    }

    @SuppressLint("MissingPermission")
    fun startCamera(
        width: Int = 1920,
        height: Int = 1080,
        fps: Int = 60,
        cameraId: String? = null
    ) {
        this.streamWidth = width
        this.streamHeight = height
        this.targetFps = fps
        if (cameraId != null) {
            this.currentCameraId = cameraId
            _activeSensor.value = _availableSensors.value.firstOrNull { it.id == cameraId }
        }

        try {
            sensorCharacteristics = cameraManager.getCameraCharacteristics(currentCameraId)
            cameraManager.openCamera(currentCameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    cameraDevice = camera
                    setupCaptureSession()
                }

                override fun onDisconnected(camera: CameraDevice) {
                    camera.close()
                    cameraDevice = null
                }

                override fun onError(camera: CameraDevice, error: Int) {
                    Log.e(TAG, "CameraDevice error: $error")
                    camera.close()
                    cameraDevice = null
                }
            }, backgroundHandler)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to open camera $currentCameraId", e)
        }
    }

    private fun setupCaptureSession() {
        val device = cameraDevice ?: return
        try {
            imageReader?.close()
            imageReader = ImageReader.newInstance(streamWidth, streamHeight, ImageFormat.JPEG, 2).apply {
                setOnImageAvailableListener({ reader ->
                    var image: Image? = null
                    try {
                        image = reader.acquireLatestImage()
                        if (image != null) {
                            val plane = image.planes[0]
                            val buffer = plane.buffer
                            val bytes = ByteArray(buffer.remaining())
                            buffer.get(bytes)
                            onFrameAvailable(bytes, image.width, image.height, image.timestamp)
                        }
                    } catch (e: Exception) {
                        Log.e(TAG, "Error acquiring camera frame", e)
                    } finally {
                        image?.close()
                    }
                }, backgroundHandler)
            }

            val targets = mutableListOf<Surface>()
            imageReader?.surface?.let { targets.add(it) }
            previewSurface?.let { targets.add(it) }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                val outputs = targets.map { OutputConfiguration(it) }
                val sessionConfig = SessionConfiguration(
                    SessionConfiguration.SESSION_REGULAR,
                    outputs,
                    Executors.newSingleThreadExecutor(),
                    object : CameraCaptureSession.StateCallback() {
                        override fun onConfigured(session: CameraCaptureSession) {
                            captureSession = session
                            updateCaptureRequest()
                        }

                        override fun onConfigureFailed(session: CameraCaptureSession) {
                            Log.e(TAG, "Failed to configure CameraCaptureSession")
                        }
                    }
                )
                device.createCaptureSession(sessionConfig)
            } else {
                @Suppress("DEPRECATION")
                device.createCaptureSession(targets, object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(session: CameraCaptureSession) {
                        captureSession = session
                        updateCaptureRequest()
                    }

                    override fun onConfigureFailed(session: CameraCaptureSession) {
                        Log.e(TAG, "Failed to configure CameraCaptureSession")
                    }
                }, backgroundHandler)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Exception during setupCaptureSession", e)
        }
    }

    private fun updateCaptureRequest() {
        val session = captureSession ?: return
        val device = cameraDevice ?: return
        val readerSurface = imageReader?.surface ?: return

        try {
            val builder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(readerSurface)
                previewSurface?.let { addTarget(it) }

                val fpsRanges = sensorCharacteristics?.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
                val targetRange = fpsRanges?.filter { it.upper == targetFps }?.maxByOrNull { it.lower }
                    ?: fpsRanges?.maxByOrNull { it.upper }
                    ?: Range(30, 30)
                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, targetRange)

                set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    val zoomRange = sensorCharacteristics?.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
                    if (zoomRange != null) {
                        val clampedZoom = zoomRatio.coerceIn(zoomRange.lower, zoomRange.upper)
                        set(CaptureRequest.CONTROL_ZOOM_RATIO, clampedZoom)
                    } else {
                        applyCropRegionZoom(this)
                    }
                } else {
                    applyCropRegionZoom(this)
                }

                if (isTorchEnabled) {
                    set(CaptureRequest.FLASH_MODE, CaptureRequest.FLASH_MODE_TORCH)
                } else {
                    set(CaptureRequest.FLASH_MODE, CaptureRequest.FLASH_MODE_OFF)
                }

                when (val mode = currentMode) {
                    is CameraMode.Auto -> {
                        set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                        set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
                    }
                    is CameraMode.Manual -> {
                        set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                        if (mode.iso > 0) {
                            set(CaptureRequest.SENSOR_SENSITIVITY, mode.iso)
                        }
                        if (mode.exposureTimeNs > 0L) {
                            set(CaptureRequest.SENSOR_EXPOSURE_TIME, mode.exposureTimeNs)
                        }
                        if (mode.focusDistance > 0.0f) {
                            set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
                            set(CaptureRequest.LENS_FOCUS_DISTANCE, mode.focusDistance)
                        }
                        set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
                    }
                }
            }

            session.setRepeatingRequest(builder.build(), null, backgroundHandler)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to updateCaptureRequest", e)
        }
    }

    private fun applyCropRegionZoom(builder: CaptureRequest.Builder) {
        val activeArray = sensorCharacteristics?.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return
        val maxDigitalZoom = sensorCharacteristics?.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 10.0f
        val clampedZoom = zoomRatio.coerceIn(1.0f, maxDigitalZoom)

        val cropWidth = (activeArray.width() / clampedZoom).roundToInt()
        val cropHeight = (activeArray.height() / clampedZoom).roundToInt()
        val cropX = (activeArray.width() - cropWidth) / 2
        val cropY = (activeArray.height() - cropHeight) / 2

        val cropRect = Rect(cropX, cropY, cropX + cropWidth, cropY + cropHeight)
        builder.set(CaptureRequest.SCALER_CROP_REGION, cropRect)
    }

    fun setZoom(zoom: Float) {
        this.zoomRatio = zoom
        updateCaptureRequest()
    }

    fun setTorch(enabled: Boolean) {
        this.isTorchEnabled = enabled
        updateCaptureRequest()
    }

    fun setCameraMode(mode: CameraMode) {
        this.currentMode = mode
        updateCaptureRequest()
    }

    fun triggerAutoFocus() {
        val session = captureSession ?: return
        val device = cameraDevice ?: return
        val readerSurface = imageReader?.surface ?: return

        try {
            val triggerBuilder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(readerSurface)
                previewSurface?.let { addTarget(it) }
                set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
            }
            session.capture(triggerBuilder.build(), null, backgroundHandler)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to triggerAutoFocus", e)
        }
    }

    fun triggerFocusAtPoint(xNorm: Float, yNorm: Float) {
        val session = captureSession ?: return
        val device = cameraDevice ?: return
        val readerSurface = imageReader?.surface ?: return
        val activeArray = sensorCharacteristics?.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return

        val centerX = (xNorm * activeArray.width()).toInt().coerceIn(0, activeArray.width())
        val centerY = (yNorm * activeArray.height()).toInt().coerceIn(0, activeArray.height())
        val halfSize = 100

        val focusRect = Rect(
            (centerX - halfSize).coerceAtLeast(0),
            (centerY - halfSize).coerceAtLeast(0),
            (centerX + halfSize).coerceAtMost(activeArray.width()),
            (centerY + halfSize).coerceAtMost(activeArray.height())
        )

        val meteringRect = MeteringRectangle(focusRect, MeteringRectangle.METERING_WEIGHT_MAX)

        try {
            val focusBuilder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(readerSurface)
                previewSurface?.let { addTarget(it) }
                set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(meteringRect))
                set(CaptureRequest.CONTROL_AE_REGIONS, arrayOf(meteringRect))
                set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO)
                set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
            }
            session.capture(focusBuilder.build(), null, backgroundHandler)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to triggerFocusAtPoint", e)
        }
    }

    fun switchSensor(sensorId: String) {
        if (currentCameraId == sensorId && cameraDevice != null) return
        stopCamera()
        currentCameraId = sensorId
        _activeSensor.value = _availableSensors.value.firstOrNull { it.id == sensorId }
        startCamera(streamWidth, streamHeight, targetFps, currentCameraId)
    }

    fun stopCamera() {
        try {
            captureSession?.stopRepeating()
            captureSession?.close()
            captureSession = null

            cameraDevice?.close()
            cameraDevice = null

            imageReader?.close()
            imageReader = null
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping camera", e)
        }
    }

    fun release() {
        stopCamera()
        stopBackgroundThread()
    }
}
