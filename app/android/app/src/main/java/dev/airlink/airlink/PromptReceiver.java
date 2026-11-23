package dev.airlink.airlink;

import android.app.NotificationManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** A request notification's Accept / Decline button: answers the core. */
public class PromptReceiver extends BroadcastReceiver {
    static final String EXTRA_ID = "request_id";
    static final String EXTRA_ACCEPT = "accept";

    @Override
    public void onReceive(Context context, Intent intent) {
        long id = intent.getLongExtra(EXTRA_ID, -1);
        if (id < 0) return;
        context.getSystemService(NotificationManager.class).cancel(AirLinkService.notificationId(id));
        MainActivity.respond(id, intent.getBooleanExtra(EXTRA_ACCEPT, false));
    }
}
