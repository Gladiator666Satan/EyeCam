package com.eyecam.service

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
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
import androidx.lifecycle.LifecycleService
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
 * Ongoing Foreground LifecycleService, що підтримує життєвий цикл захоплення CameraX,
 * апаратного H.264 кодування, TCP/ADB сервера керування і трансляції (:8554) та моніторингу USB / Wi-Fi.
 */
class CameraStreamService : LifecycleService() {

    private val serviceJob = SupervisorJob()
    private val serviceScope = CoroutineScope(Dispatchers.Default + serviceJob)

    private val binder = LocalBinder()

    private lateinit var powerLockManager: PowerLockManager
    private lateinit var thermalManager: ThermalManager
    lateinit var cameraController: CameraController
        private set
    private lateinit var controlServer: ControlServer

    private var currentPreviewSurface: Surface? = null
    private var currentPreviewWidth: Int = 0
    private var currentPreviewHeight: Int = 0
    private var currentDisplayRotation: Int = Surface.ROTATION_0
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
        cameraController.attachLifecycleOwner(this)
        cameraController.onH264FrameAvailable = { annexBBytes, isKeyFrame, _ ->
            if (_uiState.value.isStreaming) {
                controlServer.sendH264Frame(annexBBytes, isKeyFrame)
            }
        }
        cameraController.onTelemetryMeasured = { fps, bitrateKbps ->
            _uiState.update { current ->
                current.copy(
                    telemetry = current.telemetry.copy(
                        fps = if (_uiState.value.isStreaming) fps.toInt() else 0,
                        bitrateKbps = if (_uiState.value.isStreaming) bitrateKbps else 0,
                        batteryPercentage = readBatteryPercent().toInt(),
                        batteryTemperatureC = readBatteryTempC()
                    )
                )
            }
        }
        cameraController.onStateChanged = {
            _uiState.update { current ->
                current.copy(
                    currentZoomRatio = cameraController.currentZoomRatio,
                    isFrontCamera = cameraController.isFrontCameraActive,
                    minZoomRatio = cameraController.minZoomRatio,
                    maxZoomRatio = cameraController.maxZoomRatio,
                    supportedZoomPresets = cameraController.supportedZoomPresets.toList(),
                    telemetry = current.telemetry.copy(
                        resolutionWidth = cameraController.streamWidth,
                        resolutionHeight = cameraController.streamHeight,
                        targetFps = cameraController.streamFps
                    )
                )
            }
            if (controlServer.isClientConnected) {
                controlServer.sendTelemetryState()
            }
        }

        controlServer = ControlServer(
            port = 8554,
            cameraController = cameraController,
            onKeyframeRequested = { cameraController.requestKeyFrame() },
            onClientConnectionChanged = { connected, viaUsb, remoteIp ->
                isPcConnected = connected
                isPcViaUsbAdb = viaUsb
                if (connected && !_uiState.value.isStreaming && cameraController.hasCameraPermission()) {
                    serviceScope.launch(Dispatchers.Main) {
                        startStreaming()
                    }
                }
                refreshConnectionStatus()
                Log.i(TAG, "PC Client connection changed: connected=$connected, viaUsb=$viaUsb, ip=$remoteIp")
            },
            onZoomUpdatedFromPc = { _, _ ->
                _uiState.update {
                    it.copy(
                        currentZoomRatio = cameraController.currentZoomRatio,
                        isFrontCamera = cameraController.isFrontCameraActive
                    )
                }
                updateNotification()
            },
            onResolutionUpdatedFromPc = { w, h, fps ->
                _uiState.update { current ->
                    current.copy(
                        telemetry = current.telemetry.copy(
                            resolutionWidth = w,
                            resolutionHeight = h,
                            targetFps = fps
                        )
                    )
                }
            },
            batteryLevelProvider = { readBatteryPercent() },
            thermalTempProvider = { readBatteryTempC() }
        )
        controlServer.start()

