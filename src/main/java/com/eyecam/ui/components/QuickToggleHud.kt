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
import androidx.compose.material.icons.filled.CenterFocusStrong
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.FlashOff
import androidx.compose.material.icons.filled.FlashOn
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.LockOpen
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.NightlightRound
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.SystemUpdateAlt
import androidx.compose.material.icons.filled.Thermostat
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.Usb
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.eyecam.model.CameraModuleInfo
import com.eyecam.model.ConnectionType
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
import kotlinx.coroutines.delay
import java.util.Locale
import kotlin.math.roundToInt

/**
 * Повнофункціональний плаваючий HUD EyeCam:
 * - Окремі кнопки фізичних модулів камери (активні лише для наявних у пристрої модулів)
 * - Плавний зум виключно всередині активного модуля (без авто-перестрибування)
 * - Візуальний відгук на кнопках Автофокусу та Авто-режиму
 * - Кнопка Ліхтарика (неактивна на фронтальній камері) та Кнопка Блокування Експозиції (AE Lock)
 * - Ручні повзунки (ISO, Shutter, Focus, WB) із замочками (🔒) для скидання окремого повзунка в AUTO
 * - Завантаження та застосування колірних профілів .cube 3D LUT
 * - Кнопки «Підтримати автора» та «GitHub / Оновлення»
 */
@Composable
fun QuickToggleHud(
    uiState: StreamUiState,
    onToggleStream: () -> Unit,
    onToggleMicMute: () -> Unit,
    onToggleTorch: () -> Unit,
    onToggleAeLock: () -> Unit,
    onSelectModule: (String) -> Unit,
    onSetIntraModuleZoom: (Float) -> Unit,
    onTriggerAutoFocus: () -> Unit,
    onResetAllToAuto: () -> Unit,
    onUpdateManualIso: (Int) -> Unit,
    onUpdateManualShutter: (Long) -> Unit,
    onUpdateManualFocus: (Float) -> Unit,
    onUpdateManualWb: (Int) -> Unit,
    onPickCubeLut: () -> Unit,
    onClearCubeLut: () -> Unit,
    onOpenSupportModal: () -> Unit,
    onOpenUpdateModal: () -> Unit,
    onActivateDisplayProtection: () -> Unit,
    modifier: Modifier = Modifier
) {
    var showProPanel by remember { mutableStateOf(false) }
    var afHighlightActive by remember { mutableStateOf(false) }
    var autoHighlightActive by remember { mutableStateOf(false) }

    LaunchedEffect(uiState.afTriggerPulseSeq) {
        if (uiState.afTriggerPulseSeq > 0L) {
            afHighlightActive = true
            delay(750L)
            afHighlightActive = false
        }
    }

    LaunchedEffect(uiState.autoModePulseSeq) {
        if (uiState.autoModePulseSeq > 0L) {
            autoHighlightActive = true
            delay(750L)
            autoHighlightActive = false
        }
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .padding(14.dp)
    ) {
        // 1. ВЕРХНІЙ РЯДОК: Підключення + Кнопки Підтримки / GitHub + Температура
        TopStatusBar(
            uiState = uiState,
            onOpenSupportModal = onOpenSupportModal,
            onOpenUpdateModal = onOpenUpdateModal,
            modifier = Modifier.align(Alignment.TopCenter)
        )

        // 2. БОКОВИЙ СЕЛЕКТОР МОДУЛІВ КАМЕР + ЗУМ ВСЕРЕДИНІ МОДУЛЯ
        ModuleAndIntraZoomColumn(
            availableModules = uiState.availableModules,
            activeModuleKey = uiState.activeModuleKey,
            intraZoom = uiState.intraModuleZoom,
            maxIntraZoom = uiState.maxIntraModuleZoom,
            onSelectModule = onSelectModule,
            onSetIntraZoom = onSetIntraModuleZoom,
            modifier = Modifier
                .align(Alignment.CenterEnd)
                .padding(end = 2.dp)
        )

        // 3. НИЖНІЙ БЛОК: Панель Pro ISP & LUT + Телеметрія + Кнопки дій
        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth(),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            AnimatedVisibility(
                visible = showProPanel,
                enter = fadeIn(),
                exit = fadeOut()
            ) {
                ProManualAndLutPanel(
                    uiState = uiState,
                    autoHighlightActive = autoHighlightActive,
                    onResetAllToAuto = onResetAllToAuto,
                    onToggleAeLock = onToggleAeLock,
                    onUpdateManualIso = onUpdateManualIso,
                    onUpdateManualShutter = onUpdateManualShutter,
                    onUpdateManualFocus = onUpdateManualFocus,
                    onUpdateManualWb = onUpdateManualWb,
                    onPickCubeLut = onPickCubeLut,
                    onClearCubeLut = onClearCubeLut
                )
            }

            StreamTelemetryBar(uiState = uiState)

            FloatingActionControls(
                isStreaming = uiState.isStreaming,
                isMicMuted = uiState.isMicMuted,
                isTorchOn = uiState.isTorchOn,
                isTorchAvailable = uiState.isTorchAvailable,
                isAeLocked = uiState.isAeLocked,
                isProOpen = showProPanel || uiState.cameraMode.hasAnyManualLock || uiState.activeLutName.isNotEmpty(),
                afHighlightActive = afHighlightActive,
                onToggleStream = onToggleStream,
                onToggleMicMute = onToggleMicMute,
                onToggleTorch = onToggleTorch,
                onToggleAeLock = onToggleAeLock,
                onTriggerAutoFocus = onTriggerAutoFocus,
                onToggleProPanel = { showProPanel = !showProPanel },
                onActivateDisplayProtection = onActivateDisplayProtection
            )
        }
    }
}

