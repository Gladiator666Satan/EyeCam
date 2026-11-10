package com.eyecam.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Color
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import android.view.Surface
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import com.eyecam.R
import com.eyecam.camera.CameraController
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.ConnectionType
import com.eyecam.model.StreamState
import com.eyecam.model.StreamUiState
import com.eyecam.model.ThermalState
import com.eyecam.network.ControlServer
import com.eyecam.network.NetworkTelemetryManager
import com.eyecam.power.PowerLockManager
import com.eyecam.thermal.DesktopThermalThrottlingPolicy
import com.eyecam.thermal.ThermalManager
import com.eyecam.ui.MainActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

class CameraStreamService : Service() {

    companion object {
        private const val TAG = "CameraStreamService"
        private const val NOTIFICATION_CHANNEL_ID = "eyecam_stream_channel"
        private const val NOTIFICATION_ID = 1001

        const val ACTION_START_STREAM = "com.eyecam.action.START_STREAM"
        const val ACTION_STOP_STREAM = "com.eyecam.action.STOP_STREAM"
        const val ACTION_TOGGLE_MUTE = "com.eyecam.action.TOGGLE_MUTE"
    }

    private val binder = LocalBinder()
    private val serviceJob = Job()
    private val serviceScope = CoroutineScope(Dispatchers.Main + serviceJob)

    lateinit var cameraController: CameraController
        private set

    private lateinit var thermalManager: ThermalManager
    private lateinit var powerLockManager: PowerLockManager
    private lateinit var controlServer: ControlServer
    private lateinit var networkTelemetryManager: NetworkTelemetryManager

    private val _uiState = MutableStateFlow(StreamUiState())
    val uiState: StateFlow<StreamUiState> = _uiState.asStateFlow()

    private var previewSurface: Surface? = null
    private var isPcConnected: Boolean = false
    private var isPcViaUsbAdb: Boolean = false

    inner class LocalBinder : Binder() {
        fun getService(): CameraStreamService = this@CameraStreamService
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "Створення CameraStreamService")

        cameraController = CameraController(applicationContext)
        thermalManager = ThermalManager(applicationContext)
        powerLockManager = PowerLockManager(applicationContext)
        networkTelemetryManager = NetworkTelemetryManager(applicationContext)

