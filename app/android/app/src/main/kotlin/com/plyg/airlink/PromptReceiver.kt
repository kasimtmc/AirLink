package com.plyg.airlink

import android.app.NotificationManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** Answers the core when the user accepts or declines a request notification. */
class PromptReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == PhoneStatus.ACTION_FIND_STOP) {  // "Buldum" (or the notification tapped)
            PhoneStatus.stopRinging()
            return
        }
        if (intent.hasExtra(EXTRA_PASSWORD)) {  // "Panoda şifre var": send it to the trusted devices or not
            MainActivity.answerPassword(intent.getBooleanExtra(EXTRA_PASSWORD, false))
            return
        }
        val id = intent.getLongExtra(EXTRA_ID, -1L)
        if (id < 0) return

        context.getSystemService(NotificationManager::class.java)
            ?.cancel(AirLinkService.notificationId(id))
        MainActivity.respond(id, intent.getBooleanExtra(EXTRA_ACCEPT, false))
    }

    companion object {
        const val EXTRA_ID = "request_id"
        const val EXTRA_ACCEPT = "accept"
        const val EXTRA_PASSWORD = "send_password"
    }
}
