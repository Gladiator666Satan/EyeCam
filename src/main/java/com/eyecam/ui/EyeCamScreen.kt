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
 * Головний екран програми EyeCam, що поєднує попередній перегляд камери,
 * плаваючий HUD з перемикачами та оверлей захисту OLED екрана.
 */
@Composable
fun EyeCamScreen(
    viewModel: StreamViewModel,
    modifier: Modifier = Modifier
) {
    val uiState by viewModel.uiState.collectAsState()

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(OledPureBlack)
            // Будь-яка взаємодія користувача скидає таймер до авто-затемнення екрана
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
        // Шар 1: Прев'ю камери (автоматично відключається при активному захисті для збереження енергії)
        CameraPreview(
            isStreaming = uiState.isStreaming,
            isDisplayProtectionActive = uiState.isDisplayProtectionActive
        )

        // Шар 2: Швидкі перемикачі HUD (видимі, коли захист екрану вимкнено)
        if (!uiState.isDisplayProtectionActive) {
            QuickToggleHud(
                uiState = uiState,
                onToggleStream = { viewModel.toggleStreaming() },
                onToggleMicMute = { viewModel.toggleMicMute() },
                onSwitchSensor = { sensor -> viewModel.switchSensor(sensor) },
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
