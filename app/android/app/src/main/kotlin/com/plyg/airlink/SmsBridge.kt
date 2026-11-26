package com.plyg.airlink

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.ContactsContract
import android.provider.ContactsContract.CommonDataKinds.Phone
import android.provider.Telephony
import android.telephony.SmsManager
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.Executors

/** Serves SMS and contact requests from paired computers off the UI thread. */
object SmsBridge {
    private val worker = Executors.newSingleThreadExecutor { task -> Thread(task, "airlink-sms") }
    private const val MAX_THREADS = 60
    private const val MAX_MESSAGES = 150

    @JvmStatic
    fun granted(context: Context): Boolean =
        context.checkSelfPermission(Manifest.permission.READ_SMS) == PackageManager.PERMISSION_GRANTED &&
            context.checkSelfPermission(Manifest.permission.SEND_SMS) == PackageManager.PERMISSION_GRANTED

    /** Called by the native bridge for a paired computer's app message. */
    @JvmStatic
    fun handle(peerId: String, kind: String, body: String) {
        val context = AirLinkService.appContext ?: return
        worker.execute {
            try {
                val request = JSONObject(body)
                when {
                    kind.startsWith("apps.") -> AppStreams.handle(context, peerId, kind, request)
                    kind.startsWith("screen.") -> Handler(Looper.getMainLooper()).post {
                        when (kind) {
                            "screen.start" -> ScreenShare.requested(context, peerId, request)
                            "screen.stop" -> ScreenShare.stopVideo(peerId)
                            "screen.input" -> if (ScreenShare.active() && ScreenShare.peer() == peerId)
                                AirLinkAccessibility.input(request)
                        }
                    }
                    kind.startsWith("call") -> CallBridge.handle(context, peerId, kind, request)
                    kind.startsWith("media.") -> PhoneMedia.handle(peerId, kind, request)
                    kind == "contacts.search" -> reply(peerId, "contacts.results", searchContacts(context, request.optString("query")))
                    kind == "contacts.list" -> reply(peerId, "contacts.page", listContacts(context,
                        request.optString("query"), request.optInt("offset").coerceAtLeast(0)))
                    !granted(context) -> reply(
                        peerId, "sms.error", JSONObject().put("error", "no_permission").put("request", kind),
                    )
                    kind == "sms.threads_request" -> reply(peerId, "sms.threads", JSONObject().put("threads", threads(context)))
                    kind == "sms.thread_request" -> {
                        val id = request.getLong("thread_id")
                        reply(peerId, "sms.messages", JSONObject().put("thread_id", id).put("messages", messages(context, id)))
                    }
                    kind == "sms.send" -> {
                        val address = request.getString("address")
                        val text = request.getString("text")
                        val result = JSONObject().put("client_id", request.optString("client_id")).put("address", address)
                        try {
                            sendSms(context, address, text)
                            result.put("ok", true)
                        } catch (error: Exception) {
                            result.put("ok", false).put("error", error.message.orEmpty())
                        }
                        reply(peerId, "sms.sent", result)
                    }
                }
            } catch (error: Exception) {
                android.util.Log.w("AirLink", "sms request $kind failed", error)
            }
        }
    }

    @JvmStatic
    fun received(context: Context, address: String?, body: String, date: Long) {
        worker.execute {
            try {
                val sender = address.orEmpty()
                val message = JSONObject().put("address", sender).put("name", contactName(context, sender))
                    .put("body", body).put("date", date).put("thread_id", threadOf(context, sender))
                if (MainActivity.nativeReady) nativeBroadcast("sms.received", message.toString())
            } catch (_: Exception) {
                // A contact or SMS provider may disappear while a broadcast is being handled.
            }
        }
    }

