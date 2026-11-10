package com.eyecam.ui.components

import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
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
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.eyecam.ui.theme.OledBorder
import com.eyecam.ui.theme.OledPillBackground
import com.eyecam.ui.theme.OledPureBlack
import com.eyecam.ui.theme.StatusCyan
import com.eyecam.ui.theme.StatusEmerald
import com.eyecam.ui.theme.TextDimmed
import kotlinx.coroutines.delay

/**
 * Професійний в'юпорт попереднього перегляду камери (Camera2 SurfaceView):
 * - Суворе дотримання пропорцій сенсора без розтягування зображення у вертикальному та горизонтальному режимах.
 * - Фіксація точного розміру буфера через SurfaceHolder.setFixedSize.
 * - Утримання SurfaceView активним у дереві Compose для безперервної фонової роботи та захисту від сну.
 * - Сенсорний фокус (Tap-to-Focus) з візуальною міткою наведення різкості.
 * - Рамка візирного кадру та бейдж поточної орієнтації (16:9 / 9:16).
 */
@Composable
fun CameraPreview(
    isStreaming: Boolean,
    hasCameraPermission: Boolean = true,
    isDisplayProtectionActive: Boolean = false,
    streamWidth: Int = 1920,
    streamHeight: Int = 1080,
    onSurfaceChanged: (Surface?) -> Unit = {},
    onFocusPointTapped: ((Float, Float) -> Unit)? = null,
    onRequestPermissions: () -> Unit = {},
    modifier: Modifier = Modifier
) {
    var tapFocusPoint by remember { mutableStateOf<Offset?>(null) }

    LaunchedEffect(tapFocusPoint) {
        if (tapFocusPoint != null) {
            delay(1200L)
            tapFocusPoint = null
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

            // Точний розрахунок пропорцій для усунення розтягування (Aspect Fit з Letterboxing)
            val streamAspect = if (streamHeight > 0) streamWidth.toFloat() / streamHeight.toFloat() else (16f / 9f)
            val targetAspect = if (isLandscape) streamAspect else (1.0f / streamAspect)

            var viewW = maxWidth
            var viewH = maxWidth / targetAspect
            if (viewH > maxHeight) {
                viewH = maxHeight
                viewW = viewH * targetAspect
            }

            val bufW = if (isLandscape) streamWidth else streamHeight
            val bufH = if (isLandscape) streamHeight else streamWidth

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
                // SurfaceView ЗАВЖДИ утримується в ієрархії Compose, що гарантує
                // безперебійну роботу камери у фоні та при заблокованому екрані
                AndroidView(
                    factory = { context ->
                        SurfaceView(context).apply {
                            holder.setKeepScreenOn(true)
                            holder.setFixedSize(bufW, bufH)
                            holder.addCallback(object : SurfaceHolder.Callback {
                                override fun surfaceCreated(holder: SurfaceHolder) {
                                    holder.setFixedSize(bufW, bufH)
                                    onSurfaceChanged(holder.surface)
                                }

                                override fun surfaceChanged(
                                    holder: SurfaceHolder,
                                    format: Int,
                                    width: Int,
                                    height: Int
                                ) {
                                    onSurfaceChanged(holder.surface)
                                }

                                override fun surfaceDestroyed(holder: SurfaceHolder) {
                                    onSurfaceChanged(null)
                                }
                            })
                        }
                    },
                    update = { surfaceView ->
                        surfaceView.holder.setFixedSize(bufW, bufH)
                    },
                    modifier = Modifier.fillMaxSize()
                )

                // Візирна рамка та індикатор формату зйомки
                if (!isDisplayProtectionActive) {
                    Box(
                        modifier = Modifier
                            .fillMaxSize()
                            .border(1.dp, OledBorder.copy(alpha = 0.4f), RoundedCornerShape(8.dp))
                    )

                    // Бейдж співвідношення сторін кадру
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
                        Text(
                            text = if (isLandscape) "📷 16:9 ВЕБ-КАМЕРА [$resLabel]" else "📷 9:16 ВЕРТИКАЛЬНО [$resLabel]",
                            fontSize = 10.sp,
                            fontWeight = FontWeight.Bold,
                            color = StatusCyan,
                            fontFamily = FontFamily.Monospace
                        )
                    }

                    // Анімація мітки перефокусування при тапі по екрану
                    tapFocusPoint?.let { pt ->
                        Box(
                            modifier = Modifier
                                .offset { IntOffset((pt.x - 24.dp.toPx()).toInt(), (pt.y - 24.dp.toPx()).toInt()) }
                            .size(48.dp)
                            .clip(CircleShape)
                            .border(2.dp, StatusCyan, CircleShape),
                            contentAlignment = Alignment.Center
                        ) {
                            Icon(
                                imageVector = Icons.Default.CenterFocusStrong,
                                contentDescription = "Focusing",
                                tint = StatusCyan,
                                modifier = Modifier.size(20.dp)
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
                    Text(
                        text = "Потрібен доступ до камери",
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
                            text = "Надати дозвіл на камеру",
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
