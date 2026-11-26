package com.plyg.airlink

import android.content.ComponentName
import android.content.Context
import android.graphics.Bitmap
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.util.Base64
import org.json.JSONObject
import java.io.ByteArrayOutputStream

/**
 * What plays on the phone, for the computer's tray panel: `media.state` (title, artist, cover,
 * playing, position) whenever it changes, and `media.control` (play, pause, next, previous) from
 * the computer. Reads the media sessions through the notification access AirLink already has.
 */
object PhoneMedia {
    private val main = Handler(Looper.getMainLooper())
    private var context: Context? = null
    private var watched: MediaController? = null
    private var lastSent = ""
    private var lastArtKey = ""
    private var lastArt = ""

    private val sessionsChanged = MediaSessionManager.OnActiveSessionsChangedListener { watch() }
    private val callback = object : MediaController.Callback() {
        override fun onPlaybackStateChanged(state: PlaybackState?) = publish(null)
        override fun onMetadataChanged(metadata: MediaMetadata?) = publish(null)
        override fun onSessionDestroyed() = watch()
    }

    /** The service runs: follow the sessions (once notification access is granted). On the main thread. */
    @JvmStatic
    fun start(context: Context) {
        this.context = context.applicationContext
        main.post { listen() }
    }

    private fun listen(): Boolean {
        val ctx = context ?: return false
        if (!AirLinkNotificationListener.accessGranted()) return false
        val sessions = ctx.getSystemService(MediaSessionManager::class.java) ?: return false
        return try {
            sessions.removeOnActiveSessionsChangedListener(sessionsChanged)
            sessions.addOnActiveSessionsChangedListener(sessionsChanged, listener(ctx), main)
            watch()
            true
        } catch (_: SecurityException) { false }
    }

    private fun listener(ctx: Context) = ComponentName(ctx, AirLinkNotificationListener::class.java)

    /** The session to show: the one playing, else the most recent. */
    private fun pick(): MediaController? {
        val ctx = context ?: return null
        val list = try {
            ctx.getSystemService(MediaSessionManager::class.java)?.getActiveSessions(listener(ctx)).orEmpty()
        } catch (_: SecurityException) { emptyList() }
        return list.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
            ?: list.firstOrNull { it.metadata != null }
    }

    private fun watch() {
        val next = pick()
        if (next?.sessionToken != watched?.sessionToken) {
            watched?.unregisterCallback(callback)
            watched = next
            next?.registerCallback(callback, main)
        }
        publish(null)
    }

    private fun state(): JSONObject {
        val controller = watched ?: return JSONObject().put("active", false)
        val ctx = context
        val metadata = controller.metadata
        val playback = controller.playbackState
        val label = try {
            ctx?.packageManager?.let { pm -> pm.getApplicationLabel(pm.getApplicationInfo(controller.packageName, 0)).toString() }
        } catch (_: Exception) { null } ?: controller.packageName
        val title = metadata?.getString(MediaMetadata.METADATA_KEY_TITLE) ?: metadata?.description?.title?.toString().orEmpty()
        val artist = metadata?.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: metadata?.description?.subtitle?.toString().orEmpty()
        val actions = playback?.actions ?: 0L
        val out = JSONObject()
            .put("active", true)
            .put("app", label)
            .put("package", controller.packageName)
            .put("title", title)
            .put("artist", artist)
            .put("album", metadata?.getString(MediaMetadata.METADATA_KEY_ALBUM).orEmpty())
            .put("playing", playback?.state == PlaybackState.STATE_PLAYING)
            .put("position", playback?.position ?: 0L)
            .put("position_at", System.currentTimeMillis())
            .put("duration", metadata?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L)
            .put("can_next", actions and PlaybackState.ACTION_SKIP_TO_NEXT != 0L)
            .put("can_previous", actions and PlaybackState.ACTION_SKIP_TO_PREVIOUS != 0L)
        val artKey = "${controller.packageName}|$title|$artist"
        if (artKey != lastArtKey) {
            lastArtKey = artKey
            val bitmap = metadata?.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
                ?: metadata?.getBitmap(MediaMetadata.METADATA_KEY_ART)
                ?: metadata?.description?.iconBitmap
            lastArt = bitmap?.let(::cover).orEmpty()
        }
        return out.put("art", lastArt)
    }

    /** A small JPEG of the cover, base64. */
    private fun cover(bitmap: Bitmap): String {
        val side = 160
        val scaled = Bitmap.createScaledBitmap(bitmap, side, side * bitmap.height / bitmap.width.coerceAtLeast(1), true)
        val bytes = ByteArrayOutputStream().also { scaled.compress(Bitmap.CompressFormat.JPEG, 80, it) }.toByteArray()
        return Base64.encodeToString(bytes, Base64.NO_WRAP)
    }

    /** To `peer`, or to every computer when it changed (peer null). */
    private fun publish(peer: String?) {
        val body = state()
        // The position moves on its own: a change of anything else is news.
        val key = JSONObject(body.toString()).apply { remove("position"); remove("position_at") }.toString()
        if (peer == null && key == lastSent) return
        lastSent = key
        if (peer == null) SmsBridge.broadcastAll("media.state", body.toString()) else SmsBridge.send(peer, "media.state", body)
    }

    @JvmStatic
    fun handle(peerId: String, kind: String, request: JSONObject) = main.post {
        if (watched == null) listen()
        when (kind) {
            "media.state_request" -> publish(peerId)
            "media.control" -> {
                val controls = watched?.transportControls
                when (request.optString("action")) {
                    "play" -> controls?.play()
                    "pause" -> controls?.pause()
                    "toggle" -> if (watched?.playbackState?.state == PlaybackState.STATE_PLAYING) controls?.pause() else controls?.play()
                    "next" -> controls?.skipToNext()
                    "previous" -> controls?.skipToPrevious()
                }
            }
        }
    }
}