@Composable
private fun TopStatusBar(
    uiState: StreamUiState,
    onOpenSupportModal: () -> Unit,
    onOpenUpdateModal: () -> Unit,
    modifier: Modifier = Modifier
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .padding(top = 12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            ConnectionPill(
                type = uiState.telemetry.connectionType,
                details = uiState.telemetry.connectionDetails
            )

            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                // Кнопка підтримки автора (Patreon / BuyMeACoffee)
                Row(
                    modifier = Modifier
                        .clip(RoundedCornerShape(18.dp))
                        .background(OledPillBackground)
                        .border(1.dp, StatusAmber.copy(alpha = 0.65f), RoundedCornerShape(18.dp))
                        .clickable { onOpenSupportModal() }
                        .padding(horizontal = 10.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(5.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.Favorite,
                        contentDescription = "Підтримати автора",
                        tint = StatusAmber,
                        modifier = Modifier.size(14.dp)
                    )
                    Text(
                        text = "Підтримати",
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Bold,
                        color = StatusAmber
                    )
                }

                // Кнопка GitHub та перевірки оновлень
                Row(
                    modifier = Modifier
                        .clip(RoundedCornerShape(18.dp))
                        .background(OledPillBackground)
                        .border(1.dp, StatusCyan.copy(alpha = 0.65f), RoundedCornerShape(18.dp))
                        .clickable { onOpenUpdateModal() }
                        .padding(horizontal = 10.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(5.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.SystemUpdateAlt,
                        contentDescription = "GitHub та Оновлення",
                        tint = StatusCyan,
                        modifier = Modifier.size(14.dp)
                    )
                    Text(
                        text = "GitHub",
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Bold,
                        color = StatusCyan
                    )
                }

                ThermalStatusPill(thermalState = uiState.telemetry.thermalState)
            }
        }
    }
}

