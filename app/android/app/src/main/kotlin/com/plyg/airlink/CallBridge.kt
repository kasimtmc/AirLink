package com.plyg.airlink

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.media.AudioManager
import android.media.AudioDeviceInfo
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.CallLog
import android.telecom.TelecomManager
import android.telephony.TelephonyManager
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.Executors

/** Call state, log and controls for paired computers. */
object CallBridge {
    private val worker = Executors.newSingleThreadExecutor { task -> Thread(task, "airlink-calls") }
    private val main = Handler(Looper.getMainLooper())
    private var receiver: BroadcastReceiver? = null
    private var lastState = TelephonyManager.EXTRA_STATE_IDLE
    private var ringingTold = false
    private var lastNumber = ""

    @JvmStatic
    fun granted(context: Context): Boolean = has(context, Manifest.permission.READ_PHONE_STATE) &&
        has(context, Manifest.permission.READ_CALL_LOG)

    private fun has(context: Context, permission: String): Boolean =
        context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    @JvmStatic
    fun permissions(): Array<String> = if (Build.VERSION.SDK_INT >= 26) {
        arrayOf(Manifest.permission.READ_PHONE_STATE, Manifest.permission.READ_CALL_LOG,
            Manifest.permission.ANSWER_PHONE_CALLS, Manifest.permission.CALL_PHONE)
    } else {
        arrayOf(Manifest.permission.READ_PHONE_STATE, Manifest.permission.READ_CALL_LOG, Manifest.permission.CALL_PHONE)
    }

