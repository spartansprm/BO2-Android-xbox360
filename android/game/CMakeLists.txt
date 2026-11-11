cmake_minimum_required(VERSION 3.25)
project(bo2_android_game LANGUAGES C CXX)
add_compile_options($<$<CONFIG:Release>:-g0>)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "The game targets Android ARM64")
endif()
get_filename_component(BO2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)


set(BO2_ANDROID_ARCH_FLAGS "-march=armv8.2-a+fp16+dotprod -mtune=cortex-a78" CACHE STRING
    "CPU flags for the game library (empty = generic armv8-a)")


set(BO2_ANDROID_OPT_LEVEL "-O3" CACHE STRING "Optimization level for the game library sources")


option(BO2_ANDROID_LTO "ThinLTO for the game library" OFF)
separate_arguments(BO2_ANDROID_ARCH_FLAGS_LIST UNIX_COMMAND "${BO2_ANDROID_ARCH_FLAGS}")
set(REXSDK_DIR "${BO2_ROOT}/sdk")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BO2_ANDROID_RUNTIME_DIR "${BO2_ROOT}/work/android-runtime" CACHE PATH "Compiled native SDK")
set(BO2_ANDROID_RECOMP_DIR "${BO2_ROOT}/work/android-recomp" CACHE PATH "Private generated archive")


