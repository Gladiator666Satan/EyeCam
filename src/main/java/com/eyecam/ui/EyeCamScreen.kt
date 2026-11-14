package com.eyecam.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
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
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.FileProvider
import com.eyecam.model.AppLanguage
import com.eyecam.ui.components.CameraPreview
import com.eyecam.ui.components.DisplayProtectionOverlay
import com.eyecam.ui.components.QuickToggleHud
import com.eyecam.ui.theme.OledBorder
import com.eyecam.ui.theme.OledCard
import com.eyecam.ui.theme.OledPillBackground
import com.eyecam.ui.theme.OledPureBlack
import com.eyecam.ui.theme.StatusAmber
import com.eyecam.ui.theme.StatusCyan
import com.eyecam.ui.theme.StatusEmerald
import com.eyecam.ui.theme.TextDimmed
import com.eyecam.ui.theme.TextPrimary
import com.eyecam.ui.theme.TextSecondary
import com.eyecam.viewmodel.StreamViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL

private const val CURRENT_APP_VERSION = "1.4.1"
private const val PREFS_NAME = "eyecam_prefs"
private const val KEY_SKIPPED_VERSION = "skipped_update_version"
private const val PATREON_URL = "https://www.patreon.com/cw/Gladiator_Satan"
private const val BUYMEACOFFEE_URL = "https://buymeacoffee.com/gladiator_satan"
private const val GITHUB_REPO_URL = "https://github.com/Gladiator666Satan/EyeCam"
private const val GITHUB_RELEASES_URL = "https://github.com/Gladiator666Satan/EyeCam/releases/latest"
private const val GITHUB_API_LATEST_URL = "https://api.github.com/repos/Gladiator666Satan/EyeCam/releases/latest"

/**
 * Головний екран програми EyeCam:
 * - Живе прев'ю камери з TextureView + EglVideoTee (.cube 3D LUT + intra-module zoom)
 * - Плаваючий HUD із горизонтально прокручуваним нижнім меню, кнопками модулів та внутрішнім зумом
 * - Автоматичне сповіщення про оновлення (Встановити зараз / Нагадати пізніше / Пропустити цю версію)
 * - Єдине модальне вікно Меню із заблюреним фоном програми:
 *   1. Перемикач мови (English за замовчуванням при першому встановленні / Українська)
 *   2. Підтримка автора (Patreon + Buy Me a Coffee)
 *   3. GitHub та автоматична перевірка й встановлення оновлень з GitHub Releases
 */
