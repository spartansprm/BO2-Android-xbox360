cmake_minimum_required(VERSION 3.25)
project(bo2zm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)


include("${CMAKE_CURRENT_LIST_DIR}/bo2zm_sources.cmake")


if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/bo2zm_devlink.cpp")
    option(BO2ZM_DEVLINK "Development consoles: the link to a PC for builds and logs, and CHECK FOR UPDATES" ON)
else()
    set(BO2ZM_DEVLINK OFF CACHE BOOL "Development consoles: the link to a PC (its sources are not in this tree)" FORCE)
endif()
message(STATUS "bo2zm: BO2ZM_DEVLINK ${BO2ZM_DEVLINK}")
if(BO2ZM_DEVLINK)
    list(APPEND BO2ZM_SOURCES src/bo2zm_devlink.cpp src/bo2zm_updates.cpp)
else()
    list(APPEND BO2ZM_SOURCES src/bo2zm_lifecycle.cpp)
endif()

if(WIN32)
    add_executable(bo2zm WIN32 ${BO2ZM_SOURCES})
else()
    add_executable(bo2zm ${BO2ZM_SOURCES})
endif()


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(bo2zm PRIVATE src/bo2zm_linux.cpp)
endif()

if(BO2ZM_DEVLINK AND REXGLUE_PLATFORM_SWITCH)
    target_sources(bo2zm PRIVATE src/bo2zm_devlink_net.cpp src/bo2zm_devlink_screen.cpp)
elseif(BO2ZM_DEVLINK)


    target_sources(bo2zm PRIVATE src/bo2zm_devlink_net.cpp)
    set_source_files_properties(src/bo2zm_devlink_net.cpp PROPERTIES
                                COMPILE_DEFINITIONS "BO2ZM_DEVLINK_NO_ZLIB=1;_CRT_SECURE_NO_WARNINGS")
endif()
if(WIN32)
    target_link_libraries(bo2zm PRIVATE ws2_32)
endif()


rexglue_setup_target(bo2zm GPU_PLUGINS xenos)


