package dev.airlink.airlink.helper;

import android.os.Looper;
import android.view.MotionEvent;

import java.io.FileOutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/**
 * AirLink's helper: runs as the shell user, started over the phone's own wireless debugging with
 *   CLASSPATH=<AirLink's APK> app_process / dev.airlink.airlink.helper.Helper <mode> ...
 * so it may create trusted virtual displays (an app each), start apps on them and inject input.
 *
 * Modes: "test <package/activity>" (a display, the app on it, two frames saved to
 * /data/local/tmp, a tap between them), "serve <port> <token>" (for the AirLink app).
 */
public final class Helper {
    private Helper() {}

    public static void main(String[] args) {
        try {
            Workarounds.apply();
            if (args.length >= 2 && args[0].equals("test")) test(args[1]);
            else if (args.length >= 3 && args[0].equals("serve")) Server.run(Integer.parseInt(args[1]), args[2]);
            else Log.i("usage: test <package/activity> | serve <port> <token>");
        } catch (Throwable t) {
            Log.e("helper failed", t);
            System.exit(1);
        }
        System.exit(0);
    }

    /** Starts `component` on a display of its own (`am start --display`). */
    static boolean launch(int displayId, String component) throws Exception {
        String[] cmd = {"am", "start", "--display", String.valueOf(displayId), "--activity-multiple-task",
                "-a", "android.intent.action.MAIN", "-c", "android.intent.category.LAUNCHER", "-n", component};
        Process p = Runtime.getRuntime().exec(cmd);
        String out = new String(p.getInputStream().readAllBytes()) + new String(p.getErrorStream().readAllBytes());
        int code = p.waitFor();
        Log.i("am start --display " + displayId + " " + component + " -> " + code + " " + out.trim());
        return code == 0 && !out.contains("Error");
    }

    private static void test(String component) throws Exception {
        AtomicReference<byte[]> latest = new AtomicReference<>();
        CountDownLatch first = new CountDownLatch(1);
        AppDisplay display = new AppDisplay("test", 720, 1280, 320, (d, jpeg) -> {
            latest.set(jpeg);
            first.countDown();
        });
        Log.i("virtual display " + display.id() + " created");
        launch(display.id(), component);
        Thread.sleep(3000);
        save(latest.get(), "/data/local/tmp/airlink-test-1.jpg");
        Log.i("tap at the middle: " + Input.touch(display.id(), MotionEvent.ACTION_DOWN, 360, 640)
                + " " + Input.touch(display.id(), MotionEvent.ACTION_UP, 360, 640));
        Thread.sleep(1500);
        save(latest.get(), "/data/local/tmp/airlink-test-2.jpg");
        display.release();
        Log.i("done");
    }

    private static void save(byte[] jpeg, String path) throws Exception {
        if (jpeg == null) {
            Log.i("no frame for " + path);
            return;
        }
        try (FileOutputStream f = new FileOutputStream(path)) {
            f.write(jpeg);
        }
        Log.i("saved " + path + " (" + jpeg.length + " bytes)");
    }

    static boolean onMainLooper() {
        return Looper.myLooper() == Looper.getMainLooper();
    }
}