@Composable
fun EyeCamScreen(
    viewModel: StreamViewModel,
    onRequestPermissions: () -> Unit = {},
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val uiState by viewModel.uiState.collectAsState()
    var showMenuModal by remember { mutableStateOf(false) }

    // Стан автоматичного сповіщення про доступне оновлення
    var autoUpdateBannerResult by remember { mutableStateOf<UpdateCheckResult?>(null) }
    var bannerDismissedForSession by remember { mutableStateOf(false) }
    var isDownloadingUpdate by remember { mutableStateOf(false) }
    var downloadProgressPct by remember { mutableIntStateOf(0) }
    var downloadErrorMsg by remember { mutableStateOf("") }

    // Автоматична перевірка нової версії при запуску програми
    LaunchedEffect(Unit) {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val skippedTag = prefs.getString(KEY_SKIPPED_VERSION, "") ?: ""
        val isUa = uiState.language == AppLanguage.UA
        val result = checkGitHubLatestRelease(isUa)
        if (result.isNewer && result.latestTag.isNotBlank() &&
            !result.latestTag.equals(skippedTag, ignoreCase = true)
        ) {
            autoUpdateBannerResult = result
        }
    }

    val cubePickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri != null) {
            var fileName = "custom.cube"
            try {
                context.contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                    val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0 && cursor.moveToFirst()) {
                        fileName = cursor.getString(idx) ?: fileName
                    }
                }
            } catch (_: Exception) {}
            viewModel.loadCubeLutFromUri(uri, fileName)
        }
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(OledPureBlack)
    ) {
        // Фоновий шар програми (блюриться при відкритті модального вікна Меню)
        Box(
            modifier = Modifier
                .fillMaxSize()
                .then(if (showMenuModal) Modifier.blur(18.dp) else Modifier)
                .pointerInput(uiState.isDisplayProtectionActive, showMenuModal) {
                    if (!uiState.isDisplayProtectionActive && !showMenuModal) {
                        detectTapGestures(
                            onPress = {
                                viewModel.onUserInteraction()
                            }
                        )
                    }
                }
        ) {
            CameraPreview(
                isStreaming = uiState.isStreaming,
                language = uiState.language,
                hasCameraPermission = uiState.hasCameraPermission,
                isDisplayProtectionActive = uiState.isDisplayProtectionActive,
                streamWidth = uiState.telemetry.resolutionWidth,
                streamHeight = uiState.telemetry.resolutionHeight,
                afTriggerPulseSeq = uiState.afTriggerPulseSeq,
                onSurfaceChanged = { surface, w, h, rot -> viewModel.attachPreviewSurface(surface, w, h, rot) },
                onFocusPointTapped = { x, y -> viewModel.triggerFocusAtPoint(x, y) },
                onRequestPermissions = onRequestPermissions
            )

            if (!uiState.isDisplayProtectionActive) {
                QuickToggleHud(
                    uiState = uiState,
                    onToggleStream = { viewModel.toggleStreaming(onPermissionMissing = onRequestPermissions) },
                    onToggleMicMute = { viewModel.toggleMicMute() },
                    onToggleTorch = { viewModel.toggleTorch() },
                    onToggleAeLock = { viewModel.toggleAeLock() },
                    onSelectModule = { modKey -> viewModel.selectCameraModule(modKey) },
                    onSetIntraModuleZoom = { zoom -> viewModel.setZoomRatio(zoom) },
                    onTriggerAutoFocus = { viewModel.triggerAutoFocus() },
                    onResetAllToAuto = { viewModel.resetAllToAuto() },
                    onUpdateManualIso = { iso -> viewModel.updateManualIso(iso) },
                    onUpdateManualShutter = { expNs -> viewModel.updateManualExposure(expNs) },
                    onUpdateManualFocus = { diopters -> viewModel.updateManualFocus(diopters) },
                    onUpdateManualWb = { kelvin -> viewModel.updateManualWhiteBalance(kelvin) },
                    onPickCubeLut = { cubePickerLauncher.launch(arrayOf("*/*")) },
                    onClearCubeLut = { viewModel.clearCubeLut() },
                    onOpenMenuModal = { showMenuModal = true },
                    onActivateDisplayProtection = { viewModel.activateDisplayProtection() }
                )
            }
        }

        // Компактне сповіщення з пропозицією оновитись (якщо знайдено нову версію)
        val bannerData = autoUpdateBannerResult
        AnimatedVisibility(
            visible = bannerData != null && !bannerDismissedForSession && !showMenuModal && !uiState.isDisplayProtectionActive,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier
                .align(Alignment.TopCenter)
                .statusBarsPadding()
                .padding(top = 56.dp, start = 12.dp, end = 12.dp)
        ) {
            if (bannerData != null) {
                UpdatePromptBanner(
                    language = uiState.language,
                    latestTag = bannerData.latestTag,
                    isDownloading = isDownloadingUpdate,
                    downloadProgressPct = downloadProgressPct,
                    errorMessage = downloadErrorMsg,
                    onInstallNow = {
                        if (!isDownloadingUpdate) {
                            isDownloadingUpdate = true
                            downloadProgressPct = 0
                            downloadErrorMsg = ""
                            scope.launch {
                                val ok = downloadAndInstallApk(
                                    context = context,
                                    apkUrl = bannerData.apkDownloadUrl,
                                    fallbackWebUrl = bannerData.htmlUrl,
                                    onProgress = { pct -> downloadProgressPct = pct }
                                )
                                isDownloadingUpdate = false
                                if (ok) {
                                    bannerDismissedForSession = true
                                } else {
                                    downloadErrorMsg = if (uiState.language == AppLanguage.UA) {
                                        "Не вдалося завантажити APK автоматично. Відкриваємо сторінку релізу..."
                                    } else {
                                        "Direct download failed. Opening release page..."
                                    }
                                }
                            }
                        }
                    },
                    onRemindLater = {
                        bannerDismissedForSession = true
                    },
                    onSkipVersion = {
                        try {
                            context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                                .edit()
                                .putString(KEY_SKIPPED_VERSION, bannerData.latestTag)
                                .apply()
                        } catch (_: Exception) {}
                        bannerDismissedForSession = true
                    }
                )
            }
        }

        // Модальне вікно Меню (Мова EN/UA + Підтримка автора + GitHub та Оновлення) поверх заблюреного фону
        if (showMenuModal) {
            UnifiedMenuModalOverlay(
                context = context,
                language = uiState.language,
                onSelectLanguage = { lang -> viewModel.setLanguage(lang) },
                onDismiss = { showMenuModal = false },
                onOpenUrl = { url ->
                    try {
                        context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
                    } catch (_: Exception) {}
                }
            )
        }

        // Оверлей захисту OLED екрана
        DisplayProtectionOverlay(
            uiState = uiState,
            onWake = { viewModel.wakeFromDisplayProtection() }
        )
    }
}

