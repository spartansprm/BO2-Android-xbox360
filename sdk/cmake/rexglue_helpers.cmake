include_guard(GLOBAL)

function(_rexglue_stage_macos_vulkan_runtime target_name)
    if(TARGET Vulkan::Loader AND TARGET MoltenVK::MoltenVK)
        set(_rexglue_vulkan_loader Vulkan::Loader)
        set(_rexglue_moltenvk MoltenVK::MoltenVK)
        set(_rexglue_moltenvk_icd "${REXGLUE_ROOT}/cmake/MoltenVK_icd.json")
    elseif(TARGET rex::vulkan-loader AND TARGET rex::moltenvk)
        set(_rexglue_vulkan_loader rex::vulkan-loader)
        set(_rexglue_moltenvk rex::moltenvk)
        set(_rexglue_moltenvk_icd "${REXGLUE_MOLTENVK_ICD}")
    else()
        message(FATAL_ERROR "rexglue: pinned macOS Vulkan runtime targets are unavailable")
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/share/vulkan/icd.d"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${_rexglue_vulkan_loader}>"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib/libvulkan.1.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${_rexglue_moltenvk}>"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/lib/libMoltenVK.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_rexglue_moltenvk_icd}"
            "$<TARGET_FILE_DIR:${target_name}>/vulkan/share/vulkan/icd.d/MoltenVK_icd.json"
        VERBATIM
    )
endfunction()


function(rexglue_apply_target_settings target_name)
    if(UNIX AND NOT APPLE)

        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            target_link_options(${target_name} PRIVATE -Wl,--no-relax)
            target_compile_options(${target_name} PRIVATE -mcmodel=large)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
            target_compile_options(${target_name} PRIVATE -march=armv8-a)
        endif()
    endif()

    if(NOT MSVC)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            target_compile_options(${target_name} PRIVATE -msse4.1)
        endif()
    endif()
endfunction()


function(rexglue_configure_target target_name)
    cmake_parse_arguments(ARG "" "" "GPU_PLUGINS" ${ARGN})

    if(REXGLUE_PLATFORM_SWITCH)

        rexglue_switch_configure_target(${target_name} ${ARGN})
        return()
    endif()

    target_sources(${target_name} PRIVATE
        ${REXGLUE_SHARE_DIR}/windowed_app_main_sdl.cpp
        ${REXGLUE_SHARE_DIR}/rex_app.cpp)

    target_compile_definitions(${target_name} PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>")

    if(UNIX AND NOT APPLE)
        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH ON
        )
    elseif(APPLE)


        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "@executable_path"
            BUILD_WITH_INSTALL_RPATH ON
        )
    endif()

    rexglue_apply_target_settings(${target_name})

    if(WIN32)


        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target_name}>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:${target_name}>;$<TARGET_FILE_DIR:${target_name}>>"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    elseif(APPLE)


        foreach(_rexglue_runtime_lib rex::runtime rexruntime rex::TracyClient TracyClient)
            if(TARGET ${_rexglue_runtime_lib})
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:${_rexglue_runtime_lib}>
                        $<TARGET_FILE_DIR:${target_name}>
                    VERBATIM
                )
            endif()
        endforeach()
    endif()

    if(WIN32)


        foreach(_fx amd_fidelityfx_vk amd_fidelityfx_dx12)
            if(TARGET ${_fx})
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:${_fx}>
                        $<TARGET_FILE_DIR:${target_name}>
                    VERBATIM
                )
            endif()
        endforeach()
    endif()


    foreach(_plugin IN LISTS ARG_GPU_PLUGINS)
        if(TARGET rexgpu-${_plugin})

            set(_plugin_target rexgpu-${_plugin})
            add_dependencies(${target_name} ${_plugin_target})
        elseif(TARGET rex::gpu-${_plugin})

            set(_plugin_target rex::gpu-${_plugin})
        else()
            message(FATAL_ERROR
                "rexglue_configure_target: unknown GPU plugin '${_plugin}' "
                "(no target rexgpu-${_plugin} or rex::gpu-${_plugin})")
        endif()
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${_plugin_target}>
                $<TARGET_FILE_DIR:${target_name}>
            VERBATIM
        )
        unset(_plugin_target)
    endforeach()

    if(APPLE AND REXGLUE_USE_VULKAN)
        _rexglue_stage_macos_vulkan_runtime(${target_name})
    endif()
endfunction()


