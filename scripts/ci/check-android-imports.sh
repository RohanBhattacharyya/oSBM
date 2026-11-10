#!/usr/bin/env bash
set -euo pipefail
if (( $# == 0 )); then
  echo "usage: check-android-imports.sh <apk>..." >&2
  exit 1
fi
READELF="${READELF:-${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME or READELF}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
for APK in "$@"; do
  unzip -Z1 "$APK" > "$WORK/entries"
  if ! grep -E '^lib/[^/]+/libmain\.so$' "$WORK/entries" > "$WORK/libraries"; then
    echo "No native libraries in $APK" >&2
    exit 1
  fi
  while IFS= read -r LIBRARY; do
    unzip -p "$APK" "$LIBRARY" > "$WORK/libmain.so"
    "$READELF" --dyn-syms --wide "$WORK/libmain.so" > "$WORK/symbols"
    # minSdk is 26; Bionic's iconv API starts at 28. SDL must use its
    # bundled implementation, or old Android releases fail before app startup
    # (#49). Defined/static iconv symbols are fine; only imports are rejected.
    if awk '$7 == "UND" { print $8 }' "$WORK/symbols" | grep -E '^iconv(_open|_close)?(@.*)?$'; then
      echo "API-28 iconv import in $APK ($LIBRARY)" >&2
      exit 1
    fi
    echo "PASS Android iconv imports: $APK ($LIBRARY)"
  done < "$WORK/libraries"
done