/**
 * Компактне спливаюче сповіщення про нову версію з 3 діями:
 * 1. Встановити зараз (завантажує APK з прогресом і запускає інсталятор пакета)
 * 2. Нагадати пізніше (приховує на поточну сесію)
 * 3. Пропустити саме цю версію (зберігає тег у SharedPreferences)
 */
@Composable
private fun UpdatePromptBanner(
    language: AppLanguage,
    latestTag: String,
    isDownloading: Boolean,
    downloadProgressPct: Int,
    errorMessage: String,
    onInstallNow: () -> Unit,
    onRemindLater: () -> Unit,
    onSkipVersion: () -> Unit
) {
    val isUa = language == AppLanguage.UA

    Column(
        modifier = Modifier
            .widthIn(max = 440.dp)
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(OledCard.copy(alpha = 0.97f))
            .border(1.2.dp, StatusEmerald.copy(alpha = 0.85f), RoundedCornerShape(16.dp))
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(7.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Text(
                text = if (isUa) {
                    "🚀 Доступне оновлення EyeCam $latestTag (поточна v$CURRENT_APP_VERSION)"
                } else {
                    "🚀 EyeCam Update $latestTag available (current v$CURRENT_APP_VERSION)"
                },
                fontSize = 11.5.sp,
                fontWeight = FontWeight.Bold,
                color = StatusEmerald,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f)
            )
        }

        if (isDownloading) {
            Text(
                text = if (isUa) {
                    "⏳ Завантаження та підготовка до встановлення... $downloadProgressPct%"
                } else {
                    "⏳ Downloading update package... $downloadProgressPct%"
                },
                fontSize = 10.5.sp,
                fontWeight = FontWeight.SemiBold,
                color = StatusCyan,
                fontFamily = FontFamily.Monospace
            )
        } else if (errorMessage.isNotEmpty()) {
            Text(
                text = errorMessage,
                fontSize = 10.sp,
                color = StatusAmber
            )
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // 1. Встановити зараз
            Box(
                modifier = Modifier
                    .weight(1.15f)
                    .clip(RoundedCornerShape(9.dp))
                    .background(if (isDownloading) StatusCyan.copy(alpha = 0.35f) else StatusEmerald)
                    .clickable(enabled = !isDownloading) { onInstallNow() }
                    .padding(vertical = 7.dp, horizontal = 6.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = if (isDownloading) {
                        "$downloadProgressPct%"
                    } else {
                        if (isUa) "⬇️ Встановити зараз" else "⬇️ Install Now"
                    },
                    fontSize = 10.5.sp,
                    fontWeight = FontWeight.Bold,
                    color = Color.Black,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }

            // 2. Нагадати пізніше
            Box(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(9.dp))
                    .background(OledPillBackground)
                    .border(1.dp, OledBorder, RoundedCornerShape(9.dp))
                    .clickable(enabled = !isDownloading) { onRemindLater() }
                    .padding(vertical = 7.dp, horizontal = 6.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = if (isUa) "Нагадати пізніше" else "Remind Later",
                    fontSize = 10.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = TextPrimary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }

            // 3. Пропустити саме цю версію
            Box(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(9.dp))
                    .background(OledPillBackground)
                    .border(1.dp, OledBorder, RoundedCornerShape(9.dp))
                    .clickable(enabled = !isDownloading) { onSkipVersion() }
                    .padding(vertical = 7.dp, horizontal = 6.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = if (isUa) "Пропустити $latestTag" else "Skip $latestTag",
                    fontSize = 10.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = TextDimmed,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
    }
}

@Composable
private fun UnifiedMenuModalOverlay(
    context: Context,
    language: AppLanguage,
    onSelectLanguage: (AppLanguage) -> Unit,
    onDismiss: () -> Unit,
    onOpenUrl: (String) -> Unit
) {
    val isUa = language == AppLanguage.UA
    val scope = rememberCoroutineScope()
    var checking by remember { mutableStateOf(true) }
    var statusText by remember {
        mutableStateOf(
            if (isUa) "Перевірка оновлень на GitHub Releases..." else "Checking GitHub Releases for updates..."
        )
    }
    var latestVersion by remember { mutableStateOf("") }
    var releaseUrl by remember { mutableStateOf(GITHUB_RELEASES_URL) }
    var apkDownloadUrl by remember { mutableStateOf("") }
    var hasUpdate by remember { mutableStateOf(false) }
    var downloadingInModal by remember { mutableStateOf(false) }
    var modalProgressPct by remember { mutableIntStateOf(0) }

    fun triggerCheck(forUa: Boolean) {
        checking = true
        statusText = if (forUa) "Перевірка оновлень на GitHub Releases..." else "Checking GitHub Releases for updates..."
        scope.launch {
            val res = checkGitHubLatestRelease(forUa)
            checking = false
            latestVersion = res.latestTag
            releaseUrl = res.htmlUrl
            apkDownloadUrl = res.apkDownloadUrl
            hasUpdate = res.isNewer
            statusText = res.message
        }
    }

    LaunchedEffect(isUa) {
        triggerCheck(isUa)
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xB806090F))
            .clickable { onDismiss() },
        contentAlignment = Alignment.Center
    ) {
        Column(
            modifier = Modifier
                .padding(16.dp)
                .widthIn(max = 390.dp)
                .heightIn(max = 600.dp)
                .clip(RoundedCornerShape(24.dp))
                .background(OledCard)
                .border(1.5.dp, StatusCyan.copy(alpha = 0.7f), RoundedCornerShape(24.dp))
                .clickable(enabled = false) {}
                .verticalScroll(rememberScrollState())
                .padding(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text(
                text = if (isUa) "☰ МЕНЮ EYECAM" else "☰ EYECAM MENU",
                fontSize = 17.sp,
                fontWeight = FontWeight.Bold,
                color = StatusCyan,
                textAlign = TextAlign.Center
            )

            // 1. Вибір мови (English за замовчуванням / Українська)
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(14.dp))
                    .background(OledPillBackground)
                    .border(1.dp, OledBorder, RoundedCornerShape(14.dp))
                    .padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    text = if (isUa) "🌐 МОВА ІНТЕРФЕЙСУ" else "🌐 APP LANGUAGE",
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                    color = TextSecondary
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    val enSelected = language == AppLanguage.EN
                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .clip(RoundedCornerShape(10.dp))
                            .background(if (enSelected) StatusEmerald else OledCard)
                            .border(1.dp, if (enSelected) StatusEmerald else OledBorder, RoundedCornerShape(10.dp))
                            .clickable { onSelectLanguage(AppLanguage.EN) }
                            .padding(vertical = 10.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = "English (EN)",
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            color = if (enSelected) Color.Black else TextPrimary
                        )
                    }

                    val uaSelected = language == AppLanguage.UA
                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .clip(RoundedCornerShape(10.dp))
                            .background(if (uaSelected) StatusEmerald else OledCard)
                            .border(1.dp, if (uaSelected) StatusEmerald else OledBorder, RoundedCornerShape(10.dp))
                            .clickable { onSelectLanguage(AppLanguage.UA) }
                            .padding(vertical = 10.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = "Українська (UA)",
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            color = if (uaSelected) Color.Black else TextPrimary
                        )
                    }
                }
            }

            // 2. Підтримка автора (Patreon & Buy Me a Coffee)
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(14.dp))
                    .background(OledPillBackground)
                    .border(1.dp, StatusAmber.copy(alpha = 0.55f), RoundedCornerShape(14.dp))
                    .padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text(
                    text = if (isUa) "❤️ ПІДТРИМАТИ АВТОРА" else "❤️ SUPPORT THE AUTHOR",
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    color = StatusAmber,
                    textAlign = TextAlign.Center
                )
                Text(
                    text = if (isUa) {
                        "Підтримайте розвиток EyeCam та вихід нових оновлень:"
                    } else {
                        "Support EyeCam development and future updates:"
                    },
                    fontSize = 11.sp,
                    color = TextSecondary,
                    textAlign = TextAlign.Center
                )

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(11.dp))
                        .background(Color(0xFFFF424D))
                        .clickable { onOpenUrl(PATREON_URL) }
                        .padding(vertical = 11.dp, horizontal = 12.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = "🟠  Patreon — Gladiator_Satan",
                        fontSize = 13.sp,
                        fontWeight = FontWeight.Bold,
                        color = Color.White
                    )
                }

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(11.dp))
                        .background(StatusAmber)
                        .clickable { onOpenUrl(BUYMEACOFFEE_URL) }
                        .padding(vertical = 11.dp, horizontal = 12.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = "☕  Buy Me a Coffee — gladiator_satan",
                        fontSize = 13.sp,
                        fontWeight = FontWeight.Bold,
                        color = Color.Black
                    )
                }
            }

            // 3. GitHub та Перевірка оновлень
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(14.dp))
                    .background(OledPillBackground)
                    .border(1.dp, StatusCyan.copy(alpha = 0.55f), RoundedCornerShape(14.dp))
                    .padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text(
                    text = if (isUa) "🐙 GITHUB ТА ОНОВЛЕННЯ (v$CURRENT_APP_VERSION)" else "🐙 GITHUB & UPDATES (v$CURRENT_APP_VERSION)",
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    color = StatusCyan,
                    fontFamily = FontFamily.Monospace,
                    textAlign = TextAlign.Center
                )

                Text(
                    text = statusText,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = if (hasUpdate) StatusEmerald else TextPrimary,
                    textAlign = TextAlign.Center
                )

                if (hasUpdate) {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(11.dp))
                            .background(if (downloadingInModal) StatusCyan else StatusEmerald)
                            .clickable(enabled = !downloadingInModal) {
                                downloadingInModal = true
                                modalProgressPct = 0
                                scope.launch {
                                    downloadAndInstallApk(
                                        context = context,
                                        apkUrl = apkDownloadUrl,
                                        fallbackWebUrl = releaseUrl,
                                        onProgress = { pct -> modalProgressPct = pct }
                                    )
                                    downloadingInModal = false
                                }
                            }
                            .padding(vertical = 11.dp, horizontal = 12.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = if (downloadingInModal) {
                                if (isUa) "⏳ Завантаження APK... $modalProgressPct%" else "⏳ Downloading APK... $modalProgressPct%"
                            } else {
                                if (isUa) "⬇️ Встановити зараз ($latestVersion)" else "⬇️ Install Now ($latestVersion)"
                            },
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            color = Color.Black
                        )
                    }
                }

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .clip(RoundedCornerShape(10.dp))
                            .background(StatusCyan)
                            .clickable { onOpenUrl(GITHUB_REPO_URL) }
                            .padding(vertical = 10.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = "🌐 GitHub Repo",
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            color = Color.Black
                        )
                    }

                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .clip(RoundedCornerShape(10.dp))
                            .background(OledCard)
                            .border(1.dp, StatusCyan.copy(alpha = 0.6f), RoundedCornerShape(10.dp))
                            .clickable(enabled = !checking) { triggerCheck(isUa) }
                            .padding(vertical = 10.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = if (isUa) "↻ Перевірити" else "↻ Check Updates",
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            color = StatusCyan
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.height(2.dp))

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp))
                    .background(OledPillBackground)
                    .border(1.dp, OledBorder, RoundedCornerShape(12.dp))
                    .clickable { onDismiss() }
                    .padding(vertical = 11.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = if (isUa) "Закрити меню" else "Close Menu",
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = TextPrimary
                )
            }
        }
    }
}