function(rexglue_configure_module_target target_name)
    cmake_parse_arguments(ARG "" "HOST" "" ${ARGN})

    if(ARG_HOST)
        set_target_properties(${target_name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
            RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
            ARCHIVE_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${ARG_HOST}>
        )


        cmake_language(EVAL CODE
            "cmake_language(DEFER CALL add_dependencies ${ARG_HOST} ${target_name})")
    elseif(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(${target_name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        )
    endif()

    if(UNIX AND NOT APPLE)
        set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH ON
        )
    endif()

    rexglue_apply_target_settings(${target_name})
endfunction()


function(rexglue_embed_metadata target_name)
    cmake_parse_arguments(ARG "" "DIRECTORY;PREFIX" "" ${ARGN})

    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "rexglue_embed_metadata: target '${target_name}' does not exist")
    endif()
    if(NOT ARG_DIRECTORY)
        message(FATAL_ERROR "rexglue_embed_metadata: DIRECTORY is required")
    endif()
    if(NOT IS_DIRECTORY "${ARG_DIRECTORY}")
        message(FATAL_ERROR "rexglue_embed_metadata: '${ARG_DIRECTORY}' is not a directory")
    endif()

    file(GLOB_RECURSE _rexglue_metadata_files
        CONFIGURE_DEPENDS
        "${ARG_DIRECTORY}/*")

    set(_rexglue_regular_files)
    foreach(_rexglue_file IN LISTS _rexglue_metadata_files)
        if(NOT IS_DIRECTORY "${_rexglue_file}")
            list(APPEND _rexglue_regular_files "${_rexglue_file}")
        endif()
    endforeach()

    if(NOT _rexglue_regular_files)
        message(STATUS "rexglue_embed_metadata: no files found in ${ARG_DIRECTORY}")
        return()
    endif()

    string(MAKE_C_IDENTIFIER "${target_name}" _rexglue_target_id)
    set(_rexglue_output_dir "${CMAKE_CURRENT_BINARY_DIR}/rexglue_embedded_metadata")
    set(_rexglue_output "${_rexglue_output_dir}/${_rexglue_target_id}_embedded_metadata.cpp")
    file(MAKE_DIRECTORY "${_rexglue_output_dir}")

    set(_rexglue_content "// Auto-generated by rexglue_embed_metadata - DO NOT EDIT\n")
    string(APPEND _rexglue_content
        "#include <cstddef>\n"
        "#include <cstdint>\n"
        "#include <rex/embedded_metadata.h>\n\n"
        "namespace {\n\n")

    set(_rexglue_count 0)
    foreach(_rexglue_file IN LISTS _rexglue_regular_files)
        file(RELATIVE_PATH _rexglue_rel "${ARG_DIRECTORY}" "${_rexglue_file}")
        string(REPLACE "\\" "/" _rexglue_rel "${_rexglue_rel}")
        if(ARG_PREFIX)
            string(REPLACE "\\" "/" _rexglue_prefix "${ARG_PREFIX}")
            string(REGEX REPLACE "/$" "" _rexglue_prefix "${_rexglue_prefix}")
            set(_rexglue_asset_path "${_rexglue_prefix}/${_rexglue_rel}")
        else()
            set(_rexglue_asset_path "${_rexglue_rel}")
        endif()
        string(REPLACE "\"" "\\\"" _rexglue_asset_path "${_rexglue_asset_path}")

        string(MD5 _rexglue_asset_id "${_rexglue_asset_path}")
        file(READ "${_rexglue_file}" _rexglue_hex HEX)
        string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1," _rexglue_bytes "${_rexglue_hex}")

        string(APPEND _rexglue_content
            "const std::uint8_t kAsset_${_rexglue_asset_id}[] = {${_rexglue_bytes}};\n"
            "const bool kRegistered_${_rexglue_asset_id} = "
            "::rex::RegisterEmbeddedMetadataAsset(\"${_rexglue_asset_path}\", "
            "kAsset_${_rexglue_asset_id}, sizeof(kAsset_${_rexglue_asset_id}));\n\n")
        math(EXPR _rexglue_count "${_rexglue_count} + 1")
    endforeach()

    string(APPEND _rexglue_content "}  // namespace\n")
    file(WRITE "${_rexglue_output}" "${_rexglue_content}")
    target_sources(${target_name} PRIVATE "${_rexglue_output}")
    message(STATUS "Embedded ${_rexglue_count} metadata asset(s) into ${target_name}")
endfunction()
