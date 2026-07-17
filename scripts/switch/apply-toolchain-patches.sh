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

echo "== libgcc (NOP the CHKFEAT-guarded GCS unwinder path) =="
# gcc 16's aarch64 unwinder probes for the Guarded Control Stack feature with
# `mov x16,#1; chkfeat x16; cbnz x16, skip`. Real hardware (Cortex-A57)
# executes the unallocated hint as a NOP (x16 stays 1 => GCS skipped), but
# Ryujinx mis-implements the hint as "feature present", then faults on the
# unsupported `mrs gcspr_el0` read -- every C++ throw crashes under the
# emulator. NOPing the (already guarded) chkfeat forces the skip everywhere;
# Horizon has no GCS, so this changes nothing on real hardware.
# NOTE: patch EVERY multilib variant -- -fPIE selects pic/libgcc.a, not the
# top-level archive.
GCCLIBDIR="$(dirname "$("$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc" -print-libgcc-file-name)")/.."
python3 - "$GCCLIBDIR" << 'PYEOF'
import sys, glob, os
root = os.path.abspath(sys.argv[1])
chk = bytes.fromhex("1f2503d5")   # chkfeat x16
mrs = bytes.fromhex("21253bd5")   # mrs x1, gcspr_el0
nop = bytes.fromhex("1f2003d5")   # nop
for p in glob.glob(os.path.join(root, "**", "libgcc.a"), recursive=True):
    data = bytearray(open(p, "rb").read())
    n = 0
    i = 0
    while True:
        i = data.find(chk, i)
        if i < 0:
            break
        if bytes.fromhex("300080d2") in bytes(data[max(0, i - 16):i]):  # mov x16,#1
            data[i:i + 4] = nop
            n += 1
        i += 4
    # The mrs sites are unreachable once chkfeat is a NOP, but Ryujinx's JIT
    # rejects the unknown register at TRANSLATION time (whole-block), so the
    # dead instructions must go too.
    m = data.count(mrs)
    if m:
        data = bytearray(bytes(data).replace(mrs, nop))
    if n or m:
        open(p, "wb").write(bytes(data))
    print(f"patched {n} chkfeat + {m} gcspr site(s) in {p}")
PYEOF

echo "Done. IMPORTANT: delete dist/starbound.elf before the next game build"
echo "so ninja relinks against the updated libraries."
