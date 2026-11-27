package com.plyg.airlink

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.drawable.Drawable
import android.os.Handler
import android.os.Looper
import android.util.Base64
import android.view.KeyCharacterMap
import android.view.KeyEvent
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.Socket
import java.nio.charset.StandardCharsets
import java.security.SecureRandom
import java.util.ArrayDeque
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** Relays phone app listings, separate display frames and input to a paired computer. */
object AppStreams {
    private val worker = Executors.newSingleThreadExecutor { task -> Thread(task, "airlink-apps") }
    private val sessionPeer = ConcurrentHashMap<Int, String>()
    private val waiting = ArrayDeque<String>()
    private var helper: Socket? = null
    private var helperOut: OutputStream? = null
    private var starting = false
    private var port = 0
    private var token = ""

    @JvmStatic
    fun handle(context: Context, peerId: String, kind: String, request: JSONObject) {
        worker.execute {
            try {
                when (kind) {
                    "apps.list" -> sendList(peerId, list(context))
                    "apps.open" -> {
                        sessionPeer[request.optInt("session")] = peerId
                        toHelper(request.put("cmd", "open"))
                    }
                    "apps.close" -> toHelper(request.put("cmd", "close"))
                    "apps.input" -> {
                        val input = if (request.optString("cmd") == "text" && !typeable(request.optString("text"))) {
                            paste(context, request.optString("text"))
                            JSONObject().put("cmd", "key").put("session", request.optInt("session"))
                                .put("code", KeyEvent.KEYCODE_PASTE)
                        } else request
                        toHelper(input)
                    }
                }
            } catch (error: Exception) {
                android.util.Log.w("AirLink", "apps request $kind failed", error)
            }
        }
    }

