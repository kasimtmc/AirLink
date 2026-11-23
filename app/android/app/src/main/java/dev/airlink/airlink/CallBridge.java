package dev.airlink.airlink;

import android.Manifest;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.media.AudioManager;
import android.os.Handler;
import android.os.Looper;
import android.net.Uri;
import android.os.Build;
import android.provider.CallLog;
import android.telecom.TelecomManager;
import android.telephony.TelephonyManager;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * This phone's calls for paired computers (docs/protocol.md, "call.*" and "calls.*"): a ringing
 * call as it comes (who calls) and its end, the call log, and what a computer asks: answer or
 * decline the ringing call, or call a number.
 */
final class CallBridge {
    private static final ExecutorService worker = Executors.newSingleThreadExecutor(r -> new Thread(r, "airlink-calls"));
    private static BroadcastReceiver receiver;
    private static String lastState = TelephonyManager.EXTRA_STATE_IDLE;
    private static boolean ringingTold;
    private static String lastNumber = "";  // the ringing call's, for the status of the fallback path

    private static final Handler main = new Handler(Looper.getMainLooper());

    private CallBridge() {}

    static boolean granted(Context c) {
        return has(c, Manifest.permission.READ_PHONE_STATE) && has(c, Manifest.permission.READ_CALL_LOG);
    }

