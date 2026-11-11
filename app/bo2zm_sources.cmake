set(BO2ZM_SOURCES
    src/main.cpp
    src/kernel/xboxkrnl_usbcam.cpp
    src/bo2zm_relaunch.cpp
    src/debug_party.cpp
    src/bo2zm_autoexec.cpp
    src/bo2zm_dlc.cpp
    src/bo2zm_gsc_mods.cpp
    src/bo2zm_mod_hud_fit.cpp
    src/bo2zm_script_manager.cpp
    src/bo2zm_bots.cpp
    src/bo2zm_launch_options.cpp
    src/bo2zm_local_order.cpp
    src/bo2zm_ui.cpp

    src/bo2zm_sound_waits.cpp
    src/bo2zm_commands.cpp
    src/bo2zm_cheats.cpp
    src/bo2zm_cheat_sync.cpp
    src/bo2zm_player_projection.cpp

    src/bo2zm_widescreen.cpp
    src/bo2zm_dvars.cpp
    src/bo2zm_extras.cpp
    src/bo2zm_name.cpp

    src/bo2zm_online.cpp
    src/bo2zm_pc_profiler.cpp
    src/bo2zm_controllers.cpp
    src/bo2zm_pad.cpp
    src/bo2zm_settings.cpp
    src/bo2zm_preset.cpp
    src/bo2zm_benchmark.cpp
    src/bo2zm_heap_track.cpp
    src/bo2zm_memcpy_callers.cpp
    src/bo2zm_debug_options.cpp
    src/bo2zm_test_waits.cpp
    src/bo2zm_waits.cpp
    src/bo2zm_native_audio.cpp
    src/bo2zm_native_limiter.cpp
    src/bo2zm_webm.cpp
    src/bo2zm_native_lzx.cpp
    src/bo2zm_page_writes.cpp
    src/bo2zm_native_skin.cpp
    src/bo2zm_gpu_skin.cpp
    src/bo2zm_native_cull.cpp

    src/bo2zm_native_entities.cpp
    src/bo2zm_stream_alloc.cpp
    src/bo2zm_jobs.cpp

    src/bo2zm_crt.cpp
    src/bo2zm_test_pad.cpp
      src/bo2zm_party_probe.cpp
    src/bo2zm_d3d_trace.cpp
    src/bo2zm_d3d_marker.cpp

    src/bo2zm_d3d_native.cpp
    src/native/bo2zm_native_system.cpp
    src/bo2zm_switch.cpp


    src/bo2zm_debug_watch.cpp

    src/bo2zm_volume_writes.cpp


    src/native/render/bo2zm_native_render.cpp
    src/native/render/bo2zm_native_targets.cpp
    src/native/render/bo2zm_native_draws.cpp
    src/native/render/bo2zm_native_deferred.cpp
    src/native/render/bo2zm_native_shaders.cpp
    src/native/render/bo2zm_shader_library.cpp
    src/native/render/bo2zm_shader_index.cpp
    src/native/render/bo2zm_native_progress.cpp
    src/native/render/bo2zm_native_hooks.cpp
    src/native/render/bo2zm_native_texture_pool.cpp
    src/native/render/bo2zm_native_texture_bc.cpp
    src/native/render/bo2zm_native_texture_upload.cpp
    src/native/render/bo2zm_native_precompile.cpp
    src/native/render/bo2zm_native_capture.cpp
    src/native/render/bo2zm_graphics_settings.cpp
    src/native/bo2zm_render_stubs.cpp


    ${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp
    ${REXSDK_DIR}/src/graphics/format/ucode.cpp
    ${REXSDK_DIR}/src/graphics/register_file.cpp
)
