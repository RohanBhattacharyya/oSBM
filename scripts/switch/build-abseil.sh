#!/usr/bin/env bash
# Build the static Abseil prerequisite without changing an installed SDK.
set -euo pipefail
: "${DEVKITPRO:?set DEVKITPRO to the patched Switch SDK}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${1:?usage: build-abseil.sh <isolated-work-directory>}"
VERSION=20260107.1
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
if [[ ! -d "$WORK/abseil-cpp-$VERSION" ]]; then
  curl -fL --retry 3 "https://github.com/abseil/abseil-cpp/archive/refs/tags/$VERSION.tar.gz" -o "$WORK/source.tar.gz"
  echo "4314e2a7cbac89cac25a2f2322870f343d81579756ceff7f431803c2c9090195  $WORK/source.tar.gz" | sha256sum -c -
  tar -xzf "$WORK/source.tar.gz" -C "$WORK"
fi
ABSEIL_SOURCE="$WORK/abseil-cpp-$VERSION"
printf 'set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -I%s/toolchains/switch/include")\n' "$REPO_ROOT" > "$WORK/compat.cmake"
PATCH="$REPO_ROOT/toolchains/patches/abseil-switch.patch"
if git -C "$ABSEIL_SOURCE" apply --check "$PATCH" 2>/dev/null; then
  git -C "$ABSEIL_SOURCE" apply "$PATCH"
else
  git -C "$ABSEIL_SOURCE" apply --reverse --check "$PATCH"
fi
cmake -S "$ABSEIL_SOURCE" -B "$WORK/build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_INSTALL_PREFIX="$WORK/install" \
  -UCMAKE_CXX_FLAGS -DCMAKE_PROJECT_INCLUDE="$WORK/compat.cmake" \
  -DABSL_BUILD_TESTING=OFF -DABSL_ENABLE_INSTALL=ON \
  -DABSL_PROPAGATE_CXX_STD=ON -DBUILD_SHARED_LIBS=OFF
cmake --build "$WORK/build" --target install --parallel "${OSBM_BUILD_JOBS:-2}"
echo "Switch Abseil prefix: $WORK/install"
