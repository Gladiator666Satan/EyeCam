package com.eyecam.network

import android.os.Build
import android.os.SystemClock
import android.util.Log
import com.eyecam.camera.CameraController
import com.eyecam.model.CameraMode
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.EOFException
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.nio.charset.StandardCharsets
import java.util.ArrayDeque
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * Control & Live Stream TCP daemon running on Android device (port 8554).
 * Works over both Wi-Fi LAN (<PhoneIP>:8554) and USB ADB (adb forward tcp:8554 tcp:8554):
 * - Device capabilities handshake (model name, min/max zoom presets, 4K support)
 * - Hardware Annex-B H.264 video frames (MSG_TYPE_H264_FRAME = 8) + legacy JPEG fallback (7)
 * - Non-blocking async writer with congestion P-frame drop & automatic IDR recovery
 * - Camera parameters (Zoom 0.5x..10x+, ISO, Shutter, Focus, WB, Resolution, Rotation)
 * - Real live telemetry broadcast
 */
class ControlServer(
    private val port: Int = 8554,
    private val cameraController: CameraController,
    private val onKeyframeRequested: () -> Unit,
    private val onClientConnectionChanged: ((Boolean, Boolean, String) -> Unit)? = null,
    private val onZoomUpdatedFromPc: ((Float, Boolean) -> Unit)? = null,
    private val onResolutionUpdatedFromPc: ((Int, Int, Int) -> Unit)? = null,
    private val batteryLevelProvider: (() -> Float)? = null,
    private val thermalTempProvider: (() -> Float)? = null
) {
    companion object {
        private const val TAG = "EyeCam-ControlServer"

        const val CONTROL_MAGIC: Short = 0x4350 // 'C' (0x43), 'P' (0x50)

        const val MSG_TYPE_JSON_COMMAND: Short = 1
        const val MSG_TYPE_PROTOBUF_COMMAND: Short = 2
        const val MSG_TYPE_FRAME_PING: Short = 3
        const val MSG_TYPE_FRAME_PONG: Short = 4
        const val MSG_TYPE_KEYFRAME_REQUEST: Short = 5
        const val MSG_TYPE_KEEP_ALIVE: Short = 6
        const val MSG_TYPE_VIDEO_FRAME: Short = 7
        const val MSG_TYPE_H264_FRAME: Short = 8

        private const val MAX_CONTROL_QUEUE = 64
        private const val MAX_VIDEO_QUEUE = 6
    }

    private data class OutboundPacket(
        val msgType: Short,
        val payload: ByteArray,
        val isKeyFrame: Boolean = false
    )

    private var serverSocket: ServerSocket? = null
    @Volatile
    private var activeSocket: Socket? = null
    @Volatile
    private var writerRunning = false

    val isClientConnected: Boolean
        get() = activeSocket != null && activeSocket?.isClosed == false

    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private var acceptJob: Job? = null
    private var telemetryJob: Job? = null

    // Non-blocking outbound queues guarded by lock
    private val queueLock = ReentrantLock()
    private val queueCondition = queueLock.newCondition()
    private val controlQueue = ArrayDeque<OutboundPacket>(16)
    private val videoQueue = ArrayDeque<OutboundPacket>(8)
    private var needKeyframeBeforeDelta = false
    private var lastKeyframeRequestMs = 0L

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
                    try {
                        client.sendBufferSize = 2 * 1024 * 1024
                    } catch (_: Exception) {
                    }
                    Log.i(TAG, "Client connected: ${client.remoteSocketAddress}")

                    try {
                        activeSocket?.close()
                    } catch (_: Exception) {
                    }

                    scope.launch {
                        handleClient(client)
                    }
                }
            } catch (_: SocketException) {
                Log.i(TAG, "ControlServer socket closed.")
            } catch (e: Exception) {
                Log.e(TAG, "ControlServer error", e)
            }
        }
    }

    private suspend fun handleClient(client: Socket) {
        activeSocket = client
        clearQueues(resetKeyframeWait = true)
        writerRunning = true

        val writerThread = Thread({
            writerLoop(client)
        }, "EyeCam-TcpWriter-$port").apply {
            isDaemon = true
            start()
        }

        val remoteIp = client.inetAddress?.hostAddress ?: ""
        val isUsbAdb = remoteIp == "127.0.0.1" || remoteIp == "::1" || remoteIp.contains("localhost")
        onClientConnectionChanged?.invoke(true, isUsbAdb, remoteIp)

        sendDeviceCapabilities()
        onKeyframeRequested()

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
            writerRunning = false
            queueLock.withLock {
                queueCondition.signalAll()
            }
            try {
                client.close()
            } catch (_: Exception) {
            }
            try {
                writerThread.join(400)
            } catch (_: Exception) {
            }
            if (activeSocket === client) {
                activeSocket = null
                clearQueues(resetKeyframeWait = true)
                onClientConnectionChanged?.invoke(false, false, "")
            }
        }
    }

    private fun writerLoop(client: Socket) {
        try {
            val out = BufferedOutputStream(client.getOutputStream(), 256 * 1024)
            val header = ByteArray(8)
            header[0] = 0x43.toByte()
            header[1] = 0x50.toByte()

            while (writerRunning && !client.isClosed && activeSocket === client) {
                var pkt: OutboundPacket? = null
                var shouldFlush = false

                queueLock.withLock {
                    while (writerRunning && controlQueue.isEmpty() && videoQueue.isEmpty() &&
                        !client.isClosed && activeSocket === client
                    ) {
                        queueCondition.await()
                    }
                    if (!writerRunning || client.isClosed || activeSocket !== client) return@withLock
                    pkt = if (controlQueue.isNotEmpty()) {
                        shouldFlush = true
                        controlQueue.removeFirst()
                    } else if (videoQueue.isNotEmpty()) {
                        val v = videoQueue.removeFirst()
                        shouldFlush = videoQueue.isEmpty() || v.isKeyFrame
                        v
                    } else {
                        null
                    }
                }

                val packet = pkt ?: break
                val msgType = packet.msgType.toInt()
                val size = packet.payload.size
                header[2] = ((msgType ushr 8) and 0xFF).toByte()
                header[3] = (msgType and 0xFF).toByte()
                header[4] = ((size ushr 24) and 0xFF).toByte()
                header[5] = ((size ushr 16) and 0xFF).toByte()
                header[6] = ((size ushr 8) and 0xFF).toByte()
                header[7] = (size and 0xFF).toByte()

                out.write(header)
                out.write(packet.payload)
                if (shouldFlush) {
                    out.flush()
                }
            }
        } catch (e: Exception) {
            if (writerRunning) {
                Log.w(TAG, "TCP writer loop ended: ${e.message}")
            }
            try {
                client.close()
            } catch (_: Exception) {
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
            put("current_zoom", cameraController.currentZoomRatio.toDouble())
            put("supports_4k", cameraController.supports4K)
            put("zoom_presets", presetsArr)
            put("stream_width", cameraController.streamWidth)
            put("stream_height", cameraController.streamHeight)
            put("stream_fps", cameraController.streamFps)
            put("is_front", cameraController.isFrontCameraActive)
            put("rotation_deg", cameraController.manualStreamRotationOffsetDeg)
        }
        enqueueControlPacket(MSG_TYPE_JSON_COMMAND, capObj.toString().toByteArray(StandardCharsets.UTF_8))
    }

    fun sendVideoFrame(jpegBytes: ByteArray) {
        if (!isClientConnected) return
        enqueueVideoPacket(MSG_TYPE_VIDEO_FRAME, jpegBytes, isKeyFrame = true)
    }

    fun sendH264Frame(annexBBytes: ByteArray, isKeyFrame: Boolean) {
        if (!isClientConnected) return
        enqueueVideoPacket(MSG_TYPE_H264_FRAME, annexBBytes, isKeyFrame = isKeyFrame)
    }

    private fun enqueueControlPacket(msgType: Short, payload: ByteArray) {
        if (!isClientConnected) return
        queueLock.withLock {
            if (controlQueue.size >= MAX_CONTROL_QUEUE) {
                controlQueue.removeFirst()
            }
            controlQueue.addLast(OutboundPacket(msgType, payload))
            queueCondition.signal()
        }
    }

    private fun enqueueVideoPacket(msgType: Short, payload: ByteArray, isKeyFrame: Boolean) {
        if (!isClientConnected) return
        var requestIdr = false
        queueLock.withLock {
            if (!writerRunning) return@withLock
            if (needKeyframeBeforeDelta && !isKeyFrame) {
                return@withLock
            }

            if (isKeyFrame) {
                needKeyframeBeforeDelta = false
                // Drop queued stale video frames before an IDR keyframe to keep latency minimal
                videoQueue.clear()
                videoQueue.addLast(OutboundPacket(msgType, payload, isKeyFrame = true))
                queueCondition.signal()
                return@withLock
            }

            if (videoQueue.size >= MAX_VIDEO_QUEUE) {
                // Network congestion: drop stale delta and wait for a clean keyframe
                needKeyframeBeforeDelta = true
                val now = SystemClock.uptimeMillis()
                if (now - lastKeyframeRequestMs > 400L) {
                    lastKeyframeRequestMs = now
                    requestIdr = true
                }
                return@withLock
            }

            videoQueue.addLast(OutboundPacket(msgType, payload, isKeyFrame = false))
            queueCondition.signal()
        }
        if (requestIdr) {
            onKeyframeRequested()
        }
    }

    private fun clearQueues(resetKeyframeWait: Boolean) {
        queueLock.withLock {
            controlQueue.clear()
            videoQueue.clear()
            if (resetKeyframeWait) {
                needKeyframeBeforeDelta = false
            }
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
                    enqueueControlPacket(MSG_TYPE_FRAME_PONG, pongObj.toString().toByteArray(StandardCharsets.UTF_8))
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
                    onKeyframeRequested()
                    return
                }

                "SetZoom", "SetZoomRatio" -> {
                    val zoom = if (json.has("zoom")) json.optDouble("zoom", 1.0).toFloat() else json.optDouble("zoom_ratio", 1.0).toFloat()
                    cameraController.setZoomRatio(zoom)
                    onZoomUpdatedFromPc?.invoke(cameraController.currentZoomRatio, cameraController.isFrontCameraActive)
                }

                "SetCameraModule", "SwitchSensor", "SwitchModule", "SetSensor" -> {
                    val module = json.optString("module", json.optString("sensor_id", "1.0x"))
                    cameraController.switchToSensor(module)
                    val isFront = module.equals("front", ignoreCase = true) || module.equals("self", ignoreCase = true)
                    val zoomVal = if (isFront) 1.0f else (module.removeSuffix("x").removeSuffix("X").toFloatOrNull() ?: 1.0f)
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

                "SetRotation" -> {
                    val deg = json.optInt("degrees", -1)
                    cameraController.setStreamRotationOffset(deg)
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
        } catch (e: Exception) {
            success = false
            errorMessage = e.message ?: "Execution error"
        }

        val ackObj = JSONObject().apply {
            put("type", "ack")
            put("action", action)
            put("success", success)
            if (!success) put("error", errorMessage)
        }
        enqueueControlPacket(MSG_TYPE_JSON_COMMAND, ackObj.toString().toByteArray(StandardCharsets.UTF_8))
    }

    fun sendTelemetryState() {
        val modelName = "${Build.MANUFACTURER.replaceFirstChar { it.uppercase() }} ${Build.MODEL}"
        val presetsArr = JSONArray()
        cameraController.supportedZoomPresets.forEach { presetsArr.put(it.toDouble()) }

        val battery = batteryLevelProvider?.invoke() ?: 100f
        val tempC = thermalTempProvider?.invoke() ?: 34.0f

        val stateObj = JSONObject().apply {
            put("type", "device_state")
            put("device_name", modelName)
            put("min_zoom", cameraController.minZoomRatio.toDouble())
            put("max_zoom", cameraController.maxZoomRatio.toDouble())
            put("current_zoom", cameraController.currentZoomRatio.toDouble())
            put("supports_4k", cameraController.supports4K)
            put("zoom_presets", presetsArr)
            put("stream_width", cameraController.streamWidth)
            put("stream_height", cameraController.streamHeight)
            put("stream_fps", cameraController.streamFps)
            put("is_front", cameraController.isFrontCameraActive)
            put("rotation_deg", cameraController.manualStreamRotationOffsetDeg)
            put("state", JSONObject().apply {
                put("battery_level", battery.toDouble())
                put("thermal_temp_c", tempC.toDouble())
                put("measured_fps", cameraController.measuredFps.toInt())
                put("measured_bitrate_kbps", cameraController.measuredBitrateKbps)
            })
        }
        enqueueControlPacket(MSG_TYPE_JSON_COMMAND, stateObj.toString().toByteArray(StandardCharsets.UTF_8))
    }

    fun stop() {
        writerRunning = false
        acceptJob?.cancel()
        telemetryJob?.cancel()
        queueLock.withLock {
            controlQueue.clear()
            videoQueue.clear()
            queueCondition.signalAll()
        }
        try {
            activeSocket?.close()
        } catch (_: Exception) {
        }
        try {
            serverSocket?.close()
        } catch (_: Exception) {
        }
        serverSocket = null
        activeSocket = null
        Log.i(TAG, "ControlServer stopped.")
    }
}
