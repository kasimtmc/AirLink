package com.plyg.airlink

import android.graphics.Bitmap
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Bundle
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer

/** Encodes independent AVC pictures so AirLink's latest-frame media queue can drop old frames. */
class H264Encoder(private val width: Int, private val height: Int, private val fps: Int, quality: Int) {
    private val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
    private val info = MediaCodec.BufferInfo()
    private var config = ByteArray(0)
    private var frameNumber = 0L
    private var loggedPacket = false

    init {
        require(width > 0 && height > 0 && width % 2 == 0 && height % 2 == 0)
        try {
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height)
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
            format.setInteger(MediaFormat.KEY_BIT_RATE, (width.toLong() * height * fps * quality / 350).coerceIn(500_000, 8_000_000).toInt())
            format.setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 0)
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            codec.start()
            android.util.Log.i("AirLink", "H.264 encoder ${codec.name} ${width}x$height at $fps fps")
        } catch (error: Exception) {
            codec.release()
            throw error
        }
    }

    /** Returns the newest self-contained access unit, if the encoder produced one. */
    fun encode(bitmap: Bitmap): ByteArray? {
        val pending = drain()
        val index = codec.dequeueInputBuffer(0)
        if (index < 0) return pending
        // getInputImage invalidates the ByteBuffer view on some hardware codecs. Read its
        // capacity first; asking for getInputBuffer again after closing the Image can fail.
        val inputSize = codec.getInputBuffer(index)?.capacity() ?: width * height * 3 / 2
        val image = codec.getInputImage(index) ?: error("AVC encoder has no flexible YUV input")
        try {
            val pixels = IntArray(width * height)
            bitmap.getPixels(pixels, 0, width, 0, 0, width, height)
            val planes = image.planes
            for (y in 0 until height step 2) for (x in 0 until width step 2) {
                var u = 0
                var v = 0
                for (dy in 0..1) for (dx in 0..1) {
                    val color = pixels[(y + dy) * width + x + dx]
                    val r = color shr 16 and 255
                    val g = color shr 8 and 255
                    val b = color and 255
                    val yy = ((66 * r + 129 * g + 25 * b + 128) shr 8) + 16
                    put(planes[0].buffer, planes[0].rowStride, planes[0].pixelStride, x + dx, y + dy, yy)
                    u += ((-38 * r - 74 * g + 112 * b + 128) shr 8) + 128
                    v += ((112 * r - 94 * g - 18 * b + 128) shr 8) + 128
                }
                put(planes[1].buffer, planes[1].rowStride, planes[1].pixelStride, x / 2, y / 2, (u / 4).coerceIn(0, 255))
                put(planes[2].buffer, planes[2].rowStride, planes[2].pixelStride, x / 2, y / 2, (v / 4).coerceIn(0, 255))
            }
        } finally {
            image.close()
        }
        codec.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        codec.queueInputBuffer(index, 0, inputSize,
            frameNumber++ * 1_000_000L / fps, 0)
        return drain() ?: pending
    }

    private fun drain(): ByteArray? {
        var latest: ByteArray? = null
        while (true) {
            val index = codec.dequeueOutputBuffer(info, 0)
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                val format = codec.outputFormat
                val output = ByteArrayOutputStream()
                for (name in listOf("csd-0", "csd-1")) format.getByteBuffer(name)?.let { output.write(copy(it)) }
                config = output.toByteArray()
                continue
            }
            if (index < 0) break
            val buffer = codec.getOutputBuffer(index)
            if (buffer != null && info.size > 0) {
                buffer.position(info.offset)
                buffer.limit(info.offset + info.size)
                val bytes = copy(buffer)
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) config = bytes
                else if (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0 && config.isNotEmpty()) {
                    latest = ByteArray(4 + config.size + bytes.size).also {
                        "ALH1".toByteArray(Charsets.US_ASCII).copyInto(it)
                        config.copyInto(it, 4)
                        bytes.copyInto(it, 4 + config.size)
                    }
                    if (!loggedPacket) {
                        android.util.Log.i("AirLink", "first H.264 access unit: ${latest.size} bytes")
                        loggedPacket = true
                    }
                }
            }
            codec.releaseOutputBuffer(index, false)
        }
        return latest
    }

    fun close() {
        codec.stop()
        codec.release()
    }

    private fun put(buffer: ByteBuffer, stride: Int, pixelStride: Int, x: Int, y: Int, value: Int) {
        buffer.put(y * stride + x * pixelStride, value.coerceIn(0, 255).toByte())
    }

    private fun copy(buffer: ByteBuffer): ByteArray = ByteArray(buffer.remaining()).also { buffer.get(it) }
}
