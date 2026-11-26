package com.plyg.airlink

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import java.util.concurrent.ArrayBlockingQueue

/** Plays short PCM packets from the desktop currently being viewed. */
object DesktopAudioPlayer {
    private var playback: Playback? = null

    @JvmStatic
    @Synchronized
    fun offer(pcm: ByteArray) {
        if (pcm.isEmpty() || pcm.size > 19200 || pcm.size % 2 != 0) return
        val current = playback ?: Playback().also { playback = it; it.start() }
        if (!current.packets.offer(pcm)) {
            current.packets.poll()
            current.packets.offer(pcm)
        }
    }

    @JvmStatic
    @Synchronized
    fun stop() {
        val current = playback ?: return
        playback = null
        current.interrupt()
    }

    private class Playback : Thread("airlink-desktop-audio") {
        val packets = ArrayBlockingQueue<ByteArray>(3)

        override fun run() {
            var track: AudioTrack? = null
            try {
                val attributes = AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                if (android.os.Build.VERSION.SDK_INT >= 29)
                    attributes.setAllowedCapturePolicy(AudioAttributes.ALLOW_CAPTURE_BY_NONE)
                val format = AudioFormat.Builder().setSampleRate(48000)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build()
                val size = AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT)
                val output = AudioTrack.Builder().setAudioAttributes(attributes.build())
                    .setAudioFormat(format).setBufferSizeInBytes(maxOf(size, 19200))
                    .setTransferMode(AudioTrack.MODE_STREAM).build()
                track = output
                output.play()
                while (!isInterrupted) {
                    val pcm = packets.take()
                    if (isInterrupted) break
                    if (output.write(pcm, 0, pcm.size, AudioTrack.WRITE_BLOCKING) < 0) break
                }
            } catch (_: InterruptedException) {
                // The desktop share ended while waiting for its next packet.
            } catch (error: Exception) {
                android.util.Log.w("AirLink", "desktop audio playback failed", error)
            } finally {
                try { track?.pause(); track?.flush() } catch (_: IllegalStateException) { }
                track?.release()
                synchronized(DesktopAudioPlayer) {
                    if (playback === this) playback = null
                }
            }
        }
    }
}
