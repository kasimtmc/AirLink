package com.plyg.airlink

import android.app.Activity
import android.app.Notification
import android.app.NotificationManager
import android.app.PendingIntent
import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.graphics.Rect
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.DisplayMetrics
import android.view.WindowManager
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.util.concurrent.Executors

/** Shares the phone's display after the system MediaProjection consent prompt. */
object ScreenShare {
    const val REQUEST = 7101
    const val AUDIO_PERMISSION = 7102
    private const val NOTIFICATION = 1500
    private const val AUDIO_NOTIFICATION = 1501
    private const val MAX_FPS = 15
    private const val ROUTE_PREFS = "screen_audio_route"
    private const val MUTED_BY_AIRLINK = "owns_music_mute"

    private var pendingPeer: String? = null
    private var pauseForSelectedPlayers = true
    private var selectedPlayerPlaying = false
    private var captureScheduled = false
    private var mutedContext: Context? = null
    private var systemConsentOpen = false
    @Volatile private var peer: String? = null
    private var projection: MediaProjection? = null
    private var display: VirtualDisplay? = null
    private val audioLock = Any()
    private val audioWorker = Executors.newSingleThreadExecutor { task -> Thread(task, "airlink-media-audio") }
    private val main = Handler(Looper.getMainLooper())
    private var reader: ImageReader? = null
    private var thread: HandlerThread? = null
    private var lastFrame = 0L
    private var width = 0
    private var height = 0
    private var quality = 60
    private var codec = "jpeg"
    private var encoder: H264Encoder? = null
    private var codecFailed = false
    @Volatile private var screenAudio = false
    @Volatile private var audioGeneration = 0
    @Volatile private var audioRecord: AudioRecord? = null
    @Volatile private var videoRequested = false

    @JvmStatic fun active(): Boolean = videoRequested && display != null

    @JvmStatic
    fun requested(context: Context, peerId: String, request: JSONObject) {
        if (systemConsentOpen || pendingPeer != null || (projection != null && (peer != peerId || active()))) {
            tell(peerId, "screen.stopped", "busy")
            return
        }
        quality = request.optInt("quality", 60).coerceIn(30, 90)
        codec = request.optString("codec", "jpeg")
        codecFailed = false
        pendingPeer = peerId
        val activity = MainActivity.current()
        if (activity != null && MainActivity.visible) {
            askConsent(activity)
            return
        }
        val open = Intent(context, MainActivity::class.java).setAction("com.plyg.airlink.SHARE_SCREEN")
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        val builder = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(context, "requests") else Notification.Builder(context)
        val notification = builder.setSmallIcon(R.drawable.ic_stat_airlink)
            .setContentTitle("Bilgisayar ekranınızı görmek istiyor")
            .setContentText("Başlatmak için dokunun")
            .setContentIntent(PendingIntent.getActivity(context, NOTIFICATION, open, PendingIntent.FLAG_IMMUTABLE))
            .setAutoCancel(true).build()
        context.getSystemService(NotificationManager::class.java)?.notify(NOTIFICATION, notification)
    }

    @JvmStatic
    fun setScreenAudio(context: Context, enabled: Boolean) {
        if (screenAudio == enabled) return
        screenAudio = enabled
        updateScreenAudioPolicy(context)
    }

    fun setPauseForSelectedPlayers(context: Context, pause: Boolean) {
        if (pauseForSelectedPlayers == pause) return
        pauseForSelectedPlayers = pause
        updateScreenAudioPolicy(context)
    }

    private fun updateScreenAudioPolicy(context: Context) {
        val watch = active() && screenAudio && pauseForSelectedPlayers
        PlayerBluetoothAudio.setScreenPlaybackWatch(context, watch)
        updateCapture(context)
    }

    private fun wantsScreenAudio(): Boolean =
        active() && screenAudio && !(pauseForSelectedPlayers && selectedPlayerPlaying)

    fun updatePlayerPlayback(context: Context, playing: Boolean) {
        if (selectedPlayerPlaying == playing) return
        selectedPlayerPlaying = playing
        if (active() && screenAudio) updateCapture(context)
    }

