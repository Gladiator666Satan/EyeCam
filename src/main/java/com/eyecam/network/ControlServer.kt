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

    fun sendDeviceCapabilities() {
        val modelName = "${Build.MANUFACTURER.replaceFirstChar { it.uppercase() }} ${Build.MODEL}"
        val presetsArr = JSONArray()
        cameraController.supportedZoomPresets.forEach { presetsArr.put(it.toDouble()) }

        val capObj = JSONObject().apply {
            put("type", "device_capabilities")
            put("device_name", modelName)
            put("model_name", modelName)
            put("min_zoom", cameraController.minZoomRatio.toDouble())
            put("max_zoom", cameraController.maxZoomRatio.toDouble())
            put("supports_4k", cameraController.supports4K)
            put("zoom_presets", presetsArr)
            put("stream_width", cameraController.streamWidth)
            put("stream_height", cameraController.streamHeight)
            put("stream_fps", cameraController.streamFps)
            put("is_front", cameraController.isFrontCameraActive)
        }
        sendFrame(MSG_TYPE_JSON_COMMAND, capObj.toString().toByteArray(StandardCharsets.UTF_8))
    }

    fun sendVideoFrame(jpegBytes: ByteArray) {
        if (!isClientConnected) return
        sendFrame(MSG_TYPE_VIDEO_FRAME, jpegBytes)
    }

    private fun processMessage(msgType: Short, payload: ByteArray, rxTimeUs: Long) {
        val jsonStr = String(payload, StandardCharsets.UTF_8)

        when (msgType) {
            MSG_TYPE_FRAME_PING -> {
                try {
                    val obj = JSONObject(jsonStr)
                    val t1 = obj.optLong("t1_us", 0L)
                    val t2 = rxTimeUs
                    val t3 = SystemClock.elapsedRealtimeNanos() / 1000

                    val pongObj = JSONObject().apply {
                        put("t1_us", t1)
                        put("t2_us", t2)
                        put("t3_us", t3)
                    }
                    sendFrame(MSG_TYPE_FRAME_PONG, pongObj.toString().toByteArray(StandardCharsets.UTF_8))
                } catch (e: Exception) {
                    Log.w(TAG, "Failed to parse ping", e)
                }
            }

            MSG_TYPE_KEYFRAME_REQUEST -> {
                onKeyframeRequested()
            }

            MSG_TYPE_JSON_COMMAND -> {
                executeJsonCommand(jsonStr)
            }
        }
    }

    private fun executeJsonCommand(jsonStr: String) {
        var action = ""
        var success = true
        var errorMessage = ""

        try {
            val json = JSONObject(jsonStr)
            action = if (json.has("action")) json.getString("action") else json.optString("cmd", "")

            when (action) {
                "GetCapabilities", "GetDeviceCapabilities" -> {
                    sendDeviceCapabilities()
                    return
                }

                "SetZoom", "SetZoomRatio" -> {
                    val zoom = if (json.has("zoom")) json.optDouble("zoom", 1.0).toFloat() else json.optDouble("zoom_ratio", 1.0).toFloat()
                    cameraController.setZoomRatio(zoom)
                    onZoomUpdatedFromPc?.invoke(zoom, cameraController.isFrontCameraActive)
                }

                "SetCameraModule", "SwitchSensor", "SwitchModule", "SetSensor" -> {
                    val module = json.optString("module", json.optString("sensor_id", "1.0x"))
                    cameraController.switchToSensor(module)
                    val isFront = module.equals("front", ignoreCase = true) || module.equals("self", ignoreCase = true)
                    val zoomVal = module.removeSuffix("x").toFloatOrNull() ?: 1.0f
                    onZoomUpdatedFromPc?.invoke(zoomVal, isFront)
                }

                "SwitchCamera", "SetCamera" -> {
                    val facing = json.optString("facing", "")
                    val isFront = facing.equals("FRONT", ignoreCase = true) || json.optBoolean("front", false)
                    cameraController.switchFrontBack(isFront)
                    onZoomUpdatedFromPc?.invoke(1.0f, isFront)
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
                            val expNs = (value * 1_000_000_000L).toLong()
                            cameraController.updateManualExposure(expNs)
                        }
                        "focus" -> cameraController.updateManualFocus(value.toFloat())
                        "wb" -> cameraController.updateManualWhiteBalance(value.toInt())
                    }
                }

                else -> {
                    Log.w(TAG, "Unknown action received: $action")
                    success = false
                    errorMessage = "Unknown action: $action"
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error executing action $action", e)
            success = false
            errorMessage = e.message ?: "Execution error"
        }

        // Відправляємо підтвердження виконання команди (ACK)
        try {
            val ack = JSONObject().apply {
                put("type", "ack")
                put("action", action)
                put("success", success)
                if (!success) put("error", errorMessage)
            }
            sendFrame(MSG_TYPE_JSON_COMMAND, ack.toString().toByteArray(StandardCharsets.UTF_8))
        } catch (_: Exception) {}
    }

    private fun sendTelemetryState() {
        try {
            val telemetry = JSONObject().apply {
                put("type", "telemetry")
                put("fps", 60)
                put("is_front", cameraController.isFrontCameraActive)
                put("width", cameraController.streamWidth)
                put("height", cameraController.streamHeight)
            }
            sendFrame(MSG_TYPE_JSON_COMMAND, telemetry.toString().toByteArray(StandardCharsets.UTF_8))
        } catch (_: Exception) {}
    }

    private fun sendFrame(msgType: Short, payload: ByteArray) {
        val out = outStream ?: return
        val sock = activeSocket ?: return
        if (sock.isClosed) return

        try {
            synchronized(out) {
                out.writeShort(CONTROL_MAGIC.toInt())
                out.writeShort(msgType.toInt())
                out.writeInt(payload.size)
                out.write(payload)
                out.flush()
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to send frame $msgType: ${e.message}")
        }
    }

    fun stop() {
        telemetryJob?.cancel()
        acceptJob?.cancel()
        try { activeSocket?.close() } catch (_: Exception) {}
        try { serverSocket?.close() } catch (_: Exception) {}
        activeSocket = null
        serverSocket = null
        outStream = null
    }
}
