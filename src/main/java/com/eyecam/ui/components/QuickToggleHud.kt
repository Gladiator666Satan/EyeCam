package com.eyecam.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.eyecam.model.StreamUiState

@Composable
fun QuickToggleHud(
    uiState: StreamUiState,
    onToggleStream: () -> Unit,
    onToggleTorch: () -> Unit,
    onToggleMic: () -> Unit,
    onSwitchCamera: () -> Unit,
    onSelectZoom: (Float) -> Unit,
    onRefocus: () -> Unit,
    onToggleOledProtection: () -> Unit,
    modifier: Modifier = Modifier
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        // Zoom Preset Chips
        Row(
            modifier = Modifier
                .clip(RoundedCornerShape(20.dp))
                .background(Color(0x991E1E1E))
                .padding(horizontal = 8.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            uiState.supportedZoomPresets.forEach { zoom ->
                val isSelected = (uiState.currentZoomRatio - zoom).let { Math.abs(it) < 0.05f }
                Box(
                    modifier = Modifier
                        .clip(CircleShape)
                        .background(if (isSelected) Color(0xFF0078D4) else Color.Transparent)
                        .clickable { onSelectZoom(zoom) }
                        .padding(horizontal = 10.dp, vertical = 6.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = if (zoom < 1.0f) "${zoom}x" else "${zoom.toInt()}x",
                        color = if (isSelected) Color.White else Color(0xFFCCCCCC),
                        fontSize = 12.sp,
                        fontWeight = if (isSelected) FontWeight.Bold else FontWeight.Normal
                    )
                }
            }
        }

        Spacer(modifier = Modifier.height(16.dp))

        // Main Controls Row
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(32.dp))
                .background(Color(0xCC1A1A1A))
                .padding(horizontal = 16.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Torch Button
            HudIconButton(
                icon = if (uiState.isTorchOn) Icons.Default.FlashOn else Icons.Default.FlashOff,
                isActive = uiState.isTorchOn,
                onClick = onToggleTorch
            )

            // Refocus Button
            HudIconButton(
                icon = Icons.Default.CenterFocusStrong,
                isActive = false,
                onClick = onRefocus
            )

            // Primary Stream (Play / Stop) Button
            Box(
                modifier = Modifier
                    .size(56.dp)
                    .clip(CircleShape)
                    .background(if (uiState.isStreaming) Color(0xFFD83B01) else Color(0xFF0078D4))
                    .clickable { onToggleStream() },
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    imageVector = if (uiState.isStreaming) Icons.Default.Stop else Icons.Default.PlayArrow,
                    contentDescription = if (uiState.isStreaming) "Зупинити" else "Запустити",
                    tint = Color.White,
                    modifier = Modifier.size(32.dp)
                )
            }

            // Switch Camera Button
            HudIconButton(
                icon = Icons.Default.FlipCameraAndroid,
                isActive = false,
                onClick = onSwitchCamera
            )

            // OLED Protection Button
            HudIconButton(
                icon = Icons.Default.Lock,
                isActive = uiState.isDisplayProtectionActive,
                onClick = onToggleOledProtection
            )
        }
    }
}

@Composable
fun HudIconButton(
    icon: ImageVector,
    isActive: Boolean,
    onClick: () -> Unit
) {
    Box(
        modifier = Modifier
            .size(44.dp)
            .clip(CircleShape)
            .background(if (isActive) Color(0xFF333333) else Color(0x33FFFFFF))
            .clickable { onClick() },
        contentAlignment = Alignment.Center
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = if (isActive) Color(0xFF0078D4) else Color.White,
            modifier = Modifier.size(22.dp)
        )
    }
}
