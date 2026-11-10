package com.eyecam.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.BitmapFactory
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.util.Log
import android.view.Surface
import androidx.core.app.NotificationCompat
import com.eyecam.camera.CameraController
import com.eyecam.hardware.PowerLockManager
import com.eyecam.hardware.ThermalManager
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.ConnectionType
import com.eyecam.model.SensorType
import com.eyecam.model.StreamTelemetry
import com.eyecam.model.StreamUiState
import com.eyecam.model.ThermalState
import com.eyecam.network.ControlServer
import com.eyecam.ui.MainActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.net.Inet4Address
import java.net.NetworkInterface

/**
 * Ключовий Android Foreground Service додатку EyeCam.
 * Відповідає за:
 * - Захоплення відеопотоку через Camera2 (з підтримкою перемикання сенсорів 0.5x..10x та 60 FPS).
 * - Трансляцію на Windows Studio через вбудований ControlServer (порт 8554, USB ADB / Wi-Fi).
 * - Утримання WakeLock & WifiLock для запобігання засинанню при блокуванні екрана смартфону.
 * - Моніторинг теплового стану процесора (Thermal Throttling) та динамічну оптимізацію FPS.
 * - Постійне оновлення інтерактивного системного сповіщення з кнопками керування.
 */
class CameraStreamService : Service() {

    private val binder = LocalBinder()
    private val serviceScope = CoroutineScope(Dispatchers.Default + Job())

    lateinit var cameraController: CameraController
        private set
    lateinit var controlServer: ControlServer
        private set
    private lateinit var powerLockManager: PowerLockManager
    private lateinit var thermalManager: ThermalManager

    private val _uiState = MutableStateFlow(StreamUiState())
    val uiState: StateFlow<StreamUiState> = _uiState.asStateFlow()

    private var currentPreviewSurface: Surface? = null
    private var isPcConnected: Boolean = false
    private var isPcViaUsbAdb: Boolean = false

    companion object {
        private const val TAG = "CameraStreamService"

        private const val NOTIFICATION_CHANNEL_ID = "eyecam_stream_channel"
        private const val NOTIFICATION_CHANNEL_NAME = "EyeCam Streaming Service"
        private const val NOTIFICATION_ID = 1001

        const val ACTION_START_STREAM = "com.eyecam.action.START_STREAM"
        const val ACTION_STOP_STREAM = "com.eyecam.action.STOP_STREAM"
        const val ACTION_TOGGLE_MUTE = "com.eyecam.action.TOGGLE_MUTE"
        const val ACTION_SWITCH_SENSOR = "com.eyecam.action.SWITCH_SENSOR"
        const val EXTRA_SENSOR_ID = "extra_sensor_id"
    }

