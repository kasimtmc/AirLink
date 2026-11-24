package com.plyg.airlink.helper

import android.annotation.SuppressLint
import android.os.Build
import android.os.SystemClock
import android.view.InputDevice
import android.view.InputEvent
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import java.lang.reflect.Method
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

/** Injects touchscreen and keyboard events into a shell-owned virtual display. */
@SuppressLint("PrivateApi", "DiscouragedPrivateApi", "BlockedPrivateApi")
object Input {
    private var manager: Any? = null
    private lateinit var injectMethod: Method
    private lateinit var setDisplayId: Method
    private var downTime = 0L

    private fun init() {
        if (manager != null) return
        val klass = Class.forName(if (Build.VERSION.SDK_INT >= 34)
            "android.hardware.input.InputManagerGlobal" else "android.hardware.input.InputManager")
        manager = klass.getDeclaredMethod("getInstance").invoke(null)
        injectMethod = klass.getMethod("injectInputEvent", InputEvent::class.java, Int::class.javaPrimitiveType)
        setDisplayId = InputEvent::class.java.getMethod("setDisplayId", Int::class.javaPrimitiveType)
    }

    @JvmStatic
    fun inject(event: InputEvent, displayId: Int): Boolean = try {
        init()
        setDisplayId.invoke(event, displayId)
        injectMethod.invoke(manager, event, 0) as Boolean
    } catch (error: Exception) {
        Log.e("inject failed", error)
        false
    }

    @JvmStatic
    fun touch(displayId: Int, action: Int, x: Float, y: Float): Boolean {
        if (action == MotionEvent.ACTION_DOWN) downTime = SystemClock.uptimeMillis()
        return touch(displayId, action, x, y, downTime)
    }

    private fun touch(displayId: Int, action: Int, x: Float, y: Float, start: Long): Boolean {
        val properties = arrayOf(MotionEvent.PointerProperties().apply {
            id = 0
            toolType = MotionEvent.TOOL_TYPE_FINGER
        })
        val coords = arrayOf(MotionEvent.PointerCoords().apply {
            this.x = x
            this.y = y
            pressure = if (action == MotionEvent.ACTION_UP) 0f else 1f
            size = 1f
        })
        val event = MotionEvent.obtain(start, SystemClock.uptimeMillis(), action, 1, properties, coords,
            0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
        return try { inject(event, displayId) } finally { event.recycle() }
    }

    private class Pending {
        var h = 0f
        var v = 0f
        var x = 0f
        var y = 0f
        var width = 0
        var height = 0
        var queued = false
    }

    private class Finger {
        var x = 0f
        var y = 0f
        var downTime = 0L
        var lift: ScheduledFuture<*>? = null
    }

    private val pending = HashMap<Int, Pending>()
    private val fingers = HashMap<Int, Finger>() // only the wheel executor reads this
    private val wheel = Executors.newSingleThreadScheduledExecutor { task -> Thread(task, "airlink-wheel") }

    /** Coalesces wheel turns into a single live finger stroke, lifted without flinging. */
    @JvmStatic
    fun scroll(displayId: Int, x: Float, y: Float, h: Float, v: Float, width: Int, height: Int) {
        synchronized(pending) {
            val item = pending.getOrPut(displayId) { Pending() }
            item.h += h
            item.v += v
            item.x = x
            item.y = y
            item.width = width
            item.height = height
            if (item.queued) return
            item.queued = true
        }
        wheel.execute { play(displayId) }
    }

    private fun play(displayId: Int) {
        val h: Float
        val v: Float
        val x: Float
        val y: Float
        val width: Int
        val height: Int
        synchronized(pending) {
            val item = pending[displayId] ?: return
            h = item.h
            v = item.v
            x = item.x
            y = item.y
            width = item.width
            height = item.height
            item.h = 0f
            item.v = 0f
            item.queued = false
        }
        val dx = h * width * 0.10f
        val dy = v * height * 0.07f
        var finger = fingers[displayId]
        finger?.lift?.cancel(false)
        if (finger != null) {
            val tx = finger.x + dx
            val ty = finger.y + dy
            if (tx < width * 0.05f || tx > width * 0.95f || ty < height * 0.05f || ty > height * 0.95f) {
                lift(displayId, finger)
                fingers.remove(displayId)
                finger = null
            }
        }
        if (finger == null) {
            finger = Finger().apply {
                this.x = if (h == 0f) x else if (dx < 0) width * 0.85f else width * 0.15f
                this.y = if (v == 0f) y else if (dy < 0) height * 0.85f else height * 0.15f
                downTime = SystemClock.uptimeMillis()
            }
            touch(displayId, MotionEvent.ACTION_DOWN, finger.x, finger.y, finger.downTime)
            fingers[displayId] = finger
        }
        finger.x = clamp(finger.x + dx, width)
        finger.y = clamp(finger.y + dy, height)
        touch(displayId, MotionEvent.ACTION_MOVE, finger.x, finger.y, finger.downTime)
        val current = finger
        finger.lift = wheel.schedule({
            if (fingers[displayId] === current) {
                fingers.remove(displayId)
                lift(displayId, current)
            }
        }, 120, TimeUnit.MILLISECONDS)
    }

    private fun lift(displayId: Int, finger: Finger) {
        touch(displayId, MotionEvent.ACTION_MOVE, finger.x, finger.y, finger.downTime)
        try { Thread.sleep(30) } catch (_: InterruptedException) { }
        touch(displayId, MotionEvent.ACTION_MOVE, finger.x, finger.y, finger.downTime)
        touch(displayId, MotionEvent.ACTION_UP, finger.x, finger.y, finger.downTime)
    }

    private fun clamp(value: Float, size: Int): Float = value.coerceIn(1f, (size - 2).toFloat())

    @JvmStatic
    fun key(displayId: Int, keyCode: Int, meta: Int): Boolean {
        val now = SystemClock.uptimeMillis()
        val down = KeyEvent(now, now, KeyEvent.ACTION_DOWN, keyCode, 0, meta,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD)
        val up = KeyEvent(now, now, KeyEvent.ACTION_UP, keyCode, 0, meta,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD)
        return inject(down, displayId) && inject(up, displayId)
    }

    @JvmStatic
    fun text(displayId: Int, text: String): Boolean {
        val events = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD).getEvents(text.toCharArray()) ?: return false
        events.forEach { inject(it, displayId) }
        return true
    }
}
