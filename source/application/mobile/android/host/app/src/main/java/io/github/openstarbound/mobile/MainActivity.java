package io.github.openstarbound.mobile;

import android.app.AlertDialog;
import android.app.ActivityManager;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ResolveInfo;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Color;
import android.hardware.Sensor;
import android.hardware.SensorManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Debug;
import android.os.LocaleList;
import android.os.Looper;
import android.util.Log;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.DisplayCutout;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Toast;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import androidx.core.content.FileProvider;
import androidx.documentfile.provider.DocumentFile;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

public final class MainActivity extends SDLActivity {
    private static final String TAG = "OpenStarbound";
    private static final int REQUEST_PICK_PAK = 0x5001;
    private static final int REQUEST_PICK_MOD_PAK = 0x5002;
    private static final int REQUEST_PICK_MOD_FOLDER = 0x5003;
    private static final int REQUEST_PICK_MODS_FOLDER = 0x5004;
    private static final int REQUEST_PICK_SAVE_ZIP = 0x5005;
    private static final Object PICKER_LOCK = new Object();
    private static final Object SAFE_AREA_LOCK = new Object();
    private static final long PICKER_TIMEOUT_SECONDS = 180;
    private static final int[] sSafeAreaInsets = new int[] { 0, 0, 0, 0 };

    private static MainActivity sInstance;
    private static CountDownLatch sLatch;
    private static String sTargetPackedPak;
    private static String sTargetModsDir;
    private static String sTargetSaveRoot;
    private static String sPickedPakResult;
    private static ArrayList<String> sImportedMods;
    private static boolean sSaveImportResult;
    private OnBackInvokedCallback mBackCallback;

    @Override
    protected String[] getLibraries() {
        // SDL3 is linked statically into libmain.so in this build.
        return new String[] { "main" };
    }

    public static native void onNativeGyroAim(float x, float y, float z);

    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        if (w <= 1 || h <= 1) {
            return;
        }

        boolean allowLandscapeLeft = hint.contains("LandscapeLeft");
        boolean allowLandscapeRight = hint.contains("LandscapeRight");
        boolean allowPortrait = hint.contains("Portrait ") || hint.endsWith("Portrait");
        boolean allowPortraitUpsideDown = hint.contains("PortraitUpsideDown");
        boolean allowLandscapeFamily = allowLandscapeLeft || allowLandscapeRight;
        boolean allowPortraitFamily = allowPortrait || allowPortraitUpsideDown;

        int requestedOrientation;
        if (allowLandscapeFamily && allowPortraitFamily) {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_FULL_USER;
        } else if (allowLandscapeFamily) {
            requestedOrientation = allowLandscapeLeft && allowLandscapeRight
                ? ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE
                : allowLandscapeLeft
                    ? ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
                    : ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE;
        } else if (allowPortraitFamily) {
            requestedOrientation = allowPortrait && allowPortraitUpsideDown
                ? ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT
                : allowPortrait
                    ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                    : ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT;
        } else if (resizable) {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_FULL_USER;
        } else {
            requestedOrientation = w > h
                ? ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE
                : ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT;
        }

