package dev.airlink.airlink.helper;

/** The helper's log: stderr (the shell that started it) and logcat, tag AirLinkHelper. */
final class Log {
    private Log() {}

    static void i(String message) {
        System.err.println("airlink-helper: " + message);
        android.util.Log.i("AirLinkHelper", message);
    }

    static void e(String message, Throwable t) {
        System.err.println("airlink-helper: " + message + ": " + t);
        android.util.Log.e("AirLinkHelper", message, t);
    }
}
