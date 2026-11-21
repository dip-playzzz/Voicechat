# Build the upstream Meson project as static libraries; do not vendor its sources.
# The revision and all downloaded archives are pinned. APM is linked into the
# mod, not installed as another runtime .so.
include(FetchContent)
include(ExternalProject)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(VOICECHAT_WEBRTC_JOBS 2 CACHE STRING "Parallel jobs for the WebRTC Meson build")
find_program(VOICECHAT_MESON meson REQUIRED)
find_program(VOICECHAT_NINJA NAMES ninja ninja-build REQUIRED)

FetchContent_Declare(voicechat_webrtc_src
    URL https://codeload.github.com/thesofproject/webrtc-audio-processing/tar.gz/d0569cfa50c1858ee279d77b3fc8870be6902441
    URL_HASH SHA256=1dd02894eb5f6ad53f650e906312b00b2ae57d5271fd58381e4bd88e20ff7c68
    SOURCE_SUBDIR voicechat_no_cmake_project)
FetchContent_MakeAvailable(voicechat_webrtc_src)

# Meson's fallback subproject must sit under the WebRTC source tree. Stage an
# independent copy in each build directory: never modify a shared FetchContent
# source override, and never let simultaneous ABI builds extract over each other.
set(_webrtc_stage "${CMAKE_CURRENT_BINARY_DIR}/webrtc-apm-source")
file(MAKE_DIRECTORY "${_webrtc_stage}")
file(COPY "${voicechat_webrtc_src_SOURCE_DIR}/" DESTINATION "${_webrtc_stage}"
     PATTERN ".git" EXCLUDE PATTERN "abseil-cpp-20240722.0" EXCLUDE
     PATTERN "ex-voicechat_*" EXCLUDE)

# Use the exact Abseil version and wrapdb port specified by upstream's .wrap,
# fetched from codeload so the build does not depend on release-asset mirrors.
FetchContent_Declare(voicechat_abseil_src
    URL https://codeload.github.com/abseil/abseil-cpp/tar.gz/refs/tags/20240722.0
    URL_HASH SHA256=f50e5ac311a81382da7fa75b97310e4b9006474f9560ac46f54a9967f07d4ae3
    SOURCE_DIR "${_webrtc_stage}/subprojects/abseil-cpp-20240722.0"
    SOURCE_SUBDIR voicechat_no_cmake_project)
FetchContent_MakeAvailable(voicechat_abseil_src)
FetchContent_Declare(voicechat_wrapdb_src
    URL https://codeload.github.com/mesonbuild/wrapdb/tar.gz/refs/tags/abseil-cpp_20240722.0-3
    URL_HASH SHA256=340a0e0626e8b62147c45d9a4214936f1e210b2ebd182d6d7acb43f5f4858b29
    SOURCE_SUBDIR voicechat_no_cmake_project)
FetchContent_MakeAvailable(voicechat_wrapdb_src)
configure_file("${voicechat_wrapdb_src_SOURCE_DIR}/subprojects/packagefiles/abseil-cpp/meson.build"
               "${voicechat_abseil_src_SOURCE_DIR}/meson.build" COPYONLY)