option(BO2_ANDROID_CAMPAIGN "Build the campaign (default.xex) as libbo2cp.so instead of libbo2game.so" OFF)
set(BO2ZM_SRC "${BO2_ROOT}/app/src")
if(BO2_ANDROID_CAMPAIGN)
  set(BO2_GAME_TARGET bo2cp)
  set(BO2_GENERATED_GAME "${BO2_ROOT}/campaign/generated/default")
  if(NOT EXISTS "${BO2_GENERATED_GAME}/bo2cp_init.h")
    message(FATAL_ERROR "campaign/generated/default is missing: run tools/codegen_android_campaign.sh with your own "
                        "default.xex and TU18 default.xexp first")
  endif()


  set(BO2CP_ANDROID_IMAGE_SHA256 "" CACHE STRING "SHA-256 of the TU18 default.xex image campaign/generated was made from")
  string(LENGTH "${BO2CP_ANDROID_IMAGE_SHA256}" _bo2cp_sha_length)
  if(NOT _bo2cp_sha_length EQUAL 64 OR NOT BO2CP_ANDROID_IMAGE_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Set -DBO2CP_ANDROID_IMAGE_SHA256 to the lowercase SHA-256 of work/dump_tu18/default.image.bin")
  endif()
  include("${BO2_ROOT}/campaign/bo2cp_native_sources.cmake")
  set(game_sources ${BO2CP_NATIVE_SOURCES}


    "${BO2ZM_SRC}/native/render/bo2zm_native_texture_bc.cpp"
    "${BO2ZM_SRC}/native/render/bo2zm_native_texture_upload.cpp"
    "${BO2CP_NATIVE_STUBS}"
    "${BO2_ROOT}/campaign/src/bo2cp_relaunch.cpp" "${BO2_ROOT}/campaign/src/bo2cp_autoexec.cpp"
    "${BO2ZM_SRC}/bo2zm_launch_options.cpp" "${BO2ZM_SRC}/bo2zm_heap_track.cpp"
    "${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp"
    "${REXSDK_DIR}/src/graphics/format/ucode.cpp"
    "${REXSDK_DIR}/src/graphics/register_file.cpp")
  set(BO2_NATIVE_REMAPPED_SOURCES ${BO2CP_NATIVE_SOURCES}
    "${BO2ZM_SRC}/native/render/bo2zm_native_texture_bc.cpp"
    "${BO2ZM_SRC}/native/render/bo2zm_native_texture_upload.cpp")
else()
  set(BO2_GAME_TARGET bo2game)
  set(BO2_GENERATED_GAME "${BO2_ROOT}/app/generated/default_mp")
  include("${BO2_ROOT}/app/bo2zm_sources.cmake")
  list(REMOVE_ITEM BO2ZM_SOURCES src/main.cpp)
  set(game_sources)
  foreach(source IN LISTS BO2ZM_SOURCES)
    if(IS_ABSOLUTE "${source}")
      list(APPEND game_sources "${source}")
    else()
      list(APPEND game_sources "${BO2_ROOT}/app/${source}")
    endif()
  endforeach()
  list(APPEND game_sources "${BO2_ROOT}/app/src/bo2zm_lifecycle.cpp")
endif()
add_library(rexruntime SHARED IMPORTED)
set_target_properties(rexruntime PROPERTIES IMPORTED_LOCATION "${BO2_ANDROID_RUNTIME_DIR}/lib/librexruntime.so")
add_library(SDL3 SHARED IMPORTED)
set_target_properties(SDL3 PROPERTIES IMPORTED_LOCATION "${BO2_ANDROID_RUNTIME_DIR}/sdl3/libSDL3.so")
add_library(recompiled STATIC IMPORTED)
set_target_properties(recompiled PROPERTIES IMPORTED_LOCATION "${BO2_ANDROID_RECOMP_DIR}/libbo2recomp.a")


if(BO2_ANDROID_CAMPAIGN)
  set(BO2_TOUCH_INPUT_SYMBOL sub_821EBCF8)
else()
  set(BO2_TOUCH_INPUT_SYMBOL sub_8225E798)
endif()
execute_process(COMMAND "${CMAKE_NM}" --undefined-only "${BO2_ANDROID_RECOMP_DIR}/libbo2recomp.a"
  RESULT_VARIABLE BO2_TOUCH_NM_RESULT OUTPUT_VARIABLE BO2_TOUCH_ARCHIVE_SYMBOLS ERROR_QUIET)
if(NOT BO2_TOUCH_NM_RESULT EQUAL 0 OR NOT BO2_TOUCH_ARCHIVE_SYMBOLS MATCHES "U ${BO2_TOUCH_INPUT_SYMBOL}([\r\n]|$)")
  message(FATAL_ERROR "The PPC archive bypasses the relative touch hook. Run tools/direct_calls.py for this game's "
                      "generated directory, then rebuild its android/recomp target. An older private archive cannot be reused.")
endif()
add_library(${BO2_GAME_TARGET} SHARED ${game_sources}
  "${REXSDK_DIR}/src/ui/rex_app.cpp" entry.cpp touch_look.cpp content_mount.cpp shader_capture.cpp fault_evidence.cpp)
target_include_directories(${BO2_GAME_TARGET} PRIVATE "${BO2_ROOT}/app" "${BO2_ROOT}/app/src"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${BO2_GENERATED_GAME}" "${REXSDK_DIR}/include" "${REXSDK_DIR}/src"
  "${BO2_ANDROID_RUNTIME_DIR}/sdk/include"
  "${REXSDK_DIR}/src/ui/shaders" "${BO2_ROOT}/android/sdl3/include"
  "${REXSDK_DIR}/thirdparty/xxHash" "${REXSDK_DIR}/thirdparty/simde"
  "${REXSDK_DIR}/thirdparty/fmt/include" "${REXSDK_DIR}/thirdparty/spdlog/include"
  "${REXSDK_DIR}/thirdparty/tomlplusplus/include" "${REXSDK_DIR}/thirdparty/imgui"
  "${REXSDK_DIR}/thirdparty/renderdoc" "${REXSDK_DIR}/thirdparty/vulkan-headers/include")

target_compile_definitions(${BO2_GAME_TARGET} PRIVATE SPDLOG_FMT_EXTERNAL REX_HAS_VULKAN=1 REXGLUE_BUILD_CONFIG="Release"
  REX_HAS_FIDELITYFX_SDK=1)
set_source_files_properties("${BO2_ROOT}/app/src/bo2zm_switch.cpp" PROPERTIES
  COMPILE_DEFINITIONS "BO2ZM_BUILD_ID=\"android-0.55.0\"")
if(NOT BO2_ANDROID_CAMPAIGN AND EXISTS "${BO2_ROOT}/app/generated/default_mp/bo2zm_locals.cpp")
  target_compile_definitions(${BO2_GAME_TARGET} PRIVATE BO2ZM_HAS_LOCALS=1)
endif()
target_compile_options(${BO2_GAME_TARGET} PRIVATE ${BO2_ANDROID_OPT_LEVEL} -g0 -fno-strict-aliasing -fno-char8_t -ffp-contract=off
  ${BO2_ANDROID_ARCH_FLAGS_LIST})
if(BO2_ANDROID_LTO)
  target_compile_options(${BO2_GAME_TARGET} PRIVATE -flto=thin)
  target_link_options(${BO2_GAME_TARGET} PRIVATE -flto=thin)
endif()
target_link_options(${BO2_GAME_TARGET} PRIVATE "-Wl,--no-undefined" "-Wl,-z,max-page-size=16384")


target_link_options(${BO2_GAME_TARGET} PRIVATE "-Wl,--exclude-libs,liblibavcodec.a")
target_link_libraries(${BO2_GAME_TARGET} PRIVATE "-Wl,--whole-archive" recompiled "-Wl,--no-whole-archive" rexruntime SDL3 android log vulkan)
foreach(lib fmt spdlog libavcodec libavutil mspack)
  target_link_libraries(${BO2_GAME_TARGET} PRIVATE "${BO2_ANDROID_RUNTIME_DIR}/lib/lib${lib}.a")
endforeach()
set(BO2ZM_DEVLINK OFF)
set(BO2ZM_APP_SOURCE_DIR "${BO2_ROOT}/app")
set(BO2ZM_APP_TARGET ${BO2_GAME_TARGET})
include("${BO2_ROOT}/app/bo2zm_assets.cmake")
set_source_files_properties("${BO2_ROOT}/app/src/bo2zm_webm.cpp" PROPERTIES INCLUDE_DIRECTORIES "${REXSDK_DIR}/thirdparty/FFmpeg")

set_property(SOURCE "${BO2_ROOT}/app/src/bo2zm_native_limiter.cpp" "${BO2_ROOT}/app/src/bo2zm_player_projection.cpp"
  APPEND PROPERTY COMPILE_OPTIONS -ffp-contract=off)
if(BO2_ANDROID_CAMPAIGN)


  target_include_directories(${BO2_GAME_TARGET} PRIVATE "${BO2_ROOT}/campaign" "${BO2_ROOT}/campaign/src"
    "${BO2ZM_SRC}/native")
  target_compile_definitions(${BO2_GAME_TARGET} PRIVATE BO2CP_NATIVE=1 BO2_ANDROID_CAMPAIGN=1
    BO2CP_ANDROID_IMAGE_SHA256="${BO2CP_ANDROID_IMAGE_SHA256}"
    BO2ZM_NATIVE_CACHE_FOLDER="cache_cp" BO2ZM_SHADER_LIBRARY_FILE="bo2zm_shaders.spvpack")
  set_property(SOURCE ${BO2_NATIVE_REMAPPED_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "-include" "${BO2CP_GAME_REMAP}")
endif()


option(BO2_ANDROID_RUNTIME_SHADERS "Translate shaders missing from the library on the device (glslang)" ON)
set(BO2_KHRONOS_TAG "vulkan-sdk-1.4.304.0" CACHE STRING "Vulkan SDK tag of glslang, SPIRV-Tools and SPIRV-Headers")
if(BO2_ANDROID_RUNTIME_SHADERS)
  include(FetchContent)
  FetchContent_Declare(spirv_headers GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
    GIT_TAG "${BO2_KHRONOS_TAG}" GIT_SHALLOW TRUE)
  FetchContent_Declare(spirv_tools GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
    GIT_TAG "${BO2_KHRONOS_TAG}" GIT_SHALLOW TRUE)
  FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG "${BO2_KHRONOS_TAG}" GIT_SHALLOW TRUE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
  set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
  set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
  set(SPIRV_HEADERS_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
  set(SPIRV_HEADERS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
  set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
  set(ENABLE_OPT ON CACHE BOOL "" FORCE)
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(spirv_headers)
  set(SPIRV-Headers_SOURCE_DIR "${spirv_headers_SOURCE_DIR}")
  FetchContent_MakeAvailable(spirv_tools)
  FetchContent_MakeAvailable(glslang)

  set(BO2_SHADER_COMMON "${BO2_ROOT}/tools/xenosrecomp/translator/shader_common.h")
  file(READ "${BO2_SHADER_COMMON}" BO2_SHADER_COMMON_TEXT)
  string(FIND "${BO2_SHADER_COMMON_TEXT}" ")BO2SC\"" BO2_SC_DELIMITER)
  if(NOT BO2_SC_DELIMITER EQUAL -1)
    message(FATAL_ERROR "shader_common.h contains the raw string delimiter )BO2SC\"")
  endif()
  set(BO2_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(CONFIGURE OUTPUT "${BO2_GENERATED_DIR}/bo2zm_shader_common_text.h"
    CONTENT "// Generated from shader_common.h (do not edit).\n#pragma once\ninline constexpr char kShaderCommonText[] = R\"BO2SC(@BO2_SHADER_COMMON_TEXT@)BO2SC\";\n"
    @ONLY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BO2_SHADER_COMMON}")
  set(BO2_TRANSLATOR_DIR "${BO2_ROOT}/tools/xenosrecomp/translator")
  set(BO2_RENDER_DIR "${BO2_ROOT}/app/src/native/render")
  target_sources(${BO2_GAME_TARGET} PRIVATE "${BO2_RENDER_DIR}/bo2zm_shader_translator.cpp"
    "${BO2_RENDER_DIR}/bo2zm_shader_translator_hlsl.cpp" "${BO2_TRANSLATOR_DIR}/shader_recompiler.cpp")


  set_source_files_properties("${BO2_TRANSLATOR_DIR}/shader_recompiler.cpp" PROPERTIES
    INCLUDE_DIRECTORIES "${BO2_TRANSLATOR_DIR}"
    COMPILE_DEFINITIONS "BO2ZM_RECOMP"
    COMPILE_OPTIONS "-include;${BO2_RENDER_DIR}/bo2zm_xenosrecomp_prelude.h;-w")
  set_source_files_properties("${BO2_RENDER_DIR}/bo2zm_shader_translator_hlsl.cpp" PROPERTIES
    INCLUDE_DIRECTORIES "${BO2_TRANSLATOR_DIR};${BO2_GENERATED_DIR}"
    COMPILE_DEFINITIONS "BO2ZM_RECOMP"
    COMPILE_OPTIONS "-w")
  target_compile_definitions(${BO2_GAME_TARGET} PRIVATE BO2ZM_RUNTIME_SHADERS=1)
  set(BO2_GLSLANG_LIBS glslang glslang-default-resource-limits SPIRV-Tools-opt)
  if(TARGET SPIRV)
    list(APPEND BO2_GLSLANG_LIBS SPIRV)
  endif()
  if(TARGET SPIRV-Tools-static)
    list(APPEND BO2_GLSLANG_LIBS SPIRV-Tools-static)
  endif()
  target_link_libraries(${BO2_GAME_TARGET} PRIVATE ${BO2_GLSLANG_LIBS})
endif()
