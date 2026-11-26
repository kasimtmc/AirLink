package com.plyg.airlink

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.Ringtone
import android.media.RingtoneManager
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import org.json.JSONObject

/**
 * The phone's battery for paired computers (`phone.battery`: level and charging, sent as either
 * changes and on `phone.battery_request`), and "find my phone": `phone.find` rings the alarm sound
 * at full alarm volume (through silent mode) and vibrates until `phone.find_stop`, the
 * notification's "Buldum", or a minute; `phone.find_state` tells the computers.
 */
object PhoneStatus {
    private const val CHANNEL_FIND = "find"
    private const val FIND_ID = 4950
    private const val RING_MS = 60_000L
    const val ACTION_FIND_STOP = "com.plyg.airlink.FIND_STOP"

    private val main = Handler(Looper.getMainLooper())
    private var context: Context? = null
    private var level = -1
    private var charging = false

    private var ringtone: Ringtone? = null
    private var savedAlarmVolume = -1
    private val timeout = Runnable { stopRinging() }

    private val batteryReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) = update(intent)
    }

    /** The service runs: follow the battery. On the main thread. */
    @JvmStatic
    fun start(context: Context) {
        this.context = context.applicationContext
        val sticky = context.applicationContext.registerReceiver(batteryReceiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        if (sticky != null) update(sticky)
    }

    @JvmStatic
    fun stop() {
        val ctx = context ?: return
        try { ctx.unregisterReceiver(batteryReceiver) } catch (_: IllegalArgumentException) {}
        stopRinging()
    }

    private fun update(intent: Intent) {
        val raw = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100).coerceAtLeast(1)
        val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
        val newLevel = if (raw < 0) -1 else raw * 100 / scale
        val newCharging = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL
        if (newLevel == level && newCharging == charging) return  // (it comes for temperature, voltage...)
        level = newLevel
        charging = newCharging
        SmsBridge.broadcastAll("phone.battery", battery().toString())
    }

    private fun battery() = JSONObject().put("level", level).put("charging", charging)

    /** A computer's `phone.*` request (on the bridge's worker thread). */
    fun handle(peerId: String, kind: String) = main.post {
        when (kind) {
            "phone.battery_request" -> if (level >= 0) SmsBridge.send(peerId, "phone.battery", battery())
            "phone.find" -> ring()
            "phone.find_stop" -> stopRinging()
        }
    }

    private fun ring() {
        val ctx = context ?: return
        if (ringtone?.isPlaying == true) return
        val audio = ctx.getSystemService(AudioManager::class.java)
        if (audio != null) try {
            savedAlarmVolume = audio.getStreamVolume(AudioManager.STREAM_ALARM)
            audio.setStreamVolume(AudioManager.STREAM_ALARM, audio.getStreamMaxVolume(AudioManager.STREAM_ALARM), 0)
        } catch (e: SecurityException) {
            savedAlarmVolume = -1
            Log.w("AirLink", "find volume: $e")
        }
        val uri = RingtoneManager.getActualDefaultRingtoneUri(ctx, RingtoneManager.TYPE_ALARM)
            ?: RingtoneManager.getActualDefaultRingtoneUri(ctx, RingtoneManager.TYPE_RINGTONE)
            ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)
        ringtone = RingtoneManager.getRingtone(ctx, uri)?.apply {
            audioAttributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_ALARM)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build()
            if (Build.VERSION.SDK_INT >= 28) isLooping = true
            play()
        }
        vibrator(ctx)?.vibrate(VibrationEffect.createWaveform(longArrayOf(0, 800, 400), 0),
            AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ALARM).build())
        notify(ctx)
        main.removeCallbacks(timeout)
        main.postDelayed(timeout, RING_MS)
        SmsBridge.broadcastAll("phone.find_state", JSONObject().put("ringing", true).toString())
    }

    @JvmStatic
    fun stopRinging() {
        main.removeCallbacks(timeout)
        val ctx = context ?: return
        val tone = ringtone ?: return
        ringtone = null
        try { tone.stop() } catch (e: Exception) { Log.w("AirLink", "find stop: $e") }
        vibrator(ctx)?.cancel()
        if (savedAlarmVolume >= 0) {
            try {
                ctx.getSystemService(AudioManager::class.java)?.setStreamVolume(AudioManager.STREAM_ALARM, savedAlarmVolume, 0)
            } catch (_: SecurityException) {}
            savedAlarmVolume = -1
        }
        ctx.getSystemService(NotificationManager::class.java)?.cancel(FIND_ID)
        SmsBridge.broadcastAll("phone.find_state", JSONObject().put("ringing", false).toString())
    }

    private fun vibrator(ctx: Context): Vibrator? =
        if (Build.VERSION.SDK_INT >= 31) ctx.getSystemService(VibratorManager::class.java)?.defaultVibrator
        else @Suppress("DEPRECATION") ctx.getSystemService(Vibrator::class.java)

    private fun notify(ctx: Context) {
        val notifications = ctx.getSystemService(NotificationManager::class.java) ?: return
        notifications.createNotificationChannel(
            NotificationChannel(CHANNEL_FIND, "Telefonumu bul", NotificationManager.IMPORTANCE_HIGH).apply {
                setSound(null, null)  // (the ring is its own)
            },
        )
        val stop = PendingIntent.getBroadcast(
            ctx, FIND_ID, Intent(ctx, PromptReceiver::class.java).setAction(ACTION_FIND_STOP), PendingIntent.FLAG_IMMUTABLE,
        )
        val notification = Notification.Builder(ctx, CHANNEL_FIND)
            .setSmallIcon(R.drawable.ic_stat_airlink)
            .setContentTitle("Telefonunuz aranıyor")
            .setContentText("Bir bilgisayardan çaldırıldı")
            .setCategory(Notification.CATEGORY_ALARM)
            .setOngoing(true)
            .setContentIntent(stop)
            .setDeleteIntent(stop)
            .addAction(Notification.Action.Builder(null, "Buldum", stop).build())
            .build()
        try {
            notifications.notify(FIND_ID, notification)
        } catch (e: SecurityException) {
            Log.w("AirLink", "find notification: $e")
        }
    }
}
