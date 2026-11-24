package com.plyg.airlink

import android.app.Notification
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.RemoteInput
import android.content.ComponentName
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import org.json.JSONObject

/** Mirrors eligible phone notifications and handles dismiss and reply requests. */
class AirLinkNotificationListener : NotificationListenerService() {
    override fun onListenerConnected() {
        instance = this
    }

    override fun onListenerDisconnected() {
        if (instance === this) instance = null
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        describe(sbn)?.let { broadcast("notification.posted", it) }
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        if (shared(sbn)) broadcast("notification.removed", JSONObject().put("key", sbn.key).toString())
    }

    private fun shared(sbn: StatusBarNotification): Boolean {
        if (sbn.packageName == packageName) return false
        val excluded = Notification.FLAG_ONGOING_EVENT or Notification.FLAG_GROUP_SUMMARY or Notification.FLAG_FOREGROUND_SERVICE
        return sbn.notification.flags and excluded == 0
    }

    private fun describe(sbn: StatusBarNotification): String? {
        if (!shared(sbn)) return null
        val notification = sbn.notification
        val title = notification.extras.getCharSequence(Notification.EXTRA_TITLE)
        val text = notification.extras.getCharSequence(Notification.EXTRA_BIG_TEXT)
            ?: notification.extras.getCharSequence(Notification.EXTRA_TEXT)
        if (title == null && text == null) return null
        return JSONObject()
            .put("key", sbn.key)
            .put("package", sbn.packageName)
            .put("app", appName(sbn.packageName))
            .put("title", title?.toString().orEmpty())
            .put("text", text?.toString().orEmpty())
            .put("time", sbn.postTime)
            .put("can_reply", replyAction(notification) != null)
            .toString()
    }

    private fun appName(pkg: String): String = try {
        val info = packageManager.getApplicationInfo(pkg, 0)
        packageManager.getApplicationLabel(info).toString()
    } catch (_: Exception) {
        pkg
    }

    companion object {
        @Volatile
        private var instance: AirLinkNotificationListener? = null

        private fun replyAction(notification: Notification): Notification.Action? =
            notification.actions?.firstOrNull { action -> action.remoteInputs?.any { it.allowFreeFormInput } == true }

        private fun broadcast(kind: String, body: String) {
            android.util.Log.d("AirLink", "notification $kind (native ready: ${MainActivity.nativeReady})")
            if (MainActivity.nativeReady) nativeBroadcast(kind, body)
        }

        /** Called from the native bridge on the UI thread. */
        @JvmStatic
        fun accessGranted(): Boolean {
            val context = AirLinkService.appContext ?: return false
            val component = ComponentName(context, AirLinkNotificationListener::class.java)
            if (Build.VERSION.SDK_INT >= 27) {
                return context.getSystemService(NotificationManager::class.java)
                    ?.isNotificationListenerAccessGranted(component) == true
            }
            val enabled = Settings.Secure.getString(context.contentResolver, "enabled_notification_listeners")
            return enabled?.contains(component.flattenToString()) == true
        }

        /** Opens the system's notification access page. */
        @JvmStatic
        fun openAccessSettings() {
            val activity = MainActivity.current() ?: return
            val intent = if (Build.VERSION.SDK_INT >= 30) {
                Intent(Settings.ACTION_NOTIFICATION_LISTENER_DETAIL_SETTINGS).putExtra(
                    Settings.EXTRA_NOTIFICATION_LISTENER_COMPONENT_NAME,
                    ComponentName(activity, AirLinkNotificationListener::class.java).flattenToString(),
                )
            } else {
                Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
            }
            try {
                activity.startActivity(intent)
            } catch (_: Exception) {
                activity.startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
            }
        }

        /** Handles a paired computer's dismiss or reply request. */
        @JvmStatic
        fun act(kind: String, key: String, text: String) {
            val service = instance ?: return
            if (kind == "notification.dismiss") {
                service.cancelNotification(key)
                return
            }
            if (kind != "notification.reply") return
            val found = service.getActiveNotifications(arrayOf(key))?.firstOrNull() ?: return
            val action = replyAction(found.notification) ?: return
            val inputs = action.remoteInputs ?: return
            val results = Bundle()
            inputs.filter { it.allowFreeFormInput }.forEach { results.putCharSequence(it.resultKey, text) }
            val fill = Intent()
            RemoteInput.addResultsToIntent(inputs, fill, results)
            try {
                action.actionIntent.send(service, 0, fill)
            } catch (_: PendingIntent.CanceledException) {
                // The source app removed the action after the desktop requested it.
            }
        }

        @JvmStatic
        private external fun nativeBroadcast(kind: String, body: String)
    }
}
