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

    fun hasCameraPermission(): Boolean {
        return ContextCompat.checkSelfPermission(
            context,
            Manifest.permission.CAMERA
        ) == PackageManager.PERMISSION_GRANTED
    }

    fun startBackgroundThread() {
        if (cameraThread == null) {
            cameraThread = HandlerThread("CameraBackgroundThread", Process.THREAD_PRIORITY_MORE_FAVORABLE).apply {
                start()
                cameraHandler = Handler(looper)
            }
        }
    }

    fun stopBackgroundThread() {
        cameraThread?.quitSafely()
        try {
            cameraThread?.join()
            cameraThread = null
            cameraHandler = null
        } catch (_: Exception) {}
    }

    fun setPreviewSurface(surface: Surface?) {
        this.previewSurface = surface
        if (isSessionActive.get()) {
            reconfigureSession()
        }
    }

    fun setEncoderSurface(surface: Surface?) {
        this.encoderSurface = surface
        if (isSessionActive.get()) {
            reconfigureSession()
        }
    }

    @SuppressLint("MissingPermission")
    fun openCamera(
        logicalCameraId: String = "0",
        physicalCameraId: String? = null,
        width: Int = 1920,
        height: Int = 1080,
        targetFps: Int = 60
    ) {
        if (!hasCameraPermission()) return

        startBackgroundThread()
        activeLogicalCameraId = logicalCameraId
        activePhysicalCameraId = physicalCameraId

        try {
            currentSensorCharacteristics = cameraManager.getCameraCharacteristics(logicalCameraId)
            cameraManager.openCamera(logicalCameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    cameraDevice = camera
                    createCaptureSession(width, height, targetFps)
                }

                override fun onDisconnected(camera: CameraDevice) {
                    camera.close()
                    cameraDevice = null
                    isSessionActive.set(false)
                }

                override fun onError(camera: CameraDevice, error: Int) {
                    camera.close()
                    cameraDevice = null
                    isSessionActive.set(false)
                }
            }, cameraHandler)
        } catch (e: Exception) {
            Log.e(TAG, "Помилка відкриття камери", e)
        }
    }

    private fun createCaptureSession(width: Int, height: Int, targetFps: Int) {
        val device = cameraDevice ?: return
        val handler = cameraHandler ?: return

        try {
            imageReader?.close()
            // 60fps hardware JPEG format directly from ISP
            imageReader = ImageReader.newInstance(width, height, ImageFormat.JPEG, 2).apply {
                setOnImageAvailableListener({ reader ->
                    if (isNetworkStreamActive) {
                        val image = reader.acquireLatestImage()
                        if (image != null) {
                            try {
                                val plane = image.planes[0]
                                val buffer = plane.buffer
                                val bytes = ByteArray(buffer.remaining())
                                buffer.get(bytes)
                                onJpegFrameAvailable?.invoke(bytes, image.width, image.height)
                            } finally {
                                image.close()
                            }
                        }
                    }
                }, handler)
            }

            val surfaces = mutableListOf<Surface>()
            previewSurface?.let { surfaces.add(it) }
            encoderSurface?.let { surfaces.add(it) }
            imageReader?.surface?.let { surfaces.add(it) }

            if (surfaces.isEmpty()) return

            val requestBuilder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                surfaces.forEach { addTarget(it) }

                // 60 FPS range configuration
                val fpsRanges = currentSensorCharacteristics?.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
                val targetRange = fpsRanges?.filter { it.upper == targetFps }?.maxByOrNull { it.lower }
                    ?: fpsRanges?.maxByOrNull { it.upper }
                    ?: Range(30, 30)
                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, targetRange)

                set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)

                // HAL multi-camera zoom
                applyZoomRatio(this, currentZoomRatio)
            }

            previewRequestBuilder = requestBuilder

            val stateCallback = object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    captureSession = session
                    isSessionActive.set(true)
                    try {
                        session.setRepeatingRequest(requestBuilder.build(), null, handler)
                    } catch (e: Exception) {
                        Log.e(TAG, "Помилка запуску repeatingRequest", e)
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    isSessionActive.set(false)
                }
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                val outputConfigs = surfaces.map { OutputConfiguration(it) }
                val sessionConfig = SessionConfiguration(
                    SessionConfiguration.SESSION_REGULAR,
                    outputConfigs,
                    Executors.newSingleThreadExecutor(),
                    stateCallback
                )
                device.createCaptureSession(sessionConfig)
            } else {
                @Suppress("DEPRECATION")
                device.createCaptureSession(surfaces, stateCallback, handler)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Помилка створення сесії", e)
        }
    }

    private fun applyZoomRatio(builder: CaptureRequest.Builder, zoomRatio: Float) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val zoomRange = currentSensorCharacteristics?.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
            if (zoomRange != null) {
                val clamped = zoomRatio.coerceIn(zoomRange.lower, zoomRange.upper)
                builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, clamped)
                return
            }
        }

        // Fallback digital crop
        val activeArray = currentSensorCharacteristics?.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return
        val maxDigitalZoom = currentSensorCharacteristics?.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 10.0f
        val clampedZoom = zoomRatio.coerceIn(1.0f, maxDigitalZoom)

        val cropWidth = (activeArray.width() / clampedZoom).toInt()
        val cropHeight = (activeArray.height() / clampedZoom).toInt()
        val cropX = (activeArray.width() - cropWidth) / 2
        val cropY = (activeArray.height() - cropHeight) / 2

        val cropRect = Rect(cropX, cropY, cropX + cropWidth, cropY + cropHeight)
        builder.set(CaptureRequest.SCALER_CROP_REGION, cropRect)
    }

    fun setZoomRatio(zoomRatio: Float) {
        currentZoomRatio = zoomRatio
        val builder = previewRequestBuilder ?: return
        val session = captureSession ?: return

        try {
            applyZoomRatio(builder, zoomRatio)
            session.setRepeatingRequest(builder.build(), null, cameraHandler)
        } catch (e: Exception) {
            Log.e(TAG, "Помилка встановлення зуму", e)
        }
    }

    fun triggerAutoFocus() {
        val session = captureSession ?: return
        val builder = previewRequestBuilder ?: return

        try {
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
            session.capture(builder.build(), null, cameraHandler)
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
        } catch (e: Exception) {
            Log.e(TAG, "Помилка фокусування", e)
        }
    }

    fun triggerFocusAtPoint(xNorm: Float, yNorm: Float) {
        val session = captureSession ?: return
        val builder = previewRequestBuilder ?: return
        val activeArray = currentSensorCharacteristics?.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return

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
            builder.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(meteringRect))
            builder.set(CaptureRequest.CONTROL_AE_REGIONS, arrayOf(meteringRect))
            builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO)
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
            session.capture(builder.build(), null, cameraHandler)
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
        } catch (e: Exception) {
            Log.e(TAG, "Помилка точкового фокусування", e)
        }
    }

    fun setTorch(enabled: Boolean) {
        val builder = previewRequestBuilder ?: return
        val session = captureSession ?: return

        try {
            builder.set(
                CaptureRequest.FLASH_MODE,
                if (enabled) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF
            )
            session.setRepeatingRequest(builder.build(), null, cameraHandler)
        } catch (e: Exception) {
            Log.e(TAG, "Помилка перемикання спалаху", e)
        }
    }

    fun setCameraMode(mode: CameraMode) {
        currentMode = mode
        val builder = previewRequestBuilder ?: return
        val session = captureSession ?: return

        try {
            when (mode) {
                is CameraMode.Auto -> {
                    builder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                    builder.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
                }
                is CameraMode.Manual -> {
                    builder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                    if (mode.iso > 0) {
                        builder.set(CaptureRequest.SENSOR_SENSITIVITY, mode.iso)
                    }
                    if (mode.exposureTimeNs > 0L) {
                        builder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, mode.exposureTimeNs)
                    }
                    if (mode.focusDistance > 0.0f) {
                        builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
                        builder.set(CaptureRequest.LENS_FOCUS_DISTANCE, mode.focusDistance)
                    }
                    // Retain CONTROL_AWB_MODE_AUTO to eliminate green tint
                    builder.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
                }
            }
            session.setRepeatingRequest(builder.build(), null, cameraHandler)
        } catch (e: Exception) {
            Log.e(TAG, "Помилка режиму камери", e)
        }
    }

    private fun reconfigureSession() {
        val device = cameraDevice ?: return
        if (!isSessionActive.get()) return

        try {
            captureSession?.stopRepeating()
            captureSession?.close()
            captureSession = null
            createCaptureSession(1920, 1080, 60)
        } catch (e: Exception) {
            Log.e(TAG, "Помилка reconfigureSession", e)
        }
    }

    fun closeCamera() {
        try {
            isSessionActive.set(false)
            captureSession?.stopRepeating()
            captureSession?.close()
            captureSession = null

            cameraDevice?.close()
            cameraDevice = null

            imageReader?.close()
            imageReader = null

            stopBackgroundThread()
        } catch (e: Exception) {
            Log.e(TAG, "Помилка закриття камери", e)
        }
    }
}