        setRequestedOrientation(requestedOrientation);
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        applyStableDisplayConfig();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sInstance = this;
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE);
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
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyStableDisplayConfig();
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        // Consume back button instead of finishing the activity.
    }

    private static MainActivity instance() {
        return sInstance;
    }

    public static int[] getSafeAreaInsets() {
        synchronized (SAFE_AREA_LOCK) {
            return sSafeAreaInsets.clone();
        }
    }

    private static void setSafeAreaInsets(int top, int left, int bottom, int right) {
        synchronized (SAFE_AREA_LOCK) {
            sSafeAreaInsets[0] = Math.max(0, top);
            sSafeAreaInsets[1] = Math.max(0, left);
            sSafeAreaInsets[2] = Math.max(0, bottom);
            sSafeAreaInsets[3] = Math.max(0, right);
        }
    }

    private void updateSafeAreaInsets(WindowInsets insets) {
        if (Build.VERSION.SDK_INT < 28 || insets == null) {
            setSafeAreaInsets(0, 0, 0, 0);
            return;
        }

        DisplayCutout cutout = insets.getDisplayCutout();
        if (cutout == null) {
            setSafeAreaInsets(0, 0, 0, 0);
            return;
        }

        setSafeAreaInsets(
            cutout.getSafeInsetTop(),
            cutout.getSafeInsetLeft(),
            cutout.getSafeInsetBottom(),
            cutout.getSafeInsetRight());
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
            if (Build.VERSION.SDK_INT >= 21) {
                window.setStatusBarColor(Color.TRANSPARENT);
                window.setNavigationBarColor(Color.TRANSPARENT);
            }
            if (Build.VERSION.SDK_INT >= 29) {
                window.setStatusBarContrastEnforced(false);
                window.setNavigationBarContrastEnforced(false);
            }
            if (Build.VERSION.SDK_INT >= 30) {
                window.setDecorFitsSystemWindows(false);
            }

            View decor = window.getDecorView();
            if (decor != null) {
                decor.setOnApplyWindowInsetsListener((view, insets) -> {
                    updateSafeAreaInsets(insets);
                    return insets;
                });
                updateSafeAreaInsets(decor.getRootWindowInsets());
                decor.requestApplyInsets();

                int flags = View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
                decor.setSystemUiVisibility(flags);

                if (Build.VERSION.SDK_INT >= 30) {
                    WindowInsetsController controller = decor.getWindowInsetsController();
                    if (controller != null) {
                        controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                        controller.hide(WindowInsets.Type.systemBars());
                    }
                }
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
            sTargetSaveRoot = null;
            sPickedPakResult = null;
            sImportedMods = new ArrayList<>();
            sSaveImportResult = false;
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

    private static String[] runModImportRequest(MainActivity activity, String modsDirectory, int requestCode, Intent intent) {
        if (activity == null) {
            return new String[0];
        }

        CountDownLatch latch = new CountDownLatch(1);
        synchronized (PICKER_LOCK) {
            sLatch = latch;
            sTargetPackedPak = null;
            sTargetModsDir = modsDirectory;
            sTargetSaveRoot = null;
            sPickedPakResult = null;
            sImportedMods = new ArrayList<>();
            sSaveImportResult = false;
        }

        activity.runOnUiThread(() -> activity.startActivityForResult(intent, requestCode));

        try {
            latch.await(PICKER_TIMEOUT_SECONDS, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }

        synchronized (PICKER_LOCK) {
            return sImportedMods.toArray(new String[0]);
        }
    }

    public static String[] importModPak(String modsDirectory) {
        MainActivity activity = instance();
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "*/*", "application/octet-stream" });
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        return runModImportRequest(activity, modsDirectory, REQUEST_PICK_MOD_PAK, intent);
    }

    public static String[] importSingleModFolder(String modsDirectory) {
        MainActivity activity = instance();
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        return runModImportRequest(activity, modsDirectory, REQUEST_PICK_MOD_FOLDER, intent);
    }

    public static String[] importModsFolder(String modsDirectory) {
        MainActivity activity = instance();
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        return runModImportRequest(activity, modsDirectory, REQUEST_PICK_MODS_FOLDER, intent);
    }

    public static boolean importSaveZip(String storageRoot) {
        MainActivity activity = instance();
        if (activity == null || storageRoot == null || storageRoot.isEmpty()) {
            return false;
        }

        CountDownLatch latch = new CountDownLatch(1);
        synchronized (PICKER_LOCK) {
            sLatch = latch;
            sTargetPackedPak = null;
            sTargetModsDir = null;
            sTargetSaveRoot = storageRoot;
            sPickedPakResult = null;
            sImportedMods = new ArrayList<>();
            sSaveImportResult = false;
        }

        activity.runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/zip");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "application/zip", "application/x-zip-compressed", "*/*" });
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            activity.startActivityForResult(intent, REQUEST_PICK_SAVE_ZIP);
        });

        try {
            latch.await(PICKER_TIMEOUT_SECONDS, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }

        synchronized (PICKER_LOCK) {
            return sSaveImportResult;
        }
    }

    private static Uri externalStorageDocumentUriForPath(String absolutePath) {
        if (absolutePath == null || absolutePath.isEmpty()) {
            return null;
        }

        String normalized = absolutePath.replace('\\', '/');
        String relative = null;

        String[] prefixes = new String[] {
            "/storage/emulated/0/",
            "/storage/self/primary/",
            "/sdcard/"
        };
        for (String prefix : prefixes) {
            if (normalized.startsWith(prefix)) {
                relative = normalized.substring(prefix.length());
                break;
            }
        }
        if (relative == null || relative.isEmpty()) {
            return null;
        }

        try {
            return DocumentsContract.buildDocumentUri(
                "com.android.externalstorage.documents",
                "primary:" + relative
            );
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static boolean ensureWritableDirectory(File directory) {
        if (directory == null) {
            return false;
        }

        try {
            if (!directory.exists() && !directory.mkdirs()) {
                return false;
            }
            if (!directory.isDirectory()) {
                return false;
            }

            File test = File.createTempFile(".osbm-write-test", ".tmp", directory);
            try (FileOutputStream out = new FileOutputStream(test, false)) {
                out.write(1);
            }
            return test.delete() || !test.exists();
        } catch (Throwable ignored) {
            return false;
        }
    }

    public static String resolveStorageRoot(String fallbackStorageRoot) {
        MainActivity activity = instance();
        if (activity != null) {
            try {
                File externalFiles = activity.getExternalFilesDir(null);
                if (ensureWritableDirectory(externalFiles)) {
                    return externalFiles.getAbsolutePath();
                }
            } catch (Throwable ignored) {
            }

            try {
                File internalFiles = activity.getFilesDir();
                if (ensureWritableDirectory(internalFiles)) {
                    return internalFiles.getAbsolutePath();
                }
            } catch (Throwable ignored) {
            }
        }

        if (fallbackStorageRoot == null || fallbackStorageRoot.isEmpty()) {
            return null;
        }

        File fallback = new File(fallbackStorageRoot);
        return ensureWritableDirectory(fallback) ? fallback.getAbsolutePath() : fallbackStorageRoot;
    }

    public static boolean setGyroSensorEnabled(boolean enabled) {
        MainActivity activity = instance();
        if (activity == null) {
            return false;
        }

        if (enabled && !hasGyroSensor()) {
            return false;
        }

        if (mSurface == null) {
            return false;
        }

        AtomicBoolean result = new AtomicBoolean(false);
        Runnable enableTask = () -> {
            if (mSurface != null) {
                mSurface.enableSensor(Sensor.TYPE_GYROSCOPE, enabled);
                result.set(true);
            }
        };

        if (Looper.myLooper() == Looper.getMainLooper()) {
            enableTask.run();
            return result.get();
        }

        CountDownLatch latch = new CountDownLatch(1);
        activity.runOnUiThread(() -> {
            try {
                enableTask.run();
            } finally {
                latch.countDown();
            }
        });

        try {
            if (!latch.await(2, TimeUnit.SECONDS)) {
                return false;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }

        return result.get();
    }

    public static boolean hasGyroSensor() {
        MainActivity activity = instance();
        if (activity == null) {
            return false;
        }

        SensorManager sensorManager = (SensorManager)activity.getSystemService(SENSOR_SERVICE);
        if (sensorManager == null) {
            return false;
        }

        if (sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null) {
            return true;
        }

        return Build.VERSION.SDK_INT >= 18 && sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE_UNCALIBRATED) != null;
    }

    public static String resolveModsDirectory(String fallbackModsDirectory) {
        MainActivity activity = instance();

        File modsDir = null;
        if (activity != null) {
            try {
                File externalFiles = activity.getExternalFilesDir(null);
                if (externalFiles != null) {
                    modsDir = new File(externalFiles, "mods");
                }
            } catch (Throwable ignored) {
            }
        }

        if (modsDir == null) {
            if (fallbackModsDirectory == null || fallbackModsDirectory.isEmpty()) {
                return null;
            }
            modsDir = new File(fallbackModsDirectory);
        }

        if (!modsDir.exists() && !modsDir.mkdirs()) {
            return fallbackModsDirectory;
        }
        return modsDir.getAbsolutePath();
    }

    private static boolean isPakFileName(String name) {
        return name != null && name.toLowerCase().endsWith(".pak");
    }

    private static File uniqueTargetFile(File parent, String requestedName, boolean treatAsDirectory) {
        String safeName = sanitizeFileName(requestedName, treatAsDirectory ? "mod_folder" : "mod.pak");
        String baseName = safeName;
        String extension = "";

        if (!treatAsDirectory) {
            int dot = safeName.lastIndexOf('.');
            if (dot > 0 && dot < safeName.length() - 1) {
                baseName = safeName.substring(0, dot);
                extension = safeName.substring(dot);
            }
        }

        File candidate = new File(parent, safeName);
        if (!candidate.exists()) {
            return candidate;
        }

        for (int i = 2; i < 10000; ++i) {
            String suffix = " (" + i + ")";
            String nextName = baseName + suffix + extension;
            candidate = new File(parent, nextName);
            if (!candidate.exists()) {
                return candidate;
            }
        }

        return new File(parent, baseName + "_" + System.currentTimeMillis() + extension);
    }

    private static boolean copyDocumentTreeContents(
        MainActivity activity,
        DocumentFile sourceDir,
        File targetDir
    ) {
        DocumentFile[] children;
        try {
            children = sourceDir.listFiles();
        } catch (Throwable ignored) {
            return false;
        }

        boolean copiedAny = false;
        for (DocumentFile child : children) {
            if (child == null) {
                continue;
            }

            String childName = child.getName();
            if (childName == null || childName.isEmpty()) {
                childName = child.isDirectory() ? "mod_folder" : "mod_file";
            }
            String safeName = sanitizeFileName(childName, child.isDirectory() ? "mod_folder" : "mod_file");
            File target = new File(targetDir, safeName);

            if (child.isDirectory()) {
                if (!target.exists() && !target.mkdirs()) {
                    continue;
                }
                copiedAny |= copyDocumentTreeContents(activity, child, target);
            } else if (child.isFile()) {
                String imported = copyUriToPath(activity, child.getUri(), target);
                if (imported != null) {
                    copiedAny = true;
                }
            }
        }

        return copiedAny;
    }

    private static boolean deleteRecursively(File file) {
        if (file == null || !file.exists()) {
            return true;
        }
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    if (!deleteRecursively(child)) {
                        return false;
                    }
                }
            }
        }
        return file.delete() || !file.exists();
    }

    private static boolean copyLocalTree(File source, File target) {
        if (source == null || target == null || !source.exists()) {
            return false;
        }

        if (source.isDirectory()) {
            if (!target.exists() && !target.mkdirs()) {
                return false;
            }
            File[] children = source.listFiles();
            if (children == null) {
                return true;
            }
            for (File child : children) {
                if (!copyLocalTree(child, new File(target, child.getName()))) {
                    return false;
                }
            }
            return true;
        }

        File parent = target.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            return false;
        }
        File tmp = new File(target.getAbsolutePath() + ".tmp");
        try (FileInputStream in = new FileInputStream(source);
             FileOutputStream out = new FileOutputStream(tmp, false)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
            out.flush();
        } catch (IOException e) {
            tmp.delete();
            return false;
        }
        if (target.exists() && !deleteRecursively(target)) {
            tmp.delete();
            return false;
        }
        if (!tmp.renameTo(target)) {
            tmp.delete();
            return false;
        }
        return true;
    }

    private static String safeZipEntryPath(String entryName) {
        if (entryName == null || entryName.isEmpty()) {
            return null;
        }
        String normalized = entryName.replace('\\', '/');
        while (normalized.startsWith("./")) {
            normalized = normalized.substring(2);
        }
        if (normalized.startsWith("/") || normalized.contains(":")) {
            return null;
        }
        String[] parts = normalized.split("/");
        ArrayList<String> safeParts = new ArrayList<>();
        for (String part : parts) {
            if (part == null || part.isEmpty() || ".".equals(part)) {
                continue;
            }
            if ("..".equals(part)) {
                return null;
            }
            safeParts.add(sanitizeFileName(part, "save"));
        }
        if (safeParts.isEmpty()) {
            return null;
        }
        StringBuilder builder = new StringBuilder();
        for (int i = 0; i < safeParts.size(); ++i) {
            if (i > 0) {
                builder.append(File.separatorChar);
            }
            builder.append(safeParts.get(i));
        }
        return builder.toString();
    }

    private static boolean unzipSaveArchive(MainActivity activity, Uri zipUri, File tempRoot) {
        if (activity == null || zipUri == null || tempRoot == null) {
            return false;
        }
        if (!tempRoot.exists() && !tempRoot.mkdirs()) {
            return false;
        }

        try (InputStream raw = activity.getContentResolver().openInputStream(zipUri);
             ZipInputStream zip = raw == null ? null : new ZipInputStream(raw)) {
            if (zip == null) {
                return false;
            }

            byte[] buffer = new byte[64 * 1024];
            ZipEntry entry;
            boolean extractedAny = false;
            while ((entry = zip.getNextEntry()) != null) {
                String safePath = safeZipEntryPath(entry.getName());
                if (safePath == null) {
                    zip.closeEntry();
                    return false;
                }

                File target = new File(tempRoot, safePath);
                String targetPath = target.getCanonicalPath();
                String rootPath = tempRoot.getCanonicalPath();
                if (!targetPath.equals(rootPath) && !targetPath.startsWith(rootPath + File.separator)) {
                    zip.closeEntry();
                    return false;
                }

                if (entry.isDirectory()) {
                    if (!target.exists() && !target.mkdirs()) {
                        zip.closeEntry();
                        return false;
                    }
                } else {
                    File parent = target.getParentFile();
                    if (parent != null && !parent.exists() && !parent.mkdirs()) {
                        zip.closeEntry();
                        return false;
                    }
                    try (FileOutputStream out = new FileOutputStream(target, false)) {
                        int read;
                        while ((read = zip.read(buffer)) > 0) {
                            out.write(buffer, 0, read);
                        }
                    }
                    extractedAny = true;
                }
                zip.closeEntry();
            }
            return extractedAny;
        } catch (IOException e) {
            return false;
        }
    }

    private static boolean hasSavePayload(File directory) {
        return directory != null && directory.isDirectory()
            && (new File(directory, "player").isDirectory() || new File(directory, "universe").isDirectory());
    }

    private static File findSavePayloadRoot(File directory, int depth) {
        if (directory == null || !directory.isDirectory() || depth < 0) {
            return null;
        }
        if (hasSavePayload(directory)) {
            return directory;
        }

        File[] children = directory.listFiles();
        if (children == null) {
            return null;
        }

        for (File child : children) {
            if (child != null && child.isDirectory()) {
                File found = findSavePayloadRoot(child, depth - 1);
                if (found != null) {
                    return found;
                }
            }
        }
        return null;
    }

    private static boolean installSavePayload(File payloadRoot, File storageRoot) {
        if (!hasSavePayload(payloadRoot) || storageRoot == null) {
            return false;
        }
        if (!storageRoot.exists() && !storageRoot.mkdirs()) {
            return false;
        }

        String[] saveDirectories = new String[] { "player", "universe" };
        File backupRoot = new File(storageRoot, ".save-import-backup-" + System.currentTimeMillis());
        ArrayList<String> moved = new ArrayList<>();
        boolean success = false;

        try {
            for (String name : saveDirectories) {
                File source = new File(payloadRoot, name);
                if (!source.isDirectory()) {
                    continue;
                }

                File target = new File(storageRoot, name);
                if (target.exists()) {
                    if (!backupRoot.exists() && !backupRoot.mkdirs()) {
                        return false;
                    }
                    File backup = new File(backupRoot, name);
                    if (!target.renameTo(backup)) {
                        return false;
                    }
                    moved.add(name);
                }

                if (!copyLocalTree(source, target)) {
                    return false;
                }
            }

            success = true;
            return true;
        } finally {
            if (success) {
                deleteRecursively(backupRoot);
            } else if (backupRoot.exists()) {
                for (String name : moved) {
                    File target = new File(storageRoot, name);
                    File backup = new File(backupRoot, name);
                    if (backup.exists()) {
                        deleteRecursively(target);
                        backup.renameTo(target);
                    }
                }
                deleteRecursively(backupRoot);
            }
        }
    }

    private static boolean importSaveZipFromUri(MainActivity activity, Uri zipUri, String storageRoot) {
        File root = new File(storageRoot);
        File tempRoot = new File(root, ".save-import-" + System.currentTimeMillis());
        try {
            if (!unzipSaveArchive(activity, zipUri, tempRoot)) {
                return false;
            }
            File payloadRoot = findSavePayloadRoot(tempRoot, 3);
            return payloadRoot != null && installSavePayload(payloadRoot, root);
        } finally {
            deleteRecursively(tempRoot);
        }
    }

    private static void importSingleModFolderFromTree(
        MainActivity activity,
        DocumentFile pickedTree,
        File modsDirFile,
        ArrayList<String> importedMods
    ) {
        String rootName = pickedTree.getName();
        if (rootName == null || rootName.isEmpty()) {
            rootName = "mod_folder";
        }

        File targetModRoot = uniqueTargetFile(modsDirFile, rootName, true);
        if (!targetModRoot.exists() && !targetModRoot.mkdirs()) {
            return;
        }

        copyDocumentTreeContents(activity, pickedTree, targetModRoot);
        importedMods.add(targetModRoot.getAbsolutePath());
    }

    private static void importAllModsFromFolderTree(
        MainActivity activity,
        DocumentFile pickedTree,
        File modsDirFile,
        ArrayList<String> importedMods
    ) {
        DocumentFile[] entries;
        try {
            entries = pickedTree.listFiles();
        } catch (Throwable ignored) {
            return;
        }

        for (DocumentFile entry : entries) {
            if (entry == null) {
                continue;
            }

            String entryName = entry.getName();
            if (entryName == null || entryName.isEmpty()) {
                entryName = entry.isDirectory() ? "mod_folder" : "mod_file";
            }

            if (entry.isDirectory()) {
                File targetModRoot = uniqueTargetFile(modsDirFile, entryName, true);
                if (!targetModRoot.exists() && !targetModRoot.mkdirs()) {
                    continue;
                }
                copyDocumentTreeContents(activity, entry, targetModRoot);
                importedMods.add(targetModRoot.getAbsolutePath());
            } else if (entry.isFile() && isPakFileName(entryName)) {
                File targetPak = uniqueTargetFile(modsDirFile, entryName, false);
                String imported = copyUriToPath(activity, entry.getUri(), targetPak);
                if (imported != null) {
                    importedMods.add(imported);
                }
            }
        }
    }

    private static boolean openDirectoryInSystemBrowser(MainActivity activity, File directory) {
        if (activity == null || directory == null) {
            return false;
        }
        if (!directory.exists() && !directory.mkdirs()) {
            return false;
        }
        if (!directory.isDirectory()) {
            return false;
        }

        activity.runOnUiThread(() -> {
            try {
                Uri documentUri = externalStorageDocumentUriForPath(directory.getAbsolutePath());

                if (documentUri != null) {
                    Intent documentIntent = new Intent(Intent.ACTION_VIEW);
                    documentIntent.setDataAndType(documentUri, DocumentsContract.Document.MIME_TYPE_DIR);
                    documentIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    documentIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    if (activity.getPackageManager().resolveActivity(documentIntent, 0) != null) {
                        activity.startActivity(documentIntent);
                        return;
                    }
                }

                Uri uri = FileProvider.getUriForFile(
                    activity,
                    activity.getPackageName() + ".fileprovider",
                    directory
                );

                Intent intent = new Intent(Intent.ACTION_VIEW);
                intent.setDataAndType(uri, DocumentsContract.Document.MIME_TYPE_DIR);
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);

                List<ResolveInfo> targets = activity.getPackageManager().queryIntentActivities(intent, 0);
                for (ResolveInfo target : targets) {
                    activity.grantUriPermission(
                        target.activityInfo.packageName,
                        uri,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                    );
                }

                if (!targets.isEmpty()) {
                    activity.startActivity(intent);
                    return;
                }

                Intent fallback = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                fallback.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                fallback.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                Uri fallbackUri = externalStorageDocumentUriForPath(directory.getAbsolutePath());
                if (Build.VERSION.SDK_INT >= 26 && fallbackUri != null) {
                    fallback.putExtra(DocumentsContract.EXTRA_INITIAL_URI, fallbackUri);
                }
                activity.startActivity(fallback);
            } catch (Throwable ignored) {
            }
        });
        return true;
    }

    public static boolean openModsDirectory(String modsDirectory) {
        MainActivity activity = instance();
        if (activity == null) {
            return false;
        }

        String resolvedModsDir = resolveModsDirectory(modsDirectory);
        if (resolvedModsDir == null || resolvedModsDir.isEmpty()) {
            return false;
        }

        return openDirectoryInSystemBrowser(activity, new File(resolvedModsDir));
    }

    public static boolean openSaveDirectory(String storageRoot) {
        MainActivity activity = instance();
        if (activity == null || storageRoot == null || storageRoot.isEmpty()) {
            return false;
        }

        return openDirectoryInSystemBrowser(activity, new File(storageRoot));
    }

    private static void writeZipText(ZipOutputStream zip, String name, String text) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        byte[] bytes = text.getBytes("UTF-8");
        zip.write(bytes, 0, bytes.length);
        zip.closeEntry();
    }

    private static void addFileToZip(ZipOutputStream zip, File file, String entryName) throws IOException {
        if (file == null || !file.isFile()) {
            return;
        }

        zip.putNextEntry(new ZipEntry(entryName));
        try (FileInputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                zip.write(buffer, 0, read);
            }
        }
        zip.closeEntry();
    }

    private static void addDirectoryToZip(ZipOutputStream zip, File root, File directory, String prefix) throws IOException {
        if (directory == null || !directory.isDirectory()) {
            return;
        }

        File[] children = directory.listFiles();
        if (children == null) {
            return;
        }

        for (File child : children) {
            String relative = root.toURI().relativize(child.toURI()).getPath();
            String entryName = prefix + "/" + relative;
            if (child.isDirectory()) {
                addDirectoryToZip(zip, root, child, prefix);
            } else if (child.isFile()) {
                addFileToZip(zip, child, entryName);
            }
        }
    }

    private static boolean hasExportableSaveData(File root) {
        return root != null && root.isDirectory()
            && (new File(root, "player").isDirectory() || new File(root, "universe").isDirectory());
    }

    private static void shareZipFile(MainActivity activity, File zipFile, String title) {
        Uri uri = FileProvider.getUriForFile(
            activity,
            activity.getPackageName() + ".fileprovider",
            zipFile
        );

        Intent intent = new Intent(Intent.ACTION_SEND);
        intent.setType("application/zip");
        intent.putExtra(Intent.EXTRA_STREAM, uri);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);

        List<ResolveInfo> targets = activity.getPackageManager().queryIntentActivities(intent, 0);
        for (ResolveInfo target : targets) {
            activity.grantUriPermission(
                target.activityInfo.packageName,
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
            );
        }

        activity.startActivity(Intent.createChooser(intent, title));
    }

    public static boolean exportSaveZip(String storageRoot) {
        MainActivity activity = instance();
        if (activity == null || storageRoot == null || storageRoot.isEmpty()) {
            return false;
        }

        File root = new File(storageRoot);
        if (!hasExportableSaveData(root)) {
            return false;
        }

        activity.runOnUiThread(() -> {
            try {
                File exportDir = new File(activity.getFilesDir(), "save-exports");
                if (!exportDir.exists() && !exportDir.mkdirs()) {
                    Toast.makeText(activity, "Could not create save export folder.", Toast.LENGTH_LONG).show();
                    return;
                }

                File zipFile = new File(exportDir, "openstarbound-save-" + System.currentTimeMillis() + ".zip");
                try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(zipFile, false))) {
                    File player = new File(root, "player");
                    File universe = new File(root, "universe");
                    addDirectoryToZip(zip, player, player, "player");
                    addDirectoryToZip(zip, universe, universe, "universe");
                }

                shareZipFile(activity, zipFile, "Share OpenStarbound save");
            } catch (Throwable t) {
                Toast.makeText(activity, "Could not export save: " + t.getMessage(), Toast.LENGTH_LONG).show();
            }
        });
        return true;
    }

    private static String diagnosticsDeviceSummary() {
        StringBuilder builder = new StringBuilder();
        builder.append("manufacturer=").append(Build.MANUFACTURER).append('\n');
        builder.append("brand=").append(Build.BRAND).append('\n');
        builder.append("model=").append(Build.MODEL).append('\n');
        builder.append("device=").append(Build.DEVICE).append('\n');
        builder.append("product=").append(Build.PRODUCT).append('\n');
        builder.append("hardware=").append(Build.HARDWARE).append('\n');
        builder.append("board=").append(Build.BOARD).append('\n');
        builder.append("supportedAbis=");
        for (String abi : Build.SUPPORTED_ABIS) {
            builder.append(abi).append(' ');
        }
        builder.append('\n');
        builder.append("sdk=").append(Build.VERSION.SDK_INT).append('\n');
        builder.append("release=").append(Build.VERSION.RELEASE).append('\n');
        int[] insets = getSafeAreaInsets();
        builder.append("safeArea=").append(insets[0]).append(',')
            .append(insets[1]).append(',')
            .append(insets[2]).append(',')
            .append(insets[3]).append('\n');
        return builder.toString();
    }

    private static String readProcSelfStatus() {
        StringBuilder builder = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new FileReader("/proc/self/status"))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (line.startsWith("Name:")
                    || line.startsWith("State:")
                    || line.startsWith("VmPeak:")
                    || line.startsWith("VmSize:")
                    || line.startsWith("VmHWM:")
                    || line.startsWith("VmRSS:")
                    || line.startsWith("RssAnon:")
                    || line.startsWith("RssFile:")
                    || line.startsWith("RssShmem:")
                    || line.startsWith("Threads:")) {
                    builder.append(line).append('\n');
                }
            }
        } catch (Throwable t) {
            builder.append("procStatusError=").append(t.getMessage()).append('\n');
        }
        return builder.toString();
    }

    private static String diagnosticsMemorySummary() {
        StringBuilder builder = new StringBuilder();
        Runtime runtime = Runtime.getRuntime();
        builder.append("javaHeapUsed=").append(runtime.totalMemory() - runtime.freeMemory()).append('\n');
        builder.append("javaHeapFree=").append(runtime.freeMemory()).append('\n');
        builder.append("javaHeapTotal=").append(runtime.totalMemory()).append('\n');
        builder.append("javaHeapMax=").append(runtime.maxMemory()).append('\n');
        builder.append("nativeHeapAllocated=").append(Debug.getNativeHeapAllocatedSize()).append('\n');
        builder.append("nativeHeapFree=").append(Debug.getNativeHeapFreeSize()).append('\n');
        builder.append("nativeHeapSize=").append(Debug.getNativeHeapSize()).append('\n');

        ActivityManager activityManager = (ActivityManager) instance().getSystemService(Context.ACTIVITY_SERVICE);
        if (activityManager != null) {
            ActivityManager.MemoryInfo memoryInfo = new ActivityManager.MemoryInfo();
            activityManager.getMemoryInfo(memoryInfo);
            builder.append("memoryClassMb=").append(activityManager.getMemoryClass()).append('\n');
            builder.append("largeMemoryClassMb=").append(activityManager.getLargeMemoryClass()).append('\n');
            builder.append("isLowRamDevice=").append(activityManager.isLowRamDevice()).append('\n');
            builder.append("systemAvailMem=").append(memoryInfo.availMem).append('\n');
            builder.append("systemTotalMem=").append(memoryInfo.totalMem).append('\n');
            builder.append("systemThreshold=").append(memoryInfo.threshold).append('\n');
            builder.append("systemLowMemory=").append(memoryInfo.lowMemory).append('\n');
        }

        Debug.MemoryInfo processMemory = new Debug.MemoryInfo();
        Debug.getMemoryInfo(processMemory);
        builder.append("processTotalPssKb=").append(processMemory.getTotalPss()).append('\n');
        builder.append("processTotalPrivateDirtyKb=").append(processMemory.getTotalPrivateDirty()).append('\n');
        builder.append("processTotalSharedDirtyKb=").append(processMemory.getTotalSharedDirty()).append('\n');
        builder.append('\n').append("[proc/self/status]").append('\n');
        builder.append(readProcSelfStatus());
        return builder.toString();
    }

    private static void appendFileListing(StringBuilder builder, File file, String label, int depth) {
        if (file == null) {
            return;
        }

        for (int i = 0; i < depth; ++i) {
            builder.append("  ");
        }
        builder.append(label)
            .append(" exists=").append(file.exists())
            .append(" directory=").append(file.isDirectory())
            .append(" file=").append(file.isFile())
            .append(" size=").append(file.isFile() ? file.length() : 0)
            .append(" modified=").append(file.exists() ? file.lastModified() : 0)
            .append('\n');

        if (!file.isDirectory() || depth >= 2) {
            return;
        }

        File[] children = file.listFiles();
        if (children == null) {
            for (int i = 0; i <= depth; ++i) {
                builder.append("  ");
            }
            builder.append("<unreadable>\n");
            return;
        }

        for (File child : children) {
            appendFileListing(builder, child, child.getName(), depth + 1);
        }
    }

    private static String diagnosticsStorageSummary(File root) {
        StringBuilder builder = new StringBuilder();
        builder.append("storageRoot=").append(root.getAbsolutePath()).append('\n');
        appendFileListing(builder, new File(root, "assets"), "assets", 0);
        appendFileListing(builder, new File(root, "bundled_assets"), "bundled_assets", 0);
        appendFileListing(builder, new File(root, "mods"), "mods", 0);
        appendFileListing(builder, new File(root, "storage"), "storage", 0);
        appendFileListing(builder, new File(root, "player"), "player", 0);
        appendFileListing(builder, new File(root, "universe"), "universe", 0);
        return builder.toString();
    }

    public static boolean exportDiagnostics(String storageRoot) {
        MainActivity activity = instance();
        if (activity == null || storageRoot == null || storageRoot.isEmpty()) {
            return false;
        }

        activity.runOnUiThread(() -> {
            try {
                File root = new File(storageRoot);
                File diagnosticsDir = new File(activity.getFilesDir(), "diagnostics");
                if (!diagnosticsDir.exists() && !diagnosticsDir.mkdirs()) {
                    Toast.makeText(activity, "Could not create diagnostics folder.", Toast.LENGTH_LONG).show();
                    return;
                }

                File zipFile = new File(diagnosticsDir, "openstarbound-diagnostics-" + System.currentTimeMillis() + ".zip");
                try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(zipFile, false))) {
                    writeZipText(zip, "device.txt", diagnosticsDeviceSummary());
                    writeZipText(zip, "memory.txt", diagnosticsMemorySummary());
                    writeZipText(zip, "storage.txt", diagnosticsStorageSummary(root));
                    File logs = new File(root, "logs");
                    addDirectoryToZip(zip, logs, logs, "logs");
                    addFileToZip(zip, new File(root, "mobile_launcher.json"), "mobile_launcher.json");
                    addFileToZip(zip, new File(root, "sbinit.mobile.config"), "sbinit.mobile.config");
                }

                Uri uri = FileProvider.getUriForFile(
                    activity,
                    activity.getPackageName() + ".fileprovider",
                    zipFile
                );

                Intent intent = new Intent(Intent.ACTION_SEND);
                intent.setType("application/zip");
                intent.putExtra(Intent.EXTRA_STREAM, uri);
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);

                List<ResolveInfo> targets = activity.getPackageManager().queryIntentActivities(intent, 0);
                for (ResolveInfo target : targets) {
                    activity.grantUriPermission(
                        target.activityInfo.packageName,
                        uri,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION
                    );
                }

                activity.startActivity(Intent.createChooser(intent, "Share OpenStarbound diagnostics"));
            } catch (Throwable t) {
                Toast.makeText(activity, "Could not export diagnostics: " + t.getMessage(), Toast.LENGTH_LONG).show();
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

    public static String getPreferredLocales() {
        MainActivity activity = instance();
        if (activity == null) {
            String fallback = java.util.Locale.getDefault().toLanguageTag();
            Log.i(TAG, "getPreferredLocales: no activity, fallback=" + fallback);
            return fallback;
        }

        try {
            Configuration configuration = activity.getResources().getConfiguration();
            if (Build.VERSION.SDK_INT >= 24) {
                LocaleList locales = configuration.getLocales();
                if (locales != null && !locales.isEmpty()) {
                    StringBuilder out = new StringBuilder();
                    for (int i = 0; i < locales.size(); ++i) {
                        java.util.Locale locale = locales.get(i);
                        if (locale == null) {
                            continue;
                        }
                        if (out.length() > 0) {
                            out.append(',');
                        }
                        out.append(locale.toLanguageTag());
                    }
                    if (out.length() > 0) {
                        String result = out.toString();
                        Log.i(TAG, "getPreferredLocales: " + result);
                        return result;
                    }
                }
            }

            java.util.Locale locale = configuration.locale;
            if (locale != null) {
                String result = locale.toLanguageTag();
                Log.i(TAG, "getPreferredLocales legacy: " + result);
                return result;
            }
        } catch (Throwable error) {
            Log.w(TAG, "getPreferredLocales failed", error);
        }

        String fallback = java.util.Locale.getDefault().toLanguageTag();
        Log.i(TAG, "getPreferredLocales fallback: " + fallback);
        return fallback;
    }

    public static String syncBundledAssets(String targetRootDir) {
        MainActivity activity = instance();
        if (activity == null || targetRootDir == null || targetRootDir.isEmpty()) {
            Log.w(TAG, "syncBundledAssets: invalid activity or targetRootDir");
            return null;
        }

        try {
            File targetRoot = new File(targetRootDir);
            if (!targetRoot.exists() && !targetRoot.mkdirs()) {
                Log.w(TAG, "syncBundledAssets: failed to create targetRoot=" + targetRootDir);
                return null;
            }

            boolean opensbOk = copyAssetTreeIfMissing(activity, "opensb", new File(targetRoot, "opensb"));
            boolean langOk = copyAssetTreeIfMissing(activity, "lang", new File(targetRoot, "lang"));
            boolean fontOk = copyAssetTreeIfMissing(activity, "opensb/hobo.ttf", new File(targetRoot, "hobo.ttf"));
            Log.i(TAG, "syncBundledAssets: root=" + targetRoot.getAbsolutePath()
                + " opensbOk=" + opensbOk
                + " langOk=" + langOk
                + " fontOk=" + fontOk
                + " langExists=" + new File(targetRoot, "lang/zh_CN.lang").isFile()
                + " fontExists=" + new File(targetRoot, "hobo.ttf").isFile());
            if (!opensbOk) {
                return null;
            }
            return targetRoot.getAbsolutePath();
        } catch (Throwable error) {
            Log.w(TAG, "syncBundledAssets failed", error);
            return null;
        }
    }

    private static boolean copyAssetTreeIfMissing(MainActivity activity, String assetPath, File dst) {
        try {
            // Detect file vs directory by attempting to open the asset.
            // getAssets().open() throws FileNotFoundException for directories,
            // but returns a valid stream for files — including leaf assets like
            // "hobo.ttf" or "en_US.lang" that live directly under asset dirs.
            // We must NOT rely on list() return value: it throws for files.
            try (InputStream test = activity.getAssets().open(assetPath)) {
                test.close();
            }
            // It's a file. Copy it if missing.
            if (dst.exists() && dst.isFile()) {
                return true;
            }
            if (dst.isDirectory()) {
                deleteRecursively(dst);
            }
            File parent = dst.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                Log.w(TAG, "copyAssetTreeIfMissing: failed to create parent for file: " + assetPath);
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
            Log.i(TAG, "copyAssetTreeIfMissing: copied file " + assetPath + " -> " + dst.getAbsolutePath());
            return true;
        } catch (java.io.FileNotFoundException fnfe) {
            // Not a file — must be a directory (or invalid path, which the caller won't send).
            try {
                String[] children = activity.getAssets().list(assetPath);
                if (!dst.exists() && !dst.mkdirs()) {
                    return false;
                }
                if (children == null) {
                    return true;
                }
                for (String child : children) {
                    if (!copyAssetTreeIfMissing(activity, assetPath + "/" + child, new File(dst, child))) {
                        return false;
                    }
                }
                return true;
            } catch (java.io.IOException ioe) {
                Log.w(TAG, "copyAssetTreeIfMissing: list failed for directory: " + assetPath, ioe);
                return false;
            }
        } catch (Throwable error) {
            Log.w(TAG, "copyAssetTreeIfMissing failed: assetPath=" + assetPath + " dst=" + dst.getAbsolutePath(), error);
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
            } else if (requestCode == REQUEST_PICK_MOD_PAK) {
                String modsDir;
                synchronized (PICKER_LOCK) {
                    modsDir = sTargetModsDir;
                }
                if (modsDir == null) {
                    return;
                }

                Uri pakUri = data.getData();
                if (pakUri == null) {
                    return;
                }

                File modsDirFile = new File(modsDir);
                if (!modsDirFile.exists() && !modsDirFile.mkdirs()) {
                    return;
                }

                String name = displayName(getContentResolver(), pakUri);
                if (name == null || name.isEmpty()) {
                    name = "mod.pak";
                }
                if (!isPakFileName(name)) {
                    name = name + ".pak";
                }

                File targetPak = uniqueTargetFile(modsDirFile, name, false);
                String imported = copyUriToPath(this, pakUri, targetPak);
                if (imported != null) {
                    synchronized (PICKER_LOCK) {
                        sImportedMods.add(imported);
                    }
                }
            } else if (requestCode == REQUEST_PICK_MOD_FOLDER || requestCode == REQUEST_PICK_MODS_FOLDER) {
                String modsDir;
                synchronized (PICKER_LOCK) {
                    modsDir = sTargetModsDir;
                }
                if (modsDir == null) {
                    return;
                }

                Uri treeUri = data.getData();
                if (treeUri == null) {
                    return;
                }

                ContentResolver resolver = getContentResolver();
                File modsDirFile = new File(modsDir);
                if (!modsDirFile.exists()) {
                    modsDirFile.mkdirs();
                }

                int grantFlags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                try {
                    resolver.takePersistableUriPermission(treeUri, grantFlags);
                } catch (Throwable ignored) {
                }

                DocumentFile pickedTree = DocumentFile.fromTreeUri(this, treeUri);
                if (pickedTree != null && pickedTree.isDirectory()) {
                    ArrayList<String> imported = new ArrayList<>();
                    if (requestCode == REQUEST_PICK_MOD_FOLDER) {
                        importSingleModFolderFromTree(this, pickedTree, modsDirFile, imported);
                    } else {
                        importAllModsFromFolderTree(this, pickedTree, modsDirFile, imported);
                    }
                    synchronized (PICKER_LOCK) {
                        sImportedMods.addAll(imported);
                    }
                }
            } else if (requestCode == REQUEST_PICK_SAVE_ZIP) {
                String saveRoot;
                synchronized (PICKER_LOCK) {
                    saveRoot = sTargetSaveRoot;
                }
                Uri zipUri = data.getData();
                if (zipUri != null && saveRoot != null) {
                    boolean imported = importSaveZipFromUri(this, zipUri, saveRoot);
                    synchronized (PICKER_LOCK) {
                        sSaveImportResult = imported;
                    }
                    Toast.makeText(
                        this,
                        imported ? "Save imported." : "Could not import save zip.",
                        Toast.LENGTH_LONG
                    ).show();
                }
            }
        } finally {
            latch.countDown();
            synchronized (PICKER_LOCK) {
                sLatch = null;
                sTargetPackedPak = null;
                sTargetModsDir = null;
                sTargetSaveRoot = null;
            }
        }
    }
}
