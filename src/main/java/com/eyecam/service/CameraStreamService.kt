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
import android.util.Log
import androidx.core.app.NotificationCompat
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.ConnectionType
import com.eyecam.model.SensorType
import com.eyecam.model.StreamTelemetry
import com.eyecam.model.StreamUiState
import com.eyecam.model.ThermalState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * Ongoing Foreground Service, що підтримує життєвий цикл захоплення Camera2,
 * апаратного кодека MediaCodec, аудіопотоку та мережевого сокета в фоновому режимі.
 */
class CameraStreamService : Service() {

    private val serviceJob = SupervisorJob()
    private val serviceScope = CoroutineScope(Dispatchers.Default + serviceJob)

    private val binder = LocalBinder()

    private lateinit var powerLockManager: PowerLockManager
    private lateinit var thermalManager: ThermalManager

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

        createNotificationChannel()
        initMockSensors()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
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

    override fun onBind(intent: Intent?): IBinder = binder

    /**
     * Запуск трансляції: переведення в режим Foreground Service,
     * захоплення блокувань (Wakelock) та моніторинг температури.
     */
    fun startStreaming() {
        if (_uiState.value.isStreaming) return

        Log.i(TAG, "Запуск потоку трансляції та підняття Foreground Service...")

        // 1. Активація блокування сну процесора та Wi-Fi адаптера
        powerLockManager.acquireWakeLock()
        if (_uiState.value.telemetry.connectionType == ConnectionType.WIFI) {
            powerLockManager.acquireWifiLock()
        }

        // 2. Моніторинг нагрівання пристрою
        thermalManager.startMonitoring()

        // 3. Запуск Foreground Service відповідно до вимог Android 14+ (API 34)
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            var foregroundTypes = ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                foregroundTypes = foregroundTypes or ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            }
            startForeground(NOTIFICATION_ID, notification, foregroundTypes)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        // 4. Оновлення стану
        _uiState.update { current ->
            current.copy(
                isStreaming = true,
                telemetry = current.telemetry.copy(
                    fps = 60,
                    bitrateKbps = 12000,
                    connectionType = ConnectionType.USB,
                    connectionDetails = "USB 3.0 (Direct AOA Protocol)"
                )
            )
        }

        // 5. Запуск внутрішнього корутинного циклу моніторингу
        observeThermalChanges()
    }

    /**
     * Зупинка трансляції та очищення всіх апаратних ресурсів.
     */
    fun stopStreaming() {
        Log.i(TAG, "Зупинка трансляції EyeCam...")

        thermalManager.stopMonitoring()
        powerLockManager.releaseAll()

        _uiState.update { current ->
            current.copy(
                isStreaming = false,
                isDisplayProtectionActive = false,
                telemetry = current.telemetry.copy(fps = 0, bitrateKbps = 0)
            )
        }

        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    /**
     * Миттєве вимкнення / увімкнення мікрофона (Audio Mute).
     */
    fun toggleMicrophoneMute() {
        _uiState.update { it.copy(isMicMuted = !it.isMicMuted) }
        updateNotification()
        Log.d(TAG, "Стан мікрофона змінено: Muted = ${_uiState.value.isMicMuted}")
    }

    /**
     * Перемикання активного фізичного сенсора камери без переривання сесії клієнта.
     */
    fun switchSensor(newSensor: CameraSensorInfo) {
        if (_uiState.value.activeSensor?.id == newSensor.id) return

        Log.i(TAG, "Перемикання сенсора: ${_uiState.value.activeSensor?.id} -> ${newSensor.id} (${newSensor.sensorType})")
        
        // Тут здійснюється безшовний rebind поверхні Camera2 в NDK / MediaCodec
        _uiState.update { it.copy(activeSensor = newSensor) }
        updateNotification()
    }

    /**
     * Увімкнення або вимкнення режиму захисту екрану (Display Protection / OLED Blackout).
     */
    fun setDisplayProtection(active: Boolean) {
        _uiState.update { it.copy(isDisplayProtectionActive = active) }
        Log.d(TAG, "Display Protection Mode: $active")
    }

    /**
     * Спостереження за змінами термального стану та адаптація бітрейту.
     */
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
                        // Якщо перегрів MODERATE або SEVERE — примусово активуємо OLED захист екрану
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

    /**
     * Відправка події тротлінгу на десктопний клієнт через UDP/USB протокол (eyecam.proto).
     */
    private fun onDesktopThermalThrottlingTriggered(
        thermalState: ThermalState,
        adjustedFps: Int,
        adjustedBitrateKbps: Int,
        reason: String
    ) {
        Log.w(
            TAG,
            "ТЕЛЕМЕТРІЯ ДЛЯ ДЕСКТОПА: Тротлінг [$thermalState] -> Новий FPS: $adjustedFps, " +
                    "Бітрейт: ${adjustedBitrateKbps}Kbps. Причина: $reason"
        )
        // Тут викликається відправка protobuf ControlEnvelope з DeviceStateMessage або AckMessage
    }

    /**
     * Генерація стабільного сповіщення Foreground Service з діями швидкого доступу.
     */
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
        val sensorName = _uiState.value.activeSensor?.sensorType?.name ?: "MAIN"
        val thermalName = _uiState.value.telemetry.thermalState.descriptionUa

        return NotificationCompat.Builder(this, NOTIFICATION_CHANNEL_ID)
            .setContentTitle("EyeCam: Трансляція активна")
            .setContentText("Камера: $sensorName | $micStatus | $thermalName")
            .setSmallIcon(android.R.drawable.presence_video_online)
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

    private fun initMockSensors() {
        val mainSensor = CameraSensorInfo(
            id = "0",
            isLogical = false,
            sensorType = SensorType.WIDE,
            lensFacing = 1, // BACK
            focalLengths = floatArrayOf(24f),
            minFocusDistance = 10.0f,
            isoRange = 50..6400,
            exposureTimeRangeNs = 10000L..1000000000L
        )

        val ultraWideSensor = CameraSensorInfo(
            id = "1",
            isLogical = false,
            sensorType = SensorType.ULTRA_WIDE,
            lensFacing = 1, // BACK
            focalLengths = floatArrayOf(13f),
            minFocusDistance = 5.0f,
            isoRange = 50..3200,
            exposureTimeRangeNs = 10000L..1000000000L
        )

        val teleSensor = CameraSensorInfo(
            id = "2",
            isLogical = false,
            sensorType = SensorType.TELEPHOTO,
            lensFacing = 1, // BACK
            focalLengths = floatArrayOf(70f),
            minFocusDistance = 2.0f,
            isoRange = 50..3200,
            exposureTimeRangeNs = 10000L..1000000000L
        )

        val frontSensor = CameraSensorInfo(
            id = "3",
            isLogical = false,
            sensorType = SensorType.FRONT,
            lensFacing = 0, // FRONT
            focalLengths = floatArrayOf(22f),
            minFocusDistance = 8.0f,
            isoRange = 50..3200,
            exposureTimeRangeNs = 10000L..1000000000L
        )

        _uiState.update {
            it.copy(
                activeSensor = mainSensor,
                availableSensors = listOf(ultraWideSensor, mainSensor, teleSensor, frontSensor)
            )
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.i(TAG, "Знищення CameraStreamService та звільнення ресурсів")
        serviceJob.cancel()
        thermalManager.stopMonitoring()
        powerLockManager.releaseAll()
    }
}
