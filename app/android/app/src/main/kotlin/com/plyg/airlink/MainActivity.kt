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
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        shared(intent)
        if (intent.action == SHARE_SCREEN) ScreenShare.askConsent(this)
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

    fun stageUris(uris: Array<String>) {
        stage(uris.map(Uri::parse), false)
    }

    private fun stage(uris: List<Uri>, share: Boolean) {
        Thread({
            val paths = mutableListOf<String>()
            val staging = File(File(cacheDir, "airlink_send"), System.nanoTime().toString())
            for ((index, uri) in uris.withIndex()) {
                val destination = File(File(staging, index.toString()), displayName(uri))
                destination.parentFile?.mkdirs()
                try {
                    contentResolver.openInputStream(uri)?.use { input ->
                        FileOutputStream(destination).use { output -> input.copyTo(output, 256 * 1024) }
                        paths.add(destination.absolutePath)
                    }
                } catch (_: Exception) { destination.delete() }
            }
            runOnUiThread {
                val result = paths.toTypedArray()
                if (share) nativeShared(result) else nativeFilesPicked(result)
            }
        }, "airlink-pick").start()
    }

    private fun displayName(uri: Uri): String {
        try {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) cursor.getString(0)?.takeIf { it.isNotEmpty() }?.let { return it.replace('/', '_') }
            }
        } catch (_: Exception) { }
        return (uri.lastPathSegment ?: "file").replace('/', '_')
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

        @JvmField @Volatile var visible = false
        @JvmField @Volatile var nativeReady = false
        @Volatile private var currentActivity: MainActivity? = null

        @JvmStatic fun current(): MainActivity? = currentActivity
        @JvmStatic fun screenControlEnabled(): Boolean = AirLinkAccessibility.enabled()
        @JvmStatic fun openScreenControlSettings() = AirLinkAccessibility.openSettings()
        @JvmStatic fun smsGranted(): Boolean = AirLinkService.appContext?.let(SmsBridge::granted) == true
        @JvmStatic fun callsGranted(): Boolean = AirLinkService.appContext?.let(CallBridge::granted) == true

        @JvmStatic fun requestSmsPermissions() {
            currentActivity?.requestPermissions(arrayOf(Manifest.permission.READ_SMS, Manifest.permission.SEND_SMS,
                Manifest.permission.RECEIVE_SMS, Manifest.permission.READ_CONTACTS), 2)
        }

        @JvmStatic fun requestCallPermissions() {
            currentActivity?.requestPermissions(CallBridge.permissions(), 3)
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

        @JvmStatic private external fun nativeInit(activity: MainActivity, filesDir: String, downloadDir: String, deviceName: String)
        @JvmStatic private external fun nativeFilesPicked(paths: Array<String>)
        @JvmStatic private external fun nativeShared(paths: Array<String>)
        @JvmStatic private external fun nativePump()
        @JvmStatic private external fun nativeAdbPair(code: String)
        @JvmStatic private external fun nativeStartHelper(command: String)
        @JvmStatic private external fun nativeRespond(requestId: Long, accept: Boolean)
    }
}
