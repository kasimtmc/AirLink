package dev.airlink.airlink;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.RemoteInput;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;

import org.json.JSONObject;

/**
 * Shares this phone's notifications with paired computers (once the user turns on Notification
 * access for AirLink), and carries out what a computer asks of them: dismiss, or reply.
 */
public class AirLinkNotificationListener extends NotificationListenerService {
    private static AirLinkNotificationListener instance;

    @Override
    public void onListenerConnected() {
        instance = this;
    }

    @Override
    public void onListenerDisconnected() {
        if (instance == this) instance = null;
    }

    @Override
    public void onNotificationPosted(StatusBarNotification sbn) {
        String body = describe(sbn);
        if (body != null) broadcast("notification.posted", body);
    }

    @Override
    public void onNotificationRemoved(StatusBarNotification sbn) {
        if (!shared(sbn)) return;
        try {
            broadcast("notification.removed", new JSONObject().put("key", sbn.getKey()).toString());
        } catch (Exception ignored) {
        }
    }

    // Not AirLink's own, not ongoing ones (music players, downloads, calls in progress), not a
    // group's summary (its messages come one by one).
    private boolean shared(StatusBarNotification sbn) {
        Notification n = sbn.getNotification();
        if (sbn.getPackageName().equals(getPackageName())) return false;
        if ((n.flags & (Notification.FLAG_ONGOING_EVENT | Notification.FLAG_GROUP_SUMMARY | Notification.FLAG_FOREGROUND_SERVICE)) != 0) return false;
        return true;
    }

    private String describe(StatusBarNotification sbn) {
        if (!shared(sbn)) return null;
        Notification n = sbn.getNotification();
        Bundle extras = n.extras;
        CharSequence title = extras.getCharSequence(Notification.EXTRA_TITLE);
        CharSequence text = extras.getCharSequence(Notification.EXTRA_BIG_TEXT);
        if (text == null) text = extras.getCharSequence(Notification.EXTRA_TEXT);
        if (title == null && text == null) return null;
        try {
            return new JSONObject()
                    .put("key", sbn.getKey())
                    .put("package", sbn.getPackageName())
                    .put("app", appName(sbn.getPackageName()))
                    .put("title", title != null ? title.toString() : "")
                    .put("text", text != null ? text.toString() : "")
                    .put("time", sbn.getPostTime())
                    .put("can_reply", replyAction(n) != null)
                    .toString();
        } catch (Exception e) {
            return null;
        }
    }

    private String appName(String pkg) {
        try {
            PackageManager pm = getPackageManager();
            ApplicationInfo info = pm.getApplicationInfo(pkg, 0);
            return pm.getApplicationLabel(info).toString();
        } catch (Exception e) {
            return pkg;
        }
    }

    // A messaging app's "Reply" action: one that takes typed text.
    private static Notification.Action replyAction(Notification n) {
        if (n.actions == null) return null;
        for (Notification.Action a : n.actions) {
            RemoteInput[] inputs = a.getRemoteInputs();
            if (inputs == null) continue;
            for (RemoteInput in : inputs)
                if (in.getAllowFreeFormInput()) return a;
        }
        return null;
    }

    private static void broadcast(String kind, String body) {
        android.util.Log.d("AirLink", "notification " + kind + " (native ready: " + MainActivity.nativeReady + ")");
        if (MainActivity.nativeReady) nativeBroadcast(kind, body);
    }

    // ---- Called from native code, on the UI thread ----------------------------------------

    /** Whether the user turned on Notification access for AirLink. */
    static boolean accessGranted() {
        Context c = AirLinkService.appContext;
        if (c == null) return false;
        ComponentName me = new ComponentName(c, AirLinkNotificationListener.class);
        if (Build.VERSION.SDK_INT >= 27) return c.getSystemService(NotificationManager.class).isNotificationListenerAccessGranted(me);
        String enabled = Settings.Secure.getString(c.getContentResolver(), "enabled_notification_listeners");
        return enabled != null && enabled.contains(me.flattenToString());
    }

    /** Opens Settings at Notification access, where the user allows AirLink. */
    static void openAccessSettings() {
        MainActivity a = MainActivity.current();
        if (a == null) return;
        Intent intent = new Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS);
        if (Build.VERSION.SDK_INT >= 30) {
            intent = new Intent(Settings.ACTION_NOTIFICATION_LISTENER_DETAIL_SETTINGS)
                    .putExtra(Settings.EXTRA_NOTIFICATION_LISTENER_COMPONENT_NAME,
                            new ComponentName(a, AirLinkNotificationListener.class).flattenToString());
        }
        try {
            a.startActivity(intent);
        } catch (Exception e) {
            a.startActivity(new Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS));
        }
    }

    /** A computer asked: "notification.dismiss", or "notification.reply" with `text`. */
    static void act(String kind, String key, String text) {
        AirLinkNotificationListener self = instance;
        if (self == null) return;
        if (kind.equals("notification.dismiss")) {
            self.cancelNotification(key);
            return;
        }
        if (!kind.equals("notification.reply")) return;
        StatusBarNotification[] found = self.getActiveNotifications(new String[]{key});
        if (found == null || found.length == 0) return;
        Notification.Action action = replyAction(found[0].getNotification());
        if (action == null) return;
        Intent fill = new Intent();
        Bundle results = new Bundle();
        for (RemoteInput in : action.getRemoteInputs())
            if (in.getAllowFreeFormInput()) results.putCharSequence(in.getResultKey(), text);
        RemoteInput.addResultsToIntent(action.getRemoteInputs(), fill, results);
        try {
            action.actionIntent.send(self, 0, fill);
        } catch (PendingIntent.CanceledException ignored) {
        }
    }

    private static native void nativeBroadcast(String kind, String body);
}
