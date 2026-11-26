package com.plyg.airlink

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.OpenableColumns
import android.provider.Settings
import dev.kompoz.KompozActivity
import java.io.File
import java.io.FileOutputStream

/** Android lifecycle, file picking and share intents around the native Kompoz UI. */
class MainActivity : KompozActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        AirLinkService.appContext = applicationContext
        super.onCreate(savedInstanceState)
        currentActivity = this
        cleanStaged(System.nanoTime())
        nativeInit(this, filesDir.absolutePath, downloadDir(), deviceName())
        nativeReady = true
        AirLinkService.start(this)
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        shared(intent)
        if (intent?.action == SHARE_SCREEN) ScreenShare.askConsent(this)
        if (intent?.action == SHARE_AUDIO) ScreenShare.askAudioPermission(this)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        shared(intent)
        if (intent.action == SHARE_SCREEN) ScreenShare.askConsent(this)
        if (intent.action == SHARE_AUDIO) ScreenShare.askAudioPermission(this)
    }

    override fun onResume() {
        super.onResume()
        visible = true
    }

    override fun onPause() {
        visible = false
        super.onPause()
    }

    override fun onDestroy() {
        if (currentActivity === this) currentActivity = null
        nativeDetachActivity(this)
        super.onDestroy()
    }

    private fun shared(intent: Intent?) {
        if (intent == null) return
        val uris = when (intent.action) {
            Intent.ACTION_SEND -> listOfNotNull(intent.getParcelableExtra<Uri>(Intent.EXTRA_STREAM))
            Intent.ACTION_SEND_MULTIPLE -> intent.getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM).orEmpty()
            else -> emptyList()
        }
        if (uris.isEmpty()) return
        setIntent(Intent(this, MainActivity::class.java))
        stage(uris, true)
    }

    private fun downloadDir(): String {
        val public = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "AirLink")
        if (writable(public)) return public.absolutePath
        return File(getExternalFilesDir(null) ?: filesDir, "AirLink").absolutePath
    }

    private fun deviceName(): String {
        val chosen = Settings.Global.getString(contentResolver, Settings.Global.DEVICE_NAME)
        if (!chosen.isNullOrBlank()) return chosen
        val model = Build.MODEL ?: "Android"
        val maker = Build.MANUFACTURER.orEmpty()
        if (maker.isEmpty() || model.startsWith(maker, ignoreCase = true)) return model
        return maker.replaceFirstChar { it.uppercaseChar() } + maker.drop(1) + " " + model
    }

    /** Called by the native UI to open the system's multi-file picker. */
    fun pickFiles() {
        val open = Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
            .setType("*/*").putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        try { startActivityForResult(open, PICK_FILES) }
        catch (_: Exception) { nativeFilesPicked(emptyArray()) }
    }

    fun scanFiles(paths: Array<String>) {
        MediaScannerConnection.scanFile(this, paths, null, null)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == ScreenShare.REQUEST) {
            ScreenShare.consentResult(this, resultCode, data)
            return
        }
        if (requestCode != PICK_FILES) return
        val uris = mutableListOf<Uri>()
        if (resultCode == RESULT_OK && data != null) {
            val clip = data.clipData
            if (clip != null) {
                for (index in 0 until clip.itemCount) uris.add(clip.getItemAt(index).uri)
            } else data.data?.let(uris::add)
        }
        stage(uris, false)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == ScreenShare.AUDIO_PERMISSION) ScreenShare.audioPermissionResult(this)
    }

    fun stageUris(uris: Array<String>) {
        stage(uris.map(Uri::parse), false)
    }

    private fun stage(uris: List<Uri>, share: Boolean) {
        Thread({
            val result = Staging.copy(this, uris).toTypedArray()
            runOnUiThread { if (share) nativeShared(result) else nativeFilesPicked(result) }
        }, "airlink-pick").start()
    }

    private fun cleanStaged(before: Long) {
        val root = File(cacheDir, "airlink_send")
        Thread({
            root.listFiles()?.forEach { batch ->
                if (batch.name.toLongOrNull()?.let { it < before } != false) deleteTree(batch)
            }
        }, "airlink-clean").start()
    }

    companion object {
        private const val PICK_FILES = 7001
        private const val SHARE_SCREEN = "com.plyg.airlink.SHARE_SCREEN"
        private const val SHARE_AUDIO = "com.plyg.airlink.SHARE_AUDIO"

        @JvmField @Volatile var visible = false
        @JvmField @Volatile var nativeReady = false
        @Volatile private var currentActivity: MainActivity? = null

        @JvmStatic fun current(): MainActivity? = currentActivity
        /** A restarted foreground service must bring the core back without opening a window. */
        @JvmStatic fun ensureBackgroundNative(context: android.content.Context) {
            if (nativeReady) return
            System.loadLibrary("main")
            val public = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "AirLink")
            val download = if (writable(public)) public else File(context.getExternalFilesDir(null) ?: context.filesDir, "AirLink")
            val chosen = Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME)
            val name = chosen?.takeIf { it.isNotBlank() } ?: (Build.MODEL ?: "Android")
            nativeInit(null, context.filesDir.absolutePath, download.absolutePath, name)
            nativeReady = true
            pump()
        }
        @JvmStatic fun screenControlEnabled(): Boolean = AirLinkAccessibility.enabled()
        @JvmStatic fun openScreenControlSettings() = AirLinkAccessibility.openSettings()
        @JvmStatic fun needsBackgroundAutostartSettings(): Boolean =
            Build.MANUFACTURER.equals("Xiaomi", ignoreCase = true)

        @JvmStatic fun openBackgroundAutostartSettings() {
            val activity = currentActivity ?: return
            try {
                activity.startActivity(Intent().setClassName(
                    "com.miui.securitycenter", "com.miui.permcenter.autostart.AutoStartManagementActivity",
                ))
            } catch (_: Exception) {
                activity.startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                    Uri.parse("package:${activity.packageName}")))
            }
        }
        @JvmStatic fun smsGranted(): Boolean = AirLinkService.appContext?.let(SmsBridge::granted) == true
        @JvmStatic fun callsGranted(): Boolean = AirLinkService.appContext?.let(CallBridge::granted) == true

        @JvmStatic fun requestSmsPermissions() {
            currentActivity?.requestPermissions(arrayOf(Manifest.permission.READ_SMS, Manifest.permission.SEND_SMS,
                Manifest.permission.RECEIVE_SMS, Manifest.permission.READ_CONTACTS), 2)
        }

        @JvmStatic fun requestCallPermissions() {
            currentActivity?.requestPermissions(CallBridge.permissions(), 3)
        }

        @JvmStatic fun flingAvailable(): Boolean = AirLinkService.appContext?.let(FlingDetector::available) == true
        @JvmStatic fun flingEnabled(): Boolean = AirLinkService.appContext?.let(FlingDetector::enabled) == true
        @JvmStatic fun setFlingEnabled(on: Boolean) {
            val context = currentActivity ?: AirLinkService.appContext ?: return
            FlingDetector.setEnabled(context, on)
        }
        @JvmStatic fun playDesktopAudio(pcm: ByteArray) = DesktopAudioPlayer.offer(pcm)
        @JvmStatic fun stopDesktopAudio() = DesktopAudioPlayer.stop()

        @JvmStatic fun stopPhoneScreenForPeer(peer: String) {
            android.os.Handler(android.os.Looper.getMainLooper()).post { ScreenShare.stopVideo(peer) }
        }

        @JvmStatic fun setPauseScreenAudioDuringMusic(enabled: Boolean) {
            val context = AirLinkService.appContext ?: currentActivity ?: return
            android.os.Handler(android.os.Looper.getMainLooper()).post {
                ScreenShare.setPauseForSelectedPlayers(context, enabled)
            }
        }

        @JvmStatic fun setScreenShareAudio(enabled: Boolean) {
            val context = AirLinkService.appContext ?: currentActivity ?: return
            android.os.Handler(android.os.Looper.getMainLooper()).post {
                ScreenShare.setScreenAudio(context, enabled)
            }
        }
        private val FLING_MEDIA = if (Build.VERSION.SDK_INT >= 33)
            arrayOf(Manifest.permission.READ_MEDIA_IMAGES, Manifest.permission.READ_MEDIA_VIDEO, Manifest.permission.READ_MEDIA_AUDIO)
            else arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE)
        /** Fling from a gallery finds the photo on screen in MediaStore. */
        @JvmStatic fun flingMediaGranted(): Boolean {
            val context = AirLinkService.appContext ?: currentActivity ?: return false
            return FLING_MEDIA.all { context.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }
        }
        @JvmStatic fun requestFlingMedia() {
            currentActivity?.let { activity -> activity.runOnUiThread { activity.requestPermissions(FLING_MEDIA, 3) } }
        }
        /** A file of the Files tab as MediaStore has it (AirLink's received files are scanned there). */
        private fun contentUri(path: String): android.net.Uri? {
            val context = AirLinkService.appContext ?: currentActivity ?: return null
            val files = android.provider.MediaStore.Files.getContentUri("external")
            return try {
                @Suppress("DEPRECATION")
                context.contentResolver.query(files, arrayOf(android.provider.MediaStore.MediaColumns._ID),
                    "${android.provider.MediaStore.MediaColumns.DATA} = ?", arrayOf(path), null)?.use { c ->
                    if (c.moveToFirst()) android.content.ContentUris.withAppendedId(files, c.getLong(0)) else null
                }
            } catch (_: Exception) { null }
        }

        private fun mimeOf(path: String): String =
            android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(path.substringAfterLast('.', "").lowercase()) ?: "*/*"

        @JvmStatic fun openFile(path: String) {
            val activity = currentActivity ?: return
            val uri = contentUri(path) ?: return activity.runOnUiThread {
                android.widget.Toast.makeText(activity, "Dosya bulunamadı", android.widget.Toast.LENGTH_SHORT).show()
            }
            activity.runOnUiThread {
                try {
                    activity.startActivity(Intent(Intent.ACTION_VIEW).setDataAndType(uri, mimeOf(path))
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
                } catch (_: Exception) {
                    android.widget.Toast.makeText(activity, "Bu dosyayı açacak uygulama yok", android.widget.Toast.LENGTH_SHORT).show()
                }
            }
        }

        /** The folder AirLink saves to, in the system's file browser (Downloads). */
        @JvmStatic fun showInFolder(path: String) {
            val activity = currentActivity ?: return
            activity.runOnUiThread {
                try { activity.startActivity(Intent(android.app.DownloadManager.ACTION_VIEW_DOWNLOADS)) } catch (_: Exception) { }
            }
        }

        @JvmStatic fun shareWithApps(path: String): Boolean {
            val activity = currentActivity ?: return false
            val uri = contentUri(path) ?: return false
            activity.runOnUiThread {
                val send = Intent(Intent.ACTION_SEND).setType(mimeOf(path)).putExtra(Intent.EXTRA_STREAM, uri)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                activity.startActivity(Intent.createChooser(send, null))
            }
            return true
        }

        @JvmStatic fun chooseMediaAudioPlayers() {
            currentActivity?.let { activity -> activity.runOnUiThread { PlayerBluetoothAudio.choosePlayers(activity) } }
        }

        @JvmStatic fun setPlayerAudioTarget(peer: String?) {
            val context = AirLinkService.appContext ?: currentActivity ?: return
            android.os.Handler(android.os.Looper.getMainLooper()).post {
                PlayerBluetoothAudio.setTarget(context, peer)
            }
        }

        @JvmStatic fun respond(requestId: Long, accept: Boolean) = nativeRespond(requestId, accept)
        @JvmStatic fun pump() = nativePump()
        @JvmStatic fun adbPair(code: String) = nativeAdbPair(code)
        @JvmStatic fun startHelper(command: String) = nativeStartHelper(command)

        private fun writable(directory: File): Boolean = try {
            if (!directory.isDirectory && !directory.mkdirs()) false
            else File(directory, ".airlink-probe").let { probe -> FileOutputStream(probe).close(); probe.delete() }
        } catch (_: Exception) { false }

        private fun deleteTree(file: File) {
            file.listFiles()?.forEach(::deleteTree)
            file.delete()
        }

        @JvmStatic private external fun nativeInit(activity: MainActivity?, filesDir: String, downloadDir: String, deviceName: String)
        @JvmStatic private external fun nativeDetachActivity(activity: MainActivity)
        @JvmStatic private external fun nativeFilesPicked(paths: Array<String>)
        /** Files shared with AirLink (from ShareReceiverActivity): the share dialog offers them. */
        @JvmStatic fun shareStaged(paths: Array<String>) = nativeShared(paths)
        @JvmStatic private external fun nativeShared(paths: Array<String>)
        @JvmStatic private external fun nativePump()
        @JvmStatic private external fun nativeAdbPair(code: String)
        @JvmStatic private external fun nativeStartHelper(command: String)
        @JvmStatic private external fun nativeRespond(requestId: Long, accept: Boolean)
    }
}
