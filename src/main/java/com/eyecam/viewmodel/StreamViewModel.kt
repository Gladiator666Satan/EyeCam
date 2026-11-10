package com.eyecam.viewmodel

import android.view.Surface
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.eyecam.model.CameraMode
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.StreamUiState
import com.eyecam.service.CameraStreamService
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

class StreamViewModel : ViewModel() {

    private var streamService: CameraStreamService? = null

    private val _uiState = MutableStateFlow(StreamUiState())
    val uiState: StateFlow<StreamUiState> = _uiState.asStateFlow()

    fun bindService(service: CameraStreamService) {
        this.streamService = service
        viewModelScope.launch {
            service.uiState.collect { state ->
                _uiState.value = state
            }
        }
    }

    fun unbindService() {
        this.streamService = null
    }

    fun onCameraPermissionGranted() {
        _uiState.update { it.copy(hasCameraPermission = true) }
    }

    fun toggleStreaming() {
        val s = streamService ?: return
        if (_uiState.value.isStreaming) {
            s.stopStreaming()
        } else {
            s.startStreaming()
        }
    }

    fun toggleTorch() {
        val s = streamService ?: return
        val newState = !_uiState.value.isTorchOn
        s.toggleTorch(newState)
    }

    fun toggleMicrophoneMute() {
        val s = streamService ?: return
        val newState = !_uiState.value.isMicMuted
        s.toggleMicrophoneMute(newState)
    }

    fun switchCamera() {
        val s = streamService ?: return
        val nextIsFront = !_uiState.value.isFrontCamera
        s.setFrontCamera(nextIsFront)
    }

    fun setZoomRatio(ratio: Float) {
        val s = streamService ?: return
        s.setZoomRatio(ratio)
    }

    fun triggerAutoFocus() {
        streamService?.triggerAutoFocus()
    }

    fun triggerFocusAtPoint(xNorm: Float, yNorm: Float) {
        streamService?.triggerFocusAtPoint(xNorm, yNorm)
    }

    fun attachPreviewSurface(surface: Surface?) {
        streamService?.attachPreviewSurface(surface)
    }

    fun toggleDisplayProtection() {
        _uiState.update { it.copy(isDisplayProtectionActive = !it.isDisplayProtectionActive) }
    }

    fun setManualMode(mode: CameraMode) {
        streamService?.setManualMode(mode)
    }
}
