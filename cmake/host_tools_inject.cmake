get_filename_component(SWITCH_BO2_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
    CALL include "${SWITCH_BO2_ROOT}/tools/xexdump/CMakeLists.txt")