    private fun threads(context: Context): JSONArray {
        val byThread = linkedMapOf<Long, JSONObject>()
        val names = mutableMapOf<String, String>()
        val cols = arrayOf(Telephony.Sms.THREAD_ID, Telephony.Sms.ADDRESS, Telephony.Sms.BODY,
            Telephony.Sms.DATE, Telephony.Sms.READ, Telephony.Sms.TYPE)
        context.contentResolver.query(Telephony.Sms.CONTENT_URI, cols, null, null, "${Telephony.Sms.DATE} DESC")?.use { cursor ->
            var scanned = 0
            while (cursor.moveToNext() && byThread.size < MAX_THREADS && scanned++ < 5000) {
                val thread = cursor.getLong(0)
                val unread = cursor.getInt(4) == 0 && cursor.getInt(5) == Telephony.Sms.MESSAGE_TYPE_INBOX
                val existing = byThread[thread]
                if (existing != null) {
                    if (unread) existing.put("unread", existing.getInt("unread") + 1)
                    continue
                }
                val address = cursor.getString(1).orEmpty()
                val name = names.getOrPut(address) { contactName(context, address) }
                byThread[thread] = JSONObject().put("thread_id", thread).put("address", address).put("name", name)
                    .put("snippet", cursor.getString(2).orEmpty()).put("date", cursor.getLong(3))
                    .put("unread", if (unread) 1 else 0)
            }
        }
        return JSONArray(byThread.values.toList())
    }

    private fun messages(context: Context, threadId: Long): JSONArray {
        val list = ArrayList<JSONObject>()
        val cols = arrayOf(Telephony.Sms._ID, Telephony.Sms.BODY, Telephony.Sms.DATE, Telephony.Sms.TYPE)
        context.contentResolver.query(Telephony.Sms.CONTENT_URI, cols, "${Telephony.Sms.THREAD_ID}=?",
            arrayOf(threadId.toString()), "${Telephony.Sms.DATE} DESC")?.use { cursor ->
            while (cursor.moveToNext() && list.size < MAX_MESSAGES) {
                val type = cursor.getInt(3)
                list.add(0, JSONObject().put("id", cursor.getLong(0)).put("body", cursor.getString(1).orEmpty())
                    .put("date", cursor.getLong(2)).put("outgoing", type != Telephony.Sms.MESSAGE_TYPE_INBOX)
                    .put("failed", type == Telephony.Sms.MESSAGE_TYPE_FAILED))
            }
        }
        return JSONArray(list)
    }

    private fun threadOf(context: Context, address: String): Long = try {
        Telephony.Threads.getOrCreateThreadId(context, address)
    } catch (_: Exception) { -1L }

    private fun sendSms(context: Context, address: String, text: String) {
        val sms = if (Build.VERSION.SDK_INT >= 31) context.getSystemService(SmsManager::class.java) else SmsManager.getDefault()
        val parts = sms.divideMessage(text)
        if (parts.size == 1) sms.sendTextMessage(address, null, text, null, null)
        else sms.sendMultipartTextMessage(address, null, parts, null, null)
    }

    private fun searchContacts(context: Context, query: String): JSONObject {
        val out = JSONObject().put("query", query)
        val list = JSONArray()
        if (context.checkSelfPermission(Manifest.permission.READ_CONTACTS) != PackageManager.PERMISSION_GRANTED)
            return out.put("contacts", list).put("error", "no_permission")
        val trimmed = query.trim()
        if (trimmed.isNotEmpty()) {
            val digits = trimmed.replace(Regex("[^0-9+]"), "")
            val selection = "${ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME} LIKE ?" +
                if (digits.length >= 2) " OR ${ContactsContract.CommonDataKinds.Phone.NUMBER} LIKE ?" else ""
            val args = if (digits.length >= 2) arrayOf("%$trimmed%", "%$digits%") else arrayOf("%$trimmed%")
            val cols = arrayOf(ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME, ContactsContract.CommonDataKinds.Phone.NUMBER,
                ContactsContract.CommonDataKinds.Phone.TYPE, ContactsContract.CommonDataKinds.Phone.LABEL)
            val seen = hashSetOf<String>()
            context.contentResolver.query(ContactsContract.CommonDataKinds.Phone.CONTENT_URI, cols, selection, args,
                "${ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME} ASC")?.use { cursor ->
                while (cursor.moveToNext() && list.length() < 20) {
                    val name = cursor.getString(0)
                    val number = cursor.getString(1) ?: continue
                    if (!seen.add("$name|${number.replace(Regex("[^0-9+]"), "")}")) continue
                    val label = ContactsContract.CommonDataKinds.Phone.getTypeLabel(context.resources, cursor.getInt(2), cursor.getString(3))
                    list.put(JSONObject().put("name", name.orEmpty()).put("number", number)
                        .put("label", label?.toString().orEmpty()))
                }
            }
        }
        return out.put("contacts", list)
    }

