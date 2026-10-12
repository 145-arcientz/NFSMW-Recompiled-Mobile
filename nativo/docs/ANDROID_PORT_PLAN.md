# Android port plan

## Audited architecture

`nfsmw-nx` is already a native static recompilation project. ReXGlue translates the Xbox 360 PowerPC XEX into generated C++, and the game-specific app replaces selected game routines with native equivalents. The NFSMW renderer consumes the game's graphics command ring and emits Vulkan draws; shader preparation is handled separately by the `shaders/` tooling. The SDK supplies the guest kernel/runtime, filesystem devices, XMA audio, input, SDL UI, and Vulkan utilities.

The Switch build is selected by `REXGLUE_PLATFORM_SWITCH` and uses devkitA64/libnx, a NRO entry point, Horizon services, Switch window/input/audio implementations, and a Switch-specific Mesa/NVK build. The Android path must not enable this switch mode.

The checked-in SDK platform header already recognizes `__ANDROID__` as `REX_PLATFORM_ANDROID` while defining the POSIX/Linux compatibility flag. That makes existing pthread and mmap paths candidates for reuse. However, SDK CMake currently treats all non-Apple Unix platforms as desktop Linux: it requires X11/Wayland and selects `surface_gnulinux.cpp`; the SDL window source also includes X11 on its generic Linux branch. These are blockers for a direct Android application build. The standalone app must first prove Android JNI, private storage, and Vulkan loader integration, then the SDK needs an explicit Android build path and Android surface/lifecycle support.

## Reuse and replacement map

| Subsystem | Switch implementation | Android target | Status |
|---|---|---|---|
| PPC recompilation | ReXGlue generated C++ / AArch64 GCC | ReXGlue generated C++ / Android Clang AArch64 | Reusable in principle; SDK CMake/platform integration required |
| Guest memory | SDK mappings plus Horizon-specific branch | POSIX/Bionic `mmap`/`memfd` path | Investigate fixed-address and backing-file behavior on Android |
| Threads/fibers | SDK pthread paths and Switch branches | Bionic pthread/std::thread; validate ucontext/fiber support | Adapt and verify |
| Game filesystem | `sdmc:` and Switch game-root lookup | app-private files plus SAF URI import/copy | Replace path selection; SAF bridge not implemented |
| Window/lifecycle | libnx window/app loop | Android Activity + JNI + `ANativeWindow` or SDL Android | Replace; current shell uses Activity/JNI |
| Vulkan presentation | Switch surface and Mesa/NVK | Android Vulkan surface/loader; optional AdrenoTools/Turnip | Android `VkSurfaceKHR` and graphics-queue presentation support verified on Adreno 830; swapchain/rendering still pending |
| NFSMW renderer | PM4 ring parser, shader library, Vulkan pipelines | Same renderer against Android Vulkan device | Likely reusable after SDK surface/device adaptation |
| Shaders | SPIR-V and NFSSPV game-derived library | SPIR-V; user-generated library in app storage | Reuse format; game-derived files remain user supplied |
| Input | libnx HID/controller | SDL3 Android/gamepad and touch mapping | Replace |
| Audio | libnx audio output plus SDK XMA | SDL3 Android or AAudio output plus SDK decoder | Replace output backend; preserve XMA decode |
| Mesa/NVK | Switch Mesa fork embedded in NRO | Android system Vulkan; optional AdrenoTools Turnip driver | Switch fork is not an Android driver package |

## Android architecture

The first milestone is a Java Activity loading `libnfsmw_android.so` through JNI. Native startup creates app-private game/cache directories, writes Android Logcat startup records, and attempts Vulkan instance/device discovery without requiring game files. Later, the existing `nfsmw` sources and SDK runtime will be connected behind an Android platform target. SAF selection will copy/import the chosen extracted game into app-private storage so existing POSIX file access has a stable path; ISO mounting can follow.

## Dependencies and build strategy

- Android Gradle Plugin and Gradle wrapper build the APK.
- Android SDK platform 35 and NDK 28.2.13676358 target `arm64-v8a` only; ELF load segments use 16 KiB maximum page alignment for recent Android devices.
- Android CMake builds the native bootstrap as C++23 and links Android's Vulkan loader and `liblog`.
- ReXGlue host code generation remains a separate host build step and requires the user's own `default.xex`; generated code is ignored by git.
- The current machine has Git, but no CMake, Gradle, Android SDK/NDK, or ADB command available on PATH. Local APK/device verification is therefore blocked until those tools are installed. CI is set up to provide an SDK/NDK build.
- The requested Dante's Inferno repository is listed publicly by GitHub search but direct Git, API, and codeload requests return HTTP 404 in this environment. Its public repository description and AGENTS/build notes were reviewed through GitHub web results; no files were copied.

## Main risks

1. Android's available address-space layout and Bionic mapping APIs may not satisfy ReXGlue's 4 GiB guest views at the addresses used by current Linux builds.
2. SDK CMake and SDL window/surface code currently assume desktop Linux outside Switch/macOS/Windows.
3. The NFSMW target currently builds as an executable and depends on generated game code. It needs an Android shared-library entry path that respects Activity lifecycle and native-window ownership.
4. Android storage providers expose document URIs, not stable POSIX paths; the initial approach should import the game into app-private storage.
5. Turnip/AdrenoTools packaging varies by device and must remain optional; the baseline should use the system Vulkan driver.

## Roadmap

1. Create and build an ARM64 APK shell with JNI startup, Logcat, app-private directories, and Vulkan device discovery. **In progress.**
2. Add SAF selection/import for extracted game folders and persist the selected URI. **Implemented; direct picker import on device still needs verification.**
3. Add `REX_PLATFORM_ANDROID` to SDK CMake, omit desktop X11/Wayland requirements, and build the runtime as Android objects.
4. Port window/surface lifecycle and Vulkan presentation, then connect NFSMW native renderer sources.
5. Integrate generated ReXGlue code and validate guest memory mappings using a locally supplied game copy.
6. Add controller/touch input and audio output; then reach boot, first frame, menu, and gameplay.
7. Optimize NEON only after functional milestones; package optional driver support only after system Vulkan works.

## Dante reference boundary

The requested `WINDROID-EMU/Dantes-inferno-Android` source could not be fetched: Git clone, GitHub API, and codeload each returned 404, although GitHub search results expose its public description and repository notes. No Dante code is copied. Its described architecture confirms the useful areas to compare later: NDK/CMake, Gradle APK packaging, SDL3 Android, app-private game data, native ARM64 ReXGlue code, and optional Turnip through AdrenoTools.
