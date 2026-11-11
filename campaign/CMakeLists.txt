cmake_minimum_required(VERSION 3.25)
project(bo2cp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)


set(BO2CP_SOURCES
    src/main.cpp
    src/bo2cp_relaunch.cpp
    src/bo2cp_autoexec.cpp
    ../app/src/bo2zm_launch_options.cpp
    ../app/src/bo2zm_heap_track.cpp
)


option(BO2CP_NATIVE "Build the native renderer into bo2cp" ON)
set(BO2ZM_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../app/src")
if(BO2CP_NATIVE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/bo2cp_native_sources.cmake")
    list(APPEND BO2CP_SOURCES ${BO2CP_NATIVE_SOURCES}
        src/native/bo2cp_native_stubs.cpp
        ${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp
        ${REXSDK_DIR}/src/graphics/format/ucode.cpp
        ${REXSDK_DIR}/src/graphics/register_file.cpp)


    set(BO2CP_UI_LUA "${BO2ZM_SRC}/ui/menus.lua")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BO2CP_UI_LUA}")
    file(READ "${BO2CP_UI_LUA}" BO2ZM_UI_LUA_TEXT)
    configure_file("${BO2ZM_SRC}/ui/bo2zm_ui_lua.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated_ui/bo2zm_ui_lua.h" @ONLY)
    set_source_files_properties(${BO2CP_NATIVE_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/native/bo2cp_game_remap.h")

    string(TIMESTAMP BO2CP_BUILD_TIME "%Y%m%d-%H%M%S")
    set_property(SOURCE ${BO2ZM_SRC}/bo2zm_switch.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS "BO2ZM_BUILD_ID=\"bo2cp-${BO2CP_BUILD_TIME}\"")


    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set_property(SOURCE ${BO2ZM_SRC}/bo2zm_native_limiter.cpp APPEND PROPERTY COMPILE_OPTIONS -ffp-contract=off)
    endif()
endif()

if(WIN32)
    add_executable(bo2cp WIN32 ${BO2CP_SOURCES})
else()
    add_executable(bo2cp ${BO2CP_SOURCES})
endif()


set(BO2CP_EXE_ICON "" CACHE FILEPATH "Windows .ico for bo2cp.exe")
if(WIN32 AND BO2CP_EXE_ICON AND EXISTS "${BO2CP_EXE_ICON}")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bo2cp_icon.rc"
        CONTENT "1 ICON \"@BO2CP_EXE_ICON@\"\n" @ONLY)
    target_sources(bo2cp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bo2cp_icon.rc")
endif()

rexglue_setup_target(bo2cp GPU_PLUGINS xenos)


if(REXGLUE_PLATFORM_SWITCH AND BO2CP_NATIVE)
    target_sources(bo2cp PRIVATE ${BO2ZM_SRC}/bo2zm_switch_mutex.cpp)
    target_link_options(bo2cp PRIVATE "-Wl,--wrap=pthread_mutex_lock" "-Wl,--wrap=__malloc_lock,--wrap=__malloc_unlock"
                        "-Wl,--wrap=viOpenDisplay")
endif()


option(BO2CP_GUEST_MEMORY_PLAIN "Generated code: plain guest loads and stores, with compiler barriers on guest memory"
       OFF)
if(TARGET bo2cp_recomp)
    target_precompile_headers(bo2cp_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../app/src/bo2zm_guest_memory.h")
    if(BO2CP_GUEST_MEMORY_PLAIN)
        target_compile_definitions(bo2cp_recomp PRIVATE BO2ZM_GUEST_MEMORY_PLAIN=1)
    endif()
endif()


option(BO2CP_HEAP_TRACK "Switch test build: log the C heap's live bytes by caller every minute" OFF)
if(REXGLUE_PLATFORM_SWITCH AND BO2CP_HEAP_TRACK)
    set_source_files_properties(../app/src/bo2zm_heap_track.cpp PROPERTIES COMPILE_DEFINITIONS BO2ZM_HEAP_TRACK=1
                                COMPILE_OPTIONS -fno-omit-frame-pointer)
    target_link_options(bo2cp PRIVATE "-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc,--wrap=memalign"
                        "-Wl,--wrap=aligned_alloc,--wrap=posix_memalign")
endif()


if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(bo2cp PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()


if(REXGLUE_PLATFORM_SWITCH)
    rexglue_switch_add_nro(bo2cp
        NAME "Black Ops II 360 NX Campaign"
        AUTHOR "AlvaroEPena"
        VERSION "1.0.0")
endif()

if(BO2CP_NATIVE)
    target_compile_definitions(bo2cp PRIVATE BO2CP_NATIVE=1)
    target_include_directories(bo2cp PRIVATE "${BO2ZM_SRC}" "${BO2ZM_SRC}/native" "${REXSDK_DIR}/src/ui/shaders"
                               "${CMAKE_CURRENT_BINARY_DIR}/generated_ui")
    find_path(BO2CP_XXHASH_INCLUDE xxhash.h HINTS "${REXSDK_DIR}/thirdparty/xxHash" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    target_include_directories(bo2cp PRIVATE "${BO2CP_XXHASH_INCLUDE}")
    if(REXGLUE_PLATFORM_SWITCH)
        target_include_directories(bo2cp PRIVATE "${REXSDK_DIR}/src")
    endif()
    if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
        target_include_directories(bo2cp PRIVATE $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
    if(TARGET Vulkan::Headers)
        target_link_libraries(bo2cp PRIVATE Vulkan::Headers)
    endif()

    target_link_libraries(bo2cp PRIVATE $<LINK_ONLY:libavcodec> $<LINK_ONLY:libavutil>)
    set_source_files_properties(${BO2ZM_SRC}/bo2zm_webm.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${REXSDK_DIR}/thirdparty/FFmpeg")

    if(WIN32)
        target_link_options(bo2cp PRIVATE "LINKER:/errorlimit:0")
    endif()
endif()
