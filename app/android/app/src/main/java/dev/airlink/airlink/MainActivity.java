package dev.airlink.airlink;

import android.Manifest;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.media.MediaScannerConnection;
import android.net.Uri;
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
// Android: the background service, where files go, the device's name, the file picker, and files
// shared to AirLink from other apps (the share sheet).
public class MainActivity extends KompozActivity {
    private static final int PICK_FILES = 7001;

    static volatile boolean visible;  // on screen: requests show as the app's dialogs, not notifications
    private static MainActivity current;

    static MainActivity current() {
        return current;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        AirLinkService.appContext = getApplicationContext();
        super.onCreate(savedInstanceState);  // loads libmain.so
        current = this;
        cleanStaged(System.nanoTime());
        nativeInit(this, getFilesDir().getAbsolutePath(), downloadDir(), deviceName());
        AirLinkService.start(this);
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1);
        shared(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        shared(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        visible = true;
    }

    @Override
    protected void onPause() {
        visible = false;
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (current == this) current = null;
        super.onDestroy();
    }

    // Files from another app's Share menu: copied, then the app asks which device gets them.
    private void shared(Intent intent) {
        if (intent == null) return;
        String action = intent.getAction();
        List<Uri> uris = new ArrayList<>();
        if (Intent.ACTION_SEND.equals(action)) {
            Uri uri = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            if (uri != null) uris.add(uri);
        } else if (Intent.ACTION_SEND_MULTIPLE.equals(action)) {
            ArrayList<Uri> list = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM);
            if (list != null) uris.addAll(list);
        }
        if (uris.isEmpty()) return;
        setIntent(new Intent(this, MainActivity.class));  // handled: not again when recreated
        stage(uris, true);
    }

    /** A request notification's button (PromptReceiver), on the UI thread. */
    static void respond(long requestId, boolean accept) {
        nativeRespond(requestId, accept);
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
        stage(uris, false);
    }

    /** Files dropped from another app (content:// URIs): copied, then given to nativeFilesPicked. */
    public void stageUris(String[] uris) {
        List<Uri> list = new ArrayList<>();
        for (String u : uris) list.add(Uri.parse(u));
        stage(list, false);
    }

    // content:// URIs have no path: copy them into the cache (each in a folder of its own, so
    // equal names don't collide), off the UI thread, then hand the paths to native code. A drop's
    // URIs stay readable until the next drop (KompozView holds its DragAndDropPermissions).
    private void stage(List<Uri> uris, boolean share) {
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
            runOnUiThread(() -> {
                if (share) nativeShared(result);
                else nativeFilesPicked(result);
            });
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

    // Copies staged for sending by earlier runs (a folder per batch, named by System.nanoTime()).
    private void cleanStaged(long before) {
        File root = new File(getCacheDir(), "airlink_send");
        new Thread(() -> {
            File[] batches = root.listFiles();
            if (batches == null) return;
            for (File batch : batches) {
                try {
                    if (Long.parseLong(batch.getName()) < before) deleteTree(batch);
                } catch (NumberFormatException e) {
                    deleteTree(batch);
                }
            }
        }, "airlink-clean").start();
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }

    private static native void nativeInit(MainActivity activity, String filesDir, String downloadDir, String deviceName);

    private static native void nativeFilesPicked(String[] paths);

    private static native void nativeShared(String[] paths);

    private static native void nativeRespond(long requestId, boolean accept);
}
