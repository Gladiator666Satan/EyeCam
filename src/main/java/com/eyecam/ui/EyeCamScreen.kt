package com.eyecam.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import com.eyecam.ui.components.CameraPreview
import com.eyecam.ui.components.DisplayProtectionOverlay
import com.eyecam.ui.components.QuickToggleHud
import com.eyecam.ui.theme.OledPureBlack
import com.eyecam.viewmodel.StreamViewModel

/**
 * Головний екран програми EyeCam, що поєднує живий попередній перегляд камери,
 * плаваючий HUD з перемикачами зуму (до 10x) та оверлей захисту OLED екрана.
 */
@Composable
fun EyeCamScreen(
    viewModel: StreamViewModel,
    onRequestPermissions: () -> Unit = {},
    modifier: Modifier = Modifier
) {
    val uiState by viewModel.uiState.collectAsState()

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(OledPureBlack)
            .pointerInput(uiState.isDisplayProtectionActive) {
                if (!uiState.isDisplayProtectionActive) {
                    detectTapGestures(
                        onPress = {
                            viewModel.onUserInteraction()
                        }
                    )
                }
            }
    ) {
        // Шар 1: Живе прев'ю камери з підключенням SurfaceHolder до CameraController
        CameraPreview(
            isStreaming = uiState.isStreaming,
            hasCameraPermission = uiState.hasCameraPermission,
            isDisplayProtectionActive = uiState.isDisplayProtectionActive,
            streamWidth = uiState.telemetry.resolutionWidth,
            streamHeight = uiState.telemetry.resolutionHeight,
            onSurfaceChanged = { surface -> viewModel.attachPreviewSurface(surface) },
            onFocusPointTapped = { x, y -> viewModel.triggerFocusAtPoint(x, y) },
            onRequestPermissions = onRequestPermissions
        )

        // Шар 2: Швидкі перемикачі HUD (видимі, коли захист екрану вимкнено)
        if (!uiState.isDisplayProtectionActive) {
            QuickToggleHud(
                uiState = uiState,
                onToggleStream = { viewModel.toggleStreaming(onPermissionMissing = onRequestPermissions) },
                onToggleMicMute = { viewModel.toggleMicMute() },
                onSelectZoom = { zoom ->
                    viewModel.setZoomRatio(zoom)
                },
                onSelectFrontCamera = { viewModel.setFrontCamera(true) },
                onTriggerAutoFocus = { viewModel.triggerAutoFocus() },
                onActivateDisplayProtection = { viewModel.activateDisplayProtection() }
            )
        }

        // Шар 3: Display Protection Mode оверлей (OLED Blackout з Pixel Shifting)
        DisplayProtectionOverlay(
            uiState = uiState,
            onWake = { viewModel.wakeFromDisplayProtection() }
        )
    }
}