        createNotificationChannel()
        refreshHardwareCapabilities()
        startConnectionMonitorLoop()
    }

    fun refreshHardwareCapabilities() {
        val sensors = cameraController.discoverAvailableSensors()
        val hasPerm = cameraController.hasCameraPermission()
        val modelName = "${Build.MANUFACTURER.replaceFirstChar { it.uppercase() }} ${Build.MODEL}"

        _uiState.update { current ->
            current.copy(
                hasCameraPermission = hasPerm,
                activeSensor = sensors.firstOrNull(),
                availableSensors = sensors,
                minZoomRatio = cameraController.minZoomRatio,
                maxZoomRatio = cameraController.maxZoomRatio,
                supportedZoomPresets = cameraController.supportedZoomPresets,
                deviceModelName = modelName
            )
        }

        if (hasPerm && currentPreviewSurface != null) {
            cameraController.updatePreviewSurface(
                currentPreviewSurface,
                currentPreviewWidth,
                currentPreviewHeight,
                currentDisplayRotation
            )
            if (!cameraController.isCameraRunning()) {
                cameraController.openCamera(previewSurface = currentPreviewSurface)
            }
        }
    }

    fun attachPreviewSurface(
        surface: Surface?,
        width: Int = 0,
        height: Int = 0,
        displayRotation: Int = Surface.ROTATION_0
    ) {
        currentPreviewSurface = surface
        if (width > 0 && height > 0) {
            currentPreviewWidth = width
            currentPreviewHeight = height
        }
        currentDisplayRotation = displayRotation

        if (surface != null && surface.isValid && cameraController.hasCameraPermission()) {
            cameraController.updatePreviewSurface(surface, currentPreviewWidth, currentPreviewHeight, displayRotation)
            if (!cameraController.isCameraRunning()) {
                cameraController.openCamera(previewSurface = surface)
            }
        } else if (surface == null) {
            // Detach UI surface ONLY — keep CameraX + HardwareEncoder running seamlessly!
            cameraController.updatePreviewSurface(null, 0, 0, displayRotation)
            if (!_uiState.value.isStreaming) {
                cameraController.closeCamera()
            }
        }
    }

    fun triggerAutoFocus() {
        cameraController.triggerAutoFocus()
    }

    fun triggerFocusAtPoint(x: Float, y: Float) {
        cameraController.triggerFocusAtPoint(x, y)
    }

    private fun startConnectionMonitorLoop() {
        serviceScope.launch {
            while (isActive) {
                refreshConnectionStatus()
                delay(1500)
            }
        }
    }

    private fun refreshConnectionStatus() {
        val usbConnected = isUsbCableConnected()
        val localIp = getLocalWifiIpAddress()

        val (connType, connDetails) = when {
            isPcConnected && isPcViaUsbAdb -> {
                ConnectionType.USB to "USB ADB • ПК ПІДКЛЮЧЕНО"
            }
            isPcConnected -> {
                ConnectionType.WIFI to "WI-FI • ПК ПІДКЛЮЧЕНО"
            }
            usbConnected -> {
                val ipSuffix = if (localIp.isNotEmpty()) " • IP: $localIp" else ""
                ConnectionType.USB to "USB ПІДКЛЮЧЕНО$ipSuffix"
            }
            localIp.isNotEmpty() -> {
                ConnectionType.WIFI to "WI-FI: $localIp"
            }
            else -> {
                ConnectionType.NONE to "ОФЛАЙН (Підключіть USB або Wi-Fi)"
            }
        }

        _uiState.update { current ->
            current.copy(
                hasCameraPermission = cameraController.hasCameraPermission(),
                telemetry = current.telemetry.copy(
                    connectionType = connType,
                    connectionDetails = connDetails,
                    isPcConnected = isPcConnected,
                    localIpAddress = localIp
                )
            )
        }
    }

    private fun isUsbCableConnected(): Boolean {
        try {
            val usbState = registerReceiver(null, IntentFilter("android.hardware.usb.action.USB_STATE"))
            if (usbState != null && (usbState.getBooleanExtra("connected", false) || usbState.getBooleanExtra("configured", false))) {
                return true
            }
        } catch (_: Exception) {}

        try {
            val battIntent = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            val plugged = battIntent?.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1) ?: -1
            if (plugged == BatteryManager.BATTERY_PLUGGED_USB) {
                return true
            }
        } catch (_: Exception) {}

        return false
    }

    private fun getLocalWifiIpAddress(): String {
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces() ?: return ""
            for (intf in interfaces) {
                if (!intf.isUp || intf.isLoopback) continue
                val name = intf.name.lowercase()
                if (!name.startsWith("wlan") && !name.startsWith("eth") && !name.startsWith("ap")) continue
                for (addr in intf.inetAddresses) {
                    if (!addr.isLoopbackAddress && addr is Inet4Address) {
                        return addr.hostAddress ?: ""
                    }
                }
            }
        } catch (_: Exception) {}
        return ""
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        when (intent?.action) {
            ACTION_START_STREAM -> startStreaming()
            ACTION_STOP_STREAM -> stopStreaming()
            ACTION_TOGGLE_MUTE -> toggleMicrophoneMute()
            ACTION_SWITCH_SENSOR -> {
                val sensorId = intent.getStringExtra(EXTRA_SENSOR_ID)
                sensorId?.let { id ->
                    val targetSensor = _uiState.value.availableSensors.find { it.id == id }
                    targetSensor?.let { switchSensor(it) }
                }
            }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent): IBinder? {
        super.onBind(intent)
        return binder
    }

    /**
     * Запуск трансляції: безпечне підняття Foreground Service з перевіркою дозволів Android 14/15/16.
     */
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
                currentZoomRatio = cameraController.currentZoomRatio
            )
        }
        updateNotification()
    }

    /**
     * Перемикання на тиловий пресет зуму / об'єктиву (наприклад 0.5x, 1x, 2x, 3x, 5x, 10x).
     * Автоматично виходить із фронтальної камери, якщо вона була активною.
     */
    fun selectLensPreset(zoom: Float) {
        cameraController.switchToSensor("${zoom}x")
        _uiState.update {
            it.copy(
                isFrontCamera = cameraController.isFrontCameraActive,
                currentZoomRatio = cameraController.currentZoomRatio,
                minZoomRatio = cameraController.minZoomRatio,
                maxZoomRatio = cameraController.maxZoomRatio
            )
        }
        updateNotification()
    }

    fun setFrontCamera(useFront: Boolean) {
        cameraController.switchFrontBack(useFront)
        _uiState.update {
            it.copy(
                isFrontCamera = useFront,
                currentZoomRatio = 1.0f,
                minZoomRatio = cameraController.minZoomRatio,
                maxZoomRatio = cameraController.maxZoomRatio
            )
        }
        updateNotification()
    }

    fun switchSensor(newSensor: CameraSensorInfo) {
        cameraController.switchPhysicalCameraSeamless(newSensor.id)
        _uiState.update { it.copy(activeSensor = newSensor) }
        updateNotification()
    }

    fun switchSensor(sensorId: String) {
        val target = _uiState.value.availableSensors.find { it.id == sensorId }
        if (target != null) {
            switchSensor(target)
        } else {
            cameraController.switchToSensor(sensorId)
        }
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

    private fun readBatteryPercent(): Float {
        return try {
            val intent = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            val level = intent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
            val scale = intent?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
            if (level >= 0 && scale > 0) (level * 100f) / scale else 100f
        } catch (_: Exception) {
            100f
        }
    }

    private fun readBatteryTempC(): Float {
        return try {
            val intent = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            val tenths = intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 350) ?: 350
            tenths / 10.0f
        } catch (_: Exception) {
            35.0f
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
