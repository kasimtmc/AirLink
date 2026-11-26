package com.plyg.airlink

import android.os.Build
import android.os.OutcomeReceiver
import android.telecom.Call
import android.telecom.CallAudioState
import android.telecom.CallEndpoint
import android.telecom.CallEndpointException
import android.telecom.InCallService
import org.json.JSONArray
import org.json.JSONObject

/** Companion Telecom service for full call controls when Android binds it. */
class AirLinkInCallService : InCallService() {
    private var call: Call? = null
    private var endpoints: List<CallEndpoint>? = null
    private var endpoint: CallEndpoint? = null

    private val callback = object : Call.Callback() {
        override fun onStateChanged(call: Call, state: Int) = CallBridge.statusChanged()
        override fun onDetailsChanged(call: Call, details: Call.Details) = CallBridge.statusChanged()
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        android.util.Log.d("AirLink", "in-call service bound: full call controls available")
    }

    override fun onDestroy() {
        if (instance === this) instance = null
        super.onDestroy()
    }

    override fun onCallAdded(added: Call) {
        call?.unregisterCallback(callback)
        call = added
        added.registerCallback(callback)
        CallBridge.statusChanged()
    }

    override fun onCallRemoved(removed: Call) {
        removed.unregisterCallback(callback)
        if (call === removed) call = calls.firstOrNull()
        CallBridge.statusChanged()
    }

    override fun onCallAudioStateChanged(state: CallAudioState) = CallBridge.statusChanged()

    override fun onCallEndpointChanged(newEndpoint: CallEndpoint) {
        endpoint = newEndpoint
        CallBridge.statusChanged()
    }

    override fun onAvailableCallEndpointsChanged(available: List<CallEndpoint>) {
        endpoints = available
        CallBridge.statusChanged()
    }

    override fun onMuteStateChanged(muted: Boolean) = CallBridge.statusChanged()

    fun hasCall(): Boolean = call != null

    fun describe(out: JSONObject) {
        out.put("full_control", true)
        val current = call ?: return
        val details = current.details
        val state = if (Build.VERSION.SDK_INT >= 31) details.state else current.state
        out.put("state", stateName(state))
            .put("held", state == Call.STATE_HOLDING)
            .put("can_hold", details.can(Call.Details.CAPABILITY_HOLD) || details.can(Call.Details.CAPABILITY_SUPPORT_HOLD))
            .put("since", details.connectTimeMillis)
        details.handle?.let { out.put("number", it.schemeSpecificPart) }
        val audio = callAudioState
        if (audio != null) out.put("muted", audio.isMuted)
        val routes = JSONArray()
        var route = ""
        if (Build.VERSION.SDK_INT >= 34 && endpoints != null) {
            endpoints?.forEach { routes.put(routeName(it.endpointType)) }
            route = endpoint?.let { routeName(it.endpointType) }.orEmpty()
        } else if (audio != null) {
            val mask = audio.supportedRouteMask
            if (mask and CallAudioState.ROUTE_EARPIECE != 0) routes.put("earpiece")
            if (mask and CallAudioState.ROUTE_SPEAKER != 0) routes.put("speaker")
            if (mask and CallAudioState.ROUTE_BLUETOOTH != 0) routes.put("bluetooth")
            if (mask and CallAudioState.ROUTE_WIRED_HEADSET != 0) routes.put("wired")
            route = legacyRouteName(audio.route)
        }
        out.put("routes", routes).put("route", route)
    }

    fun hangUp() { call?.disconnect() }
    fun answer() { call?.answer(0) }
    fun hold(on: Boolean) { if (on) call?.hold() else call?.unhold() }
    fun mute(on: Boolean) { setMuted(on) }
    /** A keypad key during the call (menus, PINs): its tone for a moment. */
    fun dtmf(digit: Char) {
        val current = call ?: return
        current.playDtmfTone(digit)
        android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({ current.stopDtmfTone() }, 180)
    }

    fun route(name: String) {
        if (Build.VERSION.SDK_INT >= 34) {
            for (candidate in endpoints.orEmpty()) {
                if (routeName(candidate.endpointType) != name) continue
                requestCallEndpointChange(candidate, mainExecutor, object : OutcomeReceiver<Void, CallEndpointException> {
                    override fun onResult(result: Void?) = Unit
                    override fun onError(error: CallEndpointException) {
                        android.util.Log.w("AirLink", "audio route change failed: ${error.message}")
                    }
                })
                return
            }
        }
        val legacy = when (name) {
            "speaker" -> CallAudioState.ROUTE_SPEAKER
            "bluetooth" -> CallAudioState.ROUTE_BLUETOOTH
            "wired" -> CallAudioState.ROUTE_WIRED_HEADSET
            else -> CallAudioState.ROUTE_EARPIECE
        }
        setAudioRoute(legacy)
    }

    companion object {
        @JvmField @Volatile var instance: AirLinkInCallService? = null

        private fun stateName(state: Int) = when (state) {
            Call.STATE_RINGING -> "ringing"
            Call.STATE_DIALING, Call.STATE_CONNECTING, Call.STATE_PULLING_CALL -> "dialing"
            Call.STATE_ACTIVE -> "active"
            Call.STATE_HOLDING -> "held"
            Call.STATE_DISCONNECTING, Call.STATE_DISCONNECTED -> "ended"
            else -> "other"
        }

        private fun routeName(type: Int) = when (type) {
            CallEndpoint.TYPE_SPEAKER -> "speaker"
            CallEndpoint.TYPE_BLUETOOTH -> "bluetooth"
            CallEndpoint.TYPE_WIRED_HEADSET -> "wired"
            CallEndpoint.TYPE_STREAMING -> "streaming"
            else -> "earpiece"
        }

        private fun legacyRouteName(route: Int) = when (route) {
            CallAudioState.ROUTE_SPEAKER -> "speaker"
            CallAudioState.ROUTE_BLUETOOTH -> "bluetooth"
            CallAudioState.ROUTE_WIRED_HEADSET -> "wired"
            else -> "earpiece"
        }
    }
}