@Composable
private fun ConnectionPill(
    type: ConnectionType,
    details: String
) {
    val (icon, color, defaultLabel) = when (type) {
        ConnectionType.USB -> Triple(Icons.Default.Usb, StatusEmerald, "USB ПІДКЛЮЧЕНО")
        ConnectionType.WIFI -> Triple(Icons.Default.Wifi, StatusCyan, "WI-FI LAN")
        ConnectionType.NONE -> Triple(Icons.Default.Usb, TextDimmed, "ОФЛАЙН")
    }

    val displayText = if (details.isNotBlank() && details != "Очікування підключення...") {
        details
    } else {
        defaultLabel
    }

    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(20.dp))
            .background(OledPillBackground)
            .border(1.dp, if (type != ConnectionType.NONE) color.copy(alpha = 0.5f) else OledBorder, RoundedCornerShape(20.dp))
            .padding(horizontal = 10.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(5.dp)
    ) {
        Icon(
            imageVector = icon,
            contentDescription = displayText,
            tint = color,
            modifier = Modifier.size(15.dp)
        )
        Text(
            text = displayText,
            fontSize = 10.sp,
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
        ThermalState.NORMAL -> StatusEmerald to "OK"
        ThermalState.LIGHT -> StatusEmerald to "ТЕПЛО"
        ThermalState.MODERATE -> StatusAmber to "ТРОТЛІНГ"
        ThermalState.SEVERE -> StatusCrimson to "ГАРЯЧЕ"
        ThermalState.CRITICAL -> StatusCrimson to "КРИТИЧНО"
        ThermalState.UNKNOWN -> TextDimmed to "N/A"
    }

    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(20.dp))
            .background(OledPillBackground)
            .border(1.dp, if (thermalState == ThermalState.SEVERE) StatusCrimson else OledBorder, RoundedCornerShape(20.dp))
            .padding(horizontal = 8.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        Icon(
            imageVector = Icons.Default.Thermostat,
            contentDescription = "Thermal Status",
            tint = color,
            modifier = Modifier.size(14.dp)
        )
        Text(
            text = label,
            fontSize = 10.sp,
            fontWeight = FontWeight.Bold,
            color = color,
            fontFamily = FontFamily.Monospace
        )
    }
}

/**
 * Окремі кнопки фізичних модулів камери (активні лише при наявності модуля в смартфоні)
 * + кнопки плавного зуму всередині обраного модуля без авто-перемикання між лінзами.
 */
