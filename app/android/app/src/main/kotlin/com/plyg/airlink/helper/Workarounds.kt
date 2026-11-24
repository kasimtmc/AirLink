package com.plyg.airlink.helper

import android.annotation.SuppressLint
import android.content.AttributionSource
import android.content.Context
import android.content.ContextWrapper
import android.os.Build
import android.os.Looper

/** Supplies a shell identity and a Context to framework APIs used by app_process. */
@SuppressLint("PrivateApi", "DiscouragedPrivateApi")
object Workarounds {
    private const val SHELL_PACKAGE = "com.android.shell"
    private const val SHELL_UID = 2000

    private lateinit var fake: Context

    /** Called once before creating a virtual display, on the helper's main thread. */
    @JvmStatic
    fun apply() {
        if (Looper.getMainLooper() == null) Looper.prepareMainLooper()
        val activityThreadClass = Class.forName("android.app.ActivityThread")
        val constructor = activityThreadClass.getDeclaredConstructor().apply { isAccessible = true }
        val activityThread = constructor.newInstance()
        activityThreadClass.getDeclaredField("sCurrentActivityThread").apply {
            isAccessible = true
            set(null, activityThread)
        }
        activityThreadClass.getDeclaredField("mSystemThread").apply {
            isAccessible = true
            setBoolean(activityThread, true)
        }
        val systemContext = activityThreadClass.getDeclaredMethod("getSystemContext").invoke(activityThread) as Context
        fake = FakeContext(systemContext)
    }

    @JvmStatic
    fun context(): Context = fake

    private class FakeContext(base: Context) : ContextWrapper(base) {
        override fun getPackageName(): String = SHELL_PACKAGE

        override fun getOpPackageName(): String = SHELL_PACKAGE

        override fun getAttributionSource(): AttributionSource {
            if (Build.VERSION.SDK_INT < 31) return super.getAttributionSource()
            return AttributionSource.Builder(SHELL_UID).setPackageName(SHELL_PACKAGE).build()
        }

        override fun getDeviceId(): Int = 0

        override fun getApplicationContext(): Context = this
    }
}
