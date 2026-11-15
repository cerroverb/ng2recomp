# ng2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ng2 VERSION 0.6.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Cross-compiled (Windows from Linux): the build-time tools are the host
# build's (NG2_HOST_BUILD_DIR, built first): the code generator, and the
# shader cache that build translated (the code and shaders don't depend on
# the target).
if(CMAKE_CROSSCOMPILING)
    set(NG2_HOST_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/out/build/linux-amd64-release" CACHE PATH
        "A native build of this tree, for its build-time tools and shader cache")
    set(NG2_HOST_REXGLUE "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk/out/install/linux-amd64/bin/rexglue"
        CACHE FILEPATH "The host's ReXGlue code generator")
    if(NOT EXISTS "${NG2_HOST_REXGLUE}")
        message(FATAL_ERROR "No ${NG2_HOST_REXGLUE}: build and install the native SDK first")
    endif()
    set_target_properties(rex::rexglue PROPERTIES
        IMPORTED_LOCATION "${NG2_HOST_REXGLUE}" IMPORTED_LOCATION_RELEASE "${NG2_HOST_REXGLUE}")
endif()

# Sources
set(NG2_SOURCES
    src/main.cpp
    src/gpu/video.cpp
    src/gpu/interp.cpp
    src/gpu/native_gpu.cpp
    src/gpu/win_display.cpp
    src/gpu/menu_label.cpp
    src/gpu/prompts.cpp
    src/controls.cpp
    src/extras.cpp
    src/storage_prompt.cpp
    src/cpu_handover.cpp
    src/gpu/d3d_hooks.cpp
    src/perf_overlay.cpp
    src/recomp_settings.cpp
    src/movie/movie_hooks.cpp
    src/movie/host_movie.cpp
    src/debug_waits.cpp
    src/room_jump.cpp
    src/cheats.cpp
    src/frame_wait.cpp
    src/dlc.cpp
    src/mods.cpp
    src/stderr_log.cpp
    src/gameplay.cpp
    src/game_fixes.cpp
    src/aspect.cpp
    src/windows_setup.cpp
)

if(WIN32)
    # The executable's icon.
    add_executable(ng2 WIN32 ${NG2_SOURCES} packaging/windows/ng2.rc packaging/windows/ng2.manifest)
else()
    add_executable(ng2 ${NG2_SOURCES})
endif()

# xenos (GPU emulation) is the reference renderer while the native one is built.
rexglue_setup_target(ng2 GPU_PLUGINS xenos)
set(NG2_VERSION_SUFFIX "" CACHE STRING "Appended to the version shown (test builds)")
target_compile_definitions(ng2 PRIVATE NG2_VERSION="${PROJECT_VERSION}${NG2_VERSION_SUFFIX}")

# The SDK sets an $ORIGIN rpath on Linux but only stages its shared runtime
# libraries on Windows and macOS.
if(UNIX AND NOT APPLE)
    foreach(_lib rex::runtime rex::TracyClient)
        if(TARGET ${_lib})
            add_custom_command(TARGET ng2 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_lib}> $<TARGET_FILE_DIR:ng2>
                VERBATIM)
        endif()
    endforeach()
endif()

# Shaders, translated ahead of time like Unleashed Recompiled does: the
# shader containers sit in the XEX image, ng2_dump_image extracts that image
# from the user's default.xex and XenosRecomp (patched for NG2, see
# docs/shaders.md) turns every container into SPIR-V, embedded as a zstd
# compressed cache.
set(NG2_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/game_files" CACHE PATH
    "Extracted game files (default.xex is read at build time for its shaders)")
