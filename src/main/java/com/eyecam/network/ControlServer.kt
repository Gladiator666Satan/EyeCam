package com.eyecam.network

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
import org.json.JSONObject
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.EOFException
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.nio.charset.StandardCharsets

/**
 * Control Plane TCP daemon running on Android device (port 8554).
 * Reconciles the bidirectional control contract with Desktop Engine (Flutter / Qt):
 * - FramePing / FramePong (NTP-style RTT & clock sync)
 * - Keyframe request (IDR / PLI)
 * - Camera parameters (ISO, Shutter, Focus, WB, Lens module)
 * - Status telemetry broadcast
 */
class ControlServer(
    private val port: Int = 8554,
    private val cameraController: CameraController,
    private val onKeyframeRequested: () -> Unit
) {
    companion object {
        private const val TAG = "ControlServer"

        // 8-byte binary control frame constants
        const val CONTROL_MAGIC: Short = 0x4350 // 'C' (0x43), 'P' (0x50)

        const val MSG_TYPE_JSON_COMMAND: Short = 1
        const val MSG_TYPE_PROTOBUF_COMMAND: Short = 2
        const val MSG_TYPE_FRAME_PING: Short = 3
        const val MSG_TYPE_FRAME_PONG: Short = 4
        const val MSG_TYPE_KEYFRAME_REQUEST: Short = 5
        const val MSG_TYPE_KEEP_ALIVE: Short = 6
    }

    private var serverSocket: ServerSocket? = null
    private var activeSocket: Socket? = null
    private var outStream: DataOutputStream? = null

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
                    client.tcpNoDelay = true // Disable Nagle's algorithm for sub-ms latency
                    Log.i(TAG, "Client connected: ${client.remoteSocketAddress}")

                    handleClient(client)
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

        // Start periodic telemetry loop (2 Hz)
        telemetryJob?.cancel()
        telemetryJob = scope.launch {
            while (isActive && !client.isClosed) {
                delay(500)
                sendTelemetryState()
            }
        }

        try {
            val inStream = DataInputStream(client.getInputStream())
            while (scope.isActive && !client.isClosed) {
                // Read 8-byte header: Magic(2B), MsgType(2B), PayloadLen(4B)
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
            activeSocket = null
            outStream = null
        }
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
                Log.w(TAG, "Received IDR Keyframe request from Desktop Engine!")
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
            // Support both standard 'action' dictionary and legacy 'cmd'
            action = if (json.has("action")) json.getString("action") else json.optString("cmd", "")

            when (action) {
                "SetCameraModule" -> {
                    val module = json.optString("module", "0")
                    cameraController.switchToSensor(module)
                }

                "SetISO" -> {
                    val isAuto = json.optBoolean("auto", true)
                    val iso = json.optInt("iso", 100)
                    if (isAuto) {
                        cameraController.setMode(CameraMode.Auto)
                    } else {
                        cameraController.setMode(CameraMode.Manual(iso = iso))
                    }
                }

                "SetShutter" -> {
                    val isAuto = json.optBoolean("auto", true)
                    val expNs = json.optLong("exposure_ns", 20_000_000L)
                    if (isAuto) {
                        cameraController.setMode(CameraMode.Auto)
                    } else {
                        cameraController.setMode(CameraMode.Manual(exposureTimeNs = expNs))
                    }
                }

                "SetFocus" -> {
                    val mode = json.optString("mode", "AUTO")
                    val diopters = json.optDouble("distance_diopters", 0.0).toFloat()
                    if (mode == "MANUAL") {
                        cameraController.setMode(CameraMode.Manual(focusDistance = diopters))
                    } else {
                        cameraController.setMode(CameraMode.Auto)
                    }
                }

                "SetWhiteBalance" -> {
                    val mode = json.optString("mode", "AUTO")
                    val kelvin = json.optInt("kelvin", 5500)
                    if (mode == "MANUAL_KELVIN") {
                        cameraController.setMode(CameraMode.Manual(whiteBalanceKelvin = kelvin))
                    } else {
                        cameraController.setMode(CameraMode.Auto)
                    }
                }

                // Legacy command fallbacks from Desktop AppState:
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
                        "iso" -> cameraController.setMode(CameraMode.Manual(iso = value.toInt()))
                        "shutter" -> {
                            val expNs = if (value > 0) (1_000_000_000.0 / value).toLong() else 20_000_000L
                            cameraController.setMode(CameraMode.Manual(exposureTimeNs = expNs))
                        }
                        "focus" -> cameraController.setMode(CameraMode.Manual(focusDistance = value.toFloat()))
                        "wb" -> cameraController.setMode(CameraMode.Manual(whiteBalanceKelvin = value.toInt()))
                    }
                }

                else -> {
                    success = false
                    errorMessage = "Unknown command: $action"
                }
            }
        } catch (e: Exception) {
            success = false
            errorMessage = e.message ?: "Execution error"
        }

        // Send ACK back to Desktop
        val ackObj = JSONObject().apply {
            put("type", "ack")
            put("action", action)
            put("success", success)
            if (!success) put("error", errorMessage)
        }
        sendFrame(MSG_TYPE_JSON_COMMAND, ackObj.toString().toByteArray(StandardCharsets.UTF_8))
    }

    private fun sendTelemetryState() {
        val stateObj = JSONObject().apply {
            put("type", "device_state")
            put("state", JSONObject().apply {
                put("battery_level", 85.0)
                put("thermal_temp_c", 36.5)
                put("measured_fps", 60)
                put("measured_bitrate_kbps", 12000)
            })
        }
        sendFrame(MSG_TYPE_JSON_COMMAND, stateObj.toString().toByteArray(StandardCharsets.UTF_8))
    }

    @Synchronized
    private fun sendFrame(msgType: Short, payload: ByteArray) {
        val out = outStream ?: return
        try {
            out.writeShort(CONTROL_MAGIC.toInt())
            out.writeShort(msgType.toInt())
            out.writeInt(payload.size)
            out.write(payload)
            out.flush()
        } catch (e: Exception) {
            Log.w(TAG, "Failed to send control frame", e)
        }
    }

    fun stop() {
        acceptJob?.cancel()
        telemetryJob?.cancel()
        try {
            activeSocket?.close()
        } catch (_: Exception) {}
        try {
            serverSocket?.close()
        } catch (_: Exception) {}
        serverSocket = null
        activeSocket = null
        outStream = null
        Log.i(TAG, "ControlServer stopped.")
    }
}
