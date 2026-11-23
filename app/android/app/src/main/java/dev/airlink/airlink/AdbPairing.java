package dev.airlink.airlink;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.RemoteInput;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;

/**
 * Setting up advanced mode: the user opens Wireless debugging > "Pair device with pairing code"
 * and types the six digits it shows into this notification's reply field. The pairing screen has
 * to stay open meanwhile (leaving it ends pairing), which is why the code comes in a notification
 * and not in AirLink's own screen.
 */
final class AdbPairing {
    static final int NOTIFICATION = 1600;
    static final String KEY_CODE = "code";

    private AdbPairing() {}

    /** From native code: Developer options, and the notification to type the code into. */
    static void start() {
        Context c = AirLinkService.appContext;
        if (c == null) return;
        post(c, "Eşleme kodunu buraya yazın",
                "Geliştirici seçenekleri > Kablosuz hata ayıklama > Eşleme koduyla cihaz eşle; gösterilen 6 haneli kodu girin.", true);
        MainActivity a = MainActivity.current();
        if (a != null) {
            try {
                a.startActivity(new Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS));
            } catch (Exception e) {
                a.startActivity(new Intent(Settings.ACTION_DEVICE_INFO_SETTINGS));  // Developer options not on yet
            }
        }
    }

    /** From native code: how the pairing went. */
    static void done(boolean ok, String message) {
        Context c = AirLinkService.appContext;
        if (c == null) return;
        if (ok) post(c, "Gelişmiş mod hazır", "AirLink artık telefon ekranını uygulama uygulama bilgisayara açabilir.", false);
        else post(c, "Eşleşme olmadı", message + " — kodu yeniden girin.", true);
    }

    private static void post(Context c, String title, String text, boolean askCode) {
        Notification.Builder b = (Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(c, "requests") : new Notification.Builder(c))
                .setSmallIcon(R.drawable.ic_stat_airlink)
                .setContentTitle(title)
                .setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text))
                .setOnlyAlertOnce(true);
        if (askCode) {
            RemoteInput input = new RemoteInput.Builder(KEY_CODE).setLabel("6 haneli eşleme kodu").build();
            // Mutable: the system adds the typed code to it.
            PendingIntent reply = PendingIntent.getBroadcast(c, NOTIFICATION, new Intent(c, Receiver.class),
                    PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0));
            b.addAction(new Notification.Action.Builder(null, "Kodu gir", reply).addRemoteInput(input).build());
        } else {
            b.setAutoCancel(true);
        }
        c.getSystemService(NotificationManager.class).notify(NOTIFICATION, b.build());
    }

    /** The code typed into the notification. */
    public static class Receiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            Bundle results = RemoteInput.getResultsFromIntent(intent);
            CharSequence code = results != null ? results.getCharSequence(KEY_CODE) : null;
            if (code == null) return;
            post(context, "Eşleniyor…", "Kablosuz hata ayıklama ile eşleşiliyor.", false);
            if (MainActivity.nativeReady) MainActivity.adbPair(code.toString().trim());
        }
    }
}
