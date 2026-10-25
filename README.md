# NFSMW Recompiled

A static native recompilation of **Need for Speed: Most Wanted (2005)**, Xbox 360,
built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

> Reference title ID: `454107D9`

This is **not an emulator**. The PowerPC code inside the game's `default.xex` is
translated ahead of time into C++, then compiled into a native x86-64 binary. There
is no JIT and no instruction interpreter at runtime — the game's own logic runs as
native code. What the SDK provides is everything *around* that: the Xbox 360 kernel
calls, the filesystem, audio, input, and a translation of the Xenos GPU to Direct3D 12.

**You need your own copy of the game.** This repository contains no game data, no
`default.xex`, no generated C++, and no compiled binary — and it never will. See
[Legal](#legal).

## This fork

This is a fork of [madelrandel-blip/NFSMW-Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled)
that adds an **Android port** (arm64, Vulkan or OpenGL ES), an **iOS port** (iPhone, Vulkan
over MoltenVK, built and signed from Windows without a Mac) and performance work on the
Xenos GPU path. See [docs/android.md](docs/android.md), [ios/README.md](ios/README.md),
[docs/pipeline-nativo.md](docs/pipeline-nativo.md) and the [CHANGELOG](CHANGELOG.md).
Everything it changes in the ReXGlue SDK is in [sdk/](sdk/): one diff to read, generated
from the patch scripts in `tools/`.

> **This is a vibe-coded project.** The Android and iOS ports and the changes in this fork were
> written with an AI coding assistant (Claude Code), directed, tested on real hardware and
> accepted by a human. The documentation records what was measured and what was not; trust
> the numbers in it, not the confidence of any comment. Review the code before reusing it.

---

## Status

Playable, with rough edges. Known to run start to finish through the prologue and
into free roam on an Intel Iris 540 (a weak integrated GPU) at roughly 18–38 FPS
at 854×480. On Android it is playable on recent Snapdragon phones with the native
engine, including races between two phones over the local network, and the same engine
runs on an iPhone 13 Pro.

| Area | State |
|---|---|
| Boot, menus, free roam | Working |
| Audio | Working. A decoder deadlock that killed sound and froze the game on returning to the menu is fixed — see [docs/diario/audio-cuelgue.md](docs/diario/audio-cuelgue.md) |
| Graphics (D3D12) | Working. Both EDRAM emulation paths are selectable; the fast one roughly doubles the frame rate on integrated GPUs |
| Graphics (Vulkan) | Compiles and loads, renders black on Intel. Untested elsewhere |
| Controller and keyboard | Working |
| V-sync and frame limiting | Working, via a local patch — neither exists in the stock SDK |
| Internal resolution scaling | Working, up to 4× |
| Save games | Working |
| Multiplayer (LAN) | **Working on Android, native engine; on iOS too.** The 360 version has no System Link, only "Xbox Live" against EA's 2005 servers; a PC on the same Wi-Fi stands in for them with [nfsmw-server-lan](https://github.com/victorgbd/nfsmw-server-lan), and the app takes its IP in the "LAN server" setting. A phone can be that server itself ("Host on this phone", Android or iPhone, also with the app in the background), and "Search the network" finds it. Quick Match works end to end: create, find and join a game, the game room, a full race between two phones (peer to peer over UDP) and switching hosts. Tested between a Snapdragon 8 Elite and a Mali-G57 phone, PAL Spain edition; the other editions build but are untested online. Gamertags are checked against the server so two phones can't join with the same one. Stats and leaderboards are in progress (circuit only). See [docs/diario/red-y-privilegios.md](docs/diario/red-y-privilegios.md) |
| Android | **Playable.** Two engines: the Xenos path of this fork and the native renderer that started as [codepdbh/nfsmw-android](https://github.com/codepdbh/nfsmw-android) (`nativo/`, now maintained here), which is the one recommended. The native engine draws with Vulkan or, for Mali and older phones, OpenGL ES 3.2; Android 8.0 or newer. One APK carries six editions of the game (PAL Spain, USA, Japan, PAL Germany, Italy and UK); PAL Spain and USA have been played on phones, the rest build but are untested. Touch controls in races and menus driven by touch, physical controllers, tilt steering, graphics presets, saved games export and import. Launcher in ten languages. See [docs/android.md](docs/android.md) and [docs/motor-nativo.md](docs/motor-nativo.md) |
| iOS | **Playable on iPhone.** The same native engine over MoltenVK (Vulkan on Metal), built and signed from Windows with [xtool](https://github.com/xtool-org/xtool), no Mac. Tested on an iPhone 13 Pro (iOS 26) with a free Apple ID: menus, races, touch controls, the launcher and the LAN host, including in the background. Saved games export and import (the same `.zip` as Android) build but are untested on the phone. The app carries one edition, PAL Spain. See [ios/README.md](ios/README.md) |

### Current versions

**Android 0.5.5.** What changed since 0.5.4:

- **Android 8.0 is now the minimum** (it was 10), to reach the phones with OpenGL ES 3.2
  that stayed on Android 8 or 9 (Galaxy S7, S8 and Note 8, Snapdragon 820 and 835, Pixel 1).
  Untested on Android 8 or 9.
- **Finding the ISO.** With access to all files, the app searches the internal storage and
  the SD card itself and lists only the ISOs the APK can play, with their edition. Some
  phones' file picker does not show the internal storage at all. Untested on a phone.
- **Saved games export and import** to a `.zip`, the same one as on iOS. Untested on a
  phone.
- **Fixed on Mali with Vulkan:** the black rear-view mirror, the black strip on the
  horizon of the forest track and the doubled menu video. They were synchronization bugs
  of ours that Adreno's driver forgives; the Vulkan validation layer found them on a
  Galaxy A22 (Mali-G57), and with the fix it reports none.
- **Fixed: the game closed shortly after the first in-race cutscene.** The game copies
  memory over overlapping ranges, and Android's `memcpy` on arm64 does not copy them the
  way the game's own does. Checked on an AYN Thor: past the cutscene and seven minutes of
  racing.
- **Launcher:** a **My settings** preset brings back your own settings after **Battery
  saver** or **Factory settings**; **Display** is its own section; **Graphics API** is the
  first option in Graphics; the app is now called **NFSMW**.
- **Shadows are drawn every frame.** The "every 2 frames" option is gone: it left the
  shadow map a frame behind the car and made the shadows shake.
- For debugging, APKs with the Vulkan validation layer: `apk_ediciones.py --capas-vulkan`.

**iOS 0.1.0**, the first version: the native engine on the iPhone over MoltenVK 1.4.1,
iOS 16 or newer. It has the launcher in ten languages, touch controls, phone rumble, the
FPS counter, LAN multiplayer with the host on the phone (also in the background) and
saved games export and import. The colored squares in the sky and the squares without
shadow around the car, both from MoltenVK, are fixed.

Everything, version by version: the [CHANGELOG](CHANGELOG.md) (Spanish) and, for iOS,
[ios/README.md](ios/README.md).

## What you need

- Windows 10 or 11, x64
- Visual Studio 2022 Build Tools (MSVC + Windows SDK)
- CMake 3.28+, Ninja, Clang 20+
- Python 3.10+
- A copy of the ReXGlue SDK checked out next to this repository
- Your own ISO or GOD dump of Need for Speed: Most Wanted for Xbox 360

Full setup instructions: [docs/00-entorno.md](docs/00-entorno.md) (Spanish).

## Build

```bat
tools\bootstrap.ps1          :: clones and builds the ReXGlue SDK into ..\rexglue-sdk
EXTRAER_XEX.bat              :: pulls default.xex out of your ISO into assets\
CONSTRUIR.bat                :: patches the SDK, recompiles it, builds the game, packages build\
```

`CONSTRUIR.bat` is the whole pipeline. It applies every patch this project carries,
rebuilds the SDK (that is where the fixes live), runs the code generator, compiles the
game, assembles a portable `build\` folder, verifies that folder is self-contained, and
finally writes two ready-to-send folders into `..\build release\`:

- `NFSMW Windows x64\` — playable, with the game executable inside. Zip it and send it
  to someone who owns the game; **never** publish it (see [Legal](#legal)).
- `NFSMW Windows x64 - Portable\` — everything except the game executable. This one is
  safe to publish.

The ISO is left out of both. The script refuses to finish if anything that looks like
game data ends up in the publishable folder.

Step-by-step detail, including what to do when something fails:
[docs/compilar.md](docs/compilar.md).

## How to run

You need your own disc image of Need for Speed: Most Wanted for Xbox 360. Nothing here
runs without it. On Windows it must be the **PAL Spain** edition. On Android one APK can
carry several editions and picks the one that matches your ISO; on iOS the app is built
for one edition: see [Game editions](#game-editions).

### Windows

1. Build it (see [Build](#build)). The result is the `build\` folder.
2. Open `build\NFS_Most_Wanted.exe`. That is the **launcher**; the game itself is
   `nfsmw.exe`.
3. In the launcher, pick your `.iso` (it is extracted once into `game_root_cache\`) or a
   folder you already extracted, choose the graphics options and press **JUGAR**.
4. In the game, `Esc` opens the settings menu. A controller is recommended; keyboard keys
   and controller notes are in [docs/controles.md](docs/controles.md).

### Android

On the phone: 64-bit, Android 8.0 or newer, and Vulkan 1.1 (the native engine also runs
on OpenGL ES 3.2 where Vulkan is missing or broken).

On the PC, to build the APK: Android Studio (for the SDK and its JDK), NDK
`28.2.13676358`, the Android SDK's CMake `3.31.6`, Python 3.10+, Git, Node.js (for the
native engine's shader library), and Visual Studio Build Tools with *Desktop development
with C++* and the *Clang compiler* (the code generator runs on the PC).

1. Build and install the APK. With the **native engine**, the recommended one:

   ```bat
   python tools\android\preparar_nativo.py --iso "your.iso"   :: translates the game into nativo\
   python tools\android\apk_ediciones.py                      :: out\apk\nfsmw-all-0.5.5-release.apk
   adb install -r out\apk\nfsmw-all-0.5.5-release.apk
   ```

   The first build takes about 30 minutes. `apk_ediciones.py` puts every edition you have
   prepared into the one APK: see [Game editions](#game-editions).

   With the **Xenos engine**, the one `gradlew` builds when no engine is given:

   ```bat
   python tools\fase1_extraer.py "your.iso" --solo-xex   :: assets\game_root\default.xex
   python tools\android\preparar_sdk.py                  :: ..\rexglue-sdk-android, patched
   python tools\android\generar_codigo.py                :: app\generated-android\
   cd android
   gradlew assembleRelease
   adb install -r app\build\outputs\apk\release\app-release.apk
   ```

   The first build is slow: the whole SDK plus 133 generated files. `gradlew` needs a
   recent JDK: if it fails with `UnsupportedClassVersionError`, point `JAVA_HOME` at the
   one Android Studio ships (`...\Android Studio\jbr`).
2. Copy your ISO to the phone's **internal** storage, for example
   `adb push your.iso /sdcard/Download/`. A FAT32 microSD will not hold a file over 4 GB.
3. Open the app and tap **Choose ISO…**. It asks for **access to all files**: with it, the
   app searches the internal storage and the SD card itself and lists only the ISOs of
   the editions the APK carries, with edition, size and folder (incomplete ones are
   marked). Some phones' file picker does not show the internal storage at all. Without
   the permission, **Use the file picker** works as before. Either way the ISO is read in
   place, not copied.
4. Tap **Play**.

Things worth knowing:

- **Controls.** On-screen touch controls show up by default; **Edit touch controls** in
  the launcher lets you move and resize them. Connect a gamepad and they hide by
  themselves.
- **Language.** The app follows the phone's language (English, Spanish, German, French,
  Italian, Polish, Portuguese, Japanese, Korean or Traditional Chinese), or the one you
  pick under **Language**. The game itself stays in the language of your disc's edition.
- **Saved games.** The native engine saves inside the app, where file managers and a PC
  cannot reach. **Saved games** in the Game section exports them to a `.zip` (profile,
  saves and achievements) — to `NFSMW/partidas` on internal storage with access to all
  files, or wherever the file picker says — and imports one back with the file picker.
  Before importing, the current saves are kept in `partidas_antes_de_importar.zip`.
- **Graphics.** **Graphics API** picks Vulkan or OpenGL ES 3.2 for the native engine
  (OpenGL ES is the way in on Mali and older GPUs whose Vulkan driver fails). **Battery
  saver** and **Factory settings** are presets; your own settings are kept and come back
  with **My settings**.
- **Drivers.** It runs on the phone's own GPU driver, Adreno or Mali. On Adreno you can
  try Turnip with **Import driver (.zip)** and an adrenotools package.
- **If it does not start**, open **Advanced** and tap **Test Vulkan**: it reports which
  driver loaded and whether the GPU is accepted, without needing the game.
- **The APK contains the game's code**, translated. It is for your own devices: never
  share or publish it (see [Legal](#legal)). Build the Xenos engine without
  `fase1_extraer.py` and `generar_codigo.py` and you get an APK with only the Vulkan test,
  which contains nothing from the game.

Everything else, including the settings and what was measured:
[docs/android.md](docs/android.md) (Spanish).

### iOS

On the iPhone: iOS 16 or newer and a GPU with Metal; tested on an iPhone 13 Pro (A15,
6 GB) with iOS 26. It runs the same native engine as Android, with Vulkan translated to
Metal by MoltenVK.

On the PC, no Mac is needed: the app is built and signed from Windows with
[xtool](https://github.com/xtool-org/xtool) inside WSL, with the iPhone passed into WSL by
[usbipd-win](https://github.com/dorssel/usbipd-win). xtool needs the Swift toolchain for
Linux and your `Xcode.xip` (downloaded with your Apple ID); a free Apple ID is enough. The
one-time setup, including a patch usbmuxd needs over usbip, is in
[ios/README.md](ios/README.md) (Spanish).

1. Prepare the native engine with your PAL Spain ISO, as for Android:
   `python tools\android\preparar_nativo.py --iso "your.iso"`.
2. Build, sign and install, from WSL:

   ```sh
   bash ios/motor/compilar.sh --instalar
   ```

3. Copy your ISO into the app's folder with the Files app (**On My iPhone › NFSMW**) or
   the Finder, or pick it anywhere in Files with **Choose ISO…** (it is read in place, not
   copied).
4. Tap **Play**.

Things worth knowing:

- **A free Apple ID signs the app for 7 days.** After that it has to be installed again;
  settings and saves are kept.
- **Back to the launcher means closing the app** from the app switcher and opening it
  again: iOS does not let an app close itself. Settings apply when you tap **Play**.
- **Saved games** are in the app's `datos` folder, which the Files app shows. **Saved
  games** in the Game section exports them to a `.zip` with the iOS file picker (Files,
  iCloud Drive…) and imports one back. It is the same `.zip` as Android's, so saves move
  between an Android phone and an iPhone.
- **Hosting a LAN game** keeps working with the app in the background or the screen locked:
  while **Host on this phone** is on, the app plays silence (mixed with your music) so iOS
  does not suspend it. Turn it off when you are done; it costs battery. Close the app
  before reinstalling it, or the install waits for it.
- **What iOS does not allow:** OpenGL ES (iOS stopped at 3.0, the engine needs 3.1+), so it
  is Vulkan through MoltenVK; and finding servers by broadcast, which needs an Apple
  entitlement a free Apple ID does not have, so the search asks every address on the
  network instead.
- **The app contains the game's code**: it is for your own devices and can never go to the
  App Store. Never share it (see [Legal](#legal)).

## Game editions

The game shipped in several editions, and each one has its own `default.xex`. A different
`default.xex` is a different program: the USA, Japanese and PAL English editions are
separate compilations of the game, with the code at other addresses (and, in the Japanese
one, the data as well). The build translates *your* disc's executable into C++ and
compiles it, so **each edition is its own compiled code**. With the native engine one APK
can carry several of them, one library per edition, and loads the one that matches the
ISO you pick. With the Xenos engine, and on iOS, the app is built for one edition.

| Edition | `default.xex` SHA-256 | Windows | Android, Xenos engine | Android, native engine | iOS | Game language |
|---|---|---|---|---|---|---|
| PAL Spain | `aad15fc2…` | Yes | Yes | Yes | Yes | Spanish |
| USA | `aebdf3c1…` | No | Builds, not verified in-game | Played on a phone | No | English |
| Japan | `23af89d4…` | No | No | Builds, not verified in-game | No | Japanese |
| PAL Germany | `23ece29d…` | No | Recognised, untested | Builds, not verified in-game | No | German |
| PAL Italy | `acb09332…` | No | Recognised, untested | Builds, not verified in-game | No | Italian |
| PAL UK (the "Europe" ISO) | `e2ea2ff1…` | No | No | Builds, not verified in-game | No | English |

PAL Germany and Italy are the same compilation as PAL Spain with another language (five
instructions change); PAL UK is another compilation, translated like the USA one. PAL
France is also recognised, but its disc has not been tried. The fan Polish translation,
Korea and Asia (Traditional Chinese) are recognised by their SHA-256 and would be
prepared the same way; none has been tried.

The two Android engines are explained in [docs/motor-nativo.md](docs/motor-nativo.md): the
Xenos engine emulates the Xbox 360 GPU; the native engine (which started as
[codepdbh/nfsmw-android](https://github.com/codepdbh/nfsmw-android)) replaces it with its
own renderer, on Vulkan or OpenGL ES 3.2, and is the faster one.

### Which disc do I have?

```bat
python tools\fase1_extraer.py "your.iso" --solo-xex            :: assets\game_root\default.xex
python tools\ediciones\ediciones.py detectar assets\game_root\default.xex
```

It prints the edition, the SHA-256 of its `default.xex`, and the language and country the
game is given. The app checks the same thing on the phone: the launcher says which editions
the APK carries, and warns when the ISO you pick belongs to another edition or is
incomplete (a cut download or extraction), instead of leaving you with a black screen.

### Building the APK (native engine)

The native engine's game code is written against the PAL Spain executable, and the other
editions are built by translating every address in it. So **the PAL Spain disc is needed
even to build any other edition**: prepare it first, then each of the others, and build
one APK with all of them.

```bat
python tools\android\preparar_nativo.py --iso "NFSMW (PAL Spain).iso"   :: always first
python tools\android\preparar_nativo.py --iso "NFSMW (USA).iso"         :: adds the USA edition
python tools\android\preparar_nativo.py --iso "NFSMW (Japan).iso"       :: adds the Japanese edition
python tools\android\apk_ediciones.py                                   :: one APK, every prepared edition
python tools\android\apk_ediciones.py --ediciones usa                   :: or only PAL Spain and USA
```

`apk_ediciones.py` writes `out\apk\nfsmw-all-<version>-release.apk`. Each edition adds
about 16 MB to the APK (40 MB installed). Before putting them together it checks that every
edition was compiled against the same SDK. With `--registro`, `--depurable` or
`--capas-vulkan` it writes `nfsmw-all-<version>-depuracion-release.apk` instead, so a
debugging build does not overwrite the one you install.

An APK with a single edition can still be built with Gradle:

```bat
cd android
gradlew assembleRelease -Pnfsmw.motor=nativo                            :: PAL Spain
gradlew assembleRelease -Pnfsmw.motor=nativo -Pnfsmw.edicion=usa        :: USA
gradlew assembleRelease -Pnfsmw.motor=nativo -Pnfsmw.edicion=jpn        :: Japan
gradlew assembleRelease -Pnfsmw.motor=nativo -Pnfsmw.edicion=pal_de     :: PAL Germany
gradlew assembleRelease -Pnfsmw.motor=nativo -Pnfsmw.edicion=pal_it     :: PAL Italy
gradlew assembleRelease -Pnfsmw.motor=nativo -Pnfsmw.edicion=pal_en     :: PAL UK ("Europe")
```

- `preparar_nativo.py` recognises the edition from the ISO itself and keeps each one
  apart, inside `nativo\`: `nativo\app_<edition>`,
  `assets\game_root_<edition>` and `out\<edition>`.
- **Before building another edition it checks the translation, and stops if anything
  fails:**
  - every address the app uses has a safe equivalent;
  - the game functions the app's code touches are identical apart from addresses;
  - every address those functions compute translates exactly.

  It also compares the shader library it generates with nfsmw-nx's official one for that
  edition, and says if they differ. For PAL Spain, USA and Japan they match byte for byte.
- **Build time:** each edition compiles in its own build folder, so switching between them
  does not rebuild the others. The first build of an edition takes about 15–20 minutes.
- **Single-edition builds all write the same file**,
  `android\app\build\outputs\apk\release\app-release.apk`. Copy it somewhere else before
  building the next edition, for example `out\apk\nfsmw-nativo-usa-release.apk`.

With the Xenos engine only PAL Spain and USA are supported, and the USA build does not
need the Spanish disc: see [docs/ediciones.md](docs/ediciones.md).

### On the phone

- **Installing one APK replaces the other.** All APKs share the same app identifier.
  Settings and save games are kept, and every edition reads the same save folder.
  Whether one edition loads another's saves has not been tested.
- **Copy the ISO of any edition the APK carries** to internal storage and pick it with
  **Choose ISO…**: its search only lists those. Each ISO is about 7.8 GB, so several can
  sit side by side if there is room.
- **The game's language comes from the edition**: Spanish, English, Japanese, German or
  Italian. The app's language setting only changes the launcher.
- **Like any APK here, they contain the game's code**: never share them (see
  [Legal](#legal)).

## Documentation

The README is in English; the technical documentation is in Spanish, matching the
source comments.

| Document | What it covers |
|---|---|
| [docs/arquitectura.md](docs/arquitectura.md) | How the pieces fit: SDK, app, patches, launcher |
| [docs/compilar.md](docs/compilar.md) | Building from a clean checkout |
| [docs/parches.md](docs/parches.md) | Every patch: what it changes, why, and how it was verified |
| [docs/lanzador.md](docs/lanzador.md) | The launcher, its settings and how it is built |
| [docs/rendimiento.md](docs/rendimiento.md) | Measured findings: EDRAM paths, resolution scaling, frame pacing |
| [docs/controles.md](docs/controles.md) | Keyboard keys and controller notes on Windows |
| [docs/pipeline-nativo.md](docs/pipeline-nativo.md) | A Vulkan drawing path made for this game, instead of emulating the 360 GPU |
| [docs/android.md](docs/android.md) | The Android port: building the APK, Turnip, and why each piece is the way it is |
| [docs/motor-nativo.md](docs/motor-nativo.md) | The native engine: how it is built for each edition and with several in one APK, Mali and other non-Adreno GPUs |
| [docs/motor-nativo-gles.md](docs/motor-nativo-gles.md) | The native engine's OpenGL ES 3.2 backend, for Mali and older phones |
| [docs/ediciones.md](docs/ediciones.md) | Game editions: how a disc is recognised, and the USA edition with the Xenos engine |
| [ios/README.md](ios/README.md) | The iOS port: building and signing from Windows with xtool, MoltenVK, and what iOS does not allow |
| [docs/problemas-conocidos.md](docs/problemas-conocidos.md) | What is broken and how far each one was traced |
| [docs/diario/](docs/diario/) | Long-form write-ups of the harder diagnoses |

The diary is worth reading before touching the audio or graphics code. Each entry
records what the evidence actually said, including the times a plausible theory turned
out to be wrong.

## Layout

```
NFSMW Recompiled/
├── app/                 the game application: CMake, codegen config, app subclass
│   ├── src/             main.cpp and the ReXApp subclass with the game's quirks
│   ├── nfsmw_manifest.toml   what the code generator reads
│   ├── overrides.toml   hand-written codegen fixes, each with its reason
│   └── huecos.toml      generated gap list (774 entries), see HUECOS.bat
├── android/             the Android app: Gradle, the launcher (Java) and the engine glue (C++)
├── nativo/              the native engine: its SDK fork, the game app (one app_<edition>
│                        per extra edition) and the Vulkan and OpenGL ES renderers
├── ios/motor/           the iOS app: CMake, launcher, touch controls, sound, LAN host
├── sdk/                 nfsmw-parches.diff: everything this fork changes in the SDK
├── tools/
│   ├── parche_*.py      the patches, applied to the SDK before building it
│   ├── android/         SDK preparation, codegen, native engine, the all-editions APK
│   ├── ediciones/       edition recognition and address translation
│   ├── lanzador/        the Windows launcher (C#, WinForms)
│   └── diagnostico/     instrumentation, not part of a normal build
├── launcher-linux/      the Linux launcher (Qt 6), with packaging/appimage/
├── docs/
└── CONSTRUIR.bat        the Windows build
```

### Why the fixes are patches against the SDK

Almost nothing this project fixes lives in the game application. The audio deadlock,
the missing v-sync, the graphics API selector, the Xbox Live privilege gate — all of
them are in ReXGlue, and they end up compiled into `rexruntime.dll`, not into the game
executable. So the build patches the SDK source, rebuilds it, and only then builds the
game.

Every patch is a Python script that applies and reverts by exact text replacement,
block by block. They refuse to touch anything if an anchor does not match exactly once,
they are idempotent, and `--revertir` restores the original. Run any of them with
`--estado` to see what is applied. The reasoning behind that design, and the bugs that
forced it, are in [docs/parches.md](docs/parches.md).

## Contributing

Pull requests are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first — the
short version is that patches carry their reasoning in the code, and a change that
fixes something should say what evidence says it is fixed.

## Legal

Static recompilation of a game you own, for your own use, sits on the same ground as
emulation: the translated code derives from a binary you bought.

What must **never** be distributed:

- `default.xex` or any other game file
- the C++ the code generator produces from it
- **the compiled game executable** — it contains the game's own code, translated

What is shared here is the *patch*: configuration, hooks, stubs, scripts and
documentation. Never the game.

The launcher's cover art and icon are Electronic Arts' artwork and are **not** in this
repository. The launcher builds and runs without them; see
[docs/lanzador.md](docs/lanzador.md) if you want to supply your own.

This project is licensed under the GNU General Public License v3.0 — see
[LICENSE](LICENSE). The ReXGlue SDK it builds against is BSD 3-Clause and is a separate
work with its own terms.

Need for Speed and Most Wanted are trademarks of Electronic Arts Inc. This project is
not affiliated with, endorsed by, or connected to Electronic Arts in any way.

⚠️ IMPORTANT DISC REQUIREMENT: the Windows build strictly requires Need for Speed: Most
Wanted (2005) for Xbox 360 in its **PAL Spain** version; PAL UK (English) and NTSC (US)
discs do not work there (for now). The Android build also supports the USA, Japanese,
PAL German, Italian and UK editions, several in one APK; the iOS app is PAL Spain only:
see [Game editions](#game-editions).

## Credits

Thanks to everyone whose work this stands on.

**The original project**

- [madelrandel-blip/NFSMW-Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) —
  the recompilation this fork is based on, by its authors and contributors, including the
  Linux ARM64 launcher and AppImage by MaSieS4Fun

**What it is built on**

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) — the runtime this is built on
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) — the static recompilation approach
- [Xenia](https://xenia.jp/) — the kernel and GPU emulation ReXGlue descends from
- The libraries the SDK bundles, each under its own license: FFmpeg (XMA audio), SDL3,
  glslang, SPIRV-Tools and SPIRV-Cross, Dear ImGui, spdlog, fmt, xxHash, Vulkan Memory
  Allocator and toml++, among others

**The Android port**

- [hells-gate-recomp-android](https://github.com/deivid22srk/hells-gate-recomp-android) by
  deivid22srk — the Android patch for the ReXGlue SDK (bionic, ASharedMemory, 16 KB pages,
  ANativeWindow, ARM64 memory barriers). It declares no license, so it is **not** included
  here: `tools/android/preparar_sdk.py` downloads it from that repository at build time
- [libadrenotools](https://github.com/bylaws/libadrenotools) by Billy Laws — loading custom
  Turnip drivers on Adreno GPUs
- [Mesa / Turnip](https://docs.mesa3d.org/drivers/freedreno.html) — the open-source Adreno
  Vulkan driver the app can load
- [codepdbh/nfsmw-android](https://github.com/codepdbh/nfsmw-android) — the Android port of
  nfsmw-nx. The native engine (`-Pnfsmw.motor=nativo`, also the one on iOS) started from its
  tree: its SDK fork, its game app and its native renderer, now in `nativo/` and maintained
  here (its Android app is no longer used). See [docs/motor-nativo.md](docs/motor-nativo.md)
- [StevensND/nfsmw-nx](https://github.com/StevensND/nfsmw-nx) — the Nintendo Switch port of this
  same project. The single-pass scene (`android/app/src/main/cpp/render_targets.cpp`) is ported
  from it, and its documentation on the game's renderer was a guide
- [Xenia Canary](https://github.com/xenia-canary/xenia-canary) and
  [Xenia Edge](https://github.com/has207/xenia-edge) — XMA decoder and ring buffer fixes ported
  in `tools/parche_xma_paquetes.py`, `tools/parche_xma_edge.py` and
  `tools/parche_anillo_bloques.py`
- [XenDroid](https://github.com/rfandango/XenDroid) — the reference for audio on Android
- [Buku313/Skate3-Mobile](https://github.com/Buku313/Skate3-Mobile) — the Skate 3 Android port
  the touch controller started from
- [Material Components for Android](https://github.com/material-components/material-components-android)
  — the launcher's interface

**The iOS port**

- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) by Khronos — Vulkan on Metal, so the
  native engine's Vulkan renderer runs on the iPhone
- [xtool](https://github.com/xtool-org/xtool) — building, signing and installing iOS apps
  from Linux, here inside WSL, without a Mac
- [usbipd-win](https://github.com/dorssel/usbipd-win) — passing the iPhone into WSL
- [usbmuxd](https://github.com/libimobiledevice/usbmuxd) from libimobiledevice — talking to
  the iPhone over USB; [ios/README.md](ios/README.md) has the patch it needs over usbip
