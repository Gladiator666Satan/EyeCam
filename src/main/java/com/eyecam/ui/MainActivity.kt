package com.eyecam.ui

import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.eyecam.ui.theme.EyeCamTheme
import com.eyecam.ui.theme.OledPureBlack
import com.eyecam.viewmodel.StreamViewModel

/**
 * Головна Activity додатку EyeCam.
 * Відповідає за:
 * - Повноекранний Immersive режим (приховування системних панелей).
 * - Утримання екрана активним під час стрімінгу (FLAG_KEEP_SCREEN_ON).
 * - Апаратне регулювання яскравості екрана для режиму Display Protection Mode (OLED Blackout).
 */
class MainActivity : ComponentActivity() {

    private val viewModel: StreamViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        // Утримуємо екран активним, щоб система не блокувала пристрій під час відеосесії
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // Приховуємо навігаційний та статус-бари для імерсивного OLED-досвіду
        configureImmersiveMode()

        setContent {
            EyeCamTheme {
                val uiState by viewModel.uiState.collectAsState()

                // Апаратне керування яскравістю під час переходу в режим захисту
                LaunchedEffect(uiState.isDisplayProtectionActive) {
                    applyDisplayBrightness(uiState.isDisplayProtectionActive)
                }

                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = OledPureBlack
                ) {
                    EyeCamScreen(viewModel = viewModel)
                }
            }
        }
    }

    /**
     * Динамічне регулювання яскравості підсвітки екрана:
     * Коли активний захист — знижуємо яскравість до абсолютного мінімуму (0.01f),
     * що суттєво мінімізує тепловиділення та розряд акумулятора.
     */
    private fun applyDisplayBrightness(isDimmed: Boolean) {
        val layoutParams = window.attributes
        layoutParams.screenBrightness = if (isDimmed) {
            0.01f // Мінімальна апаратна яскравість
        } else {
            WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE // Повернення системної яскравості
        }
        window.attributes = layoutParams
    }

    /**
     * Конфігурація повноекранного режиму з приховуванням жестів та барів.
     */
    private fun configureImmersiveMode() {
        val windowInsetsController = WindowCompat.getInsetsController(window, window.decorView)
        windowInsetsController.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        windowInsetsController.hide(WindowInsetsCompat.Type.systemBars())
    }

    override fun onUserInteraction() {
        super.onUserInteraction()
        viewModel.onUserInteraction()
    }
}
