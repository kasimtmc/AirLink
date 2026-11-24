package com.plyg.airlink

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.provider.Telephony

/** Reports incoming SMS messages to paired computers. */
class SmsReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Telephony.Sms.Intents.SMS_RECEIVED_ACTION) return
        val parts = Telephony.Sms.Intents.getMessagesFromIntent(intent) ?: return
        val first = parts.firstOrNull() ?: return

        val body = parts.joinToString(separator = "") { it.messageBody.orEmpty() }
        SmsBridge.received(context.applicationContext, first.originatingAddress, body, first.timestampMillis)
    }
}