    @JvmStatic
    fun start(context: Context) {
        if (receiver != null) return
        val listener = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                stateChanged(context.applicationContext, intent.getStringExtra(TelephonyManager.EXTRA_STATE),
                    intent.getStringExtra(TelephonyManager.EXTRA_INCOMING_NUMBER))
            }
        }
        val filter = IntentFilter(TelephonyManager.ACTION_PHONE_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(listener, filter, Context.RECEIVER_EXPORTED)
        else context.registerReceiver(listener, filter)
        receiver = listener
    }

    @JvmStatic
    fun stop(context: Context) {
        val listener = receiver ?: return
        context.unregisterReceiver(listener)
        receiver = null
    }

    private fun stateChanged(context: Context, state: String?, number: String?) {
        if (state == null) return
        val previous = lastState
        val wasRinging = lastState == TelephonyManager.EXTRA_STATE_RINGING
        lastState = state
        if (state == TelephonyManager.EXTRA_STATE_RINGING) {
            if (!wasRinging) ringingTold = false
            val numberKnown = !number.isNullOrEmpty()
            if (ringingTold || (!numberKnown && has(context, Manifest.permission.READ_CALL_LOG))) return
            ringingTold = true
            val caller = number.orEmpty()
            lastNumber = caller
            worker.execute {
                try {
                    SmsBridge.broadcastAll("call.ringing", JSONObject().put("number", caller)
                        .put("name", if (caller.isEmpty()) "" else SmsBridge.contactName(context, caller)).toString())
                } catch (_: Exception) { }
            }
            statusChanged()
        } else {
            // The phone-state broadcast has no number for outgoing calls. Never reuse a prior
            // caller's identity for a new call (or while the phone is idle).
            if (state == TelephonyManager.EXTRA_STATE_IDLE ||
                (state == TelephonyManager.EXTRA_STATE_OFFHOOK && previous == TelephonyManager.EXTRA_STATE_IDLE)) {
                lastNumber = ""
            }
            statusChanged()
            val label = if (state == TelephonyManager.EXTRA_STATE_OFFHOOK) "offhook" else "idle"
            worker.execute {
                try { SmsBridge.broadcastAll("call.state", JSONObject().put("state", label).toString()) }
                catch (_: Exception) { }
            }
        }
    }

    private val sendStatus = Runnable {
        val context = AirLinkService.appContext ?: return@Runnable
        try { SmsBridge.broadcastAll("call.status", status(context).toString()) }
        catch (_: Exception) { }
    }

    @JvmStatic
    fun statusChanged() {
        main.removeCallbacks(sendStatus)
        main.postDelayed(sendStatus, 150)
    }

    private fun status(context: Context): JSONObject {
        val out = JSONObject()
        val service = AirLinkInCallService.instance
        val audio = context.getSystemService(AudioManager::class.java) ?: return out
        if (service != null && service.hasCall()) {
            service.describe(out)
        } else {
            val state = when (lastState) {
                TelephonyManager.EXTRA_STATE_OFFHOOK -> "active"
                TelephonyManager.EXTRA_STATE_RINGING -> "ringing"
                else -> "ended"
            }
            out.put("full_control", false).put("state", state).put("held", false).put("can_hold", false)
                .put("muted", audio.isMicrophoneMute)
                .put("route", currentRoute(audio))
                .put("routes", availableRoutes(audio))
        }
        if (!out.has("number") && lastNumber.isNotEmpty()) out.put("number", lastNumber)
        if (out.has("number")) out.put("name", SmsBridge.contactName(context, out.getString("number")))
        out.put("volume", audio.getStreamVolume(AudioManager.STREAM_VOICE_CALL))
            .put("volume_max", audio.getStreamMaxVolume(AudioManager.STREAM_VOICE_CALL))
        return out
    }

    @Suppress("DEPRECATION")
    private fun control(context: Context, kind: String, request: JSONObject) {
        val service = AirLinkInCallService.instance
        val full = service?.hasCall() == true
        val audio = context.getSystemService(AudioManager::class.java) ?: return
        val telecom = context.getSystemService(TelecomManager::class.java)
        when (kind) {
            "call.hangup" -> if (full) service?.hangUp()
                else if (Build.VERSION.SDK_INT >= 28 && has(context, Manifest.permission.ANSWER_PHONE_CALLS)) telecom?.endCall()
            "call.hold" -> if (full) service?.hold(request.optBoolean("on"))
            "call.mute" -> if (full) service?.mute(request.optBoolean("on")) else audio.isMicrophoneMute = request.optBoolean("on")
            "call.route" -> if (full) service?.route(request.optString("route"))
                else setRoute(audio, request.optString("route"))
            "call.volume" -> audio.adjustStreamVolume(AudioManager.STREAM_VOICE_CALL,
                if (request.optInt("delta") > 0) AudioManager.ADJUST_RAISE else AudioManager.ADJUST_LOWER, 0)
            else -> return
        }
        statusChanged()
    }

    private fun currentRoute(audio: AudioManager): String {
        if (Build.VERSION.SDK_INT >= 31) {
            audio.communicationDevice?.let { routeName(it.type)?.let { route -> return route } }
        }
        return if (audio.isSpeakerphoneOn) "speaker" else "earpiece"
    }

    private fun availableRoutes(audio: AudioManager): JSONArray {
        if (Build.VERSION.SDK_INT < 31) return JSONArray().put("earpiece").put("speaker")
        val routes = JSONArray()
        audio.availableCommunicationDevices.mapNotNull { routeName(it.type) }.distinct().forEach { routes.put(it) }
        return routes
    }

    private fun routeName(type: Int): String? = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "earpiece"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "speaker"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO, AudioDeviceInfo.TYPE_BLE_HEADSET -> "bluetooth"
        AudioDeviceInfo.TYPE_WIRED_HEADSET, AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "wired"
        else -> null
    }

    private fun setRoute(audio: AudioManager, name: String) {
        if (Build.VERSION.SDK_INT >= 31) {
            val device = audio.availableCommunicationDevices.firstOrNull { routeName(it.type) == name }
            val applied = device != null && audio.setCommunicationDevice(device)
            android.util.Log.d("AirLink", "call route $name: ${if (applied) "requested" else "rejected"}")
            return
        }
        audio.isSpeakerphoneOn = name == "speaker"
    }

    @JvmStatic
    fun handle(context: Context, peerId: String, kind: String, request: JSONObject) {
        val telecom = context.getSystemService(TelecomManager::class.java)
        when (kind) {
            "call.hangup", "call.hold", "call.mute", "call.route", "call.volume" ->
                main.post { control(context, kind, request) }
            "call.status_request" -> statusChanged()
            "calls.log_request" -> {
                if (!granted(context)) {
                    SmsBridge.send(peerId, "calls.error", JSONObject().put("error", "no_permission"))
                } else {
                    SmsBridge.send(peerId, "calls.log", log(context, request.optInt("offset").coerceAtLeast(0)))
                }
            }
            "call.answer" -> main.post {
                val service = AirLinkInCallService.instance
                if (service?.hasCall() == true) service.answer()
                else if (Build.VERSION.SDK_INT >= 26 && has(context, Manifest.permission.ANSWER_PHONE_CALLS)) telecom?.acceptRingingCall()
            }
            "call.reject" -> main.post {
                val service = AirLinkInCallService.instance
                if (service?.hasCall() == true) service.hangUp()
                else if (Build.VERSION.SDK_INT >= 28 && has(context, Manifest.permission.ANSWER_PHONE_CALLS)) telecom?.endCall()
            }
            "call.dial" -> {
                val number = request.optString("number")
                val result = JSONObject().put("number", number)
                if (number.isEmpty() || !has(context, Manifest.permission.CALL_PHONE)) {
                    result.put("ok", false).put("error", if (number.isEmpty()) "no_number" else "no_permission")
                } else {
                    telecom?.placeCall(Uri.fromParts("tel", number, null), null)
                    result.put("ok", telecom != null)
                }
                SmsBridge.send(peerId, "call.dial_result", result)
            }
        }
    }

    private fun log(context: Context, offset: Int): JSONObject {
        val list = JSONArray()
        val names = mutableMapOf<String, String>()
        val limit = 80
        var nextOffset = offset
        var more = false
        val cols = arrayOf(CallLog.Calls.NUMBER, CallLog.Calls.CACHED_NAME, CallLog.Calls.TYPE,
            CallLog.Calls.DATE, CallLog.Calls.DURATION, CallLog.Calls._ID)
        context.contentResolver.query(CallLog.Calls.CONTENT_URI, cols, null, null,
            "${CallLog.Calls.DATE} DESC, ${CallLog.Calls._ID} DESC")?.use { cursor ->
            if (cursor.moveToPosition(offset)) do {
                if (list.length() == limit) { more = true; break }
                nextOffset++
                val number = cursor.getString(0).orEmpty()
                val type = when (cursor.getInt(2)) {
                    CallLog.Calls.INCOMING_TYPE -> "incoming"
                    CallLog.Calls.OUTGOING_TYPE -> "outgoing"
                    CallLog.Calls.MISSED_TYPE -> "missed"
                    CallLog.Calls.REJECTED_TYPE -> "rejected"
                    CallLog.Calls.BLOCKED_TYPE -> "blocked"
                    else -> "other"
                }
                // CACHED_NAME belongs to the call's past contact snapshot and can outlive
                // a deleted or changed contact. Resolve the number against the current book.
                list.put(JSONObject().put("number", number)
                    .put("name", names.getOrPut(number) { SmsBridge.contactName(context, number) }).put("type", type)
                    .put("date", cursor.getLong(3)).put("duration", cursor.getLong(4)))
            } while (cursor.moveToNext())
        }
        return JSONObject().put("offset", offset).put("next_offset", nextOffset)
            .put("has_more", more).put("calls", list)
    }
}
