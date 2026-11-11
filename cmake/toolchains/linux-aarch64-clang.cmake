# Cross-compile Ninja Gaiden II for 64-bit ARM Linux from an x86-64 Linux host: clang and lld
# against an aarch64 sysroot (packaging/aarch64/fetch-sysroot.py builds one out of Ubuntu 24.04's
# arm64 packages, as the ReXGlue SDK's linux-arm64 release is built on). Used by
# packaging/aarch64/build.sh.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(NG2_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../../out/aarch64-sysroot" CACHE PATH
    "The aarch64 sysroot: glibc, the C++ runtime and the libraries the game links")
if(NOT EXISTS "${NG2_SYSROOT}/usr/lib/aarch64-linux-gnu/libc.so.6")
    message(FATAL_ERROR "No aarch64 sysroot in ${NG2_SYSROOT} (packaging/aarch64/fetch-sysroot.py)")
endif()

set(_triple aarch64-linux-gnu)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_triple})
set(CMAKE_AR llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_SYSROOT "${NG2_SYSROOT}")

# The ARMv8.0 baseline, as the SDK's linux-arm64 presets. Extra flags for the build come in
# NG2_CROSS_FLAGS: a CMAKE_C_FLAGS given on the command line would replace these.
set(NG2_CROSS_FLAGS "-march=armv8-a" CACHE STRING "Extra C/C++ flags for the aarch64 cross build")
set(CMAKE_C_FLAGS_INIT "${NG2_CROSS_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${NG2_CROSS_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${NG2_CROSS_FLAGS}")
# The C++ runtime linked in and kept private, as in the AppImage (packaging/appimage/build.sh,
# #27): the sysroot's libstdc++ may be newer than the target system's.
set(_link "-fuse-ld=lld -static-libstdc++ -static-libgcc -Wl,--exclude-libs,libstdc++.a:libgcc.a:libgcc_eh.a")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_link}")

# Headers, libraries and CMake/pkg-config packages from the sysroot only; programs from the host.
set(NG2_FIND_ROOTS "${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/rexglue-sdk/out/install/linux-arm64"
    CACHE STRING "Extra aarch64 install prefixes to search")
set(CMAKE_FIND_ROOT_PATH "${NG2_SYSROOT}" ${NG2_FIND_ROOTS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${NG2_SYSROOT}")
# usr/lib/pkgconfig: the FFmpeg packaging/aarch64/build.sh installs there.
set(ENV{PKG_CONFIG_LIBDIR}
    "${NG2_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${NG2_SYSROOT}/usr/lib/pkgconfig:${NG2_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
