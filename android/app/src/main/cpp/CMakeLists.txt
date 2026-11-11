cmake_minimum_required(VERSION 3.22)
project(bo2_android_diagnostic LANGUAGES C CXX)

add_compile_definitions(REX_HAS_FIDELITYFX_SDK=1)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "This diagnostic targets Android arm64-v8a only")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN ON CACHE BOOL "" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
get_filename_component(BO2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(BO2_ANDROID_RUNTIME_DIR "" CACHE PATH "Compiled Android SDK runtime")
set(BO2_ANDROID_RECOMP_DIR "" CACHE PATH "Compiled private game archive")
if(BO2_ANDROID_RUNTIME_DIR)
    add_library(SDL3 SHARED IMPORTED)
    set_target_properties(SDL3 PROPERTIES IMPORTED_LOCATION "${BO2_ANDROID_RUNTIME_DIR}/sdl3/libSDL3.so"
        INTERFACE_INCLUDE_DIRECTORIES "${BO2_ROOT}/android/sdl3/include")
    add_library(SDL3::SDL3 ALIAS SDL3)
    add_library(rexruntime SHARED IMPORTED)
    set_target_properties(rexruntime PROPERTIES IMPORTED_LOCATION "${BO2_ANDROID_RUNTIME_DIR}/lib/librexruntime.so")
    if(BO2_ANDROID_RECOMP_DIR)
    add_library(private_recomp STATIC IMPORTED)
    set_target_properties(private_recomp PROPERTIES IMPORTED_LOCATION "${BO2_ANDROID_RECOMP_DIR}/libbo2recomp.a")
    add_library(bo2runtimeprobe SHARED runtime_probe.cpp "${BO2_ROOT}/app/src/bo2zm_crt.cpp" "${BO2_ROOT}/app/src/bo2zm_page_writes.cpp"
        "${BO2_ROOT}/app/src/kernel/xboxkrnl_usbcam.cpp")
    target_include_directories(bo2runtimeprobe PRIVATE "${BO2_ROOT}/sdk/include"
        "${BO2_ROOT}/sdk/thirdparty/fmt/include" "${BO2_ROOT}/sdk/thirdparty/spdlog/include"
        "${BO2_ROOT}/sdk/thirdparty/simde" "${BO2_ROOT}/sdk/thirdparty/tomlplusplus/include"
        "${BO2_ROOT}/sdk/thirdparty/imgui" "${BO2_ROOT}/app/src")
    target_compile_definitions(bo2runtimeprobe PRIVATE SPDLOG_FMT_EXTERNAL BO2_ANDROID_RUNTIME_PROBE=1 REXGLUE_BUILD_CONFIG="Release")
    target_compile_options(bo2runtimeprobe PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
    target_link_options(bo2runtimeprobe PRIVATE "-Wl,--no-undefined" "-Wl,-z,max-page-size=16384")
    target_link_libraries(bo2runtimeprobe PRIVATE "-Wl,--whole-archive" private_recomp "-Wl,--no-whole-archive" rexruntime SDL3::SDL3 android log)
    endif()
else()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../../sdl3" sdl3)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(PROBE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/probe_shaders.h")
set(PROBE_SHADER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tools/compile_android_diagnostic_shaders.py")
add_custom_command(OUTPUT "${PROBE_SHADER_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${PROBE_SHADER_SCRIPT}"
        --ndk "${CMAKE_ANDROID_NDK}"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        --output "${PROBE_SHADER_HEADER}"
    DEPENDS "${PROBE_SHADER_SCRIPT}" shaders/probe.vert shaders/probe.frag
    VERBATIM)
find_program(BO2_DXC NAMES dxc HINTS "$ENV{BO2_DXC_ROOT}/bin" "${BO2_ROOT}/out/dxc/bin" REQUIRED)
if(CMAKE_HOST_WIN32)
    set(BO2_SHADER_HOST windows-x86_64)
    set(BO2_SHADER_SUFFIX .exe)
elseif(CMAKE_HOST_APPLE)
    set(BO2_SHADER_HOST darwin-x86_64)
else()
    set(BO2_SHADER_HOST linux-x86_64)
endif()
set(PORT_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/port_probe_shaders.h")
add_custom_command(OUTPUT "${PORT_SHADER_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${BO2_ROOT}/tools/compile_android_port_shaders.py"
        --dxc "${BO2_DXC}"
        --spirv-val "${CMAKE_ANDROID_NDK}/shader-tools/${BO2_SHADER_HOST}/spirv-val${BO2_SHADER_SUFFIX}"
        --output "${PORT_SHADER_HEADER}"
    DEPENDS "${BO2_ROOT}/tools/compile_android_port_shaders.py" "${BO2_ROOT}/tools/check_ubo_spirv.py"
        "${BO2_ROOT}/tools/xenosrecomp/translator/shader_common.h"
        "${BO2_ROOT}/tools/shader_tests/ubo_vertex.hlsl" "${BO2_ROOT}/tools/shader_tests/ubo_pixel.hlsl"
        "${BO2_ROOT}/tools/shader_tests/texture_vertex.hlsl" "${BO2_ROOT}/tools/shader_tests/texture_pixel.hlsl"
        "${BO2_ROOT}/tools/shader_tests/pipeline_vertex.hlsl" "${BO2_ROOT}/tools/shader_tests/pipeline_pixel.hlsl"
    VERBATIM)
add_library(main SHARED diagnostic.cpp graphics_probe.cpp "${PROBE_SHADER_HEADER}" "${PORT_SHADER_HEADER}"
    "${BO2_ROOT}/app/src/native/render/bo2zm_native_texture_bc.cpp"
    "${BO2_ROOT}/app/src/native/render/bo2zm_native_texture_upload.cpp")
if(BO2_ANDROID_RUNTIME_DIR)
    target_sources(main PRIVATE sdk_provider_probe.cpp resource_probe.cpp zone_export.cpp game_shader_probe.cpp game_shader_jni.cpp
        "${BO2_ROOT}/app/src/native/render/bo2zm_shader_library.cpp"
        "${BO2_ROOT}/app/src/native/render/bo2zm_shader_index.cpp")
    target_include_directories(main SYSTEM PRIVATE "${BO2_ROOT}/sdk/include")
    target_include_directories(main PRIVATE "${BO2_ROOT}/app/src"
        "${BO2_ROOT}/android/game"
        "${BO2_ROOT}/sdk/thirdparty/fmt/include" "${BO2_ROOT}/sdk/thirdparty/spdlog/include"
        "${BO2_ROOT}/sdk/thirdparty/vulkan-headers/include" "${BO2_ROOT}/sdk/thirdparty/renderdoc")
    target_include_directories(main SYSTEM PRIVATE "${BO2_ROOT}/sdk/thirdparty/xxHash")
    target_compile_definitions(main PRIVATE BO2_HAS_SDK_PROVIDER=1 SPDLOG_FMT_EXTERNAL)
    target_link_libraries(main PRIVATE rexruntime)
    target_link_options(main PRIVATE "-Wl,--no-undefined")
endif()
target_include_directories(main PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${BO2_ROOT}/app/src/native/render" "${BO2_ROOT}/tools")
target_compile_options(main PRIVATE -Wall -Wextra -Werror -fno-strict-aliasing)
target_link_options(main PRIVATE "-Wl,-z,max-page-size=16384")
target_link_libraries(main PRIVATE SDL3::SDL3 android log vulkan)
