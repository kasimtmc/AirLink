package com.plyg.airlink

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.ConcurrentHashMap

/**
 * Copies shared content (content:// or file://) into the cache, as files the core can send, and
 * remembers where each copy came from: once sent, the Files tab records the original (its URI and
 * name) and the copy goes.
 */
object Staging {
    private class Origin(val uri: String, val name: String)
    private val origins = ConcurrentHashMap<String, Origin>()
    private const val DIR = "airlink_send"

    /** Blocking: call off the main thread. Returns the copies that could be read. */
    @JvmStatic
    fun copy(context: Context, uris: List<Uri>): List<String> {
        val paths = mutableListOf<String>()
        val staging = File(File(context.cacheDir, DIR), System.nanoTime().toString())
        for ((index, uri) in uris.withIndex()) {
            val name = displayName(context, uri)
            val destination = File(File(staging, index.toString()), name)
            destination.parentFile?.mkdirs()
            try {
                context.contentResolver.openInputStream(uri)?.use { input ->
                    FileOutputStream(destination).use { output -> input.copyTo(output, 256 * 1024) }
                    paths.add(destination.absolutePath)
                    origins[destination.absolutePath] = Origin(uri.toString(), name)
                }
            } catch (_: Exception) { destination.delete() }
        }
        return paths
    }

    /** "uri\nname" of a copy's original, or "" if it isn't one. */
    @JvmStatic
    fun originOf(path: String): String = origins[path]?.let { "${it.uri}\n${it.name}" }.orEmpty()

    /** A copy that was sent: gone, with its folders. */
    @JvmStatic
    fun release(path: String) {
        origins.remove(path)
        val file = File(path)
        val root = AirLinkService.appContext?.let { File(it.cacheDir, DIR) } ?: return
        if (!file.absolutePath.startsWith(root.absolutePath + "/")) return
        file.delete()
        var dir = file.parentFile
        while (dir != null && dir != root && (dir.list()?.isEmpty() == true)) {
            dir.delete()
            dir = dir.parentFile
        }
    }

    /** Copies left behind (a send that failed, or the app stopped first), at the service's start:
     *  those older than ten minutes (no send is still reading them). */
    @JvmStatic
    fun cleanOld(context: Context) {
        val root = File(context.cacheDir, DIR)
        val cutoff = System.currentTimeMillis() - 10L * 60 * 1000
        root.listFiles()?.forEach { if (it.lastModified() < cutoff) it.deleteRecursively() }
    }

    private fun displayName(context: Context, uri: Uri): String {
        try {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) cursor.getString(0)?.takeIf { it.isNotEmpty() }?.let { return it.replace('/', '_') }
            }
        } catch (_: Exception) { }
        return (uri.lastPathSegment ?: "file").replace('/', '_')
    }
}
