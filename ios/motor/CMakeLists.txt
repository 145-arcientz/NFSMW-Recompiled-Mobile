# The native engine for iOS: the same app as Android (../../nativo/app) and the same SDK, built as one
# static executable for an .app bundle. Only the native Vulkan renderer: no Xenos emulation plugin.
# MoltenVK is the prebuilt libMoltenVK.a of the Khronos release, linked statically.
#
# compilar.sh configures it with ios.toolchain.cmake (clang + the iPhoneOS SDK from xtool).

cmake_minimum_required(VERSION 3.25)
project(nfsmw_ios LANGUAGES C CXX OBJC OBJCXX)

get_filename_component(NFSMW_NATIVO "${CMAKE_CURRENT_LIST_DIR}/../../nativo" ABSOLUTE)

set(REXSDK_DIR "${NFSMW_NATIVO}/sdk" CACHE PATH "ReXGlue SDK source tree")
set(REXGLUE_USE_VULKAN ON CACHE BOOL "Vulkan through MoltenVK" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(REXGLUE_BUILD_XENOS OFF CACHE BOOL "iOS: native renderer only" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "No Tracy on iOS" FORCE)
set(REXGLUE_VERSION_OVERRIDE "0.10.0-dev.ios" CACHE STRING "ReXGlue SDK version for iOS")
set(REXGLUE_RECOMP_DEBUG_INFO "none" CACHE STRING "Omit debug tables for the large generated guest code")
# glslang and other old third-party CMake files: CMake 4 refuses cmake_minimum_required below 3.5.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

set(NFSMW_MOLTENVK "$ENV{HOME}/deps/moltenvk/MoltenVK/MoltenVK" CACHE PATH
    "MoltenVK of the Khronos release (MoltenVK-ios.tar), with static/ and include/")
set(NFSMW_MOLTENVK_LIB "${NFSMW_MOLTENVK}/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a")
if(NOT EXISTS "${NFSMW_MOLTENVK_LIB}")
    message(FATAL_ERROR "Falta ${NFSMW_MOLTENVK_LIB}: compilar.sh baja MoltenVK-ios.tar de la release de Khronos")
endif()

add_subdirectory("${NFSMW_NATIVO}/app" "nfsmw-app")

# 7 GB of RAM in WSL: the generated files, two at a time.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS recomp=2)
if(TARGET nfsmw_recomp)
    set_target_properties(nfsmw_recomp PROPERTIES JOB_POOL_COMPILE recomp)
endif()

# MoltenVK statically: the SDK takes vkGetInstanceProcAddr directly (vulkan_instance.cpp), as with
# Mesa on the Switch. It needs Metal and the frameworks below, and libc++.
target_link_libraries(nfsmw PRIVATE
    "${NFSMW_MOLTENVK_LIB}"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework IOSurface"
    "-framework CoreGraphics"
    "-framework UIKit"
    "-framework Foundation")

# Compiler runtime for iOS (__isPlatformVersionAtLeast, behind the @available checks in SDL's
# Objective-C). The Swift toolchain for Linux has none; xtool extracted Xcode's.
file(GLOB NFSMW_CLANG_RT_IOS
    "${NFSMW_XTOOL_SDK}/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/*/lib/darwin/libclang_rt.ios.a")
if(NOT NFSMW_CLANG_RT_IOS)
    message(FATAL_ERROR "No esta libclang_rt.ios.a en ${NFSMW_XTOOL_SDK}: ejecuta 'xtool setup'")
endif()
list(GET NFSMW_CLANG_RT_IOS 0 NFSMW_CLANG_RT_IOS)
target_link_libraries(nfsmw PRIVATE "${NFSMW_CLANG_RT_IOS}")

# On iOS CMake already builds the executable as a bundle (nfsmw-app/nfsmw.app/nfsmw); this is its
# Info.plist. compilar.sh zips the bundle as an .ipa for xtool to sign and install.
set_target_properties(nfsmw PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist")
# CMake copies it into the bundle when it configures: a change to it has to reconfigure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/Info.plist")
