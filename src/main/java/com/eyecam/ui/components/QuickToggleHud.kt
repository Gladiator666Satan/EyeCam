package com.eyecam.ui.components

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Thermostat
import androidx.compose.material.icons.filled.Usb
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.eyecam.model.CameraSensorInfo
import com.eyecam.model.ConnectionType
import com.eyecam.model.SensorType
import com.eyecam.model.StreamUiState
import com.eyecam.model.ThermalState
import com.eyecam.ui.theme.OledBorder
import com.eyecam.ui.theme.OledCard
import com.eyecam.ui.theme.OledPillBackground
import com.eyecam.ui.theme.StatusAmber
import com.eyecam.ui.theme.StatusCrimson
import com.eyecam.ui.theme.StatusCyan
import com.eyecam.ui.theme.StatusEmerald
import com.eyecam.ui.theme.TextDimmed
import com.eyecam.ui.theme.TextPrimary
import com.eyecam.ui.theme.TextSecondary

/**
 * Мінімалістичний плаваючий інтерфейс (HUD) швидкого керування трансляцією.
 * Оптимізований для OLED дисплеїв: темний фон, відсутність яскравих фонових площин,
 * збереження ресурсів акумулятора та захист від вигорання субпікселів.
 */
@Composable
fun QuickToggleHud(
    uiState: StreamUiState,
    onToggleStream: () -> Unit,
    onToggleMicMute: () -> Unit,
    onSwitchSensor: (CameraSensorInfo) -> Unit,
    onActivateDisplayProtection: () -> Unit,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {
        // ВЕРХНІЙ РЯДОК СТАТУСУ (Підключення, Температура, Батарея)
        TopStatusBar(
            uiState = uiState,
            modifier = Modifier.align(Alignment.TopCenter)
        )

        // БОКОВИЙ / ЦЕНТРАЛЬНИЙ СЕЛЕКТОР ОБ'ЄКТИВІВ (0.5x, 1x, 3x, FRONT)
        SensorSelectorBar(
            sensors = uiState.availableSensors,
            activeSensor = uiState.activeSensor,
            onSelect = onSwitchSensor,
            modifier = Modifier
                .align(Alignment.CenterEnd)
                .padding(end = 8.dp)
        )

        // НИЖНІЙ БЛОК: Телеметрія + Плаваючі кнопки дій
        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth(),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            // Телеметрія (FPS, Bitrate, Роздільна здатність)
            StreamTelemetryBar(uiState = uiState)

            // Панель швидких перемикачів (Mute, Stream, Sleep)
            FloatingActionControls(
                isStreaming = uiState.isStreaming,
                isMicMuted = uiState.isMicMuted,
                onToggleStream = onToggleStream,
                onToggleMicMute = onToggleMicMute,
                onActivateDisplayProtection = onActivateDisplayProtection
            )
        }
    }
}

/**
 * Верхній бар статусу підключення та термального стану.
 */
@Composable
private fun TopStatusBar(
    uiState: StreamUiState,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(top = 16.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        // Індикатор типу з'єднання (USB / Wi-Fi)
        ConnectionPill(
            type = uiState.telemetry.connectionType,
            details = uiState.telemetry.connectionDetails
        )

        // Індикатор термального стану (Thermal Throttling)
        ThermalStatusPill(thermalState = uiState.telemetry.thermalState)
    }
}

@Composable
private fun ConnectionPill(
    type: ConnectionType,
    details: String
) {
    val (icon, color, label) = when (type) {
        ConnectionType.USB -> Triple(Icons.Default.Usb, StatusEmerald, "USB НАПРЯМУ")
        ConnectionType.WIFI -> Triple(Icons.Default.Wifi, StatusCyan, "WI-FI LAN")
        ConnectionType.NONE -> Triple(Icons.Default.Usb, TextDimmed, "ОФЛАЙН")
    }

    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(20.dp))
            .background(OledPillBackground)
            .border(1.dp, OledBorder, RoundedCornerShape(20.dp))
            .padding(horizontal = 12.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Icon(
            imageVector = icon,
            contentDescription = label,
            tint = color,
            modifier = Modifier.size(16.dp)
        )
        Text(
            text = label,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold,
            color = color,
            fontFamily = FontFamily.Monospace
        )
    }
}

@Composable
private fun ThermalStatusPill(
    thermalState: ThermalState
) {
    val (color, label) = when (thermalState) {
        ThermalState.NORMAL -> StatusEmerald to "ТЕМП: ОК"
        ThermalState.LIGHT -> StatusEmerald to "ЛЕГКИЙ НАГРІВ"
        ThermalState.MODERATE -> StatusAmber to "ТРОТЛІНГ (45 FPS)"
        ThermalState.SEVERE -> StatusCrimson to "КРИТИЧНО (30 FPS)"
        ThermalState.CRITICAL -> StatusCrimson to "ПЕРЕГРІВ!"
        ThermalState.UNKNOWN -> TextDimmed to "ТЕМП: N/A"
    }

    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(20.dp))
            .background(OledPillBackground)
            .border(1.dp, if (thermalState == ThermalState.SEVERE) StatusCrimson else OledBorder, RoundedCornerShape(20.dp))
            .padding(horizontal = 12.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Icon(
            imageVector = Icons.Default.Thermostat,
            contentDescription = "Thermal Status",
            tint = color,
            modifier = Modifier.size(16.dp)
        )
        Text(
            text = label,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold,
            color = color,
            fontFamily = FontFamily.Monospace
        )
    }
}

