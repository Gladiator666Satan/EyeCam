package com.eyecam.ui

import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
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
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
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
import com.eyecam.ui.theme.TextPrimary
import com.eyecam.ui.theme.TextSecondary
import com.eyecam.viewmodel.StreamViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

private const val CURRENT_APP_VERSION = "1.4.0"
private const val PATREON_URL = "https://www.patreon.com/cw/Gladiator_Satan"
private const val BUYMEACOFFEE_URL = "https://buymeacoffee.com/gladiator_satan"
private const val GITHUB_REPO_URL = "https://github.com/Gladiator666Satan/EyeCam"
private const val GITHUB_RELEASES_URL = "https://github.com/Gladiator666Satan/EyeCam/releases/latest"
private const val GITHUB_API_LATEST_URL = "https://api.github.com/repos/Gladiator666Satan/EyeCam/releases/latest"

/**
 * Головний екран програми EyeCam:
 * - Живе прев'ю камери з TextureView + EglVideoTee (.cube 3D LUT)
 * - Плаваючий HUD із горизонтально прокручуваним нижнім меню, кнопками модулів та внутрішнім зумом
 * - Єдине модальне вікно Меню із заблюреним фоном програми:
 *   1. Перемикач мови (English за замовчуванням при першому встановленні / Українська)
 *   2. Підтримка автора (Patreon + Buy Me a Coffee)
 *   3. GitHub та автоматична перевірка оновлень з GitHub Releases
 */
@Composable
fun EyeCamScreen(
    viewModel: StreamViewModel,
    onRequestPermissions: () -> Unit = {},
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    val uiState by viewModel.uiState.collectAsState()
    var showMenuModal by remember { mutableStateOf(false) }

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

        // Модальне вікно Меню (Мова EN/UA + Підтримка автора + GitHub та Оновлення) поверх заблюреного фону
        if (showMenuModal) {
            UnifiedMenuModalOverlay(
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

@Composable
private fun UnifiedMenuModalOverlay(
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
    var hasUpdate by remember { mutableStateOf(false) }

    fun triggerCheck(forUa: Boolean) {
        checking = true
        statusText = if (forUa) "Перевірка оновлень на GitHub Releases..." else "Checking GitHub Releases for updates..."
        scope.launch {
            val res = checkGitHubLatestRelease(forUa)
            checking = false
            latestVersion = res.latestTag
            releaseUrl = res.htmlUrl
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
                            .background(StatusEmerald)
                            .clickable { onOpenUrl(releaseUrl) }
                            .padding(vertical = 11.dp, horizontal = 12.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = if (isUa) "⬇️ Завантажити оновлення ($latestVersion)" else "⬇️ Download Update ($latestVersion)",
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
            val newer = isRemoteVersionNewer(CURRENT_APP_VERSION, tag)
            val msg = if (newer) {
                if (isUa) "🚀 Доступна нова версія $tag на GitHub Releases!" else "🚀 New version $tag is available on GitHub Releases!"
            } else {
                if (isUa) "✓ У вас встановлена найновіша версія (v$CURRENT_APP_VERSION)" else "✓ You are running the latest version (v$CURRENT_APP_VERSION)"
            }
            UpdateCheckResult(newer, tag, url, msg)
        } else {
            UpdateCheckResult(
                false,
                "v$CURRENT_APP_VERSION",
                GITHUB_RELEASES_URL,
                if (isUa) "Не вдалося отримати дані релізу (HTTP $code)." else "Could not fetch release info (HTTP $code)."
            )
        }
    } catch (e: Exception) {
        UpdateCheckResult(
            false,
            "v$CURRENT_APP_VERSION",
            GITHUB_RELEASES_URL,
            if (isUa) "Немає з'єднання з GitHub API." else "Could not reach GitHub API."
        )
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
