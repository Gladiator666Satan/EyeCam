package com.eyecam.ui

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.eyecam.ui.theme.EyeCamTheme
import com.eyecam.ui.theme.OledPureBlack
import com.eyecam.viewmodel.StreamViewModel

/**
 * Головна Activity додатку EyeCam.
 * Відповідає за:
 * - Запит дозволів CAMERA, RECORD_AUDIO та POST_NOTIFICATIONS при запуску (запобігає падінню на Android 14/15/16).
 * - Повноекранний Immersive режим.
 * - Утримання екрана активним під час стрімінгу (FLAG_KEEP_SCREEN_ON).
 */
class MainActivity : ComponentActivity() {

    private val viewModel: StreamViewModel by viewModels()

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        val cameraGranted = results[Manifest.permission.CAMERA] == true ||
            ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        if (cameraGranted) {
            viewModel.onPermissionsGranted()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
            setShowWhenLocked(true)
            setTurnScreenOn(true)
        } else {
            @Suppress("DEPRECATION")
            window.addFlags(
                WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED or
                WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON
            )
        }
        configureImmersiveMode()
        requestRequiredPermissionsIfNeeded()

        setContent {
            EyeCamTheme {
                val uiState by viewModel.uiState.collectAsState()

                LaunchedEffect(uiState.isDisplayProtectionActive) {
                    applyDisplayBrightness(uiState.isDisplayProtectionActive)
                }

                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = OledPureBlack
                ) {
                    EyeCamScreen(
                        viewModel = viewModel,
                        onRequestPermissions = { requestRequiredPermissionsIfNeeded() }
                    )
                }
            }
        }
    }

    private fun requestRequiredPermissionsIfNeeded() {
        val perms = mutableListOf(
            Manifest.permission.CAMERA,
            Manifest.permission.RECORD_AUDIO
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            perms.add(Manifest.permission.POST_NOTIFICATIONS)
        }

        val missing = perms.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            permissionLauncher.launch(missing.toTypedArray())
        } else {
            viewModel.onPermissionsGranted()
        }
    }

    private fun applyDisplayBrightness(isDimmed: Boolean) {
        val layoutParams = window.attributes
        layoutParams.screenBrightness = if (isDimmed) {
            0.01f
        } else {
            WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
        window.attributes = layoutParams
    }

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
