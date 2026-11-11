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
        val encoder = HardwareEncoder(
            config = encoderConfig,
            listener = NalUnitListener { nalUnit ->
                networkSocketSender(nalUnit)
            }
        )
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
