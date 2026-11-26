package com.plyg.airlink

import android.content.ClipboardManager
import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import kotlin.math.abs

/**
 * "Fling to send" (phase 4B): a quick push of the phone away from the user, toward the computer,
 * sends what is waiting (shared files, or the clipboard's text) to the last used computer.
 *
 * The phone is held facing the user, so the push is along the screen's normal, away from the
 * user: device -Z. Linear acceleration (gravity removed) shows a sharp -Z peak, then the stop's
 * +Z peak within a short time, while X and Y stay small. Listens while AirLink's foreground
 * service runs: with AirLink on screen it sends the shared files or the clipboard, with another
 * app on screen what that app shows (ForegroundContent).
 */
object FlingDetector : SensorEventListener {
    private const val PREFS = "airlink"
    private const val KEY = "fling_enabled"
    private const val PUSH = 11f          // m/s², the push's peak (away from the user)
    private const val STOP = 7f           // m/s², the stop's peak (opposite)
    private const val SIDEWAYS = 0.6f     // X/Y may be at most this share of the Z peak
    private const val WINDOW_MS = 450L    // push to stop
    private const val COOLDOWN_MS = 2000L

    private var manager: SensorManager? = null
    private var listening = false
    private var pushAt = 0L
    private var lastFling = 0L
    private var context: Context? = null

    @JvmStatic
    fun available(context: Context): Boolean =
        context.getSystemService(SensorManager::class.java)?.getDefaultSensor(Sensor.TYPE_LINEAR_ACCELERATION) != null

    @JvmStatic
    fun enabled(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY, true)

    @JvmStatic
    fun setEnabled(context: Context, on: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(KEY, on).apply()
        if (on) resume(context) else pause()
    }

    /** AirLink's service runs (the detector keeps the application context). */
    @JvmStatic
    fun resume(context: Context) {
        if (listening || !enabled(context)) return
        @Suppress("NAME_SHADOWING") val context = context.applicationContext
        val sensors = context.getSystemService(SensorManager::class.java) ?: return
        val sensor = sensors.getDefaultSensor(Sensor.TYPE_LINEAR_ACCELERATION) ?: return
        this.context = context
        manager = sensors
        listening = sensors.registerListener(this, sensor, SensorManager.SENSOR_DELAY_GAME)
    }

    @JvmStatic
    fun pause() {
        if (listening) manager?.unregisterListener(this)
        listening = false
        pushAt = 0L
        context = null
    }

    override fun onSensorChanged(event: SensorEvent) {
        val (x, y, z) = Triple(event.values[0], event.values[1], event.values[2])
        val now = SystemClock.elapsedRealtime()
        if (now - lastFling < COOLDOWN_MS) return
        val sideways = maxOf(abs(x), abs(y))
        if (z < -PUSH && sideways < SIDEWAYS * abs(z)) {
            pushAt = now
        } else if (pushAt != 0L && z > STOP && now - pushAt <= WINDOW_MS) {
            pushAt = 0L
            lastFling = now
            flung()
        } else if (pushAt != 0L && now - pushAt > WINDOW_MS) {
            pushAt = 0L
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    private fun flung() {
        val context = context ?: return
        vibrate(context)
        if (!MainActivity.visible) {
            // Another app on screen: what it shows (Android keeps the clipboard from the background).
            if (MainActivity.nativeReady) ForegroundContent.fling(context)
            return
        }
        val text = try {
            context.getSystemService(ClipboardManager::class.java)?.primaryClip
                ?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(context)?.toString().orEmpty()
        } catch (_: Exception) { "" }
        if (MainActivity.nativeReady) nativeFlung(text)
    }

    @Suppress("DEPRECATION")
    private fun vibrate(context: Context) {
        val vibrator = context.getSystemService(Vibrator::class.java) ?: return
        if (Build.VERSION.SDK_INT >= 26) vibrator.vibrate(VibrationEffect.createOneShot(40, VibrationEffect.DEFAULT_AMPLITUDE))
        else vibrator.vibrate(40)
    }

    @JvmStatic private external fun nativeFlung(clipboardText: String)
}
