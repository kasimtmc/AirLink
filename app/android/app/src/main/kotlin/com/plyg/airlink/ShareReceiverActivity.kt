package com.plyg.airlink

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.WindowManager

/**
 * "Share > AirLink ile gönder". An ordinary share opens AirLink's share dialog (MainActivity).
 * A share AirLink started itself, by pressing an app's Send button after a fling
 * (ForegroundContent), goes straight to the fling target: this activity is invisible and lets
 * touches through, and stays only while it copies the files (its read grant lasts that long).
 */
class ShareReceiverActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)
        val uris = sharedUris(intent)
        if (!ForegroundContent.takeAutomatedShare() || !MainActivity.nativeReady) {
            startActivity(Intent(intent).setClass(this, MainActivity::class.java)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
            finish()
            return
        }
        if (uris.isEmpty()) {
            val text = intent.getStringExtra(Intent.EXTRA_TEXT).orEmpty()
            if (text.isNotEmpty()) ForegroundContent.sendText(this, text)
            else ForegroundContent.report(this, Texts.tr("Paylaşılan içerik okunamadı"))
            finish()
            return
        }
        Thread({
            val paths = Staging.copy(this, uris).toTypedArray()
            runOnUiThread {
                ForegroundContent.sendFiles(this, paths)
                finish()
            }
        }, "airlink-fling-share").start()
    }

    override fun finish() {
        super.finish()
        @Suppress("DEPRECATION") overridePendingTransition(0, 0)
    }

    @Suppress("DEPRECATION")
    private fun sharedUris(intent: Intent): List<Uri> = when (intent.action) {
        Intent.ACTION_SEND -> listOfNotNull(intent.getParcelableExtra<Uri>(Intent.EXTRA_STREAM))
        Intent.ACTION_SEND_MULTIPLE -> intent.getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM).orEmpty()
        else -> emptyList()
    }
}
