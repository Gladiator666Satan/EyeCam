package com.eyecam.service

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.graphics.Color
import android.os.BatteryManager
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.util.Log
import android.view.Surface
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import com.eyecam.R
import com.eyecam.camera.CameraController
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.ConnectionType
import com.eyecam.model.StreamUiState
import com.eyecam.model.ThermalState
import com.eyecam.network.ControlServer
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.net.Inet4Address
import java.net.NetworkInterface

/**
 * Ongoing Foreground Service, що підтримує життєвий цикл захоплення Camera2,
 * TCP/ADB сервера керування і трансляції (:8554) та моніторингу USB / Wi-Fi підключення.
 */
class CameraStreamService : Service() {

    private val serviceJob = SupervisorJob()
    private val serviceScope = CoroutineScope(Dispatchers.Default + serviceJob)

    private val binder = LocalBinder()

    private lateinit var powerLockManager: PowerLockManager
    private lateinit var thermalManager: ThermalManager
    lateinit var cameraController: CameraController
        private set
    private lateinit var controlServer: ControlServer

    private var currentPreviewSurface: Surface? = null
    private var isPcConnected = false
    private var isPcViaUsbAdb = false

    private val _uiState = MutableStateFlow(StreamUiState())
    val uiState: StateFlow<StreamUiState> = _uiState.asStateFlow()

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
            onKeyframeRequested = {
                cameraController.triggerAutoFocus()
            },
            onClientConnectionChanged = { connected, isUsb, clientIp ->
                onDesktopClientConnectionChanged(connected, isUsb, clientIp)
            },
            onZoomUpdatedFromPc = { zoomRatio, isFront ->
                _uiState.update { state ->
                    state.copy(
                        currentZoomRatio = zoomRatio,
                        isFrontCamera = isFront
                    )
                }
            },
            onResolutionUpdatedFromPc = { w, h, fps ->
                _uiState.update { state ->
                    state.copy(
                        streamWidth = w,
                        streamHeight = h,
                        streamFps = fps
                    )
                }
            }
        )
        controlServer.start()

        createNotificationChannel()
        startConnectionMonitoring()
        startTelemetryLoop()

        // Початкова синхронізація сенсорів
        val sensors = cameraController.availableSensors
        _uiState.update { state ->
            state.copy(
                availableSensors = sensors,
                minZoomRatio = cameraController.minZoomRatio,
                maxZoomRatio = cameraController.maxZoomRatio,
                supports4K = cameraController.supports4K,
                supportedZoomPresets = cameraController.supportedZoomPresets,
                streamWidth = cameraController.streamWidth,
                streamHeight = cameraController.streamHeight,
                streamFps = cameraController.streamFps
            )
        }
    }

    private fun startConnectionMonitoring() {
        serviceScope.launch {
            while (isActive) {
                refreshConnectionStatus()
                delay(1500)
            }
        }
    }

    private fun refreshConnectionStatus() {
        var ip = "Очікування мережі..."
        var isWifi = false
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            for (iface in interfaces) {
                if (iface.isUp && !iface.isLoopback) {
                    val addrs = iface.inetAddresses
                    for (addr in addrs) {
                        if (!addr.isLoopbackAddress && addr is Inet4Address) {
                            val host = addr.hostAddress ?: ""
                            if (host.isNotEmpty() && !host.startsWith("127.")) {
                                ip = host
                                if (iface.name.contains("wlan") || iface.name.contains("ap")) {
                                    isWifi = true
                                }
                                break
                            }
                        }
                    }
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Помилка визначення локального IP: ${e.message}")
        }

        // Перевірка стану USB з'єднання через IntentFilter
        val batteryIntent = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val plugged = batteryIntent?.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1) ?: -1
        val isUsbPlugged = (plugged == BatteryManager.BATTERY_PLUGGED_USB)
        val level = batteryIntent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = batteryIntent?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        val batteryPct = if (level >= 0 && scale > 0) level / scale.toFloat() else 1.0f

        val activeConnType = when {
            isPcConnected && isPcViaUsbAdb -> ConnectionType.USB
            isPcConnected -> ConnectionType.WIFI
            isUsbPlugged -> ConnectionType.USB
            isWifi -> ConnectionType.WIFI
            else -> ConnectionType.NONE
        }

        _uiState.update { state ->
            state.copy(
                telemetry = state.telemetry.copy(
                    connectionType = activeConnType,
                    localIpAddress = ip,
                    batteryLevel = batteryPct,
                    isCharging = isUsbPlugged || plugged == BatteryManager.BATTERY_PLUGGED_AC
                )
            )
        }
    }

    private fun onDesktopClientConnectionChanged(connected: Boolean, isUsb: Boolean, clientIp: String) {
        this.isPcConnected = connected
        this.isPcViaUsbAdb = isUsb

        Log.i(TAG, "Зміна статусу клієнта ПК: connected=$connected, isUsb=$isUsb, ip=$clientIp")

        refreshConnectionStatus()

        if (connected && !_uiState.value.isStreaming) {
            startStreaming()
        }
    }

    private fun startTelemetryLoop() {
        serviceScope.launch {
            while (isActive) {
                delay(1000)
                if (_uiState.value.isStreaming) {
                    _uiState.update { state ->
                        state.copy(
                            telemetry = state.telemetry.copy(
                                fps = cameraController.streamFps,
                                bitrateKbps = 18000
                            )
                        )
                    }
                }
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START_STREAM -> startStreaming()
            ACTION_STOP_STREAM -> stopStreaming()
            ACTION_TOGGLE_MUTE -> toggleMicrophone()
            ACTION_SWITCH_SENSOR -> {
                val sensorId = intent.getStringExtra(EXTRA_SENSOR_ID)
                if (sensorId != null) {
                    switchSensor(sensorId)
                }
            }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    fun startStreaming() {
        if (_uiState.value.isStreaming) return

        val hasCamPerm = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        if (!hasCamPerm) {
            Log.w(TAG, "Неможливо запустити трансляцію: відсутній дозвіл CAMERA")
            return
        }

        Log.i(TAG, "Запуск потоку трансляції та підняття Foreground Service...")

        powerLockManager.acquireWakeLock()
        if (_uiState.value.telemetry.connectionType == ConnectionType.WIFI) {
            powerLockManager.acquireWifiLock()
        }

        thermalManager.startMonitoring()

        // Безпечний запуск Foreground Service для Android 14/15/16 (перевірка фактично наданих дозволів)
        val notification = buildNotification()
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                var foregroundTypes = ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
                val hasMicPerm = ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
                if (hasMicPerm) {
                    foregroundTypes = foregroundTypes or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
                }
                startForeground(NOTIFICATION_ID, notification, foregroundTypes)
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Помилка startForeground: ${e.message}", e)
        }

        cameraController.isNetworkStreamActive = true
        cameraController.openCamera(
            logicalCameraId = if (_uiState.value.isFrontCamera) (cameraController.frontCameraId ?: "1") else cameraController.backCameraId,
            previewSurface = currentPreviewSurface
        )

        _uiState.update { it.copy(isStreaming = true) }
        updateNotification()
    }

    fun stopStreaming() {
        if (!_uiState.value.isStreaming) return
        Log.i(TAG, "Зупинка трансляції...")

        cameraController.isNetworkStreamActive = false
        cameraController.closeCamera()
        thermalManager.stopMonitoring()
        powerLockManager.releaseAll()

        stopForeground(STOP_FOREGROUND_REMOVE)
        _uiState.update { it.copy(isStreaming = false) }
    }

    fun setPreviewSurface(surface: Surface?) {
        this.currentPreviewSurface = surface
        cameraController.updatePreviewSurface(surface)
    }

    fun setZoom(zoom: Float) {
        cameraController.setZoomRatio(zoom)
        _uiState.update { it.copy(currentZoomRatio = zoom) }
    }

    fun switchSensor(sensorModuleOrId: String) {
        cameraController.switchToSensor(sensorModuleOrId)
        val isFront = sensorModuleOrId.equals("front", ignoreCase = true) || sensorModuleOrId.equals("self", ignoreCase = true)
        val zoom = sensorModuleOrId.removeSuffix("x").toFloatOrNull() ?: 1.0f
        _uiState.update {
            it.copy(
                currentZoomRatio = zoom,
                isFrontCamera = isFront
            )
        }
    }

    fun switchFrontBack() {
        val targetFront = !_uiState.value.isFrontCamera
        cameraController.switchFrontBack(targetFront)
        _uiState.update {
            it.copy(
                isFrontCamera = targetFront,
                currentZoomRatio = 1.0f
            )
        }
    }

    fun toggleMicrophone() {
        _uiState.update { it.copy(isMicMuted = !it.isMicMuted) }
        updateNotification()
    }

    fun setDisplayProtection(active: Boolean) {
        _uiState.update { it.copy(isDisplayProtectionActive = active) }
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
                if (_uiState.value.isMicMuted) "Увімкнути звук" else "Вимкнути звук",
                mutePendingIntent
            )
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .build()
    }

    private fun updateNotification() {
        if (_uiState.value.isStreaming) {
            val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            notificationManager.notify(NOTIFICATION_ID, buildNotification())
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                NOTIFICATION_CHANNEL_ID,
                NOTIFICATION_CHANNEL_NAME,
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Постійне сповіщення активної відеотрансляції EyeCam"
                enableLights(false)
                enableVibration(false)
                setShowBadge(false)
            }
            val manager = getSystemService(NotificationManager::class.java)
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
