package com.eyecam.network

import android.os.Build
import android.os.SystemClock
import android.util.Log
import com.eyecam.camera.CameraController
import com.eyecam.model.CameraMode
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.EOFException
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.nio.charset.StandardCharsets

/**
 * Control & Live Stream TCP daemon running on Android device (port 8554).
 * Works over both Wi-Fi LAN (<PhoneIP>:8554) and USB ADB (adb forward tcp:8554 tcp:8554):
 * - Device capabilities handshake (Model name, min/max zoom up to 10x+, 4K support)
 * - Live MJPEG/JPEG video frames (MSG_TYPE_VIDEO_FRAME = 7)
 * - Camera parameters (Zoom 0.5x..10x, ISO, Shutter, Focus, WB, Resolution)
 * - Status telemetry broadcast
 */
class ControlServer(
    private val port: Int = 8554,
    private val cameraController: CameraController,
    private val onKeyframeRequested: () -> Unit,
    private val onClientConnectionChanged: ((Boolean, Boolean, String) -> Unit)? = null,
    private val onZoomUpdatedFromPc: ((Float, Boolean) -> Unit)? = null,
    private val onResolutionUpdatedFromPc: ((Int, Int, Int) -> Unit)? = null
) {
    companion object {
        private const val TAG = "ControlServer"

        const val CONTROL_MAGIC: Short = 0x4350 // 'C' (0x43), 'P' (0x50)

        const val MSG_TYPE_JSON_COMMAND: Short = 1
        const val MSG_TYPE_PROTOBUF_COMMAND: Short = 2
        const val MSG_TYPE_FRAME_PING: Short = 3
        const val MSG_TYPE_FRAME_PONG: Short = 4
        const val MSG_TYPE_KEYFRAME_REQUEST: Short = 5
        const val MSG_TYPE_KEEP_ALIVE: Short = 6
        const val MSG_TYPE_VIDEO_FRAME: Short = 7
    }

    private var serverSocket: ServerSocket? = null
    @Volatile
    private var activeSocket: Socket? = null
    @Volatile
    private var outStream: DataOutputStream? = null

    val isClientConnected: Boolean
        get() = activeSocket != null && activeSocket?.isClosed == false

    private val scope = CoroutineScope(Dispatchers.IO + Job())
    private var acceptJob: Job? = null
    private var telemetryJob: Job? = null

    fun start() {
        if (serverSocket != null && !serverSocket!!.isClosed) return

        acceptJob = scope.launch {
            try {
                serverSocket = ServerSocket(port).apply {
                    reuseAddress = true
                }
                Log.i(TAG, "ControlServer listening on TCP port $port")

                while (isActive) {
                    val client = serverSocket?.accept() ?: break
                    client.tcpNoDelay = true
                    client.sendBufferSize = 2 * 1024 * 1024
                    Log.i(TAG, "Client connected: ${client.remoteSocketAddress}")

                    // Close previous socket if a new client reconnects
                    try { activeSocket?.close() } catch (_: Exception) {}

                    scope.launch {
                        handleClient(client)
                    }
                }
            } catch (e: SocketException) {
                Log.i(TAG, "ControlServer socket closed.")
            } catch (e: Exception) {
                Log.e(TAG, "ControlServer error", e)
            }
        }
    }

    private suspend fun handleClient(client: Socket) {
        activeSocket = client
        outStream = DataOutputStream(client.getOutputStream())

        val remoteIp = client.inetAddress?.hostAddress ?: ""
        val isUsbAdb = remoteIp == "127.0.0.1" || remoteIp == "::1" || remoteIp.contains("localhost")
        onClientConnectionChanged?.invoke(true, isUsbAdb, remoteIp)

        // Immediately send device capabilities (model name, max zoom e.g. 10x on Galaxy S26 Ultra, 4K support)
        sendDeviceCapabilities()

        telemetryJob?.cancel()
        telemetryJob = scope.launch {
            while (isActive && !client.isClosed) {
                delay(1000)
                sendTelemetryState()
            }
        }

        try {
            val inStream = DataInputStream(client.getInputStream())
            while (scope.isActive && !client.isClosed) {
                val magic = inStream.readShort()
                if (magic != CONTROL_MAGIC) {
                    Log.w(TAG, "Invalid control magic: 0x${Integer.toHexString(magic.toInt())}")
                    break
                }
                val msgType = inStream.readShort()
                val payloadLen = inStream.readInt()

                if (payloadLen < 0 || payloadLen > 10 * 1024 * 1024) {
                    Log.e(TAG, "Invalid payload length: $payloadLen")
                    break
                }

                val payloadBytes = ByteArray(payloadLen)
                inStream.readFully(payloadBytes)
                val rxTimeUs = SystemClock.elapsedRealtimeNanos() / 1000

                processMessage(msgType, payloadBytes, rxTimeUs)
            }
        } catch (_: EOFException) {
            Log.i(TAG, "Client disconnected.")
        } catch (e: Exception) {
            Log.e(TAG, "Error handling control stream", e)
        } finally {
            telemetryJob?.cancel()
            try { client.close() } catch (_: Exception) {}
            if (activeSocket === client) {
                activeSocket = null
                outStream = null
                onClientConnectionChanged?.invoke(false, false, "")
            }
        }
    }

    private fun processMessage(msgType: Short, payload: ByteArray, rxTimeUs: Long) {
        when (msgType) {
            MSG_TYPE_FRAME_PING -> {
                if (payload.size >= 8) {
                    val frameIndex = java.nio.ByteBuffer.wrap(payload, 0, 4).int
                    val txTimeUs = java.nio.ByteBuffer.wrap(payload, 4, 8).long
                    sendFramePong(frameIndex, txTimeUs, rxTimeUs)
                }
            }
            MSG_TYPE_KEYFRAME_REQUEST -> {
                onKeyframeRequested()
            }
            MSG_TYPE_KEEP_ALIVE -> {
                // Heartbeat handled
            }
            MSG_TYPE_JSON_COMMAND -> {
                val jsonStr = String(payload, StandardCharsets.UTF_8)
                handleJsonCommand(jsonStr)
            }
        }
    }

    private fun handleJsonCommand(jsonStr: String) {
        try {
            val json = JSONObject(jsonStr)
            val action = json.optString("action", "")
            val commandId = json.optString("command_id", "")

            var success = true
            var errorMessage: String? = null

            when (action) {
                "GetDeviceCapabilities" -> {
                    sendDeviceCapabilities()
                    return
                }

                "SetZoomRatio" -> {
                    val zoom = json.optDouble("zoom_ratio", 1.0).toFloat()
                    cameraController.setZoomRatio(zoom)
                    onZoomUpdatedFromPc?.invoke(zoom, cameraController.isFrontCameraActive)
                }

                "SwitchCamera", "SetCamera" -> {
                    val facing = json.optString("facing", "")
                    val isFront = facing.equals("FRONT", ignoreCase = true) || json.optBoolean("front", false)
                    cameraController.switchFrontBack(isFront)
                    onZoomUpdatedFromPc?.invoke(1.0f, isFront)
                }

                "SwitchSensor", "SwitchModule" -> {
                    val module = json.optString("module", json.optString("sensor_id", "1.0x"))
                    cameraController.switchToSensor(module)
                    val isFront = module.equals("front", ignoreCase = true) || module.equals("self", ignoreCase = true)
                    val zoomVal = module.removeSuffix("x").toFloatOrNull() ?: 1.0f
                    onZoomUpdatedFromPc?.invoke(zoomVal, isFront)
                }

                "SetResolution" -> {
                    val w = json.optInt("width", 1920)
                    val h = json.optInt("height", 1080)
                    val fps = json.optInt("fps", 60)
                    cameraController.setResolution(w, h, fps)
                    onResolutionUpdatedFromPc?.invoke(w, h, fps)
                }

                "TriggerAutoFocus", "Refocus", "trigger_focus" -> {
                    cameraController.triggerAutoFocus()
                }

                "SetFocusPoint" -> {
                    val x = json.optDouble("x", 0.5).toFloat()
                    val y = json.optDouble("y", 0.5).toFloat()
                    cameraController.triggerFocusAtPoint(x, y)
                }

                "SetISO" -> {
                    val isAuto = json.optBoolean("auto", true)
                    val iso = json.optInt("iso", 100)
                    if (isAuto) {
                        cameraController.updateManualIso(0)
                    } else {
                        cameraController.updateManualIso(iso)
                    }
                }

                "SetShutter" -> {
                    val isAuto = json.optBoolean("auto", true)
                    val expNs = json.optLong("exposure_ns", 20_000_000L)
                    if (isAuto) {
                        cameraController.updateManualExposure(0L)
                    } else {
                        cameraController.updateManualExposure(expNs)
                    }
                }

                "SetFocus" -> {
                    val mode = json.optString("mode", "AUTO")
                    val diopters = json.optDouble("distance_diopters", 0.0).toFloat()
                    if (mode == "MANUAL") {
                        cameraController.updateManualFocus(diopters)
                    } else {
                        cameraController.updateManualFocus(0.0f)
                    }
                }

                "SetWhiteBalance" -> {
                    val mode = json.optString("mode", "AUTO")
                    val kelvin = json.optInt("kelvin", 5500)
                    if (mode == "MANUAL_KELVIN") {
                        cameraController.updateManualWhiteBalance(kelvin)
                    } else {
                        cameraController.updateManualWhiteBalance(0)
                    }
                }

                "set_mode" -> {
                    val modeStr = json.optString("mode", "auto")
                    if (modeStr == "pro") {
                        cameraController.setMode(CameraMode.Manual())
                    } else {
                        cameraController.setMode(CameraMode.Auto)
                    }
                }

                "set_param" -> {
                    val param = json.optString("param", "")
                    val value = json.optDouble("value", 0.0)
                    when (param) {
                        "iso" -> cameraController.updateManualIso(value.toInt())
                        "shutter" -> {
                            val expNs = if (value > 0) (1_000_000_000.0 / value).toLong() else 0L
                            cameraController.updateManualExposure(expNs)
                        }
                        "focus" -> cameraController.updateManualFocus(value.toFloat())
                        "wb" -> cameraController.updateManualWhiteBalance(value.toInt())
                    }
                }

                else -> {
                    success = false
                    errorMessage = "Unknown command: $action"
                }
            }

            // Reply ACK
            val resp = JSONObject().apply {
                put("type", "COMMAND_ACK")
                put("command_id", commandId)
                put("action", action)
                put("success", success)
                if (errorMessage != null) put("error", errorMessage)
            }
            sendJson(resp)
        } catch (e: Exception) {
            Log.e(TAG, "Error handling JSON command", e)
        }
    }

    fun sendVideoFrame(jpegBytes: ByteArray) {
        val stream = outStream ?: return
        try {
            synchronized(this) {
                stream.writeShort(CONTROL_MAGIC.toInt())
                stream.writeShort(MSG_TYPE_VIDEO_FRAME.toInt())
                stream.writeInt(jpegBytes.size)
                stream.write(jpegBytes)
                stream.flush()
            }
        } catch (_: SocketException) {
            // Client likely closed
        } catch (e: Exception) {
            Log.w(TAG, "Error sending video frame: ${e.message}")
        }
    }

    private fun sendDeviceCapabilities() {
        val model = "${Build.MANUFACTURER.replaceFirstChar { it.uppercase() }} ${Build.MODEL}"
        val resp = JSONObject().apply {
            put("type", "DEVICE_CAPABILITIES")
            put("model_name", model)
            put("min_zoom", cameraController.minZoomRatio)
            put("max_zoom", cameraController.maxZoomRatio)
            put("supports_4k", cameraController.supports4K)
            put("zoom_presets", JSONArray(cameraController.supportedZoomPresets))
            put("stream_width", cameraController.streamWidth)
            put("stream_height", cameraController.streamHeight)
            put("stream_fps", cameraController.streamFps)
            put("is_front", cameraController.isFrontCameraActive)
        }
        sendJson(resp)
    }

    private fun sendTelemetryState() {
        val resp = JSONObject().apply {
            put("type", "TELEMETRY_UPDATE")
            put("zoom_ratio", cameraController.minZoomRatio)
            put("stream_fps", cameraController.streamFps)
            put("is_front", cameraController.isFrontCameraActive)
        }
        sendJson(resp)
    }

    private fun sendFramePong(frameIndex: Int, txTimeUs: Long, rxTimeUs: Long) {
        val pongTimeUs = SystemClock.elapsedRealtimeNanos() / 1000
        val buffer = java.nio.ByteBuffer.allocate(28).apply {
            putInt(frameIndex)
            putLong(txTimeUs)
            putLong(rxTimeUs)
            putLong(pongTimeUs)
        }
        sendRawMessage(MSG_TYPE_FRAME_PONG, buffer.array())
    }

    private fun sendJson(json: JSONObject) {
        val bytes = json.toString().toByteArray(StandardCharsets.UTF_8)
        sendRawMessage(MSG_TYPE_JSON_COMMAND, bytes)
    }

    private fun sendRawMessage(msgType: Short, payload: ByteArray) {
        val stream = outStream ?: return
        try {
            synchronized(this) {
                stream.writeShort(CONTROL_MAGIC.toInt())
                stream.writeShort(msgType.toInt())
                stream.writeInt(payload.size)
                stream.write(payload)
                stream.flush()
            }
        } catch (_: Exception) {}
    }

    fun stop() {
        acceptJob?.cancel()
        telemetryJob?.cancel()
        try { activeSocket?.close() } catch (_: Exception) {}
        try { serverSocket?.close() } catch (_: Exception) {}
        activeSocket = null
        outStream = null
    }
}
