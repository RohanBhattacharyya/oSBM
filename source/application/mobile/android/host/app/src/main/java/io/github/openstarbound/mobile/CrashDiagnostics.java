package io.github.openstarbound.mobile;

import android.annotation.TargetApi;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.os.Build;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Export the app's own exit records; no root, logcat permission or network. */
@TargetApi(30)
final class CrashDiagnostics {
    private static final int MAX_RECORDS = 3;
    private static final int MAX_TRACE_BYTES = 4 * 1024 * 1024;

    private CrashDiagnostics() {}

    static void write(ActivityManager manager, String packageName, ZipOutputStream zip) throws IOException {
        StringBuilder summary = new StringBuilder();
        try {
            List<ApplicationExitInfo> records = manager.getHistoricalProcessExitReasons(packageName, 0, MAX_RECORDS);
            summary.append("records=").append(records.size()).append('\n');
            for (int i = 0; i < records.size() && i < MAX_RECORDS; ++i) {
                ApplicationExitInfo record = records.get(i);
                summary.append("exit=").append(i)
                    .append(" timestamp=").append(record.getTimestamp())
                    .append(" reason=").append(record.getReason())
                    .append(" status=").append(record.getStatus())
                    .append(" description=").append(record.getDescription()).append('\n');
                // Android 12+ returns native tombstones as protobuf, not text.
                if (Build.VERSION.SDK_INT < 31 || record.getReason() != ApplicationExitInfo.REASON_CRASH_NATIVE)
                    continue;
                byte[] trace;
                try (InputStream input = record.getTraceInputStream()) {
                    if (input == null) {
                        summary.append("trace=").append(i).append(" unavailable\n");
                        continue;
                    }
                    trace = readBounded(input, MAX_TRACE_BYTES);
                } catch (IOException | RuntimeException error) {
                    summary.append("trace=").append(i).append(" unavailable: ").append(error).append('\n');
                    continue;
                }
                writeEntry(zip, "crashes/native-" + i + ".pb", trace);
                summary.append("trace=").append(i).append(" bytes=").append(trace.length).append('\n');
            }
        } catch (RuntimeException error) {
            summary.append("exit history unavailable: ").append(error).append('\n');
        }
        writeEntry(zip, "crashes/exits.txt", summary.toString().getBytes(StandardCharsets.UTF_8));
    }

    // Buffer first so a failed or oversized read never leaves a partial ZIP entry.
    static byte[] readBounded(InputStream input, int limit) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = input.read(buffer)) != -1) {
            if (count > limit - output.size())
                throw new IOException("Native trace exceeds " + limit + " bytes");
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }

    private static void writeEntry(ZipOutputStream zip, String name, byte[] bytes) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(bytes);
        zip.closeEntry();
    }
}
