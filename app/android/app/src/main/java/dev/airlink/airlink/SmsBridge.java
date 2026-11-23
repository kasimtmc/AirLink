package dev.airlink.airlink;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.ContactsContract;
import android.provider.Telephony;
import android.telephony.SmsManager;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * This phone's SMS for paired computers (docs/protocol.md, app messages "sms.*"): its conversations,
 * a conversation's messages, sending, and messages as they arrive. Runs off the UI thread; answers go
 * back to the asking computer as app messages.
 */
final class SmsBridge {
    private static final ExecutorService worker = Executors.newSingleThreadExecutor(r -> new Thread(r, "airlink-sms"));
    private static final int MAX_THREADS = 60;
    private static final int MAX_MESSAGES = 150;

    private SmsBridge() {}

    static boolean granted(Context c) {
        return c.checkSelfPermission(Manifest.permission.READ_SMS) == PackageManager.PERMISSION_GRANTED
                && c.checkSelfPermission(Manifest.permission.SEND_SMS) == PackageManager.PERMISSION_GRANTED;
    }

    /** From native code: a computer's "sms.*" request. */
    static void handle(String peerId, String kind, String body) {
        Context c = AirLinkService.appContext;
        if (c == null) return;
        worker.execute(() -> {
            try {
                JSONObject req = new JSONObject(body);
                if (kind.equals("contacts.search")) {  // only needs the contacts permission
                    reply(peerId, "contacts.results", searchContacts(c, req.optString("query")));
                    return;
                }
                if (!granted(c)) {
                    reply(peerId, "sms.error", new JSONObject().put("error", "no_permission").put("request", kind));
                    return;
                }
                switch (kind) {
                    case "sms.threads_request":
                        reply(peerId, "sms.threads", new JSONObject().put("threads", threads(c)));
                        break;
                    case "sms.thread_request": {
                        long id = req.getLong("thread_id");
                        reply(peerId, "sms.messages", new JSONObject().put("thread_id", id).put("messages", messages(c, id)));
                        break;
                    }
                    case "sms.send": {
                        String address = req.getString("address"), text = req.getString("text");
                        JSONObject result = new JSONObject().put("client_id", req.optString("client_id")).put("address", address);
                        try {
                            send(c, address, text);
                            result.put("ok", true);
                        } catch (Exception e) {
                            result.put("ok", false).put("error", String.valueOf(e.getMessage()));
                        }
                        reply(peerId, "sms.sent", result);
                        break;
                    }
                    default:
                        break;
                }
            } catch (Exception e) {
                android.util.Log.w("AirLink", "sms request " + kind + " failed", e);
            }
        });
    }

    /** An SMS arrived (SmsReceiver): to every paired computer. */
    static void received(Context c, String address, String body, long date) {
        worker.execute(() -> {
            try {
                JSONObject m = new JSONObject().put("address", address).put("name", contactName(c, address))
                        .put("body", body).put("date", date).put("thread_id", threadOf(c, address));
                if (MainActivity.nativeReady) nativeBroadcast("sms.received", m.toString());
            } catch (Exception ignored) {
            }
        });
    }

    // The latest message of each conversation, newest conversation first.
    private static JSONArray threads(Context c) throws Exception {
        Map<Long, JSONObject> byThread = new LinkedHashMap<>();
        Map<String, String> names = new HashMap<>();
        String[] cols = {Telephony.Sms.THREAD_ID, Telephony.Sms.ADDRESS, Telephony.Sms.BODY, Telephony.Sms.DATE, Telephony.Sms.READ,
                Telephony.Sms.TYPE};
        try (Cursor cur = c.getContentResolver().query(Telephony.Sms.CONTENT_URI, cols, null, null, Telephony.Sms.DATE + " DESC")) {
            int scanned = 0;
            while (cur != null && cur.moveToNext() && byThread.size() < MAX_THREADS && scanned++ < 5000) {
                long thread = cur.getLong(0);
                JSONObject t = byThread.get(thread);
                boolean unread = cur.getInt(4) == 0 && cur.getInt(5) == Telephony.Sms.MESSAGE_TYPE_INBOX;
                if (t != null) {
                    if (unread) t.put("unread", t.getInt("unread") + 1);
                    continue;
                }
                String address = cur.getString(1) != null ? cur.getString(1) : "";
                if (!names.containsKey(address)) names.put(address, contactName(c, address));
                byThread.put(thread, new JSONObject().put("thread_id", thread).put("address", address).put("name", names.get(address))
                        .put("snippet", cur.getString(2) != null ? cur.getString(2) : "").put("date", cur.getLong(3))
                        .put("unread", unread ? 1 : 0));
            }
        }
        return new JSONArray(new ArrayList<>(byThread.values()));
    }

