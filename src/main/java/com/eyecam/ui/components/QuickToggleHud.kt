package com.eyecam.ui.components

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CenterFocusStrong
import androidx.compose.material.icons.filled.FlashOff
import androidx.compose.material.icons.filled.FlashOn
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.LockOpen
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.NightlightRound
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Thermostat
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.Usb
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Icon
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
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.eyecam.model.AppLanguage
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
 * Адаптивний плаваючий HUD EyeCam:
 * - Верхній рядок з одною кнопкою «☰ Menu / Меню» без сплющування елементів на малих та великих екранах
 * - Кнопки лише реально наявних фізичних модулів камери + внутрішній зум всередині модуля
 * - Нижнє меню з горизонтальним прогортанням (horizontalScroll), куди інтегрована і кнопка OLED-затемнення
 * - Ручні повзунки (ISO, Shutter, Focus, WB) із замочками (🔒) та завантаженням .cube 3D LUT
 * - Повна підтримка англійської (EN за замовчуванням) та української (UA) мов
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
    onOpenMenuModal: () -> Unit,
    onActivateDisplayProtection: () -> Unit,
    modifier: Modifier = Modifier
) {
    var showProPanel by remember { mutableStateOf(false) }
    var afHighlightActive by remember { mutableStateOf(false) }
    var autoHighlightActive by remember { mutableStateOf(false) }
    val isUa = uiState.language == AppLanguage.UA

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
            .padding(horizontal = 10.dp, vertical = 10.dp)
    ) {
        // 1. ВЕРХНІЙ РЯДОК: Статус підключення + Температура + Єдина кнопка «☰ Menu / Меню»
        TopStatusBar(
            uiState = uiState,
            isUa = isUa,
            onOpenMenuModal = onOpenMenuModal,
            modifier = Modifier.align(Alignment.TopCenter)
        )

        // 2. БОКОВИЙ СЕЛЕКТОР МОДУЛІВ КАМЕР + ЗУМ ВСЕРЕДИНІ МОДУЛЯ
        ModuleAndIntraZoomColumn(
            availableModules = uiState.availableModules,
            activeModuleKey = uiState.activeModuleKey,
            intraZoom = uiState.intraModuleZoom,
            maxIntraZoom = uiState.maxIntraModuleZoom,
            isUa = isUa,
            onSelectModule = onSelectModule,
            onSetIntraZoom = onSetIntraModuleZoom,
            modifier = Modifier
                .align(Alignment.CenterEnd)
                .padding(top = 48.dp, bottom = 92.dp, end = 2.dp)
        )

        // 3. НИЖНІЙ БЛОК: Панель Pro ISP & LUT + Телеметрія + Горизонтально прокручуване меню дій
        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth(),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            AnimatedVisibility(
                visible = showProPanel,
                enter = fadeIn(),
                exit = fadeOut()
            ) {
                ProManualAndLutPanel(
                    uiState = uiState,
                    isUa = isUa,
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

            StreamTelemetryBar(uiState = uiState, isUa = isUa)

            ScrollableBottomControlBar(
                isStreaming = uiState.isStreaming,
                isMicMuted = uiState.isMicMuted,
                isTorchOn = uiState.isTorchOn,
                isTorchAvailable = uiState.isTorchAvailable,
                isAeLocked = uiState.isAeLocked,
                isProOpen = showProPanel || uiState.cameraMode.hasAnyManualLock || uiState.activeLutName.isNotEmpty(),
                afHighlightActive = afHighlightActive,
                isUa = isUa,
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
    isUa: Boolean,
    onOpenMenuModal: () -> Unit,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(top = 6.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(modifier = Modifier.weight(1f, fill = false)) {
            ConnectionPill(
                type = uiState.telemetry.connectionType,
                details = uiState.telemetry.connectionDetails,
                isUa = isUa
            )
        }

        Spacer(modifier = Modifier.width(8.dp))

        Row(
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            ThermalStatusPill(
                thermalState = uiState.telemetry.thermalState,
                isUa = isUa
            )

            // Єдина компактна кнопка Меню (Підтримка автора + GitHub / Оновлення + Мова EN/UA)
            Row(
                modifier = Modifier
                    .clip(RoundedCornerShape(18.dp))
                    .background(OledPillBackground)
                    .border(1.dp, StatusCyan.copy(alpha = 0.7f), RoundedCornerShape(18.dp))
                    .clickable { onOpenMenuModal() }
                    .padding(horizontal = 11.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(5.dp)
            ) {
                Icon(
                    imageVector = Icons.Default.Menu,
                    contentDescription = if (isUa) "Меню" else "Menu",
                    tint = StatusCyan,
                    modifier = Modifier.size(15.dp)
                )
                Text(
                    text = if (isUa) "Меню" else "Menu",
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                    color = StatusCyan,
                    maxLines = 1
                )
            }
        }
    }
}

@Composable
private fun ConnectionPill(
    type: ConnectionType,
    details: String,
    isUa: Boolean
) {
    val (icon, color, defaultLabel) = when (type) {
        ConnectionType.USB -> Triple(Icons.Default.Usb, StatusEmerald, if (isUa) "USB ГОТОВИЙ" else "USB READY")
        ConnectionType.WIFI -> Triple(Icons.Default.Wifi, StatusCyan, "WI-FI LAN")
        ConnectionType.NONE -> Triple(Icons.Default.Usb, TextDimmed, if (isUa) "ОФЛАЙН" else "OFFLINE")
    }

    val displayText = if (details.isNotBlank()) details else defaultLabel

    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(18.dp))
            .background(OledPillBackground)
            .border(1.dp, if (type != ConnectionType.NONE) color.copy(alpha = 0.5f) else OledBorder, RoundedCornerShape(18.dp))
            .padding(horizontal = 10.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(5.dp)
    ) {
        Icon(
            imageVector = icon,
            contentDescription = displayText,
            tint = color,
            modifier = Modifier.size(14.dp)
        )
        Text(
            text = displayText,
            fontSize = 10.sp,
            fontWeight = FontWeight.Bold,
            color = color,
            fontFamily = FontFamily.Monospace,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

@Composable
private fun ThermalStatusPill(
    thermalState: ThermalState,
    isUa: Boolean
) {
    val (color, label) = when (thermalState) {
        ThermalState.NORMAL -> StatusEmerald to "OK"
        ThermalState.LIGHT -> StatusEmerald to if (isUa) "ТЕПЛО" else "WARM"
        ThermalState.MODERATE -> StatusAmber to if (isUa) "ТРОТЛ" else "THROT"
        ThermalState.SEVERE -> StatusCrimson to if (isUa) "ГАРЯЧЕ" else "HOT"
        ThermalState.CRITICAL -> StatusCrimson to if (isUa) "КРИТ" else "CRIT"
        ThermalState.UNKNOWN -> TextDimmed to "N/A"
    }

    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(18.dp))
            .background(OledPillBackground)
            .border(1.dp, if (thermalState == ThermalState.SEVERE) StatusCrimson else OledBorder, RoundedCornerShape(18.dp))
            .padding(horizontal = 8.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(3.dp)
    ) {
        Icon(
            imageVector = Icons.Default.Thermostat,
            contentDescription = "Thermal Status",
            tint = color,
            modifier = Modifier.size(13.dp)
        )
        Text(
            text = label,
            fontSize = 10.sp,
            fontWeight = FontWeight.Bold,
            color = color,
            fontFamily = FontFamily.Monospace,
            maxLines = 1
        )
    }
}

/**
 * Компактна колонка фізично наявних модулів камери + плавний зум всередині активного модуля.
 * На смартфонах із 2 камерами (напр. Redmi 8A) відображає рівно 2 кнопки модулів («1x» та «SELF»).
 */
@Composable
private fun ModuleAndIntraZoomColumn(
    availableModules: List<CameraModuleInfo>,
    activeModuleKey: String,
    intraZoom: Float,
    maxIntraZoom: Float,
    isUa: Boolean,
    onSelectModule: (String) -> Unit,
    onSetIntraZoom: (Float) -> Unit,
    modifier: Modifier = Modifier
) {
    val modulesToShow = if (availableModules.isNotEmpty()) {
        availableModules
    } else {
        listOf(
            CameraModuleInfo(
                key = "main",
                label = "1x Main",
                shortLabel = "1x",
                logicalCameraId = "0"
            )
        )
    }

    Column(
        modifier = modifier,
        horizontalAlignment = Alignment.End,
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Column(
            modifier = Modifier
                .clip(RoundedCornerShape(22.dp))
                .background(OledPillBackground)
                .border(1.dp, OledBorder, RoundedCornerShape(22.dp))
                .padding(vertical = 5.dp, horizontal = 4.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                text = if (isUa) "ЛІНЗИ" else "LENS",
                fontSize = 7.sp,
                fontWeight = FontWeight.Bold,
                color = TextDimmed,
                fontFamily = FontFamily.Monospace,
                maxLines = 1
            )
            modulesToShow.forEach { mod ->
                val selected = activeModuleKey == mod.key
                val activeColor = if (mod.isFront) StatusCyan else StatusEmerald

                Box(
                    modifier = Modifier
                        .size(34.dp)
                        .clip(CircleShape)
                        .background(if (selected) activeColor else Color(0xFF151C28))
                        .border(
                            width = 1.dp,
                            color = if (selected) activeColor else OledBorder,
                            shape = CircleShape
                        )
                        .clickable { onSelectModule(mod.key) },
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = mod.shortLabel,
                        fontSize = 9.sp,
                        fontWeight = FontWeight.Bold,
                        color = if (selected) Color.Black else TextPrimary,
                        fontFamily = FontFamily.Monospace,
                        maxLines = 1
                    )
                }
            }
        }

        // Зум суворо всередині обраного модуля (1.0x .. maxIntraZoom)
        Column(
            modifier = Modifier
                .clip(RoundedCornerShape(18.dp))
                .background(OledPillBackground)
                .border(1.dp, OledBorder, RoundedCornerShape(18.dp))
                .padding(vertical = 4.dp, horizontal = 4.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(3.dp)
        ) {
            Box(
                modifier = Modifier
                    .size(30.dp)
                    .clip(CircleShape)
                    .background(OledCard)
                    .clickable {
                        onSetIntraZoom((intraZoom + 0.5f).coerceAtMost(maxIntraZoom))
                    },
                contentAlignment = Alignment.Center
            ) {
                Text("+", color = StatusCyan, fontWeight = FontWeight.Bold, fontSize = 14.sp)
            }

            Text(
                text = String.format(Locale.US, "%.1fx", intraZoom),
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                color = StatusEmerald,
                fontFamily = FontFamily.Monospace,
                maxLines = 1
            )

            Box(
                modifier = Modifier
                    .size(30.dp)
                    .clip(CircleShape)
                    .background(OledCard)
                    .clickable {
                        onSetIntraZoom((intraZoom - 0.5f).coerceAtLeast(1.0f))
                    },
                contentAlignment = Alignment.Center
            ) {
                Text("−", color = StatusCyan, fontWeight = FontWeight.Bold, fontSize = 14.sp)
            }
        }
    }
}

@Composable
private fun ProManualAndLutPanel(
    uiState: StreamUiState,
    isUa: Boolean,
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
            .widthIn(max = 460.dp)
            .heightIn(max = 240.dp)
            .clip(RoundedCornerShape(18.dp))
            .background(OledCard.copy(alpha = 0.96f))
            .border(1.dp, OledBorder, RoundedCornerShape(18.dp))
            .verticalScroll(rememberScrollState())
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .clip(RoundedCornerShape(10.dp))
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
                        RoundedCornerShape(10.dp)
                    )
                    .clickable { onResetAllToAuto() }
                    .padding(horizontal = 10.dp, vertical = 5.dp)
            ) {
                Text(
                    text = if (autoHighlightActive) {
                        if (isUa) "✨ AUTO АКТИВОВАНО!" else "✨ AUTO RESTORED!"
                    } else {
                        if (isUa) "✨ Скинути все в AUTO" else "✨ Reset All to AUTO"
                    },
                    fontSize = 10.sp,
                    fontWeight = FontWeight.Bold,
                    color = if (autoHighlightActive) Color.Black else StatusEmerald,
                    maxLines = 1
                )
            }

            Row(
                modifier = Modifier
                    .clip(RoundedCornerShape(10.dp))
                    .background(if (uiState.isAeLocked) StatusAmber else OledPillBackground)
                    .border(1.dp, if (uiState.isAeLocked) StatusAmber else OledBorder, RoundedCornerShape(10.dp))
                    .clickable { onToggleAeLock() }
                    .padding(horizontal = 9.dp, vertical = 5.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Icon(
                    imageVector = if (uiState.isAeLocked) Icons.Default.Lock else Icons.Default.LockOpen,
                    contentDescription = "AE Lock",
                    tint = if (uiState.isAeLocked) Color.Black else StatusAmber,
                    modifier = Modifier.size(13.dp)
                )
                Text(
                    text = if (uiState.isAeLocked) {
                        if (isUa) "AE ЗАФІКСОВАНО" else "AE LOCKED"
                    } else {
                        if (isUa) "Фікс. Експозиції" else "Lock Exposure"
                    },
                    fontSize = 10.sp,
                    fontWeight = FontWeight.Bold,
                    color = if (uiState.isAeLocked) Color.Black else StatusAmber,
                    maxLines = 1
                )
            }
        }

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

        val shutterDenom = 30 + (shutterSlider * 3970f).roundToInt()
        val shutterLabel = if (isUa) "Витримка" else "Shutter"
        LockableParameterSliderRow(
            title = if (mode.isShutterManual) "$shutterLabel: 1/${shutterDenom}s" else "$shutterLabel: AUTO",
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

        val focusDiopters = focusSlider * 10.0f
        val focusLabel = if (isUa) "Фокус" else "Focus"
        LockableParameterSliderRow(
            title = if (mode.isFocusManual) {
                String.format(Locale.US, "$focusLabel: %.1f dpt", focusDiopters)
            } else {
                "$focusLabel: AUTO"
            },
            value = focusSlider,
            isLocked = mode.isFocusManual,
            onValueChange = { v ->
                focusSlider = v
                onUpdateManualFocus((v * 10.0f).coerceAtLeast(0.05f))
            },
            onUnlockToAuto = { onUpdateManualFocus(0f) }
        )

        val wbKelvin = 2800 + (wbSlider * 5700f).roundToInt()
        val wbLabel = if (isUa) "ББ" else "WB"
        LockableParameterSliderRow(
            title = if (mode.isWbManual) "$wbLabel: ${wbKelvin}K" else "$wbLabel: AUTO",
            value = wbSlider,
            isLocked = mode.isWbManual,
            onValueChange = { v ->
                wbSlider = v
                val k = 2800 + (v * 5700f).roundToInt()
                onUpdateManualWb(k)
            },
            onUnlockToAuto = { onUpdateManualWb(0) }
        )

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(10.dp))
                    .background(OledPillBackground)
                    .border(1.dp, if (uiState.activeLutName.isNotEmpty()) StatusCyan else OledBorder, RoundedCornerShape(10.dp))
                    .clickable { onPickCubeLut() }
                    .padding(horizontal = 10.dp, vertical = 7.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = if (uiState.activeLutName.isNotEmpty()) {
                        "🎨 LUT: ${uiState.activeLutName}"
                    } else {
                        if (isUa) "📂 Завантажити .cube 3D LUT" else "📂 Load .cube 3D LUT"
                    },
                    fontSize = 10.sp,
                    fontWeight = FontWeight.Bold,
                    color = if (uiState.activeLutName.isNotEmpty()) StatusCyan else TextPrimary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }

            if (uiState.activeLutName.isNotEmpty()) {
                Spacer(modifier = Modifier.width(6.dp))
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(10.dp))
                        .background(StatusCrimson.copy(alpha = 0.22f))
                        .border(1.dp, StatusCrimson, RoundedCornerShape(10.dp))
                        .clickable { onClearCubeLut() }
                        .padding(horizontal = 10.dp, vertical = 7.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = if (isUa) "✕ Скинути" else "✕ Clear",
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Bold,
                        color = StatusCrimson,
                        maxLines = 1
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
        horizontalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Text(
            text = title,
            fontSize = 10.sp,
            fontWeight = FontWeight.SemiBold,
            color = if (isLocked) StatusCyan else TextSecondary,
            fontFamily = FontFamily.Monospace,
            maxLines = 1,
            modifier = Modifier.width(114.dp)
        )

        Slider(
            value = value,
            onValueChange = onValueChange,
            modifier = Modifier
                .weight(1f)
                .height(24.dp),
            colors = SliderDefaults.colors(
                thumbColor = if (isLocked) StatusCyan else TextSecondary,
                activeTrackColor = if (isLocked) StatusCyan else TextDimmed,
                inactiveTrackColor = OledBorder
            )
        )

        if (isLocked) {
            Box(
                modifier = Modifier
                    .size(26.dp)
                    .clip(CircleShape)
                    .background(StatusAmber.copy(alpha = 0.22f))
                    .border(1.dp, StatusAmber, CircleShape)
                    .clickable { onUnlockToAuto() },
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    imageVector = Icons.Default.Lock,
                    contentDescription = "Unlock to AUTO",
                    tint = StatusAmber,
                    modifier = Modifier.size(13.dp)
                )
            }
        } else {
            Spacer(modifier = Modifier.size(26.dp))
        }
    }
}

@Composable
private fun StreamTelemetryBar(
    uiState: StreamUiState,
    isUa: Boolean,
    modifier: Modifier = Modifier
) {
    AnimatedVisibility(
        visible = uiState.isStreaming,
        enter = fadeIn(),
        exit = fadeOut()
    ) {
        Row(
            modifier = modifier
                .clip(RoundedCornerShape(14.dp))
                .background(OledPillBackground)
                .border(1.dp, OledBorder, RoundedCornerShape(14.dp))
                .padding(horizontal = 12.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            TelemetryItem(
                label = "FPS",
                value = "${uiState.telemetry.fps}/${uiState.telemetry.targetFps}"
            )
            TelemetryItem(
                label = if (isUa) "МОДУЛЬ" else "MODULE",
                value = "${uiState.activeModuleKey.uppercase()} (${String.format(Locale.US, "%.1fx", uiState.intraModuleZoom)})"
            )
            TelemetryItem(
                label = if (isUa) "ФОРМАТ" else "FORMAT",
                value = "${uiState.telemetry.resolutionWidth}x${uiState.telemetry.resolutionHeight}"
            )
        }
    }
}

@Composable
private fun TelemetryItem(label: String, value: String) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(text = label, fontSize = 8.sp, color = TextDimmed, fontWeight = FontWeight.SemiBold, maxLines = 1)
        Text(
            text = value,
            fontSize = 10.sp,
            color = TextPrimary,
            fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace,
            maxLines = 1
        )
    }
}

/**
 * Горизонтально прокручуване нижнє меню керування:
 * Всі кнопки (Старт/Стоп, Автофокус, Ліхтарик, Блокування експозиції, Pro/LUT, Мікрофон та OLED Blackout)
 * плавно гортаються горизонтально і ніколи не налазять одна на одну навіть на маленьких екранах.
 */
@Composable
private fun ScrollableBottomControlBar(
    isStreaming: Boolean,
    isMicMuted: Boolean,
    isTorchOn: Boolean,
    isTorchAvailable: Boolean,
    isAeLocked: Boolean,
    isProOpen: Boolean,
    afHighlightActive: Boolean,
    isUa: Boolean,
    onToggleStream: () -> Unit,
    onToggleMicMute: () -> Unit,
    onToggleTorch: () -> Unit,
    onToggleAeLock: () -> Unit,
    onTriggerAutoFocus: () -> Unit,
    onToggleProPanel: () -> Unit,
    onActivateDisplayProtection: () -> Unit
) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .widthIn(max = 520.dp)
            .clip(RoundedCornerShape(28.dp))
            .background(OledCard.copy(alpha = 0.94f))
            .border(1.dp, OledBorder, RoundedCornerShape(28.dp))
    ) {
        Row(
            modifier = Modifier
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 10.dp, vertical = 7.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // 1. Головна кнопка Старт / Стоп
            Row(
                modifier = Modifier
                    .height(42.dp)
                    .clip(RoundedCornerShape(21.dp))
                    .background(if (isStreaming) StatusCrimson else StatusEmerald)
                    .clickable { onToggleStream() }
                    .padding(horizontal = 14.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(5.dp)
            ) {
                Icon(
                    imageVector = if (isStreaming) Icons.Default.Stop else Icons.Default.PlayArrow,
                    contentDescription = if (isStreaming) "Stop" else "Start",
                    tint = Color.Black,
                    modifier = Modifier.size(20.dp)
                )
                Text(
                    text = if (isStreaming) {
                        if (isUa) "Стоп" else "Stop"
                    } else {
                        if (isUa) "Старт" else "Start"
                    },
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    color = Color.Black,
                    maxLines = 1
                )
            }

            // 2. Автофокус із візуальним відгуком
            ScrollableHudPillButton(
                icon = Icons.Default.CenterFocusStrong,
                label = if (isUa) "Фокус" else "AF",
                active = afHighlightActive,
                enabled = true,
                activeBg = StatusCyan,
                activeText = Color.Black,
                idleTint = StatusCyan,
                onClick = onTriggerAutoFocus
            )

            // 3. Ліхтарик (недійсний при фронтальній камері)
            ScrollableHudPillButton(
                icon = if (isTorchOn && isTorchAvailable) Icons.Default.FlashOn else Icons.Default.FlashOff,
                label = if (isUa) "Світло" else "Torch",
                active = isTorchOn && isTorchAvailable,
                enabled = isTorchAvailable,
                activeBg = StatusAmber,
                activeText = Color.Black,
                idleTint = TextPrimary,
                onClick = { if (isTorchAvailable) onToggleTorch() }
            )

            // 4. Блокування експозиції (AE Lock)
            ScrollableHudPillButton(
                icon = if (isAeLocked) Icons.Default.Lock else Icons.Default.LockOpen,
                label = if (isUa) "Фікс. AE" else "AE Lock",
                active = isAeLocked,
                enabled = true,
                activeBg = StatusAmber,
                activeText = Color.Black,
                idleTint = StatusAmber,
                onClick = onToggleAeLock
            )

            // 5. Панель Pro повзунків із замочками та .cube LUT
            ScrollableHudPillButton(
                icon = Icons.Default.Tune,
                label = "Pro / LUT",
                active = isProOpen,
                enabled = true,
                activeBg = StatusCyan,
                activeText = Color.Black,
                idleTint = StatusCyan,
                onClick = onToggleProPanel
            )

            // 6. Мікрофон
            ScrollableHudPillButton(
                icon = if (isMicMuted) Icons.Default.MicOff else Icons.Default.Mic,
                label = if (isMicMuted) {
                    if (isUa) "Вимк." else "Muted"
                } else {
                    if (isUa) "Звук" else "Mic"
                },
                active = isMicMuted,
                enabled = true,
                activeBg = StatusCrimson,
                activeText = Color.White,
                idleTint = TextPrimary,
                onClick = onToggleMicMute
            )

            // 7. Захист OLED екрана (Blackout) — всередині прокручуваного рядка без накладання
            ScrollableHudPillButton(
                icon = Icons.Default.NightlightRound,
                label = if (isUa) "Екран" else "OLED",
                active = false,
                enabled = true,
                activeBg = OledBorder,
                activeText = TextPrimary,
                idleTint = TextPrimary,
                onClick = onActivateDisplayProtection
            )
        }
    }
}

