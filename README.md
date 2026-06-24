<div align="center">
  <img src="assets/osbm/osbmPixelArt-8x.png" alt="OpenStarbound Mobile pixel art banner" width="760">
  <h1>OpenStarbound Mobile</h1>
  <p><strong>A mobile-focused port of OpenStarbound for Android and iOS devices.</strong></p>
  <p>
    <a href="https://github.com/RohanBhattacharyya/oSBM/releases">
      <img alt="Latest release" src="https://img.shields.io/github/v/release/RohanBhattacharyya/oSBM?include_prereleases&label=release&style=for-the-badge&color=00d1b2">
    </a>
    <a href="https://github.com/RohanBhattacharyya/oSBM/actions/workflows/android-manual.yml">
      <img alt="Android workflow" src="https://img.shields.io/github/actions/workflow/status/RohanBhattacharyya/oSBM/android-manual.yml?branch=main&label=android%20apk&style=for-the-badge&logo=android&logoColor=white">
    </a>
    <a href="https://github.com/RohanBhattacharyya/oSBM/actions/workflows/ios-manual.yml">
      <img alt="iOS workflow" src="https://img.shields.io/github/actions/workflow/status/RohanBhattacharyya/oSBM/ios-manual.yml?branch=main&label=ios%20ipa&style=for-the-badge&logo=apple&logoColor=white">
    </a>
  </p>
  <p>
    <img alt="Android ARM64" src="https://img.shields.io/badge/Android-ARM64-3ddc84?style=flat-square&logo=android&logoColor=white">
    <img alt="iOS ARM64" src="https://img.shields.io/badge/iOS-ARM64-000000?style=flat-square&logo=apple&logoColor=white">
    <img alt="CMake 3.23 or newer" src="https://img.shields.io/badge/CMake-3.23%2B-064f8c?style=flat-square&logo=cmake&logoColor=white">
    <img alt="Java 17" src="https://img.shields.io/badge/Java-17-f89820?style=flat-square&logo=openjdk&logoColor=white">
  </p>
</div>

You **must** own a copy of Starbound. Base game assets are not provided for very obvious reasons.

The code is worked on whenever I have the spare time. Contributions and issues are welcome!

## Installation
### Download the [latest release](https://github.com/RohanBhattacharyya/oSBM/releases).
The latest stable release is recommended, as the nightly build can have bugs.

At the moment, you must have a copy of the game assets (**packed.pak**) to upload to the app.

## Usage
<details>
<summary>Setting up</summary>

All you have to do is download your **packed.pak** from your game files. Find the root of your Starbound installation, and under the assets/ folder, you will find packed.pak. Send that to your phone through any method, and then you can select that in the launcher by clicking "Pick packed.pak". After selecting it once, you will never have to select it again (unless you delete your data).
</details>

<details>
<summary>Using/configuring controls</summary>

There are many features that oSBM has that you should know about.
- Direct screen touch gestures
- Gyro Aiming
- Aim joystick
- D-Pad
- Gamepad controls and remapping
- etc. in the Controls manager.

### Controls Manager
The Controls manager is a menu in the oSBM launcher that can be accessed by clicking the Controls button in the main menu. Opening it up, you will be presented with many options.

There are touch control settings directly in the Controls manager, and a Gamepad Controls button that opens gamepad-specific settings. For touch controls, there are two sections to the manager: the general toggles at the top, and the configurable buttons/joysticks below. In the top section, you can enable/disable various options about the touch controls.
- **Enable touch overlay:** this toggles the actual usage of all touch controls.
- **Enable direct screen touch gestures:** this toggles if the user can touch the screen anywhere to act as a left click at that location (the virtual mouse is placed at the place of the tap, and a left click occurs). Holding down with one finger where you want to aim, and then tapping or holding down with a second finger on the screen will initiate a right click/hold, occurring at the aimed location of the first finger.
- **Invert gyro X/Y axis:** inverts the axis of the gyro if preferred.

#### Custom Buttons
You can add more custom buttons with some sort of interaction, such Tech, Codex, Chat, Interact, etc. Many of the essential functionalities are already created as buttons in the long list of options, though not all enabled by default.
By clicking on the name of a button, it will open up details about it. In general, there will be the Displayed text, which you can change to change the name of the button and what is displayed on it in-game, the size which is self explanatory, the position, which can be adjusted visually with the Preview / Adjust Layout button at the top, the Interaction that occurs when the button is interacted with, and button behavior, which is **Single press, Rapid fire/repeat, Hold, and Toggle.** Single press runs the interaction once when the button is tapped. Rapid fire repeatedly runs the interaction while the button is held, hold will send the signal that the interaction should be held (typically the one you want), and toggle which is hold but instead of canceling when released it is canceled when it is tapped for the second time.