    // A conversation's latest messages, oldest first.
    private static JSONArray messages(Context c, long threadId) throws Exception {
        ArrayList<JSONObject> list = new ArrayList<>();
        String[] cols = {Telephony.Sms._ID, Telephony.Sms.BODY, Telephony.Sms.DATE, Telephony.Sms.TYPE};
        try (Cursor cur = c.getContentResolver().query(Telephony.Sms.CONTENT_URI, cols, Telephony.Sms.THREAD_ID + "=?",
                new String[]{String.valueOf(threadId)}, Telephony.Sms.DATE + " DESC")) {
            while (cur != null && cur.moveToNext() && list.size() < MAX_MESSAGES) {
                int type = cur.getInt(3);
                list.add(0, new JSONObject().put("id", cur.getLong(0)).put("body", cur.getString(1) != null ? cur.getString(1) : "")
                        .put("date", cur.getLong(2)).put("outgoing", type != Telephony.Sms.MESSAGE_TYPE_INBOX)
                        .put("failed", type == Telephony.Sms.MESSAGE_TYPE_FAILED));
            }
        }
        return new JSONArray(list);
    }

    private static long threadOf(Context c, String address) {
        try {
            return Telephony.Threads.getOrCreateThreadId(c, address);
        } catch (Exception e) {
            return -1;
        }
    }

    private static void send(Context c, String address, String text) {
        SmsManager sms = Build.VERSION.SDK_INT >= 31 ? c.getSystemService(SmsManager.class) : SmsManager.getDefault();
        ArrayList<String> parts = sms.divideMessage(text);
        // Not the default SMS app: the system records the sent message in the conversation itself.
        if (parts.size() == 1) sms.sendTextMessage(address, null, text, null, null);
        else sms.sendMultipartTextMessage(address, null, parts, null, null);
    }

    // Contacts whose name or number contains `query`, with each of their numbers.
    private static JSONObject searchContacts(Context c, String query) throws Exception {
        JSONObject out = new JSONObject().put("query", query);
        JSONArray list = new JSONArray();
        if (c.checkSelfPermission(Manifest.permission.READ_CONTACTS) != PackageManager.PERMISSION_GRANTED) {
            return out.put("contacts", list).put("error", "no_permission");
        }
        String q = query.trim();
        if (!q.isEmpty()) {
            String digits = q.replaceAll("[^0-9+]", "");
            String sel = ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME + " LIKE ?"
                    + (digits.length() >= 2 ? " OR " + ContactsContract.CommonDataKinds.Phone.NUMBER + " LIKE ?" : "");
            String[] args = digits.length() >= 2 ? new String[]{"%" + q + "%", "%" + digits + "%"} : new String[]{"%" + q + "%"};
            String[] cols = {ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME, ContactsContract.CommonDataKinds.Phone.NUMBER,
                    ContactsContract.CommonDataKinds.Phone.TYPE, ContactsContract.CommonDataKinds.Phone.LABEL};
            java.util.HashSet<String> seen = new java.util.HashSet<>();
            try (Cursor cur = c.getContentResolver().query(ContactsContract.CommonDataKinds.Phone.CONTENT_URI, cols, sel, args,
                    ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME + " ASC")) {
                while (cur != null && cur.moveToNext() && list.length() < 20) {
                    String name = cur.getString(0), number = cur.getString(1);
                    if (number == null || !seen.add(name + "|" + number.replaceAll("[^0-9+]", ""))) continue;
                    CharSequence label = ContactsContract.CommonDataKinds.Phone.getTypeLabel(c.getResources(), cur.getInt(2), cur.getString(3));
                    list.put(new JSONObject().put("name", name != null ? name : "").put("number", number)
                            .put("label", label != null ? label.toString() : ""));
                }
            }
        }
        return out.put("contacts", list);
    }

    // The contact's name for a number, or "" (without the contacts permission too).
    private static String contactName(Context c, String address) {
        if (address == null || address.isEmpty()) return "";
        if (c.checkSelfPermission(Manifest.permission.READ_CONTACTS) != PackageManager.PERMISSION_GRANTED) return "";
        Uri uri = Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode(address));
        try (Cursor cur = c.getContentResolver().query(uri, new String[]{ContactsContract.PhoneLookup.DISPLAY_NAME}, null, null, null)) {
            if (cur != null && cur.moveToFirst()) return cur.getString(0);
        } catch (Exception ignored) {
        }
        return "";
    }

    private static void reply(String peerId, String kind, JSONObject body) {
        if (MainActivity.nativeReady) nativeSendTo(peerId, kind, body.toString());
    }

    private static native void nativeSendTo(String peerId, String kind, String body);

    private static native void nativeBroadcast(String kind, String body);
}
