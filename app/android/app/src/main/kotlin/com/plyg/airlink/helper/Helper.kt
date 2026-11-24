package com.plyg.airlink.helper

import android.os.Looper
import android.view.MotionEvent
import java.io.FileOutputStream
import java.util.concurrent.atomic.AtomicReference

/** Shell user entry point, launched over wireless debugging through app_process. */
object Helper {
    @JvmStatic
    fun main(args: Array<String>) {
        try {
            Workarounds.apply()
            when {
                args.size >= 2 && args[0] == "test" -> test(args[1], "jpeg")
                args.size >= 2 && args[0] == "test-h264" -> test(args[1], "h264")
                args.size >= 3 && args[0] == "serve" -> Server.run(args[1].toInt(), args[2])
                else -> Log.i("usage: test|test-h264 <package/activity> | serve <port> <token>")
            }
        } catch (cause: Throwable) {
            Log.e("helper failed", cause)
            System.exit(1)
        }
        System.exit(0)
    }

    /** Starts the component on a virtual display through ActivityManager's shell command. */
    @JvmStatic
    fun launch(displayId: Int, component: String): Boolean {
        val process = Runtime.getRuntime().exec(
            arrayOf(
                "am", "start", "--display", displayId.toString(), "--activity-multiple-task",
                "-a", "android.intent.action.MAIN", "-c", "android.intent.category.LAUNCHER", "-n", component,
            ),
        )
        val output = process.inputStream.readBytes().toString(Charsets.UTF_8) +
            process.errorStream.readBytes().toString(Charsets.UTF_8)
        val code = process.waitFor()
        Log.i("am start --display $displayId $component -> $code ${output.trim()}")
        return code == 0 && !output.contains("Error")
    }

    private fun test(component: String, codec: String) {
        val latest = AtomicReference<ByteArray?>()
        val display = AppDisplay("test", 720, 1280, 320, codec) { _, frame -> latest.set(frame) }
        try {
            Log.i("virtual display ${display.id()} created")
            launch(display.id(), component)
            Thread.sleep(3000)
            save(latest.get(), "/data/local/tmp/airlink-test-1.${if (codec == "h264") "h264" else "jpg"}")
            Log.i(
                "tap at the middle: ${Input.touch(display.id(), MotionEvent.ACTION_DOWN, 360f, 640f)} " +
                    Input.touch(display.id(), MotionEvent.ACTION_UP, 360f, 640f),
            )
            Thread.sleep(1500)
            save(latest.get(), "/data/local/tmp/airlink-test-2.${if (codec == "h264") "h264" else "jpg"}")
        } finally {
            display.release()
        }
        Log.i("done")
    }

    private fun save(jpeg: ByteArray?, path: String) {
        if (jpeg == null) {
            Log.i("no frame for $path")
            return
        }
        val frame = if (path.endsWith(".h264")) {
            require(jpeg.size > 4 && jpeg.copyOfRange(0, 4).contentEquals("ALH1".toByteArray(Charsets.US_ASCII))) {
                "H.264 requested but a non-H.264 frame was produced"
            }
            jpeg.copyOfRange(4, jpeg.size)
        } else jpeg
        FileOutputStream(path).use { it.write(frame) }
        Log.i("saved $path (${jpeg.size} bytes)")
    }

    @JvmStatic
    fun onMainLooper(): Boolean = Looper.myLooper() == Looper.getMainLooper()
}