        // Інтеграція MJPEG Live Frame Callback для DirectShow на ПК
        cameraController.onJpegFrameAvailable = { jpegBytes, w, h ->
            if (cameraController.isNetworkStreamActive && isPcConnected) {
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
        observeThermalChanges()
        observeNetworkTelemetry()

        _uiState.update {
            it.copy(
                supportedZoomPresets = cameraController.supportedZoomPresets,
                currentZoomRatio = 1.0f
            )
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START_STREAM -> {
                serviceScope.launch {
                    startStreaming()
                }
            }
            ACTION_STOP_STREAM -> {
                serviceScope.launch {
                    stopStreaming()
                }
            }
            ACTION_TOGGLE_MUTE -> {
                toggleMicrophoneMute()
            }
        }
        return START_STICKY
    }

    suspend fun startStreaming() {
        if (_uiState.value.isStreaming) return
        Log.i(TAG, "Запуск відеотрансляції EyeCam")

        promoteToForegroundService()

        powerLockManager.acquireStreamLocks()

        cameraController.isNetworkStreamActive = true
        if (cameraController.hasCameraPermission()) {
            cameraController.openCamera(
                logicalCameraId = cameraController.backCameraId,
                previewSurface = previewSurface,
                encoderSurface = null
            )
        }

        _uiState.update {
            it.copy(
                isStreaming = true,
                streamState = StreamState.Active,
                supportedZoomPresets = cameraController.supportedZoomPresets
            )
        }

        refreshConnectionStatus()
        updateNotification()
    }

    suspend fun stopStreaming() {
        if (!_uiState.value.isStreaming) return
        Log.i(TAG, "Зупинка відеотрансляції EyeCam")

        cameraController.isNetworkStreamActive = false
        // НЕ закриваємо камеру повністю, якщо прев'ю активне у UI
        if (previewSurface == null) {
            cameraController.release()
        }

        powerLockManager.releaseStreamLocks()

        _uiState.update {
            it.copy(
                isStreaming = false,
                streamState = StreamState.Idle,
                isDisplayProtectionActive = false
            )
        }

        demoteFromForegroundService()
    }

    private fun promoteToForegroundService() {
        val notification = buildNotification()

        val foregroundTypes = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        } else {
            0
        }

        try {
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                notification,
                foregroundTypes
            )
            Log.i(TAG, "Foreground Service успішно активовано")
        } catch (e: Exception) {
            Log.e(TAG, "Помилка запуску Foreground Service: ${e.message}")
        }
    }

    private fun demoteFromForegroundService() {
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
    }

    private fun updateNotification() {
        if (!_uiState.value.isStreaming) return
        val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        notificationManager.notify(NOTIFICATION_ID, buildNotification())
    }

    private fun observeNetworkTelemetry() {
        serviceScope.launch {
            networkTelemetryManager.connectionStatusFlow.collect { status ->
                _uiState.update { current ->
                    current.copy(
                        telemetry = current.telemetry.copy(
                            connectionType = if (isPcViaUsbAdb) ConnectionType.USB else status.type,
                            connectionDetails = if (isPcViaUsbAdb) "USB ADB (127.0.0.1:8554)" else status.details,
                            measuredBitrateKbps = status.linkSpeedMbps * 1000
                        )
                    )
                }
            }
        }
    }

    private fun refreshConnectionStatus() {
        val currentStatus = networkTelemetryManager.getCurrentStatus()
        val connType = if (isPcViaUsbAdb) ConnectionType.USB else if (isPcConnected) ConnectionType.WIFI else currentStatus.type
        val details = if (isPcViaUsbAdb) {
            "USB ADB [ОНЛАЙН]"
        } else if (isPcConnected) {
            "${currentStatus.details} [ОНЛАЙН]"
        } else {
            "${currentStatus.details} [ОФЛАЙН]"
        }

        _uiState.update { current ->
            current.copy(
                telemetry = current.telemetry.copy(
                    connectionType = connType,
                    connectionDetails = details
                )
            )
        }
    }

    fun setPreviewSurface(surface: Surface?) {
        this.previewSurface = surface
        cameraController.updatePreviewSurface(surface)
        if (surface != null && cameraController.hasCameraPermission() && !cameraController.isNetworkStreamActive) {
            cameraController.openCamera(
                logicalCameraId = cameraController.backCameraId,
                previewSurface = surface
            )
        }
    }

    fun toggleMicrophoneMute() {
        _uiState.update {
            it.copy(isMicMuted = !it.isMicMuted)
        }
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
            thermalManager.thermalStatusFlow.collect { sample ->
                val policy = DesktopThermalThrottlingPolicy.evaluate(sample.thermalState)

                if (policy.shouldThrottleFps || policy.shouldReduceBitrate) {
                    onDesktopThermalThrottlingTriggered(
                        sample.thermalState,
                        policy.maxAllowedFps,
                        policy.bitrateCapKbps,
                        policy.reason
                    )
                }

                _uiState.update { current ->
                    current.copy(
                        telemetry = current.telemetry.copy(
                            thermalState = sample.thermalState,
                            deviceTemperatureC = sample.temperatureC,
                            targetFps = if (policy.shouldThrottleFps) {
                                policy.maxAllowedFps
                            } else {
                                current.telemetry.targetFps
                            }
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

    private fun onDesktopThermalThrottlingTriggered(
        thermalState: ThermalState,
        adjustedFps: Int,
        adjustedBitrateKbps: Int,
        reason: String
    ) {
        Log.w(TAG, "Тротлінг [$thermalState] -> FPS: $adjustedFps, Бітрейт: ${adjustedBitrateKbps}Kbps ($reason)")
    }

    private fun buildNotification(): Notification {
        val stopIntent = Intent(this, CameraStreamService::class.java).apply {
            action = ACTION_STOP_STREAM
        }
        val stopPendingIntent = PendingIntent.getService(
            this, 1, stopIntent, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val muteIntent = Intent(this, CameraStreamService::class.java).apply {
            action = ACTION_TOGGLE_MUTE
        }
        val mutePendingIntent = PendingIntent.getService(
            this, 2, muteIntent, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val micStatus = if (_uiState.value.isMicMuted) "Мікрофон: ВИМКНЕНО" else "Мікрофон: АКТИВНИЙ"
        val zoomLabel = if (_uiState.value.isFrontCamera) "FRONT" else "${_uiState.value.currentZoomRatio}x"
        val thermalName = _uiState.value.telemetry.thermalState.descriptionUa

        return NotificationCompat.Builder(this, NOTIFICATION_CHANNEL_ID)
            .setContentTitle("EyeCam: Трансляція активна")
            .setContentText("Зум: $zoomLabel | $micStatus | $thermalName")
            .setSmallIcon(R.drawable.ic_eyecam_logo)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setColor(Color.BLACK)
            .addAction(android.R.drawable.ic_menu_close_clear_cancel, "Зупинити", stopPendingIntent)
            .addAction(
                if (_uiState.value.isMicMuted) android.R.drawable.stat_notify_call_mute else android.R.drawable.presence_audio_online,
                if (_uiState.value.isMicMuted) "Увімк. мікрофон" else "Вимк. мікрофон",
                mutePendingIntent
            )
            .setContentIntent(
                PendingIntent.getActivity(
                    this, 0,
                    Intent(this, MainActivity::class.java),
                    PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
                )
            )
            .build()
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                NOTIFICATION_CHANNEL_ID,
                "EyeCam Трансляція",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Сповіщення активного стрімінгу камери EyeCam"
                enableLights(false)
                enableVibration(false)
                setShowBadge(false)
            }
            val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            manager.createNotificationChannel(channel)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.i(TAG, "Знищення CameraStreamService та звільнення ресурсів")
        serviceJob.cancel()
        controlServer.stop()
        cameraController.release()
        thermalManager.stopMonitoring()
        powerLockManager.releaseAll()
    }
}
