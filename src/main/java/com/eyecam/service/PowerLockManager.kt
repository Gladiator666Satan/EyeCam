package com.eyecam.service

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.os.PowerManager
import android.util.Log

/**
 * Керує системними блокуваннями енергозбереження (WakeLock та WifiLock)
 * для запобігання переходу CPU та мережевого адаптера в режим сну під час трансляції.
 */
class PowerLockManager(private val context: Context) {

    private val powerManager = context.getSystemService(Context.POWER_SERVICE) as? PowerManager
    private val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager

    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    companion object {
        private const val TAG = "PowerLockManager"
        private const val WAKE_LOCK_TAG = "EyeCam:StreamWakeLock"
        private const val WIFI_LOCK_TAG = "EyeCam:StreamWifiLock"
    }

    /**
     * Захоплює PARTIAL_WAKE_LOCK для підтримання роботи CPU при вимкненому/затемненому екрані.
     */
    @Synchronized
    fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) {
            Log.d(TAG, "WakeLock вже утримується")
            return
        }

        try {
            wakeLock = powerManager?.newWakeLock(
                PowerManager.PARTIAL_WAKE_LOCK,
                WAKE_LOCK_TAG
            )?.apply {
                setReferenceCounted(false)
                acquire()
            }
            Log.i(TAG, "WakeLock успішно захоплено: $WAKE_LOCK_TAG")
        } catch (e: Exception) {
            Log.e(TAG, "Помилка захоплення WakeLock: ${e.message}", e)
        }
    }

    /**
     * Захоплює WifiLock у режимі низької затримки (WIFI_MODE_FULL_LOW_LATENCY для API 29+)
     * для мінімізації джиттеру та затримок пакетів відеотрансляції.
     */
    @Synchronized
    fun acquireWifiLock() {
        if (wifiLock?.isHeld == true) {
            Log.d(TAG, "WifiLock вже утримується")
            return
        }

        try {
            val lockMode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            } else {
                @Suppress("DEPRECATION")
                WifiManager.WIFI_MODE_FULL_HIGH_PERF
            }

            wifiLock = wifiManager?.createWifiLock(lockMode, WIFI_LOCK_TAG)?.apply {
                setReferenceCounted(false)
                acquire()
            }
            Log.i(TAG, "WifiLock успішно захоплено з режимом $lockMode")
        } catch (e: Exception) {
            Log.e(TAG, "Помилка захоплення WifiLock: ${e.message}", e)
        }
    }

    /**
     * Безпечно звільняє всі системні блокування при зупинці трансляції або знищенні сервісу.
     */
    @Synchronized
    fun releaseAll() {
        try {
            wakeLock?.let {
                if (it.isHeld) {
                    it.release()
                    Log.i(TAG, "WakeLock успішно звільнено")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Помилка звільнення WakeLock: ${e.message}", e)
        } finally {
            wakeLock = null
        }

        try {
            wifiLock?.let {
                if (it.isHeld) {
                    it.release()
                    Log.i(TAG, "WifiLock успішно звільнено")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Помилка звільнення WifiLock: ${e.message}", e)
        } finally {
            wifiLock = null
        }
    }
}
