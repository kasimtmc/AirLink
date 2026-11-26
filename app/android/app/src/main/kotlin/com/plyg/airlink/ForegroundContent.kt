package com.plyg.airlink

import android.accessibilityservice.AccessibilityService
import android.content.ComponentName
import android.content.ContentUris
import android.content.Context
import android.media.ExifInterface
import android.media.MediaMetadata
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.MediaStore
import android.view.accessibility.AccessibilityNodeInfo
import android.widget.Toast
import java.net.URLEncoder
import java.util.Calendar
import java.util.Locale

/**
 * Fling from the background: finds what the app on screen shows and sends it to the fling
 * target, in this order:
 *  1. files shared with AirLink and waiting (as when AirLink is open);
 *  2. a selection in a gallery or file manager: AirLink presses the app's own Send button and
 *     picks itself in the share sheet (ShareReceiverActivity sends what arrives);
 *  3. the one photo or video a gallery shows: matched in MediaStore by the time on screen
 *     (the shot's time, else its EXIF time), sent as the file without opening anything;
 *     if no file matches, the app's Send button as in 2;
 *  4. what plays: a local file as the file, a stream as "artist – title" and a search link;
 *  5. else a note that there was nothing to send.
 * It reads the screen through AirLink's accessibility service and only when the user flings.
 */
object ForegroundContent {
    private val handler = Handler(Looper.getMainLooper())

    /** Galleries and file managers whose Send/Share button AirLink may press for the user. */
    private val SHARE_APPS = setOf(
        "com.miui.gallery", "com.mi.android.globalFileexplorer", "com.android.fileexplorer",
        "com.google.android.apps.photos", "com.google.android.apps.nbu.files",
        "com.google.android.documentsui", "com.android.documentsui",
        "com.sec.android.gallery3d", "com.sec.android.app.myfiles",
        "com.coloros.gallery3d", "com.oplus.gallery", "com.android.gallery3d",
    )
    private val SHARE_LABELS = setOf("gönder", "paylaş", "share", "send")
    private const val SHARE_WAIT_MS = 6000L
    private const val AUTOMATED_SHARE_MS = 20000L

    @Volatile private var automatedShareAt = 0L

    /** A share that arrives now is the one AirLink started (once). */
    @JvmStatic
    fun takeAutomatedShare(): Boolean {
        val started = automatedShareAt
        automatedShareAt = 0L
        return started != 0L && SystemClock.elapsedRealtime() - started < AUTOMATED_SHARE_MS
    }

    /** The phone was flung while AirLink was not on screen. On the main thread. */
    @JvmStatic
    fun fling(context: Context) {
        val app = context.applicationContext
        val pending = nativeFlingPendingShare()
        if (pending.isNotEmpty()) return report(app, pending)

        val service = AirLinkAccessibility.service()
        val root = service?.rootInActiveWindow
        if (service != null && root != null) {
            val shareApp = root.packageName?.toString() in SHARE_APPS
            if (shareApp && selecting(root)) {
                if (pressShare(service, root)) return
            } else {
                val shown = shownMedia(root)
                if (shown != null) {
                    Thread({
                        val uri = MediaMatch.find(app, shown)
                        handler.post {
                            if (uri != null) sendUris(app, listOf(uri))
                            else {
                                val now = service.rootInActiveWindow
                                if (!(shareApp && now != null && pressShare(service, now))) mediaOrNothing(app)
                            }
                        }
                    }, "airlink-fling-match").start()
                    return
                }
                if (shareApp && pressShare(service, root)) return
            }
        }
        mediaOrNothing(app, service == null)
    }

    private fun mediaOrNothing(context: Context, noAccessibility: Boolean = false) {
        if (sendPlaying(context)) return
        report(context, if (noAccessibility) "Ekrandaki içeriği bulmak için Erişilebilirlik'te AirLink'i açın"
                        else "Gönderilecek içerik bulunamadı")
    }

    @JvmStatic
    fun sendFiles(context: Context, paths: Array<String>) = report(context, nativeFlingFiles(paths))

    @JvmStatic
    fun sendText(context: Context, text: String) = report(context, nativeFlingText(text))

