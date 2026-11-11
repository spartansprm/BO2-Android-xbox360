cmake_minimum_required(VERSION 3.25)

if(APPLE)
    set(REXGLUE_LANGUAGES C CXX OBJC OBJCXX)
else()
    set(REXGLUE_LANGUAGES C CXX)
endif()

project(ReXGlue
    VERSION 0.10.0
    LANGUAGES ${REXGLUE_LANGUAGES}
    DESCRIPTION "Xbox 360 Recompilation SDK"
)


option(REXGLUE_BUILD_TESTS       "Build PPC instruction and unit tests" OFF)


option(REXGLUE_BUILD_HOST_TOOLS  "Build the host code generator tools" ON)
option(REXGLUE_RUNTIME_ONLY "Build a headless runtime without the GPU plugin" OFF)
option(REXGLUE_ENABLE_SANITIZERS "Enable UndefinedBehaviorSanitizer" OFF)
option(REXGLUE_ENABLE_FIDELITYFX "Enable optional AMD FidelityFX integration (experimental)" OFF)
option(REXGLUE_ENABLE_TRACY                "Enable Tracy profiler integration" ON)
option(REXGLUE_ENABLE_PERF_COUNTERS        "Enable lightweight perf counter registry and overlay" ON)
option(REXGLUE_PLATFORM_SWITCH             "Build for Nintendo Switch (devkitA64 + libnx; set by the toolchain file)" OFF)

if(REXGLUE_ENABLE_TRACY)
    add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()
if(REXGLUE_ENABLE_PERF_COUNTERS)
    add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()


if(WIN32)
    option(REXGLUE_USE_D3D12  "Enable D3D12 graphics backend" ON)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" OFF)
else()
    set(REXGLUE_USE_D3D12 OFF)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" ON)
endif()

if(NOT REXGLUE_RUNTIME_ONLY AND NOT REXGLUE_USE_D3D12 AND NOT REXGLUE_USE_VULKAN)
    message(FATAL_ERROR "At least one graphics backend must be enabled (REXGLUE_USE_D3D12 or REXGLUE_USE_VULKAN)")
endif()

if (REXGLUE_ENABLE_FIDELITYFX AND
    NOT (REXGLUE_USE_VULKAN OR REXGLUE_USE_D3D12))
    message(FATAL_ERROR
        "REXGLUE_ENABLE_FIDELITYFX requires at least one graphics backend")
endif()


set(REXGLUE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")


include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_helpers.cmake)
if(REXGLUE_PLATFORM_SWITCH)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_switch.cmake)
endif()

if(UNIX AND NOT APPLE)


    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    list(APPEND CMAKE_INSTALL_RPATH
        "$ORIGIN"
        "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
elseif(APPLE)


    list(APPEND CMAKE_INSTALL_RPATH
        "@loader_path"
        "@loader_path/../${CMAKE_INSTALL_LIBDIR}"
    )
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
endif()


if(REXGLUE_PLATFORM_SWITCH)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "The Nintendo Switch build uses devkitA64's GCC (switch-devkitA64.cmake toolchain)")
    endif()
elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
    message(FATAL_ERROR "ReXGlue requires the Clang compiler (LLVM Clang or AppleClang). Please set CMAKE_CXX_COMPILER to clang++")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "18.0")
        message(FATAL_ERROR "ReXGlue requires Clang 18 or newer (found ${CMAKE_CXX_COMPILER_VERSION})")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.0")
        message(FATAL_ERROR "ReXGlue requires AppleClang 16 or newer / Xcode 16+ for C++23 support (found ${CMAKE_CXX_COMPILER_VERSION})")
    endif()
endif()


if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ReXGlue only supports 64‑bit architectures (x86_64 or ARM64)")
endif()


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DEBUG_POSTFIX d)
set(CMAKE_RELWITHDEBINFO_POSTFIX rd)


function(rex_config_postfix target)
    set_target_properties(${target} PROPERTIES
        DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}"
        RELWITHDEBINFO_POSTFIX "${CMAKE_RELWITHDEBINFO_POSTFIX}")
endfunction()

if(NOT WIN32)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64" AND NOT APPLE)


        add_compile_options(-mcmodel=large)
    else()
        add_compile_options(-mcmodel=small)
    endif()

    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()


add_compile_options(
    -fno-strict-aliasing


    $<IF:$<CXX_COMPILER_ID:GNU>,-ffp-contract=off,-ffp-model=strict>
    $<$<CONFIG:DEBUG>:-g>
    $<$<CONFIG:DEBUG>:-O0>
    $<$<CONFIG:RELEASE>:-O3>
    $<$<CONFIG:RELEASE>:-DNDEBUG>
    -fno-char8_t
)


set(REX_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES REX_OSX_ARCH_COUNT)
    if(REX_OSX_ARCH_COUNT GREATER 1)
        message(FATAL_ERROR "ReXGlue macOS presets expect a single CMAKE_OSX_ARCHITECTURES value")
    endif()
    list(GET CMAKE_OSX_ARCHITECTURES 0 REX_TARGET_PROCESSOR)
endif()

if(REXGLUE_PLATFORM_SWITCH)


    set(REX_PLATFORM "switch-arm64")

    include_directories(BEFORE SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/switch_compat)
elseif(WIN32)
    if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(REX_PLATFORM "win-amd64")
    elseif(REX_TARGET_PROCESSOR MATCHES "aarch64|ARM64|arm64")
        set(REX_PLATFORM "win-arm64")
    else()
        message(FATAL_ERROR "Unsupported Windows architecture: ${REX_TARGET_PROCESSOR}")
    endif()
    add_compile_definitions(REX_PLATFORM_WINDOWS=1)
