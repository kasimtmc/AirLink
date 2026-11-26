package com.plyg.airlink

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import org.json.JSONObject

/** Keeps the Rust core reachable and presents requests while the activity is closed. */
class AirLinkService : Service() {
    private val pumper = Handler(Looper.getMainLooper())
    private val pump = object : Runnable {
        override fun run() {
            if (MainActivity.nativeReady) MainActivity.pump()
            pumper.postDelayed(this, 50)
        }
    }
    private var multicastLock: WifiManager.MulticastLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onCreate() {
        super.onCreate()
        if (!ScreenShare.active()) ScreenShare.restoreMusicOutput(this)
        self = this
        appContext = applicationContext
        channels(this)
        (applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager)?.let { wifi ->
            multicastLock = wifi.createMulticastLock("airlink-mdns").apply {
                setReferenceCounted(false)
                acquire()
            }
            val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                else WifiManager.WIFI_MODE_FULL_HIGH_PERF
            wifiLock = wifi.createWifiLock(mode, "airlink").apply {
                setReferenceCounted(false)
                acquire()
            }
        }
        CallBridge.start(this)
        FlingDetector.resume(this)
        PhoneMedia.start(this)
        pumper.post(pump)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
            MainActivity.current()?.finishAndRemoveTask()
            Handler(mainLooper).postDelayed({ System.exit(0) }, 300)
            return START_NOT_STICKY
        }
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(STATUS_ID, statusNotification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(STATUS_ID, statusNotification())
        }
        MainActivity.ensureBackgroundNative(applicationContext)
        return START_STICKY
    }

    override fun onDestroy() {
        PlayerBluetoothAudio.stop()
        if (ScreenShare.active()) ScreenShare.stop("user")
        if (self === this) self = null
        pumper.removeCallbacks(pump)
        CallBridge.stop(this)
        FlingDetector.pause()
        multicastLock?.release()
        wifiLock?.release()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun statusNotification(): Notification {
        val stop = PendingIntent.getService(
            this, 0, Intent(this, AirLinkService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE,
        )
        return builder(this, CHANNEL_STATUS)
            .setSmallIcon(R.drawable.ic_stat_airlink)
            .setContentTitle("AirLink hazır")
            .setContentText("Cihazlarınız dosya ve pano gönderebilir")
            .setContentIntent(openApp(this, 0))
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Durdur", stop).build())
            .build()
    }

    companion object {
        const val ACTION_STOP = "com.plyg.airlink.STOP"
        private const val CHANNEL_STATUS = "status"
        private const val CHANNEL_REQUESTS = "requests"
        private const val STATUS_ID = 1

        @JvmField var appContext: Context? = null
        private var self: AirLinkService? = null

        @JvmStatic
        fun start(context: Context) {
            val intent = Intent(context, AirLinkService::class.java)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent) else context.startService(intent)
        }

        @JvmStatic
        fun startProjectionForeground(context: Context) {
            val service = self ?: return
            if (Build.VERSION.SDK_INT < 29) return
            service.startForeground(
                STATUS_ID, service.statusNotification(),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION,
            )
        }

        @JvmStatic
        fun stopProjectionForeground() {
            val service = self ?: return
            if (Build.VERSION.SDK_INT < 29) return
            service.startForeground(
                STATUS_ID, service.statusNotification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
            )
        }

        /** Native core request notification callback. */
        @JvmStatic
        fun onPrompt(json: String) {
            val context = appContext ?: return
            try {
                val event = JSONObject(json)
                val kind = event.getString("event")
                val id = event.getLong("request_id")
                val notifications = context.getSystemService(NotificationManager::class.java) ?: return
                val notificationId = notificationId(id)
                if (kind == "request_cancelled" || kind == "request_answered") {
                    notifications.cancel(notificationId)
                    return
                }
                if (MainActivity.visible) return
                val peer = event.getJSONObject("peer").getString("name")
                val builder = builder(context, CHANNEL_REQUESTS)
                    .setSmallIcon(R.drawable.ic_stat_airlink)
                    .setContentIntent(openApp(context, notificationId))
                    .setAutoCancel(true)
                    .setCategory(Notification.CATEGORY_CALL)
                    .setPriority(Notification.PRIORITY_HIGH)
                val acceptLabel = when (kind) {
                    "pairing_request" -> {
                        val code = event.getString("code")
                        builder.setContentTitle("$peer eşleşmek istiyor")
                            .setContentText("Kod: ${code.take(3)} ${code.drop(3)} — diğer cihazdakiyle aynıysa onaylayın")
                        "Kodlar aynı"
                    }
                    "file_offer" -> {
                        val files = event.getJSONArray("files")
                        val first = files.getJSONObject(0).getString("path")
                        builder.setContentTitle("$peer dosya göndermek istiyor")
                            .setContentText(if (files.length() == 1) first else "${files.length()} dosya, $first ve diğerleri")
                        "Kabul et"
                    }
                    else -> return
                }
                builder.addAction(Notification.Action.Builder(null, "Reddet", answer(context, id, false)).build())
                builder.addAction(Notification.Action.Builder(null, acceptLabel, answer(context, id, true)).build())
                notifications.notify(notificationId, builder.build())
            } catch (_: Exception) {
                // A withdrawn request can race with event delivery.
            }
        }

        @JvmStatic
        fun notificationId(requestId: Long): Int = 1000 + (requestId % 100000).toInt()

        private fun answer(context: Context, id: Long, accept: Boolean): PendingIntent {
            val intent = Intent(context, PromptReceiver::class.java)
                .putExtra(PromptReceiver.EXTRA_ID, id)
                .putExtra(PromptReceiver.EXTRA_ACCEPT, accept)
            val code = (id * 2 + if (accept) 1 else 0).toInt()
            return PendingIntent.getBroadcast(
                context, code, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
        }

        private fun openApp(context: Context, code: Int): PendingIntent = PendingIntent.getActivity(
            context, code, Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE,
        )

        private fun builder(context: Context, channel: String): Notification.Builder =
            if (Build.VERSION.SDK_INT >= 26) Notification.Builder(context, channel) else Notification.Builder(context)

        @JvmStatic
        fun channels(context: Context) {
            if (Build.VERSION.SDK_INT < 26) return
            val notifications = context.getSystemService(NotificationManager::class.java) ?: return
            notifications.createNotificationChannel(
                NotificationChannel(CHANNEL_STATUS, "AirLink durumu", NotificationManager.IMPORTANCE_MIN).apply {
                    setShowBadge(false)
                },
            )
            notifications.createNotificationChannel(
                NotificationChannel(CHANNEL_REQUESTS, "Eşleşme ve dosya istekleri", NotificationManager.IMPORTANCE_HIGH),
            )
        }
    }
}