#### Macros
Macros let one touch button press multiple keyboard keys at the same time. They are useful for shortcuts that normally require a modifier key, such as emotes, menu shortcuts, or any custom keybinds you have set up in Starbound.

To make a macro, open a button in the Controls manager and use the **Custom keys** field under its interaction. Enter each key name separated by `+`, spaces, commas, or semicolons, then press **Apply**. If you enter one valid key, the button becomes a normal single-key action. If you enter multiple valid keys, the button becomes a macro.

Examples:
- `LShift+F1`
- `LCtrl,LShift,Up`
- `LAlt E`
- `1` for action bar slot 1

Key names should match the names used by the game, such as `A`, `Space`, `Return`, `Esc`, `LShift`, `LCtrl`, `LAlt`, `Up`, `Down`, `F1`, or `1`. Invalid key names are ignored, so if a macro does not work, double-check the spelling and capitalization.

The selected button behavior applies to the whole macro:
- **Single press:** briefly presses all keys in the macro once.
- **Rapid fire / repeat:** repeatedly sends that same brief macro press while held.
- **Hold:** holds all keys in the macro down until you release the touch button.
- **Toggle:** holds all keys in the macro down until you tap the touch button again.

Macros are simultaneous key presses, not ordered scripts. For example, `LCtrl+LShift+Up` is treated like holding those three keys together, not pressing them one after another.

</details>
<details>
<summary>How to play</summary>
Just click Launch once the packed.pak has been selected! As long as Enable touch overlay is on, touch controls will show up in-game.
</details>

## Building

<details>
<summary>Android (ARM64 APK)</summary>

#### Requirements
* CMake 3.23+
* Ninja
* Java 17
* Android SDK with:
  * `platform-tools`
  * `platforms;android-35`
  * `build-tools;35.0.0`
  * `ndk;26.3.11579264`
* vcpkg (set `VCPKG_ROOT`)

#### Environment variables
Set these before configuring:

```bash
export VCPKG_ROOT=/absolute/path/to/vcpkg
export ANDROID_SDK_ROOT=/absolute/path/to/android-sdk
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/26.3.11579264"
```

#### Configure and build
From the repository `source/` directory:

```bash
cmake --preset android-arm64-release
cmake --build --preset android-arm64-release --target android_apk --parallel
```

Debug build:

```bash
cmake --preset android-arm64-debug
cmake --build --preset android-arm64-debug --target android_apk --parallel
```

#### Build outputs
* Release APK: `source/application/mobile/android/host/app/build/outputs/apk/release/app-release.apk`
* Debug APK: `source/application/mobile/android/host/app/build/outputs/apk/debug/app-debug.apk`
</details>
<details>
<summary>iOS (ARM64)</summary>

#### Requirements
* macOS (Xcode 15+ recommended)
* Xcode command line tools
* CMake 3.23+
* vcpkg (set `VCPKG_ROOT`)

#### Option A: compile-only check (preset)
From `source/`:

```bash
cmake --preset ios-arm64-debug
cmake --build --preset ios-arm64-debug --target starbound --parallel
```

#### Option B: build unsigned `.ipa` locally (matches CI workflow)
From repository root:

```bash
cmake -S source -B build/ios-arm64-ipa \
  -G Xcode \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/triplets" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_TARGET_TRIPLET=arm64-ios-mixed \
  -DSTAR_ENABLE_STEAM_INTEGRATION=OFF \
  -DSTAR_ENABLE_DISCORD_INTEGRATION=OFF \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_IDENTITY=""
```

Build and package the unsigned `.ipa` in one step using the `ipa` CMake target:

```bash
cmake --build build/ios-arm64-ipa --config Release --parallel --target ipa
```

The `ipa` target:
1. Builds `starbound` (and all dependencies) if out of date
2. Deletes any previous `ipa/Payload/` directory and unsigned `.ipa`
3. Copies the full current app bundle (binary + assets + Info.plist) into `ipa/Payload/OpenStarbound.app/`
4. Zips the result into the final `.ipa`

Do not package a manually staged `Payload` directory. Re-run the `ipa` target
whenever you need a fresh installable build; it is the only step that writes the
final IPA payload.

Resulting file:
* `build/ios-arm64-ipa/ipa/OpenStarbound-iOS-Release-unsigned.ipa`
</details>
<details>
<summary>CI (manual workflows)</summary>

If you prefer GitHub Actions artifacts instead of local packaging:
* Android manual workflow: `.github/workflows/android-manual.yml`
* iOS manual workflow: `.github/workflows/ios-manual.yml`
</details>
