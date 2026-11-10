package com.eyecam.service

import android.app.*
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import android.view.Surface
import androidx.core.app.NotificationCompat
import com.eyecam.camera.CameraController
import com.eyecam.camera.CameraUtils
import com.eyecam.model.CameraMode
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.ConnectionType
import com.eyecam.model.StreamState
import com.eyecam.model.StreamTelemetry
import com.eyecam.model.StreamUiState
import com.eyecam.model.ThermalState
import com.eyecam.network.ControlServer
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.io.ByteArrayOutputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder

class CameraStreamService : Service() {

    companion object {
        private const val TAG = "CameraStreamService"
        private const val NOTIFICATION_ID = 1001
        private const val CHANNEL_ID = "eyecam_stream_channel"

        const val ACTION_START_STREAM = "com.eyecam.action.START_STREAM"
        const val ACTION_STOP_STREAM = "com.eyecam.action.STOP_STREAM"
    }

    inner class LocalBinder : Binder() {
        fun getService(): CameraStreamService = this@CameraStreamService
    }

    private val binder = LocalBinder()
    private val serviceScope = CoroutineScope(Dispatchers.Main + SupervisorJob())

    private var cameraController: CameraController? = null
    private var controlServer: ControlServer? = null

    private var videoServerSocket: ServerSocket? = null
    private var isVideoServerRunning = false
    private val videoClients = mutableListOf<Socket>()

    private var wakeLock: PowerManager.WakeLock? = null

    private val _uiState = MutableStateFlow(StreamUiState())
    val uiState: StateFlow<StreamUiState> = _uiState.asStateFlow()

    private var frameCount = 0
    private var lastFpsTimestamp = System.currentTimeMillis()
    private var currentFps = 0

    private var streamWidth = 1920
    private var streamHeight = 1080
    private var streamFps = 60

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()

