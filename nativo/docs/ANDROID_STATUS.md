# Android port status

Updated: 2026-09-29

## Current build

- [x] Android ARM64 debug APK builds from the ReXGlue application target.
- [x] APK includes `libmain.so`, `libSDL3.so`, ReXGlue runtime, and the Xenos Vulkan plugin.
- [x] All packaged ARM64 libraries have 16 KiB ELF load alignment; APK ZIP alignment also passes `zipalign -P 16`.
- [x] Local code generation completed from the user's PAL Spanish `default.xex` (Title ID `454107D9`).
- [x] The XEX-derived code compiled into `libmain.so` for ARM64.
- [x] Android launcher can import `default.xex`, `NFS/`, and `Movies/` into app-private storage.
- [ ] Install and launch this new SDL-based build on the phone.
- [ ] Confirm game-data import/recognition in the new launcher.
- [ ] Confirm the guest XEX starts, renders its first frame, and reaches the menu.
- [ ] Confirm playable controls, audio, and save/load.

## What was built

The debug package is `android/app/build/outputs/apk/debug/app-debug.apk` (about 127 MiB). It has application ID `com.nfsmw.android`, targets ARM64, and includes the SDL Android activity. `GameActivity` passes the private game, user-data, and cache paths to ReXGlue. The launcher exposes a **Play** button once it finds `files/nfsmw/game_root/default.xex`.

The game code was generated locally: 265 generated C++ files and 79,237 transformed calls. Those files, the source XEX, game assets, and the APK are excluded from Git. No game files or generated game code were uploaded.

## Build checks

- `gradlew assembleDebug --no-daemon` completed successfully after the Android SDL entry point was connected to the app registration.
- The APK contains `libSDL3.so`, `libmain.so`, `librexruntimed.so`, and `librexgpu-xenosd.so` for `arm64-v8a`.
- `libmain.so` resolves the packaged SDL library by the name expected by `SDLActivity`.
- ELF load segments for the packaged native libraries use `p_align=0x4000`; `zipalign -c -P 16 4` passes.
- Earlier bootstrap builds were installed on an Adreno 830 phone and confirmed Vulkan surface support. That check predates this SDL/ReXGlue game build and does not establish that this APK boots the game.

## Device setup

On first launch, select the extracted game folder. The importer copies it transactionally to app-private storage and checks for `default.xex`, `NFS/`, and `Movies/`. The game directory copied to the device during earlier setup is expected to remain in the same app storage when the package is updated, but this new launcher has not yet been run against it.

The user plans to reconnect the phone later. The current work stops at a built APK; the phone has not been installed or launched during this build session.

## Performance work (2026-09-29)

The first builds lagged badly in races. There were two causes, and either one alone is enough to make the game slow:

1. **Debug build.** `assembleDebug` compiled the recompiled game code with `-O0 -g` (checked in `compile_commands.json`). The scripts now build `assembleRelease` with `CMAKE_BUILD_TYPE=Release` (`-O3`), ThinLTO over `nfsmw_recomp` and `nfsmw`, and `-march=armv8.2-a` (LSE atomics).
2. **Emulated GPU.** `nfsmw_renderizador` defaulted to `xenos`, the path that gave 1-3 FPS on the Switch, and no `nfsmw.toml` was read on Android. The APK now ships `assets/nfsmw.toml`, built from the Switch release settings with the native renderer on. `GameActivity` copies it to `files/nfsmw/user/`.

Other changes:

- The SDK's `-ffp-model=strict` became `-ffp-contract=off` on Android. This matches the GCC Switch build: there is still no FMA drift, but FP code can be optimized again.
- `GetExecutableFolder()` returns `REX_APP_FOLDER` (set by `GameActivity` to `files/nfsmw/user`) on Android. Before this, the pipeline cache, the settings, and the shader library pointed at `/system/bin`.
- `nfsmw_shaders.nfsp` is also looked up in the game folder. `tools/biblioteca_shaders.mjs` builds it with the installer's WASM tools. For PAL-ES it matches the official SHA-256 `a27aea23…`.
- The Switch-only defaults are turned on in the toml: `nfsmw_render_sin_mosaico` (one pass without tiling or MSAA) and `nfsmw_cubemap_caras_siempre`.
- On SoCs with a slow cluster, game threads are kept off it (`nfsmw_android_nucleos_grandes`). The app is declared as a game for the OEM game modes.
- The touch overlay no longer uses a software layer, and it redraws only when a button changes.

First run on a Galaxy S25 Ultra (SM8750 / Adreno 830) with the native renderer at 1280x720: the image is correct (sky, lighting, smoke, reflections, HUD and menus). SurfaceFlinger timestats over 20 s of the attract race: 62.4 FPS on average, 1,203 frames, 0 dropped, none over 33 ms. The frame limit (`nfsmw_limite_fps = "60"`) sets the pace. A full player-driven race and 1920x1080 are still to be measured.
