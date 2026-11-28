package com.plyg.airlink

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.widget.Toast
import java.io.File

/**
 * AirLink's own update on the phone: the release's APK (update_state.cpp downloaded and checked
 * it) handed to Android's package installer, which asks the user to confirm. Installing apps
 * needs the "install unknown apps" permission for AirLink: its settings page opens the first time.
 */
object Updater {
    /** Empty when the installer took it, else what to tell the user. */
    @JvmStatic
    fun install(path: String): String {
        val context = AirLinkService.appContext ?: MainActivity.current() ?: return "no context"
        if (Build.VERSION.SDK_INT >= 26 && !context.packageManager.canRequestPackageInstalls()) {
            context.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + context.packageName))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            return Texts.tr("AirLink'e uygulama yükleme izni verin, sonra Güncelle'ye yeniden basın")
        }
        val apk = File(path)
        return try {
            val installer = context.packageManager.packageInstaller
            val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
            params.setAppPackageName(context.packageName)
            val id = installer.createSession(params)
            installer.openSession(id).use { session ->
                apk.inputStream().use { input ->
                    session.openWrite("airlink.apk", 0, apk.length()).use { output ->
                        input.copyTo(output)
                        session.fsync(output)
                    }
                }
                val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
                val done = PendingIntent.getBroadcast(context, id, Intent(context, UpdateReceiver::class.java), flags)
                session.commit(done.intentSender)
            }
            ""
        } catch (error: Exception) {
            error.message ?: error.toString()
        }
    }
}

/** The installer's answers: its confirmation screen to show, or why it couldn't install. */
class UpdateReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                @Suppress("DEPRECATION")
                val confirm = intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT) ?: return
                context.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            }
            PackageInstaller.STATUS_SUCCESS -> {}
            PackageInstaller.STATUS_FAILURE_CONFLICT, PackageInstaller.STATUS_FAILURE_INCOMPATIBLE ->
                Toast.makeText(context, Texts.tr("Güncelleme kurulamadı: yüklü AirLink başka bir anahtarla imzalanmış"), Toast.LENGTH_LONG).show()
            PackageInstaller.STATUS_FAILURE_ABORTED -> {}
            else -> {
                val message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: ""
                Toast.makeText(context, Texts.trf("Güncelleme kurulamadı: {}", message), Toast.LENGTH_LONG).show()
            }
        }
    }
}
