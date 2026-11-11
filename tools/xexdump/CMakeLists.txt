add_executable(xexdump "${CMAKE_CURRENT_LIST_DIR}/xexdump.cpp")
target_include_directories(xexdump PRIVATE ${REXGLUE_ROOT}/src)
target_link_libraries(xexdump PRIVATE rexcodegen)
set_target_properties(xexdump PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/switch_bo2")
