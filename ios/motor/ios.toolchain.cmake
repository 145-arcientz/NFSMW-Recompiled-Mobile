# iOS arm64 from Linux (WSL): the clang of the Swift toolchain, the iPhoneOS SDK and the ld64.lld
# that `xtool setup` installs in ~/.swiftpm/swift-sdks/darwin.artifactbundle.
#
#   cmake -G Ninja -S ios/motor -B build -DCMAKE_TOOLCHAIN_FILE=ios/motor/ios.toolchain.cmake

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES arm64)
# MoltenVK 1.4 asks for iOS 14; 16 leaves out the oldest phones without gaining anything we use.
set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0)

set(NFSMW_XTOOL_SDK "$ENV{HOME}/.swiftpm/swift-sdks/darwin.artifactbundle" CACHE PATH
    "darwin.artifactbundle de xtool")
file(GLOB NFSMW_IPHONEOS_SDKS
    "${NFSMW_XTOOL_SDK}/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS[0-9]*.sdk")
list(SORT NFSMW_IPHONEOS_SDKS)
list(POP_BACK NFSMW_IPHONEOS_SDKS NFSMW_IPHONEOS_SDK)
if(NOT NFSMW_IPHONEOS_SDK)
    message(FATAL_ERROR "No hay iPhoneOS*.sdk en ${NFSMW_XTOOL_SDK}: ejecuta 'xtool setup'")
endif()
set(CMAKE_OSX_SYSROOT "${NFSMW_IPHONEOS_SDK}" CACHE PATH "" FORCE)

set(NFSMW_IOS_TRIPLE arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET})
foreach(lang C CXX OBJC OBJCXX ASM)
    set(CMAKE_${lang}_COMPILER_TARGET ${NFSMW_IOS_TRIPLE})
endforeach()
# Full paths: CMake looks the OBJC/OBJCXX compilers up differently and does not find a bare "clang".
# The Swift toolchain's clang (swiftly), outside the sysroot.
find_program(NFSMW_CLANG clang NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(NFSMW_CLANGXX clang++ NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(NFSMW_LLVM_AR llvm-ar NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(NFSMW_LLVM_RANLIB llvm-ranlib NO_CMAKE_FIND_ROOT_PATH REQUIRED)
set(CMAKE_C_COMPILER "${NFSMW_CLANG}")
set(CMAKE_CXX_COMPILER "${NFSMW_CLANGXX}")
set(CMAKE_OBJC_COMPILER "${NFSMW_CLANG}")
set(CMAKE_OBJCXX_COMPILER "${NFSMW_CLANGXX}")
set(CMAKE_ASM_COMPILER "${NFSMW_CLANG}")
set(CMAKE_AR "${NFSMW_LLVM_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${NFSMW_LLVM_RANLIB}" CACHE FILEPATH "" FORCE)
# CMake requires it on Apple, but it only rewrites dylib install names when installing: everything here
# is static and nothing is installed. The Swift toolchain has none.
set(CMAKE_INSTALL_NAME_TOOL /usr/bin/true CACHE FILEPATH "" FORCE)

# xtool's ld64.lld (toolset/bin/orig): the one in the Swift toolchain is the same LLVM linker, but
# this is the one xtool tests against the SDK it extracted. clang only passes -platform_version (which
# ld64.lld requires) to a linker it knows is new enough; on Linux it cannot ask, so it is told.
set(NFSMW_LD64 "${NFSMW_XTOOL_SDK}/toolset/bin/orig/ld64.lld")
foreach(kind EXE SHARED MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_INIT "--ld-path=${NFSMW_LD64} -mlinker-version=950")
endforeach()

# Nothing from the Linux host: headers and libraries only from the SDK.
set(CMAKE_FIND_ROOT_PATH "${CMAKE_OSX_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# The final signature is xtool's; CMake must not try to sign anything.
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