/**
 * Вертикальний перемикач активних об'єктивів (0.5x, 1x, 3x, FRONT).
 */
@Composable
private fun SensorSelectorBar(
    sensors: List<CameraSensorInfo>,
    activeSensor: CameraSensorInfo?,
    onSelect: (CameraSensorInfo) -> Unit,
    modifier: Modifier = Modifier
) {
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(28.dp))
            .background(OledPillBackground)
            .border(1.dp, OledBorder, RoundedCornerShape(28.dp))
            .padding(vertical = 8.dp, horizontal = 4.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        sensors.forEach { sensor ->
            val isSelected = activeSensor?.id == sensor.id
            val label = when (sensor.sensorType) {
                SensorType.ULTRA_WIDE -> "0.5x"
                SensorType.WIDE -> "1.0x"
                SensorType.TELEPHOTO -> "3.0x"
                SensorType.FRONT -> "SELF"
                else -> sensor.id
            }

            Box(
                modifier = Modifier
                    .size(44.dp)
                    .clip(CircleShape)
                    .background(if (isSelected) StatusEmerald else Color.Transparent)
                    .clickable { onSelect(sensor) },
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = label,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    color = if (isSelected) Color.Black else TextSecondary,
                    fontFamily = FontFamily.Monospace
                )
            }
        }
    }
}

/**
 * Рядок з реальними показниками бітрейту, FPS та роздільної здатності.
 */
@Composable
private fun StreamTelemetryBar(
    uiState: StreamUiState,
    modifier: Modifier = Modifier
) {
    AnimatedVisibility(
        visible = uiState.isStreaming,
        enter = fadeIn(),
        exit = fadeOut()
    ) {
        Row(
            modifier = modifier
                .clip(RoundedCornerShape(16.dp))
                .background(OledPillBackground)
                .border(1.dp, OledBorder, RoundedCornerShape(16.dp))
                .padding(horizontal = 16.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            TelemetryItem(
                label = "FPS",
                value = "${uiState.telemetry.fps}/${uiState.telemetry.targetFps}"
            )
            TelemetryItem(
                label = "БІТРЕЙТ",
                value = "${uiState.telemetry.bitrateKbps / 1000} Мбіт/с"
            )
            TelemetryItem(
                label = "ЯКІСТЬ",
                value = "${uiState.telemetry.resolutionWidth}x${uiState.telemetry.resolutionHeight}"
            )
        }
    }
}

@Composable
private fun TelemetryItem(label: String, value: String) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(text = label, fontSize = 9.sp, color = TextDimmed, fontWeight = FontWeight.SemiBold)
        Text(
            text = value,
            fontSize = 12.sp,
            color = TextPrimary,
            fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace
        )
    }
}

/**
 * Плаваючі кнопки дій у нижній частині екрана.
 */
@Composable
private fun FloatingActionControls(
    isStreaming: Boolean,
    isMicMuted: Boolean,
    onToggleStream: () -> Unit,
    onToggleMicMute: () -> Unit,
    onActivateDisplayProtection: () -> Unit
) {
    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(32.dp))
            .background(OledCard)
            .border(1.dp, OledBorder, RoundedCornerShape(32.dp))
            .padding(horizontal = 20.dp, vertical = 10.dp),
        horizontalArrangement = Arrangement.spacedBy(24.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        // Кнопка Mute / Unmute мікрофона
        IconButton(
            onClick = onToggleMicMute,
            modifier = Modifier
                .size(48.dp)
                .clip(CircleShape)
                .background(if (isMicMuted) StatusCrimson.copy(alpha = 0.2f) else OledBorder)
        ) {
            Icon(
                imageVector = if (isMicMuted) Icons.Default.MicOff else Icons.Default.Mic,
                contentDescription = if (isMicMuted) "Unmute" else "Mute",
                tint = if (isMicMuted) StatusCrimson else TextPrimary,
                modifier = Modifier.size(24.dp)
            )
        }

        // Головна кнопка Старт / Стоп стріму
        Box(
            modifier = Modifier
                .size(64.dp)
                .clip(CircleShape)
                .background(if (isStreaming) StatusCrimson else StatusEmerald)
                .clickable { onToggleStream() },
            contentAlignment = Alignment.Center
        ) {
            Icon(
                imageVector = if (isStreaming) Icons.Default.Stop else Icons.Default.PlayArrow,
                contentDescription = if (isStreaming) "Зупинити" else "Старт",
                tint = Color.Black,
                modifier = Modifier.size(32.dp)
            )
        }

        // Кнопка миттєвого блокування / Display Protection Mode
        IconButton(
            onClick = onActivateDisplayProtection,
            modifier = Modifier
                .size(48.dp)
                .clip(CircleShape)
                .background(OledBorder)
        ) {
            Icon(
                imageVector = Icons.Default.Lock,
                contentDescription = "OLED Blackout",
                tint = TextPrimary,
                modifier = Modifier.size(22.dp)
            )
        }
    }
}