set(NG2_XENOS_RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp")
# NG2_NAN_SAFE_SHADERS=ON: the game's shaders drop NaNs before clamping (a test
# for Intel GPUs, #18; XenosRecomp patch 0019).
option(NG2_NAN_SAFE_SHADERS "Game shaders drop NaNs before clamping (Intel test)" OFF)
set(NG2_XENOS_RECOMP_ENV "XENOS_NAN_SAFE_OFF=1")
if(NG2_NAN_SAFE_SHADERS)
    set(NG2_XENOS_RECOMP_ENV "XENOS_NAN_SAFE=1")
endif()

if(CMAKE_CROSSCOMPILING)
    set(NG2_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(NG2_SHADER_CACHE "${NG2_HOST_BUILD_DIR}/shaders/ng2_shader_cache.cpp")
    if(NOT EXISTS "${NG2_SHADER_CACHE}")
        message(FATAL_ERROR "No ${NG2_SHADER_CACHE}: build the native tree (NG2_HOST_BUILD_DIR) first")
    endif()
else()
    add_executable(ng2_dump_image tools/dump_image/main.cpp)
    target_link_libraries(ng2_dump_image PRIVATE rex::runtime)

    include(ExternalProject)
    ExternalProject_Add(xenos_recomp
        SOURCE_DIR "${NG2_XENOS_RECOMP_DIR}"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/xenos_recomp"
        CMAKE_ARGS
            -G Ninja
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DXENOS_RECOMP_DXIL=OFF
        BUILD_BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/xenos_recomp/XenosRecomp/XenosRecomp"
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON)

    set(NG2_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(NG2_SHADER_CACHE "${NG2_SHADER_DIR}/ng2_shader_cache.cpp")
    add_custom_command(
        OUTPUT "${NG2_SHADER_CACHE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NG2_SHADER_DIR}/input"
        COMMAND ng2_dump_image "${NG2_GAME_DIR}" default.xex "${NG2_SHADER_DIR}/input/default.img"
        COMMAND ${CMAKE_COMMAND} -E env ${NG2_XENOS_RECOMP_ENV}
            "${CMAKE_CURRENT_BINARY_DIR}/xenos_recomp/XenosRecomp/XenosRecomp"
            "${NG2_SHADER_DIR}/input" "${NG2_SHADER_CACHE}"
            "${NG2_XENOS_RECOMP_DIR}/XenosRecomp/shader_common.h"
        DEPENDS ng2_dump_image xenos_recomp "${NG2_GAME_DIR}/default.xex"
            "${NG2_GAME_DIR}/default.xexp"
            "${CMAKE_CURRENT_BINARY_DIR}/xenos_recomp/XenosRecomp/XenosRecomp"
            "${NG2_XENOS_RECOMP_DIR}/XenosRecomp/shader_common.h"
        COMMENT "Translating NG2 shaders with XenosRecomp"
        VERBATIM)
endif()

add_library(ng2_shaders STATIC "${NG2_SHADER_CACHE}"
    "${NG2_XENOS_RECOMP_DIR}/thirdparty/smol-v/source/smolv.cpp")
target_include_directories(ng2_shaders PUBLIC src/gpu
    "${NG2_XENOS_RECOMP_DIR}/thirdparty/smol-v/source")
if(WIN32)
    # zstd from XenosRecomp's copy, built in; FFmpeg from a prebuilt shared
    # build (packaging/windows/build.sh fetches it into NG2_FFMPEG_DIR).
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/XenosRecomp/thirdparty/zstd/build/cmake zstd EXCLUDE_FROM_ALL)
    target_link_libraries(ng2_shaders PUBLIC libzstd_static)
    target_include_directories(ng2_shaders PUBLIC thirdparty/XenosRecomp/thirdparty/zstd/lib)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    target_link_libraries(ng2_shaders PUBLIC PkgConfig::ZSTD)
endif()
target_link_libraries(ng2 PRIVATE ng2_shaders)
if(WIN32)
    # The first-run setup (src/windows_setup.cpp): the title update's SHA-1.
    target_link_libraries(ng2 PRIVATE bcrypt)
    # The display clock where Vulkan has no present_wait (src/gpu/win_display.cpp).
    target_link_libraries(ng2 PRIVATE dxgi)
endif()

# Movies decoded on the host (src/movie): the game's WMV3/VC-1 and WMA files.
if(WIN32)
    set(NG2_FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/out/windows-deps/ffmpeg" CACHE PATH
        "FFmpeg for Windows: include/, lib/*.lib, bin/*.dll")
    target_include_directories(ng2 PRIVATE "${NG2_FFMPEG_DIR}/include")
    foreach(_lib avformat avcodec avutil swresample swscale)
        target_link_libraries(ng2 PRIVATE "${NG2_FFMPEG_DIR}/lib/${_lib}.lib")
    endforeach()
else()
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswresample libswscale)
    target_link_libraries(ng2 PRIVATE PkgConfig::FFMPEG)
endif()

# Native renderer: plume (Unleashed Recompiled's RHI), Vulkan only, with the
# Wayland surface support from patches/plume.
add_subdirectory(thirdparty/plume EXCLUDE_FROM_ALL)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(plume PUBLIC X11::X11)
endif()
target_link_libraries(ng2 PRIVATE plume)

# Internal shaders (present, copies), HLSL compiled to SPIR-V with the DXC
# that ships with XenosRecomp and embedded in the executable.
set(NG2_DXC_DIR "${NG2_XENOS_RECOMP_DIR}/thirdparty/dxc-bin")
set(NG2_INTERNAL_SHADERS
    "fullscreen_vs:vs_6_0"
    "present_ps:ps_6_0"
    "rect_list_gs:gs_6_0"
    "copy_ps:ps_6_0"
    "copy_7e3_ps:ps_6_0"
    "copy_7e3_ms_ps:ps_6_0"
    "scale_ps:ps_6_0"
    "depth_copy_ps:ps_6_0"
    "depth_copy_ms_ps:ps_6_0"
    "debug_ps:ps_6_0"
)
set(NG2_INTERNAL_SHADER_SOURCES "")
foreach(_entry ${NG2_INTERNAL_SHADERS})
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _profile)
    set(_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/${_name}.hlsl")
    set(_spv "${NG2_SHADER_DIR}/internal/${_name}.spv")
    set(_cpp "${NG2_SHADER_DIR}/internal/${_name}.cpp")
    add_custom_command(
        OUTPUT "${_cpp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NG2_SHADER_DIR}/internal"
        COMMAND ${CMAKE_COMMAND} -E env "LD_LIBRARY_PATH=${NG2_DXC_DIR}/lib/x64"
            "${NG2_DXC_DIR}/bin/x64/dxc-linux" -T ${_profile} -E main -spirv
            -fvk-use-dx-layout -HV 2021 -Qstrip_debug -Fo "${_spv}" "${_hlsl}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${_spv} -DOUTPUT=${_cpp} -DNAME=g_${_name}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
        DEPENDS "${_hlsl}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
        COMMENT "Compiling ${_name}.hlsl"
        VERBATIM)
    list(APPEND NG2_INTERNAL_SHADER_SOURCES "${_cpp}")
endforeach()
target_sources(ng2_shaders PRIVATE ${NG2_INTERNAL_SHADER_SOURCES})
