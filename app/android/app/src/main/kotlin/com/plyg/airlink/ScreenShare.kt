package com.plyg.airlink

import android.app.Activity
import android.app.Notification
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.graphics.Rect
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
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

/** Shares the phone's display after the system MediaProjection consent prompt. */
object ScreenShare {
    const val REQUEST = 7101
    private const val NOTIFICATION = 1500
    private const val MAX_FPS = 15

    private var pendingPeer: String? = null
    @Volatile private var peer: String? = null
    private var projection: MediaProjection? = null
    private var display: VirtualDisplay? = null
    private var reader: ImageReader? = null
    private var thread: HandlerThread? = null
    private var lastFrame = 0L
    private var width = 0
    private var height = 0
    private var quality = 60
    private var codec = "jpeg"
    private var encoder: H264Encoder? = null
    private var codecFailed = false

    @JvmStatic fun active(): Boolean = projection != null

    @JvmStatic
    fun requested(context: Context, peerId: String, request: JSONObject) {
        if (projection != null) stop("restarted")
        pendingPeer = peerId
        quality = request.optInt("quality", 60).coerceIn(30, 90)
        codec = request.optString("codec", "jpeg")
        codecFailed = false
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
    fun askConsent(activity: Activity) {
        if (pendingPeer == null) return
        activity.getSystemService(NotificationManager::class.java)?.cancel(NOTIFICATION)
        val manager = activity.getSystemService(MediaProjectionManager::class.java) ?: return
        activity.startActivityForResult(manager.createScreenCaptureIntent(), REQUEST)
    }

    @JvmStatic
    fun consentResult(activity: Activity, resultCode: Int, data: Intent?) {
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
            return
        }
        peer = asked
        start(activity.applicationContext)
    }

    private fun start(context: Context) {
        val thread = HandlerThread("airlink-screen").also { it.start() }
        this.thread = thread
        val handler = Handler(thread.looper)
        val projection = projection ?: return
        projection.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                Handler(Looper.getMainLooper()).post { stop("user") }
            }
        }, handler)
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

    private fun frame(reader: ImageReader) {
        val image = reader.acquireLatestImage() ?: return
        try {
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
            if (output != null && MainActivity.nativeReady) nativeSendFrame(target, output)
        } finally {
            image.close()
        }
    }

    @JvmStatic
    fun stop(why: String) {
        val was = peer
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
}
