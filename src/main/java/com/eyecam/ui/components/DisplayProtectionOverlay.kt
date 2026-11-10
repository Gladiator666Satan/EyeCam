package com.eyecam.ui.components

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.eyecam.model.StreamUiState
import com.eyecam.ui.theme.OledPureBlack
import com.eyecam.ui.theme.StatusEmerald
import com.eyecam.ui.theme.TextDimmed
import kotlinx.coroutines.delay
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.random.Random

/**
 * Display Protection Mode (OLED Blackout):
 * Повний чорний екран (#000000) для OLED-матриць.
 *
 * Переваги для стійкості системи:
 * 1. Зниження температури: дисплей не виділяє тепло в корпус пристрою.
 * 2. Економія батареї: на AMOLED чорні пікселі вимкнені (0 мА струму).
 * 3. Запобігання вигоранню (Anti Burn-in): увімкнено плаваючий Pixel Shifting (зміщення координат елементів кожні 30 с).
 * 4. Фонова робота: Camera2 сесія та MediaCodec енкодер працюють на повній швидкості без переривань.
 */
@Composable
fun DisplayProtectionOverlay(
    uiState: StreamUiState,
    onWake: () -> Unit,
    modifier: Modifier = Modifier
) {
    // Час та таймер оновлення
    var currentTimeString by remember { mutableStateOf("") }
    val timeFormatter = remember { SimpleDateFormat("HH:mm:ss", Locale.getDefault()) }

    // Pixel Shifting: кожні 30 секунд трохи змінюємо зміщення (x, y) для захисту діодів OLED
    var pixelShiftX by remember { mutableIntStateOf(0) }
    var pixelShiftY by remember { mutableIntStateOf(0) }

    LaunchedEffect(Unit) {
        while (true) {
            currentTimeString = timeFormatter.format(Date())
            delay(1000L)
        }
    }

    LaunchedEffect(Unit) {
        while (true) {
            delay(30_000L) // Зсув пікселів кожні 30 секунд
            pixelShiftX = Random.nextInt(-20, 21)
            pixelShiftY = Random.nextInt(-20, 21)
        }
    }

    AnimatedVisibility(
        visible = uiState.isDisplayProtectionActive,
        enter = fadeIn(),
        exit = fadeOut(),
        modifier = modifier
    ) {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(OledPureBlack)
                // Подвійний тап для виходу з режиму захисту
                .pointerInput(Unit) {
                    detectTapGestures(
                        onDoubleTap = { onWake() },
                        onTap = {
                            // Одинарний тап нічого не ламає, запобігаючи випадковим торканням
                        }
                    )
                },
            contentAlignment = Alignment.Center
        ) {
            // Мінімалістичний плаваючий індикатор із Pixel Shifting
            Column(
                modifier = Modifier
                    .offset { IntOffset(pixelShiftX, pixelShiftY) }
                    .padding(24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.Videocam,
                        contentDescription = "Active Stream",
                        tint = StatusEmerald.copy(alpha = 0.5f),
                        modifier = Modifier.size(16.dp)
                    )
                    Text(
                        text = "EyeCam Active • ${uiState.telemetry.fps} FPS",
                        color = Color(0xFF444444),
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace
                    )
                }

                Text(
                    text = currentTimeString,
                    color = Color(0xFF2C2C2C),
                    fontSize = 28.sp,
                    fontWeight = FontWeight.Light,
                    fontFamily = FontFamily.Monospace
                )

                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                    modifier = Modifier.padding(top = 16.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.Lock,
                        contentDescription = null,
                        tint = Color(0xFF222222),
                        modifier = Modifier.size(12.dp)
                    )
                    Text(
                        text = "Двічі торкніться екрана для виходу",
                        color = Color(0xFF2C2C2C),
                        fontSize = 11.sp
                    )
                }
            }
        }
    }
}
