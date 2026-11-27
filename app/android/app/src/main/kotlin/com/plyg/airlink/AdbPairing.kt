package com.plyg.airlink

import android.app.Notification
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.RemoteInput
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import android.provider.Settings

/** Keeps the six digit pairing screen open while the code is entered through a notification. */
object AdbPairing {
    private const val NOTIFICATION = 1600
    private const val KEY_CODE = "code"

    /** Called by the native bridge to open Developer options and show the code entry. */
    @JvmStatic
    fun start() {
        val context = AirLinkService.appContext ?: return
        post(
            context,
            Texts.tr("Eşleme kodunu buraya yazın"),
            Texts.tr("Kablosuz hata ayıklama > Eşleme koduyla cihaz eşle. Kodu girin; bağlantı bulunamazsa kod:eşleme_portu yazın."),
            true,
        )
        MainActivity.current()?.let { activity ->
            try {
                activity.startActivity(Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS))
            } catch (_: Exception) {
                activity.startActivity(Intent(Settings.ACTION_DEVICE_INFO_SETTINGS))
            }
        }
    }

    /** Called by the native bridge after pairing completes. */
    @JvmStatic
    fun done(ok: Boolean, message: String) {
        val context = AirLinkService.appContext ?: return
        if (ok) {
            post(context, Texts.tr("Gelişmiş mod hazır"), Texts.tr("AirLink artık telefon ekranını uygulama uygulama bilgisayara açabilir."), false)
        } else {
            post(context, Texts.tr("Eşleşme olmadı"), Texts.trf("{} — kodu yeniden girin.", message), true)
        }
    }

    private fun post(context: Context, title: String, text: String, askCode: Boolean) {
        val builder = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(context, "requests") else Notification.Builder(context)
        builder.setSmallIcon(R.drawable.ic_stat_airlink)
            .setContentTitle(title)
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setOnlyAlertOnce(true)
        if (askCode) {
            val input = RemoteInput.Builder(KEY_CODE).setLabel(Texts.tr("6 haneli kod veya kod:port")).build()
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
            val reply = PendingIntent.getBroadcast(context, NOTIFICATION, Intent(context, Receiver::class.java), flags)
            builder.addAction(Notification.Action.Builder(null, Texts.tr("Kodu gir"), reply).addRemoteInput(input).build())
        } else {
            builder.setAutoCancel(true)
        }
        context.getSystemService(NotificationManager::class.java)?.notify(NOTIFICATION, builder.build())
    }

    /** Reads the code without leaving Android's wireless pairing screen. */
    class Receiver : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val code = RemoteInput.getResultsFromIntent(intent)?.getCharSequence(KEY_CODE)?.toString()?.trim() ?: return
            post(context, Texts.tr("Eşleniyor…"), Texts.tr("Kablosuz hata ayıklama ile eşleşiliyor."), false)
            if (MainActivity.nativeReady) MainActivity.adbPair(code)
        }
    }
}
