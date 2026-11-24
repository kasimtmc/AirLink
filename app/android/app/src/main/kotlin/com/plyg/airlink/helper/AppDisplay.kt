package com.plyg.airlink.helper

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Matrix
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import com.plyg.airlink.H264Encoder
import java.io.ByteArrayOutputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Trusted, shell-owned virtual display for one phone app. */
@SuppressLint("WrongConstant")
class AppDisplay(
    name: String,
    val width: Int,
    val height: Int,
    val dpi: Int,
    private val codec: String = "jpeg",
    private val sink: FrameSink,
) {
    fun interface FrameSink { fun frame(display: AppDisplay, jpeg: ByteArray) }

    @Volatile private var rotation = 0
    @Volatile private var quality = 70
    @Volatile private var maxFps = 30
    private var lastFrame = 0L
    private val thread = HandlerThread("airlink-display-$name").also { it.start() }
    private val handler = Handler(thread.looper)
    private val reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2)
    private val virtualDisplay: VirtualDisplay
    private var encoder: H264Encoder? = null
    @Volatile private var released = false

    init {
        reader.setOnImageAvailableListener({ onImage() }, handler)
        var flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC or
            DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY or
            SUPPORTS_TOUCH or ROTATES_WITH_CONTENT or DESTROY_CONTENT_ON_REMOVAL
        if (Build.VERSION.SDK_INT >= 33)
            flags = flags or TRUSTED or OWN_DISPLAY_GROUP or ALWAYS_UNLOCKED or TOUCH_FEEDBACK_DISABLED
        val manager = DisplayManager::class.java.getDeclaredConstructor(Context::class.java)
            .newInstance(Workarounds.context())
        virtualDisplay = manager.createVirtualDisplay("airlink-$name", width, height, dpi, reader.surface, flags)
            ?: error("createVirtualDisplay returned null")
    }

    fun logicalWidth(): Int = if (rotation % 2 == 0) width else height
    fun logicalHeight(): Int = if (rotation % 2 == 0) height else width
    fun id(): Int = virtualDisplay.display.displayId

    fun setQuality(q: Int, fps: Int) {
        quality = q.coerceIn(30, 95)
        maxFps = fps.coerceIn(1, 60)
    }

    private fun onImage() {
        if (released) return
        val image = reader.acquireLatestImage() ?: return
        try {
            val now = System.currentTimeMillis()
            if (now - lastFrame < 1000 / maxFps) return
            lastFrame = now
            val plane = image.planes[0]
            val padded = plane.rowStride / plane.pixelStride
            val bitmap = Bitmap.createBitmap(padded, height, Bitmap.Config.ARGB_8888)
            bitmap.copyPixelsFromBuffer(plane.buffer)
            rotation = virtualDisplay.display.rotation
            val cropped = if (rotation == 0) {
                if (padded == width) bitmap else Bitmap.createBitmap(bitmap, 0, 0, width, height)
            } else {
                val matrix = Matrix().apply { postRotate(-90f * rotation) }
                Bitmap.createBitmap(bitmap, 0, 0, width, height, matrix, false)
            }
            val packet = if (codec == "h264" && !codecFailed) {
                try {
                    if (encoder == null || encodedWidth != cropped.width || encodedHeight != cropped.height) {
                        encoder?.close()
                        encoder = H264Encoder(cropped.width, cropped.height, maxFps, quality)
                        encodedWidth = cropped.width
                        encodedHeight = cropped.height
                    }
                    encoder?.encode(cropped)
                } catch (error: Exception) {
                    Log.e("H.264 encoder failed; falling back to JPEG", error)
                    try { encoder?.close() } catch (_: Exception) { }
                    encoder = null
                    codecFailed = true
                    null
                }
            } else null
            val output = if (packet == null && (codec != "h264" || codecFailed)) {
                ByteArrayOutputStream(256 * 1024).also { cropped.compress(Bitmap.CompressFormat.JPEG, quality, it) }.toByteArray()
            } else packet
            if (cropped !== bitmap) cropped.recycle()
            bitmap.recycle()
            if (output != null) sink.frame(this, output)
        } finally {
            image.close()
        }
    }

    @Synchronized
    fun release() {
        if (released) return
        released = true
        virtualDisplay.release()
        reader.setOnImageAvailableListener(null, null)
        val done = CountDownLatch(1)
        handler.post {
            try {
                reader.close()
                try { encoder?.close() } catch (_: Exception) { }
                encoder = null
            } finally {
                done.countDown()
                thread.quitSafely()
            }
        }
        done.await(2, TimeUnit.SECONDS)
    }

    private var encodedWidth = 0
    private var encodedHeight = 0
    private var codecFailed = false

    private companion object {
        const val SUPPORTS_TOUCH = 1 shl 6
        const val ROTATES_WITH_CONTENT = 1 shl 7
        const val DESTROY_CONTENT_ON_REMOVAL = 1 shl 8
        const val TRUSTED = 1 shl 10
        const val OWN_DISPLAY_GROUP = 1 shl 11
        const val ALWAYS_UNLOCKED = 1 shl 12
        const val TOUCH_FEEDBACK_DISABLED = 1 shl 13
    }
}