    @JvmStatic
    fun report(context: Context, message: String) {
        if (message.isEmpty()) return
        handler.post { Toast.makeText(context.applicationContext, "AirLink: $message", Toast.LENGTH_SHORT).show() }
    }

    private fun sendUris(context: Context, uris: List<Uri>) {
        Thread({
            val paths = Staging.copy(context, uris).toTypedArray()
            handler.post { sendFiles(context, paths) }
        }, "airlink-fling-copy").start()
    }

    // ---- The screen ---------------------------------------------------------------------------

    private fun nodes(root: AccessibilityNodeInfo): Sequence<AccessibilityNodeInfo> = sequence {
        val queue = ArrayDeque<AccessibilityNodeInfo>()
        queue.add(root)
        while (queue.isNotEmpty()) {
            val node = queue.removeFirst()
            yield(node)
            for (i in 0 until node.childCount) node.getChild(i)?.let(queue::add)
        }
    }

    private val SELECTED_COUNT = Regex("""\d+\s+\S+\s+(seçildi|seçili|selected)""", RegexOption.IGNORE_CASE)

    /** A selection mode ("3 öge seçildi"), or ticked boxes. */
    private fun selecting(root: AccessibilityNodeInfo): Boolean =
        nodes(root).any { node ->
            (node.isCheckable && node.isChecked && node.isVisibleToUser) ||
                SELECTED_COUNT.containsMatchIn(node.text?.toString().orEmpty())
        }

    /** Presses the app's Send/Share button, then picks AirLink in the share sheet. */
    private fun pressShare(service: AccessibilityService, root: AccessibilityNodeInfo): Boolean {
        val button = nodes(root).firstOrNull { node ->
            node.isVisibleToUser && listOf(node.contentDescription, node.text).any {
                it?.toString()?.trim()?.lowercase(Locale.ROOT) in SHARE_LABELS
            }
        } ?: return false
        val clickable = clickableOf(button) ?: return false
        val app = root.packageName?.toString()
        automatedShareAt = SystemClock.elapsedRealtime()
        if (!clickable.performAction(AccessibilityNodeInfo.ACTION_CLICK)) {
            automatedShareAt = 0L
            return false
        }
        pickAirLink(service, app, SystemClock.elapsedRealtime(), 0)
        return true
    }

