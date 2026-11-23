package dev.airlink.airlink;

import android.os.Build;
import android.os.OutcomeReceiver;
import android.telecom.Call;
import android.telecom.CallAudioState;
import android.telecom.CallEndpoint;
import android.telecom.CallEndpointException;
import android.telecom.InCallService;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.List;

/**
 * A calling companion (CALL_COMPANION_APP, no UI of its own): when Telecom binds it during a call,
 * a paired computer can hold, mute, switch the audio (earpiece, speaker, Bluetooth, wired headset)
 * and hang up. Without it CallBridge falls back to what any app may do.
 */
public class AirLinkInCallService extends InCallService {
    static volatile AirLinkInCallService instance;
    private Call call;
    private List<CallEndpoint> endpoints;  // Android 14+
    private CallEndpoint endpoint;

    private final Call.Callback callback = new Call.Callback() {
        @Override
        public void onStateChanged(Call c, int state) {
            CallBridge.statusChanged();
        }

        @Override
        public void onDetailsChanged(Call c, Call.Details details) {
            CallBridge.statusChanged();
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        android.util.Log.d("AirLink", "in-call service bound: full call controls available");
    }

    @Override
    public void onDestroy() {
        if (instance == this) instance = null;
        super.onDestroy();
    }

    @Override
    public void onCallAdded(Call c) {
        if (call != null) call.unregisterCallback(callback);
        call = c;
        c.registerCallback(callback);
        CallBridge.statusChanged();
    }

    @Override
    public void onCallRemoved(Call c) {
        c.unregisterCallback(callback);
        if (call == c) call = getCalls().isEmpty() ? null : getCalls().get(0);
        CallBridge.statusChanged();
    }

    @Override
    public void onCallAudioStateChanged(CallAudioState state) {
        CallBridge.statusChanged();
    }

    @Override
    public void onCallEndpointChanged(CallEndpoint e) {
        endpoint = e;
        CallBridge.statusChanged();
    }

    @Override
    public void onAvailableCallEndpointsChanged(List<CallEndpoint> list) {
        endpoints = list;
        CallBridge.statusChanged();
    }

    @Override
    public void onMuteStateChanged(boolean muted) {
        CallBridge.statusChanged();
    }

    // ---- For CallBridge ---------------------------------------------------------------------

    boolean hasCall() {
        return call != null;
    }

    /** The call's state for computers: what it is, the audio, and what can be done. */
    void describe(JSONObject out) throws Exception {
        out.put("full_control", true);
        if (call == null) return;
        int state = Build.VERSION.SDK_INT >= 31 ? call.getDetails().getState() : call.getState();
        out.put("state", stateName(state)).put("held", state == Call.STATE_HOLDING)
                .put("can_hold", call.getDetails().can(Call.Details.CAPABILITY_HOLD) || call.getDetails().can(Call.Details.CAPABILITY_SUPPORT_HOLD))
                .put("since", call.getDetails().getConnectTimeMillis());
        if (call.getDetails().getHandle() != null) out.put("number", call.getDetails().getHandle().getSchemeSpecificPart());
        CallAudioState audio = getCallAudioState();
        if (audio != null) out.put("muted", audio.isMuted());
        JSONArray routes = new JSONArray();
        String current = "";
        if (Build.VERSION.SDK_INT >= 34 && endpoints != null) {
            for (CallEndpoint e : endpoints) routes.put(routeName(e.getEndpointType()));
            if (endpoint != null) current = routeName(endpoint.getEndpointType());
        } else if (audio != null) {
            int mask = audio.getSupportedRouteMask();
            if ((mask & CallAudioState.ROUTE_EARPIECE) != 0) routes.put("earpiece");
            if ((mask & CallAudioState.ROUTE_SPEAKER) != 0) routes.put("speaker");
            if ((mask & CallAudioState.ROUTE_BLUETOOTH) != 0) routes.put("bluetooth");
            if ((mask & CallAudioState.ROUTE_WIRED_HEADSET) != 0) routes.put("wired");
            current = legacyRouteName(audio.getRoute());
        }
        out.put("routes", routes).put("route", current);
    }

    void hangUp() {
        if (call != null) call.disconnect();
    }

    void answer() {
        if (call != null) call.answer(0);
    }

    void hold(boolean on) {
        if (call == null) return;
        if (on) call.hold();
        else call.unhold();
    }

    void mute(boolean on) {
        setMuted(on);
    }

    void route(String name) {
        if (Build.VERSION.SDK_INT >= 34 && endpoints != null) {
            for (CallEndpoint e : endpoints) {
                if (!routeName(e.getEndpointType()).equals(name)) continue;
                requestCallEndpointChange(e, getMainExecutor(), new OutcomeReceiver<Void, CallEndpointException>() {
                    @Override
                    public void onResult(Void r) {}

                    @Override
                    public void onError(CallEndpointException err) {
                        android.util.Log.w("AirLink", "audio route change failed: " + err.getMessage());
                    }
                });
                return;
            }
        }
        int r;
        switch (name) {
            case "speaker": r = CallAudioState.ROUTE_SPEAKER; break;
            case "bluetooth": r = CallAudioState.ROUTE_BLUETOOTH; break;
            case "wired": r = CallAudioState.ROUTE_WIRED_HEADSET; break;
            default: r = CallAudioState.ROUTE_EARPIECE;
        }
        setAudioRoute(r);
    }

    private static String stateName(int s) {
        switch (s) {
            case Call.STATE_RINGING: return "ringing";
            case Call.STATE_DIALING:
            case Call.STATE_CONNECTING:
            case Call.STATE_PULLING_CALL: return "dialing";
            case Call.STATE_ACTIVE: return "active";
            case Call.STATE_HOLDING: return "held";
            case Call.STATE_DISCONNECTING:
            case Call.STATE_DISCONNECTED: return "ended";
            default: return "other";
        }
    }

    private static String routeName(int type) {
        switch (type) {
            case CallEndpoint.TYPE_SPEAKER: return "speaker";
            case CallEndpoint.TYPE_BLUETOOTH: return "bluetooth";
            case CallEndpoint.TYPE_WIRED_HEADSET: return "wired";
            case CallEndpoint.TYPE_STREAMING: return "streaming";
            default: return "earpiece";
        }
    }

    private static String legacyRouteName(int route) {
        switch (route) {
            case CallAudioState.ROUTE_SPEAKER: return "speaker";
            case CallAudioState.ROUTE_BLUETOOTH: return "bluetooth";
            case CallAudioState.ROUTE_WIRED_HEADSET: return "wired";
            default: return "earpiece";
        }
    }
}