elseif(ANDROID)
    set(REX_PLATFORM "android-arm64")
    add_compile_definitions(REX_PLATFORM_ANDROID=1 REX_PLATFORM_LINUX=1)
elseif(UNIX AND NOT APPLE)
    if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(REX_PLATFORM "linux-amd64")
    elseif(REX_TARGET_PROCESSOR MATCHES "aarch64|ARM64|arm64")
        set(REX_PLATFORM "linux-arm64")
    else()
        set(REX_PLATFORM "linux-${REX_TARGET_PROCESSOR}")
    endif()
    add_compile_definitions(REX_PLATFORM_LINUX=1)
elseif(APPLE)
    if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(REX_PLATFORM "mac-amd64")
    elseif(REX_TARGET_PROCESSOR MATCHES "aarch64|ARM64|arm64")
        set(REX_PLATFORM "mac-arm64")
    else()
        set(REX_PLATFORM "mac-${REX_TARGET_PROCESSOR}")
    endif()
    add_compile_definitions(REX_PLATFORM_MAC=1)
else()
    message(FATAL_ERROR "Unsupported platform. ReXGlue supports Windows, Linux, and macOS only.")
endif()


add_compile_definitions(REXGLUE_BUILD_CONFIG="$<CONFIG>")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rex_pch.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rex_version.cmake)
rex_resolve_version(REXGLUE_FULL_VERSION
    FLOOR_MAJOR ${PROJECT_VERSION_MAJOR}
    FLOOR_MINOR ${PROJECT_VERSION_MINOR})


string(REGEX REPLACE "-.*$" "" REXGLUE_NUMERIC_VERSION "${REXGLUE_FULL_VERSION}")


string(REGEX MATCH "^([0-9]+)\\.([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?" _ "${REXGLUE_NUMERIC_VERSION}")
set(REXGLUE_DERIVED_MAJOR "${CMAKE_MATCH_1}")
set(REXGLUE_DERIVED_MINOR "${CMAKE_MATCH_2}")
if(CMAKE_MATCH_4)
    set(REXGLUE_DERIVED_PATCH "${CMAKE_MATCH_4}")
else()
    set(REXGLUE_DERIVED_PATCH "0")
endif()
if(CMAKE_MATCH_6)
    set(REXGLUE_DERIVED_TWEAK "${CMAKE_MATCH_6}")
else()
    set(REXGLUE_DERIVED_TWEAK "0")
endif()

set(REXGLUE_FLOOR_VERSION
    "${REXGLUE_DERIVED_MAJOR}.${REXGLUE_DERIVED_MINOR}.${REXGLUE_DERIVED_PATCH}")

rex_version_channel(REXGLUE_VERSION_CHANNEL VERSION "${REXGLUE_FULL_VERSION}")


string(TIMESTAMP REXGLUE_BUILD_TIMESTAMP "%Y%m%d_%H%M")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/rex/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/rex/version.h
    @ONLY
)


if(NOT REXGLUE_OUTPUT_DIR)
    set(REXGLUE_OUTPUT_DIR "${REXGLUE_ROOT}/out/${REX_PLATFORM}")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REXGLUE_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${REXGLUE_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${REXGLUE_OUTPUT_DIR}")


if(REXGLUE_ENABLE_SANITIZERS)
    message(STATUS "Enabling UndefinedBehaviorSanitizer (UBSan only - ASan conflicts with custom memory mapping)")

    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
endif()

if(REXGLUE_BUILD_TESTS)
    list(APPEND CMAKE_MODULE_PATH "${REXGLUE_ROOT}/cmake")
    list(APPEND CMAKE_MODULE_PATH "${REXGLUE_ROOT}/thirdparty/catch2/extras")
endif()


add_subdirectory(thirdparty)


add_compile_options(
    -Wall
    -Wextra
)

include_directories(${REXGLUE_ROOT}/include)

add_subdirectory(src/core)
add_subdirectory(src/filesystem)
add_subdirectory(src/ui)
add_subdirectory(src/input)
add_subdirectory(src/audio)
if(NOT REXGLUE_RUNTIME_ONLY)
    add_subdirectory(src/graphics)
endif()
add_subdirectory(src/system)
add_subdirectory(src/kernel)
if(NOT REXGLUE_PLATFORM_SWITCH AND REXGLUE_BUILD_HOST_TOOLS)


    add_subdirectory(src/codegen)
    add_subdirectory(src/rexglue)
endif()


if(REXGLUE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(cmake/rexglue_link_audit.cmake)


message(STATUS "===========================================")
message(STATUS "ReXGlue Configuration Summary:")
message(STATUS "  Version: ${REXGLUE_FULL_VERSION} (${REXGLUE_VERSION_CHANNEL})")
message(STATUS "  Platform: ${REX_PLATFORM}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Configurations: ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  Output Dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Graphics: D3D12=${REXGLUE_USE_D3D12} Vulkan=${REXGLUE_USE_VULKAN}")
message(STATUS "  FidelityFX: ${REXGLUE_ENABLE_FIDELITYFX}")
message(STATUS "  Tracy: ${REXGLUE_ENABLE_TRACY}")
message(STATUS "  Perf Counters: ${REXGLUE_ENABLE_PERF_COUNTERS}")
message(STATUS "===========================================")


if(PROJECT_IS_TOP_LEVEL)
    include(cmake/rexglue_devenv.cmake)
    include(cmake/rexglue_install.cmake)
else()


    set(REXGLUE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ui" PARENT_SCOPE)
    set(REXGLUE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_helpers.cmake)
endif()
