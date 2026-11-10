package com.eyecam.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val OledDarkColorScheme = darkColorScheme(
    primary = StatusEmerald,
    secondary = StatusCyan,
    background = OledPureBlack,
    surface = OledSurface,
    onPrimary = OledPureBlack,
    onSecondary = OledPureBlack,
    onBackground = TextPrimary,
    onSurface = TextPrimary
)

@Composable
fun EyeCamTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = OledDarkColorScheme,
        content = content
    )
}
