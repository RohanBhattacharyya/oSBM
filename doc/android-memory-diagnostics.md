# Android world-entry memory investigation

The WA0077 diagnostic export confirms source commit `300c3e7` still fails
when entering a newly created character's world. This is not a confirmed fix.
The first recorded fault is in `WorldServer::update`, through
`LiquidCellEngine<unsigned char>::workingCell`, inserting into its stable
cell map. Local Release symbol offsets match the named functions in the trace.

The original ARM64 signal context, recovered from the later abort's signal
stack dump, has PC `libmain.so+0x23dc3d8`, fault address/x8 `0x44700000`.
The faulting instruction stores through a hash-bucket predecessor pointer.
This establishes an invalid pointer; it does not establish which earlier
write corrupted it, or that the liquid algorithm or rpmalloc caused it.

Asset-free Windows probes exercised the actual cell engine for 4,000 updates
with a small in-memory world, and a separate stable-map probe exercised
insertion, extraction, erasure and reuse. These passed. The cell probe also
passed with the repository's rpmalloc and Android cache-limit macros.
The probes use isolated diagnostic exception/output stubs and a deterministic
random stub; they do not reproduce the full Android game or its threading.

## Optional HWASan APK

This variant is exclusively for an ARM64 Android 14+ test device. The normal
APK and supported device range are unchanged. Follow the Android NDK's
[HWASan guide](https://developer.android.com/ndk/guides/hwasan).

Configure a **separate build directory** using `android-arm64-release`, with:

```
-DSTAR_ANDROID_HWASAN=ON
-DCMAKE_ANDROID_STL_TYPE=c++_shared
-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
-DCMAKE_BUILD_TYPE=RelWithDebInfo
```

Build `starbound`, then run Gradle with `STAR_ANDROID_HWASAN=1`,
`STAR_ANDROID_ABIS=arm64-v8a`, and `:app:assembleHwasan`.
Use the same existing `STAR_SIGNING_KEYSTORE` as the installation being tested.
This does not generate or rotate a key. A different certificate cannot update
an existing installation; export saves before any installation change.

HWASan needs the system allocator and shared libc++, so this opt-in build
disables the custom allocators. It instruments engine C/C++ with HWASan,
preserves frame pointers and line tables, and leaves the sanitizer's signal
handlers intact. Dependencies are not instrumented by this option.
Native libraries go to the ignored `src/hwasan/jniLibs` tree and cannot
replace the ordinary `src/main/jniLibs` build output. The debuggable diagnostic
variant alone includes `wrap.sh`; normal Release/Debug variants do not.

After a failure, restart the launcher and export diagnostics. The ZIP also
includes reports from the app-private `files/hwasan` directory. If this build
opens the world successfully, that is an isolation result, not proof of a
specific root cause: allocator, optimization and instrumentation differ.
Device reproduction is still required before declaring the crash resolved.

## Validation on the Windows build host

- Android arm64 configure and complete `starbound` HWASan build: passed.
- Gradle `:app:assembleHwasan`: passed (35 tasks, including Java compilation).
- APK signature verification, ZIP integrity, arm64-only libraries, shared C++,
  debuggable manifest, LF-only HWASan wrapper and absence of `packed.pak`: passed.
- ELF dynamic dependencies include HWASan and shared libc++; a GNU build ID
  is emitted for matching future crash reports to exact native symbols.
- Normal `:app:assembleRelease --dry-run` with diagnostics disabled: passed;
  this is a configuration check, not another complete normal APK build.
- `git diff --check`: passed.

No device execution of this diagnostic APK, full engine test suite, or new
iOS/Switch/desktop package build was performed. This changes diagnostic
collection and build tooling, not the liquid algorithm or save format.
