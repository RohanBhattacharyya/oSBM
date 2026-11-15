#!/usr/bin/env bash
# Build the native zlib-ng API alongside the SDK's compatibility zlib.
set -euo pipefail
: "${DEVKITPRO:?set DEVKITPRO to the patched Switch SDK}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
WORK="${1:?usage: build-zlib-ng.sh <isolated-work-directory>}"
VERSION=2.3.3
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
if [[ ! -f "$WORK/source.tar.gz" ]]; then
  curl -fL --retry 3 "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/$VERSION.tar.gz" -o "$WORK/source.tar.gz"
fi
echo "e2057c764f1d5aaee738edee7e977182c5b097e3c95489dcd8de813f237d92a05daaa86d68d44b331d9fec5d1802586a8f6cfb658ba849874aaa14e72a8107f5  $WORK/source.tar.gz" | sha512sum -c -
if [[ ! -d "$WORK/zlib-ng-$VERSION" ]]; then
  tar -xzf "$WORK/source.tar.gz" -C "$WORK"
fi
cmake -S "$WORK/zlib-ng-$VERSION" -B "$WORK/build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$WORK/install" \
  -DBUILD_SHARED_LIBS=OFF -DZLIB_COMPAT=OFF -DWITH_GZFILEOP=ON \
  -DZLIB_ENABLE_TESTS=OFF -DWITH_NEW_STRATEGIES=ON \
  -DWITH_RUNTIME_CPU_DETECTION=OFF
cmake --build "$WORK/build" --target install --parallel "${OSBM_BUILD_JOBS:-2}"
echo "Switch zlib-ng prefix: $WORK/install"
