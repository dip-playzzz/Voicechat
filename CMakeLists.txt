cmake_minimum_required(VERSION 3.22)

project(voicechat_levi_mod LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MOD_ID "voicechat" CACHE STRING "LeviLauncher mod package directory name")
set(MOD_NAME "Proximity Voice Chat" CACHE STRING "LeviLauncher mod display name")
set(MOD_AUTHOR "voicechat" CACHE STRING "LeviLauncher mod author")
set(MOD_VERSION "0.1.0" CACHE STRING "LeviLauncher mod version")
set(MOD_LIBRARY_NAME "voicechat" CACHE STRING "Native library name without lib prefix or .so suffix")
set(MOD_MINECRAFT_VERSIONS "[\"1.26.6X.X\"]" CACHE STRING "Raw JSON array for manifest minecraft_versions")
set(MOD_ICON "" CACHE STRING "Optional package-relative icon path, such as icon.png")

include(FetchContent)

FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG 0.2.2
    SOURCE_SUBDIR cmake/preloader-sdk
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_Declare(
    boost_pfr
    GIT_REPOSITORY https://github.com/boostorg/pfr.git
    GIT_TAG boost-1.88.0
)
FetchContent_Declare(
    magic_enum
    GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
    GIT_TAG v0.9.7
)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.2.0
)
set(FMT_DOC OFF CACHE BOOL "Disable fmt docs" FORCE)
set(FMT_TEST OFF CACHE BOOL "Disable fmt tests" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "Disable fmt install rules" FORCE)
FetchContent_MakeAvailable(preloader_android nlohmann_json boost_pfr magic_enum fmt)

FetchContent_GetProperties(preloader_android)
set(LEVI_PRELOADER_INCLUDE_DIR "${preloader_android_SOURCE_DIR}/include")

option(VOICECHAT_BUILD_HOST_INTEGRATION "Build the simulated-device multiplayer integration test" OFF)
option(VOICECHAT_ENABLE_WEBRTC "Build WebRTC audio processing into the multiplayer voice path" ON)
# The host config generator alone does not need audio or Meson.
if(VOICECHAT_ENABLE_WEBRTC AND (ANDROID OR VOICECHAT_BUILD_HOST_INTEGRATION))
    include(cmake/WebRtcAudioProcessing.cmake)
endif()