        val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "EyeCam::StreamWakeLock")

        cameraController = CameraController(applicationContext) { jpegBytes, width, height, timestampNs ->
            onCameraFrame(jpegBytes, width, height, timestampNs)
        }

        controlServer = ControlServer(8554, this)
        controlServer?.start()

        startVideoServer(8555)

        val localIp = getLocalIpAddress()
        _uiState.update { state ->
            state.copy(
                telemetry = state.telemetry.copy(
                    localIpAddress = localIp,
                    connectionDetails = "IP: $localIp:8554"
                )
            )
        }

        refreshHardwareCapabilities()
    }

    fun refreshHardwareCapabilities() {
        val sensors = cameraController?.availableSensors?.value ?: CameraUtils.queryDeviceSensors(applicationContext)
        val active = cameraController?.activeSensor?.value ?: sensors.firstOrNull { !it.isFront } ?: sensors.firstOrNull()

        _uiState.update { state ->
            state.copy(
                availableSensors = sensors,
                activeSensor = active,
                minZoomRatio = active?.minZoomRatio ?: 0.5f,
                maxZoomRatio = active?.maxZoomRatio ?: 10.0f,
                supportedZoomPresets = CameraUtils.getSupportedZoomPresets(active?.minZoomRatio ?: 0.5f, active?.maxZoomRatio ?: 10.0f),
                deviceModelName = "${Build.MANUFACTURER} ${Build.MODEL}"
            )
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START_STREAM -> startStreaming()
            ACTION_STOP_STREAM -> stopStreaming()
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    fun startStreaming() {
        if (_uiState.value.isStreaming) return

        wakeLock?.acquire(24 * 60 * 60 * 1000L) // 24h max

        val notification = buildForegroundNotification("Трансляція активна...")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        val activeSensorId = _uiState.value.activeSensor?.id ?: "0"
        cameraController?.startCamera(streamWidth, streamHeight, streamFps, activeSensorId)

        _uiState.update { it.copy(isStreaming = true) }
    }

    fun stopStreaming() {
        if (!_uiState.value.isStreaming) return

        cameraController?.stopCamera()

        if (wakeLock?.isHeld == true) {
            wakeLock?.release()
        }

        stopForeground(STOP_FOREGROUND_REMOVE)
        _uiState.update { it.copy(isStreaming = false) }
    }

    private fun startVideoServer(port: Int) {
        isVideoServerRunning = true
        serviceScope.launch(Dispatchers.IO) {
            try {
                videoServerSocket = ServerSocket(port).apply {
                    reuseAddress = true
                }
                while (isVideoServerRunning) {
                    try {
                        val client = videoServerSocket?.accept() ?: break
                        client.tcpNoDelay = true
                        client.sendBufferSize = 4 * 1024 * 1024
                        synchronized(videoClients) {
                            videoClients.add(client)
                        }
                    } catch (e: Exception) {
                        if (isVideoServerRunning) Log.e(TAG, "Video server accept error", e)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Video server socket error", e)
            }
        }
    }

    private fun onCameraFrame(jpegBytes: ByteArray, width: Int, height: Int, timestampNs: Long) {
        frameCount++
        val now = System.currentTimeMillis()
        if (now - lastFpsTimestamp >= 1000) {
            currentFps = frameCount
            frameCount = 0
            lastFpsTimestamp = now

            val bitRate = (jpegBytes.size * currentFps * 8) / 1000
            _uiState.update { state ->
                state.copy(
                    telemetry = state.telemetry.copy(
                        fps = currentFps,
                        bitrateKbps = bitRate,
                        resolutionWidth = width,
                        resolutionHeight = height
                    )
                )
            }
        }

        // Broadcast JPEG frame to connected clients (PC Studio)
        // Format: [4 bytes payload size][payload bytes]
        synchronized(videoClients) {
            if (videoClients.isEmpty()) return

            val sizeBuffer = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(jpegBytes.size).array()
            val iterator = videoClients.iterator()
            while (iterator.hasNext()) {
                val client = iterator.next()
                try {
                    val out = client.getOutputStream()
                    out.write(sizeBuffer)
                    out.write(jpegBytes)
                    out.flush()
                } catch (e: Exception) {
                    try { client.close() } catch (_: Exception) {}
                    iterator.remove()
                }
            }
        }
    }

    fun attachPreviewSurface(surface: Surface?) {
        cameraController?.setPreviewSurface(surface)
    }

    fun setZoomRatio(ratio: Float) {
        cameraController?.setZoom(ratio)
        _uiState.update { it.copy(currentZoomRatio = ratio) }
    }

    fun toggleTorch(enabled: Boolean) {
        cameraController?.setTorch(enabled)
        _uiState.update { it.copy(isTorchOn = enabled) }
    }

    fun setFrontCamera(isFront: Boolean) {
        val target = _uiState.value.availableSensors.firstOrNull { it.isFront == isFront }
        if (target != null) {
            switchSensor(target)
        }
    }

    fun switchSensor(sensor: CameraSensorInfo) {
        switchSensor(sensor.id)
    }

    fun switchSensor(sensorId: String) {
        cameraController?.switchSensor(sensorId)
        val selected = _uiState.value.availableSensors.firstOrNull { it.id == sensorId }
        _uiState.update { state ->
            state.copy(
                activeSensor = selected,
                isFrontCamera = selected?.isFront ?: false,
                minZoomRatio = selected?.minZoomRatio ?: 0.5f,
                maxZoomRatio = selected?.maxZoomRatio ?: 10.0f,
                supportedZoomPresets = CameraUtils.getSupportedZoomPresets(selected?.minZoomRatio ?: 0.5f, selected?.maxZoomRatio ?: 10.0f)
            )
        }
    }

    fun updateStreamResolution(width: Int, height: Int, fps: Int) {
        this.streamWidth = width
        this.streamHeight = height
        this.streamFps = fps

        if (_uiState.value.isStreaming) {
            cameraController?.startCamera(width, height, fps, _uiState.value.activeSensor?.id)
        }
    }

    fun setManualMode(mode: CameraMode) {
        cameraController?.setCameraMode(mode)
    }

    fun triggerAutoFocus() {
        cameraController?.triggerAutoFocus()
    }

    fun triggerFocusAtPoint(x: Float, y: Float) {
        cameraController?.triggerFocusAtPoint(x, y)
    }

    fun toggleMicrophoneMute(isMuted: Boolean) {
        _uiState.update { it.copy(isMicMuted = isMuted) }
    }

    fun setClientConnected(connected: Boolean) {
        _uiState.update { state ->
            state.copy(
                telemetry = state.telemetry.copy(
                    isPcConnected = connected,
                    connectionType = if (connected) ConnectionType.WIFI else ConnectionType.NONE
                )
            )
        }
    }

    fun getAvailableSensors(): List<CameraSensorInfo> = _uiState.value.availableSensors
    fun getActiveSensor(): CameraSensorInfo? = _uiState.value.activeSensor
    fun getCurrentZoomRatio(): Float = _uiState.value.currentZoomRatio
    fun isTorchOn(): Boolean = _uiState.value.isTorchOn

    private fun getLocalIpAddress(): String {
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            while (interfaces.hasMoreElements()) {
                val iface = interfaces.nextElement()
                val addresses = iface.inetAddresses
                while (addresses.hasMoreElements()) {
                    val addr = addresses.nextElement()
                    if (!addr.isLoopbackAddress && addr is java.net.Inet4Address) {
                        return addr.hostAddress ?: "127.0.0.1"
                    }
                }
            }
        } catch (_: Exception) {}
        return "127.0.0.1"
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "EyeCam Stream Service",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Фоновий потік відео для EyeCam"
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }
    }

    private fun buildForegroundNotification(statusText: String): Notification {
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("EyeCam Відеострім")
            .setContentText(statusText)
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    override fun onDestroy() {
        super.onDestroy()
        stopStreaming()

        isVideoServerRunning = false
        try { videoServerSocket?.close() } catch (_: Exception) {}
        synchronized(videoClients) {
            videoClients.forEach { try { it.close() } catch (_: Exception) {} }
            videoClients.clear()
        }

        controlServer?.stop()
        cameraController?.release()
        serviceScope.cancel()
    }
}