private data class UpdateCheckResult(
    val isNewer: Boolean,
    val latestTag: String,
    val htmlUrl: String,
    val apkDownloadUrl: String,
    val message: String
)

private suspend fun checkGitHubLatestRelease(isUa: Boolean): UpdateCheckResult = withContext(Dispatchers.IO) {
    try {
        val conn = (URL(GITHUB_API_LATEST_URL).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 5000
            readTimeout = 5000
            setRequestProperty("Accept", "application/vnd.github+json")
            setRequestProperty("User-Agent", "EyeCam-Android/$CURRENT_APP_VERSION")
        }
        val code = conn.responseCode
        if (code in 200..299) {
            val body = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
            val json = JSONObject(body)
            val tag = json.optString("tag_name", "v$CURRENT_APP_VERSION")
            val url = json.optString("html_url", GITHUB_RELEASES_URL)

            var apkUrl = "https://github.com/Gladiator666Satan/EyeCam/releases/download/$tag/EyeCam-Android-Release.apk"
            val assets = json.optJSONArray("assets")
            if (assets != null) {
                for (i in 0 until assets.length()) {
                    val asset = assets.optJSONObject(i) ?: continue
                    val name = asset.optString("name", "")
                    val dlUrl = asset.optString("browser_download_url", "")
                    if (name.endsWith(".apk", ignoreCase = true) && dlUrl.isNotBlank()) {
                        apkUrl = dlUrl
                        break
                    }
                }
            }

            val newer = isRemoteVersionNewer(CURRENT_APP_VERSION, tag)
            val msg = if (newer) {
                if (isUa) "🚀 Доступна нова версія $tag на GitHub Releases!" else "🚀 New version $tag is available on GitHub Releases!"
            } else {
                if (isUa) "✓ У вас встановлена найновіша версія (v$CURRENT_APP_VERSION)" else "✓ You are running the latest version (v$CURRENT_APP_VERSION)"
            }
            UpdateCheckResult(newer, tag, url, apkUrl, msg)
        } else {
            UpdateCheckResult(
                false,
                "v$CURRENT_APP_VERSION",
                GITHUB_RELEASES_URL,
                "",
                if (isUa) "Не вдалося отримати дані релізу (HTTP $code)." else "Could not fetch release info (HTTP $code)."
            )
        }
    } catch (_: Exception) {
        UpdateCheckResult(
            false,
            "v$CURRENT_APP_VERSION",
            GITHUB_RELEASES_URL,
            "",
            if (isUa) "Немає з'єднання з GitHub API." else "Could not reach GitHub API."
        )
    }
}

