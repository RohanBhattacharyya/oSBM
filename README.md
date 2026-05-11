# OpenStarbound Mobile

<details>
<summary><b>What is this?</b></summary>
 
tl;dr: **OpenStarbound** is a mod of the latest version of Starbound, 1.4.4. It fixes many bugs, adds many new features and improves performance.

By a truly unbelievable coincidence, I was recently out for a walk when I saw a small package fall off a truck ahead of me.  As I got closer, the typeface slowly came into focus: **Starbound**. Inside, I found a submachine gun, a fursuit (tells you something about their audience I guess!), and the latest version of the [**Starbound source code**](https://archive.org/details/starbound_source_code).

![185361129-9883fb92-9597-4ba4-b003-4be3dc4971a3](https://github.com/user-attachments/assets/b35dd133-2dc8-4205-9a3f-788e14192a05)

</details>

You **must** own a copy of Starbound. Base game assets are not provided for very obvious reasons.


The code is worked on whenever I feel like it. Contributions are welcome!

## Installation
### Download the [latest release](https://github.com/RohanBhattacharyya/oSBM/releases).
The latest stable release is recommended, as the nightly build can have bugs.

At the moment, you must have a copy of the game assets (**packed.pak**) to upload to the app.

OpenStarbound is a separate installation/executable than Starbound. You can copy your `storage` folder from Starbound to transfer your save data and settings. Launching OpenStarbound with Steam open will load your subscribed Steam mods.

An installer is available for Windows. otherwise, extract the client/server zip for your platform and copy the game assets (packed.pak) to the OpenStarbound assets folder.

</details>
<details>
<summary><b>Nightly Builds</b></summary>
 
These link directly to the latest build from the [Actions](https://github.com/OpenStarbound/OpenStarbound/actions?query=branch%3Amain) tab.
 
**Windows**
[Installer](https://nightly.link/OpenStarbound/OpenStarbound/workflows/build/main/OpenStarbound-Windows-Installer.zip),
[Client](https://nightly.link/OpenStarbound/OpenStarbound/workflows/build/main/OpenStarbound-Windows-Client.zip),
[Server](https://nightly.link/OpenStarbound/OpenStarbound/workflows/build/main/OpenStarbound-Windows-Server.zip)

**Linux**
[Client](https://nightly.link/OpenStarbound/OpenStarbound/workflows/build/main/OpenStarbound-Linux-Clang-Client.zip),
[Server](https://nightly.link/OpenStarbound/OpenStarbound/workflows/build/main/OpenStarbound-Linux-Clang-Server.zip)

**macOS**
[Intel](https://nightly.link/OpenStarbound/OpenStarbound/workflows/build/main/OpenStarbound-macOS-Intel-Client.zip),
[ARM](https://nightly.link/OpenStarbound/OpenStarbound/workflows/build/main/OpenStarbound-macOS-Silicon-Client.zip)

[All Nightly Builds](https://nightly.link/OpenStarbound/OpenStarbound/workflows/build/main)
</details>

## Changes
Note: Not every function from [StarExtensions](https://github.com/StarExtensions/StarExtensions) has been ported yet, but near-full compatibility with mods that use StarExtensions features is planned.

### Lighting
**The lightmap generation has been moved off the main thread, and supports higher color range.**
  * Point lights are now additive, which is more accurate - you'll notice that different lights mix together better!
  * Object spread lights are auto-converted to a hybrid light which is 25% additive.

### Assets
* Assets can now run Lua scripts on load, and after all sources have been loaded.
  * These scripts can modify, read, patch and create new assets!
* Lua patch files now exist - **.patch.lua**
  * These can patch JSON assets, as well as images!
### Commands
**View OpenStarbound commands with `/help`! You can also view them [here](https://github.com/OpenStarbound/OpenStarbound/blob/main/assets/opensb/help.config.patch)**
  * Changes to vanilla commands:
    * `/settileprotection`
      * You can now specify as many dungeon IDs as you want: `/settileprotection 69 420 false`
      * You can now specify a range: /settileprotection 0..65535 true
### Bug Fixes
* Invalid character inventories are updated when loading in, allowing players to swap inventory mods with pre-existing characters.
* Fix vanilla world file size bloating issue.
* Modifying a single status property no longer re-networks every status property on the entity (server and client must be running at least OpenStarbound 0.15)
### Misc
* Player functions for saving/loading, modifying the humanoid identity, manipulating the inventory. [Documentation](https://github.com/OpenStarbound/OpenStarbound/tree/main/doc/lua/openstarbound)
* Character swapping (rewrite from StarExtensions, currently command-only: `/swap name` case-insensitive, only substring required)
* Custom user input support with a keybindings menu (rewrite from StarExtensions)
* Positional Voice Chat that works on completely vanilla servers, uses Opus for crisp, HD audio (rewrite from StarExtensions)
  * Both menus are made available in the options menu in this fork rather than as a chat command.
* Multiple font support (switch fonts inline with `^font=name;`, **.ttf** and **.woff2** assets are auto-detected)
  * **.woff2** fonts are much smaller than **.ttf**, [here's a web conversion tool](https://kombu.kanejaku.org/)!
* Experimental changes to the storage of directives in memory to reduce copying - can reduce their impact on frametimes when very long directives are present
  * Works especially well when extremely long directives are used for "vanilla multiplayer-compatible" creations, like [generated clothing](https://silverfeelin.github.io/Starbound-NgOutfitGenerator/) or custom items/objects.
* Perfectly Generic Items will retain the data for what item they were if a mod is uninstalled, and will attempt to restore themselves if re-installed.
* Musical instruments have their own volume slider in the options menu.
* Players can use items while lounging

* Client-side tile placement prediction (rewrite from StarExtensions)
  * You can also resize the placement area of tiles on the fly.
* Support for placing foreground tiles with a custom collision type (rewrite from StarExtensions, requires OpenSB server)
  * Additionally, objects can be placed under non-solid foreground tiles.
  * Admin characters have unlimited and unobstructed interaction/placement ranges

* Some minor polish to UI
* The Skybox's sun now matches the system type you're currently in.
  * Previously generated planets will not have this feature and will display the default sun.
  * Modded system types require a patch to display their custom sun.
  * You can also access the skybox sun scale and its default ray colors. For more details see, [sky.config.patch](https://github.com/OpenStarbound/OpenStarbound/blob/main/assets/opensb/sky.config.patch).

**Discord:** To prevent spam, paste these directives after **/render** in-game to obtain the invite code.

<details>
<summary>Directives</summary>
 
```
?crop=0;0;1;1?multiply=0000?replace;0000=4444?border=1;ccc0;0000?scale=.8;.6?replace;0000=00e178;4440=00e188;8881=001e88;9990=001e78?scale=194.50038;1?crop=0;0;195;2?replace;002078=001f78;002088=001f88;002978=002676;002988=002686;002c78=002b79;002c88=002b89;003078=002577;003088=002587;003478=003378;003488=038;003b78=002676;003b88=002686;003c78=002a78;003c88=002a88;003d78=002b78;003d88=002b88;003e78=002b79;003e88=002b89;004178=002f78;004188=002f88;004278=002577;004288=002587;004678=004578;004688=004588;005178=004779;005188=004789;005978=003677;005988=003687;006078=003677;006088=003687;006e78=003677;006e88=003687;007178=007078;007188=007088;007878=007778;007888=078;007b78=007078;007b88=007088;007d78=004577;007d88=004587;007e78=002577;007e88=002587;008278=002b73;008288=002b83;008378=002b74;008388=002b84;008478=002b75;008488=002b85;008578=002b76;008588=002b86;008678=002b77;008688=002b87;008778=002b78;008788=002b88;008878=002b79;008d78=00357a;008d88=00358a;009178=009078;009188=009088;009778=00357a;009788=00358a;009878=008e78;009888=008e88;009978=008f78;009a78=009078;009a88=009088;009b78=009078;009b88=009088;009c78=009278;009c88=009288;009d78=009378;009d88=009388;00a778=004779;00a788=004789;00b378=004578;00b388=004588;00b478=004578;00b488=004588;00c178=00337b;00c188=00338b;00cd78=00cc78;00cd88=0c8;00cf78=00ce78;00cf88=00ce88;00d478=007778;00d488=078;00d578=007778;00d588=078;00da78=004779;00da88=004789;088=002b89;098=008f88;002278=000;002378=000;002388=001;002d78=002;002d88=003;002e78=002;002e88=003;003f78=002;003f88=003;004078=002;004088=003;004878=000;004888=001;004978=000;004988=001;004a78=004;004a88=005;004b78=006;004b88=007;004c78=008;004c88=009;004f78=00a;004f88=00b;005078=00a;005088=00b;005278=000;005288=001;005378=000;005388=001;005478=004;005488=005;005578=006;005678=008;005688=009;005a78=00a;005a88=00b;005f78=00a;005f88=00b;006678=00c;006778=00c;006788=00d;006878=00c;006888=00d;006f78=00a;006f88=00b;007378=002;007388=003;007978=00c;007988=00d;007a78=00c;007a88=00d;008978=002;008988=003;009478=008;009488=009;009e78=008;009e88=009;00a578=00a;00a588=00b;00a678=00a;00a688=00b;00a878=000;00a888=001;00a978=000;00a988=001;00aa78=004;00ab78=006;00ab88=007;00ac78=008;00ac88=009;00b778=008;00b788=009;00b878=006;00b888=007;00b978=004;00b988=005;00ba78=000;00ba88=001;00bb78=000;00bc78=004;00bc88=005;00bd78=006;00bd88=007;00be78=008;00be88=009;00c278=004;00c288=005;00c378=000;00c388=001;00c478=000;00c488=001;00c578=004;00c588=005;00c678=006;00c688=007;00c778=008;00c788=009;00d878=00a;00d888=00b;00d978=00a;00d988=00b;00db78=000;00db88=001;00dc78=000;00dc88=001;00dd78=004;00de78=006;00de88=007;00df78=008;00df88=009;028=001;058=007;068=00d;0a8=005;0b8=001;0d8=005?replace;000=002178;001=002188;002=002b7a;003=002b8a;004=002478;005=002488;006=002578;007=002588;008=002678;009=002688;00a=003578;00b=003588;00c=006578;00d=006588?scale=1;15.5?crop=0;0;195;16?replace;001f79=000;001f7a=000;001f7b=000;001f7c=000;001f7d=000;001f7e=000;001f7f=000;001f80=000;001f81=000;001f82=000;001f83=000;001f84=000;00217d=000;00217e=000;002183=000;002184=000;00247d=000;00247e=000;00247f=000;002482=000;002483=000;002484=000;00257e=000;00257f=000;002580=000;002581=000;002582=000;002583=000;00267f=000;002680=000;002681=000;002682=000;002a80=000;002a81=000;002a82=000;002a83=000;002a84=000;002b7f=000;002b80=000;002b81=000;002f7e=000;002f7f=000;002f80=000;002f81=000;002f82=000;002f83=000;002f84=000;003381=000;003382=000;003385=000;003386=000;00357d=000;00357e=000;00357f=000;003580=000;003581=000;003582=000;003583=000;003584=000;003585=000;003586=000;00367d=000;00367e=000;00367f=000;003680=000;003681=000;003682=000;003683=000;003684=000;003685=000;003781=000;003782=000;003881=000;003882=000;00457d=000;00457e=000;00457f=000;004580=000;004581=000;004582=000;004583=000;004584=000;00477e=000;00477f=000;004783=000;004784=000;005b7d=000;005b7e=000;005c7d=000;005c7e=000;005d7d=000;005d7e=000;005e7d=000;005e7e=000;00637e=000;00637f=000;006380=000;006381=000;006384=000;006385=000;00647d=000;00647e=000;00647f=000;006480=000;006481=000;006482=000;006484=000;006485=000;006486=000;00657d=000;00657e=000;006581=000;006582=000;006585=000;006586=000;00697d=000;00697e=000;00697f=000;006981=000;006982=000;006983=000;006984=000;006985=000;006986=000;006a7e=000;006a7f=000;006a82=000;006a83=000;006a84=000;006a85=000;006d85=000;006d86=000;00707d=000;00707e=000;007085=000;007086=000;00727d=000;00727e=000;00747e=000;00747f=000;00777d=000;00777e=000;00777f=000;007780=000;007781=000;007782=000;007785=000;007786=000;007c7d=000;007c7e=000;007c7f=000;007c84=000;007c85=000;007c86=000;008185=000;008186=000;008a7d=000;008a7e=000;008e7a=000;008e7b=000;008e7c=000;008e7d=000;008e7e=000;008e7f=000;008e80=000;008e81=000;008e82=000;008e83=000;008e84=000;008f79=000;008f7a=000;008f7b=000;008f7d=000;008f7e=000;008f82=000;008f83=000;009079=000;00907a=000;00907d=000;00907e=000;009083=000;009084=000;009279=000;00927a=000;00927d=000;00927e=000;00927f=000;009282=000;009283=000;009284=000;009379=000;00937a=000;00937e=000;00937f=000;009380=000;009381=000;009382=000;009383=000;00a17d=000;00a17e=000;00a27d=000;00a27e=000;00af82=000;00af83=000;00b082=000;00b083=000;00b181=000;00b182=000;00b281=000;00b282=000;00ca83=000;00ca84=000;00cb7e=000;00cb7f=000;00cb80=000;00cb83=000;00cb84=000;00cc7d=000;00cc7e=000;00cc7f=000;00cc80=000;00cc81=000;00cc83=000;00cc84=000;00ce7d=000;00ce7e=000;00ce80=000;00ce81=000;00ce82=000;00ce83=000;00ce84=000;00d07d=000;00d07e=000;00d081=000;00d082=000;00d083=000;00d17d=000;00d17e=000?multiply=fff0?replace;0000=fff
```

</details>

</details>


## Building (Mobile)

Desktop compilation instructions were intentionally removed from this README.
This section documents the supported mobile targets: Android and iOS.

### Android (ARM64 APK)

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

### iOS (ARM64)

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

cmake --build build/ios-arm64-ipa --config Release --parallel --target starbound
```

Package unsigned `.ipa`:

```bash
APP_PATH="$(find build/ios-arm64-ipa dist -type d -name '*.app' | head -n 1)"
IPA_DIR="build/ios-arm64-ipa/ipa"
rm -rf "$IPA_DIR"
mkdir -p "$IPA_DIR/Payload"
cp -R "$APP_PATH" "$IPA_DIR/Payload/$(basename "$APP_PATH")"
(
  cd "$IPA_DIR"
  zip -qry "OpenStarbound-iOS-Release-unsigned.ipa" Payload
)
```

Resulting file:
* `build/ios-arm64-ipa/ipa/OpenStarbound-iOS-Release-unsigned.ipa`

### CI (manual workflows)
If you prefer GitHub Actions artifacts instead of local packaging:
* Android manual workflow: `.github/workflows/android-manual.yml`
* iOS manual workflow: `.github/workflows/ios-manual.yml`