@Composable
private fun ModuleAndIntraZoomColumn(
    availableModules: List<CameraModuleInfo>,
    activeModuleKey: String,
    intraZoom: Float,
    maxIntraZoom: Float,
    onSelectModule: (String) -> Unit,
    onSetIntraZoom: (Float) -> Unit,
    modifier: Modifier = Modifier
) {
    data class ModuleSlot(val key: String, val fallbackShort: String)
    val slots = listOf(
        ModuleSlot("uw", "0.5x"),
        ModuleSlot("main", "1x"),
        ModuleSlot("tele", "Tele"),
        ModuleSlot(if (availableModules.any { it.key == "macro" }) "macro" else "tele5x", if (availableModules.any { it.key == "macro" }) "MC" else "5x"),
        ModuleSlot("front", "SELF")
    )

    Column(
        modifier = modifier,
        horizontalAlignment = Alignment.End,
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        // Ряд кнопок модулів
        Column(
            modifier = Modifier
                .clip(RoundedCornerShape(26.dp))
                .background(OledPillBackground)
                .border(1.dp, OledBorder, RoundedCornerShape(26.dp))
                .padding(vertical = 6.dp, horizontal = 4.dp),
            verticalArrangement = Arrangement.spacedBy(5.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                text = "МОДУЛІ",
                fontSize = 8.sp,
                fontWeight = FontWeight.Bold,
                color = TextDimmed,
                fontFamily = FontFamily.Monospace
            )
            slots.forEach { slot ->
                val mod = availableModules.find { it.key == slot.key }
                val enabled = mod != null
                val selected = enabled && activeModuleKey == slot.key
                val label = mod?.shortLabel ?: slot.fallbackShort
                val activeColor = if (slot.key == "front") StatusCyan else StatusEmerald

                Box(
                    modifier = Modifier
                        .size(40.dp)
                        .clip(CircleShape)
                        .background(
                            when {
                                selected -> activeColor
                                enabled -> Color(0xFF151C28)
                                else -> Color(0xFF0A0D12)
                            }
                        )
                        .border(
                            width = 1.dp,
                            color = when {
                                selected -> activeColor
                                enabled -> OledBorder
                                else -> Color(0xFF181E29)
                            },
                            shape = CircleShape
                        )
                        .clickable(enabled = enabled) {
                            onSelectModule(slot.key)
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = label,
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Bold,
                        color = when {
                            selected -> Color.Black
                            enabled -> TextPrimary
                            else -> TextDimmed.copy(alpha = 0.35f)
                        },
                        fontFamily = FontFamily.Monospace
                    )
                }
            }
        }

        // Керування зумом всередині обраного модуля (1.0x .. maxIntraZoom)
        Column(
            modifier = Modifier
                .clip(RoundedCornerShape(20.dp))
                .background(OledPillBackground)
                .border(1.dp, OledBorder, RoundedCornerShape(20.dp))
                .padding(vertical = 6.dp, horizontal = 6.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(4.dp)
        ) {
            Box(
                modifier = Modifier
                    .size(34.dp)
                    .clip(CircleShape)
                    .background(OledCard)
                    .clickable {
                        onSetIntraZoom((intraZoom + 0.5f).coerceAtMost(maxIntraZoom))
                    },
                contentAlignment = Alignment.Center
            ) {
                Text("+", color = StatusCyan, fontWeight = FontWeight.Bold, fontSize = 15.sp)
            }

            Text(
                text = String.format(Locale.US, "%.1fx", intraZoom),
                fontSize = 10.sp,
                fontWeight = FontWeight.Bold,
                color = StatusEmerald,
                fontFamily = FontFamily.Monospace
            )

            Box(
                modifier = Modifier
                    .size(34.dp)
                    .clip(CircleShape)
                    .background(OledCard)
                    .clickable {
                        onSetIntraZoom((intraZoom - 0.5f).coerceAtLeast(1.0f))
                    },
                contentAlignment = Alignment.Center
            ) {
                Text("−", color = StatusCyan, fontWeight = FontWeight.Bold, fontSize = 15.sp)
            }
        }
    }
}

/**
 * Розгорнута панель Pro ISP та .cube LUT:
 * Кожен повзунок за замовчуванням у режимі AUTO, поки користувач не почне його крутити —
 * тоді з'являється іконка замочка (🔒), натискання на яку повертає цей повзунок у AUTO.
 */
@Composable
private fun ProManualAndLutPanel(
    uiState: StreamUiState,
    autoHighlightActive: Boolean,
    onResetAllToAuto: () -> Unit,
    onToggleAeLock: () -> Unit,
    onUpdateManualIso: (Int) -> Unit,
    onUpdateManualShutter: (Long) -> Unit,
    onUpdateManualFocus: (Float) -> Unit,
    onUpdateManualWb: (Int) -> Unit,
    onPickCubeLut: () -> Unit,
    onClearCubeLut: () -> Unit
) {
    val mode = uiState.cameraMode
    var isoSlider by remember { mutableFloatStateOf(0.20f) }
    var shutterSlider by remember { mutableFloatStateOf(0.35f) }
    var focusSlider by remember { mutableFloatStateOf(0.25f) }
    var wbSlider by remember { mutableFloatStateOf(0.48f) }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(20.dp))
            .background(OledCard.copy(alpha = 0.96f))
            .border(1.dp, OledBorder, RoundedCornerShape(20.dp))
            .padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Кнопка скидання всього в AUTO із яскравим візуальним відгуком
            Box(
                modifier = Modifier
                    .clip(RoundedCornerShape(12.dp))
                    .background(
                        when {
                            autoHighlightActive -> StatusEmerald
                            !mode.hasAnyManualLock -> StatusEmerald.copy(alpha = 0.22f)
                            else -> OledPillBackground
                        }
                    )
                    .border(
                        1.dp,
                        if (autoHighlightActive || !mode.hasAnyManualLock) StatusEmerald else OledBorder,
                        RoundedCornerShape(12.dp)
                    )
                    .clickable { onResetAllToAuto() }
                    .padding(horizontal = 12.dp, vertical = 6.dp)
            ) {
                Text(
                    text = if (autoHighlightActive) "✨ AUTO АКТИВОВАНО!" else "✨ Скинути все в AUTO",
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                    color = if (autoHighlightActive) Color.Black else StatusEmerald
                )
            }

            // Кнопка фіксації поточної авто-експозиції (AE Lock)
            Row(
                modifier = Modifier
                    .clip(RoundedCornerShape(12.dp))
                    .background(if (uiState.isAeLocked) StatusAmber else OledPillBackground)
                    .border(1.dp, if (uiState.isAeLocked) StatusAmber else OledBorder, RoundedCornerShape(12.dp))
                    .clickable { onToggleAeLock() }
                    .padding(horizontal = 10.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Icon(
                    imageVector = if (uiState.isAeLocked) Icons.Default.Lock else Icons.Default.LockOpen,
                    contentDescription = "AE Lock",
                    tint = if (uiState.isAeLocked) Color.Black else StatusAmber,
                    modifier = Modifier.size(14.dp)
                )
                Text(
                    text = if (uiState.isAeLocked) "AE ЗАФІКСОВАНО" else "Фікс. Експозиції",
                    fontSize = 10.sp,
                    fontWeight = FontWeight.Bold,
                    color = if (uiState.isAeLocked) Color.Black else StatusAmber
                )
            }
        }

        // 1. ISO Slider + Lock
        val isoVal = 100 + (isoSlider * 3100f).roundToInt()
        LockableParameterSliderRow(
            title = if (mode.isIsoManual) "ISO: $isoVal" else "ISO: AUTO",
            value = isoSlider,
            isLocked = mode.isIsoManual,
            onValueChange = { v ->
                isoSlider = v
                val nextIso = 100 + (v * 3100f).roundToInt()
                onUpdateManualIso(nextIso)
            },
            onUnlockToAuto = { onUpdateManualIso(0) }
        )

        // 2. Shutter Slider + Lock
        val shutterDenom = 30 + (shutterSlider * 3970f).roundToInt()
        LockableParameterSliderRow(
            title = if (mode.isShutterManual) "Витримка: 1/${shutterDenom}s" else "Витримка: AUTO",
            value = shutterSlider,
            isLocked = mode.isShutterManual,
            onValueChange = { v ->
                shutterSlider = v
                val denom = 30 + (v * 3970f).roundToInt()
                val expNs = 1_000_000_000L / denom.toLong()
                onUpdateManualShutter(expNs)
            },
            onUnlockToAuto = { onUpdateManualShutter(0L) }
        )

        // 3. Focus Slider + Lock
        val focusDiopters = focusSlider * 10.0f
        LockableParameterSliderRow(
            title = if (mode.isFocusManual) String.format(Locale.US, "Фокус: %.1f dpt", focusDiopters) else "Фокус: AUTO",
            value = focusSlider,
            isLocked = mode.isFocusManual,
            onValueChange = { v ->
                focusSlider = v
                onUpdateManualFocus((v * 10.0f).coerceAtLeast(0.05f))
            },
            onUnlockToAuto = { onUpdateManualFocus(0f) }
        )

        // 4. White Balance Slider + Lock
        val wbKelvin = 2800 + (wbSlider * 5700f).roundToInt()
        LockableParameterSliderRow(
            title = if (mode.isWbManual) "ББ: ${wbKelvin}K" else "Баланс білого: AUTO",
            value = wbSlider,
            isLocked = mode.isWbManual,
            onValueChange = { v ->
                wbSlider = v
                val k = 2800 + (v * 5700f).roundToInt()
                onUpdateManualWb(k)
            },
            onUnlockToAuto = { onUpdateManualWb(0) }
        )

        // 5. .cube 3D LUT колірний профіль
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(12.dp))
                    .background(OledPillBackground)
                    .border(1.dp, if (uiState.activeLutName.isNotEmpty()) StatusCyan else OledBorder, RoundedCornerShape(12.dp))
                    .clickable { onPickCubeLut() }
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = if (uiState.activeLutName.isNotEmpty()) "🎨 LUT: ${uiState.activeLutName}" else "📂 Завантажити .cube 3D LUT",
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                    color = if (uiState.activeLutName.isNotEmpty()) StatusCyan else TextPrimary
                )
            }

            if (uiState.activeLutName.isNotEmpty()) {
                Spacer(modifier = Modifier.width(8.dp))
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(12.dp))
                        .background(StatusCrimson.copy(alpha = 0.22f))
                        .border(1.dp, StatusCrimson, RoundedCornerShape(12.dp))
                        .clickable { onClearCubeLut() }
                        .padding(horizontal = 12.dp, vertical = 8.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = "✕ Скинути",
                        fontSize = 11.sp,
                        fontWeight = FontWeight.Bold,
                        color = StatusCrimson
                    )
                }
            }
        }
    }
}

