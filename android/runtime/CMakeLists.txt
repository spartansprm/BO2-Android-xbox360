cmake_minimum_required(VERSION 3.25)
project(bo2_android_runtime LANGUAGES C CXX ASM)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Runtime supports Android ARM64 only")
endif()
get_filename_component(BO2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(REXGLUE_RUNTIME_ONLY ON CACHE BOOL "" FORCE)
set(REXGLUE_BUILD_HOST_TOOLS OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "" FORCE)
option(BO2_ANDROID_VULKAN_UI "Build the real SDK native Vulkan provider (no GPU emulation plugin)" ON)
set(REXGLUE_USE_VULKAN ${BO2_ANDROID_VULKAN_UI} CACHE BOOL "" FORCE)
set(REXGLUE_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/lib" CACHE PATH "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO ON CACHE BOOL "" FORCE)
set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
add_link_options("-Wl,-z,max-page-size=16384")

set(BO2_ANDROID_ARCH_FLAGS "-march=armv8.2-a+fp16+dotprod -mtune=cortex-a78" CACHE STRING
    "CPU flags for the runtime (empty = generic armv8-a); keep equal to android/game")
separate_arguments(BO2_ANDROID_ARCH_FLAGS_LIST UNIX_COMMAND "${BO2_ANDROID_ARCH_FLAGS}")
add_compile_options(${BO2_ANDROID_ARCH_FLAGS_LIST})
add_subdirectory("${BO2_ROOT}/android/sdl3" sdl3)


add_compile_definitions(REX_HAS_FIDELITYFX_SDK=1)
add_subdirectory("${BO2_ROOT}/sdk" sdk)
target_link_options(rexruntime PRIVATE "-Wl,--no-undefined")

if(BO2_ANDROID_VULKAN_UI)


  add_library(bo2renderer_port OBJECT
    "${BO2_ROOT}/app/src/native/bo2zm_native_system.cpp"
    "${BO2_ROOT}/app/src/native/render/bo2zm_native_render.cpp"
    "${BO2_ROOT}/app/src/native/render/bo2zm_native_draws.cpp"
    "${BO2_ROOT}/app/src/native/render/bo2zm_native_texture_bc.cpp"
    "${BO2_ROOT}/app/src/native/render/bo2zm_native_texture_upload.cpp")
  target_link_libraries(bo2renderer_port PRIVATE rexui rexruntime)
  target_include_directories(bo2renderer_port PRIVATE "${BO2_ROOT}/app/src" "${BO2_ROOT}/sdk/src"
    "${BO2_ROOT}/sdk/thirdparty/xxHash")
  target_compile_definitions(bo2renderer_port PRIVATE SPDLOG_FMT_EXTERNAL)
  target_compile_options(bo2renderer_port PRIVATE -fno-strict-aliasing -fno-char8_t)
endif()
