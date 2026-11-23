package dev.airlink.airlink.helper;

import android.annotation.SuppressLint;
import android.os.Build;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.lang.reflect.Method;

/**
 * Real input events (as a touchscreen and a keyboard would send them) injected into one display:
 * what only a privileged process may do. Through InputManager's hidden injectInputEvent, as scrcpy.
 */
@SuppressLint({"PrivateApi", "DiscouragedPrivateApi", "BlockedPrivateApi"})
final class Input {
    private static final int INJECT_ASYNC = 0;
    private static Object manager;
    private static Method inject, setDisplayId;

    private Input() {}

    private static void init() throws Exception {
        if (manager != null) return;
        if (Build.VERSION.SDK_INT >= 34) {
            Class<?> global = Class.forName("android.hardware.input.InputManagerGlobal");
            manager = global.getDeclaredMethod("getInstance").invoke(null);
            inject = global.getMethod("injectInputEvent", InputEvent.class, int.class);
        } else {
            Class<?> im = Class.forName("android.hardware.input.InputManager");
            manager = im.getDeclaredMethod("getInstance").invoke(null);
            inject = im.getMethod("injectInputEvent", InputEvent.class, int.class);
        }
        setDisplayId = InputEvent.class.getMethod("setDisplayId", int.class);
    }

    static boolean inject(InputEvent event, int displayId) {
        try {
            init();
            setDisplayId.invoke(event, displayId);
            return (Boolean) inject.invoke(manager, event, INJECT_ASYNC);
        } catch (Exception e) {
            Log.e("inject failed", e);
            return false;
        }
    }

    private static long downTime;

    /** One pointer: action is MotionEvent.ACTION_DOWN, ACTION_MOVE or ACTION_UP; x, y in pixels. */
    static boolean touch(int displayId, int action, float x, float y) {
        if (action == MotionEvent.ACTION_DOWN) downTime = SystemClock.uptimeMillis();
        return touch(displayId, action, x, y, downTime);
    }

    // A finger's event; downTime: when it went down.
    private static boolean touch(int displayId, int action, float x, float y, long downTime) {
        long now = SystemClock.uptimeMillis();
        MotionEvent.PointerProperties[] props = {new MotionEvent.PointerProperties()};
        props[0].id = 0;
        props[0].toolType = MotionEvent.TOOL_TYPE_FINGER;
        MotionEvent.PointerCoords[] coords = {new MotionEvent.PointerCoords()};
        coords[0].x = x;
        coords[0].y = y;
        coords[0].pressure = action == MotionEvent.ACTION_UP ? 0f : 1f;
        coords[0].size = 1f;
        MotionEvent e = MotionEvent.obtain(downTime, now, action, 1, props, coords, 0, 0, 1f, 1f, 0, 0,
                InputDevice.SOURCE_TOUCHSCREEN, 0);
        boolean ok = inject(e, displayId);
        e.recycle();
        return ok;
    }

    /**
     * A mouse wheel turn at x, y (v and h in notches, positive = up / left), as a finger of its own:
     * Android refuses injected ACTION_SCROLL events on these displays (Android 16, HyperOS). While the
     * wheel turns the finger stays down and moves; once it rests the finger is lifted, still, so the
     * content doesn't fling on. On a thread of its own: the commands don't wait for it.
     */
    static void scroll(int displayId, float x, float y, float h, float v, int width, int height) {
        // Turns that come faster than they're played add up: the finger always goes where the wheel
        // is now, nothing waits in a queue (which kept scrolling after the wheel had stopped).
        synchronized (pending) {
            Pending p = pending.computeIfAbsent(displayId, k -> new Pending());
            p.h += h;
            p.v += v;
            p.x = x;
            p.y = y;
            p.width = width;
            p.height = height;
            if (p.queued) return;
            p.queued = true;
        }
        wheel.execute(() -> play(displayId));
    }

