package com.eyecam.viewmodel

import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.IBinder
import android.util.Log
import android.view.Surface
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
import kotlinx.coroutines.launch

/**
 * ViewModel для зв'язування Jetpack Compose UI з життєвим циклом CameraStreamService,
 * керування прев'ю камери, зумом (до 10x) та відліку таймера OLED-захисту.
 */
class StreamViewModel(application: Application) : AndroidViewModel(application) {

    private val context = application.applicationContext

    private val _uiState = MutableStateFlow(StreamUiState())
    val uiState: StateFlow<StreamUiState> = _uiState.asStateFlow()

    private var streamService: CameraStreamService? = null
    private var isBound = false
    private var pendingPreviewSurface: Surface? = null
    private var pendingPreviewWidth: Int = 0
    private var pendingPreviewHeight: Int = 0
    private var pendingDisplayRotation: Int = Surface.ROTATION_0

    private var inactivityTimerJob: Job? = null
    private val INACTIVITY_TIMEOUT_MS = 15_000L

    companion object {
        private const val TAG = "StreamViewModel"
    }

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val localBinder = binder as? CameraStreamService.LocalBinder
            streamService = localBinder?.getService()
            isBound = true
            Log.i(TAG, "Успішно підключено до CameraStreamService")

            streamService?.refreshHardwareCapabilities()
            pendingPreviewSurface?.let { surface ->
                streamService?.attachPreviewSurface(
                    surface,
                    pendingPreviewWidth,
                    pendingPreviewHeight,
                    pendingDisplayRotation
                )
            }

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
        try {
            context.startService(intent)
        } catch (_: Exception) {}
        context.bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)
    }

    fun onPermissionsGranted() {
        streamService?.refreshHardwareCapabilities()
        pendingPreviewSurface?.let { surface ->
            streamService?.attachPreviewSurface(
                surface,
                pendingPreviewWidth,
                pendingPreviewHeight,
                pendingDisplayRotation
            )
        }
    }

    fun attachPreviewSurface(
        surface: Surface?,
        width: Int = 0,
        height: Int = 0,
        displayRotation: Int = Surface.ROTATION_0
    ) {
        pendingPreviewSurface = surface
        if (width > 0 && height > 0) {
            pendingPreviewWidth = width
            pendingPreviewHeight = height
        }
        pendingDisplayRotation = displayRotation
        streamService?.attachPreviewSurface(surface, width, height, displayRotation)
    }

    /**
     * Старт / Стоп відеотрансляції.
     */
    fun toggleStreaming(onPermissionMissing: () -> Unit = {}) {
        val service = streamService
        if (service != null && !service.cameraController.hasCameraPermission()) {
            onPermissionMissing()
            return
        }

        val intent = Intent(context, CameraStreamService::class.java)
        if (_uiState.value.isStreaming) {
            intent.action = CameraStreamService.ACTION_STOP_STREAM
            context.startService(intent)
            cancelInactivityTimer()
        } else {
            intent.action = CameraStreamService.ACTION_START_STREAM
            try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    context.startForegroundService(intent)
                } else {
                    context.startService(intent)
                }
            } catch (e: Exception) {
                Log.e(TAG, "Fallback startStreaming directly on bound service: ${e.message}")
                streamService?.startStreaming()
            }
            restartInactivityTimer()
        }
    }

    fun setZoomRatio(zoom: Float) {
        onUserInteraction()
        streamService?.setZoomRatio(zoom)
    }

    fun selectLensPreset(zoom: Float) {
        onUserInteraction()
        streamService?.selectLensPreset(zoom)
    }

    fun setFrontCamera(useFront: Boolean) {
        onUserInteraction()
        streamService?.setFrontCamera(useFront)
    }

    fun switchSensor(sensor: CameraSensorInfo) {
        onUserInteraction()
        streamService?.switchSensor(sensor)
    }

    fun toggleMicMute() {
        onUserInteraction()
        streamService?.toggleMicrophoneMute()
    }

    fun triggerAutoFocus() {
        onUserInteraction()
        streamService?.triggerAutoFocus()
    }

    fun triggerFocusAtPoint(xNorm: Float, yNorm: Float) {
        onUserInteraction()
        streamService?.triggerFocusAtPoint(xNorm, yNorm)
    }

    fun onUserInteraction() {
        if (_uiState.value.isStreaming && !_uiState.value.isDisplayProtectionActive) {
            restartInactivityTimer()
        }
    }

    fun activateDisplayProtection() {
        streamService?.setDisplayProtection(true)
        cancelInactivityTimer()
    }

    fun wakeFromDisplayProtection() {
        streamService?.setDisplayProtection(false)
        restartInactivityTimer()
    }

    fun deactivateDisplayProtection() = wakeFromDisplayProtection()

    private fun restartInactivityTimer() {
        inactivityTimerJob?.cancel()
        if (!_uiState.value.isStreaming) return

        inactivityTimerJob = viewModelScope.launch {
            delay(INACTIVITY_TIMEOUT_MS)
            if (_uiState.value.isStreaming) {
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
