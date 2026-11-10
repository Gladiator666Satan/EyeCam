package com.eyecam.ui.components

import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.eyecam.model.StreamUiState
import com.eyecam.viewmodel.StreamViewModel

@Composable
fun CameraPreview(
    viewModel: StreamViewModel,
    uiState: StreamUiState,
    modifier: Modifier = Modifier
) {
    if (uiState.isDisplayProtectionActive) {
        // OLED Screen Saver Mode (deep black with dim clock/status)
        Box(
            modifier = modifier
                .fillMaxSize()
                .background(Color.Black),
            contentAlignment = Alignment.Center
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(
                    text = "🔒 OLED Захист дисплея",
                    color = Color.DarkGray,
                    fontSize = 16.sp
                )
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = "Трансляція триває у фоні. Торкніться двічі для розблокування.",
                    color = Color(0xFF333333),
                    fontSize = 12.sp
                )
            }
        }
        return
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(Color.Black)
            .pointerInput(Unit) {
                detectTapGestures { offset ->
                    val normX = offset.x / size.width.toFloat()
                    val normY = offset.y / size.height.toFloat()
                    viewModel.triggerFocusAtPoint(normX, normY)
                }
            }
    ) {
        AndroidView(
            factory = { context ->
                SurfaceView(context).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            val bufW = uiState.telemetry.resolutionWidth.coerceAtLeast(1920)
                            val bufH = uiState.telemetry.resolutionHeight.coerceAtLeast(1080)
                            holder.setFixedSize(bufW, bufH)
                            viewModel.attachPreviewSurface(holder.surface)
                        }

                        override fun surfaceChanged(
                            holder: SurfaceHolder,
                            format: Int,
                            width: Int,
                            height: Int
                        ) {
                            val bufW = uiState.telemetry.resolutionWidth.coerceAtLeast(1920)
                            val bufH = uiState.telemetry.resolutionHeight.coerceAtLeast(1080)
                            holder.setFixedSize(bufW, bufH)
                        }

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            viewModel.attachPreviewSurface(null)
                        }
                    })
                }
            },
            modifier = Modifier.fillMaxSize()
        )
    }
}