if(NOT ANDROID)
    set(LEVI_GENERATED_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-config")

    add_executable(levi_config_generator
        src/config_generator.cpp
        src/mod/Config.cpp
    )

    target_include_directories(levi_config_generator
        PRIVATE
            src
            "${LEVI_PRELOADER_INCLUDE_DIR}"
    )

    target_compile_definitions(levi_config_generator PRIVATE PL_CONFIG_NO_RUNTIME)
    target_link_libraries(levi_config_generator PRIVATE
        nlohmann_json::nlohmann_json
        Boost::pfr
        magic_enum::magic_enum
    )

    add_custom_target(levi_generate_config
        COMMAND levi_config_generator "${LEVI_GENERATED_CONFIG_DIR}"
        DEPENDS levi_config_generator
        COMMENT "Generating config.json and config.schema.json"
        VERBATIM
    )

    # Compile the Android mod's lifecycle/wiring on the host as well. The SDK and
    # config headers are real; only Android logging and the JavaVM pointer are stubbed.
    # This catches broken startup code that the standalone voice tests cannot see.
    add_library(voicechat_mod_compile_check OBJECT EXCLUDE_FROM_ALL src/mod/VoiceMod.cpp)
    target_include_directories(voicechat_mod_compile_check PRIVATE
        tests/host_stubs src "${LEVI_PRELOADER_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bedrocktools/include")
    target_link_libraries(voicechat_mod_compile_check PRIVATE
        nlohmann_json::nlohmann_json Boost::pfr magic_enum::magic_enum fmt::fmt)

    if(VOICECHAT_BUILD_HOST_INTEGRATION)
        find_package(Threads REQUIRED)
        find_library(OPUS_TEST_LIBRARY NAMES opus libopus.so.0 REQUIRED)
        add_executable(voicechat_mod_integration_test
            tests/voice_mod_integration_test.cpp
            src/mod/Config.cpp src/mod/VoiceMod.cpp src/mod/ModMenuLink.cpp
            src/voice/Loopback.cpp src/voice/PlayerTracker.cpp src/voice/MicSocketSource.cpp
            src/voice/SocketLoopback.cpp src/voice/RelayClient.cpp src/voice/VoiceSender.cpp
            src/voice/RemoteVoices.cpp src/voice/VoiceSession.cpp src/voice/WebRtcProcessor.cpp)
        target_compile_definitions(voicechat_mod_integration_test PRIVATE VOICECHAT_TEST)
        target_include_directories(voicechat_mod_integration_test PRIVATE
            tests/host_stubs tests/host_opus src "${LEVI_PRELOADER_INCLUDE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bedrocktools/include")
        target_link_libraries(voicechat_mod_integration_test PRIVATE
            nlohmann_json::nlohmann_json Boost::pfr magic_enum::magic_enum fmt::fmt
            Threads::Threads "${OPUS_TEST_LIBRARY}" ${CMAKE_DL_LIBS})
        if(TARGET voicechat_webrtc)
            target_link_libraries(voicechat_mod_integration_test PRIVATE voicechat_webrtc)
            add_executable(voicechat_webrtc_test tests/webrtc_apm_test.cpp src/voice/WebRtcProcessor.cpp)
            target_include_directories(voicechat_webrtc_test PRIVATE src)
            target_link_libraries(voicechat_webrtc_test PRIVATE voicechat_webrtc Threads::Threads)
        endif()
        # PlayerTracker finds the simulated BedrockTools_GetApi with dlsym.
        set_target_properties(voicechat_mod_integration_test PROPERTIES ENABLE_EXPORTS ON)
    endif()

    return()
endif()

# --- Audio dependencies (Android build only; the host config generator doesn't need them) ---
# Oboe and Opus still declare an old cmake_minimum_required; CMake 4 (now on GitHub's runners) refuses
# that unless told the minimum policy level explicitly.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(oboe GIT_REPOSITORY https://github.com/google/oboe.git GIT_TAG 1.9.0)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus GIT_REPOSITORY https://github.com/xiph/opus.git GIT_TAG v1.5.2)
FetchContent_MakeAvailable(oboe opus)

if(NOT DEFINED LEVI_PACKAGE_CONFIG_DIR)
    set(LEVI_PACKAGE_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/config" CACHE PATH
        "Directory containing config.json and config.schema.json to package")
endif()

get_filename_component(LEVI_PACKAGE_CONFIG_DIR "${LEVI_PACKAGE_CONFIG_DIR}" ABSOLUTE)
set(LEVI_PACKAGE_CONFIG_JSON "${LEVI_PACKAGE_CONFIG_DIR}/config.json")
set(LEVI_PACKAGE_CONFIG_SCHEMA "${LEVI_PACKAGE_CONFIG_DIR}/config.schema.json")

if(NOT IS_DIRECTORY "${LEVI_PACKAGE_CONFIG_DIR}")
    message(FATAL_ERROR "LEVI_PACKAGE_CONFIG_DIR must be a directory: ${LEVI_PACKAGE_CONFIG_DIR}")
endif()

if(NOT EXISTS "${LEVI_PACKAGE_CONFIG_JSON}")
    message(FATAL_ERROR "LEVI_PACKAGE_CONFIG_DIR must contain config.json: ${LEVI_PACKAGE_CONFIG_JSON}")
endif()

if(NOT EXISTS "${LEVI_PACKAGE_CONFIG_SCHEMA}")
    message(FATAL_ERROR "LEVI_PACKAGE_CONFIG_DIR must contain config.schema.json: ${LEVI_PACKAGE_CONFIG_SCHEMA}")
endif()

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a" AND
   NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
    message(FATAL_ERROR "Unsupported Android ABI: ${CMAKE_ANDROID_ARCH_ABI}")
endif()

option(LEVI_LINK_PRELOADER
       "Build and link the preloader target instead of leaving SDK symbols for runtime lookup"
       ON)

if(LEVI_LINK_PRELOADER AND NOT TARGET preloader)
    add_subdirectory("${preloader_android_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_preloader_android")
endif()

add_library(${MOD_LIBRARY_NAME} SHARED
    src/main.cpp
    src/mod/Config.cpp
    src/mod/VoiceMod.cpp
    src/mod/ModMenuLink.cpp
    src/voice/Loopback.cpp
    src/voice/PlayerTracker.cpp
    src/voice/AndroidPermission.cpp
    src/voice/MicSocketSource.cpp
    src/voice/SocketLoopback.cpp
    src/voice/RelayClient.cpp
    src/voice/VoiceSender.cpp
    src/voice/RemoteVoices.cpp
    src/voice/VoiceSession.cpp
    src/voice/WebRtcProcessor.cpp
)

set_target_properties(${MOD_LIBRARY_NAME} PROPERTIES
    OUTPUT_NAME "${MOD_LIBRARY_NAME}"
)

target_include_directories(${MOD_LIBRARY_NAME}
    PRIVATE
        src
        "${LEVI_PRELOADER_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bedrocktools/include"
)

target_compile_options(${MOD_LIBRARY_NAME}
    PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fvisibility=hidden>
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-fvisibility-inlines-hidden>
)

target_link_options(${MOD_LIBRARY_NAME}
    PRIVATE
        "-Wl,-z,max-page-size=16384"
)

find_library(log-lib log REQUIRED)
target_link_libraries(${MOD_LIBRARY_NAME} PRIVATE
    ${log-lib}
    nlohmann_json::nlohmann_json
    Boost::pfr
    magic_enum::magic_enum
    fmt::fmt
    oboe
    opus
)
if(TARGET voicechat_webrtc)
    target_link_libraries(${MOD_LIBRARY_NAME} PRIVATE voicechat_webrtc)
    # Avoid exporting statically linked WebRTC/Abseil symbols into Minecraft or
    # other mods that may load a different version of the same libraries.
    target_link_options(${MOD_LIBRARY_NAME} PRIVATE "-Wl,--exclude-libs,ALL")
endif()

if(TARGET preloader)
    target_link_libraries(${MOD_LIBRARY_NAME} PRIVATE preloader)
else()
    target_link_options(${MOD_LIBRARY_NAME} PRIVATE
        "-Wl,--unresolved-symbols=ignore-all")
endif()

if(NOT CMAKE_STRIP)
    message(FATAL_ERROR "CMAKE_STRIP is required to strip the generated library.")
endif()

add_custom_command(TARGET ${MOD_LIBRARY_NAME} POST_BUILD
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "$<TARGET_FILE:${MOD_LIBRARY_NAME}>"
    COMMENT "Stripping symbols from $<TARGET_FILE_NAME:${MOD_LIBRARY_NAME}>"
    VERBATIM
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/manifest.json"
    @ONLY
)

set(LEVI_PACKAGE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/package")
set(LEVI_PACKAGE_MOD_DIR "${LEVI_PACKAGE_ROOT}/${MOD_ID}")
set(LEVI_PACKAGE_FILE "${CMAKE_CURRENT_BINARY_DIR}/${MOD_ID}-${MOD_VERSION}-${CMAKE_ANDROID_ARCH_ABI}.levipack")
set(LEVI_PACKAGE_ICON_COMMAND)
set(LEVI_PACKAGE_CONFIG_COMMAND
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${LEVI_PACKAGE_CONFIG_DIR}" "${LEVI_PACKAGE_MOD_DIR}/config")
set(LEVI_PACKAGE_LICENSE_COMMAND)
if(TARGET voicechat_webrtc)
    set(_notices "${LEVI_PACKAGE_MOD_DIR}/licenses/webrtc")
    list(APPEND LEVI_PACKAGE_LICENSE_COMMAND
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_notices}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${VOICECHAT_WEBRTC_SOURCE_DIR}/COPYING" "${_notices}/COPYING"
        COMMAND "${CMAKE_COMMAND}" -E copy "${VOICECHAT_WEBRTC_SOURCE_DIR}/AUTHORS" "${_notices}/AUTHORS"
        COMMAND "${CMAKE_COMMAND}" -E copy "${VOICECHAT_WEBRTC_SOURCE_DIR}/webrtc/LICENSE" "${_notices}/LICENSE"
        COMMAND "${CMAKE_COMMAND}" -E copy "${VOICECHAT_WEBRTC_SOURCE_DIR}/webrtc/PATENTS" "${_notices}/PATENTS"
        COMMAND "${CMAKE_COMMAND}" -E copy "${VOICECHAT_WEBRTC_SOURCE_DIR}/webrtc/common_audio/third_party/ooura/LICENSE" "${_notices}/ooura-LICENSE"
        COMMAND "${CMAKE_COMMAND}" -E copy "${VOICECHAT_WEBRTC_SOURCE_DIR}/webrtc/common_audio/third_party/spl_sqrt_floor/LICENSE" "${_notices}/spl-sqrt-LICENSE"
        COMMAND "${CMAKE_COMMAND}" -E copy "${VOICECHAT_WEBRTC_SOURCE_DIR}/webrtc/modules/third_party/fft/LICENSE" "${_notices}/fft-LICENSE"
        COMMAND "${CMAKE_COMMAND}" -E copy "${VOICECHAT_WEBRTC_SOURCE_DIR}/webrtc/third_party/pffft/LICENSE" "${_notices}/pffft-LICENSE"
        COMMAND "${CMAKE_COMMAND}" -E copy "${VOICECHAT_WEBRTC_SOURCE_DIR}/webrtc/third_party/rnnoise/COPYING" "${_notices}/rnnoise-COPYING"
        COMMAND "${CMAKE_COMMAND}" -E copy "${VOICECHAT_ABSEIL_SOURCE_DIR}/LICENSE" "${_notices}/abseil-LICENSE")
endif()

if(NOT MOD_ICON STREQUAL "")
    if(IS_ABSOLUTE "${MOD_ICON}")
        message(FATAL_ERROR "MOD_ICON must be a package-relative path")
    endif()

    if(MOD_ICON MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "MOD_ICON must not contain ..")
    endif()

    set(MOD_ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${MOD_ICON}")
    if(NOT EXISTS "${MOD_ICON_SOURCE}")
        message(FATAL_ERROR "MOD_ICON does not exist: ${MOD_ICON_SOURCE}")
    endif()

    get_filename_component(MOD_ICON_DIR "${MOD_ICON}" DIRECTORY)
    if(NOT MOD_ICON_DIR STREQUAL "")
        list(APPEND LEVI_PACKAGE_ICON_COMMAND
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${LEVI_PACKAGE_MOD_DIR}/${MOD_ICON_DIR}")
    endif()
    list(APPEND LEVI_PACKAGE_ICON_COMMAND
        COMMAND "${CMAKE_COMMAND}" -E copy "${MOD_ICON_SOURCE}" "${LEVI_PACKAGE_MOD_DIR}/${MOD_ICON}")
endif()

add_custom_target(levi_package
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${LEVI_PACKAGE_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LEVI_PACKAGE_MOD_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${MOD_LIBRARY_NAME}>" "${LEVI_PACKAGE_MOD_DIR}/lib${MOD_LIBRARY_NAME}.so"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" "${LEVI_PACKAGE_MOD_DIR}/manifest.json"
    ${LEVI_PACKAGE_ICON_COMMAND}
    ${LEVI_PACKAGE_CONFIG_COMMAND}
    ${LEVI_PACKAGE_LICENSE_COMMAND}
    COMMAND "${CMAKE_COMMAND}" -E chdir "${LEVI_PACKAGE_ROOT}" "${CMAKE_COMMAND}" -E tar "cf" "${LEVI_PACKAGE_FILE}" --format=zip "${MOD_ID}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS ${MOD_LIBRARY_NAME}
    COMMENT "Packaging ${MOD_ID} for LeviLauncher"
    VERBATIM
)