    private fun listContacts(context: Context, query: String, offset: Int): JSONObject {
        val out = JSONObject().put("query", query).put("offset", offset)
        val contacts = JSONArray()
        if (context.checkSelfPermission(Manifest.permission.READ_CONTACTS) != PackageManager.PERMISSION_GRANTED)
            return out.put("contacts", contacts).put("has_more", false).put("error", "no_permission")

        val trimmed = query.trim()
        val digits = trimmed.replace(Regex("[^0-9+]"), "")
        val selection = if (trimmed.isEmpty()) null else "${Phone.DISPLAY_NAME} LIKE ?" +
            if (digits.length >= 2) " OR ${Phone.NUMBER} LIKE ?" else ""
        val args = if (trimmed.isEmpty()) null else if (digits.length >= 2)
            arrayOf("%$trimmed%", "%$digits%") else arrayOf("%$trimmed%")
        val cols = arrayOf(Phone.DISPLAY_NAME, Phone.NUMBER, Phone.TYPE, Phone.LABEL, Phone._ID)
        val limit = 100
        var more = false
        var nextOffset = offset
        context.contentResolver.query(Phone.CONTENT_URI, cols, selection, args,
            "${Phone.DISPLAY_NAME} COLLATE LOCALIZED ASC, ${Phone._ID} ASC")?.use { cursor ->
            if (cursor.moveToPosition(offset)) {
                do {
                    if (contacts.length() == limit) { more = true; break }
                    nextOffset++
                    val number = cursor.getString(1) ?: continue
                    val label = Phone.getTypeLabel(context.resources, cursor.getInt(2), cursor.getString(3))
                    contacts.put(JSONObject().put("name", cursor.getString(0).orEmpty()).put("number", number)
                        .put("label", label?.toString().orEmpty()))
                } while (cursor.moveToNext())
            }
        }
        return out.put("contacts", contacts).put("next_offset", nextOffset).put("has_more", more)
    }

    @JvmStatic
    fun contactName(context: Context, address: String?): String {
        if (address.isNullOrEmpty() || context.checkSelfPermission(Manifest.permission.READ_CONTACTS) != PackageManager.PERMISSION_GRANTED)
            return ""
        val uri = Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode(address))
        return try {
            context.contentResolver.query(uri, arrayOf(ContactsContract.PhoneLookup.DISPLAY_NAME), null, null, null)?.use {
                if (it.moveToFirst()) it.getString(0).orEmpty() else ""
            }.orEmpty()
        } catch (_: Exception) { "" }
    }

    private fun reply(peerId: String, kind: String, body: JSONObject) = send(peerId, kind, body)

    @JvmStatic
    fun send(peerId: String, kind: String, body: JSONObject) {
        if (MainActivity.nativeReady) nativeSendTo(peerId, kind, body.toString())
    }

    @JvmStatic
    fun broadcastAll(kind: String, body: String) {
        if (MainActivity.nativeReady) nativeBroadcast(kind, body)
    }

    @JvmStatic private external fun nativeSendTo(peerId: String, kind: String, body: String)
    @JvmStatic private external fun nativeBroadcast(kind: String, body: String)
}
