package dev.airlink.airlink;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.provider.Telephony;
import android.telephony.SmsMessage;

/** An SMS arrived: told to paired computers, so their open conversation updates. */
public class SmsReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (!Telephony.Sms.Intents.SMS_RECEIVED_ACTION.equals(intent.getAction())) return;
        SmsMessage[] parts = Telephony.Sms.Intents.getMessagesFromIntent(intent);
        if (parts == null || parts.length == 0) return;
        StringBuilder body = new StringBuilder();
        for (SmsMessage p : parts) body.append(p.getMessageBody());
        SmsBridge.received(context.getApplicationContext(), parts[0].getOriginatingAddress(), body.toString(), parts[0].getTimestampMillis());
    }
}