    private fun pickAirLink(service: AccessibilityService, app: String?, started: Long, scrolls: Int) {
        handler.postDelayed({
            if (automatedShareAt == 0L) return@postDelayed  // the share already arrived
            val root = service.rootInActiveWindow
            val inSheet = root != null && root.packageName?.toString() != app
            val target = if (inSheet) nodes(root!!).firstOrNull {
                it.isVisibleToUser && it.text?.toString()?.contains("AirLink", ignoreCase = true) == true
            } else null
            when {
                target != null -> clickableOf(target)?.performAction(AccessibilityNodeInfo.ACTION_CLICK)
                SystemClock.elapsedRealtime() - started > SHARE_WAIT_MS -> {
                    automatedShareAt = 0L
                    if (inSheet) service.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK)
                    report(service, "Paylaşım menüsünde AirLink bulunamadı; menüde AirLink'i sabitleyin")
                }
                inSheet && scrolls < 8 -> {
                    val list = nodes(root!!).firstOrNull { it.isScrollable && it.isVisibleToUser }
                    val scrolled = list?.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD) == true
                    pickAirLink(service, app, started, if (scrolled) scrolls + 1 else scrolls)
                }
                else -> pickAirLink(service, app, started, scrolls)
            }
        }, 250)
    }

    private fun clickableOf(node: AccessibilityNodeInfo): AccessibilityNodeInfo? {
        var current: AccessibilityNodeInfo? = node
        while (current != null && !current.isClickable) current = current.parent
        return current
    }

    /** "Fotoğraf, 27 Ekim, 2026 17:47:06, …" / "Photo, October 27, 2026 5:47:06 PM": a gallery's one item. */
    class Shown(val millis: Long, val exifTime: String)

    private val TR_TIME = Regex("""^(Fotoğraf|Resim|Video)\s*,\s*(\d{1,2})\s+(\p{L}+),?\s+(\d{4})\s+(\d{1,2}):(\d{2}):(\d{2})""")
    private val EN_TIME = Regex("""^(Photo|Picture|Video)\s*,\s*(\p{L}+)\s+(\d{1,2}),?\s+(\d{4}),?\s+(\d{1,2}):(\d{2}):(\d{2})\s*([AP]M)?""",
                                RegexOption.IGNORE_CASE)
    private val TR_MONTHS = listOf("ocak", "şubat", "mart", "nisan", "mayıs", "haziran", "temmuz", "ağustos", "eylül",
                                   "ekim", "kasım", "aralık")
    private val EN_MONTHS = listOf("january", "february", "march", "april", "may", "june", "july", "august", "september",
                                   "october", "november", "december")

    private fun shownMedia(root: AccessibilityNodeInfo): Shown? {
        val found = nodes(root).mapNotNull { node ->
            if (!node.isVisibleToUser) return@mapNotNull null
            val desc = node.contentDescription?.toString() ?: return@mapNotNull null
            TR_TIME.find(desc)?.let { m ->
                val (_, day, month, year, hour, minute, second) = m.destructured
                val index = TR_MONTHS.indexOf(month.lowercase(Locale("tr")))
                if (index < 0) null else shown(year.toInt(), index, day.toInt(), hour.toInt(), minute.toInt(), second.toInt())
            } ?: EN_TIME.find(desc)?.let { m ->
                val (_, month, day, year, hour, minute, second, ampm) = m.destructured
                val index = EN_MONTHS.indexOf(month.lowercase(Locale.ROOT))
                var h = hour.toInt()
                if (ampm.equals("PM", true) && h < 12) h += 12
                if (ampm.equals("AM", true) && h == 12) h = 0
                if (index < 0) null else shown(year.toInt(), index, day.toInt(), h, minute.toInt(), second.toInt())
            }
        }.distinctBy { it.millis }.take(2).toList()
        return found.singleOrNull()  // a grid shows many: only a page with one item counts
    }

    private fun shown(year: Int, month: Int, day: Int, hour: Int, minute: Int, second: Int): Shown {
        val calendar = Calendar.getInstance()
        calendar.clear()
        calendar.set(year, month, day, hour, minute, second)
        val exif = String.format(Locale.ROOT, "%04d:%02d:%02d %02d:%02d:%02d", year, month + 1, day, hour, minute, second)
        return Shown(calendar.timeInMillis, exif)
    }

    // ---- What plays ---------------------------------------------------------------------------

    private fun sendPlaying(context: Context): Boolean {
        if (!AirLinkNotificationListener.accessGranted()) return false
        val sessions = context.getSystemService(MediaSessionManager::class.java) ?: return false
        val controller = try {
            sessions.getActiveSessions(ComponentName(context, AirLinkNotificationListener::class.java))
                .firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
        } catch (_: SecurityException) { null } ?: return false
        val metadata = controller.metadata ?: return false
        val title = metadata.getString(MediaMetadata.METADATA_KEY_TITLE)
            ?: metadata.description?.title?.toString() ?: return false
        val artist = metadata.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: metadata.description?.subtitle?.toString().orEmpty()
        val mediaUri = metadata.getString(MediaMetadata.METADATA_KEY_MEDIA_URI)
        val duration = metadata.getLong(MediaMetadata.METADATA_KEY_DURATION)
        val player = controller.packageName
        Thread({
            val local = mediaUri?.takeIf { it.startsWith("content://") || it.startsWith("file://") }?.let(Uri::parse)
                ?: MediaMatch.audio(context, title, artist, duration)
            handler.post {
                if (local != null) sendUris(context, listOf(local))
                else sendText(context, listOfNotNull(
                    if (artist.isEmpty()) title else "$artist – $title",
                    searchLink(player, if (artist.isEmpty()) title else "$artist $title"),
                ).joinToString("\n"))
            }
        }, "airlink-fling-media").start()
        return true
    }

    private fun searchLink(player: String, query: String): String? {
        val q = URLEncoder.encode(query, "UTF-8")
        return when {
            "deezer" in player -> "https://www.deezer.com/search/$q"
            "spotify" in player -> "https://open.spotify.com/search/$q"
            "youtube.music" in player || "apps.youtube.music" in player -> "https://music.youtube.com/search?q=$q"
            "youtube" in player -> "https://www.youtube.com/results?search_query=$q"
            else -> null
        }
    }

    @JvmStatic private external fun nativeFlingPendingShare(): String
    @JvmStatic private external fun nativeFlingFiles(paths: Array<String>): String
    @JvmStatic private external fun nativeFlingText(text: String): String
}