private suspend fun downloadAndInstallApk(
    context: Context,
    apkUrl: String,
    fallbackWebUrl: String,
    onProgress: (Int) -> Unit
): Boolean = withContext(Dispatchers.IO) {
    if (apkUrl.isBlank()) {
        withContext(Dispatchers.Main) {
            try {
                context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(fallbackWebUrl)).apply {
                    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                })
            } catch (_: Exception) {}
        }
        return@withContext false
    }

    try {
        val dir = context.externalCacheDir ?: context.cacheDir
        val outFile = File(dir, "EyeCam-Update.apk")
        if (outFile.exists()) outFile.delete()

        var currentUrl = apkUrl
        var redirects = 0
        var conn: HttpURLConnection
        while (true) {
            conn = (URL(currentUrl).openConnection() as HttpURLConnection).apply {
                instanceFollowRedirects = true
                connectTimeout = 10000
                readTimeout = 20000
                setRequestProperty("User-Agent", "EyeCam-Android/$CURRENT_APP_VERSION")
            }
            val status = conn.responseCode
            if (status in 301..308 && redirects < 5) {
                val loc = conn.getHeaderField("Location")
                if (!loc.isNullOrBlank()) {
                    currentUrl = loc
                    redirects++
                    continue
                }
            }
            break
        }

        if (conn.responseCode !in 200..299) {
            throw IllegalStateException("HTTP ${conn.responseCode}")
        }

        val totalBytes = conn.contentLengthLong
        var downloadedBytes = 0L

        conn.inputStream.use { input ->
            FileOutputStream(outFile).use { output ->
                val buffer = ByteArray(32 * 1024)
                var read: Int
                var lastPct = -1
                while (input.read(buffer).also { read = it } != -1) {
                    output.write(buffer, 0, read)
                    downloadedBytes += read
                    if (totalBytes > 0) {
                        val pct = ((downloadedBytes * 100L) / totalBytes).toInt().coerceIn(0, 100)
                        if (pct != lastPct) {
                            lastPct = pct
                            withContext(Dispatchers.Main) { onProgress(pct) }
                        }
                    }
                }
                output.flush()
            }
        }

        withContext(Dispatchers.Main) {
            onProgress(100)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && !context.packageManager.canRequestPackageInstalls()) {
                try {
                    val permIntent = Intent(
                        Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:${context.packageName}")
                    ).apply {
                        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    }
                    context.startActivity(permIntent)
                } catch (_: Exception) {}
            }

            val apkUri = FileProvider.getUriForFile(
                context,
                "${context.packageName}.fileprovider",
                outFile
            )
            val installIntent = Intent(Intent.ACTION_VIEW).apply {
                setDataAndType(apkUri, "application/vnd.android.package-archive")
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            context.startActivity(installIntent)
        }
        true
    } catch (_: Exception) {
        withContext(Dispatchers.Main) {
            try {
                context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(fallbackWebUrl)).apply {
                    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                })
            } catch (_: Exception) {}
        }
        false
    }
}

private fun isRemoteVersionNewer(currentVer: String, remoteTag: String): Boolean {
    fun parseParts(v: String): List<Int> {
        return v.trim().removePrefix("v").removePrefix("V")
            .split(".", "-")
            .mapNotNull { it.toIntOrNull() }
    }
    val cur = parseParts(currentVer)
    val rem = parseParts(remoteTag)
    for (i in 0 until maxOf(cur.size, rem.size)) {
        val c = cur.getOrElse(i) { 0 }
        val r = rem.getOrElse(i) { 0 }
        if (r > c) return true
        if (r < c) return false
    }
    return false
}
