package com.plyg.airlink

import android.app.Activity
import android.app.AlertDialog
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import org.json.JSONObject

/** Watches only the player packages the user selected. PCM never passes through AirLink. */
object PlayerBluetoothAudio {
    private val handler = Handler(Looper.getMainLooper())
    private var target: String? = null
    private var watchScreenPlayback = false
    private var context: Context? = null
    private var playing = false
    private var screenAudioActive = false
    private var ticks = 0
    private var lastStatus: String? = null
    private val poll = Runnable { checkPlayback() }

    private fun playerPreferences(context: Context) =
        context.getSharedPreferences("media_audio_players", Context.MODE_PRIVATE)

    @JvmStatic
    fun choosePlayers(activity: Activity) {
        val manager = activity.packageManager
        val launch = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        val apps = manager.queryIntentActivities(launch, 0)
            .filter { it.activityInfo.packageName != activity.packageName }
            .distinctBy { it.activityInfo.packageName }
            .sortedBy { it.loadLabel(manager).toString().lowercase() }
        val prefs = playerPreferences(activity)
        val saved = prefs.getStringSet("packages", emptySet()).orEmpty()
        val selected = BooleanArray(apps.size) { apps[it].activityInfo.packageName in saved }
        AlertDialog.Builder(activity)
            .setTitle("Ekran paylaşımı olmadan ses gönderebilen oynatıcılar")
            .setMultiChoiceItems(Array(apps.size) { apps[it].loadLabel(manager) }, selected) { _, i, on ->
                selected[i] = on
            }
            .setNegativeButton("Vazgeç", null)
            .setPositiveButton("Kaydet") { _, _ ->
                prefs.edit().putStringSet("packages", apps.indices.filter { selected[it] }
                    .map { apps[it].activityInfo.packageName }.toSet()).apply()
            }.show()
    }

    fun setTarget(context: Context, peer: String?) {
        if (target == peer) return
        target?.let { signal(it, false) }
        target = peer
        this.context = context.applicationContext
        playing = false
        ticks = 0
        lastStatus = null
        refreshPlayback()
    }

    fun setScreenPlaybackWatch(context: Context, enabled: Boolean) {
        if (watchScreenPlayback == enabled) return
        watchScreenPlayback = enabled
        this.context = context.applicationContext
        refreshPlayback()
    }

    fun setScreenAudioRoute(active: Boolean) {
        if (screenAudioActive == active) return
        screenAudioActive = active
        handler.removeCallbacks(poll)
        handler.post(poll)
    }

    fun stop() {
        handler.removeCallbacks(poll)
        target?.let { signal(it, false) }
        target = null
        watchScreenPlayback = false
        context?.let { ScreenShare.updatePlayerPlayback(it, false) }
        context = null
        playing = false
        ticks = 0
        lastStatus = null
    }

    private fun refreshPlayback() {
        handler.removeCallbacks(poll)
        checkPlayback()
    }

    private fun checkPlayback() {
        val ctx = context ?: return
        val peer = target
        if (peer == null && !watchScreenPlayback) {
            ScreenShare.updatePlayerPlayback(ctx, false)
            context = null
            return
        }
        val listener = ComponentName(ctx, AirLinkNotificationListener::class.java)
        val selected = playerPreferences(ctx).getStringSet("packages", emptySet()).orEmpty()
        val hasAccess = AirLinkNotificationListener.accessGranted()
        val playerIsPlaying = if (hasAccess && selected.isNotEmpty()) {
            val sessions = ctx.getSystemService(MediaSessionManager::class.java)
            try {
                sessions?.getActiveSessions(listener)?.any {
                    it.packageName in selected && it.playbackState?.state == PlaybackState.STATE_PLAYING
                } == true
            } catch (_: SecurityException) {
                false
            }
        } else false
        ScreenShare.updatePlayerPlayback(ctx, playerIsPlaying)
        if (peer != null) updateBluetooth(peer, playerIsPlaying, hasAccess, selected.isEmpty())
        // A screen route can change while applying playback state and queue an
        // immediate poll. Keep only one pending callback.
        handler.removeCallbacks(poll)
        handler.postDelayed(poll, 1000)
    }

    private fun updateBluetooth(peer: String, playerIsPlaying: Boolean,
                                hasAccess: Boolean, noPlayersSelected: Boolean) {
        val matches = playerIsPlaying && !screenAudioActive
        // Repeat the lease while playing so the PC releases Bluetooth even if the
        // phone disappears before it can send the final stop message.
        if (matches != playing || (matches && ++ticks >= 3)) {
            signal(peer, matches)
            ticks = 0
        }
        if (!matches || !playing) {
            val status = when {
                screenAudioActive && !matches -> "Ses ekran paylaşımı üzerinden aktarılıyor"
                !hasAccess -> "Oynatmayı algılamak için AirLink bildirim erişimi gerekiyor"
                noPlayersSelected -> "Oynatıcıları seç menüsünden bir uygulama seçin"
                matches -> "Bluetooth medya bağlantısı bekleniyor"
                else -> "Seçili oynatıcıların oynatması bekleniyor"
            }
            if (status != lastStatus) {
                lastStatus = status
                nativeStatus(status)
            }
        }
        playing = matches
    }

    private fun signal(peer: String, active: Boolean) {
        SmsBridge.send(peer, "player.audio", JSONObject().put("playing", active))
    }

    @JvmStatic private external fun nativeStatus(status: String)
}
