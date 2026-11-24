package com.plyg.airlink.helper

/** The helper's log goes to stderr and logcat, with the AirLinkHelper tag. */
object Log {
    @JvmStatic
    fun i(message: String) {
        System.err.println("airlink-helper: $message")
        android.util.Log.i("AirLinkHelper", message)
    }

    @JvmStatic
    fun e(message: String, cause: Throwable) {
        System.err.println("airlink-helper: $message: $cause")
        android.util.Log.e("AirLinkHelper", message, cause)
    }
}
