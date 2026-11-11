cmake_minimum_required(VERSION 3.22)
project(bo2_host_codegen LANGUAGES CXX)
get_filename_component(BO2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BO2_HOST_SDK "" CACHE PATH "Verified official ReXGlue v0.10.0 linux-amd64 install")
set(BO2_HOST_SOURCE "" CACHE PATH "ReXGlue c94f5eb source (disassembler headers)")
set(BO2_HOST_INJA "" CACHE PATH "Inja 7d1b460 source")
foreach(required
    "${BO2_HOST_SDK}/lib/librexruntime.so"
    "${BO2_HOST_SOURCE}/thirdparty/disasm/dis-asm.h"
    "${BO2_HOST_INJA}/single_include/inja/inja.hpp")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "Missing pinned dependency: ${required}")
  endif()
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/header_overlay/rex")
file(CREATE_LINK "${BO2_ROOT}/sdk/include/rex/codegen"
    "${CMAKE_CURRENT_BINARY_DIR}/header_overlay/rex/codegen" SYMBOLIC)
set(TEMPLATE_SOURCE_DIR "${BO2_ROOT}/sdk/resources/templates")
set(TEMPLATE_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/templates")
file(MAKE_DIRECTORY "${TEMPLATE_OUTPUT_DIR}")
include("${BO2_ROOT}/sdk/cmake/embed_templates.cmake")
file(GLOB codegen_sources CONFIGURE_DEPENDS
    "${BO2_ROOT}/sdk/src/codegen/*.cpp"
    "${BO2_ROOT}/sdk/src/codegen/ppc/*.cpp"
    "${BO2_ROOT}/sdk/src/codegen/builders/*.cpp")
add_executable(bo2-codegen main.cpp ${codegen_sources}
    "${BO2_ROOT}/sdk/src/rexglue/commands/project_scan.cpp")
set_target_properties(bo2-codegen PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(bo2-codegen PRIVATE SPDLOG_FMT_EXTERNAL)
target_compile_options(bo2-codegen PRIVATE -march=x86-64-v2 -fno-char8_t)
target_include_directories(bo2-codegen PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/header_overlay"
    "${BO2_HOST_SDK}/include"
    "${BO2_HOST_SOURCE}/thirdparty/disasm"
    "${BO2_HOST_INJA}/single_include"
    "${BO2_HOST_INJA}/third_party/include"
    "${BO2_ROOT}/sdk/src/codegen"
    "${BO2_ROOT}/sdk/src/codegen/builders"
    "${BO2_ROOT}/sdk/src/rexglue/commands"
    "${TEMPLATE_OUTPUT_DIR}")
target_link_libraries(bo2-codegen PRIVATE
    "${BO2_HOST_SDK}/lib/librexruntime.so"
    "${BO2_HOST_SDK}/lib/libdisasm.a"
    "${BO2_HOST_SDK}/lib/libxxhash.a"
    "${BO2_HOST_SDK}/lib/libfmt.a")
