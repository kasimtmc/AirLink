package dev.airlink.airlink;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.content.Intent;
import android.graphics.Path;
import android.graphics.Rect;
import android.provider.Settings;
import android.view.accessibility.AccessibilityEvent;

import org.json.JSONObject;

/**
 * Carries out a paired computer's touches on this phone while its screen is shared: taps, long
 * presses, swipes and the system's Back / Home / Recents. Turned on by the user in Accessibility.
 */
public class AirLinkAccessibility extends AccessibilityService {
    private static volatile AirLinkAccessibility instance;

    @Override
    protected void onServiceConnected() {
        instance = this;
    }

    @Override
    public boolean onUnbind(Intent intent) {
        instance = null;
        return super.onUnbind(intent);
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {}

    @Override
    public void onInterrupt() {}

    static boolean enabled() {
        return instance != null;
    }

    static void openSettings() {
        MainActivity a = MainActivity.current();
        if (a != null) a.startActivity(new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS));
    }

    /** A computer's input (UI thread); positions are fractions (0..1) of the screen. */
    static void input(JSONObject req) {
        AirLinkAccessibility s = instance;
        android.util.Log.d("AirLink", "screen input " + req + " (service: " + (s != null) + ", sharing: " + ScreenShare.active() + ")");
        if (s == null || !ScreenShare.active()) return;  // only while the screen is shared
        switch (req.optString("type")) {
            case "back": s.performGlobalAction(GLOBAL_ACTION_BACK); return;
            case "home": s.performGlobalAction(GLOBAL_ACTION_HOME); return;
            case "recents": s.performGlobalAction(GLOBAL_ACTION_RECENTS); return;
            case "notifications": s.performGlobalAction(GLOBAL_ACTION_NOTIFICATIONS); return;
            default: break;
        }
        Rect screen = ScreenShare.realBounds(s);
        float x1 = (float) req.optDouble("x", 0.5) * screen.width(), y1 = (float) req.optDouble("y", 0.5) * screen.height();
        Path path = new Path();
        path.moveTo(x1, y1);
        long duration;
        switch (req.optString("type")) {
            case "tap": duration = 50; break;
            case "long_press": duration = 650; break;
            case "swipe": {
                float x2 = (float) req.optDouble("x2", 0.5) * screen.width(), y2 = (float) req.optDouble("y2", 0.5) * screen.height();
                path.lineTo(x2, y2);
                duration = Math.max(80, Math.min(2000, req.optLong("ms", 250)));
                break;
            }
            default: return;
        }
        s.dispatchGesture(new GestureDescription.Builder()
                .addStroke(new GestureDescription.StrokeDescription(path, 0, duration)).build(), null, null);
    }
}
