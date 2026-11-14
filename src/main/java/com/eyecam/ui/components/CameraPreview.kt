package com.eyecam.ui.components

import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.display.DisplayManager
import android.view.Surface
import android.view.TextureView
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CenterFocusStrong
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.eyecam.model.AppLanguage
import com.eyecam.ui.theme.OledBorder
import com.eyecam.ui.theme.OledPillBackground
import com.eyecam.ui.theme.OledPureBlack
import com.eyecam.ui.theme.StatusCyan
import com.eyecam.ui.theme.StatusEmerald
import com.eyecam.ui.theme.TextDimmed
import kotlinx.coroutines.delay

/**
 * Професійний в'юпорт попереднього перегляду камери на базі TextureView + OpenGL ES Tee:
 * - Нульове розтягування в портретному та ландшафтному режимах незалежно від автоповороту.
 * - Миттєва реакція на зміну кута дисплея (0/90/180/270), включно зі швидким переворотом на 180°.
 * - Візуальне кільце автофокусу як при тапі по екрану, так і при натисканні кнопки автофокусу.
 */
@Composable
fun CameraPreview(
    isStreaming: Boolean,
    language: AppLanguage = AppLanguage.EN,
    hasCameraPermission: Boolean = true,
    isDisplayProtectionActive: Boolean = false,
    streamWidth: Int = 1920,
    streamHeight: Int = 1080,
    afTriggerPulseSeq: Long = 0L,
    onSurfaceChanged: (surface: Surface?, width: Int, height: Int, displayRotation: Int) -> Unit = { _, _, _, _ -> },
    onFocusPointTapped: ((Float, Float) -> Unit)? = null,
    onRequestPermissions: () -> Unit = {},
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    var tapFocusPoint by remember { mutableStateOf<Offset?>(null) }
    var showCenterAfPulse by remember { mutableStateOf(false) }
    var activeTextureView by remember { mutableStateOf<TextureView?>(null) }
    var activeSurface by remember { mutableStateOf<Surface?>(null) }

    LaunchedEffect(tapFocusPoint) {
        if (tapFocusPoint != null) {
            delay(1000L)
            tapFocusPoint = null
        }
    }

    LaunchedEffect(afTriggerPulseSeq) {
        if (afTriggerPulseSeq > 0L) {
            showCenterAfPulse = true
            delay(950L)
            showCenterAfPulse = false
        }
    }

    DisposableEffect(context) {
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager
        val listener = object : DisplayManager.DisplayListener {
            override fun onDisplayAdded(displayId: Int) {}
            override fun onDisplayRemoved(displayId: Int) {}
            override fun onDisplayChanged(displayId: Int) {
                val tv = activeTextureView ?: return
                val surf = activeSurface ?: return
                if (tv.isAvailable && surf.isValid && tv.width > 0 && tv.height > 0) {
                    val rot = tv.display?.rotation ?: Surface.ROTATION_0
                    onSurfaceChanged(surf, tv.width, tv.height, rot)
                }
            }
        }
        dm?.registerDisplayListener(listener, null)
        onDispose {
            dm?.unregisterDisplayListener(listener)
        }
    }

    BoxWithConstraints(
        modifier = modifier
            .fillMaxSize()
            .background(OledPureBlack),
        contentAlignment = Alignment.Center
    ) {
        if (hasCameraPermission) {
            val isLandscape = maxWidth > maxHeight
            val streamAspect = if (streamHeight > 0) streamWidth.toFloat() / streamHeight.toFloat() else (16f / 9f)
            val targetAspect = if (isLandscape) streamAspect else (1.0f / streamAspect)

            var viewW = maxWidth
            var viewH = maxWidth / targetAspect
            if (viewH > maxHeight) {
                viewH = maxHeight
                viewW = viewH * targetAspect
            }

            Box(
                modifier = Modifier
                    .size(width = viewW, height = viewH)
                    .clip(RoundedCornerShape(8.dp))
                    .pointerInput(Unit) {
                        detectTapGestures(
                            onTap = { offset ->
                                tapFocusPoint = offset
                                val xNorm = (offset.x / size.width).coerceIn(0f, 1f)
                                val yNorm = (offset.y / size.height).coerceIn(0f, 1f)
                                onFocusPointTapped?.invoke(xNorm, yNorm)
                            }
                        )
                    },
                contentAlignment = Alignment.Center
            ) {
                AndroidView(
                    factory = { ctx ->
                        TextureView(ctx).apply {
                            keepScreenOn = true
                            isOpaque = true
                            activeTextureView = this
                            surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                                override fun onSurfaceTextureAvailable(st: SurfaceTexture, width: Int, height: Int) {
                                    activeSurface?.release()
                                    val surf = Surface(st)
                                    activeSurface = surf
                                    val rot = display?.rotation ?: Surface.ROTATION_0
                                    onSurfaceChanged(surf, width, height, rot)
                                }

                                override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, width: Int, height: Int) {
                                    val rot = display?.rotation ?: Surface.ROTATION_0
                                    onSurfaceChanged(activeSurface, width, height, rot)
                                }

                                override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                                    onSurfaceChanged(null, 0, 0, Surface.ROTATION_0)
                                    activeSurface?.release()
                                    activeSurface = null
                                    return true
                                }

                                override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
                            }
                        }
                    },
                    update = { tv ->
                        activeTextureView = tv
                    },
                    modifier = Modifier.fillMaxSize()
                )

                if (!isDisplayProtectionActive) {
                    Box(
                        modifier = Modifier
                            .fillMaxSize()
                            .border(1.dp, OledBorder.copy(alpha = 0.4f), RoundedCornerShape(8.dp))
                    )

                    Box(
                        modifier = Modifier
                            .align(Alignment.TopStart)
                            .padding(12.dp)
                            .clip(RoundedCornerShape(12.dp))
                            .background(OledPillBackground.copy(alpha = 0.85f))
                            .border(1.dp, OledBorder, RoundedCornerShape(12.dp))
                            .padding(horizontal = 10.dp, vertical = 4.dp)
                    ) {
                        val resLabel = "${streamWidth}x${streamHeight}"
                        val isUa = language == AppLanguage.UA
                        Text(
                            text = if (isLandscape) {
                                if (isUa) "📷 16:9 ВЕБ-КАМЕРА [$resLabel]" else "📷 16:9 WEBCAM [$resLabel]"
                            } else {
                                if (isUa) "📷 9:16 ПОРТРЕТ [$resLabel]" else "📷 9:16 PORTRAIT [$resLabel]"
                            },
                            fontSize = 10.sp,
                            fontWeight = FontWeight.Bold,
                            color = StatusCyan,
                            fontFamily = FontFamily.Monospace
                        )
                    }

                    val pt = tapFocusPoint
                    if (pt != null) {
                        Box(
                            modifier = Modifier
                                .offset { IntOffset((pt.x - 26.dp.toPx()).toInt(), (pt.y - 26.dp.toPx()).toInt()) }
                                .size(52.dp)
                                .clip(CircleShape)
                                .background(StatusCyan.copy(alpha = 0.14f))
                                .border(2.dp, StatusCyan, CircleShape),
                            contentAlignment = Alignment.Center
                        ) {
                            Icon(
                                imageVector = Icons.Default.CenterFocusStrong,
                                contentDescription = "Focusing",
                                tint = StatusCyan,
                                modifier = Modifier.size(22.dp)
                            )
                        }
                    } else if (showCenterAfPulse) {
                        Box(
                            modifier = Modifier
                                .align(Alignment.Center)
                                .size(64.dp)
                                .clip(CircleShape)
                                .background(StatusEmerald.copy(alpha = 0.18f))
                                .border(2.dp, StatusEmerald, CircleShape),
                            contentAlignment = Alignment.Center
                        ) {
                            Icon(
                                imageVector = Icons.Default.CenterFocusStrong,
                                contentDescription = "AF Triggered",
                                tint = StatusEmerald,
                                modifier = Modifier.size(28.dp)
                            )
                        }
                    }
                }
            }
        } else {
            Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center
            ) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    val isUa = language == AppLanguage.UA
                    Text(
                        text = if (isUa) "Потрібен доступ до камери" else "Camera permission is required",
                        color = TextDimmed,
                        fontSize = 15.sp
                    )
                    Box(
                        modifier = Modifier
                            .clip(RoundedCornerShape(20.dp))
                            .background(StatusEmerald)
                            .clickable { onRequestPermissions() }
                            .padding(horizontal = 20.dp, vertical = 10.dp)
                    ) {
                        Text(
                            text = if (isUa) "Надати дозвіл на камеру" else "Grant Camera Permission",
                            color = Color.Black,
                            fontWeight = FontWeight.Bold,
                            fontSize = 13.sp
                        )
                    }
                }
            }
        }
    }
}
