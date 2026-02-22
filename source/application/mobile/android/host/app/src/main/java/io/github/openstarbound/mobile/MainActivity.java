package io.github.openstarbound.mobile;

import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Toast;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class MainActivity extends SDLActivity {
    private static final int REQUEST_PICK_PAK = 0x5001;
    private static final int REQUEST_PICK_MODS = 0x5002;
    private static final Object PICKER_LOCK = new Object();
    private static final long PICKER_TIMEOUT_SECONDS = 180;

    private static MainActivity sInstance;
    private static CountDownLatch sLatch;
    private static String sTargetPackedPak;
    private static String sTargetModsDir;
    private static String sPickedPakResult;
    private static ArrayList<String> sImportedMods;
    private OnBackInvokedCallback mBackCallback;

    @Override
    protected String[] getLibraries() {
        // SDL3 is linked statically into libmain.so in this build.
        return new String[] { "main" };
    }

    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        // Keep orientation static from manifest.
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sInstance = this;
        super.onCreate(savedInstanceState);
        applyStableDisplayConfig();

        if (Build.VERSION.SDK_INT >= 33) {
            mBackCallback = () -> {
                // Consume predictive back to avoid activity teardown races that can
                // abort in SDL's native Vsync receiver.
            };
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                mBackCallback
            );
        }
    }

    @Override
    protected void onDestroy() {
        if (Build.VERSION.SDK_INT >= 33 && mBackCallback != null) {
            try {
                getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(mBackCallback);
            } catch (Throwable ignored) {
            }
            mBackCallback = null;
        }
        super.onDestroy();
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyStableDisplayConfig();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        // Consume back button instead of finishing the activity.
    }

    private static MainActivity instance() {
        return sInstance;
    }

    private void applyStableDisplayConfig() {
        try {
            Window window = getWindow();
            if (window == null) {
                return;
            }

            WindowManager.LayoutParams attrs = window.getAttributes();
            if (Build.VERSION.SDK_INT >= 28) {
                attrs.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            }
            window.setAttributes(attrs);

            View decor = window.getDecorView();
            if (decor != null) {
                int flags = View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
                decor.setSystemUiVisibility(flags);
            }
        } catch (Throwable ignored) {
        }
    }

    private static String sanitizeFileName(String name, String fallback) {
        if (name == null || name.isEmpty()) {
            return fallback;
        }
        return name.replaceAll("[\\\\/:*?\"<>|]", "_");
    }

    private static String displayName(ContentResolver resolver, Uri uri) {
        String name = null;
        Cursor cursor = null;
        try {
            cursor = resolver.query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null);
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) {
                    name = cursor.getString(index);
                }
            }
        } catch (Throwable ignored) {
        } finally {
            if (cursor != null) {
                cursor.close();
            }
        }
        return name;
    }

    private static String copyUriToPath(MainActivity activity, Uri uri, File targetFile) {
        ContentResolver resolver = activity.getContentResolver();
        try {
            int takeFlags = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
            resolver.takePersistableUriPermission(uri, takeFlags & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION));
        } catch (Throwable ignored) {
        }

        targetFile.getParentFile().mkdirs();
        File tmp = new File(targetFile.getAbsolutePath() + ".tmp");
        try (InputStream in = resolver.openInputStream(uri);
             FileOutputStream out = new FileOutputStream(tmp, false)) {
            if (in == null) {
                return null;
            }
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
            out.flush();
            if (!tmp.renameTo(targetFile)) {
                if (targetFile.exists() && !targetFile.delete()) {
                    return null;
                }
                if (!tmp.renameTo(targetFile)) {
                    return null;
                }
            }
            return targetFile.getAbsolutePath();
        } catch (Throwable t) {
            tmp.delete();
            return null;
        }
    }

    public static String pickPackedPakAndImport(String targetPath) {
        MainActivity activity = instance();
        if (activity == null) {
            return null;
        }

        CountDownLatch latch = new CountDownLatch(1);
        synchronized (PICKER_LOCK) {
            sLatch = latch;
            sTargetPackedPak = targetPath;
            sTargetModsDir = null;
            sPickedPakResult = null;
            sImportedMods = new ArrayList<>();
        }

        activity.runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "*/*", "application/octet-stream" });
            activity.startActivityForResult(intent, REQUEST_PICK_PAK);
        });

        try {
            latch.await(PICKER_TIMEOUT_SECONDS, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }

        synchronized (PICKER_LOCK) {
            return sPickedPakResult;
        }
    }

    public static String[] importMods(String modsDirectory) {
        MainActivity activity = instance();
        if (activity == null) {
            return new String[0];
        }

        CountDownLatch latch = new CountDownLatch(1);
        synchronized (PICKER_LOCK) {
            sLatch = latch;
            sTargetPackedPak = null;
            sTargetModsDir = modsDirectory;
            sPickedPakResult = null;
            sImportedMods = new ArrayList<>();
        }

        activity.runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            activity.startActivityForResult(intent, REQUEST_PICK_MODS);
        });

        try {
            latch.await(PICKER_TIMEOUT_SECONDS, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }

        synchronized (PICKER_LOCK) {
            return sImportedMods.toArray(new String[0]);
        }
    }

    public static boolean openModsDirectory(String modsDirectory) {
        MainActivity activity = instance();
        if (activity == null) {
            return false;
        }

        activity.runOnUiThread(() -> {
            try {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                if (Build.VERSION.SDK_INT >= 26) {
                    intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, Uri.parse("content://com.android.externalstorage.documents/document/primary%3A"));
                }
                activity.startActivity(intent);
            } catch (Throwable ignored) {
            }
        });
        return true;
    }

    public static void showToast(String message) {
        MainActivity activity = instance();
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(() -> Toast.makeText(activity, message, Toast.LENGTH_LONG).show());
    }

    public static void showDialog(String title, String message) {
        MainActivity activity = instance();
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(() -> new AlertDialog.Builder(activity)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .show());
    }

    public static boolean openAppSettings() {
        MainActivity activity = instance();
        if (activity == null) {
            return false;
        }
        activity.runOnUiThread(() -> {
            Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
            intent.setData(Uri.fromParts("package", activity.getPackageName(), null));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            activity.startActivity(intent);
        });
        return true;
    }

    public static String syncBundledAssets(String targetRootDir) {
        MainActivity activity = instance();
        if (activity == null || targetRootDir == null || targetRootDir.isEmpty()) {
            return null;
        }

        try {
            File targetRoot = new File(targetRootDir);
            if (!targetRoot.exists() && !targetRoot.mkdirs()) {
                return null;
            }

            if (!copyAssetTreeIfMissing(activity, "opensb", new File(targetRoot, "opensb"))) {
                return null;
            }
            return targetRoot.getAbsolutePath();
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static boolean copyAssetTreeIfMissing(MainActivity activity, String assetPath, File dst) {
        try {
            String[] children = activity.getAssets().list(assetPath);
            if (children == null) {
                return false;
            }

            if (children.length == 0) {
                // File node
                if (dst.exists() && dst.isFile()) {
                    return true;
                }
                File parent = dst.getParentFile();
                if (parent != null && !parent.exists() && !parent.mkdirs()) {
                    return false;
                }

                File tmp = new File(dst.getAbsolutePath() + ".tmp");
                try (InputStream in = activity.getAssets().open(assetPath);
                     FileOutputStream out = new FileOutputStream(tmp, false)) {
                    byte[] buffer = new byte[64 * 1024];
                    int read;
                    while ((read = in.read(buffer)) > 0) {
                        out.write(buffer, 0, read);
                    }
                    out.flush();
                }

                if (dst.exists() && !dst.delete()) {
                    tmp.delete();
                    return false;
                }
                if (!tmp.renameTo(dst)) {
                    tmp.delete();
                    return false;
                }
                return true;
            }

            // Directory node
            if (!dst.exists() && !dst.mkdirs()) {
                return false;
            }

            for (String child : children) {
                String childAssetPath = assetPath + "/" + child;
                File childDst = new File(dst, child);
                if (!copyAssetTreeIfMissing(activity, childAssetPath, childDst)) {
                    return false;
                }
            }
            return true;
        } catch (Throwable ignored) {
            return false;
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        CountDownLatch latch;
        synchronized (PICKER_LOCK) {
            latch = sLatch;
        }
        if (latch == null) {
            return;
        }

        try {
            if (resultCode != RESULT_OK || data == null) {
                return;
            }

            if (requestCode == REQUEST_PICK_PAK) {
                Uri uri = data.getData();
                String targetPath;
                synchronized (PICKER_LOCK) {
                    targetPath = sTargetPackedPak;
                }
                if (uri != null && targetPath != null) {
                    String imported = copyUriToPath(this, uri, new File(targetPath));
                    synchronized (PICKER_LOCK) {
                        sPickedPakResult = imported;
                    }
                }
            } else if (requestCode == REQUEST_PICK_MODS) {
                String modsDir;
                synchronized (PICKER_LOCK) {
                    modsDir = sTargetModsDir;
                }
                if (modsDir == null) {
                    return;
                }

                ArrayList<Uri> uris = new ArrayList<>();
                if (data.getData() != null) {
                    uris.add(data.getData());
                }
                if (data.getClipData() != null) {
                    for (int i = 0; i < data.getClipData().getItemCount(); ++i) {
                        Uri uri = data.getClipData().getItemAt(i).getUri();
                        if (uri != null) {
                            uris.add(uri);
                        }
                    }
                }

                ContentResolver resolver = getContentResolver();
                for (Uri uri : uris) {
                    String name = displayName(resolver, uri);
                    if (name == null || name.isEmpty()) {
                        name = "mod_" + System.currentTimeMillis() + ".pak";
                    }
                    File target = new File(modsDir, sanitizeFileName(name, "mod.pak"));
                    String imported = copyUriToPath(this, uri, target);
                    if (imported != null) {
                        synchronized (PICKER_LOCK) {
                            sImportedMods.add(imported);
                        }
                    }
                }
            }
        } finally {
            latch.countDown();
            synchronized (PICKER_LOCK) {
                sLatch = null;
                sTargetPackedPak = null;
                sTargetModsDir = null;
            }
        }
    }
}
