package com.plyg.airlink

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.content.Intent
import android.graphics.Path
import android.provider.Settings
import android.view.accessibility.AccessibilityEvent
import org.json.JSONObject

/**
 * Carries out a paired computer's gestures while the phone is sharing its screen, and, when the
 * user flings the phone, finds what the app on screen shows (ForegroundContent).
 */
class AirLinkAccessibility : AccessibilityService() {
    override fun onServiceConnected() {
        instance = this
    }

    override fun onUnbind(intent: Intent): Boolean {
        instance = null
        return super.onUnbind(intent)
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent) = Unit

    override fun onInterrupt() = Unit

    companion object {
        @Volatile
        private var instance: AirLinkAccessibility? = null

        @JvmStatic
        fun enabled(): Boolean = instance != null

        /** For ForegroundContent: reading the screen when the user flings. */
        @JvmStatic
        fun service(): AccessibilityService? = instance

        @JvmStatic
        fun openSettings() {
            MainActivity.current()?.startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
        }

        /** Handles input on the UI thread. Coordinates are fractions of the shared screen. */
        @JvmStatic
        fun input(request: JSONObject) {
            val service = instance ?: return
            if (!ScreenShare.active()) return

            val type = request.optString("type")
            val action = when (type) {
                "back" -> GLOBAL_ACTION_BACK
                "home" -> GLOBAL_ACTION_HOME
                "recents" -> GLOBAL_ACTION_RECENTS
                "notifications" -> GLOBAL_ACTION_NOTIFICATIONS
                else -> null
            }
            if (action != null) {
                service.performGlobalAction(action)
                return
            }

            val screen = ScreenShare.realBounds(service)
            val path = Path()
            path.moveTo(
                request.optDouble("x", 0.5).toFloat() * screen.width(),
                request.optDouble("y", 0.5).toFloat() * screen.height(),
            )
            val duration = when (type) {
                "tap" -> 50L
                "long_press" -> 650L
                "swipe" -> {
                    path.lineTo(
                        request.optDouble("x2", 0.5).toFloat() * screen.width(),
                        request.optDouble("y2", 0.5).toFloat() * screen.height(),
                    )
                    request.optLong("ms", 250L).coerceIn(80L, 2000L)
                }
                else -> return
            }
            val gesture = GestureDescription.Builder()
                .addStroke(GestureDescription.StrokeDescription(path, 0L, duration))
                .build()
            service.dispatchGesture(gesture, null, null)
        }
    }
}