set(BO2ZM_LOCALS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default_mp/bo2zm_locals.cpp")
if(EXISTS "${BO2ZM_LOCALS}" AND TARGET bo2zm_recomp)
  target_sources(bo2zm_recomp PRIVATE "${BO2ZM_LOCALS}")
  target_compile_definitions(bo2zm PRIVATE BO2ZM_HAS_LOCALS=1)
endif()


option(BO2ZM_GUEST_MEMORY_PLAIN "Generated code: plain guest loads and stores, with compiler barriers on guest memory"
       OFF)
if(TARGET bo2zm_recomp)
  target_precompile_headers(bo2zm_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/bo2zm_guest_memory.h")
  if(BO2ZM_GUEST_MEMORY_PLAIN)
    target_compile_definitions(bo2zm_recomp PRIVATE BO2ZM_GUEST_MEMORY_PLAIN=1)
  endif()
endif()


set(BO2ZM_PGO "" CACHE STRING "Switch: profile-guided optimization of the generated code: generate, use or empty")
set(BO2ZM_PGO_DIR "" CACHE PATH "Switch, BO2ZM_PGO=use: the folder with the training run's profile")
if(REXGLUE_PLATFORM_SWITCH AND TARGET bo2zm_recomp AND BO2ZM_PGO)
  file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _bo2zm_pgo_prefix)


  set(_bo2zm_pgo_native
      src/native/render/bo2zm_native_render.cpp src/native/render/bo2zm_native_targets.cpp
      src/native/render/bo2zm_native_draws.cpp src/native/render/bo2zm_native_deferred.cpp
      src/native/render/bo2zm_native_shaders.cpp src/native/render/bo2zm_shader_library.cpp
      src/native/render/bo2zm_native_hooks.cpp src/native/render/bo2zm_native_texture_pool.cpp
      src/native/bo2zm_native_system.cpp src/bo2zm_native_audio.cpp src/bo2zm_native_limiter.cpp
      src/bo2zm_native_skin.cpp src/bo2zm_gpu_skin.cpp src/bo2zm_native_cull.cpp src/bo2zm_native_entities.cpp
      src/bo2zm_native_lzx.cpp src/bo2zm_page_writes.cpp src/bo2zm_volume_writes.cpp src/bo2zm_d3d_marker.cpp
      src/bo2zm_d3d_native.cpp src/bo2zm_crt.cpp)
  if(BO2ZM_PGO STREQUAL "generate")


    target_compile_options(bo2zm_recomp PRIVATE -fprofile-generate=/switch/bo2zm/pgo -fno-profile-values
                           -fprofile-update=single "-fprofile-prefix-path=${_bo2zm_pgo_prefix}")
    set_property(SOURCE ${_bo2zm_pgo_native} APPEND PROPERTY COMPILE_OPTIONS -fprofile-generate=/switch/bo2zm/pgo
                 -fno-profile-values -fprofile-update=single "-fprofile-prefix-path=${_bo2zm_pgo_prefix}")
    target_compile_definitions(bo2zm PRIVATE BO2ZM_PGO_GENERATE=1)
    target_link_libraries(bo2zm PRIVATE gcov)
    target_link_options(bo2zm PRIVATE "-Wl,--wrap=fopen")
  elseif(BO2ZM_PGO STREQUAL "use")
    target_compile_options(bo2zm_recomp PRIVATE "-fprofile-use=${BO2ZM_PGO_DIR}"
                           "-fprofile-prefix-path=${_bo2zm_pgo_prefix}" -fprofile-partial-training -fprofile-correction
                           -Wno-missing-profile -Wno-coverage-mismatch)

    set_property(SOURCE ${_bo2zm_pgo_native} APPEND PROPERTY COMPILE_OPTIONS "-fprofile-use=${BO2ZM_PGO_DIR}"
                 "-fprofile-prefix-path=${_bo2zm_pgo_prefix}" -fprofile-partial-training -fprofile-correction
                 -Wno-missing-profile -Wno-coverage-mismatch)
  else()
    message(FATAL_ERROR "BO2ZM_PGO must be generate, use or empty")
  endif()
endif()


option(BO2ZM_DEBUG_LINES "Switch profile builds: -g1 line tables for the native renderer" OFF)
if(REXGLUE_PLATFORM_SWITCH AND BO2ZM_DEBUG_LINES)
  set_property(SOURCE src/native/render/bo2zm_native_render.cpp src/native/render/bo2zm_native_draws.cpp
               src/native/render/bo2zm_native_targets.cpp src/native/render/bo2zm_native_deferred.cpp
               src/native/render/bo2zm_native_shaders.cpp src/native/render/bo2zm_native_texture_pool.cpp
               src/native/render/bo2zm_native_hooks.cpp src/bo2zm_native_audio.cpp src/bo2zm_native_limiter.cpp
               APPEND PROPERTY COMPILE_OPTIONS -g1)
  if(TARGET rexaudio)  # the SDK's audio (the XMA decoder)
    target_compile_options(rexaudio PRIVATE -g1)
  endif()
  target_link_options(bo2zm PRIVATE -g1)
endif()


if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(bo2zm PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()


if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(bo2zm PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

if(TARGET Vulkan::Headers)
    target_link_libraries(bo2zm PRIVATE Vulkan::Headers)
endif()


target_link_libraries(bo2zm PRIVATE $<LINK_ONLY:libavcodec> $<LINK_ONLY:libavutil>)
set_source_files_properties(src/bo2zm_webm.cpp PROPERTIES INCLUDE_DIRECTORIES "${REXSDK_DIR}/thirdparty/FFmpeg")


target_include_directories(bo2zm PRIVATE "${REXSDK_DIR}/src/ui/shaders")
find_path(BO2ZM_XXHASH_INCLUDE xxhash.h HINTS "${REXSDK_DIR}/thirdparty/xxHash" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
target_include_directories(bo2zm PRIVATE "${BO2ZM_XXHASH_INCLUDE}")

set(BO2ZM_APP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BO2ZM_APP_TARGET bo2zm)
include("${CMAKE_CURRENT_LIST_DIR}/bo2zm_assets.cmake")

if(REXGLUE_PLATFORM_SWITCH)
    target_include_directories(bo2zm PRIVATE "${REXSDK_DIR}/src")
endif()


if(REXGLUE_PLATFORM_SWITCH)
    target_sources(bo2zm PRIVATE src/bo2zm_switch_mutex.cpp)
    target_link_options(bo2zm PRIVATE "-Wl,--wrap=pthread_mutex_lock" "-Wl,--wrap=__malloc_lock,--wrap=__malloc_unlock")

    target_link_options(bo2zm PRIVATE "-Wl,--wrap=viOpenDisplay")
endif()


if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(src/bo2zm_native_limiter.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/bo2zm_player_projection.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()


option(BO2ZM_MEMCPY_CALLERS "Switch test build: log the ring thread's memcpy calls by call site" OFF)
if(REXGLUE_PLATFORM_SWITCH AND BO2ZM_MEMCPY_CALLERS)
    set_source_files_properties(src/bo2zm_memcpy_callers.cpp PROPERTIES COMPILE_DEFINITIONS BO2ZM_MEMCPY_CALLERS=1)
    target_link_options(bo2zm PRIVATE "-Wl,--wrap=memcpy,--wrap=memmove,--wrap=memset,--wrap=memcmp")
endif()


option(BO2ZM_HEAP_TRACK "Switch test build: log the C heap's live bytes by caller every minute" OFF)
if(REXGLUE_PLATFORM_SWITCH AND BO2ZM_HEAP_TRACK)
    set_source_files_properties(src/bo2zm_heap_track.cpp PROPERTIES COMPILE_DEFINITIONS BO2ZM_HEAP_TRACK=1
                                COMPILE_OPTIONS -fno-omit-frame-pointer)
    target_link_options(bo2zm PRIVATE "-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc,--wrap=memalign"
                        "-Wl,--wrap=aligned_alloc,--wrap=posix_memalign")
endif()


set(BO2ZM_NRO_ICON "" CACHE FILEPATH "256x256 JPEG icon for bo2zm.nro (hbmenu)")
set(BO2ZM_EXE_ICON "" CACHE FILEPATH "Windows .ico for bo2zm.exe")

if(REXGLUE_PLATFORM_SWITCH)
    set(_bo2zm_icon_arg)
    if(BO2ZM_NRO_ICON AND EXISTS "${BO2ZM_NRO_ICON}")
        set(_bo2zm_icon_arg ICON "${BO2ZM_NRO_ICON}")
    endif()
    rexglue_switch_add_nro(bo2zm
        NAME "Black Ops II 360 NX"
        AUTHOR "AlvaroEPena"
        VERSION "1.0.0"
        ${_bo2zm_icon_arg})
endif()


if(WIN32 AND BO2ZM_EXE_ICON AND EXISTS "${BO2ZM_EXE_ICON}")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bo2zm_icon.rc"
        CONTENT "1 ICON \"@BO2ZM_EXE_ICON@\"\n" @ONLY)
    target_sources(bo2zm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bo2zm_icon.rc")
endif()


set(BO2ZM_BUILD_ID "dev" CACHE STRING "Build identifier shown in the log")
set_source_files_properties(src/bo2zm_switch.cpp PROPERTIES
    COMPILE_DEFINITIONS "BO2ZM_BUILD_ID=\"${BO2ZM_BUILD_ID}\"")
