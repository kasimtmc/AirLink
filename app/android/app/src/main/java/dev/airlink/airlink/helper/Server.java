package dev.airlink.airlink.helper;

import android.view.KeyEvent;
import android.view.MotionEvent;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The AirLink app's connection to the helper: one client on 127.0.0.1:port, which must first send
 * the token it started the helper with (other apps on the phone can reach the port too).
 *
 * App to helper, a JSON object per line:
 *   {"token": "..."}                                       first, always
 *   {"cmd": "open", "session": 1, "component": "pkg/.Act", "width": 720, "height": 1560, "dpi": 320}
 *   {"cmd": "close", "session": 1}
 *   {"cmd": "touch", "session": 1, "action": "down"|"move"|"up", "x": 0.5, "y": 0.5}   (fractions)
 *   {"cmd": "scroll", "session": 1, "x": .., "y": .., "v": -1, "h": 0}
 *   {"cmd": "key", "session": 1, "code": 4 (KeyEvent.KEYCODE_*), "meta": 0}
 *   {"cmd": "text", "session": 1, "text": "..."}
 *   {"cmd": "quit"}
 * Helper to app, binary: [u32 length][u8 type][u32 session][payload], length covering the rest;
 *   type 1: a JSON event ({"event":"opened","display":N} | {"event":"closed"} | {"event":"error","message":..})
 *   type 2: a JPEG frame of the session's display.
 */
final class Server {
    private static final int EVENT = 1, FRAME = 2;
    private static final Map<Integer, AppDisplay> sessions = new ConcurrentHashMap<>();
    private static DataOutputStream out;

    private Server() {}

    static void run(int port, String token) throws Exception {
        try (ServerSocket server = new ServerSocket(port, 1, InetAddress.getLoopbackAddress())) {
            Log.i("serving on 127.0.0.1:" + port);
            // The helper lives as long as the app wants it: when the app's connection ends, so does it.
            try (Socket s = server.accept()) {
                s.setTcpNoDelay(true);
                out = new DataOutputStream(s.getOutputStream());
                BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
                String first = in.readLine();
                if (first == null || !token.equals(new JSONObject(first).optString("token"))) {
                    Log.i("wrong token: closing");
                    return;
                }
                send(0, EVENT, "{\"event\":\"ready\"}".getBytes(StandardCharsets.UTF_8));
                String line;
                while ((line = in.readLine()) != null) {
                    try {
                        if (!handle(new JSONObject(line))) break;
                    } catch (Exception e) {
                        Log.e("command failed: " + line, e);
                    }
                }
            } finally {
                for (AppDisplay d : sessions.values()) d.release();
                sessions.clear();
                Log.i("client gone: exiting");
            }
        }
    }

    private static boolean handle(JSONObject c) throws Exception {
        int session = c.optInt("session");
        AppDisplay d = sessions.get(session);
        switch (c.optString("cmd")) {
            case "open": {
                if (d != null) d.release();
                int w = c.optInt("width", 720), h = c.optInt("height", 1560), dpi = c.optInt("dpi", 320);
                d = new AppDisplay(String.valueOf(session), w, h, dpi, (display, jpeg) -> send(session, FRAME, jpeg));
                d.setQuality(c.optInt("quality", 70), c.optInt("fps", 24));
                sessions.put(session, d);
                boolean started = Helper.launch(d.id(), c.getString("component"));
                event(session, started ? new JSONObject().put("event", "opened").put("display", d.id())
                        .put("width", w).put("height", h)
                        : new JSONObject().put("event", "error").put("message", "the app could not be started"));
                break;
            }
            case "close":
                if (d != null) {
                    sessions.remove(session);
                    d.release();  // DESTROY_CONTENT_ON_REMOVAL: its activities end with it
                    event(session, new JSONObject().put("event", "closed"));
                }
                break;
            case "touch": {
                if (d == null) break;
                int action = switch (c.optString("action")) {
                    case "down" -> MotionEvent.ACTION_DOWN;
                    case "up" -> MotionEvent.ACTION_UP;
                    default -> MotionEvent.ACTION_MOVE;
                };
                Input.touch(d.id(), action, (float) c.optDouble("x") * d.logicalWidth(), (float) c.optDouble("y") * d.logicalHeight());
                break;
            }
            case "scroll":
                if (d != null) Input.scroll(d.id(), (float) c.optDouble("x") * d.logicalWidth(), (float) c.optDouble("y") * d.logicalHeight(),
                        (float) c.optDouble("h"), (float) c.optDouble("v"), d.logicalWidth(), d.logicalHeight());
                break;
            case "key":
                if (d != null) Input.key(d.id(), c.optInt("code", KeyEvent.KEYCODE_BACK), c.optInt("meta"));
                break;
            case "text":
                if (d != null) Input.text(d.id(), c.optString("text"));
                break;
            case "quit":
                return false;
            default:
                break;
        }
        return true;
    }

    private static void event(int session, JSONObject e) {
        send(session, EVENT, e.toString().getBytes(StandardCharsets.UTF_8));
    }

    // From the displays' threads and the command loop: one message at a time.
    private static synchronized void send(int session, int type, byte[] payload) {
        try {
            out.writeInt(1 + 4 + payload.length);
            out.writeByte(type);
            out.writeInt(session);
            out.write(payload);
            out.flush();
        } catch (IOException e) {
            Log.e("send failed", e);  // the app went away; the read loop ends too
        }
    }
}
