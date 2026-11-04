#!/system/bin/sh
# Diagnostic variant only; Android 14+ supplies the HWASan runtime.
REPORTS=/data/data/com.devoflife.osbm.android/files/hwasan
mkdir -p "$REPORTS"
export HWASAN_OPTIONS="log_path=$REPORTS/report:halt_on_error=1"
LD_HWASAN=1 exec "$@"
