set(BO2ZM_UI_LUA "${BO2ZM_APP_SOURCE_DIR}/src/ui/menus.lua")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BO2ZM_UI_LUA}")
file(READ "${BO2ZM_UI_LUA}" BO2ZM_UI_LUA_TEXT)

if(NOT BO2ZM_DEVLINK)
    string(FIND "${BO2ZM_UI_LUA_TEXT}" "local UpdatesButton = true\n" _bo2zm_updates_at)
    if(_bo2zm_updates_at LESS 0)
        message(FATAL_ERROR "src/ui/menus.lua: the line \"local UpdatesButton = true\" is missing")
    endif()
    string(REPLACE "local UpdatesButton = true\n" "local UpdatesButton = false\n" BO2ZM_UI_LUA_TEXT
           "${BO2ZM_UI_LUA_TEXT}")
endif()


string(REGEX MATCHALL "\nlocal " BO2ZM_UI_LUA_TOP_LOCALS "${BO2ZM_UI_LUA_TEXT}")
list(LENGTH BO2ZM_UI_LUA_TOP_LOCALS BO2ZM_UI_LUA_TOP_LOCAL_COUNT)
if(BO2ZM_UI_LUA_TOP_LOCAL_COUNT GREATER 80)
    message(FATAL_ERROR "src/ui/menus.lua has ${BO2ZM_UI_LUA_TOP_LOCAL_COUNT} top-level local lines (at most 80): put "
                        "helpers that only one section uses in a do ... end block (see the top of menus.lua)")
endif()
configure_file("${BO2ZM_APP_SOURCE_DIR}/src/ui/bo2zm_ui_lua.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated_ui/bo2zm_ui_lua.h" @ONLY)


target_include_directories(${BO2ZM_APP_TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_ui")
