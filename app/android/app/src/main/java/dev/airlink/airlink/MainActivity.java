package dev.airlink.airlink;

import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.provider.Settings;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

import dev.kompoz.KompozActivity;

// All UI is drawn by Kompoz in native code (libmain.so). This activity adds what AirLink needs from
// Android: the multicast lock for mDNS, where files go, the device's name, and the file picker.
public class MainActivity extends KompozActivity {
    private static final int PICK_FILES = 7001;

    // Android drops incoming multicast (mDNS) unless an app holds this lock.
    private WifiManager.MulticastLock multicastLock;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            multicastLock = wifi.createMulticastLock("airlink-mdns");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        }
        super.onCreate(savedInstanceState);  // loads libmain.so
        // Copies staged for sending by an earlier run.
        new Thread(() -> deleteTree(new File(getCacheDir(), "airlink_send")), "airlink-clean").start();
        nativeInit(this, getFilesDir().getAbsolutePath(), downloadDir(), deviceName());
    }

    @Override
    protected void onDestroy() {
        if (multicastLock != null) multicastLock.release();
        multicastLock = null;
        super.onDestroy();
    }

    // Download/AirLink, so files are easy to find (apps may write there directly on Android 11+,
    // and on 9- with the legacy permission); the app's own external folder otherwise.
    private String downloadDir() {
        File pub = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "AirLink");
        if (writable(pub)) return pub.getAbsolutePath();
        File own = getExternalFilesDir(null);
        return new File(own != null ? own : getFilesDir(), "AirLink").getAbsolutePath();
    }

    private static boolean writable(File dir) {
        try {
            if (!dir.isDirectory() && !dir.mkdirs()) return false;
            File probe = new File(dir, ".airlink-probe");
            new FileOutputStream(probe).close();
            return probe.delete();
        } catch (Exception e) {
            return false;
        }
    }

    private String deviceName() {
        String userSet = Settings.Global.getString(getContentResolver(), Settings.Global.DEVICE_NAME);
        if (userSet != null && !userSet.trim().isEmpty()) return userSet;
        String model = Build.MODEL != null ? Build.MODEL : "Android";
        String maker = Build.MANUFACTURER != null ? Build.MANUFACTURER : "";
        if (maker.isEmpty() || model.toLowerCase().startsWith(maker.toLowerCase())) return model;
        return Character.toUpperCase(maker.charAt(0)) + maker.substring(1) + " " + model;
    }

    // ---- Called from native code, on the UI thread ----------------------------------------

    /** Opens the system file picker; the result goes to nativeFilesPicked. */
    public void pickFiles() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        try {
            startActivityForResult(intent, PICK_FILES);
        } catch (Exception e) {
            nativeFilesPicked(new String[0]);
        }
    }

    /** Makes received files visible to the gallery and file apps. */
    public void scanFiles(String[] paths) {
        MediaScannerConnection.scanFile(this, paths, null, null);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_FILES) return;
        List<Uri> uris = new ArrayList<>();
        if (resultCode == RESULT_OK && data != null) {
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); i++) uris.add(clip.getItemAt(i).getUri());
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
        }
        stage(uris);
    }

    /** Files dropped from another app (content:// URIs): copied, then given to nativeFilesPicked. */
    public void stageUris(String[] uris) {
        List<Uri> list = new ArrayList<>();
        for (String u : uris) list.add(Uri.parse(u));
        stage(list);
    }

    // content:// URIs have no path: copy them into the cache (each in a folder of its own, so
    // equal names don't collide), off the UI thread, then hand the paths to native code. A drop's
    // URIs stay readable until the next drop (KompozView holds its DragAndDropPermissions).
    private void stage(List<Uri> uris) {
        new Thread(() -> {
            List<String> paths = new ArrayList<>();
            // A folder per batch: an earlier batch may still be being sent.
            File staging = new File(new File(getCacheDir(), "airlink_send"), String.valueOf(System.nanoTime()));
            for (int i = 0; i < uris.size(); i++) {
                Uri uri = uris.get(i);
                File out = new File(new File(staging, String.valueOf(i)), displayName(uri));
                out.getParentFile().mkdirs();
                try (InputStream in = getContentResolver().openInputStream(uri); OutputStream os = new FileOutputStream(out)) {
                    if (in == null) continue;
                    byte[] buf = new byte[256 * 1024];
                    int n;
                    while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                    paths.add(out.getAbsolutePath());
                } catch (Exception e) {
                    out.delete();
                }
            }
            String[] result = paths.toArray(new String[0]);
            runOnUiThread(() -> nativeFilesPicked(result));
        }, "airlink-pick").start();
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String name = c.getString(0);
                if (name != null && !name.isEmpty()) return name.replace('/', '_');
            }
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        return last != null ? last.replace('/', '_') : "file";
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }

    private static native void nativeInit(MainActivity activity, String filesDir, String downloadDir, String deviceName);

    private static native void nativeFilesPicked(String[] paths);
}
