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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
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
 * - Плаваючий HUD з кнопками модулів, внутрішнім зумом, ліхтариком, AE Lock та замочками повзунків
 * - Модальне вікно підтримки автора із заблюреним фоном програми (Patreon + Buy Me a Coffee)
 * - Модальне вікно GitHub та автоматичної перевірки оновлень з GitHub Releases
 */
@Composable
fun EyeCamScreen(
    viewModel: StreamViewModel,
    onRequestPermissions: () -> Unit = {},
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    val uiState by viewModel.uiState.collectAsState()
    var showSupportModal by remember { mutableStateOf(false) }
    var showUpdateModal by remember { mutableStateOf(false) }

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

    val isModalOpen = showSupportModal || showUpdateModal

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(OledPureBlack)
    ) {
        // Фоновий шар програми (блюриться при відкритті модального вікна підтримки або оновлень)
        Box(
            modifier = Modifier
                .fillMaxSize()
                .then(if (isModalOpen) Modifier.blur(18.dp) else Modifier)
                .pointerInput(uiState.isDisplayProtectionActive, isModalOpen) {
                    if (!uiState.isDisplayProtectionActive && !isModalOpen) {
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
                    onOpenSupportModal = { showSupportModal = true },
                    onOpenUpdateModal = { showUpdateModal = true },
                    onActivateDisplayProtection = { viewModel.activateDisplayProtection() }
                )
            }
        }

        // Модальне вікно підтримки автора поверх заблюреного фону
        if (showSupportModal) {
            SupportAuthorModalOverlay(
                onDismiss = { showSupportModal = false },
                onOpenUrl = { url ->
                    try {
                        context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
                    } catch (_: Exception) {}
                }
            )
        }

        // Модальне вікно GitHub та перевірки оновлень поверх заблюреного фону
        if (showUpdateModal) {
            GitHubUpdateModalOverlay(
                onDismiss = { showUpdateModal = false },
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
private fun SupportAuthorModalOverlay(
    onDismiss: () -> Unit,
    onOpenUrl: (String) -> Unit
) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xB806090F))
            .clickable { onDismiss() },
        contentAlignment = Alignment.Center
    ) {
        Column(
            modifier = Modifier
                .padding(24.dp)
                .widthIn(max = 380.dp)
                .clip(RoundedCornerShape(24.dp))
                .background(OledCard)
                .border(1.5.dp, StatusAmber.copy(alpha = 0.7f), RoundedCornerShape(24.dp))
                .clickable(enabled = false) {}
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            Text(
                text = "❤️ ПІДТРИМАТИ АВТОРА",
                fontSize = 18.sp,
                fontWeight = FontWeight.Bold,
                color = StatusAmber,
                textAlign = TextAlign.Center
            )
            Text(
                text = "Дякуємо за використання EyeCam! Ви можете підтримати розвиток проєкту та нові оновлення:",
                fontSize = 13.sp,
                color = TextSecondary,
                textAlign = TextAlign.Center
            )

            // Кнопка 1: Patreon
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(14.dp))
                    .background(Color(0xFFFF424D))
                    .clickable { onOpenUrl(PATREON_URL) }
                    .padding(vertical = 14.dp, horizontal = 16.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = "🟠  Patreon — Gladiator_Satan",
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                    color = Color.White
                )
            }

            // Кнопка 2: Buy Me a Coffee
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(14.dp))
                    .background(StatusAmber)
                    .clickable { onOpenUrl(BUYMEACOFFEE_URL) }
                    .padding(vertical = 14.dp, horizontal = 16.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = "☕  Buy Me a Coffee — gladiator_satan",
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                    color = Color.Black
                )
            }

            Spacer(modifier = Modifier.height(4.dp))

            Box(
                modifier = Modifier
                    .clip(RoundedCornerShape(12.dp))
                    .background(OledPillBackground)
                    .border(1.dp, OledBorder, RoundedCornerShape(12.dp))
                    .clickable { onDismiss() }
                    .padding(horizontal = 24.dp, vertical = 10.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = "Закрити",
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = TextPrimary
                )
            }
        }
    }
}