    private static boolean has(Context c, String permission) {
        return c.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    static String[] permissions() {
        return Build.VERSION.SDK_INT >= 26
                ? new String[]{Manifest.permission.READ_PHONE_STATE, Manifest.permission.READ_CALL_LOG,
                        Manifest.permission.ANSWER_PHONE_CALLS, Manifest.permission.CALL_PHONE}
                : new String[]{Manifest.permission.READ_PHONE_STATE, Manifest.permission.READ_CALL_LOG,
                        Manifest.permission.CALL_PHONE};
    }

    // ---- The phone's state, while the service runs ------------------------------------------

    /** From AirLinkService: listens for the phone ringing, answered and hung up. */
    static void start(Context c) {
        if (receiver != null) return;
        receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String state = intent.getStringExtra(TelephonyManager.EXTRA_STATE);
                // The number comes only with the call log permission; Android sends RINGING twice,
                // once without it and once with it.
                String number = intent.getStringExtra(TelephonyManager.EXTRA_INCOMING_NUMBER);
                stateChanged(context.getApplicationContext(), state, number);
            }
        };
        IntentFilter filter = new IntentFilter(TelephonyManager.ACTION_PHONE_STATE_CHANGED);
        if (Build.VERSION.SDK_INT >= 33) c.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED);
        else c.registerReceiver(receiver, filter);
    }

    static void stop(Context c) {
        if (receiver == null) return;
        c.unregisterReceiver(receiver);
        receiver = null;
    }

    private static void stateChanged(Context c, String state, String number) {
        if (state == null) return;
        boolean wasRinging = TelephonyManager.EXTRA_STATE_RINGING.equals(lastState);
        lastState = state;
        if (TelephonyManager.EXTRA_STATE_RINGING.equals(state)) {
            if (!wasRinging) ringingTold = false;
            boolean numberKnown = number != null && !number.isEmpty();
            // Tell once, with the number when it comes (the permission's broadcast follows at once).
            if (ringingTold || (!numberKnown && has(c, Manifest.permission.READ_CALL_LOG))) return;
            ringingTold = true;
            String n = numberKnown ? number : "";
            lastNumber = n;
            worker.execute(() -> {
                try {
                    SmsBridge.broadcastAll("call.ringing", new JSONObject().put("number", n)
                            .put("name", n.isEmpty() ? "" : SmsBridge.contactName(c, n)).toString());
                } catch (Exception ignored) {
                }
            });
            statusChanged();
        } else {
            statusChanged();
            String s = TelephonyManager.EXTRA_STATE_OFFHOOK.equals(state) ? "offhook" : "idle";
            worker.execute(() -> {
                try {
                    SmsBridge.broadcastAll("call.state", new JSONObject().put("state", s).toString());
                } catch (Exception ignored) {
                }
            });
        }
    }

    // ---- A computer's requests --------------------------------------------------------------

    // ---- The ongoing call, for computers: told whenever it changes ----------------------------

    /** The call or its audio changed: its state to every computer (on the UI thread, which owns Call). */
    static void statusChanged() {
        main.removeCallbacks(sendStatus);
        main.postDelayed(sendStatus, 150);  // a burst of changes, told once
    }

    private static final Runnable sendStatus = () -> {
        Context c = AirLinkService.appContext;
        if (c == null) return;
        try {
            SmsBridge.broadcastAll("call.status", status(c).toString());
        } catch (Exception ignored) {
        }
    };

    private static JSONObject status(Context c) throws Exception {
        JSONObject out = new JSONObject();
        AirLinkInCallService ics = AirLinkInCallService.instance;
        AudioManager audio = c.getSystemService(AudioManager.class);
        if (ics != null && ics.hasCall()) {
            ics.describe(out);
        } else {
            // What any app can see and do: no hold, and the audio through AudioManager.
            String state = TelephonyManager.EXTRA_STATE_OFFHOOK.equals(lastState) ? "active"
                    : TelephonyManager.EXTRA_STATE_RINGING.equals(lastState) ? "ringing" : "ended";
            out.put("full_control", false).put("state", state).put("held", false).put("can_hold", false)
                    .put("muted", audio.isMicrophoneMute())
                    .put("route", audio.isSpeakerphoneOn() ? "speaker" : "earpiece")
                    .put("routes", new JSONArray().put("earpiece").put("speaker"));
        }
        if (!out.has("number") && !lastNumber.isEmpty()) out.put("number", lastNumber);
        if (out.has("number")) out.put("name", SmsBridge.contactName(c, out.getString("number")));
        out.put("volume", audio.getStreamVolume(AudioManager.STREAM_VOICE_CALL))
                .put("volume_max", audio.getStreamMaxVolume(AudioManager.STREAM_VOICE_CALL));
        return out;
    }

    // Controls of the ongoing call, on the UI thread: the companion InCallService's when Telecom
    // bound it, otherwise the fallbacks any app has.
    @SuppressWarnings("deprecation")
    private static void control(Context c, String kind, JSONObject req) {
        AirLinkInCallService ics = AirLinkInCallService.instance;
        boolean full = ics != null && ics.hasCall();
        AudioManager audio = c.getSystemService(AudioManager.class);
        TelecomManager telecom = c.getSystemService(TelecomManager.class);
        switch (kind) {
            case "call.hangup":
                if (full) ics.hangUp();
                else if (Build.VERSION.SDK_INT >= 28 && has(c, Manifest.permission.ANSWER_PHONE_CALLS)) telecom.endCall();
                break;
            case "call.hold":
                if (full) ics.hold(req.optBoolean("on"));
                break;
            case "call.mute":
                if (full) ics.mute(req.optBoolean("on"));
                else audio.setMicrophoneMute(req.optBoolean("on"));
                break;
            case "call.route":
                if (full) ics.route(req.optString("route"));
                else audio.setSpeakerphoneOn("speaker".equals(req.optString("route")));
                break;
            case "call.volume":
                audio.adjustStreamVolume(AudioManager.STREAM_VOICE_CALL,
                        req.optInt("delta") > 0 ? AudioManager.ADJUST_RAISE : AudioManager.ADJUST_LOWER, 0);
                break;
            default:
                return;
        }
        statusChanged();
    }

    static void handle(Context c, String peerId, String kind, JSONObject req) throws Exception {
        TelecomManager telecom = c.getSystemService(TelecomManager.class);
        switch (kind) {
            case "call.hangup":
            case "call.hold":
            case "call.mute":
            case "call.route":
            case "call.volume":
                main.post(() -> control(c, kind, req));
                break;
            case "call.status_request":
                statusChanged();
                break;
            case "calls.log_request":
                if (!granted(c)) {
                    SmsBridge.send(peerId, "calls.error", new JSONObject().put("error", "no_permission"));
                    return;
                }
                SmsBridge.send(peerId, "calls.log", new JSONObject().put("calls", log(c)));
                break;
            case "call.answer":
                main.post(() -> {
                    AirLinkInCallService ics = AirLinkInCallService.instance;
                    if (ics != null && ics.hasCall()) ics.answer();
                    else if (Build.VERSION.SDK_INT >= 26 && has(c, Manifest.permission.ANSWER_PHONE_CALLS)) telecom.acceptRingingCall();
                });
                break;
            case "call.reject":
                main.post(() -> {
                    AirLinkInCallService ics = AirLinkInCallService.instance;
                    if (ics != null && ics.hasCall()) ics.hangUp();
                    else if (Build.VERSION.SDK_INT >= 28 && has(c, Manifest.permission.ANSWER_PHONE_CALLS)) telecom.endCall();
                });
                break;
            case "call.dial": {
                String number = req.optString("number");
                JSONObject result = new JSONObject().put("number", number);
                if (number.isEmpty() || !has(c, Manifest.permission.CALL_PHONE)) {
                    result.put("ok", false).put("error", number.isEmpty() ? "no_number" : "no_permission");
                } else {
                    // Not an activity: allowed while the app is in the background.
                    telecom.placeCall(Uri.fromParts("tel", number, null), null);
                    result.put("ok", true);
                }
                SmsBridge.send(peerId, "call.dial_result", result);
                break;
            }
            default:
                break;
        }
    }

    // The latest calls, newest first.
    private static JSONArray log(Context c) throws Exception {
        JSONArray list = new JSONArray();
        String[] cols = {CallLog.Calls.NUMBER, CallLog.Calls.CACHED_NAME, CallLog.Calls.TYPE, CallLog.Calls.DATE, CallLog.Calls.DURATION};
        try (Cursor cur = c.getContentResolver().query(CallLog.Calls.CONTENT_URI, cols, null, null, CallLog.Calls.DATE + " DESC")) {
            while (cur != null && cur.moveToNext() && list.length() < 60) {
                String type;
                switch (cur.getInt(2)) {
                    case CallLog.Calls.INCOMING_TYPE: type = "incoming"; break;
                    case CallLog.Calls.OUTGOING_TYPE: type = "outgoing"; break;
                    case CallLog.Calls.MISSED_TYPE: type = "missed"; break;
                    case CallLog.Calls.REJECTED_TYPE: type = "rejected"; break;
                    case CallLog.Calls.BLOCKED_TYPE: type = "blocked"; break;
                    default: type = "other";
                }
                list.put(new JSONObject().put("number", cur.getString(0) != null ? cur.getString(0) : "")
                        .put("name", cur.getString(1) != null ? cur.getString(1) : "").put("type", type)
                        .put("date", cur.getLong(3)).put("duration", cur.getLong(4)));
            }
        }
        return list;
    }
}