    private fun updateCapture(context: Context) {
        if (wantsScreenAudio()) {
            if (!captureScheduled) startAudio(context)
        } else if (captureScheduled) {
            stopAudio()
        }
    }

    @JvmStatic
    fun stopVideo(from: String) {
        if (pendingPeer == from) {
            pendingPeer = null
            return
        }
        if (peer == from) stop("computer")
    }

    @JvmStatic
    fun askConsent(activity: Activity) {
        if (pendingPeer == null || systemConsentOpen) return
        activity.getSystemService(NotificationManager::class.java)?.cancel(NOTIFICATION)
        val manager = activity.getSystemService(MediaProjectionManager::class.java) ?: return
        systemConsentOpen = true
        activity.startActivityForResult(manager.createScreenCaptureIntent(), REQUEST)
    }

    @JvmStatic
    fun consentResult(activity: Activity, resultCode: Int, data: Intent?) {
        systemConsentOpen = false
        val asked = pendingPeer ?: return
        pendingPeer = null
        if (resultCode != Activity.RESULT_OK || data == null) {
            tell(asked, "screen.stopped", "declined")
            return
        }
        AirLinkService.startProjectionForeground(activity)
        val manager = activity.getSystemService(MediaProjectionManager::class.java) ?: return
        projection = manager.getMediaProjection(resultCode, data)
        if (projection == null) {
            tell(asked, "screen.stopped", "failed")
            stop("failed")
            return
        }
        peer = asked
        videoRequested = true
        start(activity.applicationContext)
    }