@Composable
private fun LockableParameterSliderRow(
    title: String,
    value: Float,
    isLocked: Boolean,
    onValueChange: (Float) -> Unit,
    onUnlockToAuto: () -> Unit
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Text(
            text = title,
            fontSize = 11.sp,
            fontWeight = FontWeight.SemiBold,
            color = if (isLocked) StatusCyan else TextSecondary,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(128.dp)
        )

        Slider(
            value = value,
            onValueChange = onValueChange,
            modifier = Modifier
                .weight(1f)
                .height(26.dp),
            colors = SliderDefaults.colors(
                thumbColor = if (isLocked) StatusCyan else TextSecondary,
                activeTrackColor = if (isLocked) StatusCyan else TextDimmed,
                inactiveTrackColor = OledBorder
            )
        )

        // Замочок з'являється, коли повзунок переведено в ручний режим; клік повертає його в AUTO
        if (isLocked) {
            Box(
                modifier = Modifier
                    .size(30.dp)
                    .clip(CircleShape)
                    .background(StatusAmber.copy(alpha = 0.22f))
                    .border(1.dp, StatusAmber, CircleShape)
                    .clickable { onUnlockToAuto() },
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    imageVector = Icons.Default.Lock,
                    contentDescription = "Скинути в AUTO",
                    tint = StatusAmber,
                    modifier = Modifier.size(15.dp)
                )
            }
        } else {
            Spacer(modifier = Modifier.size(30.dp))
        }
    }
}

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
                .padding(horizontal = 14.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(14.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            TelemetryItem(
                label = "FPS",
                value = "${uiState.telemetry.fps}/${uiState.telemetry.targetFps}"
            )
            TelemetryItem(
                label = "МОДУЛЬ",
                value = "${uiState.activeModuleKey.uppercase()} (${String.format(Locale.US, "%.1fx", uiState.intraModuleZoom)})"
            )
            TelemetryItem(
                label = "ФОРМАТ",
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
            fontSize = 11.sp,
            color = TextPrimary,
            fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace
        )
    }
}

