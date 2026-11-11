cmake_minimum_required(VERSION 3.22)
project(bo2_android_recomp LANGUAGES CXX)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Private recompilation targets Android ARM64")
endif()
get_filename_component(BO2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)


set(BO2_RECOMP_PROJECT "bo2zm" CACHE STRING "Generated project to archive: bo2zm (default_mp.xex) or bo2cp (default.xex)")
set_property(CACHE BO2_RECOMP_PROJECT PROPERTY STRINGS bo2zm bo2cp)
if(BO2_RECOMP_PROJECT STREQUAL "bo2cp")
  set(GEN "${BO2_ROOT}/campaign/generated/default")
elseif(BO2_RECOMP_PROJECT STREQUAL "bo2zm")
  set(GEN "${BO2_ROOT}/app/generated/default_mp")
else()
  message(FATAL_ERROR "BO2_RECOMP_PROJECT must be bo2zm or bo2cp")
endif()
if(NOT EXISTS "${GEN}/sources.cmake")
  message(FATAL_ERROR "${GEN}/sources.cmake is missing: generate the code first")
endif()
set(BO2_ANDROID_ARCH_FLAGS "-march=armv8.2-a+fp16+dotprod -mtune=cortex-a78" CACHE STRING
    "CPU flags for the recompiled module (empty = generic armv8-a); keep equal to android/game")
separate_arguments(BO2_ANDROID_ARCH_FLAGS_LIST UNIX_COMMAND "${BO2_ANDROID_ARCH_FLAGS}")


option(BO2_RECOMP_RELAXED_FP "Recompiled module: -ffp-contract=off instead of -ffp-model=strict" OFF)


option(BO2_RECOMP_GUEST_MEMORY_PLAIN "Recompiled module: plain guest memory accesses" OFF)
include("${GEN}/sources.cmake")

set(BO2_RECOMP_EXTRA)
if(EXISTS "${GEN}/${BO2_RECOMP_PROJECT}_locals.cpp")
  list(APPEND BO2_RECOMP_EXTRA "${GEN}/${BO2_RECOMP_PROJECT}_locals.cpp")
endif()
add_library(bo2recomp STATIC ${GENERATED_SOURCES} ${BO2_RECOMP_EXTRA})
target_include_directories(bo2recomp PRIVATE
  "${BO2_ROOT}/sdk/include" "${BO2_ROOT}/app/src" "${GEN}"
  "${BO2_ROOT}/sdk/thirdparty/fmt/include" "${BO2_ROOT}/sdk/thirdparty/spdlog/include"
  "${BO2_ROOT}/sdk/thirdparty/simde" "${BO2_ROOT}/sdk/thirdparty/tomlplusplus/include"
  "${BO2_ROOT}/sdk/thirdparty/imgui")
target_compile_definitions(bo2recomp PRIVATE SPDLOG_FMT_EXTERNAL)
if(BO2_RECOMP_RELAXED_FP)
  set(BO2_RECOMP_FP_FLAGS -ffp-contract=off)
else()
  set(BO2_RECOMP_FP_FLAGS -ffp-model=strict)
endif()
target_compile_options(bo2recomp PRIVATE -O2 -g0 -fno-strict-aliasing ${BO2_RECOMP_FP_FLAGS} -fno-char8_t -Wno-unused-variable
  ${BO2_ANDROID_ARCH_FLAGS_LIST})
if(BO2_RECOMP_GUEST_MEMORY_PLAIN)


  find_program(BO2_HOST_PYTHON NAMES python3 python NO_CMAKE_FIND_ROOT_PATH REQUIRED)
  execute_process(COMMAND "${BO2_HOST_PYTHON}" "${BO2_ROOT}/tools/guest_barriers.py" --gen "${GEN}" --check
                  RESULT_VARIABLE _bo2_barriers_rc OUTPUT_VARIABLE _bo2_barriers_out ERROR_VARIABLE _bo2_barriers_err)
  if(NOT _bo2_barriers_rc EQUAL 0)
    message(FATAL_ERROR "BO2_RECOMP_GUEST_MEMORY_PLAIN needs the compiler barriers in the generated sources: "
                        "${_bo2_barriers_err} Run: python tools/guest_barriers.py --gen ${GEN}")
  endif()
  target_compile_definitions(bo2recomp PRIVATE BO2ZM_GUEST_MEMORY_PLAIN=1)
endif()
target_precompile_headers(bo2recomp PRIVATE "${GEN}/${BO2_RECOMP_PROJECT}_pch.h" "${BO2_ROOT}/app/src/bo2zm_guest_memory.h")