/** MediaStore lookups for ForegroundContent (blocking, off the main thread). */
private object MediaMatch {
    private const val DAY_MS = 36L * 60 * 60 * 1000

    /** The photo or video taken at that second, else the photo whose EXIF time is that. */
    fun find(context: Context, shown: ForegroundContent.Shown): Uri? {
        val files = MediaStore.Files.getContentUri("external")
        val type = "${MediaStore.Files.FileColumns.MEDIA_TYPE} IN (${MediaStore.Files.FileColumns.MEDIA_TYPE_IMAGE}," +
            "${MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO})"
        val exact = query(context, files, "$type AND ${MediaStore.MediaColumns.DATE_TAKEN} BETWEEN ? AND ?",
                          arrayOf(shown.millis.toString(), (shown.millis + 999).toString()), 3)
        if (exact.size == 1) return exact[0]
        if (exact.size > 1) return null  // two shots in one second: the Send button decides
        // A camera with its own clock (a drone's): the gallery shows the EXIF time.
        val near = query(context, MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
                         "${MediaStore.MediaColumns.DATE_TAKEN} BETWEEN ? AND ? OR ${MediaStore.MediaColumns.DATE_MODIFIED} BETWEEN ? AND ?",
                         arrayOf((shown.millis - DAY_MS).toString(), (shown.millis + DAY_MS).toString(),
                                 ((shown.millis - DAY_MS) / 1000).toString(), ((shown.millis + DAY_MS) / 1000).toString()), 400)
        val matching = near.filter { uri ->
            try {
                context.contentResolver.openInputStream(uri)?.use { ExifInterface(it).getAttribute(ExifInterface.TAG_DATETIME_ORIGINAL) } == shown.exifTime
            } catch (_: Exception) { false }
        }
        return matching.singleOrNull()
    }

    /** A local song with this title (and artist), as long as what plays (±3 s). */
    fun audio(context: Context, title: String, artist: String, duration: Long): Uri? {
        val selection = StringBuilder("${MediaStore.Audio.Media.TITLE} = ?")
        val args = mutableListOf(title)
        if (artist.isNotEmpty()) { selection.append(" AND ${MediaStore.Audio.Media.ARTIST} = ?"); args.add(artist) }
        if (duration > 0) {
            selection.append(" AND ${MediaStore.Audio.Media.DURATION} BETWEEN ? AND ?")
            args.add((duration - 3000).toString()); args.add((duration + 3000).toString())
        }
        return query(context, MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, selection.toString(), args.toTypedArray(), 2).singleOrNull()
    }

    private fun query(context: Context, collection: Uri, selection: String, args: Array<String>, limit: Int): List<Uri> {
        val out = mutableListOf<Uri>()
        try {
            val files = collection == MediaStore.Files.getContentUri("external")
            val projection = if (files) arrayOf(MediaStore.MediaColumns._ID, MediaStore.Files.FileColumns.MEDIA_TYPE)
                             else arrayOf(MediaStore.MediaColumns._ID)
            context.contentResolver.query(collection, projection, selection, args,
                                          "${MediaStore.MediaColumns.DATE_MODIFIED} DESC")?.use { cursor ->
                while (cursor.moveToNext() && out.size < limit) {
                    val base = when {
                        !files -> collection
                        cursor.getInt(1) == MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO -> MediaStore.Video.Media.EXTERNAL_CONTENT_URI
                        else -> MediaStore.Images.Media.EXTERNAL_CONTENT_URI
                    }
                    out.add(ContentUris.withAppendedId(base, cursor.getLong(0)))
                }
            }
        } catch (_: Exception) { }
        return out
    }

}