@Composable
private fun FloatingActionControls(
    isStreaming: Boolean,
    isMicMuted: Boolean,
    isTorchOn: Boolean,
    isTorchAvailable: Boolean,
    isAeLocked: Boolean,
    isProOpen: Boolean,
    afHighlightActive: Boolean,
    onToggleStream: () -> Unit,
    onToggleMicMute: () -> Unit,
    onToggleTorch: () -> Unit,
    onToggleAeLock: () -> Unit,
    onTriggerAutoFocus: () -> Unit,
    onToggleProPanel: () -> Unit,
    onActivateDisplayProtection: () -> Unit
) {
    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(32.dp))
            .background(OledCard)
            .border(1.dp, OledBorder, RoundedCornerShape(32.dp))
            .padding(horizontal = 12.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        // 1. Мікрофон
        IconButton(
            onClick = onToggleMicMute,
            modifier = Modifier
                .size(42.dp)
                .clip(CircleShape)
                .background(if (isMicMuted) StatusCrimson.copy(alpha = 0.2f) else OledBorder)
        ) {
            Icon(
                imageVector = if (isMicMuted) Icons.Default.MicOff else Icons.Default.Mic,
                contentDescription = if (isMicMuted) "Unmute" else "Mute",
                tint = if (isMicMuted) StatusCrimson else TextPrimary,
                modifier = Modifier.size(20.dp)
            )
        }

        // 2. Ліхтарик (недійсний при фронтальній камері)
        IconButton(
            onClick = { if (isTorchAvailable) onToggleTorch() },
            enabled = isTorchAvailable,
            modifier = Modifier
                .size(42.dp)
                .clip(CircleShape)
                .background(
                    when {
                        !isTorchAvailable -> Color(0xFF0D1118)
                        isTorchOn -> StatusAmber
                        else -> OledBorder
                    }
                )
        ) {
            Icon(
                imageVector = if (isTorchOn && isTorchAvailable) Icons.Default.FlashOn else Icons.Default.FlashOff,
                contentDescription = "Ліхтарик",
                tint = when {
                    !isTorchAvailable -> TextDimmed.copy(alpha = 0.35f)
                    isTorchOn -> Color.Black
                    else -> TextPrimary
                },
                modifier = Modifier.size(20.dp)
            )
        }

        // 3. Автофокус із чітким візуальним відгуком при натисканні
        IconButton(
            onClick = onTriggerAutoFocus,
            modifier = Modifier
                .size(42.dp)
                .clip(CircleShape)
                .background(if (afHighlightActive) StatusCyan else OledBorder)
                .border(
                    width = if (afHighlightActive) 2.dp else 0.dp,
                    color = if (afHighlightActive) StatusEmerald else Color.Transparent,
                    shape = CircleShape
                )
        ) {
            Icon(
                imageVector = Icons.Default.CenterFocusStrong,
                contentDescription = "Перефокусувати",
                tint = if (afHighlightActive) Color.Black else StatusCyan,
                modifier = Modifier.size(22.dp)
            )
        }

        // 4. Головна кнопка Старт / Стоп
        Box(
            modifier = Modifier
                .size(56.dp)
                .clip(CircleShape)
                .background(if (isStreaming) StatusCrimson else StatusEmerald)
                .clickable { onToggleStream() },
            contentAlignment = Alignment.Center
        ) {
            Icon(
                imageVector = if (isStreaming) Icons.Default.Stop else Icons.Default.PlayArrow,
                contentDescription = if (isStreaming) "Зупинити" else "Старт",
                tint = Color.Black,
                modifier = Modifier.size(28.dp)
            )
        }

        // 5. Блокування експозиції (AE Lock)
        IconButton(
            onClick = onToggleAeLock,
            modifier = Modifier
                .size(42.dp)
                .clip(CircleShape)
                .background(if (isAeLocked) StatusAmber else OledBorder)
        ) {
            Icon(
                imageVector = if (isAeLocked) Icons.Default.Lock else Icons.Default.LockOpen,
                contentDescription = "AE Lock",
                tint = if (isAeLocked) Color.Black else StatusAmber,
                modifier = Modifier.size(20.dp)
            )
        }

        // 6. Відкриття панелі Pro повзунків із замочками та .cube LUT
        IconButton(
            onClick = onToggleProPanel,
            modifier = Modifier
                .size(42.dp)
                .clip(CircleShape)
                .background(if (isProOpen) StatusCyan else OledBorder)
        ) {
            Icon(
                imageVector = Icons.Default.Tune,
                contentDescription = "Pro & LUT",
                tint = if (isProOpen) Color.Black else StatusCyan,
                modifier = Modifier.size(20.dp)
            )
        }

        // 7. Захист OLED екрана (Blackout)
        IconButton(
            onClick = onActivateDisplayProtection,
            modifier = Modifier
                .size(42.dp)
                .clip(CircleShape)
                .background(OledBorder)
        ) {
            Icon(
                imageVector = Icons.Default.NightlightRound,
                contentDescription = "OLED Blackout",
                tint = TextPrimary,
                modifier = Modifier.size(20.dp)
            )
        }
    }
}