    private fun typeable(text: String): Boolean = text.isEmpty() ||
        KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD).getEvents(text.toCharArray()) != null

    private fun paste(context: Context, text: String) {
        val done = CountDownLatch(1)
        Handler(Looper.getMainLooper()).post {
            try { context.getSystemService(ClipboardManager::class.java)?.setPrimaryClip(ClipData.newPlainText("AirLink", text)) }
            finally { done.countDown() }
        }
        done.await(2, TimeUnit.SECONDS)
    }

    private fun list(context: Context): JSONArray {
        val packages = context.packageManager
        val launcher = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        val found = packages.queryIntentActivities(launcher, 0)
            .sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.loadLabel(packages).toString() })
        val apps = JSONArray()
        for (entry in found) {
            if (entry.activityInfo.packageName == context.packageName) continue
            apps.put(JSONObject().put("label", entry.loadLabel(packages).toString())
                .put("component", "${entry.activityInfo.packageName}/${entry.activityInfo.name}")
                .put("icon", icon(entry.loadIcon(packages))))
        }
        return apps
    }

    private fun sendList(peerId: String, apps: JSONArray) {
        val parts = mutableListOf<JSONArray>()
        var part = JSONArray()
        var size = 0
        for (index in 0 until apps.length()) {
            val app = apps.getJSONObject(index)
            val length = app.toString().length
            if (part.length() > 0 && size + length > 400 * 1024) {
                parts.add(part)
                part = JSONArray()
                size = 0
            }
            part.put(app)
            size += length
        }
        parts.add(part)
        val listing = java.lang.Long.toHexString(System.nanoTime())
        for (index in parts.indices) {
            SmsBridge.send(peerId, "apps.list_result", JSONObject().put("list", listing).put("part", index)
                .put("parts", parts.size).put("apps", parts[index]))
        }
    }

    private fun icon(drawable: Drawable): String {
        val size = 72
        val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        drawable.setBounds(0, 0, size, size)
        drawable.draw(Canvas(bitmap))
        val output = ByteArrayOutputStream()
        bitmap.compress(Bitmap.CompressFormat.PNG, 100, output)
        bitmap.recycle()
        return Base64.encodeToString(output.toByteArray(), Base64.NO_WRAP)
    }

    private fun toHelper(command: JSONObject) {
        val line = command.toString() + "\n"
        helperOut?.let { output ->
            try {
                output.write(line.toByteArray(StandardCharsets.UTF_8))
                output.flush()
                return
            } catch (_: Exception) { closeHelper() }
        }
        synchronized(waiting) { waiting.add(line) }
        if (starting) return
        starting = true
        val random = SecureRandom()
        port = 20000 + random.nextInt(40000)
        token = ByteArray(16).also(random::nextBytes).joinToString("") { "%02x".format(it.toInt() and 0xff) }
        val commandLine = "CLASSPATH=\$(pm path com.plyg.airlink | head -1 | cut -d: -f2) " +
            "setsid app_process / com.plyg.airlink.helper.Helper serve $port $token " +
            "</dev/null >/data/local/tmp/airlink-helper.log 2>&1 &"
        if (MainActivity.nativeReady) MainActivity.startHelper(commandLine)
    }

    @JvmStatic
    fun helperStarted(ok: Boolean, message: String) {
        if (!ok) android.util.Log.w("AirLink", "helper not started: $message")
        worker.execute {
            if (!ok) {
                starting = false
                failAll(Texts.trf("Gelişmiş moda bağlanılamadı: {}", message))
                return@execute
            }
            for (attempt in 0 until 40) {
                if (helper != null) break
                try {
                    val socket = Socket(InetAddress.getLoopbackAddress(), port)
                    socket.tcpNoDelay = true
                    val output = socket.getOutputStream()
                    output.write("{\"token\":\"$token\"}\n".toByteArray(StandardCharsets.UTF_8))
                    output.flush()
                    helper = socket
                    helperOut = output
                    Thread({ read(socket) }, "airlink-helper-in").start()
                } catch (_: Exception) {
                    try { Thread.sleep(150) } catch (_: InterruptedException) { }
                }
            }
            starting = false
            val output = helperOut
            if (helper == null || output == null) {
                failAll(Texts.tr("Yardımcı başlamadı (/data/local/tmp/airlink-helper.log)"))
                return@execute
            }
            synchronized(waiting) {
                try {
                    while (waiting.isNotEmpty()) output.write(waiting.removeFirst().toByteArray(StandardCharsets.UTF_8))
                    output.flush()
                } catch (_: Exception) { closeHelper() }
            }
        }
    }

    private fun read(socket: Socket) {
        try {
            DataInputStream(socket.getInputStream()).use { input ->
                while (true) {
                    val length = input.readInt()
                    if (length < 5 || length > 16 * 1024 * 1024) throw IllegalStateException("bad helper frame length")
                    val type = input.readUnsignedByte()
                    val session = input.readInt()
                    val payload = ByteArray(length - 5)
                    input.readFully(payload)
                    val peer = sessionPeer[session]
                    if (type == 2 && peer != null) {
                        if (MainActivity.nativeReady) nativeSendFrame(peer, "app:$session", payload)
                    } else if (type == 1 && peer != null) {
                        val event = JSONObject(payload.toString(StandardCharsets.UTF_8)).put("session", session)
                        SmsBridge.send(peer, "apps.event", event)
                        if (event.optString("event") == "closed") {
                            sessionPeer.remove(session)
                            nativeStopFrames(peer, "app:$session")
                        }
                    }
                }
            }
        } catch (error: Exception) {
            android.util.Log.i("AirLink", "helper connection ended", error)
        } finally {
            worker.execute(::closeHelper)
        }
    }

    private fun closeHelper() {
        try { helper?.close() } catch (_: Exception) { }
        helper = null
        helperOut = null
        for ((session, peer) in sessionPeer) {
            SmsBridge.send(peer, "apps.event", JSONObject().put("session", session).put("event", "closed"))
            if (MainActivity.nativeReady) nativeStopFrames(peer, "app:$session")
        }
        sessionPeer.clear()
    }

    private fun failAll(message: String) {
        synchronized(waiting) { waiting.clear() }
        for ((session, peer) in sessionPeer) {
            SmsBridge.send(peer, "apps.event", JSONObject().put("session", session)
                .put("event", "error").put("message", message))
        }
        sessionPeer.clear()
    }

    @JvmStatic private external fun nativeSendFrame(peerId: String, name: String, jpeg: ByteArray)
    @JvmStatic private external fun nativeStopFrames(peerId: String, name: String)
}
