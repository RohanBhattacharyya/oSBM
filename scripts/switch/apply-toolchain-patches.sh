#!/usr/bin/env bash
# Applies the repo's required devkitPro library patches (see
# toolchains/patches/README.md), builds them, and installs into $DEVKITPRO.
set -euo pipefail

: "${DEVKITPRO:?set DEVKITPRO to your devkitpro root (e.g. /opt/devkitpro)}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PATCHES="$REPO_ROOT/toolchains/patches"
WORK="${1:-$HOME/.cache/oSBM-toolchain-patches}"
mkdir -p "$WORK"

echo "== libdrm_nouveau (bufctx ref dedup) =="
if [ ! -d "$WORK/libdrm_nouveau" ]; then
  git clone --depth 1 https://github.com/devkitPro/libdrm_nouveau "$WORK/libdrm_nouveau"
fi
cd "$WORK/libdrm_nouveau"
git checkout -- . && git apply "$PATCHES/libdrm_nouveau-bufctx-ref-dedup.patch"
make -j"$(nproc)"
cp lib/libdrm_nouveau.a "$DEVKITPRO/portlibs/switch/lib/libdrm_nouveau.a"

echo "== SDL3 (switch touch events + blocking swkbd) =="
if [ ! -d "$WORK/SDL" ]; then
  git clone https://github.com/devkitPro/SDL "$WORK/SDL"
fi
cd "$WORK/SDL"
git checkout -- . \
  && git apply "$PATCHES/sdl3-switch-touch-events.patch" \
  && git apply "$PATCHES/sdl3-switch-swkbd-blocking.patch"
cmake -B build-switch \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=ON -DSDL_SHARED=OFF
cmake --build build-switch -j"$(nproc)" --target SDL3-static
cp build-switch/libSDL3.a "$DEVKITPRO/portlibs/switch/lib/libSDL3.a"

echo "Done. IMPORTANT: delete dist/starbound.elf before the next game build"
echo "so ninja relinks against the updated libraries."
