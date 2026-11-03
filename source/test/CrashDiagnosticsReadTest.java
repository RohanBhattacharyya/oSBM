package io.github.openstarbound.mobile;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Arrays;

/** Host JVM test: compile with CrashDiagnostics.java and the SDK's android.jar. */
public final class CrashDiagnosticsReadTest {
    private static int checks;

    private static void check(boolean condition) {
        ++checks;
        if (!condition)
            throw new AssertionError("Check " + checks + " failed");
    }

    public static void main(String[] args) throws Exception {
        check(CrashDiagnostics.readBounded(new ByteArrayInputStream(new byte[0]), 0).length == 0);
        for (int size : new int[] {1, 8191, 8192, 8193, 4 * 1024 * 1024}) {
            byte[] bytes = new byte[size];
            Arrays.fill(bytes, (byte) 0xa5);
            check(Arrays.equals(bytes, CrashDiagnostics.readBounded(new ByteArrayInputStream(bytes), size)));
            try {
                CrashDiagnostics.readBounded(new ByteArrayInputStream(bytes), size - 1);
                throw new AssertionError("Oversized trace accepted");
            } catch (IOException expected) {
                check(expected.getMessage().contains("exceeds"));
            }
        }
        try {
            CrashDiagnostics.readBounded(new InputStream() {
                @Override public int read() throws IOException { throw new IOException("read failure"); }
            }, 4096);
            throw new AssertionError("Read failure swallowed");
        } catch (IOException expected) {
            check(expected.getMessage().equals("read failure"));
        }
        System.out.println("Passed " + checks + " bounded crash trace read checks (host JVM)");
    }
}
