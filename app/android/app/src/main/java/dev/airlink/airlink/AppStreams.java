package dev.airlink.airlink;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.Base64;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Advanced mode on the phone: this phone's apps, each opened on a display of its own for a paired
 * computer. The privileged work is the helper's (dev.airlink.airlink.helper, started as the shell
 * over the phone's own wireless debugging); this relays between it and the computer:
 *   apps.list -> apps.list_result {list, part, parts, apps: [{label, component, icon (PNG, base64)}]}, in parts
 *     (app messages are at most 1 MiB, and with icons a phone's apps are more)
 *   apps.open {session, component, width, height, dpi} / apps.close {session} /
 *   apps.input {session, cmd: touch|scroll|key|text, ...}  -> the helper
 *   the helper's frames -> media stream "app:<session>"; its events -> apps.event {session, event, ...}
 */
final class AppStreams {
    private static final ExecutorService worker = Executors.newSingleThreadExecutor(r -> new Thread(r, "airlink-apps"));
    private static final Map<Integer, String> sessionPeer = new ConcurrentHashMap<>();
    private static final java.util.ArrayDeque<String> waiting = new java.util.ArrayDeque<>();  // lines for a helper still starting
    private static Socket helper;
    private static OutputStream helperOut;
    private static boolean starting;
    private static int port;
    private static String token;

    private AppStreams() {}

    static void handle(Context c, String peerId, String kind, JSONObject req) {
        worker.execute(() -> {
            try {
                switch (kind) {
                    case "apps.list":
                        sendList(peerId, list(c));
                        break;
                    case "apps.open":
                        sessionPeer.put(req.optInt("session"), peerId);
                        toHelper(req.put("cmd", "open"));
                        break;
                    case "apps.close":
                        toHelper(req.put("cmd", "close"));
                        break;
                    case "apps.input": {
                        JSONObject input = req;
                        if ("text".equals(req.optString("cmd")) && !typeable(req.optString("text"))) {
                            // Letters the key map lacks (ç, ğ, ş, emoji...): through the clipboard, then a paste.
                            paste(c, req.optString("text"));
                            input = new JSONObject().put("cmd", "key").put("session", req.optInt("session"))
                                    .put("code", android.view.KeyEvent.KEYCODE_PASTE);
                        }
                        toHelper(input);
                        break;
                    }
                    default:
                        break;
                }
            } catch (Exception e) {
                android.util.Log.w("AirLink", "apps request " + kind + " failed", e);
            }
        });
    }

    // Whether the helper can type it as keys.
    private static boolean typeable(String text) {
        if (text.isEmpty()) return true;
        android.view.KeyCharacterMap map = android.view.KeyCharacterMap.load(android.view.KeyCharacterMap.VIRTUAL_KEYBOARD);
        return map.getEvents(text.toCharArray()) != null;
    }

    // Puts text on the phone's clipboard (an app may write it from the background, not read it).
    private static void paste(Context c, String text) throws Exception {
        java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
        new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
            try {
                android.content.ClipboardManager cm = c.getSystemService(android.content.ClipboardManager.class);
                cm.setPrimaryClip(android.content.ClipData.newPlainText("AirLink", text));
            } finally {
                done.countDown();
            }
        });
        done.await(2, java.util.concurrent.TimeUnit.SECONDS);
    }

    // ---- The phone's apps ---------------------------------------------------------------------

    private static JSONArray list(Context c) throws Exception {
        PackageManager pm = c.getPackageManager();
        Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        List<ResolveInfo> found = pm.queryIntentActivities(launcher, 0);
        found.sort((a, b) -> a.loadLabel(pm).toString().compareToIgnoreCase(b.loadLabel(pm).toString()));
        JSONArray apps = new JSONArray();
        for (ResolveInfo r : found) {
            if (r.activityInfo.packageName.equals(c.getPackageName())) continue;
            apps.put(new JSONObject()
                    .put("label", r.loadLabel(pm).toString())
                    .put("component", r.activityInfo.packageName + "/" + r.activityInfo.name)
                    .put("icon", icon(r.loadIcon(pm))));
        }
        return apps;
    }

    // In parts of ~400 KB, numbered: they may arrive in any order (each is a stream of its own).
    private static void sendList(String peerId, JSONArray apps) throws Exception {
        java.util.List<JSONArray> parts = new java.util.ArrayList<>();
        JSONArray part = new JSONArray();
        int size = 0;
        for (int i = 0; i < apps.length(); i++) {
            JSONObject a = apps.getJSONObject(i);
            int n = a.toString().length();
            if (part.length() > 0 && size + n > 400 * 1024) {
                parts.add(part);
                part = new JSONArray();
                size = 0;
            }
            part.put(a);
            size += n;
        }
        parts.add(part);
        String list = Long.toHexString(System.nanoTime());  // which listing the parts belong to
        for (int i = 0; i < parts.size(); i++)
            SmsBridge.send(peerId, "apps.list_result", new JSONObject().put("list", list).put("part", i)
                    .put("parts", parts.size()).put("apps", parts.get(i)));
    }

    private static String icon(Drawable d) {
        int size = 72;
        Bitmap b = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(b);
        d.setBounds(0, 0, size, size);
        d.draw(canvas);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        b.compress(Bitmap.CompressFormat.PNG, 100, out);
        b.recycle();
        return Base64.encodeToString(out.toByteArray(), Base64.NO_WRAP);
    }

    // ---- The helper ---------------------------------------------------------------------------

    // A command line for the helper; started first if it doesn't run (worker thread).
    private static void toHelper(JSONObject command) throws Exception {
        String line = command.toString() + "\n";
        if (helperOut != null) {
            try {
                helperOut.write(line.getBytes(StandardCharsets.UTF_8));
                helperOut.flush();
                return;
            } catch (Exception e) {
                closeHelper();  // it ended (the phone restarted wireless debugging...): start it again
            }
        }
        synchronized (waiting) {
            waiting.add(line);
        }
        if (starting) return;
        starting = true;
        port = 20000 + new SecureRandom().nextInt(40000);
        byte[] t = new byte[16];
        new SecureRandom().nextBytes(t);
        StringBuilder hex = new StringBuilder();
        for (byte b : t) hex.append(String.format("%02x", b));
        token = hex.toString();
        String cmd = "CLASSPATH=$(pm path dev.airlink.airlink | head -1 | cut -d: -f2) setsid app_process / "
                + "dev.airlink.airlink.helper.Helper serve " + port + " " + token
                + " </dev/null >/data/local/tmp/airlink-helper.log 2>&1 &";
        if (MainActivity.nativeReady) MainActivity.startHelper(cmd);  // over wireless debugging: helperStarted follows
    }

    /** From native code: the shell command that starts the helper ran (or not). */
    static void helperStarted(boolean ok, String message) {
        if (!ok) android.util.Log.w("AirLink", "helper not started: " + message);
        worker.execute(() -> {
            if (!ok) {
                starting = false;
                failAll("Gelişmiş moda bağlanılamadı: " + message);
                return;
            }
            // It starts in the background: try for a few seconds.
            for (int i = 0; i < 40 && helper == null; i++) {
                try {
                    Socket s = new Socket(InetAddress.getLoopbackAddress(), port);
                    s.setTcpNoDelay(true);
                    helper = s;
                    helperOut = s.getOutputStream();
                    helperOut.write(("{\"token\":\"" + token + "\"}\n").getBytes(StandardCharsets.UTF_8));
                    helperOut.flush();
                    new Thread(() -> read(s), "airlink-helper-in").start();
                } catch (Exception e) {
                    try {
                        Thread.sleep(150);
                    } catch (InterruptedException ignored) {
                    }
                }
            }
            starting = false;
            if (helper == null) {
                failAll("Yardımcı başlamadı (/data/local/tmp/airlink-helper.log)");
                return;
            }
            synchronized (waiting) {
                try {
                    while (!waiting.isEmpty()) helperOut.write(waiting.poll().getBytes(StandardCharsets.UTF_8));
                    helperOut.flush();
                } catch (Exception e) {
                    closeHelper();
                }
            }
        });
    }

    // The helper's messages: [u32 length][u8 type][u32 session][payload].
    private static void read(Socket s) {
        try (DataInputStream in = new DataInputStream(s.getInputStream())) {
            while (true) {
                int length = in.readInt();
                int type = in.readUnsignedByte();
                int session = in.readInt();
                byte[] payload = new byte[length - 5];
                in.readFully(payload);
                String peer = sessionPeer.get(session);
                if (type == 2 && peer != null) {
                    if (MainActivity.nativeReady) nativeSendFrame(peer, "app:" + session, payload);
                } else if (type == 1 && peer != null) {
                    JSONObject e = new JSONObject(new String(payload, StandardCharsets.UTF_8)).put("session", session);
                    SmsBridge.send(peer, "apps.event", e);
                    if ("closed".equals(e.optString("event"))) {
                        sessionPeer.remove(session);
                        nativeStopFrames(peer, "app:" + session);
                    }
                }
            }
        } catch (Exception e) {
            android.util.Log.i("AirLink", "helper connection ended", e);
        } finally {
            worker.execute(AppStreams::closeHelper);
        }
    }

    private static void closeHelper() {
        try {
            if (helper != null) helper.close();
        } catch (Exception ignored) {
        }
        helper = null;
        helperOut = null;
        for (Map.Entry<Integer, String> e : sessionPeer.entrySet()) {
            try {
                SmsBridge.send(e.getValue(), "apps.event", new JSONObject().put("session", e.getKey()).put("event", "closed"));
            } catch (Exception ignored) {
            }
        }
        sessionPeer.clear();
    }

    private static void failAll(String message) {
        synchronized (waiting) {
            waiting.clear();
        }
        for (Map.Entry<Integer, String> e : sessionPeer.entrySet()) {
            try {
                SmsBridge.send(e.getValue(), "apps.event", new JSONObject().put("session", e.getKey()).put("event", "error").put("message", message));
            } catch (Exception ignored) {
            }
        }
        sessionPeer.clear();
    }

    private static native void nativeSendFrame(String peerId, String name, byte[] jpeg);

    private static native void nativeStopFrames(String peerId, String name);
}
