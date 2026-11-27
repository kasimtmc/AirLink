package com.plyg.airlink.helper

import android.content.ClipDescription
import android.content.ClipboardManager
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import org.json.JSONObject

/**
 * The phone's clipboard as other apps copy to it, with AirLink in the background or closed:
 * since Android 10 only the app in front (and the keyboard) may read it, but the shell user this
 * helper runs as holds READ_CLIPBOARD_IN_BACKGROUND. Each new text goes to the app as a
 * {"event":"clipboard"} (it sends it on to the trusted computers, as it does with the app open).
 *
 * The clip's description is looked at a few times a second (it carries the copy's time, and
 * reading it shows no "pasted" notice); the clip itself is read once per copy.
 */
object ClipboardWatch {
    @Volatile private var started = false

    fun start(send: (JSONObject) -> Unit) {
        synchronized(this) {
            if (started) return
            started = true
        }
        Thread({ run(send) }, "airlink-clipboard").apply { isDaemon = true }.start()
    }

    private fun run(send: (JSONObject) -> Unit) {
        val context = Workarounds.context()
        // Made on the shell's context (getSystemService would give the system context's, which
        // asks as the package "android"): its hidden constructor.
        val clipboard = try {
            ClipboardManager::class.java.getDeclaredConstructor(Context::class.java, Handler::class.java)
                .apply { isAccessible = true }
                .newInstance(context, Handler(Looper.getMainLooper()))
        } catch (error: Exception) {
            Log.e("clipboard: no ClipboardManager", error)
            return
        }
        Log.i("clipboard: watching")
        var lastStamp = Long.MIN_VALUE
        var failing = false
        while (true) {
            try {
                val description = clipboard.primaryClipDescription
                val stamp = description?.timestamp ?: 0L
                if (description != null && stamp != lastStamp) {
                    lastStamp = stamp
                    if (description.hasMimeType(ClipDescription.MIMETYPE_TEXT_PLAIN) || description.hasMimeType("text/*")) {
                        val text = clipboard.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(context)?.toString().orEmpty()
                        // A password manager's copy (Android 13+: the clip says so).
                        val sensitive = Build.VERSION.SDK_INT >= 33 &&
                            description.extras?.getBoolean(ClipDescription.EXTRA_IS_SENSITIVE) == true
                        if (text.isNotEmpty()) send(JSONObject().put("event", "clipboard").put("text", text).put("sensitive", sensitive))
                    }
                }
                failing = false
            } catch (error: Exception) {
                if (!failing) Log.e("clipboard read failed", error)
                failing = true
            }
            try {
                Thread.sleep(700)
            } catch (_: InterruptedException) {
                return
            }
        }
    }
}