@Composable
private fun ScrollableHudPillButton(
    icon: ImageVector,
    label: String,
    active: Boolean,
    enabled: Boolean,
    activeBg: Color,
    activeText: Color,
    idleTint: Color,
    onClick: () -> Unit
) {
    val bgColor = when {
        !enabled -> Color(0xFF0D1118)
        active -> activeBg
        else -> OledPillBackground
    }
    val contentColor = when {
        !enabled -> TextDimmed.copy(alpha = 0.35f)
        active -> activeText
        else -> idleTint
    }
    val borderColor = when {
        !enabled -> Color(0xFF181E29)
        active -> activeBg
        else -> OledBorder
    }

    Row(
        modifier = Modifier
            .height(40.dp)
            .clip(RoundedCornerShape(20.dp))
            .background(bgColor)
            .border(1.dp, borderColor, RoundedCornerShape(20.dp))
            .clickable(enabled = enabled) { onClick() }
            .padding(horizontal = 11.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(5.dp)
    ) {
        Icon(
            imageVector = icon,
            contentDescription = label,
            tint = contentColor,
            modifier = Modifier.size(17.dp)
        )
        Text(
            text = label,
            fontSize = 11.sp,
            fontWeight = FontWeight.SemiBold,
            color = contentColor,
            maxLines = 1
        )
    }
}
