package com.eyecam.viewmodel

import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.StreamUiState
import com.eyecam.service.CameraStreamService
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * ViewModel для зв'язування Jetpack Compose UI з життєвим циклом CameraStreamService,
 * відліку таймера неактивності (10 секунд для OLED захисту) та обробки користувацьких подій.
 */
class StreamViewModel(application: Application) : AndroidViewModel(application) {

    private val context = application.applicationContext

    private val _uiState = MutableStateFlow(StreamUiState())
    val uiState: StateFlow<StreamUiState> = _uiState.asStateFlow()

    private var streamService: CameraStreamService? = null
    private var isBound = false

    private var inactivityTimerJob: Job? = null
    private val INACTIVITY_TIMEOUT_MS = 10_000L // 10 секунд до переходу в Display Protection Mode

    companion object {
        private const val TAG = "StreamViewModel"
    }

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val localBinder = binder as? CameraStreamService.LocalBinder
            streamService = localBinder?.getService()
            isBound = true
            Log.i(TAG, "Успішно підключено до CameraStreamService")

            // Підписка на стан сервісу
            viewModelScope.launch {
                streamService?.uiState?.collect { serviceState ->
                    _uiState.value = serviceState
                }
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            streamService = null
            isBound = false
            Log.w(TAG, "З'єднання з CameraStreamService розірвано")
        }
    }

    init {
        bindService()
    }

    private fun bindService() {
        val intent = Intent(context, CameraStreamService::class.java)
        context.bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)
    }

    /**
     * Старт / Стоп відеотрансляції.
     */
    fun toggleStreaming() {
        val intent = Intent(context, CameraStreamService::class.java)
        if (_uiState.value.isStreaming) {
            intent.action = CameraStreamService.ACTION_STOP_STREAM
            context.startService(intent)
            cancelInactivityTimer()
        } else {
            intent.action = CameraStreamService.ACTION_START_STREAM
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
            restartInactivityTimer()
        }
    }

    /**
     * Перемикання сенсора (Основний, Ультраширик, Телевік, Фронтальний).
     */
    fun switchSensor(sensor: CameraSensorInfo) {
        onUserInteraction()
        streamService?.switchSensor(sensor)
    }

    /**
     * Mute / Unmute аудіопотоку.
     */
    fun toggleMicMute() {
        onUserInteraction()
        streamService?.toggleMicrophoneMute()
    }

    /**
     * Обробка будь-якого дотику користувача до екрану.
     * Скидає 10-секундний таймер до затемнення дисплея.
     */
    fun onUserInteraction() {
        if (_uiState.value.isStreaming && !_uiState.value.isDisplayProtectionActive) {
            restartInactivityTimer()
        }
    }

    /**
     * Примусова або автоматична активація режиму захисту OLED дисплея.
     */
    fun activateDisplayProtection() {
        streamService?.setDisplayProtection(true)
        cancelInactivityTimer()
    }

    /**
     * Вихід із режиму захисту екрана за подвійним тапом.
     */
    fun wakeFromDisplayProtection() {
        streamService?.setDisplayProtection(false)
        restartInactivityTimer()
    }

    private fun restartInactivityTimer() {
        inactivityTimerJob?.cancel()
        if (!_uiState.value.isStreaming) return

        inactivityTimerJob = viewModelScope.launch {
            delay(INACTIVITY_TIMEOUT_MS)
            if (_uiState.value.isStreaming) {
                Log.d(TAG, "10 секунд неактивності: автоматичний перехід в Display Protection Mode")
                activateDisplayProtection()
            }
        }
    }

    private fun cancelInactivityTimer() {
        inactivityTimerJob?.cancel()
        inactivityTimerJob = null
    }

    override fun onCleared() {
        super.onCleared()
        cancelInactivityTimer()
        if (isBound) {
            try {
                context.unbindService(serviceConnection)
            } catch (e: Exception) {
                Log.e(TAG, "Помилка unbindService: ${e.message}")
            }
            isBound = false
        }
    }
}
