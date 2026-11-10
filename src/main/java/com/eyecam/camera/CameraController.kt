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

    fun switchToSensor(physicalId: String) {
        switchPhysicalCameraSeamless(physicalId)
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
