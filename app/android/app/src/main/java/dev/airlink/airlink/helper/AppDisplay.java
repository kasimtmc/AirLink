package dev.airlink.airlink.helper;

import android.annotation.SuppressLint;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;

/**
 * A display of its own for one app, invisible on the phone: the app is started on it, its frames
 * are taken as JPEG images, and input is injected into it. Trusted, so other apps' activities may
 * run on it — which only a privileged process (the shell, here) may create.
 */
@SuppressLint("WrongConstant")
final class AppDisplay {
    // DisplayManager's flags; the hidden ones by value.
    private static final int PUBLIC = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC;
    private static final int OWN_CONTENT_ONLY = DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY;
    private static final int SUPPORTS_TOUCH = 1 << 6;
    private static final int ROTATES_WITH_CONTENT = 1 << 7;
    private static final int DESTROY_CONTENT_ON_REMOVAL = 1 << 8;
    private static final int TRUSTED = 1 << 10;
    private static final int OWN_DISPLAY_GROUP = 1 << 11;
    private static final int ALWAYS_UNLOCKED = 1 << 12;
    private static final int TOUCH_FEEDBACK_DISABLED = 1 << 13;

    interface FrameSink {
        void frame(AppDisplay display, byte[] jpeg);
    }

    final int width, height, dpi;
    private final VirtualDisplay virtualDisplay;
    private final ImageReader reader;
    private final HandlerThread thread;
    private final FrameSink sink;
    private volatile int quality = 70;
    private volatile int maxFps = 30;
    private long lastFrame;
    private int frames;

    AppDisplay(String name, int width, int height, int dpi, FrameSink sink) throws Exception {
        this.width = width;
        this.height = height;
        this.dpi = dpi;
        this.sink = sink;
        thread = new HandlerThread("airlink-display-" + name);
        thread.start();
        Handler handler = new Handler(thread.getLooper());
        reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2);
        reader.setOnImageAvailableListener(r -> onImage(), handler);

        int flags = PUBLIC | OWN_CONTENT_ONLY | SUPPORTS_TOUCH | ROTATES_WITH_CONTENT | DESTROY_CONTENT_ON_REMOVAL;
        if (Build.VERSION.SDK_INT >= 33) flags |= TRUSTED | OWN_DISPLAY_GROUP | ALWAYS_UNLOCKED | TOUCH_FEEDBACK_DISABLED;
        // Not SHOULD_SHOW_SYSTEM_DECORATIONS: with it the system starts its launcher and bars there, and
        // the app's first frame waits ~5 s for them (the transition's timeout). Text is typed from the computer.

        DisplayManager dm = DisplayManager.class.getDeclaredConstructor(android.content.Context.class)
                .newInstance(Workarounds.context());
        virtualDisplay = dm.createVirtualDisplay("airlink-" + name, width, height, dpi, reader.getSurface(), flags);
        if (virtualDisplay == null) throw new IllegalStateException("createVirtualDisplay returned null");
    }

    int id() {
        return virtualDisplay.getDisplay().getDisplayId();
    }

    void setQuality(int q, int fps) {
        quality = Math.max(30, Math.min(95, q));
        maxFps = Math.max(1, Math.min(60, fps));
    }

    private void onImage() {
        Image image = reader.acquireLatestImage();
        if (frames++ < 3) Log.i("display " + width + "x" + height + ": image " + (image != null));
        if (image == null) return;
        try {
            long now = System.currentTimeMillis();
            if (now - lastFrame < 1000 / maxFps) return;
            lastFrame = now;
            Image.Plane plane = image.getPlanes()[0];
            ByteBuffer buf = plane.getBuffer();
            int padded = plane.getRowStride() / plane.getPixelStride();
            Bitmap bmp = Bitmap.createBitmap(padded, height, Bitmap.Config.ARGB_8888);
            bmp.copyPixelsFromBuffer(buf);
            Bitmap cropped = padded == width ? bmp : Bitmap.createBitmap(bmp, 0, 0, width, height);
            ByteArrayOutputStream out = new ByteArrayOutputStream(256 * 1024);
            cropped.compress(Bitmap.CompressFormat.JPEG, quality, out);
            if (cropped != bmp) cropped.recycle();
            bmp.recycle();
            sink.frame(this, out.toByteArray());
        } finally {
            image.close();
        }
    }

    void release() {
        virtualDisplay.release();
        reader.close();
        thread.quitSafely();
    }
}