set(_webrtc_build "${CMAKE_CURRENT_BINARY_DIR}/webrtc-apm-build")
set(_webrtc_cross_args)
if(ANDROID)
    if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        set(_webrtc_cpu_family aarch64)
        set(_webrtc_cpu aarch64)
    elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
        set(_webrtc_cpu_family arm)
        set(_webrtc_cpu armv7)
    else()
        message(FATAL_ERROR "Unsupported WebRTC Android ABI: ${CMAKE_ANDROID_ARCH_ABI}")
    endif()
    # CMake's NDK toolchain gives compiler targets (including API level) and
    # sysroot. Meson must use the same target and libc++ as the mod. Paths are
    # normalized for Meson's single-quoted strings, including Windows hosts.
    foreach(_v CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_AR CMAKE_STRIP CMAKE_SYSROOT)
        file(TO_CMAKE_PATH "${${_v}}" _path)
        string(REPLACE "'" "\\'" _path "${_path}")
        set(_meson_${_v} "${_path}")
    endforeach()
    set(_cross_file "${CMAKE_CURRENT_BINARY_DIR}/webrtc-android.ini")
    file(WRITE "${_cross_file}"
"[binaries]\nc = '${_meson_CMAKE_C_COMPILER}'\ncpp = '${_meson_CMAKE_CXX_COMPILER}'\nar = '${_meson_CMAKE_AR}'\nstrip = '${_meson_CMAKE_STRIP}'\n\n[host_machine]\nsystem = 'android'\ncpu_family = '${_webrtc_cpu_family}'\ncpu = '${_webrtc_cpu}'\nendian = 'little'\n\n[built-in options]\nc_args = ['--target=${CMAKE_C_COMPILER_TARGET}', '--sysroot=${_meson_CMAKE_SYSROOT}', '-DANDROID', '-fdata-sections', '-ffunction-sections']\ncpp_args = ['--target=${CMAKE_CXX_COMPILER_TARGET}', '--sysroot=${_meson_CMAKE_SYSROOT}', '-DANDROID', '-fdata-sections', '-ffunction-sections', '-stdlib=libc++']\nc_link_args = ['--target=${CMAKE_C_COMPILER_TARGET}', '--sysroot=${_meson_CMAKE_SYSROOT}']\ncpp_link_args = ['--target=${CMAKE_CXX_COMPILER_TARGET}', '--sysroot=${_meson_CMAKE_SYSROOT}', '-stdlib=libc++']\n")
    list(APPEND _webrtc_cross_args "--cross-file=${_cross_file}")
    # NEON is mandatory on arm64. Keep WebRTC's optional armv7 NEON path
    # disabled, but this does not promise compatibility with non-NEON CPUs:
    # modern Android NDKs can target NEON for other 32-bit code by default.
    if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        list(APPEND _webrtc_cross_args "-Dneon=enabled")
    else()
        list(APPEND _webrtc_cross_args "-Dneon=disabled")
    endif()
endif()

set(_webrtc_archives "${_webrtc_build}/webrtc/modules/audio_processing/libwebrtc-audio-processing-2.a")
foreach(_lib base flags container debugging strings crc numeric profiling hash synchronization time types)
    list(APPEND _webrtc_archives "${_webrtc_build}/subprojects/abseil-cpp-20240722.0/libabsl_${_lib}.a")
endforeach()

# Match the mod's C++20 mode, including Abseil's C++-version-dependent ABI.
# --reconfigure applies changed options in an existing Meson build directory.
ExternalProject_Add(voicechat_webrtc_build
    SOURCE_DIR "${_webrtc_stage}"
    BINARY_DIR "${_webrtc_build}"
    CONFIGURE_COMMAND "${VOICECHAT_MESON}" setup --reconfigure <BINARY_DIR> <SOURCE_DIR>
        --wrap-mode=forcefallback --buildtype=release --default-library=static
        -Dcpp_std=c++20 -Dabseil-cpp:cpp_std=c++20
        -Db_staticpic=true -Dgnustl=disabled ${_webrtc_cross_args}
    BUILD_COMMAND "${VOICECHAT_MESON}" compile -C <BINARY_DIR> -j ${VOICECHAT_WEBRTC_JOBS}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${_webrtc_archives})

add_library(voicechat_webrtc INTERFACE)
add_dependencies(voicechat_webrtc voicechat_webrtc_build)
target_compile_definitions(voicechat_webrtc INTERFACE VOICECHAT_ENABLE_WEBRTC=1)
target_include_directories(voicechat_webrtc INTERFACE
    "${_webrtc_stage}/webrtc"
    "${voicechat_abseil_src_SOURCE_DIR}")
# The upstream static APM archive contains the other WebRTC objects, but its
# Abseil archives have cyclic references. Resolve them together on ELF targets.
target_link_libraries(voicechat_webrtc INTERFACE "-Wl,--start-group" ${_webrtc_archives} "-Wl,--end-group")

# Used by the Android packaging step to include required third-party notices.
set(VOICECHAT_WEBRTC_SOURCE_DIR "${_webrtc_stage}")
set(VOICECHAT_ABSEIL_SOURCE_DIR "${voicechat_abseil_src_SOURCE_DIR}")