    inner class LocalBinder : Binder() {
        fun getService(): CameraStreamService = this@CameraStreamService
    }

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "Ініціалізація CameraStreamService...")

        powerLockManager = PowerLockManager(this)
        thermalManager = ThermalManager(this) { thermalState, adjustedFps, adjustedBitrateKbps, reason ->
            onDesktopThermalThrottlingTriggered(thermalState, adjustedFps, adjustedBitrateKbps, reason)
        }

        cameraController = CameraController(this)
        cameraController.onJpegFrameAvailable = { jpegBytes, _, _ ->
            if (_uiState.value.isStreaming) {
                controlServer.sendVideoFrame(jpegBytes)
            }
        }

        controlServer = ControlServer(
            port = 8554,
            cameraController = cameraController,
            onKeyframeRequested = {},
            onClientConnectionChanged = { connected, viaUsb, remoteIp ->
                isPcConnected = connected
                isPcViaUsbAdb = viaUsb
                // Якщо ПК підключився — автоматично активуємо передачу кадрів
                if (connected && !_uiState.value.isStreaming && cameraController.hasCameraPermission()) {
                    serviceScope.launch(Dispatchers.Main) {
                        startStreaming()
                    }
                }
                refreshConnectionStatus()
                Log.i(TAG, "PC Client connection changed: connected=$connected, viaUsb=$viaUsb, ip=$remoteIp")
            },
            onZoomUpdatedFromPc = { zoom, isFront ->
                _uiState.update {
                    it.copy(
                        currentZoomRatio = zoom,
                        isFrontCamera = isFront
                    )
                }
            },
            onResolutionUpdatedFromPc = { w, h, fps ->
                cameraController.setResolution(w, h, fps)
                _uiState.update { current ->
                    current.copy(
                        telemetry = current.telemetry.copy(
                            resolutionWidth = w,
                            resolutionHeight = h,
                            targetFps = fps
                        )
                    )
                }
            }
        )
        controlServer.start()

        createNotificationChannel()
        refreshHardwareCapabilities()
        refreshConnectionStatus()
    }

    fun refreshHardwareCapabilities() {
        val sensors = cameraController.discoverAvailableSensors()
        _uiState.update { current ->
            current.copy(
                availableSensors = sensors,
                activeSensor = sensors.find { it.id == cameraController.backCameraId } ?: sensors.firstOrNull(),
                supportedZoomPresets = cameraController.supportedZoomPresets,
                hasCameraPermission = cameraController.hasCameraPermission(),
                minZoomRatio = cameraController.minZoomRatio,
                maxZoomRatio = cameraController.maxZoomRatio
            )
        }
    }

    private fun refreshConnectionStatus() {
        val wifiIp = getLocalWifiIpAddress()
        val connType = when {
            isPcConnected && isPcViaUsbAdb -> ConnectionType.USB_ADB
            isPcConnected -> ConnectionType.WIFI_LAN
            wifiIp.isNotEmpty() -> ConnectionType.WIFI_LAN
            else -> ConnectionType.DISCONNECTED
        }

        _uiState.update { current ->
            current.copy(
                connectionType = connType,
                deviceIpAddress = if (wifiIp.isNotEmpty()) wifiIp else "USB ADB (127.0.0.1:8554)"
            )
        }
    }

    private fun getLocalWifiIpAddress(): String {
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            while (interfaces.hasMoreElements()) {
                val intf = interfaces.nextElement()
                if (intf.isLoopback || !intf.isUp) continue
                val addrs = intf.inetAddresses
                while (addrs.hasMoreElements()) {
                    val addr = addrs.nextElement()
                    if (!addr.isLoopbackAddress && addr is Inet4Address) {
                        val host = addr.hostAddress ?: ""
                        if (host.startsWith("192.168.") || host.startsWith("10.") || host.startsWith("172.")) {
                            return host
                        }
                    }
                }
            }
        } catch (_: Exception) {}
        return ""
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START_STREAM -> startStreaming()
            ACTION_STOP_STREAM -> stopStreaming()
            ACTION_TOGGLE_MUTE -> toggleMicrophoneMute()
            ACTION_SWITCH_SENSOR -> {
                val sensorId = intent.getStringExtra(EXTRA_SENSOR_ID)
                if (sensorId != null) {
                    val target = _uiState.value.availableSensors.find { it.id == sensorId }
                    if (target != null) switchSensor(target)
                }
            }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    fun attachPreviewSurface(surface: Surface?) {
        this.currentPreviewSurface = surface
        cameraController.updatePreviewSurface(surface)
    }

    /**
     * Повноцінний запуск Foreground Service з утриманням блокувань CPU/Wi-Fi
     * та стартом потоку камери.
     */
    fun startStreaming() {
        if (_uiState.value.isStreaming) return
        Log.i(TAG, "Старт трансляції EyeCam...")

        // Утримання WakeLock & WifiLock для запобігання засинанню
        powerLockManager.acquireCpuWakeLock()
        powerLockManager.acquireWifiHighPerfLock()

        val notification = buildForegroundNotification()

        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                var fgsTypes = ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    fgsTypes = fgsTypes or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
                }
                startForeground(NOTIFICATION_ID, notification, fgsTypes)
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Fallback startForeground через обмеження FGS: ${e.message}")
            try {
                startForeground(NOTIFICATION_ID, notification)
            } catch (_: Exception) {}
        }

        cameraController.isNetworkStreamActive = true
        if (currentPreviewSurface != null) {
            cameraController.openCamera(previewSurface = currentPreviewSurface)
        } else {
            cameraController.openCamera(previewSurface = null)
        }

        _uiState.update { current ->
            current.copy(
                isStreaming = true,
                telemetry = current.telemetry.copy(
                    fps = 60,
                    bitrateKbps = 18000
                )
            )
        }

        refreshConnectionStatus()
        observeThermalChanges()
    }

    /**
     * Зупинка трансляції та очищення ресурсів.
     */
    fun stopStreaming() {
        Log.i(TAG, "Зупинка трансляції EyeCam...")

        cameraController.isNetworkStreamActive = false
        thermalManager.stopMonitoring()
        powerLockManager.releaseAll()

        _uiState.update { current ->
            current.copy(
                isStreaming = false,
                isDisplayProtectionActive = false,
                telemetry = current.telemetry.copy(fps = 0, bitrateKbps = 0)
            )
        }

        try {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } catch (_: Exception) {}
    }

    fun toggleMicrophoneMute() {
        _uiState.update { it.copy(isMicMuted = !it.isMicMuted) }
        updateNotification()
    }

    fun setZoomRatio(zoom: Float) {
        cameraController.setZoomRatio(zoom)
        _uiState.update {
            it.copy(
                currentZoomRatio = zoom.coerceIn(cameraController.minZoomRatio, cameraController.maxZoomRatio)
            )
        }
        updateNotification()
    }

    fun setFrontCamera(useFront: Boolean) {
        cameraController.switchFrontBack(useFront)
        _uiState.update {
            it.copy(
                isFrontCamera = useFront,
                currentZoomRatio = 1.0f
            )
        }
        updateNotification()
    }

    fun switchSensor(newSensor: CameraSensorInfo) {
        cameraController.switchPhysicalCameraSeamless(newSensor.id)
        _uiState.update { it.copy(activeSensor = newSensor) }
        updateNotification()
    }

    fun setDisplayProtection(active: Boolean) {
        _uiState.update { it.copy(isDisplayProtectionActive = active) }
    }

    private fun observeThermalChanges() {
        serviceScope.launch {
            thermalManager.thermalState.collect { thermalState ->
                val policy = thermalManager.getPolicyForState(thermalState)

                _uiState.update { current ->
                    current.copy(
                        telemetry = current.telemetry.copy(
                            thermalState = thermalState,
                            targetFps = policy.recommendedFps,
                            targetBitrateKbps = (ThermalManager.BASELINE_BITRATE_KBPS * policy.bitrateReductionRatio).toInt()
                        ),
                        isDisplayProtectionActive = if (policy.shouldEnforceDisplayProtection) {
                            true
                        } else {
                            current.isDisplayProtectionActive
                        }
                    )
                }
                updateNotification()
            }
        }
    }

    fun triggerAutoFocus() {
        cameraController.triggerAutoFocus()
    }

    fun triggerFocusAtPoint(xNorm: Float, yNorm: Float) {
        cameraController.triggerFocusAtPoint(xNorm, yNorm)
    }

    private fun onDesktopThermalThrottlingTriggered(
        thermalState: ThermalState,
        adjustedFps: Int,
        adjustedBitrateKbps: Int,
        reason: String
    ) {
        Log.w(TAG, "Термальне регулювання активовано: $reason | $adjustedFps FPS, $adjustedBitrateKbps kbps")
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                NOTIFICATION_CHANNEL_ID,
                NOTIFICATION_CHANNEL_NAME,
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Активне сповіщення відеотрансляції EyeCam Ultra-Low Latency"
                setShowBadge(false)
                lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }
    }

    private fun buildForegroundNotification(): Notification {
        val state = _uiState.value
        val openAppIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val pOpenApp = PendingIntent.getActivity(
            this, 0, openAppIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val stopIntent = Intent(this, CameraStreamService::class.java).apply {
            action = ACTION_STOP_STREAM
        }
        val pStop = PendingIntent.getService(
            this, 1, stopIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val muteIntent = Intent(this, CameraStreamService::class.java).apply {
            action = ACTION_TOGGLE_MUTE
        }
        val pMute = PendingIntent.getService(
            this, 2, muteIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val connTitle = when (state.connectionType) {
            ConnectionType.USB_ADB -> "🔌 USB ADB Підключено"
            ConnectionType.WIFI_LAN -> "📶 Wi-Fi: ${state.deviceIpAddress}:8554"
            else -> "Очікування підключення..."
        }

        val zoomText = if (state.isFrontCamera) "Селфі" else "${String.format("%.1f", state.currentZoomRatio)}x"
        val contentText = "$connTitle • ${state.telemetry.fps} FPS • Зум: $zoomText"

        return NotificationCompat.Builder(this, NOTIFICATION_CHANNEL_ID)
            .setContentTitle("EyeCam 4K Трансляція Активна")
            .setContentText(contentText)
            .setSmallIcon(android.R.drawable.presence_video_online)
            .setContentIntent(pOpenApp)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .addAction(
                android.R.drawable.ic_media_pause,
                "Зупинити",
                pStop
            )
            .addAction(
                if (state.isMicMuted) android.R.drawable.stat_notify_chat else android.R.drawable.ic_btn_speak_now,
                if (state.isMicMuted) "Увімк. мікрофон" else "Вимк. мікрофон",
                pMute
            )
            .build()
    }

    private fun updateNotification() {
        if (!_uiState.value.isStreaming) return
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        manager.notify(NOTIFICATION_ID, buildForegroundNotification())
    }

    override fun onDestroy() {
        super.onDestroy()
        stopStreaming()
        controlServer.stop()
        cameraController.release()
        Log.i(TAG, "CameraStreamService знищено.")
    }
}