@Composable
private fun GitHubUpdateModalOverlay(
    onDismiss: () -> Unit,
    onOpenUrl: (String) -> Unit
) {
    val scope = rememberCoroutineScope()
    var checking by remember { mutableStateOf(true) }
    var statusText by remember { mutableStateOf("Перевірка оновлень на GitHub Releases...") }
    var latestVersion by remember { mutableStateOf("") }
    var releaseUrl by remember { mutableStateOf(GITHUB_RELEASES_URL) }
    var hasUpdate by remember { mutableStateOf(false) }

    fun triggerCheck() {
        checking = true
        statusText = "Перевірка оновлень на GitHub Releases..."
        scope.launch {
            val res = checkGitHubLatestRelease()
            checking = false
            latestVersion = res.latestTag
            releaseUrl = res.htmlUrl
            hasUpdate = res.isNewer
            statusText = res.message
        }
    }

    LaunchedEffect(Unit) {
        triggerCheck()
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
                .padding(24.dp)
                .widthIn(max = 390.dp)
                .clip(RoundedCornerShape(24.dp))
                .background(OledCard)
                .border(1.5.dp, StatusCyan.copy(alpha = 0.7f), RoundedCornerShape(24.dp))
                .clickable(enabled = false) {}
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            Text(
                text = "🐙 GITHUB ТА ОНОВЛЕННЯ",
                fontSize = 18.sp,
                fontWeight = FontWeight.Bold,
                color = StatusCyan,
                textAlign = TextAlign.Center
            )

            Text(
                text = "Поточна версія EyeCam: v$CURRENT_APP_VERSION",
                fontSize = 12.sp,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold,
                color = TextSecondary
            )

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp))
                    .background(OledPillBackground)
                    .border(
                        1.dp,
                        if (hasUpdate) StatusEmerald else OledBorder,
                        RoundedCornerShape(12.dp)
                    )
                    .padding(14.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = statusText,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = if (hasUpdate) StatusEmerald else TextPrimary,
                    textAlign = TextAlign.Center
                )
            }

            if (hasUpdate) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(14.dp))
                        .background(StatusEmerald)
                        .clickable { onOpenUrl(releaseUrl) }
                        .padding(vertical = 14.dp, horizontal = 16.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = "⬇️ Завантажити нову версію ($latestVersion)",
                        fontSize = 14.sp,
                        fontWeight = FontWeight.Bold,
                        color = Color.Black
                    )
                }
            }

            // Посилання на офіційний репозиторій GitHub
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(14.dp))
                    .background(StatusCyan)
                    .clickable { onOpenUrl(GITHUB_REPO_URL) }
                    .padding(vertical = 13.dp, horizontal = 16.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = "🌐 Відкрити GitHub репозиторій EyeCam",
                    fontSize = 13.sp,
                    fontWeight = FontWeight.Bold,
                    color = Color.Black
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .clip(RoundedCornerShape(12.dp))
                        .background(OledPillBackground)
                        .border(1.dp, OledBorder, RoundedCornerShape(12.dp))
                        .clickable(enabled = !checking) { triggerCheck() }
                        .padding(vertical = 10.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = "↻ Перевірити ще раз",
                        fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold,
                        color = StatusCyan
                    )
                }

                Box(
                    modifier = Modifier
                        .weight(1f)
                        .clip(RoundedCornerShape(12.dp))
                        .background(OledPillBackground)
                        .border(1.dp, OledBorder, RoundedCornerShape(12.dp))
                        .clickable { onDismiss() }
                        .padding(vertical = 10.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = "Закрити",
                        fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold,
                        color = TextPrimary
                    )
                }
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

private suspend fun checkGitHubLatestRelease(): UpdateCheckResult = withContext(Dispatchers.IO) {
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
                "🚀 Доступна нова версія $tag на GitHub Releases!"
            } else {
                "✓ У вас встановлена найновіша версія (v$CURRENT_APP_VERSION)"
            }
            UpdateCheckResult(newer, tag, url, msg)
        } else {
            UpdateCheckResult(
                false,
                "v$CURRENT_APP_VERSION",
                GITHUB_RELEASES_URL,
                "Не вдалося отримати дані релізу (HTTP $code). Перевірте сторінку Releases."
            )
        }
    } catch (e: Exception) {
        UpdateCheckResult(
            false,
            "v$CURRENT_APP_VERSION",
            GITHUB_RELEASES_URL,
            "Немає з'єднання з GitHub API. Ви можете відкрити сторінку релізів вручну."
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
