#!/usr/bin/env python3
"""Verifica C++ modificado com NDK real e compara avisos com a base do PR."""
import argparse
import collections
import concurrent.futures
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import time

parser = argparse.ArgumentParser(description=__doc__)
for option in ("work-dir", "ndk", "zig", "sdl", "imgui", "glew"):
    parser.add_argument("--" + option, type=Path, required=True)
parser.add_argument("--baseline", default="a8cb06177b0efd48c4d4ab827786ec61edea3bfa")
parser.add_argument("--jobs", type=int, default=2)
parser.add_argument("--fmt", type=Path)
parser.add_argument("--xxhash", type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
work = args.work_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
os.environ["ZIG_GLOBAL_CACHE_DIR"] = str(work / "zig-cache")
base = work / ("baseline-" + args.baseline[:12])
if not (base / "source").exists():
    base.mkdir(exist_ok=True)
    raw = subprocess.check_output(["git", "archive", args.baseline, "source"], cwd=repo)
    with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
        archive.extractall(base, filter="data")
files = subprocess.check_output(["git", "diff", "--name-only", args.baseline], cwd=repo, text=True).splitlines()
files = [p for p in files if p.endswith(".cpp") and not p.startswith("source/test/")]
ndk = args.ndk.resolve() / "toolchains/llvm/prebuilt/windows-x86_64"
run_id = str(time.time_ns())


def command(tree, variant):
    src = tree / "source"
    if variant == "android-arm64":
        cmd = [str(ndk / "bin/clang++.exe"), "--target=aarch64-linux-android28",
               "--sysroot=" + str(ndk / "sysroot"), "-DSTAR_PLATFORM_MOBILE",
               "-DSTAR_SYSTEM_FAMILY_MOBILE", "-DSTAR_SYSTEM_ANDROID", "-DSTAR_SYSTEM_FAMILY_UNIX",
               "-DSTAR_SYSTEM_LINUX", "-fsyntax-only"]
    else:
        cmd = [str(args.zig.resolve()), "c++", "-DSTAR_SYSTEM_WINDOWS", "-DSTAR_SYSTEM_FAMILY_WINDOWS",
               "-DSTAR_PLATFORM_PC", "-c", "-Wno-nullability-completeness"]
        if variant == "windows-launcher":
            cmd.append("-DSTAR_PLATFORM_MOBILE")
    # O define muda a chave do cache do Zig: diagnósticos precisam ser reemitidos.
    cmd += ["-std=c++17", "-Wall", "-Wextra", "-DFMT_HEADER_ONLY=1", "-DSTAR_TOUCH_VALIDATION_RUN=" + run_id]
    for folder in (args.fmt / "include" if args.fmt else None, args.xxhash):
        if folder:
            cmd += ["-I", str(folder.resolve())]
    for folder in sorted(src.rglob("*")):
        if folder.is_dir() and any(folder.glob("*.hpp")):
            cmd += ["-I", str(folder)]
    for folder in (src / "extern", args.sdl / "include", args.sdl / "src/video/khronos",
                   args.imgui, args.imgui / "backends", args.imgui / "misc/freetype", args.glew / "include"):
        cmd += ["-I", str(folder.resolve())]
    return cmd


commands = {(tree, variant): command(tree, variant) for tree in (base, repo)
            for variant in ("android-arm64", "windows-launcher", "windows-desktop")}


def compile_one(job):
    file, variant = job
    label = Path(file).stem + "." + variant
    results = []
    for tree, tag in ((base, "base"), (repo, "current")):
        cmd = commands[tree, variant] + [str(tree / file)]
        if variant != "android-arm64":
            cmd += ["-o", str(work / (label + "." + tag + ".obj"))]
        result = subprocess.run(cmd, capture_output=True, text=True)
        (work / (label + "." + tag + ".command.json")).write_text(json.dumps(cmd), encoding="utf-8")
        (work / (label + "." + tag + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        results.append((result.returncode, collections.Counter(re.findall(r"warning: (.*)", result.stderr))))
    new = results[1][1] - results[0][1]
    row = dict(file=file, variant=variant, baseExit=results[0][0], exit=results[1][0],
               baseWarnings=sum(results[0][1].values()), warnings=sum(results[1][1].values()), newWarnings=dict(new))
    print(f'{variant}: {Path(file).name}: exit={row["exit"]}, warnings={row["warnings"]}, '
          f'baseline={row["baseWarnings"]}, new={sum(new.values())}', flush=True)
    return row


jobs = [(f, v) for f in files for v in ("android-arm64", "windows-launcher", "windows-desktop")
        if v != "windows-desktop" or "/mobile/" not in f]
with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
    rows = list(pool.map(compile_one, jobs))
(work / "results.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
failures = sum(bool(row["exit"] or row["baseExit"] or row["newWarnings"]) for row in rows)
print(f"Matrix: {len(rows)} checks, failures={failures}", flush=True)
raise SystemExit(bool(failures))
