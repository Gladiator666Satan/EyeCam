package com.eyecam.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import com.eyecam.model.StreamUiState
import com.eyecam.ui.components.CameraPreview
import com.eyecam.ui.components.QuickToggleHud
import com.eyecam.viewmodel.StreamViewModel

@Composable
fun EyeCamScreen(
    uiState: StreamUiState,
    viewModel: StreamViewModel,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .fillMaxSize()
            .background(Color.Black)
    ) {
        // Camera Preview taking the full screen / aspect
        CameraPreview(
            viewModel = viewModel,
            uiState = uiState,
            modifier = Modifier.fillMaxSize()
        )

        // Overlay Quick Toggle HUD on top of preview
        QuickToggleHud(
            uiState = uiState,
            onToggleStream = { viewModel.toggleStreaming() },
            onToggleTorch = { viewModel.toggleTorch() },
            onToggleMic = { viewModel.toggleMicrophoneMute() },
            onSwitchCamera = { viewModel.switchCamera() },
            onSelectZoom = { zoom -> viewModel.setZoomRatio(zoom) },
            onRefocus = { viewModel.triggerAutoFocus() },
            onToggleOledProtection = { viewModel.toggleDisplayProtection() },
            modifier = Modifier.align(Alignment.BottomCenter)
        )
    }
}
