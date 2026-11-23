package dev.airlink.airlink;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.IBinder;

import org.json.JSONObject;

/**
 * Keeps AirLink reachable while its screen is closed: a foreground service (a "ready" notification
 * the system requires) holding the locks that keep Wi-Fi and mDNS working in the background. The Rust
 * core itself lives in the app's process (libmain.so); this service keeps that process alive.
 *
 * It also shows what the core asks while the app isn't on screen (pairing requests, file offers) as
 * notifications with Accept / Decline buttons.
 */
public class AirLinkService extends Service {
    static final String ACTION_STOP = "dev.airlink.airlink.STOP";
    private static final String CHANNEL_STATUS = "status";
    private static final String CHANNEL_REQUESTS = "requests";
    private static final int STATUS_ID = 1;

    static Context appContext;  // set by MainActivity; what onPrompt posts with
    private static AirLinkService self;

    // The core's events (a computer's requests: touches, SMS, calls…) are applied by the app's
    // UI loop, which Android stops while another app is on screen: pump them from here too.
    private final android.os.Handler pumper = new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable pump = new Runnable() {
        @Override
        public void run() {
            if (MainActivity.nativeReady) MainActivity.pump();
            pumper.postDelayed(this, 50);
        }
    };

    private WifiManager.MulticastLock multicastLock;  // Android drops mDNS without it
    private WifiManager.WifiLock wifiLock;            // Wi-Fi stays fast and awake while sending

    static void start(Context context) {
        Intent intent = new Intent(context, AirLinkService.class);
        if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent);
        else context.startService(intent);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        self = this;
        channels(this);
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            multicastLock = wifi.createMulticastLock("airlink-mdns");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
            int mode = Build.VERSION.SDK_INT >= 29 ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            wifiLock = wifi.createWifiLock(mode, "airlink");
            wifiLock.setReferenceCounted(false);
            wifiLock.acquire();
        }
        CallBridge.start(this);
        pumper.post(pump);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            stopForeground(true);
            stopSelf();
            // The core lives in this process: stopping AirLink ends it (peers see it leave).
            if (MainActivity.current() != null) MainActivity.current().finishAndRemoveTask();
            new android.os.Handler(getMainLooper()).postDelayed(() -> System.exit(0), 300);
            return START_NOT_STICKY;
        }
        Notification status = statusNotification();
        if (Build.VERSION.SDK_INT >= 29) startForeground(STATUS_ID, status, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
        else startForeground(STATUS_ID, status);
        // Not restarted on its own: the core starts with the app's screen.
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        if (self == this) self = null;
        pumper.removeCallbacks(pump);
        CallBridge.stop(this);
        if (multicastLock != null) multicastLock.release();
        if (wifiLock != null) wifiLock.release();
        super.onDestroy();
    }

    /** Screen sharing began: Android 14+ needs the service to be of the mediaProjection type too. */
    static void startProjectionForeground(Context c) {
        AirLinkService s = self;
        if (s == null || Build.VERSION.SDK_INT < 29) return;
        s.startForeground(STATUS_ID, s.statusNotification(),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE | ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
    }

    static void stopProjectionForeground() {
        AirLinkService s = self;
        if (s == null || Build.VERSION.SDK_INT < 29) return;
        s.startForeground(STATUS_ID, s.statusNotification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private Notification statusNotification() {
        PendingIntent stop = PendingIntent.getService(this, 0, new Intent(this, AirLinkService.class).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE);
        return builder(this, CHANNEL_STATUS)
                .setSmallIcon(R.drawable.ic_stat_airlink)
                .setContentTitle("AirLink hazır")
                .setContentText("Cihazlarınız dosya ve pano gönderebilir")
                .setContentIntent(openApp(this, 0))
                .setOngoing(true)
                .addAction(new Notification.Action.Builder(null, "Durdur", stop).build())
                .build();
    }

    // ---- Requests while the app isn't on screen -------------------------------------------

    /** From native code (the core's thread): a request appeared or went away. */
    static void onPrompt(String json) {
        Context context = appContext;
        if (context == null) return;
        try {
            JSONObject e = new JSONObject(json);
            String kind = e.getString("event");
            long id = e.getLong("request_id");
            NotificationManager nm = context.getSystemService(NotificationManager.class);
            int nid = notificationId(id);
            if (kind.equals("request_cancelled") || kind.equals("request_answered")) {
                nm.cancel(nid);
                return;
            }
            if (MainActivity.visible) return;  // the app shows its own dialog
            String peer = e.getJSONObject("peer").getString("name");
            Notification.Builder b = builder(context, CHANNEL_REQUESTS)
                    .setSmallIcon(R.drawable.ic_stat_airlink)
                    .setContentIntent(openApp(context, nid))
                    .setAutoCancel(true)
                    .setCategory(Notification.CATEGORY_CALL)
                    .setPriority(Notification.PRIORITY_HIGH);
            String acceptLabel;
            if (kind.equals("pairing_request")) {
                String code = e.getString("code");
                b.setContentTitle(peer + " eşleşmek istiyor")
                        .setContentText("Kod: " + code.substring(0, 3) + " " + code.substring(3) + " — diğer cihazdakiyle aynıysa onaylayın");
                acceptLabel = "Kodlar aynı";
            } else if (kind.equals("file_offer")) {
                int count = e.getJSONArray("files").length();
                String first = e.getJSONArray("files").getJSONObject(0).getString("path");
                b.setContentTitle(peer + " dosya göndermek istiyor")
                        .setContentText(count == 1 ? first : count + " dosya, " + first + " ve diğerleri");
                acceptLabel = "Kabul et";
            } else {
                return;
            }
            b.addAction(new Notification.Action.Builder(null, "Reddet", answer(context, id, false)).build());
            b.addAction(new Notification.Action.Builder(null, acceptLabel, answer(context, id, true)).build());
            nm.notify(nid, b.build());
        } catch (Exception ignored) {
        }
    }

    static int notificationId(long requestId) {
        return 1000 + (int) (requestId % 100000);
    }

    private static PendingIntent answer(Context context, long id, boolean accept) {
        Intent intent = new Intent(context, PromptReceiver.class)
                .putExtra(PromptReceiver.EXTRA_ID, id)
                .putExtra(PromptReceiver.EXTRA_ACCEPT, accept);
        int code = (int) (id * 2 + (accept ? 1 : 0));
        return PendingIntent.getBroadcast(context, code, intent, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private static PendingIntent openApp(Context context, int code) {
        Intent open = new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        return PendingIntent.getActivity(context, code, open, PendingIntent.FLAG_IMMUTABLE);
    }

    private static Notification.Builder builder(Context context, String channel) {
        return Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(context, channel) : new Notification.Builder(context);
    }

    static void channels(Context context) {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationManager nm = context.getSystemService(NotificationManager.class);
        NotificationChannel status = new NotificationChannel(CHANNEL_STATUS, "AirLink durumu", NotificationManager.IMPORTANCE_MIN);
        status.setShowBadge(false);
        nm.createNotificationChannel(status);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL_REQUESTS, "Eşleşme ve dosya istekleri", NotificationManager.IMPORTANCE_HIGH));
    }
}