    private fun start(context: Context) {
        val thread = HandlerThread("airlink-screen").also { it.start() }
        this.thread = thread
        val handler = Handler(thread.looper)
        val projection = projection ?: return
        projection.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                Handler(Looper.getMainLooper()).post {
                    if (ScreenShare.projection === projection) stop("user")
                }
            }
        }, handler)
        startVideo(context, handler)
        updateScreenAudioPolicy(context)
    }

    private fun startVideo(context: Context, handler: Handler) {
        val projection = projection ?: return
        val bounds = realBounds(context)
        val scale = minOf(1f, 1280f / maxOf(bounds.width(), bounds.height()))
        width = (bounds.width() * scale).toInt() and -8
        height = (bounds.height() * scale).toInt() and -8
        val dpi = context.resources.displayMetrics.densityDpi
        val reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2)
        this.reader = reader
        reader.setOnImageAvailableListener({ frame(it) }, handler)
        display = projection.createVirtualDisplay("AirLink", width, height, dpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, reader.surface, null, handler)
        peer?.let { tell(it, "screen.started", null) }
    }

    private fun startAudio(context: Context) {
        val target = peer ?: return
        val projection = projection ?: return
        if (!wantsScreenAudio()) return
        captureScheduled = true
        if (Build.VERSION.SDK_INT < 29) {
            screenAudioFailed()
            return
        }
        if (context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            Handler(Looper.getMainLooper()).postDelayed({
                if (!wantsScreenAudio() || peer != target || this.projection !== projection) return@postDelayed
                val activity = MainActivity.current()
                if (activity != null && MainActivity.visible) {
                    askAudioPermission(activity)
                    return@postDelayed
                }
                val open = Intent(context, MainActivity::class.java).setAction("com.plyg.airlink.SHARE_AUDIO")
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                val builder = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(context, "requests") else Notification.Builder(context)
                context.getSystemService(NotificationManager::class.java)?.notify(AUDIO_NOTIFICATION,
                    builder.setSmallIcon(R.drawable.ic_stat_airlink).setContentTitle("Medya sesini paylaş")
                        .setContentText("Ses izni vermek için dokunun")
                        .setContentIntent(PendingIntent.getActivity(context, AUDIO_NOTIFICATION, open,
                            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)).setAutoCancel(true).build())
            }, 300)
            return
        }
        val generation = audioGeneration
        audioWorker.execute {
            if (generation == audioGeneration) captureAudio(context, projection, target, generation)
        }
    }

    private fun screenAudioFailed() {
        screenAudio = false
        AirLinkService.appContext?.let { PlayerBluetoothAudio.setScreenPlaybackWatch(it, false) }
        stopAudio()
        if (MainActivity.nativeReady) nativeScreenAudioFailed()
    }

    @JvmStatic fun askAudioPermission(activity: Activity) {
        if (!wantsScreenAudio() || projection == null) return
        activity.getSystemService(NotificationManager::class.java)?.cancel(AUDIO_NOTIFICATION)
        if (activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED)
            audioPermissionResult(activity)
        else activity.requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), AUDIO_PERMISSION)
    }

    @JvmStatic fun audioPermissionResult(activity: Activity) {
        if (!wantsScreenAudio() || projection == null) return
        if (activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            captureScheduled = false
            updateCapture(activity.applicationContext)
        } else screenAudioFailed()
    }

    private fun captureAudio(context: Context, projection: MediaProjection, target: String, generation: Int) {
        var record: AudioRecord? = null
        try {
            val config = AudioPlaybackCaptureConfiguration.Builder(projection)
                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            val format = AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_IN_MONO).build()
            val size = maxOf(19200, AudioRecord.getMinBufferSize(48000, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT))
            val capture = AudioRecord.Builder().setAudioFormat(format).setBufferSizeInBytes(size)
                .setAudioPlaybackCaptureConfig(config.build()).build()
            record = capture
            synchronized(audioLock) {
                if (generation != audioGeneration) return
                capture.startRecording()
                check(capture.recordingState == AudioRecord.RECORDSTATE_RECORDING)
                audioRecord = capture
            }
            main.post {
                if (generation != audioGeneration || !wantsScreenAudio()) return@post
                PlayerBluetoothAudio.setScreenAudioRoute(true)
                if (!muteMusicOutput(context.applicationContext)) screenAudioFailed()
            }
            val buffer = ByteArray(9600) // 100 ms, PCM 48 kHz mono
            while (generation == audioGeneration) {
                val count = capture.read(buffer, 0, buffer.size, AudioRecord.READ_BLOCKING)
                if (count <= 0) {
                    if (generation == audioGeneration) error("AudioRecord.read: $count")
                    break
                }
                synchronized(audioLock) {
                    if (generation != audioGeneration || !MainActivity.nativeReady) return
                    nativeSendAudio(target, if (count == buffer.size) buffer else buffer.copyOf(count))
                }
            }
        } catch (error: Exception) {
            android.util.Log.w("AirLink", "media playback capture failed", error)
            main.post {
                if (generation == audioGeneration) screenAudioFailed()
            }
        } finally {
            synchronized(audioLock) {
                if (audioRecord === record) audioRecord = null
                try { record?.stop() } catch (_: Exception) { }
                record?.release()
            }
        }
    }

    private fun muteMusicOutput(context: Context): Boolean {
        val audio = context.getSystemService(AudioManager::class.java) ?: return false
        val stream = AudioManager.STREAM_MUSIC
        if (audio.isStreamMute(stream)) return true
        val prefs = context.getSharedPreferences(ROUTE_PREFS, Context.MODE_PRIVATE)
        // Persist ownership before muting so service startup can restore after process death.
        if (!prefs.edit().putBoolean(MUTED_BY_AIRLINK, true).commit()) return false
        mutedContext = context
        return try {
            audio.adjustStreamVolume(stream, AudioManager.ADJUST_MUTE, 0)
            audio.isStreamMute(stream)
        } catch (_: SecurityException) {
            false
        }
    }

    @JvmStatic
    fun restoreMusicOutput(context: Context) {
        val prefs = context.getSharedPreferences(ROUTE_PREFS, Context.MODE_PRIVATE)
        if (!prefs.getBoolean(MUTED_BY_AIRLINK, false)) return
        val audio = context.getSystemService(AudioManager::class.java) ?: return
        try {
            if (audio.isStreamMute(AudioManager.STREAM_MUSIC))
                audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_UNMUTE, 0)
            prefs.edit().remove(MUTED_BY_AIRLINK).commit()
        } catch (error: SecurityException) {
            android.util.Log.w("AirLink", "could not restore media output", error)
        }
    }

    private fun stopAudio() {
        captureScheduled = false
        synchronized(audioLock) {
            audioGeneration++
            try { audioRecord?.stop() } catch (_: Exception) { }
            audioRecord = null
            peer?.let { if (MainActivity.nativeReady) nativeStopAudio(it) }
        }
        mutedContext?.let(::restoreMusicOutput)
        mutedContext = null
        PlayerBluetoothAudio.setScreenAudioRoute(false)
    }

    private fun frame(reader: ImageReader) {
        val image = reader.acquireLatestImage() ?: return
        try {
            if (!videoRequested) return
            val now = System.currentTimeMillis()
            val target = peer ?: return
            if (now - lastFrame < 1000 / MAX_FPS) return
            lastFrame = now
            val plane = image.planes[0]
            val padded = plane.rowStride / plane.pixelStride
            val bitmap = Bitmap.createBitmap(padded, height, Bitmap.Config.ARGB_8888)
            bitmap.copyPixelsFromBuffer(plane.buffer)
            val cropped = if (padded == width) bitmap else Bitmap.createBitmap(bitmap, 0, 0, width, height)
            val packet = if (codec == "h264" && !codecFailed) {
                try {
                    if (encoder == null) encoder = H264Encoder(width, height, MAX_FPS, quality)
                    encoder?.encode(cropped)
                } catch (error: Exception) {
                    android.util.Log.w("AirLink", "H.264 screen encoder failed; using JPEG", error)
                    try { encoder?.close() } catch (_: Exception) { }
                    encoder = null
                    codecFailed = true
                    null
                }
            } else null
            val output = if (packet == null && (codec != "h264" || codecFailed)) {
                ByteArrayOutputStream(128 * 1024).also { cropped.compress(Bitmap.CompressFormat.JPEG, quality, it) }.toByteArray()
            } else packet
            if (cropped !== bitmap) cropped.recycle()
            bitmap.recycle()
            if (output != null && videoRequested && MainActivity.nativeReady) nativeSendFrame(target, output)
        } finally {
            image.close()
        }
    }

    @JvmStatic
    fun stop(why: String) {
        val was = peer
        videoRequested = false
        AirLinkService.appContext?.let { PlayerBluetoothAudio.setScreenPlaybackWatch(it, false) }
        stopAudio()
        peer = null
        pendingPeer = null
        display?.release()
        reader?.close()
        try { encoder?.close() } catch (_: Exception) { }
        encoder = null
        projection?.stop()
        thread?.quitSafely()
        display = null
        reader = null
        projection = null
        thread = null
        AirLinkService.stopProjectionForeground()
        if (was != null) {
            nativeStopFrames(was)
            tell(was, "screen.stopped", why)
        }
    }

    @JvmStatic fun peer(): String? = peer

    @JvmStatic
    fun realBounds(context: Context): Rect {
        val manager = context.getSystemService(WindowManager::class.java) ?: return Rect()
        if (Build.VERSION.SDK_INT >= 30) return manager.maximumWindowMetrics.bounds
        val metrics = DisplayMetrics()
        @Suppress("DEPRECATION")
        manager.defaultDisplay.getRealMetrics(metrics)
        return Rect(0, 0, metrics.widthPixels, metrics.heightPixels)
    }

    private fun tell(peerId: String, kind: String, reason: String?) {
        val body = JSONObject()
        if (reason != null) body.put("reason", reason)
        if (kind == "screen.started") body.put("width", width).put("height", height)
        SmsBridge.send(peerId, kind, body)
    }

    @JvmStatic private external fun nativeSendFrame(peerId: String, jpeg: ByteArray)
    @JvmStatic private external fun nativeStopFrames(peerId: String)
    @JvmStatic private external fun nativeSendAudio(peerId: String, pcm: ByteArray)
    @JvmStatic private external fun nativeStopAudio(peerId: String)
    @JvmStatic private external fun nativeScreenAudioFailed()
}
