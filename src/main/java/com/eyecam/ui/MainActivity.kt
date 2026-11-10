package com.eyecam.ui

import android.Manifest
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.hardware.SensorManager
import android.os.Build
import android.os.Bundle
import android.view.OrientationEventListener
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
    private var orientationListener: OrientationEventListener? = null

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
        setupOrientationListener()

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

    private fun setupOrientationListener() {
        orientationListener = object : OrientationEventListener(this, SensorManager.SENSOR_DELAY_NORMAL) {
            override fun onOrientationChanged(orientation: Int) {
                if (orientation == ORIENTATION_UNKNOWN) return
                // Автоматичне динамічне перемикання орієнтації (навіть при заблокованому автоповороті в системі)
                val targetOrientation = when (orientation) {
                    in 60..120 -> ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE
                    in 240..300 -> ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
                    in 330..360, in 0..30 -> ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                    in 150..210 -> ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT
                    else -> null
                }
                if (targetOrientation != null && requestedOrientation != targetOrientation) {
                    requestedOrientation = targetOrientation
                }
            }
        }
        if (orientationListener?.canDetectOrientation() == true) {
            orientationListener?.enable()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        orientationListener?.disable()
    }
}
