package io.github.openstarbound.mobile;

import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.ResolveInfo;
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

import androidx.core.content.FileProvider;
import androidx.documentfile.provider.DocumentFile;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class MainActivity extends SDLActivity {
    private static final int REQUEST_PICK_PAK = 0x5001;
    private static final int REQUEST_PICK_MOD_PAK = 0x5002;
    private static final int REQUEST_PICK_MOD_FOLDER = 0x5003;
    private static final int REQUEST_PICK_MODS_FOLDER = 0x5004;
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

    private static String[] runModImportRequest(MainActivity activity, String modsDirectory, int requestCode, Intent intent) {
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

    public static boolean openModsDirectory(String modsDirectory) {
        MainActivity activity = instance();
        if (activity == null) {
            return false;
        }

        String resolvedModsDir = resolveModsDirectory(modsDirectory);
        if (resolvedModsDir == null || resolvedModsDir.isEmpty()) {
            return false;
        }

        File modsDirFile = new File(resolvedModsDir);
        if (!modsDirFile.exists() && !modsDirFile.mkdirs()) {
            return false;
        }

        activity.runOnUiThread(() -> {
            try {
                Uri documentUri = externalStorageDocumentUriForPath(modsDirFile.getAbsolutePath());

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
                    modsDirFile
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
                Uri fallbackUri = externalStorageDocumentUriForPath(modsDirFile.getAbsolutePath());
                if (Build.VERSION.SDK_INT >= 26 && fallbackUri != null) {
                    fallback.putExtra(DocumentsContract.EXTRA_INITIAL_URI, fallbackUri);
                }
                activity.startActivity(fallback);
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
