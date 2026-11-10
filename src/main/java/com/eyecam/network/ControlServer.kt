package com.eyecam.network

import android.util.Log
import com.eyecam.model.CameraMode
import com.eyecam.service.CameraStreamService
import kotlinx.coroutines.*
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.io.PrintWriter
import java.net.ServerSocket
import java.net.Socket

class ControlServer(
    private val port: Int = 8554,
    private val service: CameraStreamService
) {
    companion object {
        private const val TAG = "ControlServer"
    }

    private var serverSocket: ServerSocket? = null
    private var isRunning = false
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    fun start() {
        if (isRunning) return
        isRunning = true

        scope.launch {
            try {
                serverSocket = ServerSocket(port).apply {
                    reuseAddress = true
                }
                Log.i(TAG, "ControlServer listening on port $port")

                while (isRunning) {
                    try {
                        val clientSocket = serverSocket?.accept() ?: break
                        launch { handleClient(clientSocket) }
                    } catch (e: Exception) {
                        if (isRunning) Log.e(TAG, "Accept error", e)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Server error", e)
            }
        }
    }

    private suspend fun handleClient(socket: Socket) = withContext(Dispatchers.IO) {
        service.setClientConnected(true)
        Log.i(TAG, "PC Client connected from ${socket.remoteSocketAddress}")

        try {
            val reader = BufferedReader(InputStreamReader(socket.getInputStream()))
            val writer = PrintWriter(OutputStreamWriter(socket.getOutputStream()), true)

            var line: String?
            while (isRunning && reader.readLine().also { line = it } != null) {
                val command = line?.trim() ?: continue
                if (command.isEmpty()) continue

                val response = processCommand(command)
                writer.println(response)
            }
        } catch (e: Exception) {
            Log.w(TAG, "Client connection ended: ${e.message}")
        } finally {
            try { socket.close() } catch (_: Exception) {}
            service.setClientConnected(false)
            Log.i(TAG, "PC Client disconnected")
        }
    }

    private fun processCommand(jsonStr: String): String {
        return try {
            val req = JSONObject(jsonStr)
            val action = req.optString("action", req.optString("command"))

            val res = JSONObject()
            res.put("status", "ok")
            res.put("replyTo", action)

            when (action) {
                "Ping" -> {
                    res.put("message", "Pong")
                }
                "GetCapabilities" -> {
                    val caps = JSONObject()
                    val sensors = service.getAvailableSensors()
                    val sensorArr = org.json.JSONArray()
                    for (s in sensors) {
                        val obj = JSONObject()
                        obj.put("id", s.id)
                        obj.put("name", s.name)
                        obj.put("focalLength", s.focalLengthMm.toDouble())
                        obj.put("minZoom", s.minZoomRatio.toDouble())
                        obj.put("maxZoom", s.maxZoomRatio.toDouble())
                        obj.put("isFront", s.isFront)
                        sensorArr.put(obj)
                    }
                    caps.put("sensors", sensorArr)
                    caps.put("activeSensorId", service.getActiveSensor()?.id ?: "0")
                    caps.put("currentZoom", service.getCurrentZoomRatio().toDouble())
                    caps.put("isTorchOn", service.isTorchOn())
                    res.put("capabilities", caps)
                }
                "SetResolution" -> {
                    val w = req.optInt("width", 1920)
                    val h = req.optInt("height", 1080)
                    val fps = req.optInt("fps", 60)
                    service.updateStreamResolution(w, h, fps)
                }
                "SetZoom" -> {
                    val zoom = req.optDouble("zoomRatio", 1.0).toFloat()
                    service.setZoomRatio(zoom)
                }
                "SetTorch" -> {
                    val enabled = req.optBoolean("enabled", false)
                    service.toggleTorch(enabled)
                }
                "SwitchCamera" -> {
                    val sensorId = req.optString("sensorId")
                    if (sensorId.isNotEmpty()) {
                        service.switchSensor(sensorId)
                    } else {
                        val isFront = req.optBoolean("isFront", false)
                        service.setFrontCamera(isFront)
                    }
                }
                "SetCameraModule" -> {
                    val moduleId = req.optString("moduleId")
                    service.switchSensor(moduleId)
                }
                "Refocus" -> {
                    service.triggerAutoFocus()
                }
                "FocusPoint" -> {
                    val x = req.optDouble("x", 0.5).toFloat()
                    val y = req.optDouble("y", 0.5).toFloat()
                    service.triggerFocusAtPoint(x, y)
                }
                "SetManualMode" -> {
                    val enabled = req.optBoolean("enabled", false)
                    if (enabled) {
                        val iso = req.optInt("iso", 0)
                        val shutterSec = req.optDouble("shutterSpeed", 0.0)
                        val shutterNs = if (shutterSec > 0.0) (shutterSec * 1_000_000_000.0).toLong() else 0L
                        val focus = req.optDouble("focusDistance", 0.0).toFloat()
                        val awb = req.optInt("awbKelvin", 0)
                        service.setManualMode(CameraMode.Manual(iso, shutterNs, focus, awb))
                    } else {
                        service.setManualMode(CameraMode.Auto)
                    }
                }
                else -> {
                    res.put("status", "unknown_command")
                }
            }
            res.toString()
        } catch (e: Exception) {
            val err = JSONObject()
            err.put("status", "error")
            err.put("message", e.message ?: "Unknown parse error")
            err.toString()
        }
    }

    fun stop() {
        isRunning = false
        try {
            serverSocket?.close()
        } catch (_: Exception) {}
        serverSocket = null
        scope.cancel()
        Log.i(TAG, "ControlServer stopped")
    }
}
