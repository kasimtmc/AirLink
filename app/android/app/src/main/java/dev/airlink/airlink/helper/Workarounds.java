package dev.airlink.airlink.helper;

import android.annotation.SuppressLint;
import android.content.AttributionSource;
import android.content.Context;
import android.content.ContextWrapper;
import android.os.Build;
import android.os.Looper;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * The helper runs with app_process as the shell user, not as an app: there is no Application,
 * ActivityThread or Context. Some framework APIs need them, so it makes them up, as scrcpy does
 * (github.com/Genymobile/scrcpy, Apache License 2.0, which this follows).
 */
@SuppressLint({"PrivateApi", "DiscouragedPrivateApi"})
final class Workarounds {
    static final String SHELL_PACKAGE = "com.android.shell";
    static final int SHELL_UID = 2000;

    private static Context systemContext;
    private static Context fake;

    private Workarounds() {}

    /** Call once, first, on the main thread. */
    static void apply() throws Exception {
        if (Looper.getMainLooper() == null) Looper.prepareMainLooper();
        Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
        Constructor<?> ctor = activityThreadClass.getDeclaredConstructor();
        ctor.setAccessible(true);
        Object activityThread = ctor.newInstance();
        Field current = activityThreadClass.getDeclaredField("sCurrentActivityThread");
        current.setAccessible(true);
        current.set(null, activityThread);
        Field systemThread = activityThreadClass.getDeclaredField("mSystemThread");
        systemThread.setAccessible(true);
        systemThread.setBoolean(activityThread, true);
        Method getSystemContext = activityThreadClass.getDeclaredMethod("getSystemContext");
        systemContext = (Context) getSystemContext.invoke(activityThread);
        fake = new FakeContext(systemContext);
    }

    /** A context that says it is the shell (the user this process runs as). */
    static Context context() {
        return fake;
    }

    private static final class FakeContext extends ContextWrapper {
        FakeContext(Context base) {
            super(base);
        }

        @Override
        public String getPackageName() {
            return SHELL_PACKAGE;
        }

        @Override
        public String getOpPackageName() {
            return SHELL_PACKAGE;
        }

        @Override
        public AttributionSource getAttributionSource() {
            if (Build.VERSION.SDK_INT < 31) return super.getAttributionSource();
            return new AttributionSource.Builder(SHELL_UID).setPackageName(SHELL_PACKAGE).build();
        }

        @Override
        public int getDeviceId() {
            return 0;  // the default device
        }

        @Override
        public Context getApplicationContext() {
            return this;
        }
    }
}