    private static final class Pending {
        float h, v, x, y;
        int width, height;
        boolean queued;
    }

    private static final java.util.Map<Integer, Pending> pending = new java.util.HashMap<>();

    private static void play(int displayId) {
        float h, v, x, y;
        int width, height;
        synchronized (pending) {
            Pending p = pending.get(displayId);
            h = p.h;
            v = p.v;
            x = p.x;
            y = p.y;
            width = p.width;
            height = p.height;
            p.h = p.v = 0;
            p.queued = false;
        }
        Finger f = fingers.get(displayId);
        float dx = h * width * 0.10f, dy = v * height * 0.07f;
        if (f != null && f.lift != null) f.lift.cancel(false);
        if (f != null) {
            float tx = f.x + dx, ty = f.y + dy;
            if (tx < width * 0.05f || tx > width * 0.95f || ty < height * 0.05f || ty > height * 0.95f) {
                lift(displayId, f);  // out of room: a new stroke
                fingers.remove(displayId);
                f = null;
            }
        }
        if (f == null) {
            f = new Finger();
            // Where the stroke has the most room to go.
            f.x = h == 0 ? x : (dx < 0 ? width * 0.85f : width * 0.15f);
            f.y = v == 0 ? y : (dy < 0 ? height * 0.85f : height * 0.15f);
            f.downTime = SystemClock.uptimeMillis();
            touch(displayId, MotionEvent.ACTION_DOWN, f.x, f.y, f.downTime);
            fingers.put(displayId, f);
        }
        f.x = clamp(f.x + dx, width);
        f.y = clamp(f.y + dy, height);
        touch(displayId, MotionEvent.ACTION_MOVE, f.x, f.y, f.downTime);
        Finger finger = f;
        f.lift = wheel.schedule(() -> {
            if (fingers.get(displayId) == finger) {
                fingers.remove(displayId);
                lift(displayId, finger);
            }
        }, 120, java.util.concurrent.TimeUnit.MILLISECONDS);
    }

    private static final class Finger {
        float x, y;
        long downTime;
        java.util.concurrent.ScheduledFuture<?> lift;
    }

    private static final java.util.concurrent.ScheduledExecutorService wheel =
            java.util.concurrent.Executors.newSingleThreadScheduledExecutor(r -> new Thread(r, "airlink-wheel"));
    private static final java.util.Map<Integer, Finger> fingers = new java.util.HashMap<>();  // the wheel thread's

    private static void lift(int displayId, Finger f) {
        touch(displayId, MotionEvent.ACTION_MOVE, f.x, f.y, f.downTime);  // still a moment: no fling
        sleep(30);
        touch(displayId, MotionEvent.ACTION_MOVE, f.x, f.y, f.downTime);
        touch(displayId, MotionEvent.ACTION_UP, f.x, f.y, f.downTime);
    }

    private static float clamp(float v, int size) {
        return Math.max(1, Math.min(size - 2, v));
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
        }
    }

    /** A key press and release (KeyEvent.KEYCODE_*), with meta state (shift, ctrl...). */
    static boolean key(int displayId, int keyCode, int meta) {
        long now = SystemClock.uptimeMillis();
        KeyEvent down = new KeyEvent(now, now, KeyEvent.ACTION_DOWN, keyCode, 0, meta, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0,
                InputDevice.SOURCE_KEYBOARD);
        KeyEvent up = new KeyEvent(now, now, KeyEvent.ACTION_UP, keyCode, 0, meta, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0,
                InputDevice.SOURCE_KEYBOARD);
        return inject(down, displayId) && inject(up, displayId);
    }

    /** Text, as the keys that type it (letters outside the keyboard map are skipped). */
    static boolean text(int displayId, String text) {
        KeyCharacterMap map = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);
        KeyEvent[] events = map.getEvents(text.toCharArray());
        if (events == null) return false;
        for (KeyEvent e : events) inject(e, displayId);
        return true;
    }
}
