package com.eyecam.service

import android.content.Context
import android.os.Build
import android.os.PowerManager
import android.util.Log
import com.eyecam.model.ThermalState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Конфігурація обмежень кодування при термальному тротлінгу.
 */
data class ThermalThrottlingPolicy(
    val recommendedFps: Int,
    val bitrateReductionRatio: Float, // 1.0f = 100% (без змін), 0.5f = 50% бітрейту
    val shouldEnforceDisplayProtection: Boolean,
    val notifyDesktopClient: Boolean,
    val alertMessage: String
)

/**
 * Слухач сповіщень десктопного клієнта про зміни термального статусу.
 */
fun interface DesktopThermalNotifier {
    fun onThermalThrottlingTriggered(
        thermalState: ThermalState,
        adjustedFps: Int,
        adjustedBitrateKbps: Int,
        reason: String
    )
}

/**
 * Монітор температури та тротлінгу пристрою на базі PowerManager.OnThermalStatusChangedListener (API 29+).
 * Автоматично адаптує бітрейт та частоту кадрів для запобігання перегріву та аварійного вимкнення пристрою.
 */
class ThermalManager(
    context: Context,
    private val desktopNotifier: DesktopThermalNotifier? = null
) {

    private val powerManager = context.getSystemService(Context.POWER_SERVICE) as? PowerManager

    private val _thermalState = MutableStateFlow(ThermalState.NORMAL)
    val thermalState: StateFlow<ThermalState> = _thermalState.asStateFlow()

    private var thermalListener: PowerManager.OnThermalStatusChangedListener? = null

    companion object {
        private const val TAG = "ThermalManager"
        const val BASELINE_FPS = 60
        const val BASELINE_BITRATE_KBPS = 12000
    }

    /**
     * Запуск моніторингу термального стану пристрою.
     */
    fun startMonitoring() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val listener = PowerManager.OnThermalStatusChangedListener { status ->
                handleThermalStatusChange(status)
            }
            thermalListener = listener

            try {
                powerManager?.addThermalStatusListener(listener)
                Log.i(TAG, "OnThermalStatusChangedListener успішно зареєстровано")
                
                // Перевірка початкового стану
                powerManager?.currentThermalStatus?.let { handleThermalStatusChange(it) }
            } catch (e: Exception) {
                Log.e(TAG, "Помилка реєстрації thermal listener: ${e.message}", e)
            }
        } else {
            Log.w(TAG, "Thermal Status API не підтримується на версіях нижче Android 10 (Q)")
            _thermalState.value = ThermalState.NORMAL
        }
    }

    /**
     * Зупинка моніторингу термального стану та звільнення системного лістенера.
     */
    fun stopMonitoring() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && thermalListener != null) {
            try {
                powerManager?.removeThermalStatusListener(thermalListener!!)
                Log.i(TAG, "OnThermalStatusChangedListener від'єднано")
            } catch (e: Exception) {
                Log.e(TAG, "Помилка від'єднання thermal listener: ${e.message}", e)
            } finally {
                thermalListener = null
            }
        }
    }

    /**
     * Обробка зміни системного статусу та застосування алгоритмів пом'якшення нагріву.
     */
    private fun handleThermalStatusChange(rawStatus: Int) {
        val newState = when (rawStatus) {
            PowerManager.THERMAL_STATUS_NONE -> ThermalState.NORMAL
            PowerManager.THERMAL_STATUS_LIGHT -> ThermalState.LIGHT
            PowerManager.THERMAL_STATUS_MODERATE -> ThermalState.MODERATE
            PowerManager.THERMAL_STATUS_SEVERE -> ThermalState.SEVERE
            PowerManager.THERMAL_STATUS_CRITICAL,
            PowerManager.THERMAL_STATUS_EMERGENCY,
            PowerManager.THERMAL_STATUS_SHUTDOWN -> ThermalState.CRITICAL
            else -> ThermalState.UNKNOWN
        }

        val previousState = _thermalState.value
        _thermalState.value = newState

        if (previousState != newState) {
            Log.w(TAG, "Термальний статус змінився: $previousState -> $newState (код: $rawStatus)")
            applyMitigationPolicy(newState)
        }
    }

    /**
     * Розрахунок та застосування адаптивних параметрів потоку.
     */
    private fun applyMitigationPolicy(state: ThermalState) {
        val policy = getPolicyForState(state)

        val targetFps = policy.recommendedFps
        val targetBitrateKbps = (BASELINE_BITRATE_KBPS * policy.bitrateReductionRatio).toInt()

        Log.i(
            TAG,
            "Застосовано політику охолодження: FPS=$targetFps, Bitrate=${targetBitrateKbps}Kbps, " +
                    "Обов'язковий Blackout=${policy.shouldEnforceDisplayProtection}"
        )

        if (policy.notifyDesktopClient) {
            desktopNotifier?.onThermalThrottlingTriggered(
                thermalState = state,
                adjustedFps = targetFps,
                adjustedBitrateKbps = targetBitrateKbps,
                reason = policy.alertMessage
            )
        }
    }

    /**
     * Визначає правила тротлінгу відповідно до тяжкості перегріву.
     */
    fun getPolicyForState(state: ThermalState): ThermalThrottlingPolicy {
        return when (state) {
            ThermalState.NORMAL -> ThermalThrottlingPolicy(
                recommendedFps = 60,
                bitrateReductionRatio = 1.0f,
                shouldEnforceDisplayProtection = false,
                notifyDesktopClient = false,
                alertMessage = "Температура в нормі. Відновлено максимальні параметри 60 FPS."
            )
            ThermalState.LIGHT -> ThermalThrottlingPolicy(
                recommendedFps = 60,
                bitrateReductionRatio = 0.9f,
                shouldEnforceDisplayProtection = false,
                notifyDesktopClient = false,
                alertMessage = "Легке потепління сенсора. Бітрейт оптимізовано до 90%."
            )
            ThermalState.MODERATE -> ThermalThrottlingPolicy(
                recommendedFps = 45,
                bitrateReductionRatio = 0.75f,
                shouldEnforceDisplayProtection = true,
                notifyDesktopClient = true,
                alertMessage = "Попередження перегріву: FPS знижено до 45, бітрейт на 25%."
            )
            ThermalState.SEVERE -> ThermalThrottlingPolicy(
                recommendedFps = 30,
                bitrateReductionRatio = 0.5f,
                shouldEnforceDisplayProtection = true,
                notifyDesktopClient = true,
                alertMessage = "Критичний тротлінг: FPS обмежено до 30, бітрейт скорочено вдвічі для захисту сенсора!"
            )
            ThermalState.CRITICAL,
            ThermalState.UNKNOWN -> ThermalThrottlingPolicy(
                recommendedFps = 24,
                bitrateReductionRatio = 0.25f,
                shouldEnforceDisplayProtection = true,
                notifyDesktopClient = true,
                alertMessage = "Аварійний стан! Максимальне зниження навантаження (24 FPS, 25% бітрейту)!"
            )
        }
    }
}
