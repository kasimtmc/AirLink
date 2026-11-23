package dev.airlink.airlink;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.view.WindowManager;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;

/**
 * This phone's screen for a paired computer: asked with "screen.start", the user agrees (Android's
 * screen capture prompt), then JPEG frames go on the media stream "screen" until "screen.stop", the
 * user stops it, or the computer leaves.
 */
final class ScreenShare {
    static final int REQUEST = 7101;
    private static final int NOTIFICATION = 1500;
    private static final int MAX_FPS = 15;

    private static String pendingPeer;  // asked, waiting for the user's consent
    private static String peer;         // streaming to
    private static MediaProjection projection;
    private static VirtualDisplay display;
    private static ImageReader reader;
    private static HandlerThread thread;
    private static long lastFrame;
    private static int width, height;   // the frames'
    private static int quality = 60;

    private ScreenShare() {}

    static boolean active() {
        return projection != null;
    }

    /** "screen.start" from a computer (UI thread): ask the user; from a notification if the app is away. */
    static void requested(Context c, String peerId, JSONObject req) {
        if (projection != null) stop("restarted");
        pendingPeer = peerId;
        quality = Math.max(30, Math.min(90, req.optInt("quality", 60)));
        MainActivity a = MainActivity.current();
        if (a != null && MainActivity.visible) {
            askConsent(a);
            return;
        }
        // An activity can't be started from the background: the user opens it from this.
        Intent open = new Intent(c, MainActivity.class).setAction("dev.airlink.airlink.SHARE_SCREEN")
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        Notification n = (Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(c, "requests") : new Notification.Builder(c))
                .setSmallIcon(R.drawable.ic_stat_airlink)
                .setContentTitle("Bilgisayar ekranınızı görmek istiyor")
                .setContentText("Başlatmak için dokunun")
                .setContentIntent(PendingIntent.getActivity(c, NOTIFICATION, open, PendingIntent.FLAG_IMMUTABLE))
                .setAutoCancel(true)
                .build();
        c.getSystemService(NotificationManager.class).notify(NOTIFICATION, n);
    }

    /** The activity is on screen and a computer asked: Android's own consent prompt. */
    static void askConsent(Activity a) {
        if (pendingPeer == null) return;
        a.getSystemService(NotificationManager.class).cancel(NOTIFICATION);
        MediaProjectionManager mpm = a.getSystemService(MediaProjectionManager.class);
        a.startActivityForResult(mpm.createScreenCaptureIntent(), REQUEST);
    }

    /** The consent prompt's answer. */
    static void consentResult(Activity a, int resultCode, Intent data) {
        String asked = pendingPeer;
        pendingPeer = null;
        if (asked == null) return;
        if (resultCode != Activity.RESULT_OK || data == null) {
            tell(asked, "screen.stopped", "declined");
            return;
        }
        // Android 14+: capturing needs the foreground service to be of the mediaProjection type first.
        AirLinkService.startProjectionForeground(a);
        MediaProjectionManager mpm = a.getSystemService(MediaProjectionManager.class);
        projection = mpm.getMediaProjection(resultCode, data);
        if (projection == null) {
            tell(asked, "screen.stopped", "failed");
            return;
        }
        peer = asked;
        start(a.getApplicationContext());
    }

    private static void start(Context c) {
        thread = new HandlerThread("airlink-screen");
        thread.start();
        Handler handler = new Handler(thread.getLooper());
        projection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {  // the user stopped it from the system's chip or the lock screen
                new Handler(Looper.getMainLooper()).post(() -> stop("user"));
            }
        }, handler);

        // The screen's size, scaled so its long side is at most 1280 px: enough to read, small to send.
        Rect bounds = realBounds(c);
        float scale = Math.min(1f, 1280f / Math.max(bounds.width(), bounds.height()));
        width = Math.round(bounds.width() * scale) & ~1;
        height = Math.round(bounds.height() * scale) & ~1;
        int dpi = c.getResources().getDisplayMetrics().densityDpi;
        reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2);
        reader.setOnImageAvailableListener(r -> frame(r), handler);
        display = projection.createVirtualDisplay("AirLink", width, height, dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, reader.getSurface(), null, handler);
        tell(peer, "screen.started", null);
    }

    private static void frame(ImageReader r) {
        Image image = r.acquireLatestImage();
        if (image == null) return;
        try {
            long now = System.currentTimeMillis();
            if (now - lastFrame < 1000 / MAX_FPS || peer == null) return;
            lastFrame = now;
            Image.Plane plane = image.getPlanes()[0];
            ByteBuffer buf = plane.getBuffer();
            int pixelStride = plane.getPixelStride(), rowStride = plane.getRowStride();
            int padded = rowStride / pixelStride;
            Bitmap bmp = Bitmap.createBitmap(padded, height, Bitmap.Config.ARGB_8888);
            bmp.copyPixelsFromBuffer(buf);
            Bitmap cropped = padded == width ? bmp : Bitmap.createBitmap(bmp, 0, 0, width, height);
            ByteArrayOutputStream out = new ByteArrayOutputStream(128 * 1024);
            cropped.compress(Bitmap.CompressFormat.JPEG, quality, out);
            if (cropped != bmp) cropped.recycle();
            bmp.recycle();
            if (MainActivity.nativeReady) nativeSendFrame(peer, out.toByteArray());
        } finally {
            image.close();
        }
    }

    /** "screen.stop" from the computer, the user, or the app leaving (UI thread). */
    static void stop(String why) {
        String was = peer;
        peer = null;
        pendingPeer = null;
        if (display != null) display.release();
        if (reader != null) reader.close();
        if (projection != null) projection.stop();
        if (thread != null) thread.quitSafely();
        display = null;
        reader = null;
        projection = null;
        thread = null;
        AirLinkService.stopProjectionForeground();
        if (was != null) {
            nativeStopFrames(was);
            tell(was, "screen.stopped", why);
        }
    }

    static String peer() {
        return peer;
    }

    static Rect realBounds(Context c) {
        WindowManager wm = c.getSystemService(WindowManager.class);
        if (Build.VERSION.SDK_INT >= 30) return wm.getMaximumWindowMetrics().getBounds();
        DisplayMetrics m = new DisplayMetrics();
        wm.getDefaultDisplay().getRealMetrics(m);
        return new Rect(0, 0, m.widthPixels, m.heightPixels);
    }

    private static void tell(String peerId, String kind, String reason) {
        try {
            JSONObject body = new JSONObject();
            if (reason != null) body.put("reason", reason);
            if (kind.equals("screen.started")) body.put("width", width).put("height", height);
            SmsBridge.send(peerId, kind, body);
        } catch (Exception ignored) {
        }
    }

    private static native void nativeSendFrame(String peerId, byte[] jpeg);

    private static native void nativeStopFrames(String peerId);
}
