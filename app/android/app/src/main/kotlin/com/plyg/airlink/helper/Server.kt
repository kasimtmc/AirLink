package com.plyg.airlink.helper

import android.os.SystemClock
import android.view.KeyEvent
import android.view.MotionEvent
import org.json.JSONObject
import java.io.BufferedReader
import java.io.DataOutputStream
import java.io.IOException
import java.io.InputStreamReader
import java.net.InetAddress
import java.net.ServerSocket
import java.nio.charset.StandardCharsets
import java.util.concurrent.ConcurrentHashMap

/** Loopback protocol for the app's privileged shell helper. */
object Server {
    private const val EVENT = 1
    private const val FRAME = 2
    private val sessions = ConcurrentHashMap<Int, AppDisplay>()
    private val watches = ConcurrentHashMap<Int, SessionWatch>()
    private val sendLock = Any()
    private lateinit var output: DataOutputStream

    private class SessionWatch {
        val openedAt = SystemClock.elapsedRealtime()
        var seen = false
        var missed = 0
    }

    @JvmStatic
    fun run(port: Int, token: String) {
        ServerSocket(port, 1, InetAddress.getLoopbackAddress()).use { server ->
            Log.i("serving on 127.0.0.1:$port")
            server.accept().use { socket ->
                socket.tcpNoDelay = true
                output = DataOutputStream(socket.getOutputStream())
                val input = BufferedReader(InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8))
                val first = input.readLine()
                if (first == null || JSONObject(first).optString("token") != token) {
                    Log.i("wrong token: closing")
                    return
                }
                send(0, EVENT, "{\"event\":\"ready\"}".toByteArray(StandardCharsets.UTF_8))
                val watcher = Thread(::watchSessions, "airlink-app-watch").apply { isDaemon = true; start() }
                try {
                    while (true) {
                        val line = input.readLine() ?: break
                        try {
                            if (!handle(JSONObject(line))) break
                        } catch (error: Exception) {
                            Log.e("command failed: $line", error)
                        }
                    }
                } finally {
                    watcher.interrupt()
                    watches.clear()
                    sessions.values.forEach { it.release() }
                    sessions.clear()
                    Log.i("client gone: exiting")
                }
            }
        }
    }

    @Synchronized
    private fun handle(command: JSONObject): Boolean {
        val session = command.optInt("session")
        val current = sessions[session]
        when (command.optString("cmd")) {
            "open" -> {
                current?.release()
                watches.remove(session)
                val width = command.optInt("width", 720)
                val height = command.optInt("height", 1560)
                val dpi = command.optInt("dpi", 320)
                val display = AppDisplay(session.toString(), width, height, dpi, command.optString("codec", "jpeg"))
                    { _, frame -> send(session, FRAME, frame) }
                display.setQuality(command.optInt("quality", 70), command.optInt("fps", 24))
                sessions[session] = display
                val started = Helper.launch(display.id(), command.getString("component"))
                if (started) watches[session] = SessionWatch()
                else {
                    sessions.remove(session)
                    display.release()
                }
                event(session, if (started) JSONObject().put("event", "opened").put("display", display.id())
                    .put("width", width).put("height", height)
                    else JSONObject().put("event", "error").put("message", "the app could not be started"))
            }
            "close" -> if (current != null) {
                sessions.remove(session)
                watches.remove(session)
                current.release()
                event(session, JSONObject().put("event", "closed"))
            }
            "touch" -> if (current != null) {
                val action = when (command.optString("action")) {
                    "down" -> MotionEvent.ACTION_DOWN
                    "up" -> MotionEvent.ACTION_UP
                    else -> MotionEvent.ACTION_MOVE
                }
                Input.touch(current.id(), action, (command.optDouble("x") * current.logicalWidth()).toFloat(),
                    (command.optDouble("y") * current.logicalHeight()).toFloat())
            }
            "scroll" -> if (current != null) Input.scroll(current.id(),
                (command.optDouble("x") * current.logicalWidth()).toFloat(),
                (command.optDouble("y") * current.logicalHeight()).toFloat(),
                command.optDouble("h").toFloat(), command.optDouble("v").toFloat(),
                current.logicalWidth(), current.logicalHeight())
            "key" -> if (current != null) Input.key(current.id(), command.optInt("code", KeyEvent.KEYCODE_BACK), command.optInt("meta"))
            "text" -> if (current != null) Input.text(current.id(), command.optString("text"))
            "clipboard" -> ClipboardWatch.start { event(0, it) }
            "quit" -> return false
        }
        return true
    }

    /** Ends an app session when its task leaves its virtual display. */
    private fun watchSessions() {
        var listingUnavailable = false
        while (!Thread.currentThread().isInterrupted) {
            try {
                Thread.sleep(1000)
                if (watches.isEmpty()) continue
                val occupied = occupiedDisplays()
                if (occupied == null) {
                    if (!listingUnavailable) Log.i("task listing unavailable")
                    listingUnavailable = true
                    continue
                }
                listingUnavailable = false
                synchronized(this) {
                    for ((session, watch) in watches) {
                        val display = sessions[session] ?: continue
                        if (display.id() in occupied) {
                            if (!watch.seen) Log.i("session $session appeared on display ${display.id()}")
                            watch.seen = true
                            watch.missed = 0
                        } else if (watch.seen && ++watch.missed >= 2) {
                            Log.i("session $session left display ${display.id()}")
                            watches.remove(session)
                            sessions.remove(session)
                            display.release()
                            event(session, JSONObject().put("event", "closed"))
                        } else if (!watch.seen && SystemClock.elapsedRealtime() - watch.openedAt > 15000) {
                            Log.i("session $session never appeared on display ${display.id()}")
                            watches.remove(session)
                            sessions.remove(session)
                            display.release()
                            event(session, JSONObject().put("event", "error").put("message", "Uygulama sanal ekranda başlamadı"))
                        }
                    }
                }
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
            } catch (error: Exception) {
                Log.e("watching app tasks failed", error)
            }
        }
    }

    /** Parses the display identifiers with active tasks from the shell's activity listing. */
    private fun occupiedDisplays(): Set<Int>? {
        val process = Runtime.getRuntime().exec(arrayOf("am", "stack", "list"))
        val listing = process.inputStream.readBytes().toString(StandardCharsets.UTF_8)
        if (process.waitFor() != 0) return null
        val occupied = hashSetOf<Int>()
        var display = -1
        for (line in listing.lineSequence()) {
            if (line.startsWith("RootTask id=")) {
                display = line.substringAfter("displayId=", "").takeWhile { it.isDigit() }.toIntOrNull() ?: -1
            } else if (display >= 0 && line.trimStart().startsWith("taskId=")) {
                occupied.add(display)
            }
        }
        return occupied
    }

    private fun event(session: Int, body: JSONObject) = send(session, EVENT, body.toString().toByteArray(StandardCharsets.UTF_8))

    private fun send(session: Int, type: Int, payload: ByteArray) {
        synchronized(sendLock) {
            try {
                output.writeInt(5 + payload.size)
                output.writeByte(type)
                output.writeInt(session)
                output.write(payload)
                output.flush()
            } catch (error: IOException) {
                Log.e("send failed", error)
            }
        }
    }
}
