package com.plyg.airlink

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.FileOutputStream

/** Copies shared content (content:// or file://) into the cache, as files the core can send. */
object Staging {
    /** Blocking: call off the main thread. Returns the copies that could be read. */
    @JvmStatic
    fun copy(context: Context, uris: List<Uri>): List<String> {
        val paths = mutableListOf<String>()
        val staging = File(File(context.cacheDir, "airlink_send"), System.nanoTime().toString())
        for ((index, uri) in uris.withIndex()) {
            val destination = File(File(staging, index.toString()), displayName(context, uri))
            destination.parentFile?.mkdirs()
            try {
                context.contentResolver.openInputStream(uri)?.use { input ->
                    FileOutputStream(destination).use { output -> input.copyTo(output, 256 * 1024) }
                    paths.add(destination.absolutePath)
                }
            } catch (_: Exception) { destination.delete() }
        }
        return paths
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
