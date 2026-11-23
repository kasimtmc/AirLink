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
        long now = SystemClock.uptimeMillis();
        if (action == MotionEvent.ACTION_DOWN) downTime = now;
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

    /** A mouse wheel turn at x, y: v and h in notches. */
    static boolean scroll(int displayId, float x, float y, float h, float v) {
        long now = SystemClock.uptimeMillis();
        MotionEvent.PointerProperties[] props = {new MotionEvent.PointerProperties()};
        props[0].toolType = MotionEvent.TOOL_TYPE_MOUSE;
        MotionEvent.PointerCoords[] coords = {new MotionEvent.PointerCoords()};
        coords[0].x = x;
        coords[0].y = y;
        coords[0].setAxisValue(MotionEvent.AXIS_VSCROLL, v);
        coords[0].setAxisValue(MotionEvent.AXIS_HSCROLL, h);
        MotionEvent e = MotionEvent.obtain(now, now, MotionEvent.ACTION_SCROLL, 1, props, coords, 0, 0, 1f, 1f, 0, 0,
                InputDevice.SOURCE_MOUSE, 0);
        boolean ok = inject(e, displayId);
        e.recycle();
        return ok;
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
