


#include "bo2zm_native_draws.h"
#include "bo2zm_native_v040.h"
#include "bo2zm_native_v046.h"
#include "bo2zm_native_v055.h"
#include "bo2zm_native_edram.h"
#include "bo2zm_native_resolution.h"
#include "bo2zm_display_state.h"
#include "bo2zm_native_performance.h"
#include "bo2zm_native_pixel_probe.h"
#include "bo2zm_native_blend.h"
#include "bo2zm_native_scene_sync.h"
#include "bo2zm_native_scene_precise_patch.h"
#include "../bo2zm_render_formats.h"
#include "../bo2zm_native_system.h"
#include "../shaders/bo2zm_debug_magenta_ps.h"
#include "bo2zm_hitch_waits.h"

#include "bo2zm_native_vertex_dedupe.h"
#include "bo2zm_native_deferred.h"
#include "bo2zm_native_draw_batch.h"
#include "bo2zm_native_shader_probe.h"
#include "../shaders/bo2zm_probe_flat_ps.h"
#include "bo2zm_native_texture_pool.h"
#include "bo2zm_native_texture_mips.h"
#include "bo2zm_native_texture_upload.h"
#include "bo2zm_native_shader_profile.h"
#include "bo2zm_native_precompile.h"
#include "bo2zm_map_pipeline_phase.h"
#include "bo2zm_native_cache_paths.h"

#include "bo2zm_graphics_settings.h"
#include "../../bo2zm_preset.h"
#include "../../bo2zm_benchmark.h"
#include "../../bo2zm_debug_options.h"
#include "../../bo2zm_debug_watch.h"
#include "../../bo2zm_page_writes.h"
#include "../../bo2zm_volume_writes.h"
#include "../../bo2zm_gpu_skin.h"
#include "../../bo2zm_cheats.h"
#include "../../bo2zm_switch.h"
#include "shaders/bo2zm_gpu_skin_cs.h"
#if __has_include("bo2zm_native_glow_energy_spirv.h") && __has_include("bo2zm_native_smooth_glow_spirv.h")
#include "bo2zm_native_glow_energy_spirv.h"
#include "bo2zm_native_smooth_glow_spirv.h"
#else


#define BO2ZM_NO_VARIANTS_GLOW 1
static const uint32_t kSpirvGlowEnergy[1] = {0};
static const uint32_t kSpirvGlowSmooth[1] = {0};
#endif
#include "bo2zm_native_shaders.h"
#include "bo2zm_native_hooks.h"
#include "bo2zm_reflection_demand.h"
#include "bo2zm_shader_library.h"

#include <rex/cvar.h>
REXCVAR_DECLARE(bool, bo2zm_native_scene_sync);
REXCVAR_DECLARE(bool, bo2zm_native_scene_sync_precise);
REXCVAR_DECLARE(int32_t, bo2zm_native_scene_sync_mask);
REXCVAR_DECLARE(bool, bo2zm_native_scene_pass_dependencies);
REXCVAR_DECLARE(int32_t, bo2zm_native_scene_pass_mode);
#include <rex/filesystem.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>
#include <rex/ui/vulkan/util.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>


#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

#if REX_PLATFORM_SWITCH


#include "core/threading_switch.h"
#endif


REXCVAR_DEFINE_BOOL(bo2zm_native_fast_tiling, true, "BO2",
                    "Native renderer: textures untiled 16 bytes at a time with the row computed "
                    "once. The first 2000 levels are checked against the usual path and, if one differs, it turns "
                    "itself off. false = the usual path, block by block");


REXCVAR_DEFINE_BOOL(bo2zm_native_framebuffers_forget_views, true, "BO2",
                    "Native renderer: when a C2 image is destroyed, the framebuffers that use its "
                    "view are destroyed too, so a reused handle does not return an old one. false = as before");

REXCVAR_DEFINE_INT32(bo2zm_native_hash_kb_per_frame, 6144, "BO2",
                     "Native renderer: KB of STABLE textures the ring checks again at most in one "
                     "frame; the ones that do not fit wait 1-2 frames (8 times in a row at most). Spreads out the "
                     "checks that used to fall on the same frame. 0 = no limit, as before");

REXCVAR_DEFINE_BOOL(bo2zm_native_compact_bc, true, "BO2",
                    "UBO-only renderer: decode BC4/BC5 to R8/RG8 when sampled linear filtering and transfer "
                    "are supported. Same texels and view swizzles, with less upload traffic and texture memory; "
                    "false restores RGBA8 for comparison")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_INT32(bo2zm_native_hash_sampling, 8, "BO2",
                     "Native renderer: STABLE textures are rechecked with the hash of a sample "
                     "(first and last 4 KB block and 1 in 8, base and mips) instead of all their bytes; if the sample "
                     "changes, the full path follows. N = 1 in N rechecks of each texture is still full, and the first "
                     "3000 compute both hashes: a single disagreement turns it off. 0 = always the full hash, as "
                     "before")
    .range(0, 64);

REXCVAR_DEFINE_BOOL(bo2zm_shadows_without_vegetation, true, "BO2",
                    "Do not draw alpha-tested geometry into the shadow map: trees, bushes and fences. They are many "
                    "of the pass's draws but few of its triangles, so it saves little GPU time and some CPU time. "
                    "Trees stop casting shadows");


REXCVAR_DEFINE_BOOL(bo2zm_native_no_blur, true, "BO2",
                    "Remove the radial blur from a final composite the renderer recognizes by hash (no BO2 shader "
                    "has it, so this does nothing in BO2). You lose the blur at the edges of the screen: the image is "
                    "sharper");

REXCVAR_DEFINE_BOOL(bo2zm_native_skip_shadows, false, "BO2",
                    "Native renderer (FPS test): does not record the draws into a depth-only target with a pitch of "
                    "1600 or more (BO2's 560-wide shadow target is not one); visible clipping");


REXCVAR_DEFINE_STRING(bo2zm_native_dump_texture, "", "BO2",
                      "Diagnostics: dump the guest bytes of the texture at this physical address (hex) once")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

std::atomic<uint64_t> g_bo2zm_dump_texture{0};
REXCVAR_DEFINE_INT32(bo2zm_native_dump_texture_after_s, 0, "BO2",
                     "Diagnostics: bo2zm_native_dump_texture waits this many seconds after startup")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_incremental_volume, true, "BO2",
                    "Native renderer: tiled 3D textures the game rewrites piecemeal (the model lighting "
                    "volume, 512x256x4) are updated by 4 KB pages: only the rows the changed pages touch are untiled "
                    "and uploaded. Checked against the whole read and turns itself off at the first disagreement. false "
                    "= read and upload the whole texture every time");
REXCVAR_DEFINE_INT32(bo2zm_native_volume_by_hash, 1, "BO2",
                     "Native renderer: how bo2zm_native_incremental_volume finds the pages that changed. "
                     "1 = each page's hash against the one of its last read copy (reads the 2 MB once); 0 = compares "
                     "them with that copy (reads 4 MB); 2 = alternates update by update, to measure both in the same "
                     "run (the C3 line gives the time of each)")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(bo2zm_native_constants_ubo, true, "BO2",
                    "Native renderer: shaders read their constants from dynamic UBOs (a constant bank on Maxwell) "
                    "instead of through a 64-bit pointer. Same bytes: the image does not change. Needs the shader "
                    "library with SPEC_CONSTANT_CONSTANTS_UBO; false goes back to the pointer");
REXCVAR_DEFINE_INT32(bo2zm_native_constants_ubo_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates the constants between the pointer (even spans) and "
                     "UBO (odd spans) every N seconds and logs each switch, to compare captures of a still scene in the "
                     "same run");


REXCVAR_DEFINE_BOOL(bo2zm_native_set4_by_differences, true, "BO2",
                    "Native renderer: set 4 (constants by UBO) by differences in NVK: only the "
                    "descriptors that change are sent and rebound. Same result (checks itself). Needs the Mesa with "
                    "nvk_switch_set4; false = as always");
REXCVAR_DEFINE_INT32(bo2zm_native_set4_by_differences_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates set 4 by differences (odd spans) and as "
                     "always (even spans) every N seconds and logs each switch, to compare C6 substages in the same "
                     "run. 0 = does not alternate");


REXCVAR_DEFINE_INT32(bo2zm_native_nvk_measure, 64, "BO2",
                     "Native renderer: measures by parts inside NVK 1 in N calls (power of 2; C6 NVK "
                     "by parts every 10 s). 0 = does not measure")
    .range(0, 4096);
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_measure_misses, false, "BO2",
                    "Native renderer (test): reads ahead what the pipeline bind and the shader flush will "
                    "use and measures it separately (how much is cache misses). Does extra work: measurement only");
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_emission, true, "BO2",
                    "Native renderer: NVK writes each draw command in one go, with the same bytes "
                    "(checks itself). false = as always");
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_cbufs, true, "BO2",
                    "Native renderer: NVK only looks at the cbuf slots that can get dirty and skips "
                    "the cbuf flush when nothing is dirty (checks itself). false = as always");
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_dynamic, true, "BO2",
                    "Native renderer: NVK only emits the dynamic state groups with dirty bits "
                    "(checks itself). false = as always");
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_prefetch, true, "BO2",
                    "Native renderer: cache hints (PRFM) for the pipeline and the shaders before "
                    "they are used, and the pipeline requested after PipelineOf. Changes nothing. false = no hints");


REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_fingerprint_copy, true, "BO2",
                    "Native renderer: when a pipeline is bound, NVK copies only the fixed-state groups "
                    "whose fingerprint changes (checks itself). false = the usual copy");
REXCVAR_DEFINE_INT32(bo2zm_native_nvk_copy_engine, 2, "BO2",
                     "Native renderer: NVK copies on the copy engine instead of vk_meta. Bit 0 = texture "
                     "uploads (buffer to image); bit 1 = image-to-image copies. 0 = everything as before")
    .range(0, 3);
REXCVAR_DEFINE_INT32(bo2zm_native_nvk_copy_engine_b, -1, "BO2",
                     "Test (Switch), with bo2zm_benchmark_ab_s: bo2zm_native_nvk_copy_engine's value in the A/B arms "
                     "with factor 2 on. -1 = not part of the A/B")
    .range(-1, 3);


REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_kind_views, true, "BO2",
                    "Native renderer: textures in the texture pool share one GPU mapping per pool block "
                    "(and memory kind) instead of a mapping each. false = a mapping per texture, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_nvk_kind_views_ab, false, "BO2",
                    "Test (Switch), with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_nvk_kind_views for the "
                    "textures bound in each arm (on = shared mappings)");


REXCVAR_DEFINE_BOOL(bo2zm_native_evict_by_key, true, "BO2",
                    "Native renderer: near the texture limit, cold textures are picked from a list "
                    "refreshed every 16 frames and released by key, without walking the whole cache every frame. "
                    "false = the full walks, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_evict_by_key_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_evict_by_key");


REXCVAR_DEFINE_BOOL(bo2zm_native_copy_neon, true, "BO2",
                    "Native renderer (Switch): vertex copies in 64-byte NEON steps with the source "
                    "prefetched ahead. false = the plain loop");
REXCVAR_DEFINE_INT32(bo2zm_native_copy_neon_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that switches bo2zm_native_copy_neon "
                     "(on in the arms with that factor on). 0 = not part of the A/B")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_nvk_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates the four NVK improvements (on in odd "
                     "spans, off in even ones) every N seconds, to compare in the same run. 0 = does not alternate");

#if REX_PLATFORM_SWITCH


extern "C" {
struct NvkSwitchSet4Counts {
  uint64_t bindings_mismatch;
  uint64_t bindings_full;
  uint64_t writes_root;
  uint64_t dwords_root;
  uint64_t cbufs_dirty;
  uint64_t cbufs_saved;
  uint64_t draws;
  uint64_t checks;
  uint64_t mismatches;
};
struct NvkSwitchSet4 {
  int32_t version;
  int32_t requested;
  int32_t env;
  int32_t off;
  NvkSwitchSet4Counts total;
};
extern NvkSwitchSet4 nvk_switch_set4 __attribute__((weak));
}
static_assert(sizeof(NvkSwitchSet4) == 16 + 9 * 8, "NvkSwitchSet4 has to be the same size as in NVK");
#endif
#if REX_PLATFORM_SWITCH


extern "C" {
struct NvkSwPart {
  uint64_t times;
  uint64_t ticks;
};
struct NvkSwImprovement {
  int32_t requested;
  int32_t off;
  uint64_t uses;
  uint64_t validated;
  uint64_t mismatches;
  uint64_t no_check;
};
struct NvkSwitchDraw {
  int32_t version;
  int32_t measure;
  int32_t measure_misses;
  int32_t env;
  uint64_t ticks_per_second;
  NvkSwPart parts[16];
  uint64_t counts[13];
  NvkSwImprovement improvements[5];
};
extern NvkSwitchDraw nvk_switch_draw __attribute__((weak));
void vk_switch_prefetch_pipeline(VkPipeline pipeline) __attribute__((weak));
}
static_assert(sizeof(NvkSwitchDraw) == 24 + 16 * 16 + 13 * 8 + 5 * 40, "NvkSwitchDraw: the same size as in NVK");
#endif
#if REX_PLATFORM_SWITCH


extern "C" {
struct VkBo2StateCopy {
  int32_t version;
  int32_t requested;
  int32_t off;
  int32_t padding;
  uint64_t copies;
  uint64_t no_hashes;
  uint64_t bits_copied;
  uint64_t bits_skipped;
  uint64_t checked;
  uint64_t mismatches;
  uint64_t pipelines;
};
struct NvkBo2Copies {
  int32_t version;
  int32_t engine;
  uint64_t uploads_engine;
  uint64_t regions_engine;
  uint64_t uploads_meta;
  uint64_t regions_meta;
  uint64_t copies_engine;
  uint64_t copies_meta;
};
struct NvkBo2KindViews {
  int32_t version;
  int32_t enabled;
  uint64_t views_created;
  uint64_t views_freed;
  uint64_t view_bytes;
  uint64_t binds_view;
  uint64_t binds_own;
  uint64_t failures;
};
extern VkBo2StateCopy vk_bo2_copy_state __attribute__((weak));
extern NvkBo2Copies nvk_bo2_copies __attribute__((weak));
extern NvkBo2KindViews nvk_bo2_kind_views __attribute__((weak));
}
static_assert(sizeof(VkBo2StateCopy) == 16 + 7 * 8, "VkBo2StateCopy: the same size as in Mesa");
static_assert(sizeof(NvkBo2Copies) == 8 + 6 * 8, "NvkBo2Copies: the same size as in NVK");
static_assert(sizeof(NvkBo2KindViews) == 8 + 6 * 8, "NvkBo2KindViews: the same size as in NVK");
#endif


REXCVAR_DEFINE_BOOL(bo2zm_native_texture_cache_across_frames, true, "BO2",
                    "Native renderer: the sampler caches stay valid across frames as long as the texture "
                    "content is not due for a check. false: they expire every frame, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_mipmaps, true, "BO2",
                    "Native renderer: uploads the mip levels the game provides (as on the Xbox 360). false: only the "
                    "base level, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


#if REX_PLATFORM_SWITCH
constexpr int32_t kDefaultTexturesMbMax = 512;
#else
constexpr int32_t kDefaultTexturesMbMax = 384;
#endif
REXCVAR_DEFINE_INT32(bo2zm_native_textures_mb_max, kDefaultTexturesMbMax, "BO2",
                     "Native renderer: MB of textures above which the ones unused the longest (at least 120 frames) are "
                     "released, down to 75 %. If the game asks for them again, they are uploaded again. 0 = no limit, "
                     "as before")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_texture_bind_thread, true, "BO2",
                    "Native renderer: the vkBindImageMemory of new textures (address reservation and "
                    "mapping on Horizon, ~0.6 ms each) runs on a separate thread and the ring keeps recording; before "
                    "submitting, whatever is missing is waited for. Same result (checks itself). false = on the ring")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_texture_bind_thread_priority, 0x2D, "BO2",
                     "Native renderer: priority of the texture bind thread. 0x2D, the ring's: above the "
                     "guest (0x3B) so a finished ioctl does not wait for a core, and outside the audio (0x2B) and "
                     "presentation (0x2C) bands. It sleeps in the ioctl almost all the time")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_texture_hash_thread, false, "BO2",
                    "Native renderer: the hash (XXH3), the untiling and the byte order of new "
                    "textures are done by a separate thread on a copy of guest memory taken on the ring; before "
                    "submitting, whatever is missing is collected. Same result (checks itself). false = on the ring, as "
                    "before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_texture_hash_thread_priority, 0x2E, "BO2",
                     "Native renderer: priority of the texture hash thread. 0x2E: below the ring (0x2D), "
                     "which preempts it as soon as it has work, and above the guest (0x3B), so the guest does not "
                     "starve it; outside the audio (0x2B) and presentation (0x2C) bands")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_texture_hash_thread_core, -2, "BO2",
                     "Native renderer: preferred core of the hash thread. -2 = automatic: core 2, or 1 if "
                     "the ring runs on 2 (on the ring's core, which is busy, it would barely run); -1 = the process "
                     "default; 0-2 = that one. Not exclusive")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_texture_hash_thread_mb, 16, "BO2",
                     "Native renderer: MB for the guest memory copies of a submission's new textures. "
                     "Whatever does not fit is prepared on the ring, as before")
    .range(4, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_diag_reuse, true, "BO2",
                    "Native renderer (measurement only): counts how many new textures have the same "
                    "content and shape as another live one in the cache (the game reloads the packs at another address) "
                    "and how many of those others have gone unused for more than 120 frames. Changes nothing. false = "
                    "no counting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_async_pipelines, true, "BO2",
                    "Native renderer: a pipeline the driver has not compiled yet is compiled on a thread "
                    "of its own, and its draws are skipped until it is ready (a few frames), instead of the ring "
                    "stopping for it: switching to HIGH for the first time froze the game for 7.5 s, dozens of "
                    "compiles in a row. One in the driver's cache is created at once, as before. false = every "
                    "pipeline compiled on the ring");


REXCVAR_DEFINE_INT32(bo2zm_native_compile_threads, 1, "BO2",
                     "Native renderer, with bo2zm_native_async_pipelines: how many threads compile pipelines. The "
                     "extra ones only take a job while two or more wait, and in a match only with "
                     "bo2zm_native_compile_helpers_in_match. 1 = one thread, as before")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_compile_helpers_in_match, false, "BO2",
                    "Native renderer, with bo2zm_native_compile_threads >= 2: the extra compile threads also take "
                    "jobs during a match (while two or more wait). false = only outside a match, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_test_async_all, false, "BO2",
                    "Test helper, with bo2zm_native_async_pipelines: every pipeline the session creates goes to the "
                    "compile thread, those in the driver's cache too, as on a console that never compiled them (the "
                    "draws wait a frame or more). false = only those that need compiling")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_INT32(bo2zm_native_pipelines_save_in_match, 0, "BO2",
                     "Native renderer: when the pipeline file is saved in a match. 0 = never (only in menus and "
                     "loading screens); 1 = once no new pipeline has come for 30 s, and only if the cache grew (a new "
                     "prewarm record alone waits for the next menu); 2 = as outside a match (after 64 new pipelines or "
                     "a minute with any). The writer thread takes the cache from the driver, and writes the file in "
                     "slices with pauses so the SD card stays free for the game's reads")
    .range(0, 2);
REXCVAR_DEFINE_INT32(bo2zm_native_test_pipelines_resave_s, 0, "BO2",
                     "Test helper: in a match, every this many seconds the whole pipeline file is taken from the "
                     "driver and written again, the odd ones at once and the even ones in slices (the [native] C6 "
                     "pipelines saved line says which), to see what a save costs. 0 = off")
    .range(0, 3600);
REXCVAR_DEFINE_BOOL(bo2zm_native_test_fresh_pipelines, false, "BO2",
                    "Test helper: the session starts with an empty pipeline cache and no prewarm list, as a fresh "
                    "install does, and saves neither (cache/bo2zm_native_pipelines.bin is left as it is); on the "
                    "Switch Mesa's own shader cache is off too (bo2zm_switch.cpp): every pipeline is really compiled, "
                    "to measure the first visit to a map")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, bo2zm_native_test_cold_pipelines);
REXCVAR_DEFINE_INT32(bo2zm_native_prewarm_threads, 2, "BO2",
                     "Native renderer, with bo2zm_native_prewarm_pipelines: 2 = a second prewarm thread walks the "
                     "list from its end, only while no match is on (menus, loading screens), until it meets the "
                     "first; what it compiles the first then finds in the driver's cache. 1 = one thread, as before")
    .range(1, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_native_prewarm_lists, "cache/prewarm", "BO2",
                      "Native renderer: the folder, beside the NRO or the exe, whose *.bin prewarm lists are read at "
                      "start-up besides the pipelines file's own (tools/pipelines_list.py makes them)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_test_export_list, false, "BO2",
                    "Test helper: the session's prewarm list is also written to logs/bo2zm_prewarm_list.bin (at most "
                    "every 20 s while it grows), so a test can bring it back with the logs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_prewarm_pipelines, true, "BO2",
                    "Native renderer: at boot, a lowest-priority thread recreates in the Vulkan "
                    "cache the pipelines the ring created in earlier sessions (their list is in "
                    "cache/bo2zm_native_pipelines.bin), with the same function as the ring, and destroys them: when the "
                    "ring asks for them they are already compiled (without the 70-160 ms stutter per pipeline of the "
                    "first session after a change). Changes no pipeline and no draw. false = no prewarming (the list is "
                    "still saved)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_INT32(bo2zm_native_test_out_of_memory_every, 0, "BO2",
                     "Native renderer (tests only): fakes running out of GPU memory on one in N texture allocations, to "
                     "exercise the recovery. 0 = off")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_mips, false, "BO2",
                    "Native renderer (diagnostic): compares the average color of each mip level with the base's in "
                    "DXT1/3/5 and 8888 textures and logs the ones that do not look alike (a level read from somewhere "
                    "else). Does not change the image")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_test_depth_clamp, false, "BO2",
                    "Native renderer (tests only): depthClampEnable on every pipeline. Imitates what "
                    "NVK seems to do on the console with whatever is behind the far plane (the sun of the scene's "
                    "occlusion queries)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_smooth_glow_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 cycles the sky glow through original, natural and "
                     "soft (bo2zm_sky_glow) every N seconds and logs each switch, to compare captures of the same "
                     "place");
REXCVAR_DEFINE_BOOL(bo2zm_native_test_occlusion_always, false, "BO2",
                    "Native renderer (tests only): colorless draws of an occlusion query on a target "
                    "of 640 or more always pass the depth test. Tells apart a sun that covers no pixels from one hidden "
                    "by depth. Changes what the game sees")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_shadow_pass_without_load, true, "BO2",
                    "Skip loading the previous contents of depth-only shadow targets with pitch >= 1600. "
                    "The attachment must be fully replaced before it is sampled.");
REXCVAR_DECLARE(bool, bo2zm_perf_v046);
REXCVAR_DECLARE(bool, bo2zm_perf_v055);
REXCVAR_DECLARE(bool, bo2zm_perf_readonly_depth);


REXCVAR_DEFINE_BOOL(bo2zm_perf_fit_area_to_clear, false, "BO2",
                    "Android performance: a pass whose guest clear covers every draw ever seen on that render "
                    "target opens with the clear's rectangle as render area and the clear as loadOp = CLEAR, instead "
                    "of loading the whole attachment and clearing inside the pass. false = original behavior")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_perf_prepass_fuse_rect_clears, false, "BO2",
                    "Android performance: a whole-area clear of depth only or stencil only becomes that aspect's "
                    "loadOp = CLEAR (the other keeps LOAD) instead of LOAD + vkCmdClearAttachments. false = original behavior")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_depth_only_no_load, false, "BO2",
                    "Native renderer (Android experiment): depth-only passes with a target pitch of at least "
                    "bo2zm_native_depth_only_no_load_min_pitch do not load their previous depth (loadOp = "
                    "DONT_CARE). Faster on a tile-based GPU, wrong if the pass does not rewrite what it reads");
REXCVAR_DEFINE_INT32(bo2zm_native_depth_only_no_load_min_pitch, 1000, "BO2",
                     "Native renderer (Android experiment): smallest target pitch affected by "
                     "bo2zm_native_depth_only_no_load")
    .range(64, 4096);
REXCVAR_DEFINE_INT32(bo2zm_native_skip_shadows_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 skips the shadows in the odd N-second spans and logs each "
                     "switch, to compare captures of the same place");
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_repeated_vertices, false, "BO2",
                    "Native renderer (diagnostic): counts the vertex bytes that repeat address, size and content within "
                    "the same frame or against an earlier one");


REXCVAR_DEFINE_BOOL(bo2zm_native_upload_thread, true, "BO2",
                    "Native renderer: the vertex copies into the upload buffer are done by a separate thread while the "
                    "ring thread keeps recording; they are waited for before submitting the work to the GPU and before "
                    "returning the read pointer to the game")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_INT32(bo2zm_native_upload_thread_priority, 0x2E, "BO2",
                     "Native renderer: priority of the vertex copy thread. 0x2E: below the ring "
                     "(0x2D) and above the guest (0x3A-0x3B), so it copies while the ring records. 0x3B = as up to 186")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_INT32(bo2zm_native_upload_thread_core, -2, "BO2",
                     "Native renderer: preferred core of the vertex copy thread. -2 = one different "
                     "from the ring's; -1 = no preference (as up to 190); 0-2 = that one")
    .range(-2, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_reuse_textures, true, "BO2",
                    "Native renderer: a draw whose pixel shader, fetch constants and texture caches did not change since "
                    "the previous draw reuses that draw's texture words instead of running the sampler loop (guarded: "
                    "checked against the loop, off for the session on any difference)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_indices_neon, true, "BO2",
                    "Native renderer: 16-bit indices are copied and measured (min and max) with "
                    "NEON, 16 per loop. Same result (checks itself). false = the usual loop")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_upload_help, true, "BO2",
                    "Native renderer: when the ring thread has to wait for the vertex copies, it "
                    "copies itself the ones the copy thread has not started yet and waits for the one in progress with "
                    "priority inheritance. Same data (checks itself). false = waits as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_diag_gray_empties, false, "BO2",
                    "Native renderer: textures not supported yet (cube, 3D, pending formats) sample gray instead of "
                    "zero (tests only)");
REXCVAR_DEFINE_BOOL(bo2zm_native_inv_tex_size, true, "BO2",
                    "Native renderer: shaders take 1/texture size from the constants instead of "
                    "asking the texture for it on each offset sample. Same arithmetic with the same number: the image "
                    "does not change. Needs a shader library regenerated with the new helper");


REXCVAR_DEFINE_BOOL(bo2zm_native_cheap_pcf, true, "BO2",
                    "Native renderer: a single shadow map sample instead of the 3x3 half-texel pattern. The "
                    "shadow edge is a little less smooth (true by default: the bands on the "
                    "walls were acne and the depth bias removes them). Needs the regenerated shader library");


REXCVAR_DEFINE_INT32(bo2zm_native_anisotropic, 0, "BO2",
                     "Native renderer: anisotropic filtering of the world textures (linear samplers "
                     "with mips). 0 = like the Xbox 360 (trilinear, blurry at an angle and in the distance); 2, 4, 8 or "
                     "16 = sharper, at a GPU cost")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_INT32(bo2zm_native_shadow_bias_slope, 20, "BO2",
                     "Native renderer: slope-scaled depth bias in the shadow map, in tenths (20 = "
                     "2.0). Removes the acne (a diagonal dot grid on walls and doors). Too high lifts the "
                     "shadows off the objects. 0 = as before. Changes on the fly")
    .range(0, 100);
REXCVAR_DEFINE_INT32(bo2zm_native_shadow_bias_constant, 0, "BO2",
                     "Native renderer: constant depth bias in the shadow map, in thousands of 24-bit "
                     "units (1 = 1000). 0 = no constant. Changes on the fly")
    .range(0, 100);

REXCVAR_DEFINE_INT32(bo2zm_native_diag_min_mip, 0, "BO2",
                     "Diagnostic: textures with mips start N levels lower, also up close, to see "
                     "whether their mip levels are right. 0 = normal; 1, 2 or 3 to look. Changes on the fly")
    .range(0, 4);


REXCVAR_DEFINE_BOOL(bo2zm_native_submit_after_shadows, false, "BO2",
                    "Native renderer: submit the work to the GPU as soon as the shadow pass closes, instead of "
                    "all together at the end of the frame. The GPU stops sitting idle waiting for the CPU to finish "
                    "recording");
REXCVAR_DEFINE_INT32(bo2zm_native_cheap_pcf_alternate_s, 0, "BO2",
                     "Native renderer: with N > 0 alternates every N seconds the shadow map sampling between "
                     "the 3x3 pattern and a single texel, and logs it. For A/B in the SAME session: comparing "
                     "between different sessions does not work because the stretch changes");
REXCVAR_DEFINE_INT32(bo2zm_native_inv_tex_size_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates every N seconds between taking 1/size "
                     "from the constants and asking the texture for it");
REXCVAR_DEFINE_INT32(bo2zm_native_per_draw_stats_s, 0, "BO2",
                     "Native renderer (diagnostic): with N > 0 measures one frame every N seconds with one "
                     "query per draw and splits the fragments by pixel shader");
REXCVAR_DEFINE_INT32(bo2zm_native_test_scissor, 0, "BO2",
                     "Native renderer (test): clips the draws of one category to 1x1 (1 shadows, 2 scene, 3 "
                     "cube, 4 postprocess, 5 the 'reflection' category: the scene at the internal resolutions below "
                     "1280, e.g. very low's). All the state and all the draws are sent but nothing is shaded: the time "
                     "difference is the per-draw floor. BREAKS THE IMAGE while it is set: measurement only");
REXCVAR_DEFINE_INT32(bo2zm_native_test_scissor_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates bo2zm_native_test_scissor's scissor "
                     "every N seconds");


REXCVAR_DEFINE_BOOL(bo2zm_native_test_point_mip, false, "BO2",
                    "Native renderer (test): the filter BETWEEN mip levels goes from linear to point (trilinear "
                    "-> bilinear). The TMU does half the work and the ALU does not change: if the scene gets faster, "
                    "sampling is the limit; if it does not move, the ALU is. The mip step shows when moving away: "
                    "measurement only");
REXCVAR_DEFINE_INT32(bo2zm_native_test_point_mip_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates bo2zm_native_test_point_mip every N seconds "
                     "(both samplers live in the cache, no stalls when switching)");


REXCVAR_DEFINE_BOOL(bo2zm_native_dedupe_indices, true, "BO2",
                    "Native renderer: if two draws of the same frame use the same 16-bit index range, it is scanned "
                    "and uploaded only once (DedupeIndices in bo2zm_native_vertex_dedupe.h); checked against a fresh "
                    "scan for the first 20,000 reuses");
REXCVAR_DEFINE_BOOL(bo2zm_native_dedupe_keep_on_flush_waits, true, "BO2",
                    "Native renderer: the upload dedupe (vertices and indices) keeps what it recorded across the "
                    "GPU's cache flush waits (g_syncs_on_flush_waits); checked by hash on the first 20,000 reuses and "
                    "every 512th frame. false = forget at every WAIT_REG_MEM, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


namespace bo2zm::native {
std::atomic<uint32_t> g_probe_vertex_address{0};
}

REXCVAR_DEFINE_BOOL(bo2zm_native_vertex_cache, true, "BO2",
                    "Native renderer: vertex ranges whose guest bytes did not change since they were "
                    "last drawn are copied once into a persistent buffer and bound from there, instead of being "
                    "copied into every frame's upload buffer. With bo2zm_native_vertex_cache_by_writes (the default) "
                    "unchanged means no write stamped on their pages: +5.4 % on the console; with "
                    "hashes it cost about what it saved (#83, #88). false = copied every frame");
REXCVAR_DEFINE_INT32(bo2zm_native_vertex_cache_mb, 96, "BO2",
                     "Native renderer: size in MB of each of the vertex cache's two persistent buffers")
    .range(4, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_vertex_cache_sum, true, "BO2",
                    "Native renderer: the vertex cache checks a range with Fletcher sums of its 16-byte blocks "
                    "(ChangeSum, no multiplies) instead of XXH3 (~2 GB/s on the console). false = XXH3");
REXCVAR_DEFINE_BOOL(bo2zm_native_vertex_cache_sum_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 2 is bo2zm_native_vertex_cache_sum (on = ChangeSum, "
                    "off = XXH3)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_vertex_cache_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_vertex_cache (on = the cache, off "
                    "= copied every frame)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_vertex_cache_by_writes, true, "BO2",
                    "Native renderer: with bo2zm_native_vertex_cache, a persistent vertex copy stays "
                    "valid while no write was stamped on its pages since (no hashing). false = the hash checks")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_index_cache, true, "BO2",
                    "Native renderer: with the vertex cache's pools and write stamps, a 16-bit index "
                    "range is converted once and bound from there while its pages are not written (no scan, no "
                    "upload). false = scanned and uploaded every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_index_cache_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_index_cache (on = cached, off = "
                    "scanned every draw)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_deferred_batch, true, "BO2",
                    "Native renderer: the ring thread hands its queued Vulkan calls to the recorder in "
                    "512-byte batches and reads the recorder's progress only when out of room")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_deferred_batch_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 2 is bo2zm_native_deferred_batch")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_fewer_copies, true, "BO2",
                    "Native renderer: a draw that reuses the previous one's texture words skips their "
                    "zeroing, copy back and fetch snapshot, and the vertex sources are not zeroed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_fewer_copies_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_fewer_copies")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_shared_swap, true, "BO2",
                    "Native renderer: a draw's shared words are built in one of two buffers and a change "
                    "swaps them, instead of copying the block to the previous draw's")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_shared_swap_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 2 is bo2zm_native_shared_swap")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_input_front, true, "BO2",
                    "Native renderer: a draw's vertex input found by its VS microcode's generation in a "
                    "small table first (no gather, hash and map walk when a VS comes back)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_input_front_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_input_front")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_prefetch_cache_lines, false, "BO2",
                    "Native renderer: prefetch the vertex and index caches' table lines and write stamps "
                    "of each draw early. false = looked up cold")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_prefetch_cache_lines_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_prefetch_cache_lines")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_constants_used, true, "BO2",
                    "Native renderer: a draw copies only the float constants its shaders read (not "
                    "dynamically addressed ones). false = the whole block up to the highest register")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_constants_used_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_constants_used")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_stamp_regions, false, "BO2",
                    "Native renderer: the vertex, index and texture caches check a range's write stamps by "
                    "64 KB region first. false = page by page")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_stamp_regions_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_stamp_regions")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_texture_by_writes, true, "BO2",
                    "Native renderer: a stable texture's periodic recheck is skipped (no hashing) while "
                    "no write was stamped on its pages since its last check. false = always hashed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_by_writes_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_texture_by_writes (on = skipped "
                    "by stamps, off = hashed)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_texture_by_writes_young, true, "BO2",
                    "Native renderer: with bo2zm_native_texture_by_writes, the skips start after one "
                    "recheck that finds the texture unchanged (interval 2) instead of three (interval 8)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_by_writes_young_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_texture_by_writes_young (on = from "
                    "interval 2, off = from interval 8)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_texture_reuse, false, "BO2",
                    "Native renderer: a new texture with the same guest bytes and shape as a texture "
                    "unused for 120 frames takes that one's image (nothing created, read or uploaded). false = always "
                    "a new image")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_texture_reuse_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_texture_reuse (on = reused, off = "
                    "a new image)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_dedupe_vertices, true, "BO2",
                    "Native renderer: if two draws of the same frame ask for the same vertex range, it is "
                    "uploaded only once. The C6 report logs hits and MB saved");


REXCVAR_DEFINE_BOOL(bo2zm_native_zero_base_vertices, true, "BO2",
                    "Native renderer: in draws with a single vertex binding, the upload buffer stays "
                    "bound at 0 and the draw is offset with vertexOffset/firstVertex, without a vkCmdBindVertexBuffers "
                    "per draw. The GPU reads the same bytes. false = as before");

REXCVAR_DEFINE_BOOL(bo2zm_native_ps_alpha_only, true, "BO2",
                    "Native renderer: in passes without a color target, compiles the pixel shader "
                    "without the color writes. The image does not change (Vulkan discards them) and the driver removes "
                    "as dead whatever only fed the color: the alpha test remains");
REXCVAR_DEFINE_INT32(bo2zm_native_ps_alpha_only_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates every N seconds compiling the pixel "
                     "shader with and without the color writes in colorless passes");


REXCVAR_DEFINE_BOOL(bo2zm_native_early_z, true, "BO2",
                    "Native renderer: in draws that test depth but do NOT write it (smoke, particles, glass, "
                    "decals), declare EarlyFragmentTests in the pixel shader so the GPU tests depth BEFORE shading "
                    "instead of after. Without this, any shader with an alpha test or kill shades all its fragments "
                    "even when they end up hidden. The image is identical: there is no Z write to move earlier");
REXCVAR_DEFINE_INT32(bo2zm_native_early_z_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates the early depth test every N seconds, to "
                     "measure it A/B in the SAME session");


REXCVAR_DEFINE_BOOL(bo2zm_native_postponed_sky, true, "BO2",
                    "Native renderer: postpone the sky draw until after the opaque draws of the same pass, "
                    "instead of drawing it first on an empty Z-buffer. It is a single opaque draw that tests depth and "
                    "does not write it, so the image is identical: what is overwritten today, tomorrow the depth test "
                    "discards");


REXCVAR_DEFINE_BOOL(bo2zm_native_skip_invisible, true, "BO2",
                    "Native renderer: skip the draws that cannot change a single pixel (a blend that copies the "
                    "target, or an alpha test with the NEVER function). The image is identical by definition");


REXCVAR_DEFINE_BOOL(bo2zm_native_framing_cache, true, "BO2",
                    "Native renderer: a draw's viewport, ndc and scissor are reused while neither "
                    "the framing registers (their generation) nor the pass change. Checks itself. false = computed for "
                    "every draw, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_useful_height_memo, true, "BO2",
                    "Native renderer: the target's useful height is noted in the map element of the "
                    "last pitch, without looking it up for every draw. Checks itself. false = as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_fast_pass_key, true, "BO2",
                    "Native renderer: if a draw's targets are the same bytes as the open pass's, "
                    "their XXH3 is not recomputed. Checks itself. false = XXH3 on every draw");
REXCVAR_DEFINE_BOOL(bo2zm_native_cvars_per_frame, true, "BO2",
                    "Native renderer: bo2zm_native_ps_alpha_only and "
                    "bo2zm_native_no_ps_without_color (and their alternations) are read once per frame and not on every "
                    "colorless draw. false = as before");
REXCVAR_DEFINE_INT32(bo2zm_native_no_ps_without_color_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates every N seconds building the pipeline "
                     "with and without a fragment stage in colorless draws, to compare captures of the same place with "
                     "the game paused");
REXCVAR_DEFINE_BOOL(bo2zm_native_no_ps_without_color, true, "BO2",
                    "Native renderer: in draws that write no color, builds the pipeline without a "
                    "fragment stage if the pixel shader can neither discard pixels nor write depth. The image does not "
                    "change and the GPU does not shade (shadow map)");
REXCVAR_DEFINE_STRING(bo2zm_native_diag_skip_ps, "", "BO2",
                      "Native renderer: comma-separated PS numbers whose draws are not recorded (tests only, to find a "
                      "draw)");


REXCVAR_DEFINE_INT32(bo2zm_native_upload_memory, 0, "BO2",
                     "Native renderer: upload buffer memory. 0 = the one the SDK chooses (on the Switch, without CPU "
                     "cache), 1 = with CPU cache (published with vkFlushMappedMemoryRanges before submitting), 2 = "
                     "without CPU cache")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_shared_constants_cache, true, "BO2",
                    "Native renderer: each draw's shared constants (488 bytes) in a small buffer "
                    "WITH CPU cache, separate from the upload buffer. Measured in 155: writing them uncached cost 2.0 "
                    "us per draw and cached 0.4. Only with constants by UBO")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_direct_pipelines, true, "BO2",
                    "Native renderer: a direct-mapped cache of 256 pipelines in front of "
                    "PipelineOf's map. Same result as the map (checks itself). false = only the map, as before");


REXCVAR_DEFINE_BOOL(bo2zm_native_count_pipeline_changes, false, "BO2",
                    "Native renderer: counts what changes at each vkCmdBindPipeline (measurement "
                    "only, changes no draw). C6 pipeline changes lines every 20 s. false = not counted");


REXCVAR_DEFINE_BOOL(bo2zm_native_canonical_key, false, "BO2",
                    "Native renderer: the pipeline is looked up with the key in canonical form (only "
                    "what Vulkan looks at): fewer pipelines and fewer vkCmdBindPipeline. Checks itself. false = the "
                    "usual key");


REXCVAR_DEFINE_BOOL(bo2zm_native_pipeline_across_passes, true, "BO2",
                    "Native renderer: the bound pipeline is kept when a pass of the same command "
                    "buffer begins (Vulkan keeps it). false = bound again in every pass, as before");


REXCVAR_DEFINE_BOOL(bo2zm_native_dynamic_state, false, "BO2",
                    "Native renderer: cull mode, front face, topology, Z, stencil, bias and restart "
                    "with vkCmdSet* (Vulkan 1.3 EDS1/EDS2) instead of in the pipeline: fewer vkCmdBindPipeline. Checks "
                    "itself. false = everything in the pipeline, as before");


REXCVAR_DEFINE_BOOL(bo2zm_native_dynamic_state3, false, "BO2",
                    "Native renderer: blend, equation and color mask with vkCmdSet* "
                    "(VK_EXT_extended_dynamic_state3) instead of in the pipeline. Checks itself. false = in the "
                    "pipeline, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_upload_memory, true, "BO2",
                    "Native renderer: when the upload buffer is created, measures once how many MB/s the CPU writes "
                    "into each visible memory type (8 MB) and how much publishing them costs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


namespace bo2zm::guard30 {
bool NoShadows(bool of_user);
}

namespace bo2zm::native {


std::atomic<uint32_t> g_syncs_ring{0};
std::atomic<bool> g_syncs_on_flush_waits{false};
namespace {


inline uint32_t TargetCategory(uint32_t pitch, const uint64_t* keys) {
  const bool color = keys[0] || keys[1] || keys[2] || keys[3];
  if (!color && keys[4] && pitch >= 1600) {
    return kGpuShadows;
  }
  if (pitch >= 1280) {
    return keys[4] ? kGpuScene : kGpuSceneNoDepth;
  }
  if (pitch >= 640) {
    return kGpuReflection;
  }
  if (pitch >= 320) {
    return kGpu320;
  }
  return kGpuSmaller;
}

namespace gr = rex::graphics;
namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;

constexpr uint32_t kRegConstantsVs = 0x4000;
constexpr uint32_t kRegConstantsPs = 0x4400;
constexpr uint32_t kRegFetch = 0x4800;
constexpr uint32_t kRegBooleans = 0x4900;
constexpr uint32_t kRegistersConstants = 0x400;


constexpr uint32_t kCapacityHeap[4] = {16384, 64, 256, 512};


constexpr size_t kSlotsOfJob = 3;
constexpr VkDeviceSize kSizeUpload = VkDeviceSize(64) << 20;


constexpr VkDeviceSize kSizeShared = VkDeviceSize(4) << 20;


constexpr uint32_t kMagicFilePipelines = 0x43505A42u;
constexpr uint32_t kVersionFilePipelines = 1;
constexpr size_t kHeaderFilePipelines = 2 * sizeof(uint32_t) + 2 * sizeof(uint64_t);


constexpr const char* kFileCacheOld = "bo2zm_native_pipelines.bin";
constexpr const char* kFileListOld = "bo2zm_native_pipelines_list.bin";


constexpr uint32_t kMagicListPipelines = 0x4C505A42u;
constexpr uint32_t kVersionListPipelines = 1;
constexpr size_t kHeaderList = 4 * sizeof(uint32_t);
constexpr size_t kMaxRegistersList = 4096;


constexpr uint32_t kWordsShared = 172;
constexpr uint32_t kWordScale = 122;
constexpr uint32_t kWordScaleCoords = 140;
constexpr uint32_t kWordInvSize = 90;


constexpr uint32_t kWordPixelScale = 138;
static_assert(kWordScale + 16 <= kWordPixelScale && kWordPixelScale + 2 <= kWordScaleCoords,
              "the pixel position scale must stay in the padding between the fetch and coordinate scales");


constexpr uint32_t kSpecConstantsUbo = uint32_t(1) << 8;


constexpr uint32_t kSpecInvSizeTex = uint32_t(1) << 9;
constexpr uint32_t kSpecGlowNatural = uint32_t(1) << 12;
constexpr uint32_t kSpecGlowSmooth = uint32_t(1) << 13;

constexpr uint32_t kSpecOnlyAlpha = uint32_t(1) << 14;


constexpr uint32_t kSpecPcfCheap = uint32_t(1) << 15;


constexpr uint32_t kSpecFunctionAlphaOffset = 16;

constexpr uint32_t kSpecNoBlur = uint32_t(1) << 19;


constexpr uint32_t kSpecZEarly = uint32_t(1) << 20;


constexpr uint32_t kSpecShadowMin = uint32_t(1) << 23;
constexpr uint64_t kHashComposite = 0x19C0C358044A29BFull;


constexpr uint32_t kBytesTreatment = 11 * 16;
void ApplyTreatmentVisual(float* c, int mode) {
  static constexpr float kNeutral[4][4] = {{0, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  const float f = mode == 2 ? 1.0f : 0.5f;
  for (int k = 0; k < 4; ++k) {
    for (int i = 0; i < 4; ++i) {
      float& v = c[(6 + k) * 4 + i];
      v += (kNeutral[k][i] - v) * f;
    }
  }
  c[1 * 4 + 0] *= 1.0f - f;
}


REXCVAR_DEFINE_BOOL(bo2zm_native_useful_area_pass, true, "BO2",
                    "Native renderer: open each pass on the rectangle the game really uses instead of on the "
                    "whole image. The targets' height is deduced from the pitch, so the scene opens 880x880 to draw "
                    "880x720. The image is identical: Vulkan only loads and stores the renderArea");

constexpr uint64_t kHashBrightPass = 0xE849A9F6D3323B87ull;


constexpr uint64_t kHashSky = 0x28AA3CDAC6C19705ull;


constexpr uint32_t kIndicesDomeSky = 480;


constexpr uint32_t kSkyFramesTest = 90;
constexpr uint32_t kSkyFramesWithOne = 90;
constexpr VkDeviceSize kUboBytesVs = 256 * 16;

constexpr VkDeviceSize kUboBytesPs = 256 * 16;
constexpr VkDeviceSize kUboBytesShared = 43 * 16;
constexpr uint32_t kRemapIdentity = 0xFFF;
constexpr uint32_t kMaxVerticesPerDraw = uint32_t(1) << 20;
constexpr uint32_t kMemoryPhysical = 0x20000000;

#define BO2ZM_SUB(k, ...) \
  do { \
    if (time_) { \
      const auto t0_sub_ = std::chrono::steady_clock::now(); \
      __VA_ARGS__; \
      sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>( \
                                 std::chrono::steady_clock::now() - t0_sub_) \
                                 .count()); \
      ++sub_n_[k]; \
    } else { \
      __VA_ARGS__; \
    } \
  } while (0)


constexpr uint32_t kStopwatchEvery = 64;
constexpr size_t kCopiesPerWarning = 64;


constexpr size_t kCopiesPerChunk = 16;


constexpr uint64_t kWaitsCopiesObserving = 512;


uint64_t ChangeSum(const uint8_t* data, size_t bytes) {
  uint64_t lanes[17];
#if defined(__aarch64__)
  uint64x2_t a1 = vdupq_n_u64(0), a2 = vdupq_n_u64(0), b1 = a1, b2 = a1, c1 = a1, c2 = a1, d1 = a1, d2 = a1;
  size_t i = 0;
  for (; i + 64 <= bytes; i += 64) {
    a1 = vaddq_u64(a1, vreinterpretq_u64_u8(vld1q_u8(data + i)));
    b1 = vaddq_u64(b1, vreinterpretq_u64_u8(vld1q_u8(data + i + 16)));
    c1 = vaddq_u64(c1, vreinterpretq_u64_u8(vld1q_u8(data + i + 32)));
    d1 = vaddq_u64(d1, vreinterpretq_u64_u8(vld1q_u8(data + i + 48)));
    a2 = vaddq_u64(a2, a1);
    b2 = vaddq_u64(b2, b1);
    c2 = vaddq_u64(c2, c1);
    d2 = vaddq_u64(d2, d1);
  }
  uint8_t tail[64] = {};
  std::memcpy(tail, data + i, bytes - i);
  a1 = vaddq_u64(a1, vreinterpretq_u64_u8(vld1q_u8(tail)));
  b1 = vaddq_u64(b1, vreinterpretq_u64_u8(vld1q_u8(tail + 16)));
  c1 = vaddq_u64(c1, vreinterpretq_u64_u8(vld1q_u8(tail + 32)));
  d1 = vaddq_u64(d1, vreinterpretq_u64_u8(vld1q_u8(tail + 48)));
  a2 = vaddq_u64(a2, a1);
  b2 = vaddq_u64(b2, b1);
  c2 = vaddq_u64(c2, c1);
  d2 = vaddq_u64(d2, d1);
  vst1q_u64(lanes + 0, a1);
  vst1q_u64(lanes + 2, a2);
  vst1q_u64(lanes + 4, b1);
  vst1q_u64(lanes + 6, b2);
  vst1q_u64(lanes + 8, c1);
  vst1q_u64(lanes + 10, c2);
  vst1q_u64(lanes + 12, d1);
  vst1q_u64(lanes + 14, d2);
#else
  uint64_t s1[8] = {}, s2[8] = {};
  size_t i = 0;
  auto block = [&](const uint8_t* p) {
    for (int k = 0; k < 8; ++k) {
      uint64_t w;
      std::memcpy(&w, p + 8 * k, 8);
      s1[k] += w;
      s2[k] += s1[k];
    }
  };
  for (; i + 64 <= bytes; i += 64) {
    block(data + i);
  }
  uint8_t tail[64] = {};
  std::memcpy(tail, data + i, bytes - i);
  block(tail);
  for (int k = 0; k < 8; ++k) {
    lanes[2 * k] = s1[k];
    lanes[2 * k + 1] = s2[k];
  }
#endif
  lanes[16] = bytes;
  return XXH3_64bits(lanes, sizeof(lanes));
}

struct CopyJob {
  const uint8_t* source = nullptr;
  uint8_t* target = nullptr;
  uint32_t words = 0;
  xenos::Endian order = xenos::Endian::kNone;
};


void CopyVerticesPlain(const uint8_t* const source, uint8_t* const target, const uint32_t words,
                       const xenos::Endian order) {
  if (order == xenos::Endian::k8in32) {
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t v;
      std::memcpy(&v, source + size_t(i) * 4, 4);
      v = std::byteswap(v);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  } else {
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t v;
      std::memcpy(&v, source + size_t(i) * 4, 4);
      v = xenos::GpuSwap(v, order);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  }
}

#if defined(__aarch64__)
constexpr size_t kCopyPrefetchAhead = 512;


template <int kOrder>
void CopyVerticesNeon(const uint8_t* source, uint8_t* target, uint32_t words) {
  auto swap = [](uint8x16_t v) {
    if constexpr (kOrder == int(xenos::Endian::k8in16)) {
      return vrev16q_u8(v);
    } else if constexpr (kOrder == int(xenos::Endian::k8in32)) {
      return vrev32q_u8(v);
    } else if constexpr (kOrder == int(xenos::Endian::k16in32)) {
      return vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    } else {
      return v;
    }
  };
  const size_t bytes = size_t(words) * 4;
  for (size_t ahead = 64; ahead < kCopyPrefetchAhead && ahead < bytes; ahead += 64) {
    __builtin_prefetch(source + ahead, 0, 0);
  }
  size_t i = 0;
  for (; i + 64 <= bytes; i += 64) {
    if (i + kCopyPrefetchAhead < bytes) {
      __builtin_prefetch(source + i + kCopyPrefetchAhead, 0, 0);
    }
    uint8x16x4_t v = vld1q_u8_x4(source + i);
    v.val[0] = swap(v.val[0]);
    v.val[1] = swap(v.val[1]);
    v.val[2] = swap(v.val[2]);
    v.val[3] = swap(v.val[3]);
    vst1q_u8_x4(target + i, v);
  }
  for (; i + 16 <= bytes; i += 16) {
    vst1q_u8(target + i, swap(vld1q_u8(source + i)));
  }
  for (; i < bytes; i += 4) {
    uint32_t v;
    std::memcpy(&v, source + i, 4);
    v = xenos::GpuSwap(v, xenos::Endian(kOrder));
    std::memcpy(target + i, &v, 4);
  }
}


bool CopyVerticesNeonSelfTest() {
  std::vector<uint8_t> source(9000), expected(9000), got(9000);
  uint32_t seed = 12345;
  for (auto& b : source) {
    seed = seed * 1664525u + 1013904223u;
    b = uint8_t(seed >> 24);
  }
  for (int order = 0; order < 4; ++order) {
    for (uint32_t words : {0u, 1u, 3u, 4u, 15u, 16u, 17u, 31u, 33u, 127u, 128u, 129u, 1000u, 2047u}) {
      for (size_t offset : {size_t(0), size_t(4), size_t(12)}) {
        std::fill(expected.begin(), expected.end(), uint8_t(0xA5));
        std::fill(got.begin(), got.end(), uint8_t(0xA5));
        CopyVerticesPlain(source.data() + offset, expected.data() + offset, words, xenos::Endian(order));
        switch (order) {
          case 1:
            CopyVerticesNeon<1>(source.data() + offset, got.data() + offset, words);
            break;
          case 2:
            CopyVerticesNeon<2>(source.data() + offset, got.data() + offset, words);
            break;
          case 3:
            CopyVerticesNeon<3>(source.data() + offset, got.data() + offset, words);
            break;
          default:
            CopyVerticesNeon<0>(source.data() + offset, got.data() + offset, words);
            break;
        }
        if (got != expected) {
          REXLOG_ERROR("[native] C6: NEON vertex copy differs from the plain loop (order {}, {} words, offset {}): "
                       "the plain loop stays",
                       order, words, offset);
          return false;
        }
      }
    }
  }
  return true;
}
#endif


bool CopyNeonOn() {
#if defined(__aarch64__)
  static const bool on = [] {
    if (!REXCVAR_GET(bo2zm_native_copy_neon)) {
      return false;
    }
    const bool ok = CopyVerticesNeonSelfTest();
    if (ok) {
      REXLOG_INFO("[native] C6: vertex copies in 64-byte NEON steps, {} bytes prefetched ahead (checked against the "
                  "plain loop)",
                  kCopyPrefetchAhead);
    }
    return ok;
  }();
  static const int factor = REXCVAR_GET(bo2zm_native_copy_neon_ab);
  if (on && (factor == 1 || factor == 2)) {
    const int arm = bo2zm::benchmark::AbArm();
    return arm < 0 || (arm & factor) != 0;
  }
  return on;
#else
  return false;
#endif
}

void CopyVertices(const CopyJob& t) {


  const uint8_t* const source = t.source;
  uint8_t* const target = t.target;
  const uint32_t words = t.words;
  const xenos::Endian order = t.order;
#if defined(__aarch64__)
  if (CopyNeonOn()) {
    switch (order) {
      case xenos::Endian::k8in16:
        CopyVerticesNeon<1>(source, target, words);
        return;
      case xenos::Endian::k8in32:
        CopyVerticesNeon<2>(source, target, words);
        return;
      case xenos::Endian::k16in32:
        CopyVerticesNeon<3>(source, target, words);
        return;
      default:
        CopyVerticesNeon<0>(source, target, words);
        return;
    }
  }
#endif
  CopyVerticesPlain(source, target, words, order);
}


template <class T>
struct UninitializedAllocator : std::allocator<T> {
  using value_type = T;
  UninitializedAllocator() = default;
  template <class U>
  UninitializedAllocator(const UninitializedAllocator<U>&) noexcept {}
  template <class U>
  struct rebind {
    using other = UninitializedAllocator<U>;
  };
  template <class U>
  void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
    ::new (static_cast<void*>(p)) U;
  }
  template <class U, class... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};

using FnBufferAddress = VkDeviceAddress(VKAPI_PTR*)(VkDevice, const VkBufferDeviceAddressInfo*);
using FnCopyImage = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                                        VkImageLayout, uint32_t, const VkImageCopy*);

float Float(uint32_t value) {
  return std::bit_cast<float>(value);
}


uint32_t Log2Ceiling(uint32_t v) {
  return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1));
}

uint32_t Log2Floor(uint32_t v) {
  return v ? 31 - uint32_t(std::countl_zero(v)) : 0;
}


constexpr uint64_t kBlockSample = 4096;
constexpr uint64_t kSampleEvery = 8;


inline uint64_t BytesSample(uint64_t bytes) {
  if (!bytes) {
    return 0;
  }
  const uint64_t last = (bytes - 1) / kBlockSample;
  return (last + kSampleEvery - 1) / kSampleEvery * kBlockSample + (bytes - last * kBlockSample);
}


inline uint64_t HashSample(const uint8_t* data, uint64_t bytes, uint64_t seed) {
  if (!bytes) {
    return seed;
  }
  const uint64_t last = (bytes - 1) / kBlockSample;
  uint64_t hash = seed;
  for (uint64_t b = 0; b < last; b += kSampleEvery) {
    hash = XXH3_64bits_withSeed(data + b * kBlockSample, size_t(kBlockSample), hash);
  }
  return XXH3_64bits_withSeed(data + last * kBlockSample, size_t(bytes - last * kBlockSample), hash);
}


uint32_t LevelPacked(uint32_t width, uint32_t height) {
  const uint32_t l = Log2Ceiling(std::min(width, height));
  return l > 4 ? l - 4 : 0;
}


void OffsetPacked(uint32_t width, uint32_t height, uint32_t block, uint32_t level, uint32_t& x,
                               uint32_t& y) {
  const uint32_t l2_width = Log2Ceiling(width);
  const uint32_t l2_height = Log2Ceiling(height);
  const uint32_t l2 = std::min(l2_width, l2_height);
  x = 0;
  y = 0;
  if (l2 > 4 + level) {
    return;
  }
  const uint32_t base = l2 > 4 ? l2 - 4 : 0;
  const uint32_t m = level - base;
  if (m < 3) {
    if (l2_width > l2_height) {
      y = 16u >> m;
    } else {
      x = 16u >> m;
    }
  } else if (l2_width > l2_height) {
    x = (1u << (l2_width - base)) >> (m - 2);
  } else {
    y = (1u << (l2_height - base)) >> (m - 2);
  }
  x /= block;
  y /= block;
}


int32_t OffsetTiled2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}


template <uint32_t kLog2>
void UntileLevel(const uint8_t* source, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                        uint32_t bx_host, uint8_t* target) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGroup = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;
  const int32_t macros_row = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  for (uint32_t row = 0; row < by; ++row) {
    const int32_t y = int32_t(oy + row);
    const int32_t macro_y = (y >> 5) * macros_row;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    uint8_t* output = target + size_t(row) * bx_host * kBytes;
    uint32_t column = 0;
    while (column < bx) {
      const int32_t x = int32_t(ox + column);
      const int32_t macro = ((x >> 5) + macro_y) << (kLog2 + 7);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      const int32_t displacement = ((offset & ~0x1FF) << 3) + y16 + ((offset & 0x1C0) << 2) +
                                     (((y8 + (x >> 3)) & 3) << 6) + (offset & 0x3F);
      if (kGroup > 1 && (uint32_t(x) % kGroup) == 0 && column + kGroup <= bx) {
        std::memcpy(output + size_t(column) * kBytes, source + displacement, kGroup * kBytes);
        column += kGroup;
      } else {
        std::memcpy(output + size_t(column) * kBytes, source + displacement, kBytes);
        ++column;
      }
    }
  }
}


inline void ChangeOrderBytes(uint8_t* data, size_t bytes, uint32_t unit, uint32_t order) {
  constexpr uint32_t k8in16 = 1, k8in32 = 2, k16in32 = 3;
  size_t n = unit == 2 ? bytes & ~size_t(1) : bytes & ~size_t(3);
  if ((unit == 2 && order != k8in16) || (unit != 2 && unit != 4) || order == 0) {
    return;
  }
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t v = vld1q_u8(data + i);
    uint8x16_t r;
    if (unit == 2 || order == k8in16) {
      r = vrev16q_u8(v);
    } else if (order == k8in32) {
      r = vrev32q_u8(v);
    } else {
      r = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    }
    vst1q_u8(data + i, r);
  }
#endif
  if (unit == 2 || order == k8in16) {
    for (; i + 2 <= n; i += 2) {
      std::swap(data[i], data[i + 1]);
    }
  } else if (order == k8in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 3]);
      std::swap(data[i + 1], data[i + 2]);
    }
  } else if (order == k16in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 2]);
      std::swap(data[i + 1], data[i + 3]);
    }
  }
}


int32_t OffsetTiled3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                                uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  height = (height + 31) & ~uint32_t(31);
  const int32_t macro_exterior = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_exterior) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t exterior = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = exterior + ((((x >> 3) + (exterior << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}


int32_t UseLocation(uint8_t use, uint8_t index) {
  switch (use) {
    case 0:
      return index == 0 ? 0 : (index == 1 ? 15 : -1);
    case 3:
      return index == 0 ? 1 : -1;
    case 6:
      return index == 0 ? 2 : -1;
    case 7:
      return index == 0 ? 3 : -1;
    case 5:
      return index < 4 ? 4 + index : (index < 8 ? 12 + (index - 4) : -1);
    case 10:
      return index == 0 ? 8 : (index == 1 ? 11 : -1);
    case 2:
      return index == 0 ? 9 : -1;
    case 1:
      return index == 0 ? 10 : -1;
    default:
      return -1;
  }
}


bool InputWhole(uint8_t use) {


  return use == 2;
}

uint32_t ComponentsVertex(uint32_t format) {
  switch (format) {
    case 33:
    case 36:
      return 1;
    case 25:
    case 31:
    case 34:
    case 37:
      return 2;
    case 16:
    case 17:
    case 57:
      return 3;
    default:
      return 4;
  }
}


uint32_t MaskWritten(uint32_t swizzle) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((swizzle >> (i * 3)) & 0x7) != 7) {
      mask |= 1u << i;
    }
  }
  return mask;
}


uint32_t CodeRemap(uint32_t original, uint32_t patched) {
  uint32_t code = kRemapIdentity;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t o = (original >> (i * 3)) & 0x7;
    const uint32_t d = (patched >> (i * 3)) & 0x7;
    if (o <= 3 && d != 7) {
      code = (code & ~(uint32_t(0x7) << (o * 3))) | (d << (o * 3));
    }
  }
  return code;
}

VkFormat FormatAttribute(uint32_t format, bool input_whole, bool with_sign, bool whole,
                         bool red_blue, bool& r11g11b10) {
  r11g11b10 = false;
  const auto choose = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled,
                          VkFormat uint_, VkFormat sint) {
    if (input_whole) {
      return with_sign ? sint : uint_;
    }
    if (whole) {
      return with_sign ? sscaled : uscaled;
    }
    return with_sign ? snorm : unorm;
  };
  switch (format) {
    case 6:
      return red_blue ? choose(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM,
                                VK_FORMAT_B8G8R8A8_USCALED, VK_FORMAT_B8G8R8A8_SSCALED,
                                VK_FORMAT_B8G8R8A8_UINT, VK_FORMAT_B8G8R8A8_SINT)
                       : choose(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
                                VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED,
                                VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT);
    case 7:
      if (red_blue) break;
      return choose(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                    VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                    VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32);
    case 16:
      if (!input_whole || red_blue) break;
      r11g11b10 = true;
      return VK_FORMAT_R32_UINT;
    case 25:
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                    VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT);
    case 26:
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                    VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                    VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT);
    case 31:
      return input_whole || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16_SFLOAT;
    case 32:
      return input_whole || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33:
      return input_whole && !red_blue ? (with_sign ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT)
                                         : VK_FORMAT_UNDEFINED;
    case 34:
      return input_whole && !red_blue
                 ? (with_sign ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT)
                 : VK_FORMAT_UNDEFINED;
    case 35:
      return input_whole && !red_blue
                 ? (with_sign ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                 : VK_FORMAT_UNDEFINED;

    case 36:
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (input_whole ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT);
    case 37:
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (input_whole ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SFLOAT);
    case 57:
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (input_whole ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT);
    case 38:
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (input_whole ? VK_FORMAT_R32G32B32A32_UINT
                                         : VK_FORMAT_R32G32B32A32_SFLOAT);
    default:
      break;
  }
  return VK_FORMAT_UNDEFINED;
}

constexpr uint16_t kSwizzleRRRR = 0;
constexpr uint16_t kSwizzleRGGG = (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint16_t kSwizzleRGBA = (1 << 3) | (2 << 6) | (3 << 9);
constexpr uint16_t kSwizzleBGRA = 2 | (1 << 3) | (0 << 6) | (3 << 9);

struct HostTextureFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint8_t block = 1;
  uint8_t bytes = 1;
  uint8_t unit_order = 0;
  uint16_t swizzle_host = kSwizzleRGBA;
};


bool FormatTextureOf(uint32_t format, HostTextureFormat& f) {
  switch (format) {
    case 2:
    case 8:
      f = {VK_FORMAT_R8_UNORM, 1, 1, 0, kSwizzleRRRR};
      return true;
    case 10:
      f = {VK_FORMAT_R8G8_UNORM, 1, 2, 2, kSwizzleRGGG};
      return true;
    case 6:
    case 50:
      f = {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 7:
    case 54:
      f = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 18:
    case 51:
      f = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, 2, kSwizzleRGBA};
      return true;
    case 19:
    case 52:
      f = {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 20:
    case 53:
      f = {VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 49:
      f = {VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, 2, kSwizzleRGGG};
      return true;
    case 59:
      f = {VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, 2, kSwizzleRRRR};
      return true;
    case 24:
      f = {VK_FORMAT_R16_UNORM, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 25:
      f = {VK_FORMAT_R16G16_UNORM, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 26:
      f = {VK_FORMAT_R16G16B16A16_UNORM, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 30:
      f = {VK_FORMAT_R16_SFLOAT, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 31:
      f = {VK_FORMAT_R16G16_SFLOAT, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 32:
      f = {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 36:
      f = {VK_FORMAT_R32_SFLOAT, 1, 4, 4, kSwizzleRRRR};
      return true;
    case 37:
      f = {VK_FORMAT_R32G32_SFLOAT, 1, 8, 4, kSwizzleRGGG};
      return true;
    case 38:
      f = {VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16, 4, kSwizzleRGBA};
      return true;
    default:
      return false;
  }
}

bool FormatHasStencil(VkFormat format) {
  return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
         format == VK_FORMAT_D16_UNORM_S8_UINT;
}

bool IsDepth(VkFormat format) {
  return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}


VkComponentMapping MappingComponents(uint32_t swizzle_guest, uint16_t swizzle_host) {
  VkComponentMapping mapping{};
  VkComponentSwizzle* output[4] = {&mapping.r, &mapping.g, &mapping.b, &mapping.a};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t value = (swizzle_guest >> (i * 3)) & 0x7;
    if (value <= 3) {
      const uint32_t host = (swizzle_host >> (value * 3)) & 0x7;
      *output[i] = VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + host);
    } else if (value == 4) {
      *output[i] = VK_COMPONENT_SWIZZLE_ZERO;
    } else {
      *output[i] = VK_COMPONENT_SWIZZLE_ONE;
    }
  }
  return mapping;
}

struct VertexAttribute {
  uint32_t location;
  uint32_t binding;
  VkFormat format;
  uint32_t offset;
};

struct VertexBinding {
  uint32_t slot;
  uint32_t stride;
};

struct VertexInput {
  std::vector<VertexAttribute> attributes;
  std::vector<VertexBinding> bindings;
  std::array<uint32_t, 16> remaps{};
  uint32_t specialization = 0;
  uint64_t hash = 0;
};


std::vector<uint32_t> PruneColorWrites(const std::vector<uint32_t>& spirv, uint32_t& removed) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpDecorate = 71, kOpVariable = 59, kOpStore = 62;
  constexpr uint32_t kOpAccessChain = 65, kOpInBoundsAccessChain = 66;
  constexpr uint32_t kDecorationBuiltIn = 11, kDecorationLocation = 30, kDecorationIndex = 29;
  constexpr uint32_t kStoreOutput = 3;
  removed = 0;
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    return {};
  }
  std::unordered_set<uint32_t> with_location, discarded;


  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      return {};
    }
    if (code == kOpDecorate && words >= 3) {
      const uint32_t goal = spirv[i + 1];
      const uint32_t decoration = spirv[i + 2];
      if (decoration == kDecorationLocation && words >= 4 && spirv[i + 3] <= 3) {
        with_location.insert(goal);
      } else if (decoration == kDecorationBuiltIn ||
                 (decoration == kDecorationIndex && words >= 4 && spirv[i + 3] != 0)) {
        discarded.insert(goal);
      }
    }
    i += words;
  }


  std::unordered_set<uint32_t> pointers;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (code == kOpVariable && words >= 4 && spirv[i + 3] == kStoreOutput) {
      const uint32_t id = spirv[i + 2];
      if (with_location.count(id) && !discarded.count(id)) {
        pointers.insert(id);
      }
    } else if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && words >= 4 &&
               pointers.count(spirv[i + 3])) {
      pointers.insert(spirv[i + 2]);
    }
    i += words;
  }
  if (pointers.empty()) {
    return {};
  }


  std::vector<uint32_t> output;
  output.reserve(spirv.size());
  output.insert(output.end(), spirv.begin(), spirv.begin() + 5);
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    const bool outside = code == kOpStore && words >= 3 && pointers.count(spirv[i + 1]);
    if (outside) {
      ++removed;
    } else {
      output.insert(output.end(), spirv.begin() + i, spirv.begin() + i + words);
    }
    i += words;
  }
  (void)kOpEntryPoint;
  return removed ? output : std::vector<uint32_t>();
}


std::vector<uint32_t> WithTestsEarly(const std::vector<uint32_t>& spirv, const char*& reason) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpExecutionMode = 16;
  constexpr uint32_t kModelFragment = 4;
  constexpr uint32_t kModeEarly = 9, kModeDepthReplacing = 12;
  reason = "";
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    reason = "not SPIR-V";
    return {};
  }
  uint32_t input = 0;
  size_t where = 0;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      reason = "malformed module";
      return {};
    }
    if (code == kOpEntryPoint && words >= 3) {
      if (spirv[i + 1] == kModelFragment) {
        if (input) {
          reason = "more than one fragment entry point";
          return {};
        }
        input = spirv[i + 2];
      }
      if (where < i + words) {
        where = i + words;
      }
    } else if (code == kOpExecutionMode && words >= 3) {
      if (spirv[i + 2] == kModeEarly) {
        reason = "already declared it";
        return {};
      }
      if (spirv[i + 2] == kModeDepthReplacing) {
        reason = "writes gl_FragDepth";
        return {};
      }
      if (where < i + words) {
        where = i + words;
      }
    }
    i += words;
  }
  if (!input || !where) {
    reason = "no fragment entry point";
    return {};
  }
  std::vector<uint32_t> output;
  output.reserve(spirv.size() + 3);
  output.insert(output.end(), spirv.begin(), spirv.begin() + where);
  output.push_back((3u << 16) | kOpExecutionMode);
  output.push_back(input);
  output.push_back(kModeEarly);
  output.insert(output.end(), spirv.begin() + where, spirv.end());
  return output;
}

struct PipelineKey {
  uint32_t vs = 0;
  uint32_t ps = 0;
  uint64_t input = 0;
  uint32_t topology = 0;
  uint32_t specialization = 0;
  uint32_t formats[5] = {};
  uint32_t blend[4] = {};
  uint32_t masks = 0;
  uint32_t depth = 0;
  uint32_t rasterization = 0;


  uint32_t padding = 0;
  uint32_t padding2 = 0;
};
static_assert(std::has_unique_object_representations_v<PipelineKey>,
              "PipelineKey cannot have implicit padding: it is hashed and compared byte by byte");


struct RegisterAttribute {
  uint32_t location = 0;
  uint32_t binding = 0;
  uint32_t format = 0;
  uint32_t offset = 0;
};

struct PipelineRecord {
  static constexpr uint32_t kMaxAttributes = 16;
  static constexpr uint32_t kMaxBindings = 16;
  PipelineKey key;
  uint64_t hash_vs = 0;
  uint64_t hash_ps = 0;
  uint32_t n_attributes = 0;
  uint32_t n_bindings = 0;
  RegisterAttribute attributes[kMaxAttributes] = {};
  uint32_t strides[kMaxBindings] = {};
};
static_assert(std::has_unique_object_representations_v<PipelineRecord>,
              "PipelineRecord is saved to disk byte by byte: no implicit padding");


struct IncrementalVolume {
  uint32_t address = 0;
  uint32_t bound = 0;
  uint32_t pages = 0;

  std::vector<uint8_t> raw;
  std::vector<uint64_t> hashes;
  std::vector<uint8_t> host;

  struct Span {
    uint32_t source;
    uint32_t target;
    uint16_t bytes;
    uint16_t x;
    uint16_t y;
    uint16_t z;
  };
  std::vector<Span> spans;
  std::vector<uint32_t> start_page;
  std::vector<uint32_t> spans_page;
  uint32_t row = 0;
  uint32_t slice = 0;
  uint32_t height = 0;
  uint32_t depth3d = 0;
  uint32_t width_host = 0;
  uint32_t unit_order = 0;
  uint32_t order = 0;
};

struct CachedTexture {
  NativeImage image;
  uint32_t layers = 1;
  uint32_t depth3d = 0;
  uint64_t hash = 0;
  uint64_t frame = UINT64_MAX;
  uint64_t staging_failed_frame = UINT64_MAX;
  uint64_t hash_raw = 0;
  uint64_t next = 0;
  uint32_t interval = 1;
  uint32_t postponements = 0;


  uint64_t hash_sample = 0;
  bool sample_valid = false;
  uint8_t samples_consecutive = 0;
  bool needs_upload = false;
  std::vector<uint8_t> data;
  uint32_t levels = 1;
  std::array<uint32_t, 16> offset_level{};
  uint64_t bytes = 0;


  uint32_t in_flight = 0;


  uint32_t hash_job = 0;


  uint64_t shape_content = 0;
  uint64_t key_content = 0;
  uint32_t address = 0;
  bool content_per_measure = false;


  uint32_t stamp_base = 0, stamp_base_bytes = 0, stamp_mips = 0, stamp_mips_bytes = 0;
  uint32_t check_epoch = 0;
  uint16_t stamp_skips = 0;
  bool guard_stamps = false;


  std::unique_ptr<IncrementalVolume> incremental;
  std::vector<VkBufferImageCopy> regions_partial;
};

struct View {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t slot = 0;
  uint32_t heap = 0;
};

class VulkanDrawsImpl final : public VulkanDraws {
 public:
  VulkanDrawsImpl(const VulkanDevice* vulkan_device, rex::memory::Memory* guest_memory,
                    TargetContext* context)
      : vulkan_device_(vulkan_device),
        dfn_(deferred::Table(*vulkan_device)),
        device_(vulkan_device->device()),
        memory_(guest_memory),
        context_(context) {}

  ~VulkanDrawsImpl() override {
    StopPrewarm();
    StopCopies();
    StopBindings();
    StopHashes();
    SaveCachePipelines();
    StopWriterCache();
    for (auto& [key, pair] : pipelines_) {
      dfn_.vkDestroyPipeline(device_, pair.second, nullptr);
    }
    if (cache_pipelines_ != VK_NULL_HANDLE) {
      destroy_cache_(device_, cache_pipelines_, nullptr);
    }
    for (auto& [key, framebuffer] : framebuffers_) {
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (VkFramebuffer framebuffer : fb_retired_) {
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (auto& [key, pass] : passes_) {
      dfn_.vkDestroyRenderPass(device_, pass, nullptr);
    }
    for (auto& [input, shader_module] : modules_) {
      dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
    }
    for (auto& [input, shader_module] : modules_only_alpha_) {
      if (shader_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
      }
    }
    for (auto& [input, shader_module] : modules_z_early_) {
      if (shader_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
      }
    }
    for (VkShaderModule shader_module : modules_variants_) {
      if (shader_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
      }
    }
    for (auto& [key, view] : views_) {
      dfn_.vkDestroyImageView(device_, view.view, nullptr);
    }
    for (auto& [key, texture] : textures_) {
      DestroyImage(texture.image);
    }
    for (auto& [key, pair] : samplers_) {
      dfn_.vkDestroySampler(device_, pair.first, nullptr);
    }
    for (NativeImage& empty : empty_) {
      DestroyImage(empty);
    }


    pool_textures_.Finish();
    if (sampler_empty_ != VK_NULL_HANDLE) dfn_.vkDestroySampler(device_, sampler_empty_, nullptr);
    if (layout_pipeline_ != VK_NULL_HANDLE)
      dfn_.vkDestroyPipelineLayout(device_, layout_pipeline_, nullptr);
    bo2zm::gpu_skin::SetRendererReady(false);
    if (skin_pipeline_ != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, skin_pipeline_, nullptr);
    if (skin_layout_ != VK_NULL_HANDLE) dfn_.vkDestroyPipelineLayout(device_, skin_layout_, nullptr);
    if (skin_table_buffer_ != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, skin_table_buffer_, nullptr);
    if (skin_table_memory_ != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, skin_table_memory_, nullptr);
    if (skin_pool_buffer_ != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, skin_pool_buffer_, nullptr);
    if (skin_pool_memory_ != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, skin_pool_memory_, nullptr);
    if (pool_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pool_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_ubo_, nullptr);
    if (layout_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout_ubo_, nullptr);
    for (VkDescriptorSetLayout layout : layouts_) {
      if (layout != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
    for (const UploadBuffer& s : shared_bufs_) {
      if (s.data) dfn_.vkUnmapMemory(device_, s.guest_memory);
      if (s.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.buffer, nullptr);
      if (s.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.guest_memory, nullptr);
    }
    for (const UploadBuffer& s : uploads_) {
      if (s.data) dfn_.vkUnmapMemory(device_, s.guest_memory);
      if (s.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.buffer, nullptr);
      if (s.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.guest_memory, nullptr);
    }
    for (const VertexPool& v : vpools_) {
      if (v.data) dfn_.vkUnmapMemory(device_, v.memory);
      if (v.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, v.buffer, nullptr);
      if (v.memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, v.memory, nullptr);
    }
  }

  bool Init() {
    reuse_textures_on_ = REXCVAR_GET(bo2zm_native_reuse_textures);
    dedupe_keep_flush_ = REXCVAR_GET(bo2zm_native_dedupe_keep_on_flush_waits);
    g_syncs_on_flush_waits.store(!dedupe_keep_flush_, std::memory_order_relaxed);
    const auto& properties = vulkan_device_->properties();
    ubo_only_ = vulkan_device_->native_ubo_only();
    if (ubo_only_ && REXCVAR_GET(bo2zm_native_compact_bc)) {
      const auto supported = [this](VkFormat format) {
        VkFormatProperties p{};
        vulkan_device_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
            vulkan_device_->physical_device(), format, &p);
        constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        return (p.optimalTilingFeatures & required) == required;
      };
      compact_bc4_ = supported(VK_FORMAT_R8_UNORM);
      compact_bc5_ = supported(VK_FORMAT_R8G8_UNORM);
    }
    if (ubo_only_) REXLOG_INFO("[native] lossless BC storage: BC4={}, BC5={} (RGBA8 fallback)",
                              compact_bc4_ ? "R8" : "RGBA8", compact_bc5_ ? "RG8" : "RGBA8");
    const std::pair<bool, const char*> requirements[] = {
        {ubo_only_ || properties.shaderInt64, "shaderInt64 (pointer profile)"},
        {properties.bufferDeviceAddress, "bufferDeviceAddress"},
        {properties.runtimeDescriptorArray, "runtimeDescriptorArray"},
        {properties.shaderSampledImageArrayDynamicIndexing,
         "shaderSampledImageArrayDynamicIndexing"},
        {properties.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {properties.descriptorBindingSampledImageUpdateAfterBind,
         "descriptorBindingSampledImageUpdateAfterBind"},
        {properties.descriptorBindingUpdateUnusedWhilePending,
         "descriptorBindingUpdateUnusedWhilePending"},
    };
    for (const auto& [present, label] : requirements) {
      if (!present) {
        REXLOG_ERROR("[native] C6: the Vulkan device does not have {}: nothing is drawn", label);
        return false;
      }
    }
    address_buffer_ = reinterpret_cast<FnBufferAddress>(
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkGetBufferDeviceAddress"));
    copy_image_ = reinterpret_cast<FnCopyImage>(deferred::Wrap(
        "vkCmdCopyImage",
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_, "vkCmdCopyImage")));
    LoadCachePipelines();
    LoadStateDynamic();
    if (!address_buffer_ || !CreateUpload() || !CreateDescriptors()) {
      return false;
    }
    CreateGpuSkin();


    pool_textures_.Start(vulkan_device_, textures_mb_max_);
    return CreateEmpty();
  }

  bool Draw(const DrawRequest& p) override {
    VerdictVegetation(p);


    time_ = (++stopwatch_counter_ & (kStopwatchEvery - 1)) == 0;
    auto mark = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const uint32_t* r = p.registers;
    if (!r || !p.vs) {
      return false;
    }

    const uint32_t mode_edram = r[gr::XE_GPU_REG_RB_MODECONTROL] & 0x7;
    if (mode_edram != uint32_t(xenos::EdramMode::kColorDepth) && mode_edram != 5) {
      return Reject(1, "EDRAM mode without color or depth");
    }


    const ShaderInput* ps =
        mode_edram == uint32_t(xenos::EdramMode::kColorDepth) ? p.ps : nullptr;
    if (!ps && mode_edram == uint32_t(xenos::EdramMode::kColorDepth)) {
      return false;
    }

    const std::string& skip = diag_skip_ps_text_;
    if (skip != skip_text_) {
      skip_text_ = skip;
      skip_ps_.clear();
      uint32_t number = 0;
      bool has = false;
      for (char c : skip + ",") {
        if (c >= '0' && c <= '9') {
          number = number * 10 + uint32_t(c - '0');
          has = true;
        } else if (has) {
          skip_ps_.insert(number);
          number = 0;
          has = false;
        }
      }
      if (!skip_ps_.empty()) {
        REXLOG_WARN("[native] C6 diag: PS {} are not drawn", skip_text_);
      }
    }
    if (ps && !skip_ps_.empty() && skip_ps_.count(ps->number)) {
      return true;
    }
    const uint32_t initiator = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
    const uint32_t type = initiator & 0x3F;
    const uint32_t source_kind = (initiator >> 6) & 0x3;
    const bool indices32 = (initiator >> 11) & 0x1;
    const uint32_t count = initiator >> 16;

    if (prefetch_lines_ && icache_active_ && source_kind == uint32_t(xenos::SourceSelect::kDMA) && !indices32) {
      const uint32_t physical = r[gr::XE_GPU_REG_VGT_DMA_BASE] & 0x1FFFFFFEu;
      PrefetchCacheLines(physical | (uint64_t(count & 0x3FFFFFF) << 29) |
                             (uint64_t((r[gr::XE_GPU_REG_VGT_DMA_SIZE] >> 30) & 3) << 55) | (uint64_t(1) << 62) |
                             (uint64_t(1) << 63),
                         physical);
    }
    if (!count) {
      return true;
    }
    VkPrimitiveTopology topology;
    bool quads = false;
    bool accepts_restart = false;
    switch (type) {
      case 2:
        topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        break;
      case 3:
        topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        accepts_restart = true;
        break;
      case 4:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        break;
      case 5:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        accepts_restart = true;
        break;
      case 6:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        accepts_restart = true;
        break;
      case 13:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        quads = true;
        break;
      default:
        return Reject(100 + type, "primitive type not supported yet");
    }
    if (source_kind == uint32_t(xenos::SourceSelect::kImmediate)) {
      return Reject(2, "immediate indices");
    }


    const bool no_clip = (r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 16) & 0x1;
    const uint32_t mode_sc = r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
    if (((mode_sc >> 3) & 0x3) == 2 && ((mode_sc >> 5) & 0x7) != 2) {
      return Reject(4, "polygons drawn as points or lines");
    }

    const VertexInput* input = InputOf(p);
    if (!input) {
      return false;
    }
    Stage(0, mark);
    std::chrono::steady_clock::time_point t_indices_185 = mark;


    const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
    const uint32_t mask_register =
        mode_edram == uint32_t(xenos::EdramMode::kColorDepth) ? r[gr::XE_GPU_REG_RB_COLOR_MASK] : 0;
    static constexpr uint32_t kInfoColor[4] = {
        gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
        gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO};
    uint64_t keys[5] = {};
    uint32_t masks = 0;
    bool has_target = false;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t mask = (mask_register >> (i * 4)) & 0xF;
      if (!mask || !ps || !((ps->outputs >> i) & 0x1)) {
        continue;
      }
      const uint32_t info = r[kInfoColor[i]];
      const uint32_t format = (info >> 16) & 0xF;
      if (!bo2zm_formats::ColorApprox(format)) {
        return Reject(200 + format, "color target format not supported yet");
      }
      keys[i] = (uint64_t(1) << 63) | (uint64_t(info & 0xFFF) << 24) | (uint64_t(format) << 16) |
                  pitch;
      masks |= mask << (i * 4);
      has_target = true;
    }


    if (skip_invisible_ && masks) {
      static constexpr uint32_t kBlendOf[4] = {
          gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
          gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
      const uint32_t control_color_inv = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool alpha_never =
          ((control_color_inv >> 3) & 0x1) && (control_color_inv & 0x7) == 0;
      uint32_t masks_useful = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t mask_i = (masks >> (i * 4)) & 0xF;
        if (!mask_i) {
          continue;
        }
        const uint32_t m = r[kBlendOf[i]] & 0x1FFF1FFF;
        const bool copy_color = (m & 0x1F) == 0 && ((m >> 8) & 0x1F) == 1 && ((m >> 5) & 0x7) == 0;
        const bool copy_alpha =
            ((m >> 16) & 0x1F) == 0 && ((m >> 24) & 0x1F) == 1 && ((m >> 21) & 0x7) == 0;
        const bool writes_rgb = (mask_i & 0x7) != 0 && !copy_color;
        const bool writes_alpha = (mask_i & 0x8) != 0 && !copy_alpha;
        if (writes_rgb || writes_alpha) {
          masks_useful |= mask_i << (i * 4);
        }
      }
      if ((!masks_useful || alpha_never) && !occlusion_open_) {


        const uint32_t dc = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
        const bool leaves_trail =
            !alpha_never && ((((dc >> 1) & 0x1) && ((dc >> 2) & 0x1)) || (dc & 0x1));
        ++counts_z_[alpha_never ? kInvisibleAlpha : kInvisibleBlend];
        if (!leaves_trail) {
          return true;
        }
        ++counts_z_[kInvisibleOnlyColor];
      }
    }


    if (masks && ps && has_target) {
      const uint32_t control_color_now = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool test_alpha =
          ((control_color_now >> 3) & 0x1) && (control_color_now & 0x7) != 7;
      const bool discards = test_alpha || ps->discards || (ps->outputs & 0x10);
      if (pitch >= 1600) {

      } else if (discards) {
        ++scene_with_discard_;
      } else {
        ++scene_no_discard_;
      }
    }
    bool only_alpha = false;
    if (!masks && ps) {
      const uint32_t control_color_now = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool test_alpha =
          ((control_color_now >> 3) & 0x1) && (control_color_now & 0x7) != 7;
      if (!test_alpha && !ps->discards && !(ps->outputs & 0x10)) {
        ++draws_ps_useless_;
        if (cvars_per_frame_ ? no_ps_no_color_frame_ : NoPsNoColor()) {
          ps = nullptr;
        }
      } else {
        ++draws_ps_needed_;


        if (pitch >= 1600) {
          (Float(r[kRegConstantsPs + 4]) != 0.0f ? ++shadows_alpha_active_ : ++shadows_alpha_off_);
        }


        if (cvars_per_frame_ ? ps_only_alpha_frame_ : PsOnlyAlpha()) {
          only_alpha = true;
        }
      }
    }


    const bool vegetation_early = only_alpha && no_vegetation_ && !occlusion_open_;


    if (vegetation_computed_ && cvars_per_frame_ && vegetation_ring_ != vegetation_early) {
      vegetation_detail_.early = vegetation_early;
      AnnotateVegetationModel(vegetation_flags_, vegetation_detail_);
    }
    if (vegetation_early) {
      ++draws_vegetation_soon_;
      return true;
    }
    const uint32_t control_depth = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
    if (control_depth & 0x3) {
      const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
      keys[4] = (uint64_t(1) << 62) | (uint64_t(info & 0xFFF) << 24) |
                  (uint64_t((info >> 16) & 0x1) << 16) | pitch;
      has_target = true;
    }
    if (!has_target || !pitch) {

      if (occlusion_open_ && pitch >= 640 && warnings_occlusion_draw_ < 8) {
        ++warnings_occlusion_draw_;
        REXLOG_INFO("[native] C2 occlusion draw {}: NO TARGET (not drawn): VS n{} PS n{} type {} count {} pitch {} mask "
                    "{:08X} depth {:08X} EDRAM mode {}",
                    warnings_occlusion_draw_, p.vs->number, ps ? int(ps->number) : -1, type, count, pitch,
                    mask_register, control_depth, mode_edram);
      }
      return true;
    }


    const bool target_shadows =
        pitch >= 1600 && keys[4] && !keys[0] && !keys[1] && !keys[2] && !keys[3];
    if (target_shadows) {


      bool skip = bo2zm::guard30::NoShadows(REXCVAR_GET(bo2zm_native_skip_shadows));
      const int32_t alternate = REXCVAR_GET(bo2zm_native_skip_shadows_alternate_s);
      if (!skip && alternate > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_shadows_)
                                  .count();
        skip = (seconds / alternate) % 2 == 1;
        if (skip != shadows_skipped_) {
          shadows_skipped_ = skip;
          REXLOG_INFO("[native] shadow test: {} (frame {})",
                      skip ? "without shadows" : "with shadows", frame_);
        }
      }
      if (skip) {
        return true;
      }
    }

    CutSubstage(15, t_indices_185);

    const uint32_t displacement = r[gr::XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
    const bool restart = accepts_restart && ((mode_sc >> 21) & 0x1);


    float scale_bias = 0.0f, offset_bias = 0.0f;
    if (type >= 4) {
      if (((mode_sc >> 11) & 0x1) && !(mode_sc & 0x1)) {
        scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
        offset_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
      }
      if (((mode_sc >> 12) & 0x1) && !((mode_sc >> 1) & 0x1) && scale_bias == 0.0f &&
          offset_bias == 0.0f) {
        scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE]);
        offset_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET]);
      }
    } else if ((mode_sc >> 13) & 0x1) {
      scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
      offset_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
    }


    float bias[2] = {
        offset_bias * (((r[gr::XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 0x1)
                                    ? float(uint32_t(1) << 24)
                                    : float((uint32_t(1) << 24) - 1)),
        scale_bias * (1.0f / 16.0f)};

    if (target_shadows && type >= 4 && bias[0] == 0.0f && bias[1] == 0.0f &&
        (shadows_bias_constant_ != 0 || shadows_bias_pending_ != 0)) {
      bias[0] = float(shadows_bias_constant_) * 1000.0f;
      bias[1] = float(shadows_bias_pending_) * 0.1f;
    }
    const bool with_bias = bias[0] != 0.0f || bias[1] != 0.0f;
    if (with_bias && warnings_bias_ < 8 &&
        std::memcmp(bias, bias_warned_, sizeof(bias)) != 0) {
      ++warnings_bias_;
      std::memcpy(bias_warned_, bias, sizeof(bias));
      REXLOG_INFO("[native] C6: depth bias: scale {} offset {} (mode {:08X}) -> constant {} slope {}",
                  scale_bias, offset_bias, mode_sc, bias[0], bias[1]);
    }
    CutSubstage(16, t_indices_185);
    uint32_t vmin = UINT32_MAX;
    uint32_t vmax = 0;
    bool with_indices = false;
    bool indices_16 = false;
    indices_.clear();
    indices_known_ = nullptr;
    indices_pool_ = 0;
    indices_scanned_ = true;
    if (source_kind == uint32_t(xenos::SourceSelect::kDMA)) {
      const uint32_t size = r[gr::XE_GPU_REG_VGT_DMA_SIZE];
      const auto order = static_cast<xenos::Endian>(size >> 30);
      const uint32_t bytes = indices32 ? 4 : 2;
      const uint32_t base = r[gr::XE_GPU_REG_VGT_DMA_BASE] & ~(bytes - 1);
      if (count > (size & 0xFFFFFF) || uint64_t(base & 0x1FFFFFFF) + uint64_t(count) * bytes >
                                               kMemoryPhysical) {
        if (warnings_indices_ < 8) {
          ++warnings_indices_;
          REXLOG_WARN("[native] C6 diag: indices: count {} words {} VGT_DMA_SIZE {:08X} VGT_DMA_BASE {:08X} 32 bits {} "
                      "type {} VS n{} PS n{}",
                      count, size & 0xFFFFFF, size, r[gr::XE_GPU_REG_VGT_DMA_BASE], indices32,
                      type, p.vs->number, ps ? int(ps->number) : -1);
        }
        return Reject(7, "indices outside their buffer");
      }
      const uint8_t* data = memory_->TranslatePhysical(base & 0x1FFFFFFF);
      const uint32_t index_restart = r[gr::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
      if (!indices32 && !restart && !displacement && !quads &&
          (order == xenos::Endian::k8in16 || order == xenos::Endian::kNone)) {


        indices16_.resize(count);
        uint16_t* output = indices16_.data();
        uint32_t min = 0xFFFF;
        uint32_t max = 0;

        const bool from_pool = icache_active_ && IndexCacheByWrites(data, count, order == xenos::Endian::k8in16,
                                                                    base & 0x1FFFFFFF, uint32_t(order), min, max,
                                                                    indices_pool_);

        indices_known_ = nullptr;
        if (dedupe_indices_on_ && !from_pool) {
          const uint32_t sync = g_syncs_ring.load(std::memory_order_relaxed);
          if (sync != dedupe_indices_sync_view_) {
            dedupe_indices_sync_view_ = sync;
            dedupe_indices_.Forget();
          }
          indices_known_ = dedupe_indices_.Find(base & 0x1FFFFFFF, count, uint32_t(order));
        }
        index_data_ = data;
        index_flip_ = order == xenos::Endian::k8in16;
        index_base_ = base & 0x1FFFFFFF;
        index_order_ = uint32_t(order);
        const bool check_known = indices_known_ && (dedupe_indices_checks_ < kDedupeIndicesChecks ||
                                                    (++dedupe_indices_sampled_ & 4095) == 0);
        if (from_pool) {
          indices_scanned_ = false;
        } else if (indices_known_ && !check_known) {
          min = indices_known_->min;
          max = indices_known_->max;
          indices_scanned_ = false;
          ++dedupe_indices_hits_;
        } else {

          Indices16(data, count, output, index_flip_, min, max);
          if (check_known) {
            CheckKnownIndices(count, min, max);
          }
        }
        vmin = min;
        vmax = max;
        indices_16 = true;
      } else {
        indices_.resize(count);
        uint32_t* output = indices_.data();
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t v;
          if (indices32) {
            uint32_t raw;
            std::memcpy(&raw, data + size_t(i) * 4, 4);
            v = xenos::GpuSwap(raw, order) & 0xFFFFFF;
          } else {
            uint16_t raw;
            std::memcpy(&raw, data + size_t(i) * 2, 2);
            v = xenos::GpuSwap(raw, order);
          }
          if (restart && v == index_restart) {
            output[i] = UINT32_MAX;
            continue;
          }
          v = (v + displacement) & 0xFFFFFF;
          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
          output[i] = v;
        }
      }
      if (vmin > vmax) {
        return true;
      }
      with_indices = true;
    } else {
      vmin = displacement;
      vmax = displacement + count - 1;
      if (quads) {
        indices_.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
          indices_[i] = displacement + i;
        }
        with_indices = true;
      }
    }
    if (vmax - vmin >= kMaxVerticesPerDraw) {
      return Reject(8, "vertex range too large");
    }
    if (quads) {
      const size_t n = indices_.size() / 4;
      converted_.resize(n * 6);
      for (size_t q = 0; q < n; ++q) {
        const uint32_t* i = &indices_[q * 4];
        uint32_t* o = &converted_[q * 6];
        o[0] = i[0]; o[1] = i[1]; o[2] = i[2];
        o[3] = i[0]; o[4] = i[2]; o[5] = i[3];
      }
      indices_.swap(converted_);
    }
    CutSubstage(17, t_indices_185);

    const uint32_t vertices = vmax - vmin + 1;


    struct VertexSource {
      const uint8_t* data;
      xenos::Endian order;
      uint32_t bytes;
      uint64_t address;
    };


    std::array<VertexSource, 16> sources;
    if (!fewer_copies_) {
      sources.fill({});
    }
    if (input->bindings.size() > sources.size()) {
      return Reject(9, "too many vertex streams");
    }
    VkDeviceSize bytes_vertices = 0;
    for (size_t b = 0; b < input->bindings.size(); ++b) {
      const VertexBinding& binding = input->bindings[b];
      const uint32_t d0 = r[kRegFetch + binding.slot * 2];
      const uint32_t d1 = r[kRegFetch + binding.slot * 2 + 1];
      if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex)) {
        return Reject(10, "invalid vertex fetch constant");
      }
      const uint64_t address = uint64_t(d0 & 0x1FFFFFFC);
      const uint64_t available = uint64_t((d1 >> 2) & 0xFFFFFF) * 4;
      const uint64_t start = uint64_t(vmin) * binding.stride;
      uint64_t needed = uint64_t(vertices) * binding.stride;
      if (start + needed > available) {
        Warn(11, "vertices past the end of their buffer: clipped");
        needed = available > start ? (available - start) / binding.stride * binding.stride : 0;
      }
      if (!needed || address + start + needed > kMemoryPhysical) {
        return Reject(12, "vertices outside memory");
      }
      sources[b] = {memory_->TranslatePhysical(uint32_t(address + start)),
                     static_cast<xenos::Endian>(d1 & 0x3), uint32_t(needed),
                     address + start};

      if (prefetch_lines_ && vcache_active_ && vcache_by_writes_) {
        PrefetchCacheLines(((address + start) & 0x1FFFFFFF) | (uint64_t(uint32_t(needed) & 0x3FFFFFF) << 29) |
                               (uint64_t(d1 & 0x3) << 55) | (uint64_t(1) << 63),
                           uint32_t(address + start));
      }
      if (needed >= 4096) {
        g_probe_vertex_address.store(uint32_t(address + start), std::memory_order_relaxed);
      }
      bytes_vertices += (needed + 3) & ~VkDeviceSize(3);
    }


    if (diagnostics_ < 32) {
      const uint64_t key_diagnostic = (uint64_t(p.vs->number) << 44) ^
                                         (uint64_t(ps ? ps->number + 1 : 0) << 32) ^ keys[0] ^
                                         (no_clip ? 1 : 0);
      if (diagnosed_.insert(key_diagnostic).second) {
        ++diagnostics_;
        float position[4] = {};
        for (const VertexAttribute& a : input->attributes) {
          if (a.location != 0) {
            continue;
          }
          const VertexSource& source = sources[a.binding];
          if (source.bytes >= a.offset + 16) {
            for (uint32_t k = 0; k < 4; ++k) {
              uint32_t word;
              std::memcpy(&word, source.data + a.offset + k * 4, 4);
              position[k] = Float(xenos::GpuSwap(word, source.order));
            }
          }
          break;
        }
        REXLOG_INFO(
            "[native] C6 diag: VS n{} PS n{} type {} count {} {} VTE {:08X} CLIP {:08X} vp x {}+-{} y {}+-{} z {}+{} "
            "window {:08X} sc {:08X} surface {:08X} color0 {:08X} mask {:08X} depth {:08X} edram {} vtx {:08X} pos0 "
            "({:.3f} {:.3f} {:.3f} {:.3f}){}",
            p.vs->number, ps ? int(ps->number) : -1, type, count,
            source_kind == 0 ? "indices" : "auto",
            r[gr::XE_GPU_REG_PA_CL_VTE_CNTL], r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL],
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]), r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET],
            mode_sc, r[gr::XE_GPU_REG_RB_SURFACE_INFO], r[gr::XE_GPU_REG_RB_COLOR_INFO],
            r[gr::XE_GPU_REG_RB_COLOR_MASK], control_depth, mode_edram,
            r[gr::XE_GPU_REG_PA_SU_VTX_CNTL], position[0], position[1], position[2], position[3],
            no_clip ? " [no clip]" : "");
      }
    }

    CutSubstage(18, t_indices_185);
    Stage(1, mark);


    uint32_t* const shared = shared_buffers_[shared_buffer_];
    if (!fewer_copies_) {
      std::memset(shared, 0, kSharedBytes);
    }
    VkDeviceSize bytes_textures = 0;
    DiscardHashesPlanned();
    textures_a_upload_.clear();
    const auto t_samplers =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};


    const bool composite_no_blur =
        ps && ps->shader && ps->shader->hash == kHashComposite && no_blur_frame_;
    const uint32_t mip_min = bo2zm::native::TextureMipFloor(
        p.vs->fetch_full ? p.vs->hash_no_fetch : p.vs->hash,
        r[gr::XE_GPU_REG_RB_DEPTHCONTROL], preset_mip_min_, diag_mip_min_);
    bool reads_reflection = false;


    const uint64_t generation_textures_before = generation_textures_;
    const uint64_t slots_retired_before = slots_retired_;
    bool reuse_textures = reuse_textures_on_ && reuse_valid_ && ps == reuse_ps_ &&
                          mip_min == reuse_mip_min_ && !composite_no_blur &&
                          generation_textures_ == reuse_generation_textures_ && frame_ == reuse_frame_ &&
                          slots_retired_ == reuse_slots_retired_;
    if (reuse_textures && p.generation_fetch != reuse_generation_fetch_) {
      reuse_textures = SameSamplerFetch(ps, r);
      if (reuse_textures) {
        reuse_generation_fetch_ = p.generation_fetch;
        ++reuse_by_value_;
      }
    }
    const bool check_reuse =
        reuse_textures && (reuse_checks_ < kReuseTexturesChecks || (++reuse_sampled_ & 1023) == 0);
    if (reuse_textures && !check_reuse) {
      std::memcpy(shared, reuse_shared_, kSharedBytes);
      reads_reflection = reuse_reads_reflection_;
      ++reuse_hits_;
    } else {
    if (fewer_copies_) {
      std::memset(shared, 0, kSharedBytes);
    }
    {
      const float one = 1.0f;
      for (uint32_t k = 0; k < 16; ++k) {
        std::memcpy(shared + kWordScale + k, &one, sizeof(one));
        std::memcpy(shared + kWordScaleCoords + k * 2, &one, sizeof(one));
        std::memcpy(shared + kWordScaleCoords + k * 2 + 1, &one, sizeof(one));
      }
    }
    if (composite_no_blur) {
      context_->DeadDepthReads(true);
    }
    for (const SamplerShader& sampler :
         ps ? std::span<const SamplerShader>(ps->samplers) : std::span<const SamplerShader>()) {
      if (sampler.reg_index >= 16) {
        continue;
      }
      const uint32_t* fetch = r + kRegFetch + uint32_t(sampler.reg_index) * 6;


      if (measure_visibility_ && (fetch[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
          ((fetch[1] & 0xFFFFF000u) & 0x1FFFFFFFu) == bo2zm::reflection_demand::kAddress) {
        reads_reflection = true;
      }
      ++samplers_prepared_;

      const auto t_one = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      uint32_t path = 20;
      bool no_cache = false;

      CacheSampler& cache = cache_samplers_[sampler.reg_index];

      if ((cache_between_frames_ ? frame_ <= cache.valid_until : cache.frame == frame_) &&
          cache.generation == generation_textures_ && cache.mip_min == mip_min && SlotCurrent(cache) &&
          std::memcmp(cache.fetch.data(), fetch, sizeof(cache.fetch)) == 0) {
        shared[cache.heap * 16 + sampler.reg_index] = cache.slot;
        shared[48 + sampler.reg_index] = cache.sampler;
        WriteInvSize(shared, sampler.reg_index, cache.width, cache.height);
        WriteScale(shared, sampler.reg_index, fetch);
        WriteScaleCoords(shared, sampler.reg_index, fetch, cache.width, cache.height);
        ++samplers_cache_;
        SamplerPath(19, t_one);
        continue;
      }
      uint32_t slot_texture = 0;
      uint32_t heap = 0;
      uint32_t slot_sampler = 0;
      uint32_t width_host = 0, height_host = 0;


      const size_t set = (XXH3_64bits(fetch, sizeof(uint32_t) * 6) ^ (uint64_t(mip_min) << 1)) &
                         (cache_fetch_.size() - 2);
      CacheSampler* via = &cache_fetch_[set];
      if (via->mip_min != mip_min || std::memcmp(via->fetch.data(), fetch, sizeof(via->fetch)) != 0) {
        CacheSampler* const other = &cache_fetch_[set + 1];
        if ((other->mip_min == mip_min && std::memcmp(other->fetch.data(), fetch, sizeof(other->fetch)) == 0) ||
            other->frame == UINT64_MAX ||
            (via->frame != UINT64_MAX && other->valid_until < via->valid_until)) {
          via = other;
        }
      }
      CacheSampler& per_fetch = *via;
      uint64_t valid_until = frame_;
      if ((cache_between_frames_ ? frame_ <= per_fetch.valid_until : per_fetch.frame == frame_) &&
          per_fetch.generation == generation_textures_ && per_fetch.mip_min == mip_min && SlotCurrent(per_fetch) &&
          std::memcmp(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch)) == 0) {
        slot_texture = per_fetch.slot;
        heap = per_fetch.heap;
        slot_sampler = per_fetch.sampler;
        valid_until = per_fetch.valid_until;
        width_host = per_fetch.width;
        height_host = per_fetch.height;
        ++samplers_cache_fetch_;
      } else {

        if (per_fetch.mip_min != mip_min || std::memcmp(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch)) != 0) {
          ++(per_fetch.frame == UINT64_MAX ? fetch_misses_empty_ : fetch_misses_collision_);
        } else if (per_fetch.generation != generation_textures_ || !SlotCurrent(per_fetch)) {
          ++fetch_misses_generation_;
        } else {
          ++fetch_misses_expired_;
        }
        bool sampling_point = false;
        path = 21;
        PrepareTexture(fetch, slot_texture, heap, bytes_textures, sampling_point, valid_until,
                        width_host, height_host);
        slot_sampler = SlotSampler(fetch, sampling_point, mip_min);
        per_fetch.frame = frame_;
        per_fetch.generation = generation_textures_;
        per_fetch.mip_min = mip_min;
        std::memcpy(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch));
        cache_fetch_dir_[size_t(&per_fetch - cache_fetch_.data())] = AddressCacheSampler(fetch);
        per_fetch.slot = slot_texture;
        per_fetch.heap = heap;
        per_fetch.generation_slot = GenerationSlot(heap, slot_texture);
        per_fetch.sampler = slot_sampler;
        per_fetch.valid_until = valid_until;
        per_fetch.width = width_host;
        per_fetch.height = height_host;


        if (composite_no_blur && sampling_point) {
          per_fetch.frame = 0;
          per_fetch.valid_until = 0;
          valid_until = 0;
          no_cache = true;
        }
      }
      shared[heap * 16 + sampler.reg_index] = slot_texture;
      shared[48 + sampler.reg_index] = slot_sampler;
      WriteInvSize(shared, sampler.reg_index, width_host, height_host);
      WriteScale(shared, sampler.reg_index, fetch);
      WriteScaleCoords(shared, sampler.reg_index, fetch, width_host, height_host);
      cache.frame = frame_;
      cache.generation = generation_textures_;
      cache.mip_min = mip_min;
      std::memcpy(cache.fetch.data(), fetch, sizeof(cache.fetch));
      cache.slot = slot_texture;
      cache.heap = heap;
      cache.generation_slot = GenerationSlot(heap, slot_texture);
      cache.sampler = slot_sampler;
      cache.valid_until = valid_until;
      cache.width = width_host;
      cache.height = height_host;
      if (no_cache) {
        cache.frame = 0;
      }
      SamplerPath(path, t_one);
      if (path == 21) {
        SamplerPath(22 + (prepare_new_ ? 5 : prepare_outcome_), t_one);
      }
    }
    if (composite_no_blur) {
      context_->DeadDepthReads(false);
    }
    if (check_reuse) {
      CheckReuseTextures(shared, reads_reflection, ps);
    }
    }


    if (!(fewer_copies_ && reuse_textures && !check_reuse)) {
      std::memcpy(reuse_shared_, shared, kSharedBytes);
      std::memcpy(reuse_fetch_, r + kRegFetch, sizeof(reuse_fetch_));
    }
    reuse_reads_reflection_ = reads_reflection;
    reuse_ps_ = ps;
    reuse_mip_min_ = mip_min;
    reuse_generation_fetch_ = p.generation_fetch;
    reuse_generation_textures_ = generation_textures_;
    reuse_frame_ = frame_;
    reuse_slots_retired_ = slots_retired_;
    reuse_valid_ = false;
    reuse_pending_ = !composite_no_blur && generation_textures_ == generation_textures_before &&
                     slots_retired_ == slots_retired_before;


    bool shadow_min_draw = false;
    if (ps && !ps->samplers.empty()) {
      if (const uint32_t dir_overlay = context_->AddressShadowOverlay()) {
        for (const SamplerShader& s : ps->samplers) {
          if (s.reg_index >= 16) {
            continue;
          }
          const uint32_t* f = r + kRegFetch + uint32_t(s.reg_index) * 6;
          if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture) ||
              (((f[1] >> 12) << 12) & 0x1FFFFFFF) != dir_overlay) {
            continue;
          }
          const bool capable = ps->shadow_min && s.map_shadows &&
                             ((f[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked);
          const NativeImage* pair = context_->PartnerShadowOverlay(capable, ps->number);
          if (!pair) {
            continue;
          }
          const uint32_t slot_pair =
              SlotView(pair->image, pair->format, (f[3] >> 1) & 0xFFF, kSwizzleRRRR);
          if (!slot_pair) {
            context_->PartnerShadowOverlay(false, ps->number);
            continue;
          }
          shared[16 + s.reg_index] = slot_pair;
          shadow_min_draw = true;
        }
      }
    }

    if (time_) {
      sub_ns_[14] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_samplers)
                                  .count());
      ++sub_n_[14];
    }
    Stage(2, mark);

    const VkDeviceSize bytes_indices =
        indices_16 ? VkDeviceSize(indices16_.size()) * 2 : VkDeviceSize(indices_.size()) * 4;

    const VkDeviceSize gap_base_zero =
        vertices_base_cero_ && input->bindings.size() == 1 ? VkDeviceSize(input->bindings[0].stride) : 0;
    const VkDeviceSize needed = gap_base_zero + bytes_vertices + bytes_indices + bytes_textures +
                                    2 * VkDeviceSize(kRegistersConstants) * 4 +
                                    kWordsShared * 4 + 64 * 8 +

                                    (use_ubo_ ? kUboBytesVs + kUboBytesPs + kUboBytesShared +
                                                     3 * alignment_ubo_
                                               : 0);
    if (needed > kSizeUpload) {
      return Reject(13, "draw larger than the upload buffer");
    }
    const bool shared_full =
        shared_separate_ && use_ubo_ &&
        shared_used_ + kUboBytesShared + alignment_ubo_ > kSizeShared;
    if (upload_used_ + needed > kSizeUpload || shared_full) {
      const auto before_submit = std::chrono::steady_clock::now();
      const bool submitted = context_->SubmitYWait();
      ++submits_full_;
      ns_submits_full_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - before_submit)
                                        .count());
      if (!submitted) {
        return false;
      }
    }


    uint64_t key_pass;
    if (key_pass_fast_ && pass_keys_valid_ && std::memcmp(keys, pass_keys_, sizeof(keys)) == 0) {
      const uint64_t n = ++keys_pass_fast_;
      key_pass = (n <= kKeysPassACheck || (n & 4095) == 0) ? CheckKeyPass(keys, n) : pass_key_;
    } else {
      key_pass = XXH3_64bits(keys, sizeof(keys));
    }


    draw_writes_ds_ = keys[4] != 0 && bo2zm::native::v055::WritesDepthStencil(control_depth);
    draw_learn_key_ = bo2zm::native::v055::LearnKey(key_pass, p.vs->number, ps ? uint32_t(ps->number) + 1u : 0u);
    if (pass_active_ && pass_depth_readonly_ && draw_writes_ds_ && key_pass == pass_key_ &&
        pass_generation_ == context_->GenerationCommands()) {
      pass_readonly_blocked_ = true;
      FinishPass();
    }
    if (!pass_active_ || key_pass != pass_key_ ||
        pass_generation_ != context_->GenerationCommands()) {
      const auto before_pass = std::chrono::steady_clock::now();
      if (pass_generation_ != context_->GenerationCommands()) {
        ++passes_per_generation_;
      } else if (!pass_active_ && key_pass == pass_key_) {
        ++passes_resumed_;
      } else {
        ++passes_per_target_;
      }
      FinishPass();


      const auto after_finish = std::chrono::steady_clock::now();
      if (time_) {
        stages_ns_[8] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            after_finish - before_pass).count());
      }
      const bool begun = BeginPass(r, keys, pitch, key_pass, ps);
      const auto after_pass = std::chrono::steady_clock::now();
      ns_passes_ += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());

      if (time_) {
        stages_ns_[7] += uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());
      }
      if (!begun) {
        return false;
      }
    }
    if (draw_writes_ds_) {
      pass_ds_written_ = true;
    }
    const VkCommandBuffer cmd = pass_commands_;
    if (recording_generation_ != context_->GenerationCommands()) {
      recording_generation_ = context_->GenerationCommands();
      pipeline_bound_ = VK_NULL_HANDLE;
      eds_valid_ = false;
      key_bound_valid_ = false;
      sets_bound_ = false;
      ubo_bound_ = false;
      state_recorded_ = false;
      bindings_recorded_ = 0;
    }

    Stage(3, mark);


    auto t_uploads = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (CachedTexture* texture : textures_a_upload_) {
      if (!UploadTexture(*texture)) {
        return false;
      }
    }
    CutSubstage(36, t_uploads);
    reuse_valid_ = reuse_pending_ && generation_textures_ == reuse_generation_textures_ &&
                   slots_retired_ == reuse_slots_retired_;
    std::array<VkDeviceSize, 16> offsets_vertices{};


    if (dedupe_active_) {
      const uint32_t sync = g_syncs_ring.load(std::memory_order_relaxed);
      if (sync != dedupe_sync_view_) {
        dedupe_sync_view_ = sync;
        dedupe_.Forget();
      }
    }
    const auto before_vertices =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (size_t b = 0; b < input->bindings.size(); ++b) {
      const VertexSource& source = sources[b];

      const auto t_binding = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      if (diag_vertices_repeated_) {
        AnnotateVerticesRepeated(source.address, source.bytes, uint32_t(source.order), source.data);
      }
      VkDeviceSize offset;


      const uint64_t hash_dedupe =
          dedupe_active_ && dedupe_hash_frame_ ? XXH3_64bits(source.data, size_t(source.bytes)) | 1 : 0;


      if (dedupe_active_ &&
          dedupe_.Find(source.address, source.bytes, uint32_t(source.order), offset, hash_dedupe)) {
        offsets_vertices[b] = offset;
        SamplerPath(40, t_binding);
        continue;
      }


      if (skin_on_ && bo2zm::gpu_skin::MayOverlap(uint32_t(source.address), source.bytes) &&
          SkinOnGpu(source.data, source.order, source.bytes, source.address, input->bindings[b].stride,
                    input->bindings.size() == 1, offset)) {
        offsets_vertices[b] = offset;
        bytes_vertices_ += source.bytes;
        if (dedupe_active_) {
          dedupe_.Annotate(source.address, source.bytes, uint32_t(source.order), offset, hash_dedupe);
        }
        SamplerPath(42, t_binding);
        continue;
      }


      if (vcache_active_) {
        const VkDeviceSize alignment =
            vertices_base_cero_ && input->bindings.size() == 1 ? VkDeviceSize(input->bindings[0].stride) : 4;
        uint64_t hash_cache = hash_dedupe;
        if (VertexCache(source.data, source.bytes, source.order, source.address, alignment, hash_cache, offset)) {
          offsets_vertices[b] = offset;
          if (dedupe_active_) {
            dedupe_.Annotate(source.address, source.bytes, uint32_t(source.order), offset, hash_dedupe);
          }
          SamplerPath(41, t_binding);
          continue;
        }
      }

      if (vertices_base_cero_ && input->bindings.size() == 1) {
        ReserveMultiple(source.bytes, input->bindings[0].stride, offset);
      } else {
        Reserve(source.bytes, 4, offset);
      }
      const CopyJob job{source.data, upload_data_ + offset, source.bytes / 4, source.order};
      if (!copies_active_ || !EnqueueCopy(job)) {
        CopyVertices(job);
      }
      if (watch_vertices_) [[unlikely]] {
        WatchVerticesCollect(source.address, source.bytes);
      }
      bytes_vertices_ += source.bytes;
      offsets_vertices[b] = offset;
      if (dedupe_active_) {
        dedupe_.Annotate(source.address, source.bytes, uint32_t(source.order), offset, hash_dedupe);
      }
      SamplerPath(42, t_binding);
    }
    if (time_) {
      ns_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - before_vertices)
                                   .count());
    }
    CutSubstage(37, t_uploads);
    VkDeviceSize offset_indices = 0;
    if (with_indices) {
      if (indices_pool_) {
        offset_indices = indices_pool_;
        bytes_indices_saved_ += bytes_indices;
      } else if (indices_16 && indices_known_ && indices_known_->frame == dedupe_indices_.frame()) {

        offset_indices = indices_known_->offset;
        bytes_indices_saved_ += bytes_indices;
      } else {
        Reserve(bytes_indices, 4, offset_indices);
        bytes_indices_uploaded_ += bytes_indices;
        if (indices_16 && indices_known_ && !indices_scanned_) {


          uint32_t mn = 0;
          uint32_t mx = 0;
          Indices16Neon(index_data_, uint32_t(bytes_indices / 2),
                        reinterpret_cast<uint16_t*>(upload_data_ + offset_indices), index_flip_, mn, mx);
          ++dedupe_indices_converted_;
        } else {
          std::memcpy(upload_data_ + offset_indices,
                      indices_16 ? static_cast<const void*>(indices16_.data())
                                    : static_cast<const void*>(indices_.data()),
                      size_t(bytes_indices));
        }
        if (indices_16 && dedupe_indices_on_) {

          const uint64_t hash = dedupe_indices_checks_ < kDedupeIndicesChecks || (dedupe_indices_sampled_ & 4095) < 64
                                    ? XXH3_64bits(index_data_, size_t(bytes_indices))
                                    : 0;
          dedupe_indices_.Annotate(index_base_, uint32_t(bytes_indices / 2), index_order_, uint16_t(vmin),
                                   uint16_t(vmax), offset_indices, hash);
        }
      }
    }
    indices_known_ = nullptr;
    indices_scanned_ = true;
    CutSubstage(38, t_uploads);


    const uint32_t bytes_vs = std::min<uint32_t>(p.vs->constants_bytes, kRegistersConstants * 4);

    const bool vs_runs = constants_used_active_ && p.vs->constants_runs_valid;
    if (draw_batch::enabled.load(std::memory_order_relaxed) && draw_batch::SameConstants(
        !vs_runs && !constants_vs_shader_, epoch_upload_, constants_vs_epoch_,
        p.generation_constants_vs, constants_vs_generation_,
        {constants_vs_shadow_.data(), constants_vs_bytes_},
        {reinterpret_cast<const uint8_t*>(r + kRegConstantsVs), bytes_vs})) {
      constants_vs_generation_ = p.generation_constants_vs;
      draw_batch::constants_reused.fetch_add(1, std::memory_order_relaxed);
    }
    if (constants_vs_generation_ != p.generation_constants_vs ||
        constants_vs_epoch_ != epoch_upload_ || bytes_vs > constants_vs_bytes_ ||
        (constants_vs_shader_ && constants_vs_shader_ != p.vs)) {

      ++reuploads_vs_[constants_vs_generation_ != p.generation_constants_vs ? 0
                      : constants_vs_epoch_ != epoch_upload_                 ? 1
                                                                              : 2];

      const auto t_copy = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_vs, kUboBytesVs) : bytes_vs, use_ubo_ ? alignment_ubo_ : 16,
               constants_vs_offset_);
      if (vs_runs) {
        for (const auto& [first, n] : p.vs->constants_runs) {
          std::memcpy(upload_data_ + constants_vs_offset_ + first * 16u, r + kRegConstantsVs + first * 4u, n * 16u);
        }
        bytes_constants_vs_ += p.vs->constants_runs_bytes;
      } else {
        if (draw_batch::enabled.load(std::memory_order_relaxed)) {
          std::memcpy(constants_vs_shadow_.data(), r + kRegConstantsVs, bytes_vs);
          std::memcpy(upload_data_ + constants_vs_offset_, constants_vs_shadow_.data(), bytes_vs);
        } else {
          std::memcpy(upload_data_ + constants_vs_offset_, r + kRegConstantsVs, bytes_vs);
        }
        bytes_constants_vs_ += bytes_vs;
      }
      constants_vs_shader_ = vs_runs ? p.vs : nullptr;
      SamplerPath(43, t_copy);
      constants_vs_generation_ = p.generation_constants_vs;
      constants_vs_epoch_ = epoch_upload_;
      constants_vs_bytes_ = bytes_vs;
    }
    const uint32_t bytes_ps =
        ps ? std::min<uint32_t>(ps->constants_bytes, kRegistersConstants * 4) : 0;
    const bool ps_runs = ps && constants_used_active_ && ps->constants_runs_valid;
    if (draw_batch::enabled.load(std::memory_order_relaxed) && ps && draw_batch::SameConstants(
        !ps_runs && !constants_ps_shader_, epoch_upload_, constants_ps_epoch_,
        p.generation_constants_ps, constants_ps_generation_,
        {constants_ps_shadow_.data(), constants_ps_bytes_},
        {reinterpret_cast<const uint8_t*>(r + kRegConstantsPs), bytes_ps})) {
      constants_ps_generation_ = p.generation_constants_ps;
      draw_batch::constants_reused.fetch_add(1, std::memory_order_relaxed);
    }
    if (ps && (constants_ps_generation_ != p.generation_constants_ps ||
               constants_ps_epoch_ != epoch_upload_ || bytes_ps > constants_ps_bytes_ ||
               (constants_ps_shader_ && constants_ps_shader_ != ps))) {
      ++reuploads_ps_[constants_ps_generation_ != p.generation_constants_ps ? 0
                      : constants_ps_epoch_ != epoch_upload_                 ? 1
                                                                              : 2];
      const auto t_copy = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps, use_ubo_ ? alignment_ubo_ : 16,
               constants_ps_offset_);
      if (ps_runs) {
        for (const auto& [first, n] : ps->constants_runs) {

          if (first * 16u + n * 16u <= bytes_ps) {
            std::memcpy(upload_data_ + constants_ps_offset_ + first * 16u, r + kRegConstantsPs + first * 4u,
                        n * 16u);
          }
        }
        bytes_constants_ps_ += ps->constants_runs_bytes;
      } else {
        if (draw_batch::enabled.load(std::memory_order_relaxed)) {
          std::memcpy(constants_ps_shadow_.data(), r + kRegConstantsPs, bytes_ps);
          std::memcpy(upload_data_ + constants_ps_offset_, constants_ps_shadow_.data(), bytes_ps);
        } else {
          std::memcpy(upload_data_ + constants_ps_offset_, r + kRegConstantsPs, bytes_ps);
        }
        bytes_constants_ps_ += bytes_ps;
      }
      constants_ps_shader_ = ps_runs ? ps : nullptr;
      SamplerPath(44, t_copy);
      constants_ps_generation_ = p.generation_constants_ps;
      constants_ps_epoch_ = epoch_upload_;
      constants_ps_bytes_ = bytes_ps;
    }
    CopyCompositeTreated(ps, r, bytes_ps);
    CutSubstage(39, t_uploads);

    Stage(4, mark);

    auto t_pipeline = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};


    const uint32_t vte = r[gr::XE_GPU_REG_PA_CL_VTE_CNTL];
    VkViewport viewport{};
    float ndc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    VkRect2D scissor{};
    uint32_t framing_empty = 0;
    const bool framing_in_cache = framing_cache_ && framing_valid_ &&
                                   p.generation_framing == framing_generation_ && pass_series_ == framing_pass_series_;
    bool check_framing = false;
    if (framing_in_cache) {
      const uint64_t n = ++framing_hits_;
      check_framing = n <= kFramingsACheck || (n & 4095) == 0;
      ++framing_hits_report_;
    }
    if (framing_in_cache && !check_framing) {
      viewport = framing_viewport_;
      std::memcpy(ndc, framing_ndc_, sizeof(ndc));
      scissor = framing_scissor_;
      framing_empty = framing_empty_;
    } else {
      framing_empty = ComputeFraming(r, vte, no_clip, mode_sc, viewport, ndc, scissor);
      ++framing_calcs_report_;
      if (check_framing) {
        CompareFraming(viewport, ndc, scissor, framing_empty);
      }
      framing_viewport_ = viewport;
      std::memcpy(framing_ndc_, ndc, sizeof(ndc));
      framing_scissor_ = scissor;
      framing_empty_ = framing_empty;
      framing_generation_ = p.generation_framing;
      framing_pass_series_ = pass_series_;
      framing_valid_ = true;
    }
    if (framing_empty != 0) {
      return true;
    }

    uint32_t specialization = input->specialization;
    if (composite_no_blur) {
      specialization |= kSpecNoBlur;
    }
#ifndef BO2ZM_NO_VARIANTS_GLOW
    if (ps && ps->shader && ps->shader->hash == kHashBrightPass) {
      const int mode = GlowSky();
      specialization |= mode == 1 ? kSpecGlowNatural : mode == 2 ? kSpecGlowSmooth : 0;
    }
#endif
    const uint32_t control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    float threshold_alpha = 0.0f;
    uint32_t function_alpha = 7;


    if (ps && ((control_color >> 3) & 0x1) && (control_color & 0x7) != 7) {
      threshold_alpha = Float(r[gr::XE_GPU_REG_RB_ALPHA_REF]);
      function_alpha = control_color & 0x7;
      specialization |= 0x2;


      specialization |= (function_alpha & 0x7u) << kSpecFunctionAlphaOffset;
    }
    shared[64] = (r[kRegBooleans] & 0xFFFF) | ((r[kRegBooleans + 4] & 0xFFFF) << 16);
    shared[65] = 0;
    if (!(r[gr::XE_GPU_REG_PA_SU_VTX_CNTL] & 0x1)) {
      const float mid[2] = {1.0f / viewport.width, -1.0f / std::abs(viewport.height)};
      std::memcpy(&shared[66], mid, sizeof(mid));
    }
    std::memcpy(&shared[68], &threshold_alpha, sizeof(threshold_alpha));
    shared[69] = function_alpha;
    std::memcpy(&shared[70], ndc, sizeof(ndc));
    std::copy(input->remaps.begin(), input->remaps.end(), shared + 74);


    {
      const float pixel_scales[2] = {pass_scale_x_ > 0.0f ? 1.0f / pass_scale_x_ : 1.0f,
                                     pass_scale_y_ > 0.0f ? 1.0f / pass_scale_y_ : 1.0f};
      std::memcpy(&shared[kWordPixelScale], pixel_scales, sizeof(pixel_scales));
    }
    VkDeviceSize offset_shared;


    ++shared_observed_;
    const auto t_shared =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    uint32_t* const shared_previous = shared_buffers_[shared_buffer_ ^ 1];
    if (shared_epoch_ == epoch_upload_ && std::memcmp(shared, shared_previous, kSharedBytes) == 0) {
      offset_shared = shared_offset_;
    } else {
      ++shared_changed_;
      if (shared_separate_ && use_ubo_) {

        offset_shared = (shared_used_ + alignment_ubo_ - 1) & ~(alignment_ubo_ - 1);
        shared_used_ = offset_shared + kUboBytesShared;
        std::memcpy(shared_data_ + offset_shared, shared, kSharedBytes);
      } else {
        Reserve(use_ubo_ ? kUboBytesShared : kWordsShared * 4, use_ubo_ ? alignment_ubo_ : 16,
                 offset_shared);
        std::memcpy(upload_data_ + offset_shared, shared, kSharedBytes);
      }


      if (shared_swap_) {
        shared_buffer_ ^= 1;
      } else {
        std::memcpy(shared_previous, shared, kSharedBytes);
      }
      shared_offset_ = offset_shared;
      shared_epoch_ = epoch_upload_;
    }
    if (time_) {
      sub_ns_[12] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_shared)
                                  .count());
      ++sub_n_[12];
    }

    CutSubstage(48, t_pipeline);

    PipelineKey key;
    key.vs = p.vs->number + 1;
    key.ps = ps ? ps->number + 1 : 0;
    key.input = input->hash;
    key.topology = uint32_t(topology);
    if (use_ubo_) {
      specialization |= kSpecConstantsUbo;
    }
    if (inv_size_tex_) {
      specialization |= kSpecInvSizeTex;
    }
    if (pcf_cheap_) {
      specialization |= kSpecPcfCheap;
    }
    if (only_alpha && ps) {

      if (no_vegetation_) {
        return true;
      }
      specialization |= kSpecOnlyAlpha;
    }


    if (ps && keys[4] && masks) {
      const bool test_z = ((control_depth >> 1) & 0x1) != 0;
      const bool writes_z = test_z && ((control_depth >> 2) & 0x1) != 0;
      const bool has_stencil = (control_depth & 0x1) != 0;
      const bool ps_writes_z = (ps->outputs & 0x10) != 0;
      const bool late = (specialization & 0x2) || ps->discards || ps_writes_z;
      if (!test_z) {

      } else if (!late) {
        ++counts_z_[kZAlreadyEarly];
      } else if (writes_z || ps_writes_z) {
        ++counts_z_[kZWritesZ];
      } else if (has_stencil) {
        ++counts_z_[kZStencil];
      } else if (occlusion_open_) {
        ++counts_z_[kZOcclusion];
      } else if (ps->shader && ps->shader->hash != kHashBrightPass &&
                 ps->shader->hash != kHashComposite) {
        ++counts_z_[kZSet];
        if (z_early_) {
          specialization |= kSpecZEarly;
        }
      }
    }

    if (shadow_min_draw) {
      specialization |= kSpecShadowMin;
    }


    if (pass_overlay_shadow_) {
      const uint32_t control_z_overlay = keys[4] ? control_depth : 0;
      const bool writes_z_overlay = ((control_z_overlay >> 1) & 0x1) && ((control_z_overlay >> 2) & 0x1);
      const uint32_t function_z_overlay = (control_z_overlay >> 4) & 0x7;
      context_->OverlayShadowDraw(!(control_z_overlay & 0x1) && !occlusion_open_ &&
                                          (!writes_z_overlay || function_z_overlay == 0 || function_z_overlay == 1 ||
                                           function_z_overlay == 3),
                                      control_z_overlay);
    }
    key.specialization = specialization;
    std::copy(std::begin(pass_formats_), std::end(pass_formats_), std::begin(key.formats));
    static constexpr uint32_t kBlend[4] = {
        gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
        gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
    for (uint32_t i = 0; i < 4; ++i) {
      if (keys[i]) {
        key.blend[i] = r[kBlend[i]] & 0x1FFF1FFF;
      }
    }
    key.masks = masks;
    key.depth = keys[4] ? control_depth : 0;

    if (bo2zm::native::DebugNoDepth()) {
      key.depth = 0;
    }
    if (key.depth && occlusion_open_ && pitch >= 640 && !masks &&
        REXCVAR_GET(bo2zm_native_test_occlusion_always)) {
      key.depth |= 0x7 << 4;
    }
    key.rasterization = (mode_sc & 0x7) | (restart ? 0x8 : 0) | (with_bias ? 0x10 : 0);
    VkPipeline pipeline = VK_NULL_HANDLE;


    CutSubstage(49, t_pipeline);
    const uint32_t shader_probe_mode = context_->ShaderProbeMode();
    auto shader_probe_reason = shader_probe::Reason::kNoColorShader;
    VkPipeline shader_probe_pipeline = VK_NULL_HANDLE;
    uint64_t shader_probe_compile_ns = 0;
    bool shader_probe_replaced = false;
    if (shader_probe_mode) {
      const bool simple = ps && !ps->discards && !(ps->outputs & 0x10) &&
          !(specialization & 0x2) && !ColorBlendEnabled(masks & 0xf, key.blend[0]);
      const bool pure = simple && DrawBatchShaderSafe(*p.vs) && ShaderProbeFragmentSafe(*ps);
      shader_probe_reason = shader_probe::Eligible(ps != nullptr, ps ? ps->outputs : 0,
          ps && ps->discards, (specialization & 0x2) != 0,
          ColorBlendEnabled(masks & 0xf, key.blend[0]), occlusion_open_ || diag_stats_draw_, pure);
      if (shader_probe_reason == shader_probe::Reason::kEligible) {
        auto flat = key;
        flat.ps = 1;
        flat.specialization |= shader_probe::kFlat;
        const uint64_t before_compile = ns_pipelines_;
        shader_probe_pipeline = PipelineOf(LookupKey(flat), *input, p);
        shader_probe_compile_ns = ns_pipelines_ - before_compile;
      }
    }
    BO2ZM_SUB(11, pipeline = PipelineOf(LookupKey(key), *input, p));
    if (pipeline == VK_NULL_HANDLE && shader_probe_mode)
      context_->ShaderProbeResult(uint32_t(shader_probe_reason), 0, false, false, shader_probe_compile_ns, true);
    if (pipeline != VK_NULL_HANDLE && shader_probe_mode == 2 && shader_probe_pipeline != VK_NULL_HANDLE) {
      pipeline = shader_probe_pipeline;
      shader_probe_replaced = true;
    }
    if (pipeline == VK_NULL_HANDLE) {
      return false;
    }
#if REX_PLATFORM_SWITCH


    if (nvk_prefetch_app_ && pipeline != pipeline_bound_ && vk_switch_prefetch_pipeline) {

      deferred::Prefetch(vk_switch_prefetch_pipeline, pipeline);
    }
#endif
    CutSubstage(50, t_pipeline);

    Stage(5, mark);


    VkRect2D scissor_final = scissor;
    const uint32_t pixel_probe_mode = context_->PixelProbeMode();
    const bool pixel_probe_clipped = pixel_probe::Clip(
        scissor_final, pixel_probe_mode == 2 && TargetCategory(pitch, keys) == kGpuReflection);
    if (const uint32_t category_scissor = scissor_test_; category_scissor != 0) {
      const uint32_t category = TargetCategory(pitch, keys);
      static constexpr uint32_t kCategoryOf[6] = {0, kGpuShadows, kGpuScene, kGpu320,
                                                   kGpuSceneNoDepth, kGpuReflection};
      if (category_scissor <= 5 && category == kCategoryOf[category_scissor]) {
        scissor_final.extent = {1, 1};
      }
    }


    {
      if (drawn_extent_memo_ == nullptr || drawn_extent_memo_pitch_ != pitch) {
        drawn_extent_memo_ = &drawn_extent_[pitch];
        drawn_extent_memo_pitch_ = pitch;
      }
      bo2zm::native::v046::DrawnExtent& e = *drawn_extent_memo_;
      const uint32_t right = uint32_t(std::max(0, scissor.offset.x + int32_t(scissor.extent.width)));
      const uint32_t bottom = uint32_t(std::max(0, scissor.offset.y + int32_t(scissor.extent.height)));
      e.right = std::max(e.right, right);
      e.bottom = std::max(e.bottom, bottom);
      ++e.draws;
      if (pass_fitted_) {
        const int32_t aw = int32_t(pass_area_width_), ah = int32_t(pass_area_height_);
        const int32_t x1 = scissor_final.offset.x + int32_t(scissor_final.extent.width);
        const int32_t y1 = scissor_final.offset.y + int32_t(scissor_final.extent.height);
        if (x1 > aw || y1 > ah) {
          e.banned = true;
          bo2zm::native::v046::g_fit_clipped.fetch_add(1, std::memory_order_relaxed);
          const int32_t cx0 = std::min(scissor_final.offset.x, aw), cy0 = std::min(scissor_final.offset.y, ah);
          scissor_final.offset = {cx0, cy0};
          scissor_final.extent = {uint32_t(std::max(0, std::min(x1, aw) - cx0)),
                                  uint32_t(std::max(0, std::min(y1, ah) - cy0))};
        }
      }
    }
    if (area_util_) {
      const uint32_t until = uint32_t(std::max(0, scissor.offset.y + int32_t(scissor.extent.height)));


      const uint32_t rounded = bo2zm::native::v055::UsefulRows(until, area_to_tile_);
      uint32_t& pointed = HeightUsefulOf(pitch);
      if (rounded > pointed) {
        pointed = rounded;
      }
    }
    const float blend_constant[4] = {Float(r[gr::XE_GPU_REG_RB_BLEND_RED]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_GREEN]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_BLUE]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_ALPHA])};
    const uint32_t stencil_frontface = r[gr::XE_GPU_REG_RB_STENCILREFMASK];
    const uint32_t stencil_backface = ((control_depth >> 7) & 0x1)
                                       ? r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF]
                                       : stencil_frontface;


    bool opaque_in_all = true;
    for (uint32_t i = 0; i < 4 && opaque_in_all; ++i) {
      const uint32_t mask_i = (masks >> (i * 4)) & 0xF;
      if (!mask_i) {
        continue;
      }
      const uint32_t m = key.blend[i];
      const bool blend_on = ColorBlendEnabled(mask_i, m);
      if (blend_on) {
        opaque_in_all = false;
      }
    }
    const bool test_z_draw = ((control_depth >> 1) & 0x1) != 0;
    const bool writes_z_draw = test_z_draw && ((control_depth >> 2) & 0x1) != 0 &&
                                  keys[4] != 0;
    bool is_sky = false;
    if (ps && ps->shader && ps->shader->hash == kHashSky && masks) {
      ++sky_seen_;
      const bool has_stencil = (control_depth & 0x1) != 0;


      const bool in_scene = TargetCategory(pitch, keys) == kGpuScene;


      const uint32_t indices_draw =
          uint32_t(indices_16 ? indices16_.size() : indices_.size());
      const bool dome_geometry = indices_draw == kIndicesDomeSky;
      const bool first_in_pass = draws_in_pass_ == 0;

      const bool is_dome = in_scene && opaque_in_all && test_z_draw &&
                              !writes_z_draw && !has_stencil && !(ps->outputs & 0x10) &&
                              !occlusion_open_ && dome_geometry && first_in_pass;
      if (is_dome) {
        ++sky_candidates_;


        if (++sky_guard_in_frame_ > 1 && sky_guard_ != kSkyDiscarded) {
          sky_guard_ = kSkyDiscarded;
          REXLOG_WARN("[native] C6 sky: the guard sees {} domes in the SAME frame; nothing is postponed for the whole "
                      "session (a known bug of older builds). The image is untouched",
                      sky_guard_in_frame_);
        }
      }
      is_sky = sky_postpone_on_ && is_dome && sky_guard_ == kSkyPostponing;
      if (!is_sky && sky_postpone_on_) {
        ++sky_no_postponable_;
      }
    }


    if (sky_pending_) {
      if (is_sky) {
        ++sky_two_in_pass_;
        EmitSkyPostponed(cmd, kSkyPerOtherSky);
      } else if (!(opaque_in_all && writes_z_draw)) {
        EmitSkyPostponed(cmd, opaque_in_all ? kSkyPerNoZ : kSkyPerBlend);
      }
    }

    if (is_sky && reads_reflection) {
      bo2zm::reflection_demand::AnnotateVisible(false);
    }
    if (is_sky) {


      PostponedSky& c = sky_;
      c.pipeline = pipeline;
      c.buffer = upload_;
      c.uses_ubo = use_ubo_;
      c.push[0] = upload_address_ + constants_vs_offset_;
      c.push[1] = upload_address_ + constants_ps_offset_;
      c.push[2] = upload_address_ + offset_shared;
      c.offsets_ubo = use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constants_vs_offset_),
                                                          uint32_t(constants_ps_offset_),
                                                          uint32_t(offset_shared)}
                                : std::array<uint32_t, 3>{0, 0, 0};
      c.slot_ubo = slot_current_;
      c.viewport = viewport;
      c.scissor = scissor_final;
      std::memcpy(c.blend, blend_constant, sizeof(c.blend));
      std::memcpy(c.bias, bias, sizeof(c.bias));
      c.with_stencil = keys[4] != 0;
      c.stencil[0] = stencil_frontface;
      c.stencil[1] = stencil_backface;
      c.n_bindings = uint32_t(input->bindings.size());
      c.offsets_vertices = offsets_vertices;
      c.with_indices = with_indices;
      c.indices_16 = indices_16;
      c.indices = uint32_t(indices_16 ? indices16_.size() : indices_.size());
      {
        VkDeviceSize at_indices = offset_indices;
        ResolveVertexBinding(offset_indices, upload_, c.index_buffer, at_indices);
        c.first_index = uint32_t(at_indices / (indices_16 ? 2 : 4));
      }
      c.vmin = vmin;
      c.count = count;
      c.ps_more_one = ps ? ps->number + 1 : 0;
      c.vs_more_one = p.vs->number + 1;
      c.category = TargetCategory(pitch, keys);
      c.draws_on_postpone = draws_in_pass_;
      c.eds_mode = eds_mode_;
      if (eds_mode_) {
        StateEdsOf(key, c.eds, eds_mode_);
      }
      sky_pending_ = true;
      ++skies_postponed_;

      ++draws_per_category_[c.category];
      triangles_per_category_[c.category] += (with_indices ? c.indices : count) / 3;
      if (pixel_probe_mode) {
        context_->PixelProbeDraw(c.category, with_indices ? c.indices : count, pixel_probe_clipped);
      }
      Stage(6, mark);
      ++drawn_;


      drawn_timed_ += time_ ? 1 : 0;
      return true;
    }
    if (pipeline != pipeline_bound_) {
      if (count_changes_pipeline_) {
        CountChangePipeline(key, pipeline_bound_ == VK_NULL_HANDLE);
      }
      BO2ZM_SUB(0, dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
      pipeline_bound_ = pipeline;
    }


    if (eds_mode_ && !SetStateDynamic(cmd, key)) {
      shader_probe_replaced = false;
      BO2ZM_SUB(11, pipeline = PipelineOf(LookupKey(key), *input, p));
      if (pipeline == VK_NULL_HANDLE) {
        if (shader_probe_mode) context_->ShaderProbeResult(uint32_t(shader_probe_reason),0,false,false,shader_probe_compile_ns,true);
        return false;
      }
      BO2ZM_SUB(0, dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
      pipeline_bound_ = pipeline;
      key_bound_valid_ = false;
    }
    if (!sets_bound_) {
      BO2ZM_SUB(1, dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 4,
                                                sets_.data(), 0, nullptr));
      sets_bound_ = true;
    }


    const bool recorded = state_recorded_;


    if (!use_ubo_) {
      const uint64_t push[3] = {upload_address_ + constants_vs_offset_,
                                upload_address_ + constants_ps_offset_,
                                upload_address_ + offset_shared};
      if (!recorded || std::memcmp(push, push_recorded_, sizeof(push)) != 0) {
        dfn_.vkCmdPushConstants(cmd, layout_pipeline_,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(push), push);
        std::memcpy(push_recorded_, push, sizeof(push));
      }
    }


    {
      const std::array<uint32_t, 3> offsets_ubo =
          use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constants_vs_offset_), uint32_t(constants_ps_offset_),
                                              uint32_t(offset_shared)}
                    : std::array<uint32_t, 3>{0, 0, 0};
      ++set4_draws_;
      if (!ubo_bound_ || slot_ubo_bound_ != slot_current_ || offsets_ubo != offsets_ubo_bound_) {


        if (!ubo_bound_ || slot_ubo_bound_ != slot_current_) {
          ++set4_first_;
        } else {
          ++set4_changes_[(offsets_ubo[0] != offsets_ubo_bound_[0] ? 1u : 0u) |
                          (offsets_ubo[1] != offsets_ubo_bound_[1] ? 2u : 0u) |
                          (offsets_ubo[2] != offsets_ubo_bound_[2] ? 4u : 0u)];
        }
        BO2ZM_SUB(2, dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 4, 1,
                                                  &sets_ubo_[slot_current_], 3, offsets_ubo.data()));
        offsets_ubo_bound_ = offsets_ubo;
        slot_ubo_bound_ = slot_current_;
        ubo_bound_ = true;
      }
    }
    if (!recorded || std::memcmp(&viewport, &viewport_recorded_, sizeof(viewport)) != 0) {
      BO2ZM_SUB(3, dfn_.vkCmdSetViewport(cmd, 0, 1, &viewport));
      viewport_recorded_ = viewport;
    }
    if (!recorded || std::memcmp(&scissor_final, &scissor_recorded_, sizeof(scissor_final)) != 0) {
      BO2ZM_SUB(4, dfn_.vkCmdSetScissor(cmd, 0, 1, &scissor_final));
      scissor_recorded_ = scissor_final;
    }
    if (!recorded ||
        std::memcmp(blend_constant, blend_recorded_, sizeof(blend_constant)) != 0) {
      BO2ZM_SUB(5, dfn_.vkCmdSetBlendConstants(cmd, blend_constant));
      std::memcpy(blend_recorded_, blend_constant, sizeof(blend_constant));
    }

    if (!recorded || std::memcmp(bias, bias_recorded_, sizeof(bias)) != 0) {
      BO2ZM_SUB(6, dfn_.vkCmdSetDepthBias(cmd, bias[0], 0.0f, bias[1]));
      std::memcpy(bias_recorded_, bias, sizeof(bias));
    }
    if (!recorded) {
      stencil_recorded_valid_ = false;
      type_indices_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
    }
    if (keys[4]) {
      const uint32_t frontface = stencil_frontface;
      const uint32_t backface = stencil_backface;
      if (!stencil_recorded_valid_ || frontface != stencil_recorded_[0] ||
          backface != stencil_recorded_[1]) {
        BO2ZM_SUB(7, {
          dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, frontface & 0xFF);
          dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, backface & 0xFF);
          dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (frontface >> 8) & 0xFF);
          dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (backface >> 8) & 0xFF);
          dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (frontface >> 16) & 0xFF);
          dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (backface >> 16) & 0xFF);
        });
        stencil_recorded_[0] = frontface;
        stencil_recorded_[1] = backface;
        stencil_recorded_valid_ = true;
      }
    }
    state_recorded_ = true;


    bool base_cero = false;
    uint32_t first_vertex = 0;

    std::array<VkBuffer, 16> bind_buffers;
    std::array<VkDeviceSize, 16> bind_offsets;
    for (size_t i = 0; i < input->bindings.size(); ++i) {
      ResolveVertexBinding(offsets_vertices[i], upload_, bind_buffers[i], bind_offsets[i]);
    }
    if (vertices_base_cero_ && input->bindings.size() == 1) {
      const VkDeviceSize stride = input->bindings[0].stride;
      if (bind_offsets[0] % stride == 0) {
        base_cero = CheckBaseZero(bind_offsets[0], stride, sources[0].bytes, vmin, first_vertex,
                                  bind_buffers[0] == upload_ ? std::min(kSizeUpload, upload_size_real_)
                                                             : vpools_[0].size);
      } else {
        ++base_zero_misaligned_;
      }
    } else if (input->bindings.size() > 1) {
      ++base_zero_several_bindings_;
    }
    if (!input->bindings.empty()) {


      static constexpr VkDeviceSize kBindingInZero = 0;
      const VkDeviceSize* a_bind = base_cero ? &kBindingInZero : bind_offsets.data();
      const uint32_t n_bindings = uint32_t(input->bindings.size());
      bool equal = n_bindings == bindings_recorded_;
      for (uint32_t i = 0; equal && i < n_bindings; ++i) {
        equal = a_bind[i] == offsets_recorded_[i] && bind_buffers[i] == buffers_vertices_[i];
      }
      if (!equal) {
        BO2ZM_SUB(8, dfn_.vkCmdBindVertexBuffers(cmd, 0, n_bindings, bind_buffers.data(), a_bind));
        ++base_zero_bindings_recorded_;
        bindings_recorded_ = n_bindings;
        for (uint32_t i = 0; i < n_bindings; ++i) {
          offsets_recorded_[i] = a_bind[i];
          buffers_vertices_[i] = bind_buffers[i];
        }
      } else {
        ++bindings_saved_;
      }
      ++base_cero_total_;
      base_zero_draws_ += base_cero ? 1 : 0;
    }

    if (occlusion_open_ && query_occlusion_ == UINT32_MAX) {
      BO2ZM_SUB(13, query_occlusion_ = context_->BeginQueryOcclusion());
    }

    if (occlusion_open_ && pitch >= 640 && warnings_occlusion_draw_ < 16 &&
        (warnings_occlusion_draw_ < 12 ||
         std::chrono::steady_clock::now() - last_warning_occlusion_draw_ >= std::chrono::seconds(2))) {
      ++warnings_occlusion_draw_;
      last_warning_occlusion_draw_ = std::chrono::steady_clock::now();
      REXLOG_INFO("[native] C2 occlusion draw {}: host query {}: VS n{} PS n{} type {} count {} pitch {} masks {:08X} "
                  "depth {:08X} (write {}, test {}, z function {}) viewport {:.1f},{:.1f} {:.1f}x{:.1f} z {:.3f}-{:.3f} "
                  "scissor {},{} {}x{} with indices {}",
                  warnings_occlusion_draw_, query_occlusion_ == UINT32_MAX ? -1 : int64_t(query_occlusion_),
                  p.vs->number, ps ? int(ps->number) : -1, type, count, pitch, masks, control_depth,
                  (control_depth >> 2) & 0x1, (control_depth >> 1) & 0x1, (control_depth >> 4) & 0x7,
                  viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth,
                  scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height, with_indices);


      std::string positions;
      for (const VertexAttribute& a : input->attributes) {
        if (a.location != 0) {
          continue;
        }
        const VertexSource& source = sources[a.binding];
        const uint32_t stride = input->bindings[a.binding].stride;
        for (uint32_t v = 0; v < std::min<uint32_t>(count, 4); ++v) {
          const uint64_t since = uint64_t(v) * stride + a.offset;
          if (since + 16 > source.bytes) {
            break;
          }
          positions += " (";
          for (uint32_t k = 0; k < 4; ++k) {
            uint32_t word;
            std::memcpy(&word, source.data + since + k * 4, 4);
            positions += fmt::format("{}{:.7g}", k ? "," : "", Float(xenos::GpuSwap(word, source.order)));
          }
          positions += ")";
        }
        positions += fmt::format(" format {} stride {}", int(a.format), stride);
        break;
      }
      std::string constants;
      for (uint32_t k = 0; k < 7; ++k) {
        constants += fmt::format(" c{}=(", k);
        for (uint32_t c = 0; c < 4; ++c) {
          constants += fmt::format("{}{:.7g}", c ? "," : "", Float(r[kRegConstantsVs + k * 4 + c]));
        }
        constants += ")";
      }
      REXLOG_INFO("[native] C2 occlusion draw {} (geometry): VTE {:08X} CLIP {:08X} sc {:08X} RB_DEPTH_INFO {:08X} pass "
                  "{}x{} ndc ({:.5g},{:.5g},{:.5g},{:.5g}) pipeline z function {} vmin {}; positions{}; VS constants{}",
                  warnings_occlusion_draw_, vte, r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], mode_sc,
                  r[gr::XE_GPU_REG_RB_DEPTH_INFO], pass_width_, pass_height_, ndc[0], ndc[1], ndc[2], ndc[3],
                  (key.depth >> 4) & 0x7, vmin, positions, constants);
    }

    {
      const uint32_t category = TargetCategory(pitch, keys);
      const uint32_t vertices_draw = with_indices ? uint32_t(indices_16 ? indices16_.size() : indices_.size())
                                                   : count;
      ++draws_per_category_[category];
      triangles_per_category_[category] += vertices_draw / 3;
      if (pixel_probe_mode) {
        context_->PixelProbeDraw(category, vertices_draw, pixel_probe_clipped);
      }
    }


    const uint32_t query_draw =
        !diag_stats_draw_
            ? UINT32_MAX
            : context_->BeginStatsDraw(
                  ps ? ps->number + 1 : 0,
                  TargetCategory(pitch, keys));


    uint32_t query_visibility = UINT32_MAX;
    if (reads_reflection) {
      if (!occlusion_open_ && query_occlusion_ == UINT32_MAX) {
        query_visibility = context_->BeginQueryVisibility(false);
      }
      if (query_visibility == UINT32_MAX) {
        bo2zm::reflection_demand::AnnotateVisible(false);
      }
    } else if (witness_pending_ && ps && ps->shader && ps->shader->hash == kHashComposite &&
               !occlusion_open_ && query_occlusion_ == UINT32_MAX) {
      query_visibility = context_->BeginQueryVisibility(true);
      if (query_visibility != UINT32_MAX) {
        witness_pending_ = false;

        witness_next_ =
            frame_ + (bo2zm::reflection_demand::VisibilityChecked() ? kWitnessEvery : uint64_t(2));
      }
    }
    if (shader_probe_mode)
      context_->ShaderProbeResult(uint32_t(shader_probe_reason),
          with_indices ? uint32_t(indices_16 ? indices16_.size() : indices_.size()) : count,
          shader_probe_replaced,
          shader_probe_mode == 2 && shader_probe_reason == shader_probe::Reason::kEligible && !shader_probe_replaced,
          shader_probe_compile_ns);
    context_->ProfilePassDraw(p.vs->number + 1, ps ? ps->number + 1 : 0,
        with_indices ? uint32_t(indices_16 ? indices16_.size() : indices_.size()) : count);
    deferred::SetDrawBatchSafe(cmd,
        shader_probe_mode == 0 && draw_batch::enabled.load(std::memory_order_relaxed) &&
        topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && !restart &&
        DrawBatchShaderSafe(*p.vs) && (!ps || DrawBatchShaderSafe(*ps)));
    if (with_indices) {

      const VkIndexType type_indices = indices_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

      VkBuffer buffer_indices = upload_;
      VkDeviceSize at_indices = offset_indices;
      ResolveVertexBinding(offset_indices, upload_, buffer_indices, at_indices);
      if (type_indices != type_indices_recorded_ || buffer_indices != buffer_indices_recorded_) {
        BO2ZM_SUB(9, dfn_.vkCmdBindIndexBuffer(cmd, buffer_indices, 0, type_indices));
        type_indices_recorded_ = type_indices;
        buffer_indices_recorded_ = buffer_indices;
      }

      const int32_t offset_vertices =
          base_cero ? int32_t(int64_t(first_vertex) - int64_t(vmin)) : -int32_t(vmin);
      BO2ZM_SUB(10, dfn_.vkCmdDrawIndexed(cmd, uint32_t(indices_16 ? indices16_.size() : indices_.size()),
                                          1, uint32_t(at_indices / (indices_16 ? 2 : 4)),
                                          offset_vertices, 0));
    } else {
      BO2ZM_SUB(10, dfn_.vkCmdDraw(cmd, count, 1, base_cero ? first_vertex : 0, 0));
    }
    if (query_visibility != UINT32_MAX) {
      context_->FinishQueryVisibility(query_visibility);
    }
    if (query_draw != UINT32_MAX) {
      context_->FinishStatsDraw(query_draw);
    }
    Stage(6, mark);
    if (time_) {
      ++sub_samples_;
      ReportSubstages();
    }
    ++drawn_;
    ++draws_in_pass_;
    drawn_timed_ += time_ ? 1 : 0;
    return true;
  }


  void EmitSkyPostponed(VkCommandBuffer cmd, uint32_t reason) {
    if (!sky_pending_ || cmd == VK_NULL_HANDLE) {
      return;
    }
    sky_pending_ = false;
    const PostponedSky& c = sky_;
    ++sky_emitted_[reason < kSkyReasons ? reason : kSkyReasons - 1];
    const uint64_t position = draws_in_pass_ - c.draws_on_postpone;
    sky_position_sum_ += position;
    sky_position_max_ = std::max(sky_position_max_, position);
    dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipeline);
    pipeline_bound_ = c.pipeline;
    if (c.eds_mode) {
      EmitStateDynamic(cmd, c.eds, true, c.eds_mode);
    }
    eds_valid_ = false;
    key_bound_valid_ = false;
    if (!sets_bound_) {
      dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 4,
                                   sets_.data(), 0, nullptr);
      sets_bound_ = true;
    }
    if (!c.uses_ubo) {
      dfn_.vkCmdPushConstants(cmd, layout_pipeline_,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              sizeof(c.push), c.push);
    }
    dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 4, 1,
                                 &sets_ubo_[c.slot_ubo], 3, c.offsets_ubo.data());
    dfn_.vkCmdSetViewport(cmd, 0, 1, &c.viewport);
    dfn_.vkCmdSetScissor(cmd, 0, 1, &c.scissor);
    dfn_.vkCmdSetBlendConstants(cmd, c.blend);
    dfn_.vkCmdSetDepthBias(cmd, c.bias[0], 0.0f, c.bias[1]);
    if (c.with_stencil) {
      dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, c.stencil[0] & 0xFF);
      dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, c.stencil[1] & 0xFF);
      dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (c.stencil[0] >> 8) & 0xFF);
      dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (c.stencil[1] >> 8) & 0xFF);
      dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (c.stencil[0] >> 16) & 0xFF);
      dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (c.stencil[1] >> 16) & 0xFF);
    }
    if (c.n_bindings) {
      std::array<VkBuffer, 16> buffers{};
      std::array<VkDeviceSize, 16> offsets{};
      for (uint32_t i = 0; i < c.n_bindings; ++i) {
        ResolveVertexBinding(c.offsets_vertices[i], c.buffer, buffers[i], offsets[i]);
      }
      dfn_.vkCmdBindVertexBuffers(cmd, 0, c.n_bindings, buffers.data(), offsets.data());
      bindings_recorded_ = 0;
    }
    const uint32_t query = !diag_stats_draw_
                                  ? UINT32_MAX
                                  : context_->BeginStatsDraw(c.ps_more_one, c.category);
    context_->ProfilePassDraw(c.vs_more_one, c.ps_more_one, c.with_indices ? c.indices : c.count);
    if (c.with_indices) {
      const VkIndexType type = c.indices_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      dfn_.vkCmdBindIndexBuffer(cmd, c.index_buffer, 0, type);
      buffer_indices_recorded_ = VK_NULL_HANDLE;
      deferred::SetDrawBatchSafe(cmd, false);
      dfn_.vkCmdDrawIndexed(cmd, c.indices, 1, c.first_index, -int32_t(c.vmin), 0);
    } else {
      deferred::SetDrawBatchSafe(cmd, false);
      dfn_.vkCmdDraw(cmd, c.count, 1, 0, 0);
    }
    if (query != UINT32_MAX) {
      context_->FinishStatsDraw(query);
    }
    ++draws_in_pass_;


    state_recorded_ = false;
    ubo_bound_ = false;
    bindings_recorded_ = 0;
    stencil_recorded_valid_ = false;
    type_indices_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  }


  void ReportSubstages() {
    const auto now = std::chrono::steady_clock::now();
    if (now < sub_next_) {
      return;
    }
    const bool first = sub_next_ == std::chrono::steady_clock::time_point{};
    sub_next_ = now + std::chrono::seconds(10);
    if (first || !sub_samples_) {
      sub_ns_.fill(0);
      sub_n_.fill(0);
      sub_samples_ = 0;
      return;
    }
    static constexpr const char* kNames[51] = {
        "BindPipeline", "sets 0-3", "set 4 (UBO)", "Viewport", "Scissor", "BlendConstants", "DepthBias",
        "Stencil (6)", "BindVertexBuffers", "BindIndexBuffer", "Draw", "PipelineOf", "shared",
        "occlusion", "samplers (texture stage)",

        "index stage: targets and discards", "index stage: depth bias",
        "index stage: indices", "index stage: vertices and diagnostics",

        "(samplers: slot hit)", "(samplers: table hit)", "(samplers: PrepareTexture)",

        "(PrepareTexture: before the check)", "(PrepareTexture: skipped by write stamps)",
        "(PrepareTexture: postponed by the budget)", "(PrepareTexture: sample equal)",
        "(PrepareTexture: full hash equal)", "(PrepareTexture: new texture)", "(PrepareTexture: changed, read again)",
        "(PrepareTexture: to the hash thread)", "(PrepareTexture: took a cold texture's image)",

        "(new texture: hash and look up by content)", "(new texture: create the image)", "(new texture: raw hash)",
        "(new texture: read and untile)", "(new texture: byte order, data hash, queue the upload)",

        "(uploads: textures)", "(uploads: vertices)", "(uploads: indices)", "(uploads: constants)",

        "(vertices: same-frame dedupe)", "(vertices: vertex cache)", "(vertices: copied)", "(constants: VS copy)",
        "(constants: PS copy)",

        "(cache lookup: slot)", "(cache lookup: stamps)", "(cache lookup: guard)",

        "(pipeline stage: viewport, scissor, shared)", "(pipeline stage: key)", "(pipeline stage: lookup and prefetch)"};
    std::string line;
    double total = 0.0;
    for (size_t k = 0; k < 51; ++k) {
      if (!sub_n_[k]) {
        continue;
      }
      const double per_draw = double(sub_ns_[k]) / 1e3 / double(sub_samples_);
      if (k < 19) {
        total += per_draw;
      }
      line += fmt::format(" | {} {:.2f} us/draw ({:.2f} us x {:.2f} per draw)", kNames[k], per_draw,
                           double(sub_ns_[k]) / 1e3 / double(sub_n_[k]),
                           double(sub_n_[k]) / double(sub_samples_));
    }
    BO2ZM_REPORT_RING("[native] C6 substages ({} timed draws; sum {:.2f} us per draw){}; constants copied: {} KB VS "
                      "and {} KB PS in 10 s",
                      sub_samples_, total, line, bytes_constants_vs_ >> 10, bytes_constants_ps_ >> 10);
    bytes_constants_vs_ = bytes_constants_ps_ = 0;
    sub_ns_.fill(0);
    sub_n_.fill(0);
    sub_samples_ = 0;
  }


  bool CheckBaseZero(VkDeviceSize offset, VkDeviceSize stride, uint64_t bytes, uint32_t vmin,
                         uint32_t& first_vertex, VkDeviceSize limit) {
    const VkDeviceSize first = offset / stride;
    if (first > VkDeviceSize(INT32_MAX)) {
      return false;
    }
    const uint64_t n = ++base_zero_checked_;
    if (n <= kBaseZeroACheck || (n & 4095) == 0) {
      const int64_t displacement = int64_t(first) - int64_t(vmin);
      const bool ok = first * stride == offset &&
                        (displacement + int64_t(vmin)) * int64_t(stride) == int64_t(offset) &&
                        displacement >= int64_t(INT32_MIN) && displacement <= int64_t(INT32_MAX) &&
                        offset + bytes <= limit;
      if (!ok) {
        vertices_base_zero_off_ = true;
        vertices_base_cero_ = false;
        REXLOG_ERROR("[native] C6 zero-base vertices: MISMATCH (offset {} stride {} bytes {} vmin {} first {}): this "
                     "draw and the rest of the session bind as always",
                     offset, stride, bytes, vmin, first);
        return false;
      }
      if (n == kBaseZeroACheck) {
        BO2ZM_REPORT_RING("[native] C6 zero-base vertices: {} draws checked, 0 mismatches; still checking 1 in 4096", n);
      }
    }
    first_vertex = uint32_t(first);
    return true;
  }


  void ControlSet4() {
    bool requested = REXCVAR_GET(bo2zm_native_set4_by_differences);
    const int32_t alternate = REXCVAR_GET(bo2zm_native_set4_by_differences_alternate_s);
    if (alternate > 0) {
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                             set4_start_alternation_)
                                .count();
      requested = (seconds / alternate) % 2 == 1;
    }
#if REX_PLATFORM_SWITCH
    NvkSwitchSet4* const nvk = &nvk_switch_set4;
    const bool with_patch = nvk != nullptr && nvk->version == 1;
#else
    const bool with_patch = false;
#endif
    if (requested != set4_requested_ || !set4_requested_annotated_) {
      set4_requested_ = requested;
      set4_requested_annotated_ = true;
      REXLOG_INFO("[native] C6 set 4 by differences: {} (frame {}){}",
                  requested ? "requested from NVK" : "NOT requested: NVK binds the whole set", frame_,
                  with_patch ? "" : "; this NVK does not have nvk_switch_set4 (older Mesa or PC): binds as always");
    }
#if REX_PLATFORM_SWITCH
    if (with_patch) {
      __atomic_store_n(&nvk->requested, requested ? 1 : 0, __ATOMIC_RELAXED);
      if (!set4_mismatch_warned_ && __atomic_load_n(&nvk->off, __ATOMIC_RELAXED) != 0) {
        set4_mismatch_warned_ = true;
        REXLOG_ERROR("[native] C6 set 4 by differences: MISMATCH seen by NVK's guard ({} cbufs bound wrong, each one "
                     "fixed in its draw; details in rex_stderr.log). Off for the rest of the session: NVK binds the "
                     "whole set, as always",
                     __atomic_load_n(&nvk->total.mismatches, __ATOMIC_RELAXED));
      }
    }
#endif
  }


  void ControlDrawNvk() {
#if REX_PLATFORM_SWITCH
    NvkSwitchDraw* const nvk = &nvk_switch_draw;
    if (nvk == nullptr || nvk->version != 1) {
      nvk_prefetch_app_ = false;
      if (!nvk_draw_annotated_) {
        nvk_draw_annotated_ = true;
        REXLOG_INFO("[native] C6 NVK per draw: this NVK does not have the nvk_switch_draw contract (older "
                    "Mesa): no measurement and no improvements");
      }
      return;
    }
    int32_t measure = std::max<int32_t>(0, REXCVAR_GET(bo2zm_native_nvk_measure));
    if (measure > 0) {
      measure = int32_t(std::bit_floor(uint32_t(measure)));
    }
    bool improvements = true;
    const int32_t alternate = REXCVAR_GET(bo2zm_native_nvk_alternate_s);
    if (alternate > 0) {
      const auto seconds =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - nvk_draw_start_)
              .count();
      improvements = (seconds / alternate) % 2 == 1;
    }
    const int32_t requested[5] = {improvements && REXCVAR_GET(bo2zm_native_nvk_emission) ? 1 : 0,
                                improvements && REXCVAR_GET(bo2zm_native_nvk_cbufs) ? 1 : 0,
                                improvements && REXCVAR_GET(bo2zm_native_nvk_dynamic) ? 1 : 0,
                                0,
                                improvements && REXCVAR_GET(bo2zm_native_nvk_prefetch) ? 1 : 0};
    __atomic_store_n(&nvk->measure, measure, __ATOMIC_RELAXED);
    __atomic_store_n(&nvk->measure_misses, REXCVAR_GET(bo2zm_native_nvk_measure_misses) ? 1 : 0, __ATOMIC_RELAXED);
    for (size_t m = 0; m < 5; ++m) {
      __atomic_store_n(&nvk->improvements[m].requested, requested[m], __ATOMIC_RELAXED);
    }
    nvk_prefetch_app_ = requested[4] != 0;
    if (!nvk_draw_annotated_ || improvements != nvk_improvements_requested_) {
      nvk_draw_annotated_ = true;
      nvk_improvements_requested_ = improvements;
      REXLOG_INFO("[native] C6 NVK per draw: improvements {} (emission {}, cbufs {}, dynamic {}, prefetch "
                  "{}); measuring 1 in {} calls{}; environment NVK_SWITCH_DRAW {} (frame {})",
                  improvements ? "requested" : "NOT requested", requested[0], requested[1], requested[2], requested[4], measure,
                  REXCVAR_GET(bo2zm_native_nvk_measure_misses) ? " with the cache misses separately" : "",
                  __atomic_load_n(&nvk->env, __ATOMIC_RELAXED), frame_);
    }
    static constexpr const char* kNames[5] = {"emission", "cbufs", "dynamic", "fast set 4", "prefetch"};
    for (size_t m = 0; m < 5; ++m) {
      if (!nvk_off_warned_[m] && __atomic_load_n(&nvk->improvements[m].off, __ATOMIC_RELAXED) != 0) {
        nvk_off_warned_[m] = true;
        REXLOG_ERROR("[native] C6 NVK per draw: MISMATCH seen by the guard of '{}' ({} checks that differed; details in "
                     "rex_stderr.log). Off for the rest of the session: NVK does it as always",
                     kNames[m], __atomic_load_n(&nvk->improvements[m].mismatches, __ATOMIC_RELAXED));
      }
    }
#endif
  }


  void ReportDrawNvk() {
#if REX_PLATFORM_SWITCH
    const auto now = std::chrono::steady_clock::now();
    if (now - nvk_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = nvk_report_ == std::chrono::steady_clock::time_point{};
    nvk_report_ = now;
    NvkSwitchDraw* const nvk = &nvk_switch_draw;
    if (nvk == nullptr || nvk->version != 1) {
      return;
    }
    uint64_t times[16], ticks[16], counts[13], improvements[5][4];
    for (size_t i = 0; i < 16; ++i) {
      const uint64_t v = __atomic_load_n(&nvk->parts[i].times, __ATOMIC_RELAXED);
      const uint64_t t = __atomic_load_n(&nvk->parts[i].ticks, __ATOMIC_RELAXED);
      times[i] = v - nvk_partes_previous_[i][0];
      ticks[i] = t - nvk_partes_previous_[i][1];
      nvk_partes_previous_[i][0] = v;
      nvk_partes_previous_[i][1] = t;
    }
    for (size_t i = 0; i < 13; ++i) {
      const uint64_t c = __atomic_load_n(&nvk->counts[i], __ATOMIC_RELAXED);
      counts[i] = c - nvk_counts_previous_[i];
      nvk_counts_previous_[i] = c;
    }
    for (size_t m = 0; m < 5; ++m) {
      const uint64_t c[4] = {__atomic_load_n(&nvk->improvements[m].uses, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].validated, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].mismatches, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].no_check, __ATOMIC_RELAXED)};
      for (size_t k = 0; k < 4; ++k) {
        improvements[m][k] = c[k] - nvk_improvements_previous_[m][k];
        nvk_improvements_previous_[m][k] = c[k];
      }
    }
    if (first) {
      return;
    }
    const uint64_t tps = __atomic_load_n(&nvk->ticks_per_second, __ATOMIC_RELAXED);
    const double us_tick = tps ? 1e6 / double(tps) : 0.0;
    const uint64_t draws = counts[0];

    const auto part = [&](size_t i) {
      return fmt::format("{:.2f} [{:.2f}]", times[i] ? double(ticks[i]) * us_tick / double(times[i]) : 0.0,
                         draws ? double(ticks[i]) * us_tick / double(draws) : 0.0);
    };
    const auto per_draw = [&](size_t i) { return draws ? double(counts[i]) / double(draws) : 0.0; };
    if (draws) {
      BO2ZM_REPORT_RING(
          "[native] C6 NVK by parts (us per measured call [us per measured draw]; {} draws measured): whole "
          "draw {} | push desc {} | dynamic {} | shader touch {} | shaders {} | cbufs {} | emit {} | BindPipeline: "
          "shaders {} touch {} state copy {} | sets {} (root {} dirtying {}) | root table {} | new chunk {} | "
          "BindVertexBuffers {}",
          draws, part(0), part(1), part(2), part(3), part(4), part(5), part(6), part(7), part(8),
          part(9), part(10), part(11), part(12), part(13), part(14), part(15));
      BO2ZM_REPORT_RING(
          "[native] C6 NVK per measured draw: {:.1f} dwords; {:.2f} with dirty dynamic state ({:.1f} bits); {:.2f} with "
          "dirty shaders; {:.2f} cbufs rebound | pipelines: {} binds measured, {:.1f} % copies that changed nothing, "
          "{:.2f} new bits per bind | {} root table uploads ({:.1f} words each), {} new chunks",
          per_draw(1), per_draw(2), counts[2] ? double(counts[3]) / double(counts[2]) : 0.0, per_draw(4),
          per_draw(5), counts[6], counts[6] ? 100.0 * double(counts[7]) / double(counts[6]) : 0.0,
          counts[6] ? double(counts[8]) / double(counts[6]) : 0.0, counts[10],
          counts[10] ? double(counts[11]) / double(counts[10]) : 0.0, counts[12]);
    }
    static constexpr size_t kShown[4] = {0, 1, 2, 4};
    static constexpr const char* kNames[5] = {"emission", "cbufs", "dynamic", "fast set 4", "prefetch"};
    std::string text;
    for (const size_t m : kShown) {
      const char* state = __atomic_load_n(&nvk->improvements[m].off, __ATOMIC_RELAXED) != 0 ? "OFF by the guard"
                           : __atomic_load_n(&nvk->improvements[m].requested, __ATOMIC_RELAXED) == 0 ? "not requested"
                                                                                              : "on";
      text += fmt::format(" | {} ({}): {} uses, {} checked equal, {} different, {} not checked", kNames[m],
                           state, improvements[m][0], improvements[m][1], improvements[m][2], improvements[m][3]);
    }
    BO2ZM_REPORT_RING("[native] C6 NVK improvements (last 10 s){}", text);
#endif
  }


  void ControlCopiesNvk() {
#if REX_PLATFORM_SWITCH
    VkBo2StateCopy* const state = &vk_bo2_copy_state;
    NvkBo2Copies* const copies = &nvk_bo2_copies;
    const bool with_state = state != nullptr && state->version == 1;
    const bool with_copies = copies != nullptr && copies->version == 1;
    const bool hashes = REXCVAR_GET(bo2zm_native_nvk_fingerprint_copy);
    int32_t engine = std::clamp(REXCVAR_GET(bo2zm_native_nvk_copy_engine), 0, 3);
    const int32_t engine_b = REXCVAR_GET(bo2zm_native_nvk_copy_engine_b);
    if (engine_b >= 0) {
      const int arm = bo2zm::benchmark::AbArm();
      if (arm >= 0 && (arm & 2)) {
        engine = engine_b;
      }
    }
    if (with_state) {
      __atomic_store_n(&state->requested, hashes ? 1 : 0, __ATOMIC_RELAXED);
    }
    if (with_copies) {
      __atomic_store_n(&copies->engine, engine, __ATOMIC_RELAXED);
    }
    if (!copies_nvk_annotated_ || hashes != copies_nvk_hashes_ || engine != copies_nvk_engine_) {
      copies_nvk_annotated_ = true;
      copies_nvk_hashes_ = hashes;
      copies_nvk_engine_ = engine;
      REXLOG_INFO("[native] C6 NVK changes: state copy by fingerprints {}{}; copy engine: {}{} (frame {})",
                  hashes ? "requested" : "NOT requested",
                  with_state ? "" : " (this Mesa does not have it)",
                  engine == 0   ? "nothing (all through vk_meta)"
                  : engine == 1 ? "texture uploads"
                  : engine == 2 ? "image-to-image copies"
                               : "uploads and image-to-image copies",
                  with_copies ? "" : " (this NVK does not have it)", frame_);
    }
    if (with_state && !copies_nvk_mismatch_warned_ && __atomic_load_n(&state->off, __ATOMIC_RELAXED) != 0) {
      copies_nvk_mismatch_warned_ = true;
      REXLOG_ERROR("[native] C6 NVK state copy by fingerprints: MISMATCH seen by Mesa's guard ({} binds different from "
                   "the usual copy, which is the one kept; details in rex_stderr.log). Off for the rest of the session",
                   __atomic_load_n(&state->mismatches, __ATOMIC_RELAXED));
    }
#endif
  }


  void ControlKindViewsNvk() {
#if REX_PLATFORM_SWITCH
    NvkBo2KindViews* const views = &nvk_bo2_kind_views;
    const bool with_views = views != nullptr && views->version == 1;
    bool on = REXCVAR_GET(bo2zm_native_nvk_kind_views);
    if (REXCVAR_GET(bo2zm_native_nvk_kind_views_ab)) {
      const int arm = bo2zm::benchmark::AbArm();
      if (arm >= 0) {
        on = (arm & 1) != 0;
      }
    }
    if (with_views) {
      __atomic_store_n(&views->enabled, on ? 1 : 0, __ATOMIC_RELAXED);
    }
    if (!kind_views_annotated_ || on != kind_views_on_) {
      kind_views_annotated_ = true;
      kind_views_on_ = on;
      REXLOG_INFO("[native] C3 NVK kind views (bo2zm_native_nvk_kind_views): {}{} (frame {})",
                  on ? "on, pool textures share one mapping per block" : "off, a mapping per texture",
                  with_views ? "" : " (this NVK does not have them)", frame_);
    }
    const auto now = std::chrono::steady_clock::now();
    if (!with_views || now - kind_views_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = kind_views_report_ == std::chrono::steady_clock::time_point{};
    kind_views_report_ = now;
    uint64_t d[6] = {};
    const uint64_t* const v = &views->views_created;
    for (size_t i = 0; i < 6; ++i) {
      const uint64_t x = __atomic_load_n(v + i, __ATOMIC_RELAXED);
      d[i] = x - kind_views_previous_[i];
      kind_views_previous_[i] = x;
    }
    if (first) {
      return;
    }
    BO2ZM_REPORT_RING(
        "[native] C3 NVK kind views ({}), last 10 s: {} texture planes bound through a shared mapping, {} with a "
        "mapping of their own, {} without a view (failures); {} shared mappings made and {} freed, {} live ({:.0f} MB "
        "of GPU addresses)",
        on ? "on" : "off", d[3], d[4], d[5], d[0], d[1], kind_views_previous_[0] - kind_views_previous_[1],
        double(kind_views_previous_[2]) / (1024.0 * 1024.0));
#endif
  }


  void ReportCopiesNvk() {
#if REX_PLATFORM_SWITCH
    const auto now = std::chrono::steady_clock::now();
    if (now - copies_nvk_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = copies_nvk_report_ == std::chrono::steady_clock::time_point{};
    copies_nvk_report_ = now;
    VkBo2StateCopy* const state = &vk_bo2_copy_state;
    NvkBo2Copies* const copies = &nvk_bo2_copies;
    uint64_t e[7] = {};
    uint64_t c[6] = {};
    if (state != nullptr && state->version == 1) {
      const uint64_t* const v = &state->copies;
      for (size_t i = 0; i < 7; ++i) {
        const uint64_t x = __atomic_load_n(v + i, __ATOMIC_RELAXED);
        e[i] = x - copies_nvk_state_previous_[i];
        copies_nvk_state_previous_[i] = x;
      }
    }
    if (copies != nullptr && copies->version == 1) {
      const uint64_t* const v = &copies->uploads_engine;
      for (size_t i = 0; i < 6; ++i) {
        const uint64_t x = __atomic_load_n(v + i, __ATOMIC_RELAXED);
        c[i] = x - copies_nvk_copies_previous_[i];
        copies_nvk_copies_previous_[i] = x;
      }
    }
    if (first) {
      return;
    }
    const uint64_t bits = e[2] + e[3];
    BO2ZM_REPORT_RING(
        "[native] C6 NVK changes, last 10 s | state copy by fingerprints ({}): {} binds, {} of pipelines without "
        "fingerprints; {:.2f} bits copied and {:.2f} skipped per bind ({:.1f} % skipped); guard {} checked, {} "
        "different; {} pipelines with fingerprints | copy engine: {} uploads ({} regions), through vk_meta {} uploads "
        "({} regions); image copies {} on the engine, {} through vk_meta",
        state == nullptr || state->version != 1         ? "not in this Mesa"
        : __atomic_load_n(&state->off, __ATOMIC_RELAXED) ? "OFF by the guard"
        : __atomic_load_n(&state->requested, __ATOMIC_RELAXED)  ? "on"
                                                              : "not requested",
        e[0], e[1], e[0] ? double(e[2]) / double(e[0]) : 0.0, e[0] ? double(e[3]) / double(e[0]) : 0.0,
        bits ? 100.0 * double(e[3]) / double(bits) : 0.0, e[4], e[5], e[6], c[0], c[1], c[2], c[3], c[4], c[5]);
#endif
  }


  void ReportSet4() {
    const auto now = std::chrono::steady_clock::now();
    if (now - set4_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = set4_report_ == std::chrono::steady_clock::time_point{};
    set4_report_ = now;
    uint64_t bindings = set4_first_;
    for (const uint64_t n : set4_changes_) {
      bindings += n;
    }
    if (!first && set4_draws_ && bindings) {
      const auto pct = [bindings](uint64_t n) { return 100.0 * double(n) / double(bindings); };
      BO2ZM_REPORT_RING(
          "[native] C6 set 4: {:.2f} binds per draw ({} in {} draws); only VS changes {:.1f} %, only PS {:.1f} %, only "
          "shared {:.1f} %, VS+PS {:.1f} %, VS+shared {:.1f} %, PS+shared {:.1f} %, all three {:.1f} %, after buffer, "
          "slot or sky {:.1f} % | constants uploaded again: VS {} by generation, {} by epoch and {} because the shader "
          "reads more; PS {}, {} and {}",
          double(bindings) / double(set4_draws_), bindings, set4_draws_, pct(set4_changes_[1]),
          pct(set4_changes_[2]), pct(set4_changes_[4]), pct(set4_changes_[3]), pct(set4_changes_[5]),
          pct(set4_changes_[6]), pct(set4_changes_[7]), pct(set4_first_), reuploads_vs_[0], reuploads_vs_[1],
          reuploads_vs_[2], reuploads_ps_[0], reuploads_ps_[1], reuploads_ps_[2]);
#if REX_PLATFORM_SWITCH
      if (NvkSwitchSet4* const nvk = &nvk_switch_set4; nvk != nullptr && nvk->version == 1) {
        const uint64_t counts[9] = {__atomic_load_n(&nvk->total.bindings_mismatch, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.bindings_full, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.writes_root, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.dwords_root, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.cbufs_dirty, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.cbufs_saved, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.draws, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.checks, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.mismatches, __ATOMIC_RELAXED)};
        uint64_t d[9];
        for (size_t i = 0; i < 9; ++i) {
          d[i] = counts[i] - set4_nvk_previous_[i];
          set4_nvk_previous_[i] = counts[i];
        }
        const char* const state = __atomic_load_n(&nvk->off, __ATOMIC_RELAXED) != 0 ? "OFF by the guard"
                                   : __atomic_load_n(&nvk->env, __ATOMIC_RELAXED) == 0
                                       ? "off by NVK_SWITCH_DYN_UBO_DELTA"
                                   : !set4_requested_ ? "off by the cvar"
                                                   : "on";
        BO2ZM_REPORT_RING(
            "[native] C6 set 4 by differences (NVK, {}): {} binds by differences and {} whole; {:.2f} root table writes "
            "per bind (4 before) with {:.2f} words; cbufs dirtied {} and not dirtied {} ({:.1f} % fewer); guard: {} "
            "cbufs checked ({} draws on the new path), {} mismatches",
            state, d[0], d[1], d[0] ? double(d[2]) / double(d[0]) : 0.0, d[0] ? double(d[3]) / double(d[0]) : 0.0,
            d[4], d[5], d[4] + d[5] ? 100.0 * double(d[5]) / double(d[4] + d[5]) : 0.0, d[7], d[6], d[8]);
      }
#endif
    }
    set4_changes_.fill(0);
    set4_first_ = 0;
    set4_draws_ = 0;
    reuploads_vs_.fill(0);
    reuploads_ps_.fill(0);
  }


  void ReportBaseZero() {
    const auto now = std::chrono::steady_clock::now();
    if (now - base_zero_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = base_zero_report_ == std::chrono::steady_clock::time_point{};
    base_zero_report_ = now;
    if (!first && base_cero_total_) {
      BO2ZM_REPORT_RING("[native] C6 zero-base vertices ({}): {} of {} draws with vertices without binding their own "
                  "offset ({:.1f} %); {} with several bindings and {} with the dedupe copy off a multiple of the "
                  "stride; {:.2f} vkCmdBindVertexBuffers per draw",
                  vertices_base_zero_off_ ? "OFF by the guard"
                  : vertices_base_cero_       ? "on"
                                              : "off",
                  base_zero_draws_, base_cero_total_, 100.0 * double(base_zero_draws_) / double(base_cero_total_),
                  base_zero_several_bindings_, base_zero_misaligned_,
                  double(base_zero_bindings_recorded_) / double(base_cero_total_));
    }
    base_zero_draws_ = 0;
    base_cero_total_ = 0;
    base_zero_several_bindings_ = 0;
    base_zero_misaligned_ = 0;
    base_zero_bindings_recorded_ = 0;
  }


  uint32_t ComputeFraming(const uint32_t* r, uint32_t vte, bool no_clip, uint32_t mode_sc, VkViewport& viewport,
                            float ndc[4], VkRect2D& scissor) {
    float scale_x = (vte & 0x1) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]) : 1.0f;
    float center_x = (vte & 0x2) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]) : 0.0f;
    float scale_y = (vte & 0x4) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]) : 1.0f;
    float center_y = (vte & 0x8) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]) : 0.0f;
    const float scale_z = (vte & 0x10) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]) : 1.0f;
    const float center_z = (vte & 0x20) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]) : 0.0f;
    const uint32_t window = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
    const int32_t window_x = int32_t((window & 0x7FFF) << 17) >> 17;
    const int32_t window_y = int32_t(((window >> 16) & 0x7FFF) << 17) >> 17;
    if ((mode_sc >> 16) & 0x1) {
      center_x += float(window_x);
      center_y += float(window_y);
    }


    if (pass_scale_x_ != 1.0f) {
      scale_x *= pass_scale_x_;
      center_x *= pass_scale_x_;
    }
    if (pass_scale_y_ != 1.0f) {
      scale_y *= pass_scale_y_;
      center_y *= pass_scale_y_;
    }
    viewport = VkViewport{};
    viewport.x = center_x - std::abs(scale_x);
    viewport.width = 2.0f * std::abs(scale_x);

    viewport.y = center_y + scale_y;
    viewport.height = -2.0f * scale_y;
    if (!((r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 19) & 0x1)) {
      Warn(14, "OpenGL clip space (dx_clip_space_def = 0): Z not adjusted");
    }
    viewport.minDepth = std::clamp(center_z, 0.0f, 1.0f);
    viewport.maxDepth = std::clamp(center_z + scale_z, 0.0f, 1.0f);

    ndc[0] = 1.0f;
    ndc[1] = 1.0f;
    ndc[2] = 0.0f;
    ndc[3] = 0.0f;
    if (no_clip) {
      const float width = float(pass_width_);
      const float height = float(pass_height_);
      ndc[0] = scale_x * 2.0f / width;
      ndc[2] = (center_x - width * 0.5f) * 2.0f / width;

      ndc[1] = -scale_y * 2.0f / height;
      ndc[3] = -(center_y - height * 0.5f) * 2.0f / height;
      viewport.x = 0.0f;
      viewport.y = 0.0f;
      viewport.width = width;
      viewport.height = height;
    }
    scissor = VkRect2D{};
    if (viewport.width < 1.0f || std::abs(viewport.height) < 1.0f) {
      return 1;
    }
    const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
    const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
    int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
    int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
    if (!((tl >> 31) & 0x1)) {
      x0 += window_x;
      y0 += window_y;
      x1 += window_x;
      y1 += window_y;
    }

    if (pass_scale_x_ != 1.0f) {
      x0 = int32_t(std::floor(float(x0) * pass_scale_x_));
      x1 = int32_t(std::ceil(float(x1) * pass_scale_x_));
    }
    if (pass_scale_y_ != 1.0f) {
      y0 = int32_t(std::floor(float(y0) * pass_scale_y_));
      y1 = int32_t(std::ceil(float(y1) * pass_scale_y_));
    }
    x0 = std::clamp(x0, 0, int32_t(pass_width_));
    x1 = std::clamp(x1, 0, int32_t(pass_width_));
    y0 = std::clamp(y0, 0, int32_t(pass_height_));
    y1 = std::clamp(y1, 0, int32_t(pass_height_));
    if (x1 <= x0 || y1 <= y0) {
      return 2;
    }
    scissor = VkRect2D{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    return 0;
  }


  void CompareFraming(const VkViewport& viewport, const float ndc[4], const VkRect2D& scissor, uint32_t empty) {
    ++framing_checked_;
    const bool equal = empty == framing_empty_ &&
                       std::memcmp(&viewport, &framing_viewport_, sizeof(viewport)) == 0 &&
                       std::memcmp(ndc, framing_ndc_, sizeof(framing_ndc_)) == 0 &&
                       std::memcmp(&scissor, &framing_scissor_, sizeof(scissor)) == 0;
    if (!equal) {
      framing_cache_off_ = true;
      framing_cache_ = false;
      REXLOG_ERROR("[native] C6 cached framing: MISMATCH in check {} (empty {} / {}; viewport {},{} {}x{} / {},{} "
                   "{}x{}; scissor {},{} {}x{} / {},{} {}x{}). Off for the rest of the session: this draw uses the "
                   "recomputed one",
                   framing_checked_, empty, framing_empty_, viewport.x, viewport.y, viewport.width,
                   viewport.height, framing_viewport_.x, framing_viewport_.y, framing_viewport_.width,
                   framing_viewport_.height, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                   scissor.extent.height, framing_scissor_.offset.x, framing_scissor_.offset.y,
                   framing_scissor_.extent.width, framing_scissor_.extent.height);
    } else if (framing_checked_ == kFramingsACheck) {
      BO2ZM_REPORT_RING("[native] C6 cached framing: {} hits checked against the computation, 0 mismatches; still "
                  "checking 1 in 4096", framing_checked_);
    }
  }


  uint32_t& HeightUsefulOf(uint32_t pitch) {
    if (!height_useful_memo_active_) {
      return height_useful_[pitch];
    }
    if (height_useful_memo_ == nullptr || pitch != height_useful_memo_pitch_) {
      height_useful_memo_ = &height_useful_[pitch];
      height_useful_memo_pitch_ = pitch;
      return *height_useful_memo_;
    }
    const uint64_t n = ++height_useful_memo_hits_;
    if (n <= kHeightUsefulACheck || (n & 4095) == 0) {
      uint32_t* const of_map = &height_useful_[pitch];
      if (of_map != height_useful_memo_) {
        height_useful_memo_off_ = true;
        height_useful_memo_active_ = false;
        height_useful_memo_ = nullptr;
        REXLOG_ERROR("[native] C6 remembered useful height: MISMATCH (pitch {}, check {}): it is not the map element. "
                     "Off for the rest of the session", pitch, n);
        return *of_map;
      }
      if (n == kHeightUsefulACheck) {
        BO2ZM_REPORT_RING("[native] C6 remembered useful height: {} checks against the map, 0 mismatches; still "
                    "checking 1 in 4096", n);
      }
    }
    return *height_useful_memo_;
  }


  uint64_t CheckKeyPass(const uint64_t keys[5], uint64_t n) {
    const uint64_t computed = XXH3_64bits(keys, sizeof(uint64_t) * 5);
    if (computed != pass_key_) {
      key_pass_fast_off_ = true;
      key_pass_fast_ = false;
      REXLOG_ERROR("[native] C6 pass key: MISMATCH (check {}: {:016X} computed, {:016X} stored). Off for the rest of "
                   "the session", n, computed, pass_key_);
    } else if (n == kKeysPassACheck) {
      BO2ZM_REPORT_RING("[native] C6 pass key: {} reused ones checked with XXH3, 0 mismatches; still checking 1 in 4096", n);
    }
    return computed;
  }


  void ReportMinor() {
    const auto now = std::chrono::steady_clock::now();
    if (now - minor_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = minor_report_ == std::chrono::steady_clock::time_point{};
    minor_report_ = now;
    const uint64_t framings = framing_hits_report_ + framing_calcs_report_;
    if (!first && framings) {
      BO2ZM_REPORT_RING("[native] C6 minor: cached framing in {} of {} draws ({:.1f} %; {}{}); pass key "
                  "without XXH3 {} times ({}); remembered useful height {} times ({}); colorless draw settings {}",
                  framing_hits_report_, framings,
                  100.0 * double(framing_hits_report_) / double(framings),
                  framing_cache_off_ ? "OFF by the guard" : framing_cache_ ? "on" : "off",
                  framing_checked_ >= kFramingsACheck ? ", guard passed" : ", checking",
                  keys_pass_fast_ - keys_pass_fast_previous_,
                  key_pass_fast_off_ ? "OFF by the guard" : key_pass_fast_ ? "on" : "off",
                  height_useful_memo_hits_ - height_useful_memo_hits_previous_,
                  height_useful_memo_off_ ? "OFF by the guard" : height_useful_memo_active_ ? "on" : "off",
                  cvars_per_frame_ ? "once per frame" : "on every draw");
    }
    framing_hits_report_ = 0;
    framing_calcs_report_ = 0;
    keys_pass_fast_previous_ = keys_pass_fast_;
    height_useful_memo_hits_previous_ = height_useful_memo_hits_;
  }


  bool CheckCellPipeline(uint64_t hash, const PipelineKey& key, VkPipeline in_cell, uint64_t n) {
    ++pipelines_direct_checked_;
    VkPipeline of_map = VK_NULL_HANDLE;
    if (const auto it = pipelines_.find(hash);
        it != pipelines_.end() && std::memcmp(&it->second.first, &key, sizeof(key)) == 0) {
      of_map = it->second.second;
    }
    if (of_map != in_cell) {
      pipelines_direct_off_ = true;
      pipelines_direct_ = false;
      REXLOG_ERROR("[native] C6 pipelines: MISMATCH between the direct cache and the map (check {}, VS {} PS {} "
                   "specialization {:08X}). Off for the rest of the session: the map decides",
                   n, key.vs, key.ps, key.specialization);
      return false;
    }
    if (n == kPipelinesACheck) {
      BO2ZM_REPORT_RING("[native] C6 pipelines: {} direct-cache hits checked against the map, 0 mismatches; still "
                  "checking 1 in 4096", n);
    }
    return true;
  }


  static uint32_t ClassTopology(uint32_t topology) {
    switch (topology) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
        return 0;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
        return 1;
      case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return 3;
      default:
        return 2;
    }
  }


  static void StateCanonical(const PipelineKey& c, uint32_t blend[4], uint32_t& masks, uint32_t& depth) {
    masks = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      blend[i] = 0;
      if (!c.formats[i]) {
        continue;
      }
      const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
      masks |= mask << (i * 4);
      const uint32_t m = c.blend[i];
      if (ColorBlendEnabled(mask, m)) {
        blend[i] = m;
      }
    }
    uint32_t d = c.formats[4] ? c.depth : 0;
    d &= ~0x8u;
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;
    }
    if (!(d & 0x1)) {
      d &= 0x77u;
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;
    }
    depth = d;
  }


  void CountChangePipeline(const PipelineKey& fresh, bool after_begin_pass) {
    uint64_t* const n = changes_pipeline_.data();
    ++n[kChangeBindings];
    n[kChangeAfterPass] += after_begin_pass ? 1 : 0;
    const PipelineKey& a = key_bound_;
    constexpr uint32_t kBitsTestAlpha = 0x2u | (0x7u << kSpecFunctionAlphaOffset);
    if (!key_bound_valid_) {
      ++n[kChangeFirst];
    } else if (std::memcmp(&a, &fresh, sizeof(fresh)) == 0) {
      ++n[kChangeIdentical];
    } else if (a.vs != fresh.vs || a.ps != fresh.ps) {
      ++n[kChangeShaders];
    } else if (a.input != fresh.input) {
      ++n[kChangeInput];
    } else if (std::memcmp(a.formats, fresh.formats, sizeof(a.formats)) != 0) {
      ++n[kChangeFormats];
    } else if (const uint32_t spec = a.specialization ^ fresh.specialization; spec != 0) {
      ++n[(spec & ~kBitsTestAlpha) == 0                      ? kChangeSpecAlpha
          : (spec & ~(kBitsTestAlpha | kSpecZEarly)) == 0 ? kChangeSpecZEarly
                                                              : kChangeSpecOther];
    } else {

      uint32_t blend_a[4], blend_n[4], masks_a, masks_n, dpth_a, dpth_n;
      StateCanonical(a, blend_a, masks_a, dpth_a);
      StateCanonical(fresh, blend_n, masks_n, dpth_n);
      const bool topology = a.topology != fresh.topology;
      const bool class_changed = ClassTopology(a.topology) != ClassTopology(fresh.topology);
      const bool blend = std::memcmp(blend_a, blend_n, sizeof(blend_a)) != 0;
      const bool masks = masks_a != masks_n;
      const uint32_t dpth = dpth_a ^ dpth_n;
      const uint32_t rast = a.rasterization ^ fresh.rasterization;
      if (!topology && !blend && !masks && !dpth && !rast) {
        ++n[kChangeNoEffect];
        n[kChangeNoEffectAfterPass] += after_begin_pass ? 1 : 0;
      } else {
        const bool eds12 = !blend && !masks && !class_changed;
        ++n[kChangeOnlyState];
        n[kChangeStateAfterPass] += after_begin_pass ? 1 : 0;
        n[kChangeEds12] += eds12 ? 1 : 0;
        n[kChangeEds12AfterPass] += (eds12 && after_begin_pass) ? 1 : 0;
        n[kFieldTopology] += topology ? 1 : 0;
        n[kFieldClassTopology] += class_changed ? 1 : 0;
        n[kFieldBlend] += blend ? 1 : 0;
        n[kFieldMasks] += masks ? 1 : 0;
        n[kFieldZ] += (dpth & 0x76u) ? 1 : 0;
        n[kFieldStencil] += (dpth & ~0x76u) ? 1 : 0;
        n[kFieldFace] += (rast & 0x7u) ? 1 : 0;
        n[kFieldRestart] += (rast & 0x8u) ? 1 : 0;
        n[kFieldBias] += (rast & 0x10u) ? 1 : 0;
      }
    }
    key_bound_ = fresh;
    key_bound_valid_ = true;
  }


  void ReportChangesPipeline(std::chrono::steady_clock::time_point now) {
    const bool count = REXCVAR_GET(bo2zm_native_count_pipeline_changes);
    if (count != count_changes_pipeline_) {
      count_changes_pipeline_ = count;
      key_bound_valid_ = false;
    }
    if (now - changes_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = changes_report_ == std::chrono::steady_clock::time_point{};
    const double seconds = std::chrono::duration<double>(now - changes_report_).count();
    changes_report_ = now;
    const uint64_t draws = drawn_ - changes_draws_previous_;
    const uint64_t frames = frame_ - changes_frames_previous_;
    changes_draws_previous_ = drawn_;
    changes_frames_previous_ = frame_;
    const std::array<uint64_t, kChangesN> n = changes_pipeline_;
    changes_pipeline_.fill(0);
    if (first || !count_changes_pipeline_ || !n[kChangeBindings] || !draws || !frames) {
      return;
    }
    const double bindings = double(n[kChangeBindings]);
    const double f = double(frames);
    const auto per_draw = [&](uint64_t x) { return double(x) / double(draws); };
    const auto pct = [&](uint64_t x) { return 100.0 * double(x) / bindings; };
    const uint64_t spec = n[kChangeSpecAlpha] + n[kChangeSpecZEarly] + n[kChangeSpecOther];
    BO2ZM_REPORT_RING(
        "[native] C6 pipeline changes (measurement only): {} vkCmdBindPipeline in {:.0f} s ({:.3f} per draw, "
        "{:.1f} per frame; {} are the first of a pass) | without a previous key {} | the SAME key after a pass begins "
        "{} ({:.1f} %) | shaders change {} ({:.1f} %) | another input {} | other formats {} | specialization {} ({:.1f} "
        "%: alpha test {}, with early Z {}, other {}) | no effect (bits PipelineOf does not read) {} ({:.1f} %) | STATE "
        "ONLY {} ({:.1f} %, {:.3f} per draw): EDS1/EDS2 without touching the SDK {}, the rest needs EDS3",
        n[kChangeBindings], seconds, per_draw(n[kChangeBindings]), bindings / f, n[kChangeAfterPass],
        n[kChangeFirst], n[kChangeIdentical], pct(n[kChangeIdentical]), n[kChangeShaders], pct(n[kChangeShaders]),
        n[kChangeInput], n[kChangeFormats], spec, pct(spec), n[kChangeSpecAlpha], n[kChangeSpecZEarly],
        n[kChangeSpecOther], n[kChangeNoEffect], pct(n[kChangeNoEffect]), n[kChangeOnlyState],
        pct(n[kChangeOnlyState]), per_draw(n[kChangeOnlyState]), n[kChangeEds12]);


    constexpr double kUsPerBinding = 3.0;
    const uint64_t canonical = n[kChangeNoEffect] - n[kChangeNoEffectAfterPass];
    const uint64_t eds12 = n[kChangeEds12] - n[kChangeEds12AfterPass];
    const uint64_t eds123 = n[kChangeOnlyState] - n[kChangeStateAfterPass];
    BO2ZM_REPORT_RING(
        "[native] C6 pipeline changes, state-only fields: topology {} (of class {}), blend {}, masks {}, Z {}, stencil "
        "{}, cull {}, restart {}, bias {}; {} of them the first of a pass | avoidable binds per frame (at ~3 us each): "
        "{:.1f} with the canonical key ({:.2f} ms), {:.1f} by not forgetting the pipeline when a pass begins ({:.2f} "
        "ms), {:.1f} with EDS1/EDS2 ({:.2f} ms), {:.1f} with EDS1/EDS2/EDS3 ({:.2f} ms)",
        n[kFieldTopology], n[kFieldClassTopology], n[kFieldBlend], n[kFieldMasks], n[kFieldZ],
        n[kFieldStencil], n[kFieldFace], n[kFieldRestart], n[kFieldBias], n[kChangeStateAfterPass],
        double(canonical) / f, double(canonical) / f * kUsPerBinding / 1000.0, double(n[kChangeIdentical]) / f,
        double(n[kChangeIdentical]) / f * kUsPerBinding / 1000.0, double(eds12) / f,
        double(eds12) / f * kUsPerBinding / 1000.0, double(eds123) / f, double(eds123) / f * kUsPerBinding / 1000.0);
  }


  void ReportMapPipelinePhases(std::chrono::steady_clock::time_point now) {
    const uint64_t generation = bo2zm::preset::MapLoadGeneration();
    if (!generation) {
      return;
    }
    const map_pipeline::Counts counts{
        map_first_pipeline_requests_, map_first_pipeline_uncached_,
        compiles_async_, draws_waiting_pipeline_, ring_prewarmed_};
    const auto result = map_pipeline_meter_.Observe(
        generation, bo2zm::cheats::InMatch(), counts, now);
    if (!result) {
      return;
    }
    using Kind = map_pipeline::Kind;
    const char* label = "unknown";
    switch (result->kind) {
      case Kind::kMapSelected:
        map_pipeline_name_ = bo2zm::preset::LoadedMap();
        label = "MAP_SELECTED";
        break;
      case Kind::kLoadingProgress:
        label = "LOADING_10S";
        break;
      case Kind::kLoadingComplete:
        label = "LOADING_COMPLETE";
        break;
      case Kind::kPlayingProgress:
        label = "PLAYING_10S";
        break;
      case Kind::kMatchEnded:
        label = "MATCH_ENDED";
        break;
    }
    const auto& d = result->since_last;
    const auto& t = result->phase_total;
    REXLOG_INFO(
        "[native] C6 map pipelines v0.38 {}: map '{}' load {}, phase {} ms; "
        "since last: {} distinct first requests, {} require compiler, {} async completed, "
        "{} draws omitted, {} listed; phase total: {} first requests, {} require compiler, "
        "{} async completed, {} draws omitted, {} listed",
        label, map_pipeline_name_, result->generation, result->elapsed_ms,
        d.first_requests, d.needs_compile, d.async_finished, d.skipped_draws,
        d.list_prepared, t.first_requests, t.needs_compile,
        t.async_finished, t.skipped_draws, t.list_prepared);


    std::string safe_map = map_pipeline_name_.substr(0, 96);
    for (char& ch : safe_map) {
      if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == ' ')) {
        ch = '_';
      }
    }
    const std::string json = fmt::format(
        R"({{"schema":1,"event":"{}","map":"{}","generation":{},"elapsed_ms":{},"first":{},"compile_required":{},"async_finished":{},"skipped":{},"listed":{},"delta_first":{},"delta_compile_required":{},"delta_finished":{},"delta_skipped":{},"delta_listed":{}}})",
        label, safe_map, result->generation, result->elapsed_ms,
        t.first_requests, t.needs_compile, t.async_finished, t.skipped_draws,
        t.list_prepared, d.first_requests, d.needs_compile, d.async_finished,
        d.skipped_draws, d.list_prepared);
    {
      std::lock_guard<std::mutex> lock(writer_mutex_);
      try {
        StartWriterCache();
        writer_map_events_.push_back(json);
        writer_pending_ = true;
        writer_warning_.notify_one();
      } catch (const std::system_error& error) {
        REXLOG_WARN("[native] C6 map pipeline history: cannot start writer ({})", error.what());
      }
    }
  }


  void ReportPipelinesDirect() {
    const auto now = std::chrono::steady_clock::now();
    CollectCompiledPipelines();
    ReportMapPipelinePhases(now);
    ReportCompiles(now);
    ReportChangesPipeline(now);
    TryPrewarm();
    ReportPrewarm(now);
    if (now - pipelines_direct_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = pipelines_direct_report_ == std::chrono::steady_clock::time_point{};
    pipelines_direct_report_ = now;
    const uint64_t hits = pipelines_direct_hits_ - pipelines_direct_hits_previous_;
    const uint64_t misses = pipelines_direct_misses_ - pipelines_direct_misses_previous_;
    pipelines_direct_hits_previous_ = pipelines_direct_hits_;
    pipelines_direct_misses_previous_ = pipelines_direct_misses_;
    if (!first && (hits || misses)) {
      BO2ZM_REPORT_RING("[native] C6 pipelines: direct cache {} hits and {} misses ({:.1f} % of the lookups "
                  "the one-entry shortcut does not resolve); {} checked against the map ({})",
                  hits, misses, 100.0 * double(hits) / double(hits + misses),
                  pipelines_direct_checked_,
                  pipelines_direct_off_ ? "OFF by the guard"
                  : pipelines_direct_checked_ >= kPipelinesACheck ? "guard passed"
                                                                           : "checking");
    }
  }


  void CutSubstage(size_t k, std::chrono::steady_clock::time_point& t) {
    if (!time_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count());
    ++sub_n_[k];
    t = now;
  }


  void SamplerPath(uint32_t k, std::chrono::steady_clock::time_point t) {
    if (!time_) {
      return;
    }
    sub_ns_[k] += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
    ++sub_n_[k];
  }

  void Stage(size_t stage, std::chrono::steady_clock::time_point& mark) {
    if (!time_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    stages_ns_[stage] += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count());
    mark = now;
  }


  void AnnotateVerticesRepeated(uint64_t address, uint32_t bytes, uint32_t order,
                               const uint8_t* data) {
    const auto before = std::chrono::steady_clock::now();
    const uint64_t hash = XXH3_64bits(data, bytes);
    const uint64_t key =
        XXH3_64bits_withSeed(&address, sizeof(address), (uint64_t(bytes) << 2) | order);
    if (vertices_seen_.size() > 200000) {
      vertices_seen_.clear();
    }
    SeenVertices& seen = vertices_seen_[key];
    if (seen.hash == hash && seen.frame == frame_) {
      bytes_repeated_frame_ += bytes;
    } else if (seen.hash == hash && seen.frame != UINT64_MAX &&
               seen.frame < frame_) {
      bytes_equal_earlier_ += bytes;
    }
    seen.frame = frame_;
    seen.hash = hash;
    ns_hash_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - before)
                                      .count());
  }

  bool PassActive() const override { return pass_active_; }

  void FinishPass() override {
    if (pass_active_) {


      if (sky_pending_) {
        EmitSkyPostponed(pass_commands_, kSkyAtPassEnd);
      }

      if (query_occlusion_ != UINT32_MAX) {
        context_->FinishQueryOcclusion(query_occlusion_);
        query_occlusion_ = UINT32_MAX;
      }
      const auto before_end = std::chrono::steady_clock::now();
      dfn_.vkCmdEndRenderPass(pass_commands_);
      for (uint32_t i = 0; i < 5; ++i) {
        if (pass_handles_[i] != VK_NULL_HANDLE) {
          bo2zm::native::edram::Get().PassEnded(uint64_t(pass_handles_[i]), (pass_store_discard_ >> i) & 0x1);
        }
      }
      if (stats_pass_ != UINT32_MAX) {
        context_->FinishStats(stats_pass_);
        stats_pass_ = UINT32_MAX;
      }
      ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - before_end).count());
      context_->ProfilePassEnd();

      if (pass_opened_nonwriting_ && readonly_depth_ == 1) {
        namespace v = bo2zm::native::v055;
        if (readonly_learn_.size() > 8192) {
          readonly_learn_.clear();
        }
        v::ReadOnlyLearn& learned = readonly_learn_[pass_learn_key_];
        if (pass_depth_readonly_) {
          if (pass_readonly_blocked_) {
            v::g_readonly_misses.fetch_add(1, std::memory_order_relaxed);
            if (v::Missed(learned)) {
              v::g_readonly_banned.fetch_add(1, std::memory_order_relaxed);
            }
          } else {
            v::PassClosed(learned, false);
            v::g_readonly_passes.fetch_add(1, std::memory_order_relaxed);
            v::g_readonly_texels.fetch_add(pass_readonly_texels_, std::memory_order_relaxed);
          }
        } else {
          v::PassClosed(learned, pass_ds_written_);
        }
      }
      pass_depth_readonly_ = false;
      pass_opened_nonwriting_ = false;
      const uint32_t category_closed = category_pass_;
      pass_active_ = false;


      if (sky_pending_) {
        sky_pending_ = false;
        ++sky_lost_;
      }


      if (category_closed == kGpuShadows && !submitted_after_shadows_ && context_ &&
          REXCVAR_GET(bo2zm_native_submit_after_shadows)) {
        submitted_after_shadows_ = true;
        ++submits_after_shadows_;
        context_->SubmitYWait();
      }
    }
  }

  void BeforeSubmit() override {
    WaitUploads();
    FinishPass();


    CollectBindings(true);


    CollectHashes();


    if (skin_on_) {
      DispatchSkinTable();
    }
    if (upload_used_ && !upload_coherent_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, upload_memory_, upload_type_, 0,
                                                    upload_size_real_, upload_used_);
    }
    PublishVertexPools();
    if (shared_separate_ && shared_used_ && !shared_coherent_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, shared_memory_, shared_type_, 0,
                                                    shared_size_real_, shared_used_);
      shared_bytes_published_ += shared_used_;
    }
  }

  void UseSlot(uint32_t slot) override {
    WaitUploads();


    CheckGpuSkin(uint32_t(slot % uploads_.size()));
    skin_bound_ = false;
    skin_tables_[slot % uploads_.size()] = {};
    ReportCopies();
    ReportCacheFetch();


    CheckBindingsOnBufferChange();
    CheckHashesOnBufferChange();
    const UploadBuffer& s = uploads_[slot % uploads_.size()];
    upload_ = s.buffer;
    upload_memory_ = s.guest_memory;
    upload_size_real_ = s.size_real;
    upload_data_ = s.data;
    upload_address_ = s.address;
    slot_current_ = uint32_t(slot % uploads_.size());
    upload_used_ = 0;
    if (shared_separate_) {
      const UploadBuffer& c = shared_bufs_[slot_current_];
      shared_data_ = c.data;
      shared_memory_ = c.guest_memory;
      shared_size_real_ = c.size_real;
      shared_used_ = 0;
    }
    ++epoch_upload_;
    ++frame_;
    bo2zm::page_writes::NextEpoch();
    CloseGuardSkyFrame();

    measure_visibility_ = bo2zm::reflection_demand::MeasureVisibility();
    if (measure_visibility_ && frame_ >= witness_next_) {
      witness_pending_ = true;
    }


    dedupe_.NewFrame(frame_);
    dedupe_indices_.NewFrame(frame_);
    GuardDedupe();
    submitted_after_shadows_ = false;


    diag_skip_ps_text_ = REXCVAR_GET(bo2zm_native_diag_skip_ps);
    diag_vertices_repeated_ = REXCVAR_GET(bo2zm_native_diag_repeated_vertices);
    dedupe_active_ = REXCVAR_GET(bo2zm_native_dedupe_vertices);


    {
      bool on = vcache_ready_ && !vcache_off_ && REXCVAR_GET(bo2zm_native_vertex_cache) && !bo2zm::debug::CachesOff();
      if (on && vcache_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      vcache_active_ = on;

      bool sum = REXCVAR_GET(bo2zm_native_vertex_cache_sum);
      if (vcache_sum_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        if (arm >= 0) {
          sum = (arm & 2) != 0;
        }
      }
      vcache_sum_ = sum;
      ReportVertexCache();
    }

    if (tex_by_writes_ < 0) {
      tex_by_writes_ab_ = REXCVAR_GET(bo2zm_native_texture_by_writes_ab);
      tex_by_writes_ = REXCVAR_GET(bo2zm_native_texture_by_writes) || tex_by_writes_ab_ ? 1 : 0;
      if (tex_by_writes_) {
        bo2zm::page_writes::Enable(true);
      }
      REXLOG_INFO("[native] C3: texture rechecks by write stamps (bo2zm_native_texture_by_writes) = {}{}",
                  tex_by_writes_ ? "YES" : "no", tex_by_writes_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    {
      bool on = tex_by_writes_ > 0 && !bo2zm::debug::TextureShortcutsOff();
      if (on && tex_by_writes_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      tex_by_writes_active_ = on;
    }

    if (tex_young_ < 0) {
      tex_young_ab_ = REXCVAR_GET(bo2zm_native_texture_by_writes_young_ab);
      tex_young_ = REXCVAR_GET(bo2zm_native_texture_by_writes_young) || tex_young_ab_ ? 1 : 0;
      REXLOG_INFO("[native] C3: young textures skipped by write stamps too (bo2zm_native_texture_by_writes_young) = {}{}",
                  tex_young_ ? "YES" : "no", tex_young_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    {
      bool on = tex_young_ > 0 && !bo2zm::debug::TextureShortcutsOff();
      if (on && tex_young_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      tex_young_active_ = on;
    }

    if (icache_ < 0) {
      icache_ab_ = REXCVAR_GET(bo2zm_native_index_cache_ab);
      icache_ = REXCVAR_GET(bo2zm_native_index_cache) || icache_ab_ ? 1 : 0;
      REXLOG_INFO("[native] C6 index cache by write stamps (bo2zm_native_index_cache) = {}{}", icache_ ? "YES" : "no",
                  icache_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    {
      bool on = icache_ > 0 && vcache_ready_ && !vcache_off_ &&
                bo2zm::page_writes::g_enabled.load(std::memory_order_relaxed) && !bo2zm::debug::CachesOff();
      if (on && icache_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      icache_active_ = on;
    }
    ReportIndexCache();

    if (constants_used_ < 0) {
      constants_used_ab_ = REXCVAR_GET(bo2zm_native_constants_used_ab);
      constants_used_ = REXCVAR_GET(bo2zm_native_constants_used) || constants_used_ab_ ? 1 : 0;
      REXLOG_INFO("[native] C6 only the constants each shader reads (bo2zm_native_constants_used) = {}{}",
                  constants_used_ ? "YES" : "no", constants_used_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    {
      bool on = constants_used_ > 0 && !bo2zm::debug::ConstantsShortcutOff();
      if (on && constants_used_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      constants_used_active_ = on;
    }

    {
      static const bool regions_ab = REXCVAR_GET(bo2zm_native_stamp_regions_ab);
      static const bool regions = REXCVAR_GET(bo2zm_native_stamp_regions) || regions_ab;
      bool on = regions;
      if (on && regions_ab) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      bo2zm::page_writes::g_use_regions.store(on, std::memory_order_relaxed);
    }

    if (prefetch_ < 0) {
      prefetch_ab_ = REXCVAR_GET(bo2zm_native_prefetch_cache_lines_ab);
      prefetch_ = REXCVAR_GET(bo2zm_native_prefetch_cache_lines) || prefetch_ab_ ? 1 : 0;
      REXLOG_INFO("[native] C6 cache lines prefetched early (bo2zm_native_prefetch_cache_lines) = {}{}",
                  prefetch_ ? "YES" : "no", prefetch_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    {
      bool on = prefetch_ > 0;
      if (on && prefetch_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      prefetch_lines_ = on;
    }

    {
      bool on = TextureReuse();
      if (on && texreuse_ab_) {
        const int arm = bo2zm::benchmark::AbArm();
        on = arm < 0 || (arm & 1) != 0;
      }
      texreuse_active_ = on;
    }
    dedupe_indices_on_ = REXCVAR_GET(bo2zm_native_dedupe_indices) && !dedupe_indices_off_;


    {
      static const bool front_ab = REXCVAR_GET(bo2zm_native_input_front_ab);
      static const bool front = REXCVAR_GET(bo2zm_native_input_front) || front_ab;
      static const bool batch_ab = REXCVAR_GET(bo2zm_native_deferred_batch_ab);
      static const bool batch = REXCVAR_GET(bo2zm_native_deferred_batch) || batch_ab;
      const int arm = front_ab || batch_ab ? bo2zm::benchmark::AbArm() : -1;
      inputs_front_on_ = front && !inputs_front_off_ && (!front_ab || arm < 0 || (arm & 1) != 0);
      deferred::PublishInBatches(batch && (!batch_ab || arm < 0 || (arm & 2) != 0));
    }


    {
      static const bool fewer_ab = REXCVAR_GET(bo2zm_native_fewer_copies_ab);
      static const bool fewer = REXCVAR_GET(bo2zm_native_fewer_copies) || fewer_ab;
      static const bool swap_ab = REXCVAR_GET(bo2zm_native_shared_swap_ab);
      static const bool swap = REXCVAR_GET(bo2zm_native_shared_swap) || swap_ab;
      const int arm = fewer_ab || swap_ab ? bo2zm::benchmark::AbArm() : -1;
      fewer_copies_ = fewer && (!fewer_ab || arm < 0 || (arm & 1) != 0);
      shared_swap_ = swap && (!swap_ab || arm < 0 || (arm & 2) != 0);
    }
    vertices_base_cero_ = REXCVAR_GET(bo2zm_native_zero_base_vertices) && !vertices_base_zero_off_;
    diag_stats_draw_ = REXCVAR_GET(bo2zm_native_per_draw_stats_s) > 0;
    area_util_ = REXCVAR_GET(bo2zm_native_useful_area_pass);
    area_to_tile_ = REXCVAR_GET(bo2zm_perf_v055);


    if (readonly_depth_ < 0) {
      const bool wanted = REXCVAR_GET(bo2zm_perf_readonly_depth);
      const bool device = vulkan_device_->extensions().ext_1_3_KHR_dynamic_rendering;
      readonly_depth_ = wanted && device ? 1 : 0;
      bo2zm::native::v055::g_readonly_state.store(!wanted ? 0 : device ? 1 : 2, std::memory_order_relaxed);
      REXLOG_INFO("[perf] 0.55: pass area rounded to {} rows, converted resolves {} their destination, separate "
                  "full-aspect guest clears {}; read-only depth passes (storeOp = NONE) {}",
                  area_to_tile_ ? 16 : 64, area_to_tile_ ? "do not load" : "load",
                  area_to_tile_ ? "as loadOp = CLEAR" : "load and clear",
                  readonly_depth_ ? "ON: learned per kind of pass opening"
                  : wanted        ? "OFF: this device has no VK_ATTACHMENT_STORE_OP_NONE"
                                  : "off (bo2zm_perf_readonly_depth = false)");
    }

    framing_cache_ = REXCVAR_GET(bo2zm_native_framing_cache) && !framing_cache_off_;
    height_useful_memo_active_ = REXCVAR_GET(bo2zm_native_useful_height_memo) && !height_useful_memo_off_;
    key_pass_fast_ = REXCVAR_GET(bo2zm_native_fast_pass_key) && !key_pass_fast_off_;
    cvars_per_frame_ = REXCVAR_GET(bo2zm_native_cvars_per_frame);
    ps_only_alpha_frame_ = PsOnlyAlpha();
    no_ps_no_color_frame_ = NoPsNoColor();
    no_vegetation_ = REXCVAR_GET(bo2zm_shadows_without_vegetation);
    pipelines_direct_ = REXCVAR_GET(bo2zm_native_direct_pipelines) && !pipelines_direct_off_;
    key_canonical_ = REXCVAR_GET(bo2zm_native_canonical_key) && !key_canonical_off_;
    {
      const bool new_value = REXCVAR_GET(bo2zm_native_pipeline_across_passes);
      if (new_value != pipeline_between_passes_ || !pipeline_between_passes_annotated_) {
        pipeline_between_passes_ = new_value;
        pipeline_between_passes_annotated_ = true;
        REXLOG_INFO("[native] C6 pipeline when a pass begins: {} (frame {})",
                    new_value ? "the binding is KEPT (dynamic state)" : "bound again, as before",
                    frame_);
      }
    }


    {
      uint32_t mode = 0;
      if (eds12_available_ && !eds_off_ && REXCVAR_GET(bo2zm_native_dynamic_state)) {
        mode |= kEds12;
      }
      if (eds3_available_ && !eds_off_ && REXCVAR_GET(bo2zm_native_dynamic_state3)) {
        mode |= kEds3;
      }
      if (mode != eds_mode_ || !eds_mode_annotated_) {
        eds_mode_annotated_ = true;
        REXLOG_INFO("[native] C6 dynamic state: {} (frame {})", NameModeEds(mode), frame_);
      }
      eds_mode_ = mode;
    }
    scissor_test_ = TestScissor();
    no_blur_frame_ = REXCVAR_GET(bo2zm_native_no_blur);

    {
      bool new_value = REXCVAR_GET(bo2zm_native_cheap_pcf);
      const int32_t alternate_pcf = REXCVAR_GET(bo2zm_native_cheap_pcf_alternate_s);
      if (alternate_pcf > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_ps_)
                                  .count();
        new_value = (seconds / alternate_pcf) % 2 == 1;
      }
      if (new_value != pcf_cheap_) {
        pcf_cheap_ = new_value;
        REXLOG_INFO("[native] C2 shadow map sampling: {}",
                    new_value ? "a single texel (cheap PCF)" : "3x3 pattern at half a texel");
      }
    }


    {
      bool new_value = REXCVAR_GET(bo2zm_native_test_point_mip);
      const int32_t alternate_mip = REXCVAR_GET(bo2zm_native_test_point_mip_alternate_s);
      if (alternate_mip > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_ps_)
                                  .count();
        new_value = (seconds / alternate_mip) % 2 == 1;
      }
      if (new_value != mip_point_test_) {
        mip_point_test_ = new_value;
        REXLOG_INFO("[native] C4 filter between mip levels: {}",
                    new_value ? "POINT (test: trilinear -> bilinear, the TMU does half)"
                          : "linear (what the game asks for)");
      }
    }


    {
      const uint32_t new_value = uint32_t(std::clamp(int32_t(REXCVAR_GET(bo2zm_native_diag_min_mip)), 0, 4));
      const uint32_t new_preset = std::min(4u, bo2zm::preset::MipsMinus());
      if (new_value != diag_mip_min_ || new_preset != preset_mip_min_) {
        diag_mip_min_ = new_value;
        preset_mip_min_ = new_preset;
        ++generation_textures_;
        REXLOG_INFO("[native] C4 texture mip floor: world {}, UI {} (preset {}, diagnostic {})",
                    std::max(new_value, new_preset), new_value, new_preset, new_value);
      }
    }

    {
      const int32_t constant = std::clamp(int32_t(REXCVAR_GET(bo2zm_native_shadow_bias_constant)), 0, 100);
      const int32_t pending = std::clamp(int32_t(REXCVAR_GET(bo2zm_native_shadow_bias_slope)), 0, 100);
      if (constant != shadows_bias_constant_ || pending != shadows_bias_pending_ || !shadows_bias_annotated_) {
        shadows_bias_constant_ = constant;
        shadows_bias_pending_ = pending;
        shadows_bias_annotated_ = true;
        REXLOG_INFO("[native] C4 shadow map depth bias: slope {:.1f}, constant {} (x1000 24-bit units){}",
                    double(pending) * 0.1, constant, (constant || pending) ? "" : " (off, as before)");
      }
    }

    {
      const int32_t alternate = REXCVAR_GET(bo2zm_native_inv_tex_size_alternate_s);
      bool new_value = REXCVAR_GET(bo2zm_native_inv_tex_size);
      if (alternate > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_ps_)
                                  .count();
        new_value = (seconds / alternate) % 2 == 1;
      }
      if (new_value != inv_size_tex_) {
        inv_size_tex_ = new_value;
        REXLOG_INFO("[native] C2 texture size: {}",
                    new_value ? "by constant" : "asked from the texture");
      }
    }


    {
      const int32_t alternate = REXCVAR_GET(bo2zm_native_early_z_alternate_s);
      bool new_value = REXCVAR_GET(bo2zm_native_early_z);
      if (alternate > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_z_)
                                  .count();
        new_value = (seconds / alternate) % 2 == 1;
      }
      if (new_value != z_early_ || !alternation_z_annotated_) {
        z_early_ = new_value;
        alternation_z_annotated_ = true;
        REXLOG_INFO("[native] C6 depth test: {} (frame {})",
                    new_value ? "BEFORE shading where Z is not written (early Z)"
                          : "after shading, as before",
                    frame_);
      }
    }
    skip_invisible_ = REXCVAR_GET(bo2zm_native_skip_invisible);


    {
      const bool new_value = REXCVAR_GET(bo2zm_native_postponed_sky);
      if (new_value != sky_postpone_on_ || !sky_annotated_) {
        sky_postpone_on_ = new_value;
        sky_annotated_ = true;
        REXLOG_INFO("[native] C6 sky: {} (frame {})",
                    new_value ? "POSTPONED until after the pass's opaque draws"
                          : "in its original place (first, on the empty Z)",
                    frame_);
      }
    }
    ReportZEarly();
    ReportPipelinesDirect();
    ReportMinor();
    ReportBaseZero();
    ControlSet4();
    ReportSet4();
    ControlDrawNvk();
    ReportDrawNvk();
    ControlCopiesNvk();
    ReportCopiesNvk();
    ControlKindViewsNvk();
    ReportKeyCanonical();
    ReportPipelineBetweenPasses();
    ReportStateDynamic();
    ReportBindings();
    ReportVolumes();
    ReportHashes();
    ReportReuse();
    ReportTextureStamps();
    ReportTextureReuse();
    EvictTexturesYesDoesMissing();
    pool_textures_.PerFrame(frame_);


    if (alternate_ubo_s_ > 0) {
      const auto seconds =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_ubo_).count();
      const bool ubo = (seconds / alternate_ubo_s_) % 2 == 1;
      if (ubo != use_ubo_) {
        use_ubo_ = ubo;


        std::memset(push_recorded_, 0, sizeof(push_recorded_));
        REXLOG_INFO("[native] constants test: {} (frame {})", ubo ? "by UBO" : "by pointer", frame_);
      }
    }
    ++submits_;
    if (use_ubo_) {
      ++submits_ubo_;
    }

    if (const uint32_t compiled = prewarm_compiled_.load(std::memory_order_relaxed) +
                                  prewarm_compiled_helper_.load(std::memory_order_relaxed);
        compiled != prewarm_counted_) {
      pipelines_no_save_ += compiled - prewarm_counted_;
      prewarm_counted_ = compiled;
    }


    SaveCacheWhenDue();
  }


  void SaveCacheWhenDue() {
    const auto now = std::chrono::steady_clock::now();
    if (pipelines_no_save_ != pipelines_seen_) {
      pipelines_seen_ = pipelines_no_save_;
      pipeline_new_at_ = now;
    }
    const bool in_match = bo2zm::cheats::InMatch();


    const bool returned_to_lobby = pipeline_was_in_match_ && !in_match;
    pipeline_was_in_match_ = in_match;
    const int mode = REXCVAR_GET(bo2zm_native_pipelines_save_in_match);
    const bool due =
        pipelines_no_save_ && (pipelines_no_save_ >= 64 || now - cache_saved_ >= std::chrono::seconds(60));
    if (!in_match || mode == 2) {
      const bool pending_list = list_no_save_ || writer_list_waiting_.load(std::memory_order_relaxed);
      if (due || (!in_match && writer_list_waiting_.load(std::memory_order_relaxed)) ||
          (returned_to_lobby && (pipelines_no_save_ || pending_list))) {
        if (returned_to_lobby && (pipelines_no_save_ || pending_list)) {
          REXLOG_INFO("[native] C6 pipeline cache: match ended; queuing {} new pipeline cache entries and {} "
                      "pending list records for the next lobby/start",
                      pipelines_no_save_, list_no_save_);
        }
        SaveCachePipelines(false);
      }
    } else if (mode == 1 && pipelines_no_save_ && now - pipeline_new_at_ >= std::chrono::seconds(30)) {
      SaveCachePipelines(true);
    }


    if (!vouch_requested_ && !pipelines_no_save_ && !list_no_save_ && !bo2zm::native::precompile::KnownWarm() &&
        prewarm_finished_.load(std::memory_order_acquire) &&
        (!prewarm_helper_.joinable() || prewarm_helper_done_.load(std::memory_order_acquire)) &&
        prewarm_compiled_.load(std::memory_order_relaxed) + prewarm_compiled_helper_.load(std::memory_order_relaxed) ==
            prewarm_counted_ &&
        ListVerified()) {
      RequestVouch();
    }

    if (REXCVAR_GET(bo2zm_native_test_export_list) && list_session_.size() != exported_size_ &&
        now - exported_at_ >= std::chrono::seconds(20)) {
      exported_size_ = list_session_.size();
      exported_at_ = now;
      std::vector<uint8_t> list = SerializeListPipelines();
      std::lock_guard<std::mutex> lock(writer_mutex_);
      StartWriterCache();
      writer_export_ = std::move(list);
      writer_pending_ = true;
      writer_warning_.notify_one();
    }

    if (const int every = REXCVAR_GET(bo2zm_native_test_pipelines_resave_s); every > 0 && in_match) {
      if (resave_at_ == std::chrono::steady_clock::time_point{}) {
        resave_at_ = now + std::chrono::seconds(every);
      } else if (now >= resave_at_) {
        resave_at_ = now + std::chrono::seconds(every);
        {
          std::lock_guard<std::mutex> lock(writer_mutex_);
          writer_resave_ = (resaves_++ % 2) ? 2 : 1;
        }
        SaveCachePipelines(false);
      }
    }
  }

  void InvalidateTextures() override {
    ++generation_textures_;
    ++invalidations_global_;
  }


  void InvalidateTexturesIn(uint32_t address) override {
    address &= 0x1FFFF000u;
    for (CacheSampler& c : cache_samplers_) {
      const uint32_t d = AddressCacheSampler(c.fetch.data());
      if ((d & ~1u) == address || (d & 1u)) {
        c.frame = UINT64_MAX;
        c.valid_until = 0;
      }
    }


    static_assert(std::tuple_size_v<decltype(cache_fetch_dir_)> % 8 == 0, "the table is walked eight at a time");
    for (size_t i = 0; i < cache_fetch_dir_.size(); i += 8) {
      uint32_t hit = 0;
      for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t d = cache_fetch_dir_[i + k];
        hit |= uint32_t(((d & ~1u) == address) | (d & 1u)) << k;
      }
      while (hit) [[unlikely]] {
        const uint32_t k = uint32_t(std::countr_zero(hit));
        hit &= hit - 1;
        cache_fetch_[i + k].frame = UINT64_MAX;
        cache_fetch_[i + k].valid_until = 0;
      }
    }
    ++invalidations_targeted_;
  }


  static uint32_t AddressCacheSampler(const uint32_t* fetch) {
    return (fetch[1] & 0x1FFFF000u) |
           (((fetch[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::kCube) ? 1u : 0u);
  }


  void InvalidateImages(VkImage a, VkImage b) override {
    for (VkImage image : {a, b}) {
      if (image == VK_NULL_HANDLE || (image == b && a == b)) {
        continue;
      }
      const auto it = views_per_image_.find(image);
      if (it == views_per_image_.end()) {
        continue;
      }
      for (uint64_t key : it->second) {
        const auto v = views_.find(key);
        if (v != views_.end() && v->second.image == image) {
          RetireSlot(v->second.heap, v->second.slot);
        }
      }
    }
  }

  void OcclusionOpen(bool open) override {
    if (!open && query_occlusion_ != UINT32_MAX) {

      context_->FinishQueryOcclusion(query_occlusion_);
      query_occlusion_ = UINT32_MAX;
    }
    occlusion_open_ = open;
  }

  uint64_t Drawn() const override { return drawn_; }


  static constexpr uint64_t kFramesNoUseForRelease = 120;


  void AnnotateGpuMemory() {
    const auto* instance = vulkan_device_->vulkan_instance();
    if (!instance) {
      return;
    }
    const auto& ifn = instance->functions();
    const VkPhysicalDevice physical = vulkan_device_->physical_device();
    const bool has_budget = vulkan_device_->extensions().ext_EXT_memory_budget &&
                                 ifn.vkGetPhysicalDeviceMemoryProperties2 != nullptr;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties2.pNext = has_budget ? &budget : nullptr;
    if (has_budget) {
      ifn.vkGetPhysicalDeviceMemoryProperties2(physical, &properties2);
    } else {
      ifn.vkGetPhysicalDeviceMemoryProperties(physical, &properties2.memoryProperties);
    }
    const auto& mp = properties2.memoryProperties;
    std::string text;
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
      if (!text.empty()) {
        text += " | ";
      }
      const bool local = (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
      if (has_budget) {
        text += fmt::format("heap {}{}: {} MB used of {} MB budgeted (size {} MB)", i,
                             local ? " (GPU)" : "", budget.heapUsage[i] >> 20,
                             budget.heapBudget[i] >> 20, mp.memoryHeaps[i].size >> 20);
      } else {
        text += fmt::format("heap {}{}: {} MB", i, local ? " (GPU)" : "",
                             mp.memoryHeaps[i].size >> 20);
      }
    }
    REXLOG_INFO("[native] C3 GPU memory: {}{}; the texture cache holds {} MB of {} MB; texture slots in use: 2D {} of "
                "{}, 3D {} of {}, cube {} of {}",
                text, has_budget ? "" : " (without VK_EXT_memory_budget: sizes only)", bytes_textures_ >> 20,
                textures_mb_max_, SlotsUsed(0), kCapacityHeap[0], SlotsUsed(1), kCapacityHeap[1], SlotsUsed(2),
                kCapacityHeap[2]);


    if (pool_textures_.IsActive()) {
      REXLOG_INFO("[native] C3 {}", pool_textures_.Summary());
    }
  }


  static constexpr uint32_t kReleasePerFrame = 4;


  bool SlotsTight() const {
    for (uint32_t i = 0; i < 3; ++i) {
      const auto& h = heaps_[i];
      const uint64_t used = h.next - 1 - std::min<uint64_t>(h.free.size(), h.next - 1);
      if (h.capacity && used * 5 >= uint64_t(h.capacity) * 4) {
        return true;
      }
    }
    return false;
  }

  void EvictTexturesYesDoesMissing() {
    const uint64_t limit = uint64_t(std::max(textures_mb_max_, 0)) << 20;

    if ((limit && bytes_textures_ > limit / 4 * 3) || SlotsTight()) {
      ReleaseSomeFewCold();
    }
    if (!limit) {
      return;
    }


    if (bytes_textures_ > limit && frame_ >= attempt_eviction_ + 60) {
      attempt_eviction_ = frame_;
      ReleaseTextures(limit / 4 * 3, kFramesNoUseForRelease, "above the limit");
    }
  }


  bool ColdForRelease(const CachedTexture& texture) const {
    return texture.image.image != VK_NULL_HANDLE && !texture.needs_upload && !texture.in_flight && !texture.hash_job &&
           texture.frame != UINT64_MAX && texture.frame + kFramesNoUseForRelease < frame_;
  }

  bool EvictByKey() {
    static const bool on = REXCVAR_GET(bo2zm_native_evict_by_key);
    static const bool ab = REXCVAR_GET(bo2zm_native_evict_by_key_ab);
    if (ab) {
      const int arm = bo2zm::benchmark::AbArm();
      if (arm >= 0) {
        return (arm & 1) != 0;
      }
    }
    return on;
  }


  static constexpr uint64_t kColdScanEvery = 16;
  static constexpr size_t kColdKeep = 64;

  void ReleaseSomeFewColdByKey() {
    const bool due = frame_ >= cold_scan_frame_ + kColdScanEvery || (cold_list_.empty() && cold_scan_full_);
    if (due) {
      cold_list_.clear();
      for (const auto& [key, texture] : textures_) {
        if (ColdForRelease(texture)) {
          cold_list_.emplace_back(texture.frame, key);
        }
      }
      cold_scan_full_ = cold_list_.size() > kColdKeep;
      if (cold_scan_full_) {
        std::nth_element(cold_list_.begin(), cold_list_.begin() + kColdKeep, cold_list_.end());
        cold_list_.resize(kColdKeep);
      }

      std::sort(cold_list_.begin(), cold_list_.end(), std::greater<>());
      cold_scan_frame_ = frame_;
      ++cold_scans_;
    }
    keys_release_.clear();
    while (keys_release_.size() < kReleasePerFrame && !cold_list_.empty()) {
      const uint64_t key = cold_list_.back().second;
      cold_list_.pop_back();
      const auto it = textures_.find(key);
      if (it == textures_.end() || !ColdForRelease(it->second)) {
        continue;
      }
      keys_release_.push_back(key);
      bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
    }
    if (keys_release_.empty()) {
      return;
    }
    ReleaseKeys(keys_release_);
    textures_released_ += keys_release_.size();
    released_little_a_little_ += keys_release_.size();
    if (frame_ >= warning_trickle_ + 600) {
      warning_trickle_ = frame_;
      REXLOG_INFO("[native] C3: texture cache near the limit: cold ones are released a trickle at a time ({} in total, "
                  "by key, {} scans); {} textures and {} MB of {} MB remain",
                  released_little_a_little_, cold_scans_, textures_.size(), bytes_textures_ >> 20, textures_mb_max_);
    }
  }


  void ReleaseKeys(const std::vector<uint64_t>& keys) {
    ++invalidations_retired_;
    for (const uint64_t key : keys) {
      const auto it = textures_.find(key);
      if (it == textures_.end()) {
        continue;
      }
      const VkImage image = it->second.image.image;
      const auto index = image != VK_NULL_HANDLE ? views_per_image_.find(image) : views_per_image_.end();
      if (index != views_per_image_.end()) {
        for (const uint64_t view_key : index->second) {
          const auto v = views_.find(view_key);
          if (v == views_.end() || v->second.image != image) {
            continue;
          }
          RetireSlot(v->second.heap, v->second.slot);
          WriteImage(v->second.heap, v->second.slot, empty_[v->second.heap].view);
          heaps_[v->second.heap].free.push_back(v->second.slot);
          dfn_.vkDestroyImageView(device_, v->second.view, nullptr);
          views_.erase(v);
        }
        views_per_image_.erase(index);
      }
      RemoveContentTexture(it->second, it->first);
      DestroyImage(it->second.image);
      textures_.erase(it);
    }
  }


  void ReleaseSomeFewCold() {
    if (EvictByKey()) {
      ReleaseSomeFewColdByKey();
      return;
    }
    std::array<std::pair<uint64_t, uint64_t>, kReleasePerFrame> chosen{};
    uint32_t how_many = 0;
    for (const auto& [key, texture] : textures_) {

      if (texture.image.image == VK_NULL_HANDLE || texture.needs_upload || texture.in_flight || texture.hash_job ||
          texture.frame == UINT64_MAX ||
          texture.frame + kFramesNoUseForRelease >= frame_) {
        continue;
      }
      if (how_many < kReleasePerFrame) {
        chosen[how_many++] = {texture.frame, key};
        continue;
      }

      uint32_t worst = 0;
      for (uint32_t i = 1; i < how_many; ++i) {
        if (chosen[i].first > chosen[worst].first) {
          worst = i;
        }
      }
      if (texture.frame < chosen[worst].first) {
        chosen[worst] = {texture.frame, key};
      }
    }
    if (how_many == 0) {
      return;
    }
    std::unordered_set<VkImage> images;
    for (uint32_t i = 0; i < how_many; ++i) {
      auto it = textures_.find(chosen[i].second);
      if (it == textures_.end()) {
        continue;
      }
      images.insert(it->second.image.image);
      bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
    }
    ReleaseImages(images);
    textures_released_ += images.size();
    released_little_a_little_ += images.size();
    if (frame_ >= warning_trickle_ + 600) {
      warning_trickle_ = frame_;
      REXLOG_INFO("[native] C3: texture cache near the limit: cold ones are released a trickle at a time ({} in total); "
                  "{} textures and {} MB of {} MB remain",
                  released_little_a_little_, textures_.size(), bytes_textures_ >> 20, textures_mb_max_);
    }
  }


  bool ReleaseTexturesOnOutOfMemory() override {
    return ReleaseTextureMemory(nullptr);
  }

  bool ReleaseTextureMemory(const NativeImage* keep) {
    if (textures_.empty() || bytes_textures_ == 0 || releasing_on_out_of_memory_) {
      return false;
    }
    releasing_on_out_of_memory_ = true;
    bool algo = false;
    if (context_ && context_->WaitGpuIdle()) {
      algo = ReleaseTextures(bytes_textures_ / 2, 0, "OUT OF GPU MEMORY", keep) > 0;
    }
    releasing_on_out_of_memory_ = false;
    return algo;
  }


  void ReleaseImages(const std::unordered_set<VkImage>& images) {
    ++invalidations_retired_;
    for (auto it = views_.begin(); it != views_.end();) {
      if (!images.count(it->second.image)) {
        ++it;
        continue;
      }
      RetireSlot(it->second.heap, it->second.slot);
      WriteImage(it->second.heap, it->second.slot, empty_[it->second.heap].view);
      heaps_[it->second.heap].free.push_back(it->second.slot);
      dfn_.vkDestroyImageView(device_, it->second.view, nullptr);
      views_per_image_.erase(it->second.image);
      it = views_.erase(it);
    }
    for (auto it = textures_.begin(); it != textures_.end();) {
      if (!images.count(it->second.image.image)) {
        ++it;
        continue;
      }
      RemoveContentTexture(it->second, it->first);
      DestroyImage(it->second.image);
      it = textures_.erase(it);
    }
  }


  uint64_t ReleaseTextures(uint64_t goal, uint64_t min_age, const char* reason,
                           const NativeImage* keep = nullptr) {
    std::vector<std::pair<uint64_t, uint64_t>> candidates;
    for (const auto& [key, texture] : textures_) {

      if (&texture.image != keep && texture.image.image != VK_NULL_HANDLE &&
          !texture.needs_upload && !texture.in_flight && !texture.hash_job &&
          texture.frame != UINT64_MAX &&
          texture.frame + min_age < frame_) {
        candidates.emplace_back(texture.frame, key);
      }
    }
    std::sort(candidates.begin(), candidates.end());
    const uint64_t before = bytes_textures_;
    std::unordered_set<VkImage> images;
    for (const auto& [last, key] : candidates) {
      if (bytes_textures_ <= goal) {
        break;
      }
      auto it = textures_.find(key);
      images.insert(it->second.image.image);
      bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
    }
    if (images.empty()) {
      return 0;
    }
    ReleaseImages(images);
    textures_released_ += images.size();
    REXLOG_INFO("[native] C3: texture cache {} ({} MB): {} unused for more than {} frames removed ({} MB); {} textures "
                "and {} MB remain ({} released in total)",
                reason, before >> 20, images.size(), min_age,
                (before - bytes_textures_) >> 20, textures_.size(), bytes_textures_ >> 20, textures_released_);
    return before - bytes_textures_;
  }


  bool ClearDepthInPass(VkCommandBuffer commands, const NativeImage& image,
                               float depth, uint32_t stencil) override {
    if (commands == VK_NULL_HANDLE || image.view == VK_NULL_HANDLE || !image.width ||
        !image.height) {
      return false;
    }
    FinishPass();
    uint32_t formats[5] = {0, 0, 0, 0, uint32_t(image.format)};
    const VkRenderPass pass = PassOf(formats, kLoadClear);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[4] = image.view;
    const VkFramebuffer framebuffer = FramebufferOf(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue clear{};
    clear.depthStencil = {depth, stencil};
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea.extent = {image.width, image.height};
    start.clearValueCount = 1;
    start.pClearValues = &clear;
    dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdEndRenderPass(commands);
    ++clears_depth_in_pass_;
    return true;
  }


  bool ClearColorInPass(VkCommandBuffer commands, const NativeImage& image, const VkClearColorValue& color,
                         const VkRect2D& area) override {
    if (commands == VK_NULL_HANDLE || image.view == VK_NULL_HANDLE || !area.extent.width || !area.extent.height ||
        area.offset.x < 0 || area.offset.y < 0 || uint32_t(area.offset.x) + area.extent.width > image.width ||
        uint32_t(area.offset.y) + area.extent.height > image.height) {
      return false;
    }
    FinishPass();
    uint32_t formats[5] = {uint32_t(image.format), 0, 0, 0, 0};
    const VkRenderPass pass = PassOf(formats, kLoadClear);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[0] = image.view;
    const VkFramebuffer framebuffer = FramebufferOf(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue clear{};
    clear.color = color;
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = area;
    start.clearValueCount = 1;
    start.pClearValues = &clear;
    dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdEndRenderPass(commands);
    return true;
  }


  bool ClearAttachmentInPass(VkCommandBuffer commands, const NativeImage& image, VkImageAspectFlags aspects,
                           const VkClearValue& value, const VkRect2D& area) override {
    if (commands == VK_NULL_HANDLE || image.view == VK_NULL_HANDLE || !aspects || !area.extent.width ||
        !area.extent.height || area.offset.x < 0 || area.offset.y < 0 ||
        uint32_t(area.offset.x) + area.extent.width > image.width ||
        uint32_t(area.offset.y) + area.extent.height > image.height) {
      return false;
    }
    FinishPass();
    const uint32_t slot = (aspects & VK_IMAGE_ASPECT_COLOR_BIT) ? 0 : 4;
    uint32_t formats[5] = {};
    formats[slot] = uint32_t(image.format);


    static const bool clear_as_load = REXCVAR_GET(bo2zm_perf_v055);
    const bool as_load = clear_as_load && bo2zm::native::v055::ClearsWholeAttachment(
                                              uint32_t(aspects), slot == 0, FormatHasStencil(image.format));
    const VkRenderPass pass = PassOf(formats, as_load ? kLoadClear : kLoadRead);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[slot] = image.view;
    const VkFramebuffer framebuffer = FramebufferOf(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = area;
    if (as_load) {
      start.clearValueCount = 1;
      start.pClearValues = &value;
      dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
      dfn_.vkCmdEndRenderPass(commands);
      bo2zm::native::v055::g_separate_clears_as_load.fetch_add(1, std::memory_order_relaxed);
      bo2zm::native::v055::g_separate_clear_texels.fetch_add(uint64_t(area.extent.width) * area.extent.height,
                                                             std::memory_order_relaxed);
      return true;
    }
    dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment attachment{};
    attachment.aspectMask = aspects;
    attachment.colorAttachment = 0;
    attachment.clearValue = value;
    VkClearRect rectangle{};
    rectangle.rect = area;
    rectangle.baseArrayLayer = 0;
    rectangle.layerCount = 1;
    dfn_.vkCmdClearAttachments(commands, 1, &attachment, 1, &rectangle);
    dfn_.vkCmdEndRenderPass(commands);
    return true;
  }


  int ActivePassSlot(const NativeImage& image, VkImageAspectFlags aspects) const {
    if (!pass_active_ || pass_commands_ == VK_NULL_HANDLE || image.image == VK_NULL_HANDLE) {
      return -1;
    }
    if (v043::AspectsFitSlot(uint32_t(aspects), v043::kSlotDepth)) {
      return pass_images_[4] == &image && pass_handles_[4] == image.image ? 4 : -1;
    }
    if (!v043::AspectsFitSlot(uint32_t(aspects), 0)) {
      return -1;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (pass_images_[i] == &image && pass_handles_[i] == image.image) {
        return int(i);
      }
    }
    return -1;
  }

  bool CanClearInActivePass(const NativeImage& image, VkImageAspectFlags aspects,
                            const VkRect2D& area) const override {

    const int slot = ActivePassSlot(image, aspects);
    if (query_occlusion_ != UINT32_MAX || slot < 0) {
      return false;
    }


    if (slot == 4 && pass_depth_readonly_) {
      pass_readonly_blocked_ = true;
      return false;
    }
    const v043::Rect rect{area.offset.x, area.offset.y, area.extent.width, area.extent.height};
    return v043::RectInside(rect, pass_area_width_, pass_area_height_);
  }

  bool ClearInActivePass(const NativeImage& image, VkImageAspectFlags aspects, const VkClearValue& value,
                         const VkRect2D& area) override {
    if (!CanClearInActivePass(image, aspects, area)) {
      return false;
    }
    const int slot = ActivePassSlot(image, aspects);

    if (sky_pending_) {
      EmitSkyPostponed(pass_commands_, kSkyAtPassEnd);
    }
    VkClearAttachment attachment{};
    attachment.aspectMask = aspects;
    attachment.colorAttachment = slot < 4 ? v043::ColorAttachmentIndex(pass_formats_, uint32_t(slot)) : 0;
    attachment.clearValue = value;
    VkClearRect rectangle{};
    rectangle.rect = area;
    rectangle.baseArrayLayer = 0;
    rectangle.layerCount = 1;
    dfn_.vkCmdClearAttachments(pass_commands_, 1, &attachment, 1, &rectangle);
    if (slot == 4) {
      pass_ds_written_ = true;
    }
    bo2zm::native::performance::g_guest_clears_in_open_pass.fetch_add(1, std::memory_order_relaxed);
    return true;
  }


  void ForgetView(VkImageView view) override {
    if (view == VK_NULL_HANDLE || !REXCVAR_GET(bo2zm_native_framebuffers_forget_views)) {
      return;
    }
    const bool a_pair = framebuffers_views_.size() == framebuffers_.size();
    if (!fb_retire_ && (pass_active_ || !a_pair)) {
      fb_retire_ = true;
      REXLOG_ERROR("[native] C6 framebuffers: MISMATCH when destroying view {:016X}: {}. For the rest of "
                   "the session the affected framebuffers are retired without destroying them (they are destroyed on "
                   "close)",
                   uint64_t(reinterpret_cast<uintptr_t>(view)),
                   pass_active_ ? "a pass is open (whoever destroys the view has not submitted the work)"
                                : "the cache and its list of views are out of step (someone creates framebuffers "
                                  "another way)");
    }
    uint32_t outside = 0;
    if (!a_pair) {

      for (auto& [key, framebuffer] : framebuffers_) {
        fb_retired_.push_back(framebuffer);
      }
      outside = uint32_t(framebuffers_.size());
      framebuffers_.clear();
      framebuffers_views_.clear();
    } else {
      for (auto it = framebuffers_views_.begin(); it != framebuffers_views_.end();) {
        if (std::find(it->second.begin(), it->second.end(), view) == it->second.end()) {
          ++it;
          continue;
        }
        const auto fb = framebuffers_.find(it->first);
        if (fb != framebuffers_.end()) {
          if (fb_retire_) {
            fb_retired_.push_back(fb->second);
          } else {
            dfn_.vkDestroyFramebuffer(device_, fb->second, nullptr);
          }
          framebuffers_.erase(fb);
          ++outside;
        }
        it = framebuffers_views_.erase(it);
      }
    }
    ++fb_views_forgotten_;
    fb_forgotten_ += outside;
    if (fb_views_forgotten_ <= 16 || (fb_views_forgotten_ & 63) == 0) {
      BO2ZM_REPORT_RING("[native] C6 framebuffers: view {:016X} destroyed: {} framebuffers {} with it; {} "
                           "remain in the cache ({} views and {} framebuffers since boot; {} retired)",
                           uint64_t(reinterpret_cast<uintptr_t>(view)), outside,
                           fb_retire_ ? "retired (not destroyed)" : "destroyed", framebuffers_.size(),
                           fb_views_forgotten_, fb_forgotten_, fb_retired_.size());
    }
  }

  void ForgetImage(VkImage image) override {
    ++invalidations_retired_;
    const auto index = views_per_image_.find(image);
    if (index == views_per_image_.end()) {
      return;
    }
    for (uint64_t key : index->second) {
      const auto it = views_.find(key);
      if (it == views_.end() || it->second.image != image) {
        continue;
      }
      RetireSlot(it->second.heap, it->second.slot);
      WriteImage(it->second.heap, it->second.slot, empty_[it->second.heap].view);
      heaps_[it->second.heap].free.push_back(it->second.slot);
      dfn_.vkDestroyImageView(device_, it->second.view, nullptr);
      views_.erase(it);
    }
    views_per_image_.erase(index);
  }

  DrawStats Stats() const override {
    DrawStats e;
    e.drawn = drawn_;
    e.rejected = rejected_;
    e.pipelines = pipelines_.size();
    e.textures = textures_.size();
    e.uploads_texture = uploads_texture_;
    e.megs_uploaded = megs_bytes_ >> 20;
    e.megs_textures = bytes_textures_ >> 20;
    e.ms_pipelines = ns_pipelines_ / 1000000;
    e.passes = passes_begun_;
    e.submits_full = submits_full_;
    e.ns_submits_full = ns_submits_full_;
    e.bytes_vertices = bytes_vertices_;
    e.dedupe_hits = dedupe_.hits();
    e.dedupe_bytes = dedupe_.bytes_saved();
    e.dedupe_collisions = dedupe_.collisions();
    e.bytes_indices = bytes_indices_uploaded_;
    e.samplers = samplers_prepared_;
    e.samplers_cache = samplers_cache_ + samplers_cache_fetch_;
    e.ns_passes = ns_passes_;
    e.ns_vertices = ns_vertices_;
    e.inputs_computed = inputs_computed_;
    e.ns_inputs = ns_inputs_;
    e.inputs_reused = inputs_reused_ + inputs_cache_hits_;
    e.passes_per_generation = passes_per_generation_;
    e.passes_per_target = passes_per_target_;
    e.passes_resumed = passes_resumed_;
    e.ns_render_pass = ns_render_pass_;
    e.texels_passes = texels_passes_;
    e.texels_per_category = texels_per_category_;
    e.draws_per_category = draws_per_category_;
    e.draws_ps_useless = draws_ps_useless_;
    e.draws_ps_needed = draws_ps_needed_;
    e.shadows_alpha_active = shadows_alpha_active_;
    e.shadows_alpha_off = shadows_alpha_off_;
    e.triangles_per_category = triangles_per_category_;
    e.passes_per_category = passes_per_category_;
    e.submits = submits_;
    e.submits_ubo = submits_ubo_;
    e.shared_observed = shared_observed_;
    e.shared_changed = shared_changed_;
    e.bytes_repeated_frame = bytes_repeated_frame_;
    e.bytes_equal_earlier = bytes_equal_earlier_;
    e.ns_hash_vertices = ns_hash_vertices_;
    e.causes.assign(causes_.begin(), causes_.end());
    e.stages_ns = stages_ns_;
    e.scene_with_discard = scene_with_discard_;
    e.scene_no_discard = scene_no_discard_;
    e.drawn_timed = drawn_timed_;
    e.vegetation_soon = draws_vegetation_soon_;
    std::sort(e.causes.begin(), e.causes.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return e;
  }

 private:
  bool Reject(uint32_t cause, const char* text) {
    ++rejected_;
    ++causes_[cause];
    last_cause_ = cause;
    Warn(cause, text);
    return false;
  }

  void Warn(uint32_t cause, const char* text) {
    if (warned_.insert(cause).second) {
      REXLOG_WARN("[native] C6: {} (cause {})", text, cause);
    }
  }


  void DecideBindings() {
    const bool requested = REXCVAR_GET(bo2zm_native_texture_bind_thread);
    bindings_priority_ = std::clamp<int32_t>(REXCVAR_GET(bo2zm_native_texture_bind_thread_priority), 0x2C, 0x3B);
    bindings_phase_ = requested ? kBindingsObserving : kBindingsOff;
    REXLOG_INFO("[native] C3: binding new textures on a separate thread (bo2zm_native_texture_bind_thread) = {}",
                requested ? fmt::format("YES, priority {:#x}; OBSERVING phase: the first {} wait for their bind right "
                                     "away (pool textures only, which is {})",
                                     bindings_priority_, kBindingsACheck,
                                     pool_textures_.IsActive() ? "on" : "OFF: it will not be used")
                       : std::string("no, on the ring thread as always"));
  }

  void TurnOffBindings(const std::string& reason) {
    if (bindings_phase_ == kBindingsOff) {
      return;
    }
    bindings_phase_ = kBindingsOff;
    REXLOG_ERROR("[native] C3: texture bind thread OFF for the rest of the session: {}. New textures are created whole "
                 "on the ring thread again ({} bound on the thread, {} failed, {} views and {} copies postponed)",
                 reason, bindings_thread_total_, bindings_failed_, views_postponed_, copies_postponed_);
  }


  bool EnqueueBinding(VkImage image, VkDeviceMemory guest_memory, VkDeviceSize offset, uint64_t& ticket) {
    if (!bindings_thread_.joinable()) {
      try {
        bindings_thread_ = std::thread([this] { LoopBindings(); });
      } catch (const std::system_error& error) {
        TurnOffBindings(fmt::format("MISMATCH: the thread could not be created ({})", error.what()));
        return false;
      }
    }
    bool warn = false;
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      if (bindings_requested_ - bindings_collected_ >= kQueueBindings) {
        ++bindings_queue_full_;
        return false;
      }
      BindRequest& request = queue_bindings_[bindings_requested_ & (kQueueBindings - 1)];
      request.image = image;
      request.guest_memory = guest_memory;
      request.offset = offset;
      request.result = VK_NOT_READY;
      request.ns = 0;
      ticket = bindings_requested_++;
      warn = bindings_sleeping_;
    }
    if (warn) {
      bindings_cv_.notify_one();
    }
    return true;
  }


  void LoopBindings() {
    rex::thread::set_current_thread_name("BO2 texture binds");
#if REX_PLATFORM_SWITCH
    const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(bindings_priority_));
#else
    const bool priority_ok = true;
#endif
    std::unique_lock<std::mutex> lock(bindings_mutex_);
    bindings_priority_ok_ = priority_ok;
    for (;;) {
      if (bindings_done_ == bindings_requested_) {
        if (bindings_stop_) {
          return;
        }
        if (bindings_waiting_) {
          bindings_done_cv_.notify_one();
        }
        bindings_sleeping_ = true;
        bindings_cv_.wait(lock, [this] { return bindings_stop_ || bindings_done_ != bindings_requested_; });
        bindings_sleeping_ = false;
        continue;
      }

      BindRequest& request = queue_bindings_[bindings_done_ & (kQueueBindings - 1)];
      const VkImage image = request.image;
      const VkDeviceMemory guest_memory = request.guest_memory;
      const VkDeviceSize offset = request.offset;
      lock.unlock();
      const auto before = std::chrono::steady_clock::now();
      const VkResult result = dfn_.vkBindImageMemory(device_, image, guest_memory, offset);
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - before)
                                       .count());
      lock.lock();
      request.result = result;
      request.ns = ns;
      ns_bindings_thread_ += ns;
      ns_binding_worst_ = std::max(ns_binding_worst_, ns);
      ++bindings_done_;
      if (bindings_waiting_) {
        bindings_done_cv_.notify_one();
      }
    }
  }

  void StopBindings() {
    if (!bindings_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      bindings_stop_ = true;
    }
    bindings_cv_.notify_one();
    bindings_thread_.join();
    REXLOG_INFO("[native] C3: texture bind thread stopped ({} bound on the thread, {} failed, {} views and {} copies "
                "postponed, {} slots lost)",
                bindings_thread_total_, bindings_failed_, views_postponed_, copies_postponed_, slots_lost_);
  }


  void WaitBindings(uint64_t goal) {
    std::unique_lock<std::mutex> lock(bindings_mutex_);
    if (bindings_done_ >= goal) {
      return;
    }
    const auto before = std::chrono::steady_clock::now();
    bindings_waiting_ = true;
    if (bindings_sleeping_) {
      bindings_cv_.notify_one();
    }
    int seconds = 0;
    while (!bindings_done_cv_.wait_for(lock, std::chrono::seconds(2),
                                        [this, goal] { return bindings_done_ >= goal; })) {
      seconds += 2;
      bindings_stuck_ = true;
      REXLOG_ERROR("[native] C3: the ring thread has been waiting {} s for the texture binds: target {}, done {}, "
                   "requested {}; the thread is {}",
                   seconds, goal, bindings_done_, bindings_requested_,
                   bindings_sleeping_ ? "ASLEEP (wake-up lost)" : "awake (an ioctl that does not return)");
    }
    bindings_waiting_ = false;
    lock.unlock();
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - before)
                                     .count());
    ns_wait_bindings_total_ += ns;
    ns_wait_bindings_report_ += ns;
    ns_wait_bindings_worst_ = std::max(ns_wait_bindings_worst_, ns);
    ++waits_bindings_report_;
    bo2zm::waits::g_ns_waiting_bindings.fetch_add(ns, std::memory_order_relaxed);
  }


  bool CreateTextureInThread(CachedTexture& texture, VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                          uint32_t depth3d, uint32_t levels) {
    if (bindings_phase_ == kBindingsNoDecide) {
      DecideBindings();
    }

    if (bindings_phase_ == kBindingsOff || !pool_textures_.IsActive() || test_no_memory_every_ > 0) {
      return false;
    }
    const VkImageCreateInfo info = InfoImageTexture(format, width, height, layers, depth3d, levels);
    VkImage image = VK_NULL_HANDLE;
    if (dfn_.vkCreateImage(device_, &info, nullptr, &image) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req{};
    dfn_.vkGetImageMemoryRequirements(device_, image, &req);
    VkDeviceMemory block = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t id_block = 0xFFFFFFFFu;
    if (!pool_textures_.Reserve(req, block, offset, id_block)) {
      dfn_.vkDestroyImage(device_, image, nullptr);
      return false;
    }
    uint64_t ticket = 0;
    if (!EnqueueBinding(image, block, offset, ticket)) {
      dfn_.vkDestroyImage(device_, image, nullptr);
      pool_textures_.Free(id_block);
      return false;
    }
    texture.image.image = image;
    texture.image.guest_memory = VK_NULL_HANDLE;
    texture.image.pool_block = id_block;
    texture.image.width = width;
    texture.image.height = height;
    texture.image.format = format;
    texture.image.prepared = false;
    TextureInFlight flight;
    flight.texture = &texture;
    flight.ticket = ticket;
    flight.image = image;
    flight.format = format;
    flight.width = width;
    flight.height = height;
    flight.layers = layers;
    flight.depth3d = depth3d;
    flight.levels = levels;
    in_flight_.push_back(flight);
    texture.in_flight = uint32_t(in_flight_.size());
    images_in_flight_.insert(image);
    if (bindings_phase_ == kBindingsObserving) {

      CollectBindings(true);
      if (texture.image.image == VK_NULL_HANDLE) {
        return false;
      }
      if (++bindings_checked_ >= kBindingsACheck && bindings_phase_ == kBindingsObserving) {
        bindings_phase_ = kBindingsApplying;
        uint64_t ns_thread = 0;
        bool priority_ok = true;
        {
          std::lock_guard<std::mutex> lock(bindings_mutex_);
          ns_thread = ns_bindings_thread_;
          priority_ok = bindings_priority_ok_;
        }
        REXLOG_INFO("[native] C3: texture bind thread: {} binds with an immediate wait and none failed ({:.0f} us on "
                    "average inside vkBindImageMemory; the ring waited {:.0f} us on average per texture). Moves to the "
                    "APPLYING phase: the wait goes before each submit{}",
                    bindings_checked_, double(ns_thread) / 1e3 / double(bindings_checked_),
                    double(ns_wait_bindings_total_) / 1e3 / double(bindings_checked_),
                    priority_ok ? "" : " (the kernel did NOT accept the thread priority)");
      }
    }
    return true;
  }


  uint32_t SlotViewInFlight(uint64_t key, VkImage image, VkFormat format, uint32_t swizzle,
                              uint16_t swizzle_host, uint32_t heap) {
    View view;
    view.image = image;
    view.heap = heap;
    view.slot = ReserveSlot(heap);
    if (!view.slot) {
      Warn(35, "texture heap full");
      ++slots_full_;
      return 0;
    }
    views_.emplace(key, view);
    views_per_image_[image].push_back(key);
    ViewInFlight postponed;
    postponed.key = key;
    postponed.image = image;
    postponed.format = format;
    postponed.swizzle = swizzle;
    postponed.swizzle_host = swizzle_host;
    postponed.heap = heap;
    postponed.slot = view.slot;
    views_in_flight_.push_back(postponed);
    ++views_postponed_;
    ++views_postponed_report_;
    return view.slot;
  }


  bool PostponeCopy(CachedTexture& texture, VkDeviceSize offset, VkCommandBuffer upload) {
    const size_t index = size_t(texture.in_flight) - 1;
    if (index >= in_flight_.size() || in_flight_[index].texture != &texture || in_flight_[index].copy) {
      TurnOffBindings(fmt::format("MISMATCH: incoherent in-flight texture index ({} of {})", index,
                                in_flight_.size()));
      return false;
    }
    TextureInFlight& flight = in_flight_[index];
    flight.copy = true;
    flight.copy_offset = offset;
    flight.copy_epoch = epoch_upload_;
    flight.copy_commands = upload;
    ++copies_postponed_;
    ++copies_postponed_report_;
    return true;
  }


  void MoveViewsInFlight(VkImage old, VkImage fresh) {
    if (old == fresh) {
      return;
    }
    std::vector<uint64_t> keys;
    for (ViewInFlight& postponed : views_in_flight_) {
      if (postponed.image != old) {
        continue;
      }
      const auto it = views_.find(postponed.key);
      if (it == views_.end()) {
        continue;
      }
      View view = it->second;
      views_.erase(it);
      view.image = fresh;
      postponed.image = fresh;
      if (fresh != VK_NULL_HANDLE) {
        postponed.key = KeyView(fresh, postponed.swizzle, postponed.swizzle_host, postponed.heap);
      }
      if (!views_.emplace(postponed.key, view).second) {
        postponed.key = 0;
        continue;
      }
      keys.push_back(postponed.key);
    }
    views_per_image_.erase(old);
    if (fresh != VK_NULL_HANDLE && !keys.empty()) {
      auto& list = views_per_image_[fresh];
      list.insert(list.end(), keys.begin(), keys.end());
    }
  }


  void CollectBindings(bool with_copies) {


    if (in_flight_.empty() || collecting_bindings_) {
      return;
    }
    collecting_bindings_ = true;
    const auto start = std::chrono::steady_clock::now();
    const uint64_t wait_before = ns_wait_bindings_total_;
    const uint64_t goal = in_flight_.back().ticket + 1;
    WaitBindings(goal);
    if (bindings_stuck_) {
      bindings_stuck_ = false;
      TurnOffBindings("MISMATCH: a wait of more than 2 s for the thread (see the lines above)");
    }
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      for (TextureInFlight& flight : in_flight_) {
        flight.result = queue_bindings_[flight.ticket & (kQueueBindings - 1)].result;
      }
      bindings_collected_ = goal;
    }


    for (TextureInFlight& flight : in_flight_) {
      CachedTexture& texture = *flight.texture;
      images_in_flight_.erase(flight.image);
      texture.in_flight = 0;
      if (flight.result == VK_SUCCESS) {
        ++bindings_thread_total_;
        ++bindings_thread_report_;
        bo2zm::waits::g_textures_bound_thread.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      ++bindings_failed_;
      TurnOffBindings(fmt::format("MISMATCH: vkBindImageMemory returned {} on the thread for a {}x{} texture ({} "
                                "levels, {} layers)",
                                int32_t(flight.result), flight.width, flight.height, flight.levels, flight.layers));
      dfn_.vkDestroyImage(device_, flight.image, nullptr);
      pool_textures_.Free(texture.image.pool_block);
      texture.image.image = VK_NULL_HANDLE;
      texture.image.pool_block = 0xFFFFFFFFu;
      if (!CreateTexture(texture.image, flight.format, flight.width, flight.height, flight.layers, flight.depth3d,
                        flight.levels, false)) {


        texture.image = NativeImage{};
        bytes_textures_ -= std::min(bytes_textures_, texture.bytes);
        texture.bytes = 0;
      }
      MoveViewsInFlight(flight.image, texture.image.image);
    }


    for (const ViewInFlight& postponed : views_in_flight_) {
      const auto it = postponed.key ? views_.find(postponed.key) : views_.end();
      if (it != views_.end() && it->second.image != VK_NULL_HANDLE &&
          CreateViewTexture(it->second.image, postponed.format, postponed.swizzle, postponed.swizzle_host,
                            postponed.heap, it->second.view) == VK_SUCCESS) {
        WriteImage(postponed.heap, postponed.slot, it->second.view);
        continue;
      }
      WriteImage(postponed.heap, postponed.slot, empty_[postponed.heap].view);
      if (it != views_.end()) {
        views_.erase(it);
      }
      ++slots_lost_;
      Warn(34, "could not create a texture's view");
    }
    views_in_flight_.clear();

    VkCommandBuffer upload = VK_NULL_HANDLE;
    for (const TextureInFlight& flight : in_flight_) {
      if (!flight.copy) {
        continue;
      }
      CachedTexture& texture = *flight.texture;
      if (texture.image.image == VK_NULL_HANDLE) {
        continue;
      }
      if (with_copies && upload == VK_NULL_HANDLE) {
        upload = context_->CommandsUpload();
      }
      if (!with_copies || upload == VK_NULL_HANDLE || upload != flight.copy_commands ||
          flight.copy_epoch != epoch_upload_) {
        TurnOffBindings(fmt::format("MISMATCH: a postponed copy can no longer go into its upload buffer (epoch {} "
                                  "versus {}, {})",
                                  flight.copy_epoch, epoch_upload_,
                                  with_copies ? "another upload buffer" : "collected when changing buffer"));
        texture.image.prepared = false;
        texture.next = 0;
        continue;
      }
      if (!texture.image.prepared) {
        Barrier(upload, texture.image.image, texture.layers);
        texture.image.prepared = true;
      }
      RecordCopyTexture(upload, texture, flight.copy_offset);
    }
    in_flight_.clear();
    collecting_bindings_ = false;
    if (!measuring_creation_) {
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
      const uint64_t wait = ns_wait_bindings_total_ - wait_before;
      const uint64_t own = ns > wait ? ns - wait : 0;
      bo2zm::waits::g_ns_create_textures.fetch_add(own, std::memory_order_relaxed);
      ns_create_report_ += own;
    }
  }


  void CheckBindingsOnBufferChange() {
    for (const TextureInFlight& flight : in_flight_) {
      if (flight.copy) {
        CollectBindings(false);
        return;
      }
    }
  }


  struct VolumeReadInfo {
    uint32_t base = 0, format = 0, width = 0, height = 0, depth3d = 0;
    uint64_t reads = 0, bytes = 0, ns = 0, already_prepared = 0;
  };
  std::unordered_map<uint64_t, VolumeReadInfo> volumes_read_;
  std::chrono::steady_clock::time_point report_volumes_{};
  void ReportVolumes() {
    const auto now = std::chrono::steady_clock::now();
    if (now - report_volumes_ < std::chrono::seconds(10)) {
      return;
    }
    report_volumes_ = now;
    if (volume_states_ || volume_no_changes_ || volume_updates_) {
      BO2ZM_REPORT_RING("[native] C3 3D textures by pages (bo2zm_native_incremental_volume), last 10 s: {} new states, "
                           "{} unchanged, {} updated ({} pages, {:.2f} MB uploaded), {:.1f} ms; {} checked against the "
                           "whole read since boot; finding the changed pages: comparing {:.0f} us each ({}), by hash "
                           "{:.0f} us each ({}), by the writer's marks {:.0f} us each ({}; {} also hashed as a check, "
                           "{} pages marked only after the check, {} pages changed without a mark)",
                           volume_states_, volume_no_changes_, volume_updated_report_, volume_pages_,
                           double(volume_bytes_) / 1048576.0, double(volume_ns_) / 1e6, volume_checked_,
                           volume_detects_[0] ? double(volume_detect_ns_[0]) / 1e3 / double(volume_detects_[0]) : 0.0,
                           volume_detects_[0],
                           volume_detects_[1] ? double(volume_detect_ns_[1]) / 1e3 / double(volume_detects_[1]) : 0.0,
                           volume_detects_[1],
                           volume_detects_[2] ? double(volume_detect_ns_[2]) / 1e3 / double(volume_detects_[2]) : 0.0,
                           volume_detects_[2], volume_mark_checked_, volume_mark_late_, volume_mark_misses_);
      volume_states_ = volume_no_changes_ = volume_updated_report_ = 0;
      volume_pages_ = volume_bytes_ = volume_ns_ = 0;
      volume_detect_ns_[0] = volume_detect_ns_[1] = volume_detects_[0] = volume_detects_[1] = 0;
      volume_detect_ns_[2] = volume_detects_[2] = volume_mark_checked_ = volume_mark_misses_ = volume_mark_late_ = 0;
    }
    if (volumes_read_.empty()) {
      return;
    }
    std::vector<VolumeReadInfo> list;
    for (const auto& [key, v] : volumes_read_) {
      list.push_back(v);
    }
    volumes_read_.clear();
    std::sort(list.begin(), list.end(), [](const VolumeReadInfo& a, const VolumeReadInfo& b) { return a.ns > b.ns; });
    std::string text;
    for (size_t i = 0; i < std::min<size_t>(list.size(), 4); ++i) {
      const VolumeReadInfo& v = list[i];
      text += fmt::format("; {:08X} format {} {}x{}x{}: {} reads ({} of an already created one), {:.1f} MB, {:.1f} ms",
                           v.base, v.format, v.width, v.height, v.depth3d, v.reads, v.already_prepared,
                           double(v.bytes) / 1048576.0, double(v.ns) / 1e6);
    }
    BO2ZM_REPORT_RING("[native] C3 3D textures read from memory, last 10 s ({} different){}", list.size(),
                         text);
  }

  void ReportBindings() {
    if (bindings_phase_ == kBindingsNoDecide) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_bindings_ < std::chrono::seconds(10)) {
      return;
    }
    report_bindings_ = now;
    uint64_t ns_thread = 0;
    uint64_t worst_thread = 0;
    uint64_t queue_full = 0;
    bool priority_ok = true;
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      ns_thread = ns_bindings_thread_ - ns_bindings_thread_previous_;
      ns_bindings_thread_previous_ = ns_bindings_thread_;
      worst_thread = ns_binding_worst_;
      ns_binding_worst_ = 0;
      queue_full = bindings_queue_full_ - queue_full_previous_;
      queue_full_previous_ = bindings_queue_full_;
      priority_ok = bindings_priority_ok_;
    }
    const uint64_t created = textures_created_ - created_report_previous_;
    created_report_previous_ = textures_created_;
    const uint64_t slots_full = slots_full_ - slots_full_previous_;
    slots_full_previous_ = slots_full_;
    if (slots_full) {

      BO2ZM_REPORT_RING("[native] C3 texture slots: {} views found no slot in the last 10 s (drawn black); in use: 2D "
                        "{} of {}, 3D {} of {}, cube {} of {}",
                        slots_full, SlotsUsed(0), kCapacityHeap[0], SlotsUsed(1), kCapacityHeap[1], SlotsUsed(2),
                        kCapacityHeap[2]);
    }
    if (created || bindings_thread_report_ || waits_bindings_report_) {
      static constexpr const char* kPhases[] = {"off", "OBSERVING", "APPLYING"};
      BO2ZM_REPORT_RING(
          "[native] C3 new textures, last 10 s: {} created, {} with the bind on the thread; the ring spent "
          "{:.1f} ms creating and waited for the thread {:.1f} ms in {} waits (worst {:.2f} ms); the thread, {:.1f} ms "
          "inside vkBindImageMemory (average {:.0f} us, worst {:.2f} ms); {} views and {} copies postponed; {} by the "
          "usual path with the queue full; phase {}{}",
          created, bindings_thread_report_, double(ns_create_report_) / 1e6, double(ns_wait_bindings_report_) / 1e6,
          waits_bindings_report_, double(ns_wait_bindings_worst_) / 1e6, double(ns_thread) / 1e6,
          bindings_thread_report_ ? double(ns_thread) / 1e3 / double(bindings_thread_report_) : 0.0,
          double(worst_thread) / 1e6, views_postponed_report_, copies_postponed_report_, queue_full,
          kPhases[std::clamp<int32_t>(bindings_phase_, 0, 2)], priority_ok ? "" : " (the kernel did NOT accept the "
                                                                                  "priority)");
    }
    if (bindings_phase_ == kBindingsApplying && bindings_thread_report_ >= 16) {
      reports_no_compensate_ = ns_wait_bindings_report_ > ns_thread ? reports_no_compensate_ + 1 : 0;
      if (reports_no_compensate_ >= 3) {
        TurnOffBindings(fmt::format("not worth it: in 3 reports in a row the ring waited for the thread longer than the "
                                  "thread took to bind (the last one, {:.1f} ms of waiting for {:.1f} ms of binds)",
                                  double(ns_wait_bindings_report_) / 1e6, double(ns_thread) / 1e6));
      }
    }
    bindings_thread_report_ = 0;
    waits_bindings_report_ = 0;
    ns_wait_bindings_report_ = 0;
    ns_wait_bindings_worst_ = 0;
    ns_create_report_ = 0;
    views_postponed_report_ = 0;
    copies_postponed_report_ = 0;
  }


  struct HashRead {
    uint64_t source = 0;
    uint64_t row_bytes = 0;
    size_t target = 0;
    uint32_t pitch_blocks = 0;
    uint32_t ox = 0;
    uint32_t oy = 0;
    uint32_t bx = 0;
    uint32_t by = 0;
    uint32_t bx_host = 0;
    bool tiled = false;
    bool check = false;
  };
  struct HashJob {

    CachedTexture* texture = nullptr;
    uint64_t key = 0;
    uint64_t sequence = 0;
    uint64_t inst_base = 0;
    uint64_t bytes_base = 0;
    uint64_t inst_mips = 0;
    uint64_t bytes_mips = 0;
    size_t bytes_data = 0;
    uint8_t* target = nullptr;
    uint64_t epoch = 0;
    uint32_t read_start = 0;
    uint32_t reads = 0;
    uint32_t bytes_block = 1;
    uint32_t log2_block = 0;
    uint32_t unit_order = 0;
    uint32_t order = 0;
    uint32_t address = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    bool fast = false;
    bool check_order = false;
    bool compare = false;
    bool warning_levels = false;
    bool warning_orders = false;

    int32_t state = 0;
    bool per_ring = false;
    bool order_different = false;
    uint32_t levels_different = 0;
    std::array<uint32_t, 5> different{};
    uint64_t hash_raw = 0;
    uint64_t hash_data = 0;
    uint64_t ns_hash = 0;
    uint64_t ns_rest = 0;
  };
  struct HashComparison {
    uint64_t raw_hash = 0;
    uint64_t data = 0;
    bool annotated = false;
  };
  static constexpr size_t kReadsPerTexture = 6 * 16;
  static constexpr uint32_t kReadsHash = 8192;
  static constexpr uint64_t kQueueHashes = 256;
  static constexpr uint32_t kJobHashPublished = 0xFFFFFFFFu;
  static constexpr int32_t kJobPending = 1;
  static constexpr int32_t kJobInThread = 2;
  static constexpr int32_t kJobInRing = 3;
  static constexpr int32_t kJobDone = 4;
  static constexpr int32_t kHashesNoDecide = -1;
  static constexpr int32_t kHashesOff = 0;
  static constexpr int32_t kHashesObserving = 1;
  static constexpr int32_t kHashesApplying = 2;
  static constexpr uint64_t kHashesACheck = 64;
  static constexpr uint64_t kCheckHashOneOfEvery = 128;

  void DecideHashes() {
    const bool requested = REXCVAR_GET(bo2zm_native_texture_hash_thread);
    hashes_priority_ = std::clamp<int32_t>(REXCVAR_GET(bo2zm_native_texture_hash_thread_priority), 0x2C, 0x3B);
    hashes_core_requested_ = std::clamp<int32_t>(REXCVAR_GET(bo2zm_native_texture_hash_thread_core), -2, 2);
    snapshots_bytes_ = uint64_t(std::clamp<int32_t>(REXCVAR_GET(bo2zm_native_texture_hash_thread_mb), 4, 64))
                          << 20;
    hashes_phase_ = requested ? kHashesObserving : kHashesOff;
    REXLOG_INFO("[native] C3: hashing and untiling of new textures on a separate thread "
                "(bo2zm_native_texture_hash_thread) = {}",
                requested ? fmt::format("YES, priority {:#x}, {} MB of snapshots; OBSERVING phase: the first {} also go "
                                     "by the usual path and are compared",
                                     hashes_priority_, snapshots_bytes_ >> 20, kHashesACheck)
                       : std::string("no, on the ring thread as always"));
  }

  void TurnOffHashes(const std::string& reason) {
    if (hashes_phase_ == kHashesOff) {
      return;
    }
    hashes_phase_ = kHashesOff;
    REXLOG_ERROR("[native] C3: texture hash thread OFF for the rest of the session: {}. New textures are prepared whole "
                 "on the ring thread again ({} done by the thread, {} by the ring, {} compared)",
                 reason, hashes_thread_total_, hashes_ring_total_, hashes_compared_);
  }


  static bool PlanReadHash(std::array<HashRead, kReadsPerTexture>& reads, uint32_t& n,
                                   uint64_t address, bool tiled, uint32_t pitch_blocks, uint64_t row_bytes,
                                   const HostTextureFormat& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                                   uint32_t bx_host, size_t target, uint64_t range_start, uint64_t range_bytes,
                                   uint64_t in_copy) {
    if (!bx || !by) {
      return true;
    }
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t slack =
        tiled ? uint64_t(std::max<int64_t>(OffsetTiled2D(int32_t((ox + bx + 31) & ~31u),
                                                                     int32_t((oy + by + 31) & ~31u), pitch_blocks,
                                                                     log2),
                                             0))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (address + slack > kMemoryPhysical) {
      return false;
    }
    const uint64_t read =
        tiled ? uint64_t(std::max<int64_t>(OffsetTiled2D(int32_t((ox + bx - 1) & ~31u),
                                                                     int32_t((oy + by - 1) & ~31u), pitch_blocks,
                                                                     log2),
                                             0)) +
                      (log2 == 0 ? 0xA00u : log2 == 1 ? 0xC00u : (0x400u << log2))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (n >= reads.size() || address < range_start || address + read > range_start + range_bytes) {
      return false;
    }
    HashRead& l = reads[n++];
    l.source = in_copy + (address - range_start);
    l.row_bytes = row_bytes;
    l.target = target;
    l.pitch_blocks = pitch_blocks;
    l.ox = ox;
    l.oy = oy;
    l.bx = bx;
    l.by = by;
    l.bx_host = bx_host;
    l.tiled = tiled;
    l.check = false;
    return true;
  }


  bool PlanHash(CachedTexture& texture, uint64_t key, uint32_t n_reads, const uint8_t* raw_base,
                     uint64_t extension, const uint8_t* raw_mips, uint64_t extension_mips, const HostTextureFormat& tf,
                     uint32_t order, size_t bytes_data, const std::array<size_t, 16>& offsets,
                     uint32_t address, uint32_t width, uint32_t height, uint32_t format, VkDeviceSize& bytes_upload) {
    comparison_texture_ = nullptr;
    if (hashes_phase_ == kHashesNoDecide) {
      DecideHashes();
    }
    if (hashes_phase_ == kHashesOff || texture.hash_job != 0 ||
        (hashes_phase_ == kHashesObserving &&
         hashes_comparisons_planned_ >= kHashesACheck + hashes_no_compare_)) {
      return false;
    }
    if (!snapshots_) {
      snapshots_.reset(new (std::nothrow) uint8_t[size_t(snapshots_bytes_)]);
      reads_hash_.reset(new (std::nothrow) HashRead[kReadsHash]);
      if (!snapshots_ || !reads_hash_) {
        snapshots_.reset();
        reads_hash_.reset();
        TurnOffHashes(fmt::format("MISMATCH: there is no memory for {} MB of snapshots", snapshots_bytes_ >> 20));
        return false;
      }
    }
    const uint64_t inst_base = (snapshots_used_ + 63) & ~uint64_t(63);
    const uint64_t inst_mips = inst_base + ((extension + 63) & ~uint64_t(63));
    const uint64_t in_queue = hashes_published_ - hashes_collected_ + hashes_planned_pending_;
    if (in_queue >= kQueueHashes || reads_used_ + n_reads > kReadsHash ||
        inst_mips + extension_mips > snapshots_bytes_) {
      ++hashes_no_room_report_;
      return false;
    }
    const bool compare =
        hashes_phase_ == kHashesObserving || (++hashes_new_applying_ % kCheckHashOneOfEvery) == 0;

    const auto before_copy = std::chrono::steady_clock::now();
    std::memcpy(snapshots_.get() + inst_base, raw_base, size_t(extension));
    if (extension_mips) {
      std::memcpy(snapshots_.get() + inst_mips, raw_mips, size_t(extension_mips));
    }
    const uint64_t ns_copy = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - before_copy)
                                           .count());
    snapshots_used_ = inst_mips + extension_mips;
    ns_snapshots_report_ += ns_copy;
    bytes_snapshots_report_ += extension + extension_mips;
    bo2zm::waits::g_ns_hash_snapshot.fetch_add(ns_copy, std::memory_order_relaxed);
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    HashJob t;
    t.texture = &texture;
    t.key = key;
    t.inst_base = inst_base;
    t.bytes_base = extension;
    t.inst_mips = inst_mips;
    t.bytes_mips = extension_mips;
    t.bytes_data = bytes_data;
    t.read_start = reads_used_;
    t.reads = n_reads;
    t.bytes_block = tf.bytes;
    t.log2_block = log2;
    t.unit_order = tf.unit_order;
    t.order = order;
    t.address = address;
    t.width = width;
    t.height = height;
    t.format = format;
    t.fast = tiled_fast_ > 0;
    t.compare = compare;


    for (uint32_t i = 0; i < n_reads; ++i) {
      HashRead l = reads_plan_[i];
      l.source += inst_base;
      if (!compare && l.tiled && t.fast && tf.bytes == (1u << log2)) {
        ++levels_seen_;
        l.check = levels_seen_ <= kLevelsACheck || levels_seen_ % kCheckOneOfEvery == 0;
        if (l.check && ++levels_checked_ == kLevelsACheck) {
          t.warning_levels = true;
        }
      }
      reads_hash_[reads_used_ + i] = l;
    }
    reads_used_ += n_reads;
    if (!compare && t.fast && order != 0 && (tf.unit_order == 2 || tf.unit_order == 4)) {
      ++orders_seen_;
      t.check_order = orders_seen_ <= kLevelsACheck || orders_seen_ % kCheckOneOfEvery == 0;
      if (t.check_order && ++orders_checked_ == kLevelsACheck) {
        t.warning_orders = true;
      }
    }
    ++hashes_planned_report_;
    if (compare) {


      ++hashes_comparisons_planned_;
      ++hashes_comparison_report_;
      const uint64_t sequence = PublishJobHash(t, nullptr);
      comparisons_hash_[sequence & (kQueueHashes - 1)] = HashComparison{};
      comparison_texture_ = &texture;
      comparison_sequence_ = sequence;
      return false;
    }

    hashes_planned_.push_back(t);
    ++hashes_planned_pending_;
    texture.hash_job = uint32_t(hashes_planned_.size());
    texture.sample_valid = false;
    texture.samples_consecutive = 0;
    for (uint32_t n = 0; n < texture.levels; ++n) {
      texture.offset_level[n] = uint32_t(offsets[n]);
    }
    texture.interval = 1;
    texture.next = frame_ + 1;
    texture.hash = 0;
    texture.needs_upload = true;
    textures_a_upload_.push_back(&texture);
    bo2zm::waits::g_textures_uploads.fetch_add(1, std::memory_order_relaxed);
    bo2zm::waits::g_bytes_uploaded.fetch_add(bytes_data, std::memory_order_relaxed);
    bytes_upload += (bytes_data + 3) & ~size_t(3);
    return true;
  }


  uint64_t PublishJobHash(const HashJob& job, uint8_t* target) {
    if (!hashes_thread_.joinable() && !hashes_no_thread_) {
#if REX_PLATFORM_SWITCH
      const int ring = RexSwitchCurrentCore();
#else
      const int ring = -1;
#endif
      hashes_core_ = hashes_core_requested_ != -2 ? hashes_core_requested_ : ring == 2 ? 1 : 2;
      try {
        hashes_thread_ = std::thread([this] { LoopHashes(); });
        REXLOG_INFO("[native] C3: texture hash thread created: preferred core {} (the ring runs on {}), priority {:#x}",
                    hashes_core_, ring, hashes_priority_);
      } catch (const std::system_error& error) {
        hashes_no_thread_ = true;
        TurnOffHashes(fmt::format("MISMATCH: the thread could not be created ({})", error.what()));
      }
    }
    bool warn = false;
    uint64_t sequence = 0;
    {
      std::lock_guard<std::mutex> lock(hashes_mutex_);
      sequence = hashes_published_;
      HashJob& cell = queue_hashes_[sequence & (kQueueHashes - 1)];
      cell = job;
      cell.sequence = sequence;
      cell.target = target;
      cell.epoch = epoch_upload_;
      cell.state = kJobPending;
      ++hashes_published_;
      warn = hashes_sleeping_;
    }
    if (warn) {
      hashes_cv_.notify_one();
    }
    return sequence;
  }


  size_t BytesHashPlanned(const CachedTexture& texture) const {
    const size_t index = size_t(texture.hash_job) - 1;
    return index < hashes_planned_.size() && hashes_planned_[index].texture == &texture
               ? hashes_planned_[index].bytes_data
               : size_t(texture.bytes);
  }


  void PublishHash(CachedTexture& texture, uint8_t* target, size_t bytes) {
    const size_t index = size_t(texture.hash_job) - 1;
    if (index >= hashes_planned_.size() || hashes_planned_[index].texture != &texture) {


      std::memset(target, 0, bytes);
      texture.hash_job = 0;
      texture.hash_raw = 0;
      texture.next = 0;
      TurnOffHashes(fmt::format("MISMATCH: incoherent planned job ({} of {})", index,
                                hashes_planned_.size()));
      return;
    }
    PublishJobHash(hashes_planned_[index], target);
    hashes_planned_[index].texture = nullptr;
    --hashes_planned_pending_;
    texture.hash_job = kJobHashPublished;
  }


  void DiscardHashesPlanned() {
    if (!hashes_planned_pending_) {
      return;
    }
    for (HashJob& t : hashes_planned_) {
      if (t.texture) {
        t.texture->hash_job = 0;
        t.texture = nullptr;
        ++hashes_discarded_;
      }
    }
    hashes_planned_pending_ = 0;
  }


  void AnnotateComparisonHash(const CachedTexture& texture, const std::vector<uint8_t>& data) {
    comparison_texture_ = nullptr;
    if (comparison_sequence_ < hashes_collected_) {
      return;
    }
    HashComparison& c = comparisons_hash_[comparison_sequence_ & (kQueueHashes - 1)];
    c.raw_hash = texture.hash_raw;
    c.data = XXH3_64bits(data.data(), data.size());
    c.annotated = true;
  }


  void ExecuteHash(HashJob& t, std::vector<uint8_t>& data, std::vector<uint8_t>& check_index,
                      bool& fast_off) {
    const auto start = std::chrono::steady_clock::now();
    const uint8_t* const snapshot = snapshots_.get();
    uint64_t hash = XXH3_64bits(snapshot + t.inst_base, size_t(t.bytes_base));
    if (t.bytes_mips) {
      hash = XXH3_64bits_withSeed(snapshot + t.inst_mips, size_t(t.bytes_mips), hash);
    }
    t.hash_raw = hash;
    const auto after_hash = std::chrono::steady_clock::now();
    data.assign(t.bytes_data, 0);
    const uint32_t b = t.bytes_block;
    for (uint32_t i = 0; i < t.reads; ++i) {
      const HashRead& l = reads_hash_[t.read_start + i];
      const uint8_t* const source = snapshot + l.source;
      uint8_t* const target = data.data() + l.target;
      if (!l.tiled) {
        for (uint32_t y = 0; y < l.by; ++y) {
          std::memcpy(target + size_t(y) * l.bx_host * b,
                      source + uint64_t(l.oy + y) * l.row_bytes + uint64_t(l.ox) * b, size_t(l.bx) * b);
        }
        continue;
      }
      if (t.fast && !fast_off && b == (1u << t.log2_block)) {
        switch (t.log2_block) {
          case 0: UntileLevel<0>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 1: UntileLevel<1>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 2: UntileLevel<2>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 3: UntileLevel<3>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          default: UntileLevel<4>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
        }
        if (l.check) {

          check_index.assign(size_t(l.by) * l.bx_host * b, 0);
          for (uint32_t y = 0; y < l.by; ++y) {
            std::memcpy(check_index.data() + size_t(y) * l.bx_host * b, target + size_t(y) * l.bx_host * b,
                        size_t(l.bx) * b);
          }
          ReadTiledLevelUsual(source, l.pitch_blocks, t.log2_block, b, l.ox, l.oy, l.bx, l.by, l.bx_host,
                                    target);
          bool equal = true;
          for (uint32_t y = 0; y < l.by && equal; ++y) {
            equal = std::memcmp(check_index.data() + size_t(y) * l.bx_host * b,
                                  target + size_t(y) * l.bx_host * b, size_t(l.bx) * b) == 0;
          }
          if (!equal) {
            if (!t.levels_different++) {
              t.different = {l.bx, l.by, l.pitch_blocks, l.ox, l.oy};
            }
            fast_off = true;
          }
        }
        continue;
      }
      ReadTiledLevelUsual(source, l.pitch_blocks, t.log2_block, b, l.ox, l.oy, l.bx, l.by, l.bx_host, target);
    }
    const auto order = static_cast<xenos::Endian>(t.order);
    if (t.fast && !fast_off && order != xenos::Endian::kNone && (t.unit_order == 2 || t.unit_order == 4)) {
      if (t.check_order) {
        check_index = data;
      }
      ChangeOrderBytes(data.data(), data.size(), t.unit_order, t.order);
      if (t.check_order) {
        UsualOrder(check_index, t.unit_order, order);
        if (check_index != data) {
          t.order_different = true;
          data.swap(check_index);
          fast_off = true;
        }
      }
    } else {
      UsualOrder(data, t.unit_order, order);
    }
    if (t.compare) {
      t.hash_data = XXH3_64bits(data.data(), data.size());
    }
    if (t.target) {
      std::memcpy(t.target, data.data(), data.size());
    }
    const auto end = std::chrono::steady_clock::now();
    t.ns_hash = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(after_hash - start).count());
    t.ns_rest = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - after_hash).count());
  }


  void LoopHashes() {
    rex::thread::set_current_thread_name("BO2 texture hashes");
#if REX_PLATFORM_SWITCH
    const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(hashes_priority_));
    const bool core_ok = RexSwitchSetCurrentThreadCore(int(hashes_core_));
    const int core = RexSwitchCurrentCore();
#else
    const bool priority_ok = true;
    const bool core_ok = true;
    const int core = -1;
#endif
    std::vector<uint8_t> data;
    std::vector<uint8_t> check_index;
    bool fast_off = false;
    std::unique_lock<std::mutex> lock(hashes_mutex_);
    hashes_priority_ok_ = priority_ok;
    hashes_core_ok_ = core_ok;
    hashes_core_real_ = core;
    for (;;) {
      while (hashes_taken_ != hashes_published_ &&
             queue_hashes_[hashes_taken_ & (kQueueHashes - 1)].state != kJobPending) {
        ++hashes_taken_;
      }
      if (hashes_taken_ == hashes_published_) {
        if (hashes_stop_) {
          return;
        }
        if (hashes_waiting_) {
          hashes_done_cv_.notify_one();
        }
        hashes_sleeping_ = true;
        hashes_cv_.wait(lock, [this] { return hashes_stop_ || hashes_taken_ != hashes_published_; });
        hashes_sleeping_ = false;
        continue;
      }


      HashJob& t = queue_hashes_[hashes_taken_ & (kQueueHashes - 1)];
      t.state = kJobInThread;
      ++hashes_taken_;
      lock.unlock();
      ExecuteHash(t, data, check_index, fast_off);
      lock.lock();
      t.state = kJobDone;
      ++hashes_done_;
      if (hashes_waiting_) {
        hashes_done_cv_.notify_one();
      }
    }
  }


  void CollectHashes() {
    if (collecting_hashes_) {
      return;
    }
    if (hashes_published_ == hashes_collected_) {
      if (!hashes_planned_pending_ && (snapshots_used_ || reads_used_)) {
        snapshots_used_ = 0;
        reads_used_ = 0;
        hashes_planned_.clear();
      }
      return;
    }
    collecting_hashes_ = true;
    uint64_t ns_wait = 0;
    uint64_t ns_ring = 0;
    uint64_t done_ring = 0;
    {
      std::unique_lock<std::mutex> lock(hashes_mutex_);
      int seconds = 0;
      while (hashes_done_ != hashes_published_) {

        HashJob* mine = nullptr;
        for (uint64_t i = hashes_published_; i > hashes_taken_; --i) {
          HashJob& t = queue_hashes_[(i - 1) & (kQueueHashes - 1)];
          if (t.state == kJobPending) {
            t.state = kJobInRing;
            mine = &t;
            break;
          }
        }
        if (mine) {
          lock.unlock();
          const auto before = std::chrono::steady_clock::now();
          ExecuteHash(*mine, hashes_data_ring_, hashes_check_ring_, hashes_fast_off_ring_);
          ns_ring += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - before)
                                    .count());
          ++done_ring;
          lock.lock();
          mine->per_ring = true;
          mine->state = kJobDone;
          ++hashes_done_;
          continue;
        }

        const auto before = std::chrono::steady_clock::now();
        hashes_waiting_ = true;
        if (hashes_sleeping_) {
          hashes_cv_.notify_one();
        }
        const bool ready = hashes_done_cv_.wait_for(lock, std::chrono::seconds(2),
                                                         [this] { return hashes_done_ == hashes_published_; });
        hashes_waiting_ = false;
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - before)
                                         .count());
        ns_wait += ns;
        ns_wait_hashes_worst_ = std::max(ns_wait_hashes_worst_, ns);
        ++waits_hashes_report_;
        if (!ready) {
          seconds += 2;
          hashes_stuck_ = true;
          REXLOG_ERROR("[native] C3: the ring thread has been waiting {} s for the texture hash thread: published {}, "
                       "taken {}, done {}; the thread is {}",
                       seconds, hashes_published_, hashes_taken_, hashes_done_,
                       hashes_sleeping_ ? "ASLEEP (wake-up lost)" : "awake (a job that does not finish)");
        }
      }

      collected_hash_jobs_.clear();
      for (uint64_t s = hashes_collected_; s < hashes_published_; ++s) {
        collected_hash_jobs_.push_back(queue_hashes_[s & (kQueueHashes - 1)]);
      }
      hashes_collected_ = hashes_published_;
    }
    comparison_texture_ = nullptr;
    bool mismatch = false;
    std::string detail;
    uint64_t thread = 0;
    uint64_t ns_thread = 0;
    for (HashJob& t : collected_hash_jobs_) {
      if (t.per_ring) {
        ++hashes_ring_total_;
      } else {
        ++hashes_thread_total_;
        if (!t.compare) {
          ++thread;
          ns_thread += t.ns_hash + t.ns_rest;
          ns_hash_raw_thread_report_ += t.ns_hash;
        }
      }

      if (t.levels_different) {
        REXLOG_ERROR("[native] C3: the fast untiling does NOT match ({}x{} blocks of {} bytes, pitch {}, from {},{}). "
                     "It is switched OFF and the usual one is used.",
                     t.different[0], t.different[1], t.bytes_block, t.different[2], t.different[3], t.different[4]);
        tiled_fast_ = 0;
      } else if (t.warning_levels) {
        REXLOG_INFO("[native] C3: fast untiling: {} levels checked against the usual one, all equal. The fast one "
                    "stays.",
                    kLevelsACheck);
      }
      if (t.order_different) {
        REXLOG_ERROR("[native] C3: the fast byte-order change does NOT match (unit {}, order {}, {} bytes). The fast "
                     "byte order and untiling are switched OFF.",
                     t.unit_order, t.order, t.bytes_data);
        tiled_fast_ = 0;
      } else if (t.warning_orders) {
        REXLOG_INFO("[native] C3: fast byte-order change: {} textures checked against the usual one, all equal.",
                    kLevelsACheck);
      }
      if (t.compare) {
        const HashComparison& c = comparisons_hash_[t.sequence & (kQueueHashes - 1)];
        if (!c.annotated) {
          ++hashes_no_compare_;
          continue;
        }
        ++hashes_compared_;
        if (c.raw_hash != t.hash_raw || c.data != t.hash_data) {
          mismatch = true;
          detail = fmt::format("MISMATCH in texture {:08X} {}x{} format {}: raw hash {:016X} on the ring and {:016X} on "
                                "the thread; data {:016X} and {:016X}",
                                t.address, t.width, t.height, t.format, c.raw_hash, t.hash_raw, c.data,
                                t.hash_data);
        }
        continue;
      }


      const auto it = textures_.find(t.key);
      if (it == textures_.end() || &it->second != t.texture || t.texture->hash_job != kJobHashPublished) {
        mismatch = true;
        detail = fmt::format("MISMATCH: texture {:08X} of a collected job is no longer as it was left",
                              t.address);
        t.texture = nullptr;
        continue;
      }
      if (t.epoch != epoch_upload_) {
        mismatch = true;
        detail = fmt::format("MISMATCH: texture {:08X}'s data went to another upload buffer (epoch {} versus {})",
                              t.address, t.epoch, epoch_upload_);
      }
      t.texture->hash_raw = t.hash_raw;
      t.texture->hash_job = 0;
      AnnotateContentTexture(*t.texture, t.key);
    }
    if (mismatch) {
      hashes_suspicious_ = true;
      TurnOffHashes(detail);
    }
    if (hashes_stuck_) {
      hashes_stuck_ = false;
      TurnOffHashes("MISMATCH: a wait of more than 2 s for the hash thread (see the lines above)");
    }
    if (hashes_suspicious_) {


      for (const HashJob& t : collected_hash_jobs_) {
        if (!t.compare && t.texture) {
          t.texture->image.prepared = false;
          t.texture->next = 0;
          t.texture->hash_job = 0;
          ++hashes_reuploads_;
        }
      }
    } else if (hashes_phase_ == kHashesObserving && hashes_compared_ >= kHashesACheck) {
      hashes_phase_ = kHashesApplying;
      bool priority_ok = true;
      bool core_ok = true;
      int core = -1;
      {
        std::lock_guard<std::mutex> lock(hashes_mutex_);
        priority_ok = hashes_priority_ok_;
        core_ok = hashes_core_ok_;
        core = hashes_core_real_;
      }
      REXLOG_INFO("[native] C3: texture hash thread: {} new textures done by both paths, all equal (raw hash and data; "
                  "{} could not be compared). Moves to the APPLYING phase: the thread prepares them and 1 in {} is "
                  "still compared. The thread runs on core {}{}{}",
                  hashes_compared_, hashes_no_compare_, kCheckHashOneOfEvery, core,
                  core_ok ? "" : " (the kernel did NOT accept the preferred core)",
                  priority_ok ? "" : " (the kernel did NOT accept the priority)");
    }
    hashes_thread_report_ += thread;
    hashes_ring_report_ += done_ring;
    ns_hashes_thread_report_ += ns_thread;
    ns_hashes_ring_report_ += ns_ring;
    ns_wait_hashes_report_ += ns_wait;

    bo2zm::waits::g_textures_hash_thread.fetch_add(thread, std::memory_order_relaxed);
    bo2zm::waits::g_ns_hash_thread.fetch_add(ns_thread, std::memory_order_relaxed);
    bo2zm::waits::g_textures_hash_ring.fetch_add(done_ring, std::memory_order_relaxed);
    bo2zm::waits::g_ns_hash_ring.fetch_add(ns_ring, std::memory_order_relaxed);
    bo2zm::waits::g_ns_waiting_hashes.fetch_add(ns_wait, std::memory_order_relaxed);
    if (!hashes_planned_pending_) {
      snapshots_used_ = 0;
      reads_used_ = 0;
      hashes_planned_.clear();
    }
    collecting_hashes_ = false;
  }


  void CheckHashesOnBufferChange() {
    if (hashes_published_ == hashes_collected_) {
      return;
    }
    const uint64_t no_collect = hashes_published_ - hashes_collected_;
    hashes_suspicious_ = true;
    CollectHashes();
    TurnOffHashes(fmt::format("MISMATCH: {} hash thread jobs not collected when the upload buffer changed",
                              no_collect));
  }

  void StopHashes() {
    if (!hashes_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(hashes_mutex_);
      hashes_stop_ = true;
    }
    hashes_cv_.notify_one();
    hashes_thread_.join();
    REXLOG_INFO("[native] C3: texture hash thread stopped ({} textures done by the thread, {} by the ring, {} compared, "
                "{} discarded, {} uploaded again)",
                hashes_thread_total_, hashes_ring_total_, hashes_compared_, hashes_discarded_,
                hashes_reuploads_);
  }


  void ReportHashes() {
    if (hashes_phase_ == kHashesNoDecide) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_hashes_ < std::chrono::seconds(10)) {
      return;
    }
    report_hashes_ = now;
    bool priority_ok = true;
    int core = -1;
    {
      std::lock_guard<std::mutex> lock(hashes_mutex_);
      priority_ok = hashes_priority_ok_;
      core = hashes_core_real_;
    }
    if (hashes_planned_report_ || hashes_thread_report_ || hashes_ring_report_ || hashes_no_room_report_) {
      static constexpr const char* kPhases[] = {"off", "OBSERVING", "APPLYING"};
      BO2ZM_REPORT_RING(
          "[native] C3 new texture hashes, last 10 s: {} planned ({} for comparison); the thread prepared "
          "{} in {:.1f} ms (hash {:.1f} ms) and the ring {} at submit in {:.1f} ms; the ring copied {:.1f} MB of "
          "snapshots in {:.1f} ms and waited for the thread {:.1f} ms in {} waits (worst {:.2f} ms); {} by the usual "
          "path without room; {} compared since the start ({} could not be); thread on core {}, priority {:#x}{}; phase "
          "{}",
          hashes_planned_report_, hashes_comparison_report_, hashes_thread_report_,
          double(ns_hashes_thread_report_) / 1e6, double(ns_hash_raw_thread_report_) / 1e6,
          hashes_ring_report_, double(ns_hashes_ring_report_) / 1e6,
          double(bytes_snapshots_report_) / 1048576.0, double(ns_snapshots_report_) / 1e6,
          double(ns_wait_hashes_report_) / 1e6, waits_hashes_report_, double(ns_wait_hashes_worst_) / 1e6,
          hashes_no_room_report_, hashes_compared_, hashes_no_compare_, core, hashes_priority_,
          priority_ok ? "" : " (the kernel did NOT accept the priority)", kPhases[std::clamp<int32_t>(hashes_phase_, 0, 2)]);
    }
    if (hashes_phase_ == kHashesApplying && hashes_thread_report_ + hashes_ring_report_ >= 16) {
      const bool compensates = ns_hashes_thread_report_ > ns_snapshots_report_ + ns_wait_hashes_report_;
      reports_no_compensate_hashes_ = compensates ? 0 : reports_no_compensate_hashes_ + 1;
      if (reports_no_compensate_hashes_ >= 3) {
        TurnOffHashes(fmt::format("not worth it: in 3 reports in a row the thread took less off the ring than the ring "
                                  "spent copying and waiting for it (the last one: thread {:.1f} ms, copies {:.1f} ms, "
                                  "wait {:.1f} ms)",
                                  double(ns_hashes_thread_report_) / 1e6, double(ns_snapshots_report_) / 1e6,
                                  double(ns_wait_hashes_report_) / 1e6));
      }
    }
    hashes_planned_report_ = 0;
    hashes_comparison_report_ = 0;
    hashes_thread_report_ = 0;
    hashes_ring_report_ = 0;
    ns_hashes_thread_report_ = 0;
    ns_hash_raw_thread_report_ = 0;
    ns_hashes_ring_report_ = 0;
    ns_snapshots_report_ = 0;
    bytes_snapshots_report_ = 0;
    ns_wait_hashes_report_ = 0;
    ns_wait_hashes_worst_ = 0;
    waits_hashes_report_ = 0;
    hashes_no_room_report_ = 0;
  }


  enum CopyPhase : int32_t { kCopiesNoHelp = 0, kCopiesObserving = 1, kCopiesApplying = 2 };

  void WarnOtherThreadCopies(const char* where) {
    if (!copies_other_thread_warned_) {
      copies_other_thread_warned_ = true;
      REXLOG_ERROR("[native] C6: {} from a thread that is not the one queueing the vertex copies", where);
    }
  }

  bool EnqueueCopy(const CopyJob& job) {
    if (copies_no_thread_) {

      ++copies_inf_.in_line;
      copies_inf_.bytes_in_line += uint64_t(job.words) * 4;
      return false;
    }
    if (!copies_thread_.joinable()) {
      copies_producer_ = std::this_thread::get_id();
      copies_phase_ = copies_help_requested_ ? kCopiesObserving : kCopiesNoHelp;
#if REX_PLATFORM_SWITCH
      const int ring_core = RexSwitchCurrentCore();
#else
      const int ring_core = -1;
#endif
      try {
        copies_thread_ = std::thread([this, ring_core] {
          rex::thread::set_current_thread_name("BO2 vertex copies");
#if REX_PLATFORM_SWITCH

          const int32_t priority = REXCVAR_GET(bo2zm_native_upload_thread_priority);
          const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));

          const int32_t requested = REXCVAR_GET(bo2zm_native_upload_thread_core);
          const int core = requested == -2 ? (ring_core == 2 ? 1 : 2) : int(requested);
          const bool core_ok = core < 0 || RexSwitchSetCurrentThreadCore(core);
          REXLOG_INFO("[native] C6: vertex copy thread at priority {:#x}{}; preferred core {}{} (the ring runs on {}), "
                      "now on {}",
                      priority, priority_ok ? "" : " (NOT accepted: it keeps the one it was created with)", core,
                      core_ok ? "" : " (NOT accepted)", ring_core, RexSwitchCurrentCore());
#endif
          LoopCopies();
        });
      } catch (const std::system_error& error) {
        copies_no_thread_ = true;
        REXLOG_ERROR("[native] C6: could not create the vertex copy thread ({}): the copies go on the ring",
                     error.what());
        return false;
      }
      REXLOG_INFO("[native] C6: vertex copies on a separate thread (bo2zm_native_upload_thread); the ring copies them "
                  "itself when waiting for them (bo2zm_native_upload_help) = {}",
                  copies_help_requested_
                      ? fmt::format("YES, in chunks of {}; OBSERVING phase during the first {} waits with pending "
                                    "copies",
                                    kCopiesPerChunk, kWaitsCopiesObserving)
                      : std::string("no, waits for the thread as before"));
    } else if (std::this_thread::get_id() != copies_producer_) {
      WarnOtherThreadCopies("EnqueueCopy");
    }
    const size_t written = copies_written_.load(std::memory_order_relaxed);
    if (written - copies_done_.load(std::memory_order_acquire) >= copies_.size()) {
      ++copies_in_line_;
      ++copies_inf_.in_line;
      copies_inf_.bytes_in_line += uint64_t(job.words) * 4;
      return false;
    }
    copies_[written & (copies_.size() - 1)] = job;
    copies_written_.store(written + 1);
    ++copies_inf_.queued;
    copies_inf_.bytes_queued += uint64_t(job.words) * 4;


    if (((written + 1) & (kCopiesPerWarning - 1)) == 0) {
      WakeCopies();
    }
    return true;
  }

  void WakeCopies() {
    bool warn;
    {
      std::lock_guard<std::mutex> lock(copies_mutex_);
      warn = copies_sleeping_;
    }
    if (warn) {
      copies_cv_.notify_one();
    }
  }


  size_t CopiesPending() const override {
    if (!copies_thread_.joinable()) {
      return 0;
    }
    const size_t written = copies_written_.load(std::memory_order_relaxed);
    const size_t done = copies_done_.load(std::memory_order_relaxed);
    return written > done ? written - done : 0;
  }

  void WaitUploads() override {
    if (!copies_thread_.joinable()) {
      return;
    }
    const bool producer = std::this_thread::get_id() == copies_producer_;
    if (!producer) {
      WarnOtherThreadCopies("WaitUploads");
    }
    const size_t goal = copies_written_.load(std::memory_order_relaxed);
    if (copies_done_.load(std::memory_order_acquire) == goal) {
      if (producer) {
        CheckCopies(goal);
      }
      return;
    }
    const auto before = std::chrono::steady_clock::now();

    copies_inf_.no_take += goal - std::min(goal, copies_taken_.load(std::memory_order_acquire));
    uint64_t ns_helping = 0;
    uint64_t helped = 0;
    if (producer && copies_phase_ == kCopiesApplying) {


      uint64_t bytes = 0;
      while (TakeAndCopy(true, helped, bytes)) {
      }
      const auto after_help = std::chrono::steady_clock::now();
      ns_helping = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(after_help - before).count());
      copies_helped_ += helped;
      copies_bytes_helped_ += bytes;
      copies_ns_helping_ += ns_helping;
      copies_inf_.helped += helped;
      copies_inf_.bytes_helped += bytes;
      copies_inf_.ns_helping += ns_helping;
      if (helped) {
        ++copies_inf_.waits_with_help;
      }


      if (copies_done_.load(std::memory_order_acquire) != goal) {
        {
          std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
          if (copies_done_.load(std::memory_order_acquire) != goal) {
            RepairCopies(goal, "with the chunk lock held the thread has nothing in hand and copies are still missing");
          }
        }
        const uint64_t ns_chunk = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - after_help)
                                               .count());
        ++copies_inf_.waits_chunk;
        copies_inf_.ns_chunk += ns_chunk;
        copies_inf_.ns_chunk_worst = std::max(copies_inf_.ns_chunk_worst, ns_chunk);
      }
    } else {
      WaitCopiesAsAlways(goal);
    }
    if (producer) {
      CheckCopies(goal);
      if (copies_phase_ == kCopiesObserving && ++copies_waits_observing_ >= kWaitsCopiesObserving) {
        copies_phase_ = kCopiesApplying;
        REXLOG_INFO("[native] C6: vertex copies: {} waits with pending copies in the OBSERVING phase and {} copies "
                    "checked, all with their mark and the counters adding up. Moves to the APPLYING phase: the ring "
                    "copies what the thread has not taken and waits for the chunk in progress with its lock",
                    copies_waits_observing_, copies_checked_);
      }
    }
    ++waits_copies_;
    const uint64_t ns_total = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - before)
                                           .count());

    const uint64_t ns_wait = ns_total > ns_helping ? ns_total - ns_helping : 0;
    ns_waiting_copies_ += ns_wait;
    ++copies_inf_.waits;
    copies_inf_.ns_wait += ns_wait;
    copies_inf_.ns_wait_worst = std::max(copies_inf_.ns_wait_worst, ns_wait);


    bo2zm::waits::g_ns_waiting_copies.fetch_add(ns_wait, std::memory_order_relaxed);
    bo2zm::waits::g_waits_copies.fetch_add(1, std::memory_order_relaxed);
    bo2zm::waits::g_ns_helping_copies.fetch_add(ns_helping, std::memory_order_relaxed);
    bo2zm::waits::g_copies_helped.fetch_add(helped, std::memory_order_relaxed);
  }


  void WaitCopiesAsAlways(size_t goal) {
    bool a_time = false;
    {
      std::unique_lock<std::mutex> lock(copies_mutex_);
      if (copies_sleeping_) {
        copies_cv_.notify_one();
      }
      copies_waiting_ = true;
      a_time = copies_done_cv_.wait_for(lock, std::chrono::seconds(2), [this, goal] {
        return copies_done_.load(std::memory_order_acquire) == goal;
      });
      copies_waiting_ = false;
    }
    ++copies_inf_.waits_whole;
    if (a_time) {
      return;
    }

    REXLOG_ERROR("[native] C6: the ring thread has been waiting 2 s for the vertex copies: target {}, done {}, taken "
                 "{}, written {}, copied by the thread {}",
                 goal, copies_done_.load(std::memory_order_acquire),
                 copies_taken_.load(std::memory_order_acquire), copies_written_.load(std::memory_order_acquire),
                 copies_progress_.load(std::memory_order_relaxed));
    std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
    if (copies_done_.load(std::memory_order_acquire) != goal) {
      RepairCopies(goal, "2 s waiting for the thread and, with the thread out of its loop, copies are still missing");
    }
  }


  bool TakeAndCopy(bool ring, uint64_t& copies, uint64_t& bytes) {
    const size_t written = copies_written_.load(std::memory_order_acquire);
    size_t taken = copies_taken_.load(std::memory_order_acquire);
    size_t n = 0;
    do {
      if (taken >= written) {
        return false;
      }
      n = std::min<size_t>(written - taken, kCopiesPerChunk);
    } while (!copies_taken_.compare_exchange_weak(taken, taken + n, std::memory_order_acq_rel,
                                                    std::memory_order_acquire));
    const size_t mask = copies_.size() - 1;
    for (size_t i = taken; i < taken + n; ++i) {
      const CopyJob& job = copies_[i & mask];
      CopyVertices(job);
      bytes += uint64_t(job.words) * 4;
      copies_marks_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
      if (!ring) {
        copies_progress_.store(i + 1, std::memory_order_relaxed);
      }
    }
    copies += n;
    copies_done_.fetch_add(n, std::memory_order_acq_rel);
    return true;
  }

  bool HasCopiesNoTake() const {
    return copies_taken_.load(std::memory_order_acquire) != copies_written_.load(std::memory_order_acquire);
  }


  void CheckCopies(size_t goal) {
    if (goal == copies_verified_ || copies_no_thread_) {
      return;
    }
    const size_t mask = copies_.size() - 1;
    const size_t since =
        std::max(copies_verified_, goal > copies_.size() ? goal - copies_.size() : size_t(0));
    size_t no_mark = 0;
    size_t first = 0;
    for (size_t i = since; i < goal; ++i) {
      if (copies_marks_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        if (!no_mark) {
          first = i;
        }
        ++no_mark;
      }
    }
    copies_checked_ += goal - since;
    copies_inf_.checked += goal - since;
    const size_t taken = copies_taken_.load(std::memory_order_acquire);
    const size_t done = copies_done_.load(std::memory_order_acquire);
    if (!no_mark && taken == goal && done == goal) {
      copies_verified_ = goal;
      return;
    }
    const std::string reason = no_mark ? fmt::format("{} of {} copies without their mark (the first, number "
                                                     "{})", no_mark,
                                                       goal - since, first)
                                         : std::string("all with their mark, but the counters do not add up");
    std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
    if (copies_phase_ == kCopiesApplying && taken == goal && done == goal) {


      const size_t redone = RedoNoMark(goal);
      copies_verified_ = goal;
      ++copies_mismatches_;
      copies_phase_ = kCopiesNoHelp;
      REXLOG_ERROR("[native] C6: vertex copies: MISMATCH: {} ({} queued, counters adding up; {} redone on the ring "
                   "before submitting). Ring help OFF for the rest of the session: waits for the thread as before",
                   reason, goal, redone);
      return;
    }
    RepairCopies(goal, reason);
  }


  size_t RedoNoMark(size_t goal) {
    const size_t mask = copies_.size() - 1;
    const size_t since =
        std::max(copies_verified_, goal > copies_.size() ? goal - copies_.size() : size_t(0));
    size_t redone = 0;
    for (size_t i = since; i < goal; ++i) {
      if (copies_marks_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        CopyVertices(copies_[i & mask]);
        copies_marks_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
        ++redone;
      }
    }
    return redone;
  }


  void RepairCopies(size_t goal, const std::string& reason) {
    const size_t taken = copies_taken_.load(std::memory_order_acquire);
    const size_t done = copies_done_.load(std::memory_order_acquire);
    const size_t redone = RedoNoMark(goal);
    copies_taken_.store(goal, std::memory_order_release);
    copies_done_.store(goal, std::memory_order_release);
    copies_verified_ = goal;
    ++copies_mismatches_;
    copies_no_thread_ = true;
    REXLOG_ERROR("[native] C6: vertex copies: MISMATCH: {} (queued {}, taken {}, done {}; {} redone on the ring before "
                 "submitting). Copy thread OFF for the rest of the session: the copies go on the ring, as before build "
                 "90",
                 reason, goal, taken, done, redone);
  }

  const char* NamePhaseCopies() const {
    if (copies_no_thread_) {
      return "NO THREAD (copies on the ring)";
    }
    return copies_phase_ == kCopiesApplying ? "APPLYING" : copies_phase_ == kCopiesObserving ? "OBSERVING" : "NO HELP";
  }


  void ReportCopies() {
    if (!copies_thread_.joinable() && !copies_no_thread_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - copies_report_ < std::chrono::seconds(10)) {
      return;
    }
    copies_report_ = now;
    const uint64_t thread_n = copies_thread_n_.load(std::memory_order_relaxed);
    const uint64_t thread_bytes = copies_thread_bytes_.load(std::memory_order_relaxed);
    const uint64_t d_thread_n = thread_n - copies_thread_n_previous_;
    const uint64_t d_thread_bytes = thread_bytes - copies_thread_bytes_previous_;
    copies_thread_n_previous_ = thread_n;
    copies_thread_bytes_previous_ = thread_bytes;
    const CopyReportFigures& c = copies_inf_;
    if (c.queued || c.in_line || c.waits) {
      BO2ZM_REPORT_RING(
          "[native] C6 vertex copies, last 10 s: {} queued ({:.1f} MB) and {} on the ring without going "
          "through the queue ({:.1f} MB); the thread copied {} ({:.1f} MB) and the ring helped with {} ({:.1f} MB, "
          "{:.1f} ms) in {} of {} waits with pending copies ({} not taken on arrival); waited for the thread's chunk {} "
          "times ({:.2f} ms, worst {:.2f} ms) and the whole thing {} times; pure waiting {:.1f} ms (worst {:.2f} ms); "
          "{} copies checked, {} mismatches; phase {}",
          c.queued, double(c.bytes_queued) / 1048576.0, c.in_line, double(c.bytes_in_line) / 1048576.0,
          d_thread_n, double(d_thread_bytes) / 1048576.0, c.helped, double(c.bytes_helped) / 1048576.0,
          double(c.ns_helping) / 1e6, c.waits_with_help, c.waits, c.no_take, c.waits_chunk,
          double(c.ns_chunk) / 1e6, double(c.ns_chunk_worst) / 1e6, c.waits_whole, double(c.ns_wait) / 1e6,
          double(c.ns_wait_worst) / 1e6, c.checked, copies_mismatches_, NamePhaseCopies());
    }
    copies_inf_ = CopyReportFigures{};
  }


  void ReportCacheFetch() {
    const auto now = std::chrono::steady_clock::now();
    if (now - fetch_report_ < std::chrono::seconds(10)) {
      return;
    }
    fetch_report_ = now;
    {
      const uint64_t figures[4] = {dedupe_indices_hits_, dedupe_indices_converted_, bytes_indices_saved_,
                                   dedupe_indices_checks_};
      uint64_t di[4];
      for (int i = 0; i < 4; ++i) {
        di[i] = figures[i] - dedupe_indices_report_[i];
        dedupe_indices_report_[i] = figures[i];
      }
      BO2ZM_REPORT_RING("[native] C6 upload dedupe across cache flush waits ({}): {} vertex reuses checked by hash, "
                        "{} with different bytes",
                        g_syncs_on_flush_waits.load(std::memory_order_relaxed) ? "OFF: forgets at every wait" : "on",
                        dedupe_checked_, dedupe_.discrepancies());
      BO2ZM_REPORT_RING("[native] C6 index dedupe (bo2zm_native_dedupe_indices {}), last 10 s: {} index ranges reused "
                        "without a scan ({} of them converted again after a submit), {:.1f} MB of indices not "
                        "uploaded; {} reuses checked against a fresh scan ({} in total)",
                        dedupe_indices_on_ ? "on" : (dedupe_indices_off_ ? "OFF after a MISMATCH" : "off"), di[0],
                        di[1], double(di[2]) / 1048576.0, di[3], dedupe_indices_checks_);
    }
    const std::array<uint64_t, 9> now_figures{samplers_cache_fetch_, fetch_misses_collision_, fetch_misses_empty_,
                                               fetch_misses_generation_, fetch_misses_expired_, samplers_cache_,
                                               reuse_hits_, reuse_checks_ + reuse_sampled_ / 1024, reuse_by_value_};
    std::array<uint64_t, 9> d{};
    for (size_t i = 0; i < d.size(); ++i) {
      d[i] = now_figures[i] - fetch_report_previous_[i];
    }
    fetch_report_previous_ = now_figures;
    const uint64_t misses = d[1] + d[2] + d[3] + d[4];
    if (d[0] + misses == 0) {
      return;
    }
    const std::array<uint64_t, 3> invalidations{invalidations_global_, invalidations_retired_,
                                                 invalidations_targeted_};
    std::array<uint64_t, 3> di{};
    for (size_t i = 0; i < di.size(); ++i) {
      di[i] = invalidations[i] - invalidations_previous_[i];
    }
    invalidations_previous_ = invalidations;
    BO2ZM_REPORT_RING(
        "[native] C6 sampler cache by fetch ({} cells), last 10 s: {} register hits, {} table lookups with "
        "{} hits and {} misses ({:.1f} %): {} collisions with another fetch in its cell, {} in an empty cell, {} by "
        "generation and {} expired (texture due for a check); invalidations: {} of the whole cache, {} by retired "
        "images, {} of one address only; texture words reused from the previous draw {} times ({}, ~{} checked; {} by "
        "value after a fetch write)",
        cache_fetch_.size(), d[5], d[0] + misses, d[0], misses,
        100.0 * double(misses) / double(std::max<uint64_t>(d[0] + misses, 1)), d[1], d[2], d[3], d[4], di[0], di[1],
        di[2], d[6], reuse_textures_on_ ? "on" : "off", d[7], d[8]);
  }

  void StopCopies() {
    if (!copies_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(copies_mutex_);
      copies_stop_ = true;
    }
    copies_cv_.notify_one();
    copies_thread_.join();
    REXLOG_INFO("[native] C6: vertex copy thread stopped ({} copies on the ring because the queue was full, {} ring "
                "waits, {:.1f} ms waiting; the ring copied {} while waiting ({:.1f} MB, {:.1f} ms), "
                "{} checked, {} mismatches, phase {})",
                copies_in_line_, waits_copies_, double(ns_waiting_copies_) / 1e6, copies_helped_,
                double(copies_bytes_helped_) / 1048576.0, double(copies_ns_helping_) / 1e6, copies_checked_,
                copies_mismatches_, NamePhaseCopies());
  }

  void LoopCopies() {
    for (;;) {
      if (HasCopiesNoTake()) {


        uint64_t copies = 0;
        uint64_t bytes = 0;
        {
          std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
          while (TakeAndCopy(false, copies, bytes)) {
          }
        }
        copies_thread_n_.fetch_add(copies, std::memory_order_relaxed);
        copies_thread_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        continue;
      }


      std::unique_lock<std::mutex> lock(copies_mutex_);
      if (copies_waiting_) {
        copies_done_cv_.notify_one();
      }
      copies_sleeping_ = true;
      copies_cv_.wait(lock, [this] { return copies_stop_ || HasCopiesNoTake(); });
      copies_sleeping_ = false;


      if (copies_stop_ && !HasCopiesNoTake()) {
        return;
      }
    }
  }


  [[gnu::noinline]] void CopyCompositeTreated(const ShaderInput* ps, const uint32_t* r,
                                                 uint32_t bytes_ps) noexcept {
    if (!ps || !ps->shader || ps->shader->hash != kHashComposite) {
      return;
    }
    const int mode = bo2zm::settings::TreatmentVisual();

    static std::atomic<int> mode_annotated{-1};
    if (mode_annotated.exchange(mode, std::memory_order_relaxed) != mode) {
      const float* k = reinterpret_cast<const float*>(r + kRegConstantsPs);
      REXLOG_INFO("[native] color filter {}: composite with {} bytes of constants. The game sets Coeffs0 ({:.3f}, "
                  "{:.3f}, {:.3f}, {:.3f}), Coeffs1 ({:.3f}, {:.3f}, {:.3f}, {:.3f}), Desaturation {:.3f} and vignette "
                  "{:.3f}",
                  mode, bytes_ps, k[24], k[25], k[26], k[27], k[28], k[29], k[30], k[31], k[8], k[4]);
    }
    if (bytes_ps < kBytesTreatment) {
      return;
    }
    VkDeviceSize offset = 0;
    if (mode == 0 || !Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps,
                               use_ubo_ ? alignment_ubo_ : 16, offset)) {
      return;
    }
    std::memcpy(upload_data_ + offset, r + kRegConstantsPs, bytes_ps);
    ApplyTreatmentVisual(reinterpret_cast<float*>(upload_data_ + offset), mode);
    constants_ps_offset_ = offset;
    constants_ps_generation_ = UINT64_MAX;
  }

  bool Reserve(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize& offset) {
    const VkDeviceSize start = (upload_used_ + alignment - 1) & ~(alignment - 1);
    if (start + bytes > kSizeUpload) {
      offset = 0;
      return false;
    }
    offset = start;
    upload_used_ = start + bytes;
    megs_bytes_ += bytes;
    return true;
  }


  bool ReserveMultiple(VkDeviceSize bytes, VkDeviceSize multiple, VkDeviceSize& offset) {
    const VkDeviceSize start = (upload_used_ + multiple - 1) / multiple * multiple;
    if (start + bytes > kSizeUpload) {
      offset = 0;
      return false;
    }
    offset = start;
    upload_used_ = start + bytes;
    megs_bytes_ += bytes;
    return true;
  }


  static std::filesystem::path PathFilePipelines() {
    return bo2zm::native::cache::PipelineFile();
  }

  static void ReadFileWhole(const std::filesystem::path& path, std::vector<uint8_t>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff bytes = file ? std::streamoff(file.tellg()) : 0;
    if (bytes > 0 && bytes < (std::streamoff(256) << 20)) {
      data.resize(size_t(bytes));
      file.seekg(0);
      if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(bytes))) {
        data.clear();
      }
    }
  }


  void ReadFilePipelines(std::vector<uint8_t>& cache, std::vector<uint8_t>& list) {
    REXLOG_INFO("[native] C6: pipeline cache path {}", PathFilePipelines().string());
    std::vector<uint8_t> file_bytes;
    ReadFileWhole(PathFilePipelines(), file_bytes);
    if (!file_bytes.empty()) {
      uint32_t magic = 0;
      uint32_t version = 0;
      uint64_t bytes_list = 0;
      uint64_t bytes_cache = 0;
      if (file_bytes.size() >= kHeaderFilePipelines) {
        std::memcpy(&magic, file_bytes.data(), 4);
        std::memcpy(&version, file_bytes.data() + 4, 4);
        std::memcpy(&bytes_list, file_bytes.data() + 8, 8);
        std::memcpy(&bytes_cache, file_bytes.data() + 16, 8);
      }
      if (magic != kMagicFilePipelines || version != kVersionFilePipelines ||
          bytes_list > file_bytes.size() || bytes_cache > file_bytes.size() ||
          kHeaderFilePipelines + bytes_list + bytes_cache != file_bytes.size()) {
        REXLOG_WARN("[native] C6: pipeline file from another version or damaged: starting from scratch");
        return;
      }
      const auto start = file_bytes.begin() + kHeaderFilePipelines;
      list.assign(start, start + std::ptrdiff_t(bytes_list));
      cache.assign(start + std::ptrdiff_t(bytes_list), file_bytes.end());
      return;
    }
    const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
    ReadFileWhole(folder / kFileCacheOld, cache);
    ReadFileWhole(folder / kFileListOld, list);
    files_old_ = !cache.empty() || !list.empty();
  }


  void LoadCachePipelines() {
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    const auto create = reinterpret_cast<FnCreatePipelineCache>(
        ifn.vkGetDeviceProcAddr(device_, "vkCreatePipelineCache"));
    data_cache_ = reinterpret_cast<FnPipelineCacheData>(
        ifn.vkGetDeviceProcAddr(device_, "vkGetPipelineCacheData"));
    destroy_cache_ = reinterpret_cast<FnDestroyPipelineCache>(
        ifn.vkGetDeviceProcAddr(device_, "vkDestroyPipelineCache"));
    if (!create || !data_cache_ || !destroy_cache_) {
      REXLOG_WARN("[native] C6: no pipeline cache (the driver does not provide its functions)");
      return;
    }
    const std::filesystem::path path = PathFilePipelines();
    std::vector<uint8_t> data;
    ReadFilePipelines(data, list_read_);
    if (REXCVAR_GET(bo2zm_native_test_fresh_pipelines)) {
      REXLOG_INFO("[native] C6: bo2zm_native_test_fresh_pipelines: empty pipeline cache and prewarm list ({} KB and {} "
                  "KB on disk ignored, and not saved over)",
                  data.size() >> 10, list_read_.size() >> 10);
      data.clear();
      list_read_.clear();
    } else if (REXCVAR_GET(bo2zm_native_test_cold_pipelines)) {
      REXLOG_INFO("[native] C6: bo2zm_native_test_cold_pipelines: empty pipeline cache ({} KB on disk ignored), the "
                  "list kept",
                  data.size() >> 10);
      data.clear();
    }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (create(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
      info.initialDataSize = 0;
      info.pInitialData = nullptr;
      data.clear();
      if (create(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
        cache_pipelines_ = VK_NULL_HANDLE;
      }
    }
    bytes_cache_saved_ = data.size();
    cache_saved_ = std::chrono::steady_clock::now();
    REXLOG_INFO("[native] C6: pipeline cache {} ({} KB read from {}{})",
                cache_pipelines_ != VK_NULL_HANDLE ? "on" : "not available",
                data.size() >> 10, path.string(), files_old_ ? ", from the two earlier files" : "");

    written_cache_ = std::move(data);
    if (files_old_) {

      bytes_cache_saved_ = 0;
      pipelines_no_save_ = 1;
      list_no_save_ = 1;
    }
    LoadListPipelines();
  }


  void SaveCachePipelines(bool in_match = false) {
    cache_saved_ = std::chrono::steady_clock::now();
    if (REXCVAR_GET(bo2zm_native_test_fresh_pipelines)) {
      return;
    }


    std::vector<uint8_t> list;
    if (list_no_save_) {
      list_no_save_ = 0;
      list = SerializeListPipelines();
    }
    const bool want_cache = pipelines_no_save_ != 0 && cache_pipelines_ != VK_NULL_HANDLE;
    pipelines_no_save_ = 0;
    pipelines_seen_ = 0;

    const bool flush = !in_match && writer_list_waiting_.exchange(false, std::memory_order_relaxed);


    std::lock_guard<std::mutex> lock(writer_mutex_);
    if (!want_cache && list.empty() && !writer_resave_ && !flush) {
      return;
    }
    StartWriterCache();
    if (!list.empty()) {
      writer_list_ = std::move(list);
    }
    writer_want_cache_ = writer_want_cache_ || want_cache;
    writer_flush_ = writer_flush_ || flush;
    writer_in_match_ = in_match;
    writer_verified_ = ListVerified();
    writer_pending_ = true;
    writer_warning_.notify_one();
  }


  bool ListVerified() const {
    if (bo2zm::native::precompile::KnownWarm() || precompiled_) {
      return true;
    }
    if (prewarm_stop_.load(std::memory_order_relaxed)) {
      return false;
    }
    if (list_file_.empty()) {
      return prewarm_decided_;
    }
    return prewarm_finished_.load(std::memory_order_acquire) &&
           prewarm_until_.load(std::memory_order_acquire) >= list_file_.size();
  }


  void RequestVouch() {
    vouch_requested_ = true;
    if (REXCVAR_GET(bo2zm_native_test_fresh_pipelines) || cache_pipelines_ == VK_NULL_HANDLE) {
      return;
    }
    std::lock_guard<std::mutex> lock(writer_mutex_);
    StartWriterCache();
    writer_vouch_ = true;
    writer_pending_ = true;
    writer_warning_.notify_one();
  }


  void StartWriterCache() {
    if (!writer_cache_.joinable()) {
      writer_cache_ = std::thread([this] {
        rex::thread::set_current_thread_name("BO2 pipeline cache");
        WriterCacheMain();
      });
    }
  }


  std::vector<uint8_t> TakeCacheData(bool always) {
    size_t bytes = 0;
    if (cache_pipelines_ == VK_NULL_HANDLE || data_cache_(device_, cache_pipelines_, &bytes, nullptr) != VK_SUCCESS ||
        !bytes || (bytes == bytes_cache_saved_ && !always)) {
      return {};
    }
    std::vector<uint8_t> data(bytes);
    const VkResult result = data_cache_(device_, cache_pipelines_, &bytes, data.data());
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !bytes) {
      return {};
    }
    data.resize(bytes);
    bytes_cache_saved_ = bytes;
    return data;
  }


  void WriterCacheMain() {
    std::vector<uint8_t> list;
    std::vector<uint8_t> export_list;
    std::vector<std::string> map_events;
    for (;;) {
      bool want_cache = false, in_match = false, flush = false, verified = false, vouch = false;
      int resave = 0;
      {
        std::unique_lock<std::mutex> lock(writer_mutex_);
        writer_warning_.wait(lock, [this] { return writer_pending_ || writer_stop_; });
        if (!writer_pending_) {
          return;
        }
        list = std::move(writer_list_);
        writer_list_ = {};
        want_cache = writer_want_cache_;
        writer_want_cache_ = false;
        flush = writer_flush_;
        writer_flush_ = false;
        in_match = writer_in_match_;
        resave = writer_resave_;
        writer_resave_ = 0;
        export_list = std::move(writer_export_);
        writer_export_ = {};
        map_events = std::move(writer_map_events_);
        writer_map_events_ = {};
        verified = writer_verified_;
        vouch = writer_vouch_;
        writer_vouch_ = false;
        writer_pending_ = false;
      }


      if (!map_events.empty()) {
        const auto path = bo2zm::native::cache::Folder() / "bo2zm_map_pipeline_v038.jsonl";
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (!ec) {
          const uintmax_t old_bytes = std::filesystem::file_size(path, ec);
          const bool rotate = !ec && old_bytes > 96 * 1024;
          std::ofstream output(path, std::ios::binary | (rotate ? std::ios::trunc : std::ios::app));
          if (output) {
            for (const std::string& event : map_events) {
              output << event << '\n';
            }
            output.flush();
            if (!output) {
              REXLOG_WARN("[native] C6 map pipeline history: could not write {}", path.string());
            }
          } else {
            REXLOG_WARN("[native] C6 map pipeline history: could not open {}", path.string());
          }
        }
        map_events.clear();
      }
      if (!export_list.empty()) {
        const std::filesystem::path path =
            bo2zm::native::cache::UserFolder() / "logs" / "bo2zm_prewarm_list.bin";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(export_list.data()), std::streamsize(export_list.size()));
        REXLOG_INFO("[native] C6 prewarm list written to {} ({} pipelines; bo2zm_native_test_export_list)",
                    path.string(), (export_list.size() - kHeaderList) / sizeof(PipelineRecord));
        export_list.clear();
      }
      const auto before = std::chrono::steady_clock::now();
      std::vector<uint8_t> data;
      if (want_cache || resave) {
        data = TakeCacheData(resave != 0);
      }
      const double ms_taken =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
      const bool grew = !data.empty();


      if (grew) {
        written_cache_ = std::move(data);
      }
      const bool new_list = !list.empty();
      if (new_list) {
        written_list_ = std::move(list);
      }
      if (!grew && !new_list && !flush && !resave) {
        if (vouch) {
          bo2zm::native::precompile::Vouch(PathFilePipelines());
        }
        continue;
      }
      if (in_match && !grew && !resave) {

        writer_list_waiting_.store(true, std::memory_order_relaxed);
        continue;
      }


      bo2zm::native::precompile::Withdraw(PathFilePipelines());
      if (WriteFilePipelines(resave != 1, ms_taken) && (verified || vouch)) {
        bo2zm::native::precompile::Vouch(PathFilePipelines());
      }
    }
  }


  bool WriteFilePipelines(bool in_slices, double ms_taken) {
    const auto before = std::chrono::steady_clock::now();
    const std::filesystem::path path = PathFilePipelines();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      REXLOG_WARN("[native] C6: cannot create pipeline cache folder {}: {}", path.parent_path().string(),
                  error.message());
      return false;
    }
    std::filesystem::path temporal = path;
    temporal += ".tmp";
    {
      const uint32_t magic = kMagicFilePipelines;
      const uint32_t version = kVersionFilePipelines;
      const uint64_t bytes_list = written_list_.size();
      const uint64_t bytes_cache = written_cache_.size();
      std::ofstream file(temporal, std::ios::binary | std::ios::trunc);
      if (!file) {
        REXLOG_WARN("[native] C6: cannot open pipeline cache {} for writing", temporal.string());
        return false;
      }
      if (!file || !file.write(reinterpret_cast<const char*>(&magic), 4) ||
          !file.write(reinterpret_cast<const char*>(&version), 4) ||
          !file.write(reinterpret_cast<const char*>(&bytes_list), 8) ||
          !file.write(reinterpret_cast<const char*>(&bytes_cache), 8) ||
          !WriteInSlices(file, written_list_.data(), bytes_list, in_slices) ||
          !WriteInSlices(file, written_cache_.data(), bytes_cache, in_slices)) {
        REXLOG_WARN("[native] C6: failed writing pipeline cache {}", temporal.string());
        return false;
      }
      file.flush();
      if (!file) {
        REXLOG_WARN("[native] C6: failed flushing pipeline cache {}", temporal.string());
        return false;
      }
      file.close();
      if (!file) {
        REXLOG_WARN("[native] C6: failed closing pipeline cache {}", temporal.string());
        return false;
      }
    }
    error.clear();
    std::filesystem::rename(temporal, path, error);
    if (error) {
      std::filesystem::remove(path, error);
      std::filesystem::rename(temporal, path, error);
    }
    if (error) {
      REXLOG_WARN("[native] C6: failed replacing pipeline cache {}: {}", path.string(), error.message());
      return false;
    }
    if (!error && files_old_) {
      const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
      std::error_code no_importance;
      std::filesystem::remove(folder / kFileCacheOld, no_importance);
      std::filesystem::remove(folder / kFileListOld, no_importance);
      files_old_ = false;
    }
    if (saved_cache_++ < 8) {
      REXLOG_INFO("[native] C6: pipelines saved ({} KB of cache and {} in the prewarm list, in {} ms {}, from "
                  "their thread{}; the cache taken from the driver there in {:.1f} ms)",
                  written_cache_.size() >> 10,
                  written_list_.size() >= kHeaderList
                      ? (written_list_.size() - kHeaderList) / sizeof(PipelineRecord)
                      : 0,
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before)
                      .count(),
                  in_slices ? "in slices" : "at once", error ? ", could not rename" : "", ms_taken);
    }
    return !error;
  }

  bool WriteInSlices(std::ofstream& file, const uint8_t* data, size_t bytes, bool in_slices) {
    constexpr size_t kSlice = 256 * 1024;
    for (size_t at = 0; at < bytes;) {
      const size_t n = in_slices ? std::min(kSlice, bytes - at) : bytes - at;
      if (!file.write(reinterpret_cast<const char*>(data + at), std::streamsize(n))) {
        return false;
      }
      at += n;
      if (at < bytes && !writer_hurry_.load(std::memory_order_relaxed)) {
        file.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    return true;
  }

  void StopWriterCache() {
    writer_hurry_.store(true, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(writer_mutex_);
      writer_stop_ = true;
      writer_warning_.notify_one();
    }
    if (writer_cache_.joinable()) {
      writer_cache_.join();
    }
  }


  bool CreateUpload() {
    for (UploadBuffer& s : uploads_) {
      const bool created = CreateBufferUpload();
      s = {upload_, upload_memory_, upload_size_real_, upload_data_, upload_address_};
      upload_ = VK_NULL_HANDLE;
      upload_memory_ = VK_NULL_HANDLE;
      upload_data_ = nullptr;
      if (!created) {
        return false;
      }
    }
    const auto& types = vulkan_device_->memory_types();
    REXLOG_INFO("[native] C6: upload buffer in memory type {} ({}, {}; bo2zm_native_upload_memory = {})",
                upload_type_, ((types.host_cached >> upload_type_) & 1) ? "with CPU cache" : "without CPU cache",
                upload_coherent_ ? "coherent, not published" : "published before submitting",
                REXCVAR_GET(bo2zm_native_upload_memory));
    MeasureMemoryUpload();
    CreateSharedSeparate();
    CreateVertexPools();
    UseSlot(0);
    return true;
  }


  struct VertexCacheEntry;

  static constexpr VkDeviceSize kVertexPoolTag = VkDeviceSize(1) << 62;
  static constexpr VkDeviceSize kVertexPoolIndex = VkDeviceSize(1) << 61;
  static constexpr VkDeviceSize kVertexOffsetMask = kVertexPoolIndex - 1;

  static constexpr uint64_t kVertexPoolRest = 8;

  void ResolveVertexBinding(VkDeviceSize tagged, VkBuffer upload, VkBuffer& buffer, VkDeviceSize& offset) const {
    if (tagged & kVertexPoolTag) {
      buffer = vpools_[(tagged & kVertexPoolIndex) ? 1 : 0].buffer;
      offset = tagged & kVertexOffsetMask;
    } else {
      buffer = upload;
      offset = tagged;
    }
  }

  void CreateVertexPools() {
    vcache_ab_ = REXCVAR_GET(bo2zm_native_vertex_cache_ab);
    vcache_sum_ab_ = REXCVAR_GET(bo2zm_native_vertex_cache_sum_ab);
    const VkDeviceSize size = VkDeviceSize(std::clamp(REXCVAR_GET(bo2zm_native_vertex_cache_mb), 4, 256)) << 20;
    for (VertexPool& pool : vpools_) {
      VkBufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      info.size = size;
      info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (dfn_.vkCreateBuffer(device_, &info, nullptr, &pool.buffer) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6 vertex cache: vkCreateBuffer failed; the cache is off");
        return;
      }
      VkMemoryRequirements requirements;
      dfn_.vkGetBufferMemoryRequirements(device_, pool.buffer, &requirements);

      if (!((requirements.memoryTypeBits >> upload_type_) & 1)) {
        REXLOG_WARN("[native] C6 vertex cache: the upload memory type does not fit a vertex buffer; the cache is off");
        return;
      }
      VkMemoryAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reserve.allocationSize = requirements.size;
      reserve.memoryTypeIndex = upload_type_;
      void* mapped = nullptr;
      if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &pool.memory) != VK_SUCCESS ||
          dfn_.vkBindBufferMemory(device_, pool.buffer, pool.memory, 0) != VK_SUCCESS ||
          dfn_.vkMapMemory(device_, pool.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6 vertex cache: no memory for {} MB; the cache is off", size >> 20);
        return;
      }
      pool.data = static_cast<uint8_t*>(mapped);
      pool.size = size;
      pool.size_real = requirements.size;
    }
    vcache_coherent_ = upload_coherent_;
    vcache_ready_ = true;
    vcache_by_writes_ = REXCVAR_GET(bo2zm_native_vertex_cache_by_writes);
    if (vcache_by_writes_) {
      bo2zm::page_writes::Enable(true);
    }
    REXLOG_INFO("[native] C6 vertex cache (bo2zm_native_vertex_cache): two pools of {} MB in memory type {}{}", size >> 20,
                upload_type_, vcache_ab_ ? "; A/B factor 1 in the benchmark" : "");
  }


  bool watch_vertices_ = bo2zm::debug::WatchVertices();
  std::unordered_map<uint32_t, uint64_t> watch_vertex_pages_;
  void WatchVerticesCollect(uint64_t address, uint32_t bytes) {
    constexpr uint64_t kFrom = 3000, kUntil = 3300;
    if (frame_ < kFrom) {
      return;
    }
    if (frame_ >= kUntil) {
      std::vector<uint32_t> pages;
      std::vector<uint64_t> copied;
      uint64_t total = 0;
      for (const auto& [page, b] : watch_vertex_pages_) {
        pages.push_back(page);
        copied.push_back(b);
        total += b;
      }
      REXLOG_INFO("[native] vertex pages read in frames {}-{}: {} ({:.1f} MB), {:.1f} MB copied from them; watching "
                  "who writes them",
                  kFrom, kUntil, pages.size(), double(pages.size()) * 4096.0 / 1e6, double(total) / 1e6);
      bo2zm::debug::WatchPhysicalPages(pages, bo2zm::debug::Watch::kVertices, &copied);
      watch_vertices_ = false;
      return;
    }

    const uint64_t end = address + bytes;
    for (uint64_t at = address; at < end;) {
      const uint64_t page_end = (at & ~uint64_t(4095)) + 4096;
      const uint64_t take = std::min(end, page_end) - at;
      watch_vertex_pages_[uint32_t(at) & 0x1FFFF000u] += take;
      at += take;
    }
  }

  bool VertexCache(const uint8_t* data, uint32_t bytes, xenos::Endian order, uint64_t address,
                   VkDeviceSize alignment, uint64_t& hash, VkDeviceSize& tagged) {
    if (vcache_by_writes_) {
      return VertexCacheByWrites(data, bytes, order, address, alignment, tagged);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const bool by_sum = vcache_sum_;
    if (by_sum) {
      hash = ChangeSum(data, size_t(bytes)) | 1;
    } else if (hash == 0) {
      hash = XXH3_64bits(data, size_t(bytes)) | 1;
    }
    vcache_ns_hash_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count());
    vcache_bytes_hashed_ += bytes;
    const uint64_t key = (address & 0x1FFFFFFF) | (uint64_t(bytes & 0x3FFFFFF) << 29) | (uint64_t(order) << 55) |
                         (uint64_t(1) << 63);
    VertexCacheEntry& e = VertexCacheSlot(key);
    if ((by_sum ? e.sum_pool : e.hash_pool) == hash && e.generation == vpool_generation_[e.pool]) {
      ++vcache_hits_;
      vcache_bytes_hit_ += bytes;
      tagged = kVertexPoolTag | (e.pool ? kVertexPoolIndex : 0) | e.offset;
      GuardVertexCache(data, bytes, order, vpools_[e.pool].data + e.offset);
      return true;
    }
    if (e.hash_seen != hash || e.seen_by_sum != by_sum) {
      e.hash_seen = hash;
      e.seen_by_sum = by_sum;
      ++vcache_changed_;
      return false;
    }

    VkDeviceSize at = 0;
    if (!ReserveVertexPool(bytes, alignment, at)) {
      return false;
    }
    VertexPool& pool = vpools_[vpool_];
    const CopyJob job{data, pool.data + at, bytes / 4, order};
    if (!copies_active_ || !EnqueueCopy(job)) {
      CopyVertices(job);
    }
    pool.dirty_lo = std::min(pool.dirty_lo, at);
    pool.dirty_hi = std::max(pool.dirty_hi, at + bytes);

    e.hash_pool = by_sum ? XXH3_64bits(data, size_t(bytes)) | 1 : hash;
    e.sum_pool = by_sum ? hash : ChangeSum(data, size_t(bytes)) | 1;
    e.pool = vpool_;
    e.offset = at;
    e.generation = vpool_generation_[vpool_];
    ++vcache_filled_;
    vcache_bytes_filled_ += bytes;
    tagged = kVertexPoolTag | (vpool_ ? kVertexPoolIndex : 0) | at;
    return true;
  }


  bool VertexCacheByWrites(const uint8_t* data, uint32_t bytes, xenos::Endian order, uint64_t address,
                           VkDeviceSize alignment, VkDeviceSize& tagged) {
    const uint32_t physical = uint32_t(address) & 0x1FFFFFFFu;
    const uint64_t key = (address & 0x1FFFFFFF) | (uint64_t(bytes & 0x3FFFFFF) << 29) | (uint64_t(order) << 55) |
                         (uint64_t(1) << 63);
    auto t_part = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    VertexCacheEntry& e = VertexCacheSlot(key);
    CutSubstage(45, t_part);
    const uint32_t epoch = bo2zm::page_writes::Epoch();
    const bool unchanged = e.copy_epoch != 0 && e.generation == vpool_generation_[e.pool] &&
                           bo2zm::page_writes::Unchanged(physical, bytes, e.copy_epoch);
    CutSubstage(46, t_part);
    if (unchanged) {
      const bool guard_passed = GuardVertexCacheByWrites(data, bytes, physical, e.sum_pool, e.hits++);
      CutSubstage(47, t_part);
      if (!guard_passed) {
        bo2zm::page_writes::MarkVolatile(physical, bytes);
        e.copy_epoch = 0;
        e.seen_epoch = 0;
        return false;
      }
      ++vcache_hits_;
      vcache_bytes_hit_ += bytes;
      tagged = kVertexPoolTag | (e.pool ? kVertexPoolIndex : 0) | e.offset;
      return true;
    }
    if (e.seen_epoch == 0 || !bo2zm::page_writes::Unchanged(physical, bytes, e.seen_epoch)) {
      e.seen_epoch = epoch;
      e.copy_epoch = 0;
      ++vcache_changed_;
      return false;
    }
    VkDeviceSize at = 0;
    if (!ReserveVertexPool(bytes, alignment, at)) {
      return false;
    }
    VertexPool& pool = vpools_[vpool_];
    const CopyJob job{data, pool.data + at, bytes / 4, order};
    if (!copies_active_ || !EnqueueCopy(job)) {
      CopyVertices(job);
    }
    pool.dirty_lo = std::min(pool.dirty_lo, at);
    pool.dirty_hi = std::max(pool.dirty_hi, at + bytes);
    e.pool = vpool_;
    e.offset = at;
    e.generation = vpool_generation_[vpool_];
    e.copy_epoch = epoch;
    e.hits = 0;
    e.sum_pool = ChangeSum(data, size_t(bytes)) | 1;
    ++vcache_filled_;
    vcache_bytes_filled_ += bytes;
    tagged = kVertexPoolTag | (vpool_ ? kVertexPoolIndex : 0) | at;
    return true;
  }


  struct SkinJobRecord {
    VkDeviceAddress block;
    VkDeviceAddress inputs;
    VkDeviceAddress target;
    uint32_t count, kernel, bones_word, records_byte, vertices_word, bones_guest, bones_lo, unused;
  };
  static_assert(sizeof(SkinJobRecord) == 56, "the shader's job record");
  struct SkinPush {
    VkDeviceAddress jobs;
    VkDeviceAddress groups;
  };
  static constexpr uint32_t kSkinTableJobs = 4096;
  static constexpr uint32_t kSkinTableGroups = 32768;
  static constexpr VkDeviceSize kSkinTableBytes = VkDeviceSize(kSkinTableJobs) * sizeof(SkinJobRecord) +
                                                  VkDeviceSize(kSkinTableGroups) * 8;

  static constexpr VkDeviceSize kSkinPoolBytes = VkDeviceSize(8) << 20;


  bool CreateSkinBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& data,
                        VkDeviceAddress& address) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn_.vkCreateBuffer(device_, &info, nullptr, &buffer) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements;
    dfn_.vkGetBufferMemoryRequirements(device_, buffer, &requirements);
    if (!((requirements.memoryTypeBits >> upload_type_) & 1)) {
      return false;
    }
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    reserve.pNext = &flags;
    reserve.allocationSize = requirements.size;
    reserve.memoryTypeIndex = upload_type_;
    void* mapped = nullptr;
    if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &memory) != VK_SUCCESS ||
        dfn_.vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS ||
        dfn_.vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      return false;
    }
    data = static_cast<uint8_t*>(mapped);
    VkBufferDeviceAddressInfo where{};
    where.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    where.buffer = buffer;
    address = address_buffer_(device_, &where);
    return address != 0;
  }

  void CreateGpuSkin() {


    if (ubo_only_) { bo2zm::gpu_skin::SetRendererReady(false); return; }
    if (!bo2zm::gpu_skin::RendererOn()) {
      return;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = sizeof(kSpirvBo2zmGpuSkin);
    info.pCode = kSpirvBo2zmGpuSkin;
    VkShaderModule module = VK_NULL_HANDLE;
    if (CreateProfileModule(info, &module) != VK_SUCCESS) {
      REXLOG_ERROR("[gpu_skin] the compute shader could not be created: skinning stays on the CPU");
      return;
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPush)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    if (dfn_.vkCreatePipelineLayout(device_, &layout, nullptr, &skin_layout_) == VK_SUCCESS) {
      VkComputePipelineCreateInfo pipeline{};
      pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      pipeline.stage.module = module;
      pipeline.stage.pName = "main";
      pipeline.layout = skin_layout_;
      if (dfn_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &skin_pipeline_) !=
          VK_SUCCESS) {
        skin_pipeline_ = VK_NULL_HANDLE;
      }
    }
    dfn_.vkDestroyShaderModule(device_, module, nullptr);
    const bool buffers =
        skin_pipeline_ != VK_NULL_HANDLE &&
        CreateSkinBuffer(kSkinTableBytes * kSlotsOfJob, skin_table_buffer_, skin_table_memory_, skin_table_data_,
                         skin_table_address_) &&
        CreateSkinBuffer(kSkinPoolBytes * 2, skin_pool_buffer_, skin_pool_memory_, skin_pool_data_,
                         skin_pool_address_);
    skin_on_ = buffers;
    bo2zm::gpu_skin::SetRendererReady(skin_on_);
    REXLOG_INFO("[gpu_skin] renderer: {}", skin_on_ ? "the compute pipeline and its buffers are ready, the hooks may "
                                                      "record jobs"
                                                    : "no compute pipeline or buffers: skinning stays on the CPU");
  }


  VkDeviceAddress SkinCachedInputs(const bo2zm::gpu_skin::Job& job, uint32_t halves, uint32_t& records_words) {
    bo2zm::gpu_skin::RendererCounts& counts = bo2zm::gpu_skin::Counts();
    records_words = ((job.records_guest & 3) + job.count * halves * 2 + 3) / 4;
    uint32_t vertices_physical = 0, records_physical = 0;
    if (!bo2zm::page_writes::g_enabled.load(std::memory_order_relaxed) ||
        !bo2zm::gpu_skin::Physical(job.vertices_guest, vertices_physical) ||
        !bo2zm::gpu_skin::Physical(job.records_guest & ~3u, records_physical)) {
      return 0;
    }
    const uint32_t vertices_bytes = job.count * 24, records_bytes = records_words * 4;
    const uint8_t* records = job.records - (job.records_guest & 3);
    const uint64_t key = uint64_t(job.vertices_guest) | (uint64_t(job.count & 0x3FFFFFFF) << 32) |
                         (uint64_t(job.kernel) << 62);
    auto [it, fresh] = skin_cache_.try_emplace(key);
    SkinCacheEntry& e = it->second;
    if (!fresh && e.records_guest == job.records_guest && e.generation == skin_pools_[e.pool].generation &&
        bo2zm::page_writes::Unchanged(vertices_physical, vertices_bytes, e.epoch) &&
        bo2zm::page_writes::Unchanged(records_physical, records_bytes, e.epoch)) {
      const uint32_t use = e.uses++;
      if ((use >= 2 && (use & 255) != 0) ||
          (ChangeSum(job.vertices, vertices_bytes) ^ ChangeSum(records, records_bytes)) == e.sum) {
        counts.cache_hits.fetch_add(1, std::memory_order_relaxed);
        return skin_pool_address_ + e.pool * kSkinPoolBytes + e.offset;
      }
      counts.guard_fails.fetch_add(1, std::memory_order_relaxed);
    }
    const VkDeviceSize need = (VkDeviceSize(records_bytes) + vertices_bytes + 15) & ~VkDeviceSize(15);
    SkinPool* pool = &skin_pools_[skin_pool_];
    if (pool->used + need > kSkinPoolBytes) {
      SkinPool& other = skin_pools_[skin_pool_ ^ 1];
      if (need > kSkinPoolBytes || frame_ - other.left < kVertexPoolRest) {
        skin_cache_.erase(it);
        return 0;
      }
      pool->left = frame_;
      ++pool->generation;
      skin_pool_ ^= 1;
      pool = &other;
      ++pool->generation;
      pool->used = 0;
    }
    const VkDeviceSize at = pool->used;
    pool->used += need;
    uint8_t* target = skin_pool_data_ + skin_pool_ * kSkinPoolBytes + at;
    CopyVertices(CopyJob{records, target, records_words, xenos::Endian::k8in32});
    CopyVertices(CopyJob{job.vertices, target + records_bytes, job.count * 6, xenos::Endian::k8in32});
    pool->dirty_lo = std::min(pool->dirty_lo, at);
    pool->dirty_hi = std::max(pool->dirty_hi, at + need);
    e = {job.records_guest,
         skin_pool_,
         pool->generation,
         at,
         bo2zm::page_writes::Epoch(),
         0,
         ChangeSum(job.vertices, vertices_bytes) ^ ChangeSum(records, records_bytes)};
    counts.cache_fills.fetch_add(1, std::memory_order_relaxed);
    counts.cache_fill_bytes.fetch_add(need, std::memory_order_relaxed);
    return skin_pool_address_ + skin_pool_ * kSkinPoolBytes + at;
  }

  bool SkinOnGpu(const uint8_t* data, xenos::Endian order, uint32_t bytes, uint64_t address, uint32_t stride,
                 bool single, VkDeviceSize& offset) {
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t lo = uint32_t(address);
    const uint64_t hi = uint64_t(lo) + bytes;
    bo2zm::gpu_skin::Overlapping(lo, bytes, skin_jobs_);
    if (skin_jobs_.empty()) {
      return false;
    }
    bo2zm::gpu_skin::RendererCounts& counts = bo2zm::gpu_skin::Counts();
    static constexpr uint32_t kHalves[3] = {1, 3, 5};
    const char* refused = nullptr;
    VkDeviceSize space = VkDeviceSize(bytes) + stride + 16;
    uint32_t parts = 0, groups = 0;
    if (stride != 32) {
      refused = "stride";
    } else if (order != xenos::Endian::k8in32) {
      refused = "byte order";
    } else {
      for (const auto& job : skin_jobs_) {
        const uint64_t first = std::max<uint64_t>(lo, job->out);
        const uint64_t last = std::min<uint64_t>(hi, job->out + uint64_t(job->count) * 32);
        if ((first - lo) % 32 || (first - job->out) % 32 || (last - first) % 32) {
          refused = "a job not on the range's vertices";
          break;
        }
        const uint64_t n = (last - first) / 32;

        space += 128 + job->bones.size() + n * kHalves[job->kernel] * 2 + 8 + n * 24 + 32 +
                 (job->cpu_written ? n * 32 + 16 : 0);
        parts += 1;
        groups += uint32_t((n + 63) / 64);
      }
    }
    SkinTable& table = skin_tables_[slot_current_];
    if (!refused && (table.jobs + parts > kSkinTableJobs || table.groups + groups > kSkinTableGroups)) {
      refused = "the submission's job table is full";
    }
    if (!refused && upload_used_ + space > kSizeUpload) {
      refused = "no room in the upload buffer";
    }
    if (refused) {
      bo2zm::gpu_skin::Refused(refused, lo, bytes, stride, uint32_t(order));
      counts.refused.fetch_add(1, std::memory_order_relaxed);
      for (const auto& job : skin_jobs_) {
        counts.refused_vertices.fetch_add(job->count, std::memory_order_relaxed);
        bo2zm::gpu_skin::Materialize(*job);
      }
      skin_jobs_.clear();
      return false;
    }
    if (vertices_base_cero_ && single) {
      ReserveMultiple(bytes, stride, offset);
    } else {
      Reserve(bytes, 4, offset);
    }
    const auto gap = [&](uint64_t from, uint64_t to) {
      CopyVertices(CopyJob{data + (from - lo), upload_data_ + offset + (from - lo), uint32_t(to - from) / 4, order});
      counts.gap_bytes.fetch_add(to - from, std::memory_order_relaxed);
    };
    uint8_t* const table_data = skin_table_data_ + slot_current_ * kSkinTableBytes;
    uint64_t cursor = lo, per_use = 0, vertices = 0;
    for (const auto& job : skin_jobs_) {
      const uint64_t first = std::max<uint64_t>(lo, job->out);
      const uint64_t last = std::min<uint64_t>(hi, job->out + uint64_t(job->count) * 32);
      if (first > cursor) {
        gap(cursor, first);
      }
      cursor = std::max(cursor, last);
      const uint32_t v0 = uint32_t((first - job->out) / 32), n = uint32_t((last - first) / 32);
      if (n == 0) {
        continue;
      }
      const uint32_t halves = kHalves[job->kernel];
      const uint32_t bones_bytes = uint32_t(job->bones.size());
      uint32_t records_words = 0;
      VkDeviceAddress inputs = SkinCachedInputs(*job, halves, records_words);

      const uint32_t records_from = job->records_guest + v0 * halves * 2;
      const uint32_t records_word0 = records_from & ~3u;
      const uint32_t part_records_words = (records_from + n * halves * 2 - records_word0 + 3) / 4;
      const uint32_t block_bytes = 128 + bones_bytes + (inputs ? 0 : part_records_words * 4 + n * 24);
      VkDeviceSize block = 0;
      Reserve(block_bytes, 16, block);
      uint8_t* at = upload_data_ + block;
      std::memcpy(at, job->constants, 128);
      CopyVertices(CopyJob{job->bones.data(), at + 128, bones_bytes / 4, xenos::Endian::k8in32});
      SkinJobRecord record{};
      record.block = upload_address_ + block;
      record.target = upload_address_ + offset + (first - lo);
      record.count = n;
      record.kernel = job->kernel;
      record.bones_word = 32;
      record.bones_guest = job->bones_guest;
      record.bones_lo = job->bones_lo;
      if (inputs) {
        record.inputs = inputs;
        record.records_byte = (job->records_guest & 3) + v0 * halves * 2;
        record.vertices_word = records_words + v0 * 6;
      } else {
        CopyVertices(CopyJob{job->records + (int64_t(records_word0) - int64_t(job->records_guest)),
                             at + 128 + bones_bytes, part_records_words, xenos::Endian::k8in32});
        CopyVertices(CopyJob{job->vertices + size_t(v0) * 24, at + 128 + bones_bytes + part_records_words * 4, n * 6,
                             xenos::Endian::k8in32});
        record.inputs = upload_address_ + block + 128 + bones_bytes;
        record.records_byte = records_from & 3;
        record.vertices_word = part_records_words;
        counts.uncached.fetch_add(1, std::memory_order_relaxed);
      }
      std::memcpy(table_data + VkDeviceSize(table.jobs) * sizeof(SkinJobRecord), &record, sizeof(record));
      uint8_t* const group_data = table_data + VkDeviceSize(kSkinTableJobs) * sizeof(SkinJobRecord);
      for (uint32_t g = 0; g < n; g += 64) {
        const uint32_t entry[2] = {table.jobs, g};
        std::memcpy(group_data + VkDeviceSize(table.groups++) * 8, entry, 8);
      }
      ++table.jobs;
      per_use += block_bytes;
      vertices += n;
      counts.dispatches.fetch_add(1, std::memory_order_relaxed);
      if (job->cpu_written) {

        VkDeviceSize cpu = 0;
        Reserve(VkDeviceSize(n) * 32, 16, cpu);
        CopyVertices(CopyJob{data + (first - lo), upload_data_ + cpu, n * 8, order});
        skin_checks_.push_back({slot_current_, offset + (first - lo), cpu, n, job->kernel, uint32_t(first)});
      }
    }
    if (cursor < hi) {
      gap(cursor, hi);
    }
    skin_jobs_.clear();
    counts.ranges.fetch_add(1, std::memory_order_relaxed);
    counts.vertices.fetch_add(vertices, std::memory_order_relaxed);
    counts.input_bytes.fetch_add(per_use, std::memory_order_relaxed);
    counts.ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count()),
                        std::memory_order_relaxed);
    return true;
  }


  void DispatchSkinTable() {
    SkinTable& table = skin_tables_[slot_current_];
    if (table.groups == table.dispatched) {
      return;
    }
    const VkCommandBuffer upload = context_->CommandsUpload();
    if (!upload) {
      return;
    }
    if (!upload_coherent_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, skin_table_memory_, upload_type_, 0,
                                                    VK_WHOLE_SIZE, VK_WHOLE_SIZE);
      for (uint32_t p = 0; p < 2; ++p) {
        SkinPool& pool = skin_pools_[p];
        if (pool.dirty_lo < pool.dirty_hi) {
          rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, skin_pool_memory_, upload_type_, 0,
                                                        VK_WHOLE_SIZE, VK_WHOLE_SIZE);
          break;
        }
      }
    }
    for (SkinPool& pool : skin_pools_) {
      pool.dirty_lo = ~VkDeviceSize(0);
      pool.dirty_hi = 0;
    }
    if (!skin_bound_) {
      dfn_.vkCmdBindPipeline(upload, VK_PIPELINE_BIND_POINT_COMPUTE, skin_pipeline_);
      skin_bound_ = true;
    }
    const VkDeviceAddress base = skin_table_address_ + slot_current_ * kSkinTableBytes;
    const SkinPush push{base, base + VkDeviceSize(kSkinTableJobs) * sizeof(SkinJobRecord) +
                                  VkDeviceSize(table.dispatched) * 8};
    dfn_.vkCmdPushConstants(upload, skin_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    dfn_.vkCmdDispatch(upload, table.groups - table.dispatched, 1, 1);
    table.dispatched = table.groups;
    bo2zm::gpu_skin::Counts().submit_dispatches.fetch_add(1, std::memory_order_relaxed);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    dfn_.vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1,
                              &barrier, 0, nullptr, 0, nullptr);
  }


  void CheckGpuSkin(uint32_t index) {
    if (skin_checks_.empty()) {
      return;
    }
    const UploadBuffer& u = uploads_[index];
    bool invalidated = upload_coherent_;
    for (auto it = skin_checks_.begin(); it != skin_checks_.end();) {
      if (it->upload != index) {
        ++it;
        continue;
      }
      if (!invalidated) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = u.guest_memory;
        range.size = VK_WHOLE_SIZE;
        dfn_.vkInvalidateMappedMemoryRanges(device_, 1, &range);
        invalidated = true;
      }
      bo2zm::gpu_skin::Compare(u.data + it->gpu, u.data + it->cpu, it->vertices, it->kernel, it->physical);
      it = skin_checks_.erase(it);
    }
  }


  bool IndexCacheByWrites(const uint8_t* data, uint32_t count, bool flip, uint32_t physical, uint32_t order,
                          uint32_t& min, uint32_t& max, VkDeviceSize& tagged) {
    const uint32_t bytes = count * 2;
    const uint64_t key = physical | (uint64_t(count & 0x3FFFFFF) << 29) | (uint64_t(order & 3) << 55) |
                         (uint64_t(1) << 62) | (uint64_t(1) << 63);
    auto t_part = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    VertexCacheEntry& e = VertexCacheSlot(key);
    CutSubstage(45, t_part);
    const uint32_t epoch = bo2zm::page_writes::Epoch();
    const bool unchanged = e.copy_epoch != 0 && e.generation == vpool_generation_[e.pool] &&
                           bo2zm::page_writes::Unchanged(physical, bytes, e.copy_epoch);
    CutSubstage(46, t_part);
    if (unchanged) {
      const bool guard_passed = GuardIndexCache(data, bytes, physical, e.sum_pool, e.hits++);
      CutSubstage(47, t_part);
      if (!guard_passed) {
        bo2zm::page_writes::MarkVolatile(physical, bytes);
        e.copy_epoch = 0;
        e.seen_epoch = 0;
        return false;
      }
      min = e.min;
      max = e.max;
      ++icache_hits_;
      icache_bytes_hit_ += bytes;
      tagged = kVertexPoolTag | (e.pool ? kVertexPoolIndex : 0) | e.offset;
      return true;
    }
    if (e.seen_epoch == 0 || !bo2zm::page_writes::Unchanged(physical, bytes, e.seen_epoch)) {
      e.seen_epoch = epoch;
      e.copy_epoch = 0;
      ++icache_changed_;
      return false;
    }
    VkDeviceSize at = 0;
    if (!ReserveVertexPool(bytes, 4, at)) {
      ++icache_full_;
      return false;
    }
    VertexPool& pool = vpools_[vpool_];
    uint32_t mn = 0;
    uint32_t mx = 0;
    Indices16(data, count, reinterpret_cast<uint16_t*>(pool.data + at), flip, mn, mx);
    pool.dirty_lo = std::min(pool.dirty_lo, at);
    pool.dirty_hi = std::max(pool.dirty_hi, at + bytes);
    e.pool = vpool_;
    e.offset = at;
    e.generation = vpool_generation_[vpool_];
    e.copy_epoch = epoch;
    e.hits = 0;
    e.sum_pool = ChangeSum(data, size_t(bytes)) | 1;
    e.min = uint16_t(mn);
    e.max = uint16_t(mx);
    min = mn;
    max = mx;
    ++icache_filled_;
    icache_bytes_filled_ += bytes;
    tagged = kVertexPoolTag | (vpool_ ? kVertexPoolIndex : 0) | at;
    return true;
  }

  bool GuardIndexCache(const uint8_t* data, uint32_t bytes, uint32_t physical, uint64_t expected, uint32_t hit) {
    const uint64_t n = ++icache_guard_seen_;
    if (n > 2000 && hit >= 2 && (hit & 63) != 0) {
      return true;
    }
    ++icache_checked_;
    if ((ChangeSum(data, size_t(bytes)) | 1) == expected) {
      return true;
    }
    if (++icache_unstamped_ <= 16) {
      REXLOG_WARN("[native] C6 index cache by writes: physical {:08X}+{} changed without a write stamp (a writer "
                  "that does not stamp); its pages now count as written every frame",
                  physical, bytes);
    }
    ++icache_unstamped_interval_;
    return false;
  }

  void ReportIndexCache() {
    if (icache_ <= 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < icache_next_) {
      return;
    }
    const bool first = icache_next_ == std::chrono::steady_clock::time_point{};
    icache_next_ = now + std::chrono::seconds(10);
    if (!first) {
      BO2ZM_REPORT_RING("[native] C6 index cache by write stamps, last 10 s: {} hits ({:.1f} MB not scanned or "
                        "uploaded), {} new copies in the pools ({:.1f} MB), {} changed or new ranges scanned as usual, "
                        "{} without room; {} hits checked, {} changed without a stamp ({} since start)",
                        icache_hits_, double(icache_bytes_hit_) / 1048576.0, icache_filled_,
                        double(icache_bytes_filled_) / 1048576.0, icache_changed_, icache_full_, icache_checked_,
                        icache_unstamped_interval_, icache_unstamped_);
    }
    icache_hits_ = icache_bytes_hit_ = icache_filled_ = icache_bytes_filled_ = icache_changed_ = icache_full_ = 0;
    icache_checked_ = icache_unstamped_interval_ = 0;
  }


  bool GuardVertexCacheByWrites(const uint8_t* data, uint32_t bytes, uint32_t physical, uint64_t expected,
                                uint32_t hit) {
    const uint64_t n = ++vcache_guard_seen_;
    if (n > 2000 && hit >= 2 && (hit & 63) != 0) {
      return true;
    }
    ++vcache_checked_;
    if ((ChangeSum(data, size_t(bytes)) | 1) == expected) {
      return true;
    }
    if (++vcache_unstamped_ <= 16) {
      REXLOG_WARN("[native] C6 vertex cache by writes: physical {:08X}+{} changed without a write stamp (a writer "
                  "that does not stamp); its pages now count as written every frame",
                  physical, bytes);
    }
    ++vcache_unstamped_interval_;
    return false;
  }


  void PrefetchCacheLines(uint64_t key, uint32_t physical) {
    if (vcache_.empty()) {
      return;
    }
    const uint64_t mixed = (key ^ (key >> 29)) * 0x9E3779B97F4A7C15ull;
    const char* set = reinterpret_cast<const char*>(&vcache_[(mixed >> 40) % kVertexCacheSets * 4]);
    for (size_t at = 0; at < 4 * sizeof(VertexCacheEntry); at += 64) {
      __builtin_prefetch(set + at);
    }
    __builtin_prefetch(&bo2zm::page_writes::g_stamps[(physical & 0x1FFFFFFFu) >> 12]);
  }


  VertexCacheEntry& VertexCacheSlot(uint64_t key) {
    if (vcache_.empty()) {
      vcache_.resize(kVertexCacheSets * 4);
    }
    const uint64_t mixed = (key ^ (key >> 29)) * 0x9E3779B97F4A7C15ull;
    VertexCacheEntry* set = &vcache_[(mixed >> 40) % kVertexCacheSets * 4];
    VertexCacheEntry* oldest = set;
    for (int way = 0; way < 4; ++way) {
      if (set[way].key == key) {
        set[way].stamp = frame_;
        return set[way];
      }
      if (set[way].stamp < oldest->stamp) {
        oldest = &set[way];
      }
    }
    if (oldest->key != 0) {
      ++vcache_evicted_;
    }
    *oldest = VertexCacheEntry{};
    oldest->key = key;
    oldest->stamp = frame_;
    return *oldest;
  }


  bool ReserveVertexPool(uint32_t bytes, VkDeviceSize alignment, VkDeviceSize& at) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      VertexPool& pool = vpools_[vpool_];
      const VkDeviceSize start = (pool.used + alignment - 1) / alignment * alignment;
      if (start + bytes <= pool.size) {
        pool.used = start + bytes;
        at = start;
        return true;
      }
      const uint32_t other = vpool_ ^ 1;
      if (frame_ < vpools_[other].frame_left + kVertexPoolRest) {
        ++vcache_full_;
        return false;
      }
      pool.frame_left = frame_;
      ++vpool_generation_[vpool_];
      vpool_ = other;
      ++vpool_generation_[other];
      vpools_[other].used = 0;
      ++vcache_switches_;
    }
    return false;
  }


  void PublishVertexPools() {
    for (VertexPool& pool : vpools_) {
      if (pool.dirty_hi > pool.dirty_lo) {
        if (!vcache_coherent_) {
          const VkDeviceSize lo = pool.dirty_lo & ~VkDeviceSize(255);
          rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, pool.memory, upload_type_, lo, pool.size_real,
                                                        pool.dirty_hi - lo);
        }
        pool.dirty_lo = ~VkDeviceSize(0);
        pool.dirty_hi = 0;
      }
    }
  }


  void GuardVertexCache(const uint8_t* data, uint32_t bytes, xenos::Endian order, const uint8_t* pool_copy) {
    const uint64_t n = ++vcache_guard_seen_;
    if (n > 2000 && (n & 1023) != 0) {
      return;
    }
    WaitUploads();
    vcache_scratch_.resize(bytes);
    CopyVertices(CopyJob{data, vcache_scratch_.data(), bytes / 4, order});
    ++vcache_checked_;
    if (std::memcmp(vcache_scratch_.data(), pool_copy, bytes) != 0) {
      vcache_off_ = true;
      vcache_active_ = false;
      REXLOG_ERROR("[native] C6 vertex cache: MISMATCH ({} bytes, order {}): the cache is off for this session", bytes,
                   uint32_t(order));
    }
  }

  void ReportVertexCache() {
    if (!vcache_ready_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - vcache_report_ < std::chrono::seconds(10)) {
      return;
    }
    vcache_report_ = now;
    BO2ZM_REPORT_RING("[native] C6 vertex cache (bo2zm_native_vertex_cache, {}), last 10 s: {} hits ({:.1f} MB not "
                      "copied), {} new copies in the pools ({:.1f} MB), {} changed or new ranges copied as usual; "
                      "hashed {:.1f} MB in {:.1f} ms; pool {} at {:.1f} of {} MB, {} switches, {} times full; {} "
                      "entries replaced by newer ranges; {} hits checked against the guest bytes",
                      vcache_active_ ? "on" : (vcache_off_ ? "OFF after a mismatch" : "off"), vcache_hits_,
                      double(vcache_bytes_hit_) / 1048576.0, vcache_filled_, double(vcache_bytes_filled_) / 1048576.0,
                      vcache_changed_, double(vcache_bytes_hashed_) / 1048576.0, double(vcache_ns_hash_) / 1e6, vpool_,
                      double(vpools_[vpool_].used) / 1048576.0, vpools_[vpool_].size >> 20, vcache_switches_,
                      vcache_full_, vcache_evicted_, vcache_checked_);
    if (vcache_by_writes_) {
      BO2ZM_REPORT_RING("[native] C6 vertex cache by write stamps: {} ranges changed without a stamp in 10 s ({} since "
                        "start), now copied every frame",
                        vcache_unstamped_interval_, vcache_unstamped_);
      vcache_unstamped_interval_ = 0;
    }
    vcache_hits_ = vcache_bytes_hit_ = vcache_filled_ = vcache_bytes_filled_ = vcache_changed_ = 0;
    vcache_bytes_hashed_ = vcache_ns_hash_ = vcache_full_ = vcache_evicted_ = 0;
  }


  void CreateSharedSeparate() {
    shared_separate_ = false;
    if (!REXCVAR_GET(bo2zm_native_shared_constants_cache)) {
      REXLOG_INFO("[native] C6: shared constants in the upload buffer (bo2zm_native_shared_constants_cache = false)");
      return;
    }
    const auto& types = vulkan_device_->memory_types();
    for (UploadBuffer& c : shared_bufs_) {
      VkBufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      info.size = kSizeShared;
      info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (dfn_.vkCreateBuffer(device_, &info, nullptr, &c.buffer) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not create the shared constants buffer; they go in the upload buffer");
        return;
      }
      VkMemoryRequirements requirements;
      dfn_.vkGetBufferMemoryRequirements(device_, c.buffer, &requirements);
      uint32_t type = 0;
      if (!rex::bit_scan_forward(requirements.memoryTypeBits & types.host_visible & types.host_cached, &type)) {
        REXLOG_WARN("[native] C6: no visible memory with CPU cache; the shared constants go in the upload buffer");
        return;
      }
      VkMemoryAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reserve.allocationSize = requirements.size;
      reserve.memoryTypeIndex = type;
      if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &c.guest_memory) != VK_SUCCESS ||
          dfn_.vkBindBufferMemory(device_, c.buffer, c.guest_memory, 0) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not allocate the shared constants buffer; they go in the upload buffer");
        return;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, c.guest_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not map the shared constants buffer; they go in the upload buffer");
        return;
      }
      c.data = static_cast<uint8_t*>(mapped);
      c.size_real = requirements.size;
      shared_type_ = type;
    }
    shared_coherent_ = (types.host_coherent >> shared_type_) & 0x1;
    shared_separate_ = true;
    REXLOG_INFO("[native] C6: shared constants separate, in memory type {} (with CPU cache, {}), {} MB per slot",
                shared_type_, shared_coherent_ ? "coherent" : "published before submitting",
                kSizeShared >> 20);
  }

  bool CreateBufferUpload() {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kSizeUpload;

    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn_.vkCreateBuffer(device_, &info, nullptr, &upload_) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements;
    dfn_.vkGetBufferMemoryRequirements(device_, upload_, &requirements);


    const auto& types = vulkan_device_->memory_types();
    upload_type_ = UINT32_MAX;
    const int32_t preference = REXCVAR_GET(bo2zm_native_upload_memory);
    if (preference == 1 || preference == 2) {
      const uint32_t visible = requirements.memoryTypeBits & types.host_visible;
      uint32_t type = 0;
      if (rex::bit_scan_forward(visible & (preference == 1 ? types.host_cached : ~types.host_cached), &type)) {
        upload_type_ = type;
      }
    }
    if (upload_type_ == UINT32_MAX) {
      upload_type_ = rex::ui::vulkan::util::ChooseMemoryType(types, requirements.memoryTypeBits,
                                                             rex::ui::vulkan::util::MemoryPurpose::kUpload);
    }
    if (upload_type_ == UINT32_MAX) {
      return false;
    }
    upload_coherent_ = (vulkan_device_->memory_types().host_coherent >> upload_type_) & 0x1;
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    reserve.pNext = &flags;
    reserve.allocationSize = requirements.size;
    reserve.memoryTypeIndex = upload_type_;
    if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &upload_memory_) != VK_SUCCESS) {
      return false;
    }
    upload_size_real_ = requirements.size;
    if (dfn_.vkBindBufferMemory(device_, upload_, upload_memory_, 0) != VK_SUCCESS) {
      return false;
    }
    void* mapped = nullptr;
    if (dfn_.vkMapMemory(device_, upload_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      return false;
    }
    upload_data_ = static_cast<uint8_t*>(mapped);
    VkBufferDeviceAddressInfo address{};
    address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address.buffer = upload_;
    upload_address_ = address_buffer_(device_, &address);
    return upload_address_ != 0;
  }


  void MeasureMemoryUpload() {
    if (!REXCVAR_GET(bo2zm_native_diag_upload_memory)) {
      return;
    }
    constexpr VkDeviceSize kBytes = VkDeviceSize(8) << 20;
    std::vector<uint8_t> source(static_cast<size_t>(kBytes));
    for (size_t i = 0; i < source.size(); ++i) {
      source[i] = uint8_t((i * 131) ^ (i >> 11));
    }
    const auto& types = vulkan_device_->memory_types();
    std::string report;
    for (uint32_t type = 0; type < 32; ++type) {
      if (!((types.host_visible >> type) & 1)) {
        continue;
      }
      VkMemoryAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reserve.allocationSize = kBytes;
      reserve.memoryTypeIndex = type;
      VkDeviceMemory guest_memory = VK_NULL_HANDLE;
      if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &guest_memory) != VK_SUCCESS) {
        continue;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, guest_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        using Clock = std::chrono::steady_clock;
        std::memcpy(mapped, source.data(), size_t(kBytes));
        const auto before = Clock::now();
        std::memcpy(mapped, source.data(), size_t(kBytes));
        const auto copied = Clock::now();
        const bool coherent = (types.host_coherent >> type) & 1;
        if (!coherent) {
          VkMappedMemoryRange range{};
          range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
          range.memory = guest_memory;
          range.size = VK_WHOLE_SIZE;
          dfn_.vkFlushMappedMemoryRanges(device_, 1, &range);
        }
        const auto published = Clock::now();
        const double seconds = std::chrono::duration<double>(copied - before).count();
        report += fmt::format(
            "{}type {} ({}{}): {:.0f} MB/s{}", report.empty() ? "" : "; ", type,
            ((types.host_cached >> type) & 1) ? "with CPU cache" : "without CPU cache", coherent ? ", coherent" : "",
            seconds > 0 ? 8.0 / seconds : 0.0,
            coherent ? std::string()
                      : fmt::format(", publish {:.2f} ms",
                                    std::chrono::duration<double, std::milli>(published - copied).count()));
        dfn_.vkUnmapMemory(device_, guest_memory);
      }
      dfn_.vkFreeMemory(device_, guest_memory, nullptr);
    }
    REXLOG_INFO("[native] C6: CPU write speed per memory type (8 MB): {}",
                report.empty() ? std::string("no measurable types") : report);
  }

  bool CreateDescriptors() {
    static constexpr VkDescriptorType kTypes[4] = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    const VkDescriptorBindingFlags flags_binding =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    for (uint32_t i = 0; i < 4; ++i) {
      VkDescriptorSetLayoutBinding binding{};
      binding.binding = 0;
      binding.descriptorType = kTypes[i];
      binding.descriptorCount = kCapacityHeap[i];
      binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutBindingFlagsCreateInfo flags{};
      flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
      flags.bindingCount = 1;
      flags.pBindingFlags = &flags_binding;
      VkDescriptorSetLayoutCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.pNext = &flags;
      info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
      info.bindingCount = 1;
      info.pBindings = &binding;
      if (dfn_.vkCreateDescriptorSetLayout(device_, &info, nullptr, &layouts_[i]) != VK_SUCCESS) {
        return false;
      }
      heaps_[i].capacity = kCapacityHeap[i];
      if (i < generation_slot_.size()) {
        generation_slot_[i].assign(kCapacityHeap[i], 0);
      }
    }
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         kCapacityHeap[0] + kCapacityHeap[1] + kCapacityHeap[2]},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kCapacityHeap[3]}};
    VkDescriptorPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    info_pool.maxSets = 4;
    info_pool.poolSizeCount = 2;
    info_pool.pPoolSizes = sizes;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool, nullptr, &pool_) != VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve.descriptorPool = pool_;
    reserve.descriptorSetCount = 4;
    reserve.pSetLayouts = layouts_.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &reserve, sets_.data()) != VK_SUCCESS) {
      return false;
    }


    use_ubo_ = ubo_only_ || REXCVAR_GET(bo2zm_native_constants_ubo);
    cache_between_frames_ = REXCVAR_GET(bo2zm_native_texture_cache_across_frames);
    REXLOG_INFO("[native] C6: texture caches across frames (bo2zm_native_texture_cache_across_frames) = {}",
                cache_between_frames_ ? "SI" : "no");
    mipmaps_ = REXCVAR_GET(bo2zm_native_mipmaps);
    REXLOG_INFO("[native] C3: texture mip levels (bo2zm_native_mipmaps) = {}", mipmaps_ ? "SI" : "no");
    textures_mb_max_ = REXCVAR_GET(bo2zm_native_textures_mb_max);
    test_no_memory_every_ = REXCVAR_GET(bo2zm_native_test_out_of_memory_every);
    if (test_no_memory_every_ > 0) {
      REXLOG_WARN("[native] C3: TEST on: running out of memory will be faked every {} texture allocations",
                  test_no_memory_every_);
    }
    REXLOG_INFO("[native] C3: texture cache limit (bo2zm_native_textures_mb_max) = {} MB{}", textures_mb_max_,
                textures_mb_max_ > 0 ? "" : " (no limit)");
    diag_mips_ = REXCVAR_GET(bo2zm_native_diag_mips);
    if (diag_mips_) {
      REXLOG_INFO("[native] C3: mip diagnostic (bo2zm_native_diag_mips) = YES");
    }
    alignment_ubo_ = std::max<VkDeviceSize>(16, vulkan_device_->properties().minUniformBufferOffsetAlignment);
    alternate_ubo_s_ = ubo_only_ ? 0 : REXCVAR_GET(bo2zm_native_constants_ubo_alternate_s);
    REXLOG_INFO("[native] C6: constants by dynamic UBO (bo2zm_native_constants_ubo) = {}; alternate every {} s; "
                "alignment {} bytes",
                use_ubo_ ? "SI" : "no", alternate_ubo_s_, alignment_ubo_);
    std::array<VkDescriptorSetLayoutBinding, 3> bindings_ubo{};
    for (uint32_t b = 0; b < 3; ++b) {
      bindings_ubo[b].binding = b;
      bindings_ubo[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      bindings_ubo[b].descriptorCount = 1;
      bindings_ubo[b].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info_ubo{};
    info_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_ubo.bindingCount = 3;
    info_ubo.pBindings = bindings_ubo.data();
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_ubo, nullptr, &layout_ubo_) != VK_SUCCESS) {
      return false;
    }
    const VkDescriptorPoolSize size_ubo{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * uint32_t(sets_ubo_.size())};
    VkDescriptorPoolCreateInfo info_pool_ubo{};
    info_pool_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool_ubo.maxSets = uint32_t(sets_ubo_.size());
    info_pool_ubo.poolSizeCount = 1;
    info_pool_ubo.pPoolSizes = &size_ubo;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool_ubo, nullptr, &pool_ubo_) != VK_SUCCESS) {
      return false;
    }
    std::array<VkDescriptorSetLayout, kSlotsOfJob> layouts_ubo;
    layouts_ubo.fill(layout_ubo_);
    VkDescriptorSetAllocateInfo reserve_ubo{};
    reserve_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve_ubo.descriptorPool = pool_ubo_;
    reserve_ubo.descriptorSetCount = uint32_t(sets_ubo_.size());
    reserve_ubo.pSetLayouts = layouts_ubo.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &reserve_ubo, sets_ubo_.data()) != VK_SUCCESS) {
      return false;
    }
    for (size_t slot = 0; slot < sets_ubo_.size(); ++slot) {
      const VkDescriptorBufferInfo blocks[3] = {{uploads_[slot].buffer, 0, kUboBytesVs},
                                                 {uploads_[slot].buffer, 0, kUboBytesPs},
                                                 {shared_separate_ ? shared_bufs_[slot].buffer
                                                                      : uploads_[slot].buffer,
                                                  0, kUboBytesShared}};
      std::array<VkWriteDescriptorSet, 3> writes_ubo{};
      for (uint32_t b = 0; b < 3; ++b) {
        writes_ubo[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes_ubo[b].dstSet = sets_ubo_[slot];
        writes_ubo[b].dstBinding = b;
        writes_ubo[b].descriptorCount = 1;
        writes_ubo[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes_ubo[b].pBufferInfo = &blocks[b];
      }
      dfn_.vkUpdateDescriptorSets(device_, 3, writes_ubo.data(), 0, nullptr);
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    24};
    VkPipelineLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const std::array<VkDescriptorSetLayout, 5> layouts_pipeline = {layouts_[0], layouts_[1], layouts_[2],
                                                                    layouts_[3], layout_ubo_};
    info_layout.setLayoutCount = 5;
    info_layout.pSetLayouts = layouts_pipeline.data();
    info_layout.pushConstantRangeCount = ubo_only_ ? 0 : 1;
    info_layout.pPushConstantRanges = ubo_only_ ? nullptr : &range;
    return dfn_.vkCreatePipelineLayout(device_, &info_layout, nullptr, &layout_pipeline_) ==
           VK_SUCCESS;
  }


  bool CreateEmpty() {
    const std::array<std::pair<VkImageViewType, uint32_t>, 3> types = {
        {{VK_IMAGE_VIEW_TYPE_2D, 1}, {VK_IMAGE_VIEW_TYPE_3D, 1}, {VK_IMAGE_VIEW_TYPE_CUBE, 6}}};
    for (uint32_t i = 0; i < 3; ++i) {
      VkImageCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      info.imageType = i == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
      info.flags = i == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
      info.format = VK_FORMAT_R8G8B8A8_UNORM;
      info.extent = {1, 1, 1};
      info.mipLevels = 1;
      info.arrayLayers = types[i].second;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
              empty_[i].image, empty_[i].guest_memory)) {
        return false;
      }
      VkImageViewCreateInfo view{};
      view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      view.image = empty_[i].image;
      view.viewType = types[i].first;
      view.format = VK_FORMAT_R8G8B8A8_UNORM;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, types[i].second};
      if (dfn_.vkCreateImageView(device_, &view, nullptr, &empty_[i].view) != VK_SUCCESS) {
        return false;
      }
      empty_[i].width = empty_[i].height = 1;
      empty_[i].format = VK_FORMAT_R8G8B8A8_UNORM;
      WriteImage(i, 0, empty_[i].view);
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    if (dfn_.vkCreateSampler(device_, &sampler, nullptr, &sampler_empty_) != VK_SUCCESS) {
      return false;
    }
    WriteSampler(0, sampler_empty_);
    return true;
  }


  bool PrepareEmpty() {
    if (empty_prepared_) {
      return true;
    }
    const VkCommandBuffer upload = context_->CommandsUpload();
    if (!upload) {
      return false;
    }
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t layers = i == 2 ? 6 : 1;
      Barrier(upload, empty_[i].image, layers);
      VkClearColorValue cero{};
      if (REXCVAR_GET(bo2zm_native_diag_gray_empties)) {
        cero.float32[0] = cero.float32[1] = cero.float32[2] = 0.5f;
        cero.float32[3] = 1.0f;
      }
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
      dfn_.vkCmdClearColorImage(upload, empty_[i].image, VK_IMAGE_LAYOUT_GENERAL, &cero, 1,
                                &range);
      empty_[i].prepared = true;
    }
    empty_prepared_ = true;
    return true;
  }

  void Barrier(VkCommandBuffer cmd, VkImage image, uint32_t layers) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, layers};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
  }

  void WriteImage(uint32_t heap, uint32_t slot, VkImageView view) {
    VkDescriptorImageInfo image{};
    image.imageView = view;
    image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets_[heap];
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  void WriteSampler(uint32_t slot, VkSampler sampler) {
    VkDescriptorImageInfo image{};
    image.sampler = sampler;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets_[3];
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  uint32_t ReserveSlot(uint32_t heap) {
    auto& m = heaps_[heap];
    if (!m.free.empty()) {
      const uint32_t slot = m.free.back();
      m.free.pop_back();
      return slot;
    }
    return m.next < m.capacity ? m.next++ : 0;
  }


  const VertexInput* InputOf(const DrawRequest& p) {
    if (input_generation_ == p.generation_vs && input_vs_ == p.vs) {
      if (input_valid_) {
        ++inputs_reused_;
        return input_current_;
      }

      ++rejected_;
      ++causes_[input_cause_];
      return nullptr;
    }
    input_generation_ = p.generation_vs;
    input_vs_ = p.vs;
    input_valid_ = false;


    InputFront& front = inputs_front_[(p.generation_vs * 0x9E3779B97F4A7C15ull) >> (64 - kInputFrontBits)];
    if (inputs_front_on_ && front.input && front.generation == p.generation_vs && front.vs == p.vs) {
      const bool check = inputs_front_checks_ < kInputFrontChecks || (++inputs_front_sampled_ & 1023) == 0;
      if (!check) {
        input_current_ = front.input;
        input_valid_ = true;
        input_cause_ = 0;
        ++inputs_cache_hits_;
        ++inputs_front_hits_;
        return input_current_;
      }
      ++inputs_front_checks_;
      const uint64_t key_check = KeyInput(p);
      const auto it = key_check ? inputs_cache_.find(key_check) : inputs_cache_.end();
      if (it == inputs_cache_.end() || &it->second != front.input) {
        inputs_front_off_ = true;
        inputs_front_on_ = false;
        REXLOG_WARN("[native] C6 vertex inputs by VS generation: MISMATCH after {} checks (VS n{}, generation {}): "
                    "off for the session",
                    inputs_front_checks_, p.vs->number, p.generation_vs);
      }
    }

    const uint64_t key_input = KeyInput(p);
    if (key_input) {
      if (const auto it = inputs_cache_.find(key_input); it != inputs_cache_.end()) {
        input_current_ = &it->second;
        input_valid_ = true;
        input_cause_ = 0;
        ++inputs_cache_hits_;
        front = {p.generation_vs, p.vs, &it->second};
        return input_current_;
      }
    }
    input_ = VertexInput{};
    const uint64_t rejected_before = rejected_;
    const auto before_input = std::chrono::steady_clock::now();
    const VertexInput* input = ComputeInput(p);
    ns_inputs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - before_input)
                                 .count());
    ++inputs_computed_;
    input_cause_ = rejected_ != rejected_before ? last_cause_ : 0;
    input_current_ = &input_;
    if (input && key_input && inputs_cache_.size() < 4096) {
      const auto [it, added] = inputs_cache_.emplace(key_input, input_);
      front = {p.generation_vs, p.vs, &it->second};
    }
    return input;
  }


  uint64_t KeyInput(const DrawRequest& p) const {
    const ShaderInput& vs = *p.vs;
    const auto& patched = p.vs_microcode;
    if (patched.size() != vs.microcode.size() || vs.elements.size() > 32) {
      return 0;
    }
    std::array<uint32_t, 32 * 6> words;
    size_t n = 0;
    for (const VertexElement& element : vs.elements) {
      const size_t i = size_t(element.instruction) * 3;
      if (i + 2 >= patched.size()) {
        return 0;
      }
      words[n++] = (uint32_t(element.instruction) << 16) | (uint32_t(element.use) << 8) |
                      element.index_use;
      words[n++] = vs.microcode[i];
      words[n++] = vs.microcode[i + 1];
      words[n++] = patched[i];
      words[n++] = patched[i + 1];
      words[n++] = patched[i + 2];
    }
    const uint64_t key =
        XXH3_64bits_withSeed(words.data(), n * sizeof(uint32_t), uint64_t(uintptr_t(p.vs)));
    return key ? key : 1;
  }

  const VertexInput* ComputeInput(const DrawRequest& p) {
    const ShaderInput& vs = *p.vs;
    input_.remaps.fill(kRemapIdentity);
    const auto& patched = p.vs_microcode;
    if (patched.size() != vs.microcode.size()) {
      Reject(20, "the ring's VS does not have the container's length");
      return nullptr;
    }
    uint32_t locations_used = 0;
    for (const VertexElement& element : vs.elements) {
      const uint32_t reg_index = (vs.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F;
      const uint32_t original = vs.microcode[size_t(element.instruction) * 3 + 1] & 0xFFF;
      size_t q = SIZE_MAX;
      for (const VertexElement& other : vs.elements) {
        const size_t i = size_t(other.instruction) * 3;
        if (((patched[i] >> 12) & 0x3F) != reg_index || (patched[i] & 0x1F) != 0) {
          continue;
        }
        if (q == SIZE_MAX) {
          q = i;
        }
        if (MaskWritten(patched[i + 1] & 0xFFF) == MaskWritten(original)) {
          q = i;
          break;
        }
      }
      if (q == SIZE_MAX) {
        if (warned_vs_.size() < 16 && warned_vs_.insert(vs.number).second) {
          std::string detail;
          for (const VertexElement& other : vs.elements) {
            const size_t i = size_t(other.instruction) * 3;
            detail += fmt::format(" {}{}@{}: original r{} op{}, patched r{} op{} {:08X} {:08X} {:08X};",
                                   NameUse(other.use), other.index_use, other.instruction,
                                   (vs.microcode[i] >> 12) & 0x3F, vs.microcode[i] & 0x1F,
                                   (patched[i] >> 12) & 0x3F, patched[i] & 0x1F, patched[i],
                                   patched[i + 1], patched[i + 2]);
          }
          REXLOG_WARN("[native] C6 diag: VS n{} without a fetch for {}{} (register r{}):{}", vs.number,
                      NameUse(element.use), element.index_use, reg_index, detail);
        }
        Reject(21, "vertex fetch not found in the patched VS");
        return nullptr;
      }
      const uint32_t d0 = patched[q], d1 = patched[q + 1], d2 = patched[q + 2];
      const uint32_t slot = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
      const uint32_t format = (d1 >> 16) & 0x3F;
      const uint32_t stride = (d2 & 0xFF) * 4;
      const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
      const int32_t location = UseLocation(element.use, element.index_use);
      if (location < 0 || ((locations_used >> location) & 0x1)) {
        Warn(22, "vertex element without a free location in the shader: skipped");
        continue;
      }
      const uint32_t code = CodeRemap(original, d1 & 0xFFF);
      if (code != kRemapIdentity && warnings_swizzle_ < 24) {
        ++warnings_swizzle_;
        REXLOG_INFO("[native] C6: VS n{} {}{} (format {}): original swizzle {:03X}, patched {:03X}, remap {:03X}",
                    vs.number, NameUse(element.use), element.index_use, format, original,
                    d1 & 0xFFF, code);
      }
      bool r11g11b10 = false;
      const VkFormat vk = FormatAttribute(format, InputWhole(element.use), (d1 >> 12) & 0x1,
                                          (d1 >> 13) & 0x1, false, r11g11b10);
      if (vk == VK_FORMAT_UNDEFINED) {
        Reject(300 + format, "vertex format not supported yet");
        return nullptr;
      }
      if (r11g11b10) {
        input_.specialization |= 0x1;
      }
      uint32_t binding = 0;
      while (binding < input_.bindings.size() && input_.bindings[binding].slot != slot) {
        ++binding;
      }
      if (binding == input_.bindings.size()) {
        input_.bindings.push_back({slot, stride});
      } else if (!input_.bindings[binding].stride) {
        input_.bindings[binding].stride = stride;
      }
      if (offset < 0) {
        Reject(24, "negative vertex offset");
        return nullptr;
      }
      input_.attributes.push_back({uint32_t(location), binding, vk, uint32_t(offset)});
      input_.remaps[location] = code;
      locations_used |= uint32_t(1) << location;
    }
    for (const VertexBinding& binding : input_.bindings) {
      if (!binding.stride) {
        Reject(25, "vertex stream without a stride");
        return nullptr;
      }
    }
    uint64_t hash = XXH3_64bits(input_.attributes.data(),
                                  input_.attributes.size() * sizeof(VertexAttribute));
    hash = XXH3_64bits_withSeed(input_.bindings.data(),
                                  input_.bindings.size() * sizeof(VertexBinding), hash);
    input_.hash = hash ^ input_.specialization;
    input_valid_ = true;
    return &input_;
  }


  void DumpTextureBo2zm(const uint32_t* f, uint64_t address, uint64_t bytes, uint64_t dir_mips) {

    static const uint64_t goal = [] {
      const std::string& text = REXCVAR_GET(bo2zm_native_dump_texture);
      return text.empty() ? UINT64_MAX : text == "mips" ? UINT64_MAX - 1 : std::strtoull(text.c_str(), nullptr, 16);
    }();
    const bool first_with_mips = goal == UINT64_MAX - 1 && dir_mips && (f[1] & 0x3F) == 18 &&
                                  (f[2] & 0x1FFF) >= 255 && ((f[2] >> 13) & 0x1FFF) >= 255;
    if ((address != goal && !first_with_mips) || dump_bo2zm_ || address + bytes > kMemoryPhysical) {
      return;
    }
    static const auto start = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - start < std::chrono::seconds(REXCVAR_GET(bo2zm_native_dump_texture_after_s))) {
      return;
    }
    dump_bo2zm_ = true;
    const std::filesystem::path path =
        rex::filesystem::GetExecutableFolder() / fmt::format("dump_{:08X}.bin", uint32_t(address));
    std::ofstream output(path, std::ios::binary);
    output.write("BO2T", 4);
    output.write(reinterpret_cast<const char*>(f), sizeof(uint32_t) * 6);
    const uint64_t mips = dir_mips && dir_mips + (1u << 20) <= kMemoryPhysical ? (1u << 20) : 0;
    const uint64_t sizes[2] = {bytes, mips};
    output.write(reinterpret_cast<const char*>(sizes), sizeof(sizes));
    output.write(reinterpret_cast<const char*>(memory_->TranslatePhysical(uint32_t(address))), std::streamsize(bytes));
    if (mips) {
      output.write(reinterpret_cast<const char*>(memory_->TranslatePhysical(uint32_t(dir_mips))), std::streamsize(mips));
    }
    g_bo2zm_dump_texture.store((address << 32) | ((dir_mips ? dir_mips + mips : address + bytes) - address),
                                  std::memory_order_relaxed);
    REXLOG_INFO("[native] BO2: texture {:08X} dumped to {} ({} bytes; fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X})",
                uint32_t(address), path.string(), bytes, f[0], f[1], f[2], f[3], f[4], f[5]);
  }
  bool dump_bo2zm_ = false;


  static constexpr uint64_t kStampGuardFirst = 4000;
  static constexpr uint16_t kStampGuardEvery = 16;
  static constexpr uint16_t kStampGuardEveryYoung = 4;

  bool TextureUnwritten(CachedTexture& texture, uint64_t key, uint64_t& valid_until) {
    texture.guard_stamps = false;
    const bool young = texture.interval < 8;
    if (!tex_by_writes_active_ || !texture.image.prepared || texture.check_epoch == 0 ||
        texture.interval < (tex_young_active_ ? 2u : 8u) || texture.hash_job != 0) {
      return false;
    }
    if (!bo2zm::page_writes::Unchanged(texture.stamp_base, texture.stamp_base_bytes, texture.check_epoch) ||
        !bo2zm::page_writes::Unchanged(texture.stamp_mips, texture.stamp_mips_bytes, texture.check_epoch)) {
      ++tex_stamp_written_;
      return false;
    }
    if (++tex_stamp_seen_ <= kStampGuardFirst ||
        ++texture.stamp_skips >= (young ? kStampGuardEveryYoung : kStampGuardEvery)) {
      texture.stamp_skips = 0;
      texture.guard_stamps = true;
      ++tex_stamp_guarded_;
      return false;
    }
    ++tex_stamp_skipped_;
    tex_stamp_skipped_young_ += young ? 1 : 0;
    texture.frame = frame_;
    texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
    texture.next = frame_ + texture.interval + (texture.interval == 32 ? (key >> 7) & 7 : 0);
    valid_until = texture.next - 1;
    return true;
  }


  void TextureChangedWithoutStamp(CachedTexture& texture, uint32_t width, uint32_t height, uint32_t format) {
    texture.guard_stamps = false;
    ++tex_unstamped_interval_;
    tex_unstamped_young_ += texture.interval < 8 ? 1 : 0;
    if (++tex_unstamped_ <= 16) {
      REXLOG_WARN("[native] C3 texture by writes: {:08X}+{} (mips {:08X}+{}) {}x{} format {} changed without a write "
                  "stamp (a writer that does not stamp); its pages now count as written every frame",
                  texture.stamp_base, texture.stamp_base_bytes, texture.stamp_mips, texture.stamp_mips_bytes, width,
                  height, format);
    }
    bo2zm::page_writes::MarkVolatile(texture.stamp_base, texture.stamp_base_bytes);
    bo2zm::page_writes::MarkVolatile(texture.stamp_mips, texture.stamp_mips_bytes);
  }

  void ReportTextureStamps() {
    if (tex_by_writes_ <= 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < tex_stamps_next_) {
      return;
    }
    const bool first = tex_stamps_next_ == std::chrono::steady_clock::time_point{};
    tex_stamps_next_ = now + std::chrono::seconds(10);
    if (!first) {
      BO2ZM_REPORT_RING("[native] C3 texture rechecks by write stamps, last 10 s: {} skipped (no stamp since the last "
                        "check; {} of them young, interval 2-4), {} checked anyway (the guard), {} checked (a stamp "
                        "since); {} changed without a stamp ({} since start, {} of them young), their pages now count "
                        "as always written",
                        tex_stamp_skipped_, tex_stamp_skipped_young_, tex_stamp_guarded_, tex_stamp_written_,
                        tex_unstamped_interval_, tex_unstamped_, tex_unstamped_young_);
    }
    tex_stamp_skipped_ = tex_stamp_skipped_young_ = tex_stamp_guarded_ = tex_stamp_written_ =
        tex_unstamped_interval_ = 0;
  }


  bool TextureReuse() {
    if (texreuse_ < 0) {
      texreuse_ab_ = REXCVAR_GET(bo2zm_native_texture_reuse_ab);
      texreuse_ = REXCVAR_GET(bo2zm_native_texture_reuse) || texreuse_ab_ ? 1 : 0;
      REXLOG_INFO("[native] C3: new textures take a cold texture's image with the same content "
                  "(bo2zm_native_texture_reuse) = {}{}",
                  texreuse_ ? "YES" : "no", texreuse_ab_ ? "; A/B factor 1 in the benchmark" : "");
    }
    return texreuse_ > 0;
  }


  uint64_t HashRaw(uint64_t start_raw, uint64_t extension, uint64_t dir_mips, uint64_t extension_mips) {
    uint64_t hash = XXH3_64bits(memory_->TranslatePhysical(uint32_t(start_raw)), size_t(extension));
    if (extension_mips) {
      hash = XXH3_64bits_withSeed(memory_->TranslatePhysical(uint32_t(dir_mips)), size_t(extension_mips), hash);
    }
    return hash;
  }

  bool ReuseByContent(CachedTexture& texture, uint64_t key, uint64_t shape_content, VkFormat format,
                      uint32_t width_host, uint32_t height_host, uint32_t levels, uint32_t layers, uint64_t hash_raw,
                      uint32_t address) {
    const uint64_t content =
        KeyContentOf(shape_content, format, levels, width_host, height_host, layers, hash_raw);
    uint64_t taken_key = 0;
    uint32_t looked = 0;
    const auto [since, until] = live_per_content_.equal_range(content);
    for (auto it = since; it != until && looked < kReuseLookMax; ++it, ++looked) {
      if (it->second == key) {
        continue;
      }
      const auto other = textures_.find(it->second);
      if (other == textures_.end()) {
        continue;
      }
      const CachedTexture& t = other->second;
      if (t.key_content == content && t.image.image != VK_NULL_HANDLE && t.image.prepared && !t.needs_upload &&
          !t.in_flight && !t.hash_job && &t != comparison_texture_ && t.frame != UINT64_MAX &&
          t.frame + kFramesNoUseForRelease < frame_) {
        taken_key = it->second;
        break;
      }
    }
    if (!taken_key) {
      ++texreuse_missed_;
      return false;
    }
    CachedTexture& old = textures_.find(taken_key)->second;
    RemoveContentTexture(old, taken_key);
    texture.image = old.image;
    old.image = NativeImage{};
    texture.layers = old.layers;
    texture.depth3d = old.depth3d;
    texture.levels = old.levels;
    texture.offset_level = old.offset_level;
    texture.bytes = old.bytes;
    texture.hash = old.hash;
    texture.hash_raw = hash_raw;
    texture.shape_content = shape_content;
    texture.address = address;
    texture.content_per_measure = false;
    textures_.erase(taken_key);
    AnnotateContentTexture(texture, key);

    texture.frame = frame_;
    texture.interval = 8;
    texture.next = frame_ + 8;
    ++texreuse_taken_;
    ++texreuse_taken_total_;
    texreuse_bytes_ += texture.bytes;
    return true;
  }

  void ReportTextureReuse() {
    if (texreuse_ <= 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < texreuse_next_) {
      return;
    }
    const bool first = texreuse_next_ == std::chrono::steady_clock::time_point{};
    texreuse_next_ = now + std::chrono::seconds(10);
    if (!first) {
      BO2ZM_REPORT_RING("[native] C3 new textures by content, last 10 s: {} took a cold texture's image ({:.1f} MB not "
                        "created, read or uploaded; {} since start), {} looked up without a match",
                        texreuse_taken_, double(texreuse_bytes_) / 1048576.0, texreuse_taken_total_,
                        texreuse_missed_);
    }
    texreuse_taken_ = texreuse_bytes_ = texreuse_missed_ = 0;
  }

  void PrepareTexture(const uint32_t* f, uint32_t& slot, uint32_t& heap,
                       VkDeviceSize& bytes_upload, bool& sampling_point, uint64_t& valid_until,
                       uint32_t& width_host_out, uint32_t& height_host_out) {
    slot = 0;
    heap = 0;
    width_host_out = 0;
    height_host_out = 0;
    sampling_point = false;
    valid_until = frame_;
    prepare_outcome_ = 0;
    prepare_new_ = false;
    if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
      return;
    }
    const uint32_t dimension = (f[5] >> 9) & 0x3;
    const bool cube = dimension == uint32_t(xenos::DataDimension::kCube);
    const bool volume = dimension == uint32_t(xenos::DataDimension::k3D);
    if (dimension != uint32_t(xenos::DataDimension::k2DOrStacked) && !cube && !volume) {
      Warn(30, "1D textures not supported yet: an empty one is used");
      return;
    }
    heap = cube ? 2 : volume ? 1 : 0;
    const uint32_t layers = cube ? 6 : 1;
    if ((f[0] >> 2) & 0xFF) {
      Warn(31, "signed or gamma textures: read as unsigned");
    }
    const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
    const uint32_t base = (f[1] >> 12) << 12;
    if (!cube && !volume) {
      if (const NativeImage* resolved = context_->TextureResolved(base & 0x1FFFFFFF)) {


        width_host_out = resolved->width;
        height_host_out = resolved->height;


        if (resolved->width_guest && resolved->height_guest) {
          width_host_out = resolved->width_guest;
          height_host_out = resolved->height_guest;
        }


        if (warnings_invsize_resolved_.insert(uint64_t(resolved->width) << 32 | resolved->height).second) {
          REXLOG_INFO("[native] C3: resolved texture {}x{}: 1/size = {:.6f}, {:.6f}",
                      resolved->width, resolved->height, 1.0f / float(resolved->width),
                      1.0f / float(resolved->height));
        }
        if (IsDepth(resolved->format)) {


          slot = SlotView(resolved->image, resolved->format, swizzle, kSwizzleRRRR);
          sampling_point = true;


          valid_until = frame_;
          return;
        }


        slot = SlotView(resolved->image, resolved->format, swizzle,
                             resolved->swap_rb ? kSwizzleBGRA : kSwizzleRGBA);
        valid_until = frame_;
        return;
      }
    }


    if (volume) {
      if (const NativeImage* resolved = context_->TextureResolved3D(
              base & 0x1FFFFFFF, f[1] & 0x3F, (f[2] & 0x7FF) + 1, ((f[2] >> 11) & 0x7FF) + 1,
              ((f[2] >> 22) & 0x3FF) + 1, ((f[0] >> 22) & 0x1FF) << 5, (f[0] >> 31) & 0x1)) {
        slot = SlotView(resolved->image, resolved->format, swizzle,
                             resolved->swap_rb ? kSwizzleBGRA : kSwizzleRGBA, heap);
        width_host_out = resolved->width;
        height_host_out = resolved->height;
        valid_until = frame_;
        return;
      }
    }
    const uint32_t format = f[1] & 0x3F;
    HostTextureFormat tf;
    if (!FormatTextureOf(format, tf)) {
      Warn(400 + format, "texture format not supported yet: an empty one is used");
      return;
    }
    bo2zm::texture::BcFormat bc_format{};
    const bool decode_bc = ubo_only_ && bo2zm::texture::BcFormatOf(tf.format, bc_format);
    const bool compact_bc = decode_bc && ((bc_format == bo2zm::texture::BcFormat::BC4 && compact_bc4_) ||
                                         (bc_format == bo2zm::texture::BcFormat::BC5 && compact_bc5_));
    const auto bc_storage = compact_bc ? bo2zm::texture::BcStorage::Compact : bo2zm::texture::BcStorage::Rgba8;
    const VkFormat host_format = decode_bc ? bo2zm::texture::BcHostFormat(bc_format, bc_storage) : tf.format;

    const uint32_t width = volume ? (f[2] & 0x7FF) + 1 : (f[2] & 0x1FFF) + 1;
    const uint32_t height = volume ? ((f[2] >> 11) & 0x7FF) + 1 : ((f[2] >> 13) & 0x1FFF) + 1;
    const uint32_t depth3d = volume ? ((f[2] >> 22) & 0x3FF) + 1 : 0;
    if (volume && tf.block > 1) {
      Warn(38, "compressed 3D texture: an empty one is used");
      return;
    }
    const uint32_t blocks_x = (width + tf.block - 1) / tf.block;
    const uint32_t blocks_y = (height + tf.block - 1) / tf.block;
    const uint32_t pitch_blocks = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / tf.block, 1);


    const uint64_t stride_face =
        (uint64_t((pitch_blocks + 31) & ~uint32_t(31)) * tf.bytes *
             ((blocks_y + 31) & ~uint32_t(31)) +
         4095) &
        ~uint64_t(4095);
    const uint32_t keys[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9};
    const uint64_t key = XXH3_64bits(keys, sizeof(keys));


    const bool tiled_texture = (f[0] >> 31) & 0x1;
    const uint64_t dir_base = uint64_t(base) & 0x1FFFFFFF;
    const uint64_t dir_mips = uint64_t((f[5] >> 12) & 0x1FFFF) << 12;
    DumpTextureBo2zm(f, dir_base, stride_face, dir_mips);
    const uint32_t level_packed =
        mipmaps_ && !volume && ((f[5] >> 11) & 0x1) ? LevelPacked(width, height) : UINT32_MAX;
    uint32_t level_max = 0;
    bool read_base = true;
    if (mipmaps_ && !volume && dir_mips != 0) {
      const uint32_t sz_max = Log2Floor(std::max(width, height));
      uint32_t level_min = std::min((f[4] >> 2) & 0xFu, sz_max);
      level_max = std::max(std::min((f[4] >> 6) & 0xFu, sz_max), level_min);
      if (level_max != 0) {
        if (dir_base == 0) {
          level_min = std::max(level_min, 1u);
        }
        read_base = level_min == 0;
      }
    }


    struct RegionMip {
      uint64_t displacement = 0;
      uint64_t row_bytes = 0;
      uint64_t stride = 0;
      uint32_t pitch_blocks = 0;
    };
    std::array<RegionMip, 16> regions{};
    uint64_t extension_mips = 0;
    if (level_max != 0) {
      const uint32_t last_level = level_packed == 0 ? 0 : std::min(level_max, level_packed);
      for (uint32_t s = level_packed == 0 ? 0 : 1; s <= last_level; ++s) {
        RegionMip& region = regions[s];
        const uint32_t row_texels = std::max(std::bit_ceil(width) >> s, 1u);
        const uint32_t rows_texels = std::max(std::bit_ceil(height) >> s, 1u);
        region.pitch_blocks = ((row_texels + tf.block - 1) / tf.block + 31) & ~31u;
        region.row_bytes = uint64_t(region.pitch_blocks) * tf.bytes;
        if (!tiled_texture) {
          region.row_bytes = (region.row_bytes + 255) & ~uint64_t(255);
        }
        const uint64_t rows_blocks = uint64_t(((rows_texels + tf.block - 1) / tf.block + 31) & ~31u);
        region.stride = (region.row_bytes * rows_blocks + 4095) & ~uint64_t(4095);
        region.displacement = extension_mips;
        extension_mips += region.stride * layers;
      }
      if (dir_mips + extension_mips > kMemoryPhysical) {
        level_max = 0;
        extension_mips = 0;
        read_base = true;
      }
    }


    uint32_t base_ox = 0;
    uint32_t base_oy = 0;
    if (level_packed == 0) {
      OffsetPacked(width, height, tf.block, 0, base_ox, base_oy);
    }


    uint64_t start_raw = 0;
    uint64_t extension = 0;
    if (!volume) {
      const uint32_t log2_block = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                                : tf.bytes >= 2 ? 1 : 0;


      const uint32_t blocks_x_read = blocks_x + base_ox;
      const uint32_t blocks_y_read = blocks_y + base_oy;
      const uint64_t extent_layer =
          ((f[0] >> 31) & 0x1)
              ? uint64_t(std::max<int64_t>(
                    OffsetTiled2D(int32_t((blocks_x_read - 1) & ~31u),
                                            int32_t((blocks_y_read - 1) & ~31u), pitch_blocks,
                                            log2_block),
                    0)) +
                    (log2_block == 0   ? 0xA00u
                     : log2_block == 1 ? 0xC00u
                                        : (0x400u << log2_block))
              : uint64_t(std::max(pitch_blocks, blocks_x_read)) * tf.bytes * (blocks_y_read - 1) +
                    uint64_t(blocks_x_read) * tf.bytes;
      start_raw = uint64_t(base) & 0x1FFFFFFF;
      extension = (layers - 1) * stride_face + extent_layer;
    }

    if (cube && copy_image_) {


      std::array<const NativeImage*, 6> faces{};
      bool resolved_list = true;
      for (uint32_t c = 0; c < 6 && resolved_list; ++c) {
        faces[c] = context_->TextureResolved(
            uint32_t((uint64_t(base) + c * stride_face) & 0x1FFFFFFF));
        resolved_list = faces[c] && faces[c]->format == VK_FORMAT_R8G8B8A8_UNORM &&
                    faces[c]->width >= width && faces[c]->height >= height;
      }
      if (resolved_list) {
        const uint64_t key_resolved = key ^ 0x9E3779B97F4A7C15ull;
        CachedTexture& texture = textures_[key_resolved];
        if (texture.image.image == VK_NULL_HANDLE) {
          if (!CreateTexture(texture.image, VK_FORMAT_R8G8B8A8_UNORM, width, height, layers)) {
            textures_.erase(key_resolved);
            Warn(32, "could not create a texture");
            return;
          }
          texture.layers = layers;
          texture.bytes = uint64_t(width) * height * 4 * layers;
          bytes_textures_ += texture.bytes;
          REXLOG_INFO("[native] C3: cube {:08X} {}x{} with its faces resolved by C2 (fetch {:08X} {:08X} {:08X} {:08X} "
                      "{:08X} {:08X})",
                      base, width, height, f[0], f[1], f[2], f[3], f[4], f[5]);
        }
        if (texture.frame != frame_) {
          const VkCommandBuffer upload = context_->CommandsUpload();
          if (!upload) {
            return;
          }
          texture.frame = frame_;
          if (!texture.image.prepared) {
            Barrier(upload, texture.image.image, layers);
            texture.image.prepared = true;
          }
          for (uint32_t c = 0; c < 6; ++c) {
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, c, 1};
            copy.extent = {width, height, 1};
            copy_image_(upload, faces[c]->image, VK_IMAGE_LAYOUT_GENERAL, texture.image.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
          }

          ++cubes_refreshed_;
          const auto now = std::chrono::steady_clock::now();
          if (now - report_cubes_ >= std::chrono::seconds(10)) {

            BO2ZM_REPORT_RING("[native] C3 cubes with C2 faces: {} refreshes in {} jobs",
                                 cubes_refreshed_ - cubes_refreshed_previous_,
                                 frame_ - frame_report_cubes_);
            report_cubes_ = now;
            cubes_refreshed_previous_ = cubes_refreshed_;
            frame_report_cubes_ = frame_;
          }
        }
        slot = SlotView(texture.image.image, VK_FORMAT_R8G8B8A8_UNORM, swizzle,
                             faces[0]->swap_rb ? kSwizzleBGRA : kSwizzleRGBA, heap);


        width_host_out = texture.image.width;
        height_host_out = texture.image.height;
        return;
      }
    }

    CachedTexture& texture = textures_[key];
    uint64_t hash_raw_known = 0;
    bool hash_known = false;
    auto t_new = std::chrono::steady_clock::time_point{};
    if (texture.image.image == VK_NULL_HANDLE) {
      if (time_) {
        t_new = std::chrono::steady_clock::now();
      }

      const uint32_t width_host = tf.block > 1 ? (width + 3) & ~uint32_t(3) : width;
      const uint32_t height_host = tf.block > 1 ? (height + 3) & ~uint32_t(3) : height;

      const uint32_t levels = std::min(level_max + 1, Log2Floor(std::max(width_host, height_host)) + 1);
      if (decode_bc) {
        bo2zm::texture::BcMipLayout checked;
        if (!bo2zm::texture::PlanBcMips(bc_format, width_host, height_host, layers, levels, checked,
                                      bo2zm::texture::kBcUploadLimit, bc_storage)) {
          Warn(441, "BC texture exceeds the staging limit or has an invalid layout");
          return;
        }
      }


      const uint32_t shape_words[5] = {keys[0], f[1] & 0x7FF, keys[2], keys[3],
                                       ((f[5] >> 9) & 0x7) | (dir_base == 0 ? 0x8u : 0u)};
      const uint64_t shape_content = XXH3_64bits(shape_words, sizeof(shape_words));

      if (texreuse_active_ && !volume && start_raw + extension <= kMemoryPhysical) {
        hash_raw_known = HashRaw(start_raw, extension, dir_mips, extension_mips);
        hash_known = true;
        const bool reused = ReuseByContent(texture, key, shape_content, host_format, width_host, height_host, levels,
                                           layers, hash_raw_known, base);
        CutSubstage(31, t_new);
        if (reused) {
          texture.stamp_base = uint32_t(start_raw);
          texture.stamp_base_bytes = uint32_t(extension);
          texture.stamp_mips = uint32_t(dir_mips);
          texture.stamp_mips_bytes = uint32_t(extension_mips);
          texture.check_epoch =
              bo2zm::page_writes::g_enabled.load(std::memory_order_relaxed) ? bo2zm::page_writes::Epoch() : 0;
          slot = SlotView(texture.image.image, host_format, swizzle, tf.swizzle_host, heap);
          width_host_out = texture.image.width;
          height_host_out = texture.image.height;
          valid_until = texture.next - 1;
          prepare_outcome_ = 8;
          return;
        }
      }


      const auto before_create = std::chrono::steady_clock::now();
      const uint64_t wait_before_create = ns_wait_bindings_total_;
      measuring_creation_ = true;
      const bool created = CreateTextureInThread(texture, host_format, width_host, height_host, layers, depth3d, levels) ||
                          CreateTexture(texture.image, host_format, width_host, height_host, layers, depth3d, levels);
      measuring_creation_ = false;
      {
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - before_create)
                                         .count());
        const uint64_t wait = ns_wait_bindings_total_ - wait_before_create;
        const uint64_t own = ns > wait ? ns - wait : 0;
        bo2zm::waits::g_ns_create_textures.fetch_add(own, std::memory_order_relaxed);
        ns_create_report_ += own;
      }
      if (!created) {
        textures_.erase(key);
        Warn(32, "could not create a texture");
        return;
      }
      prepare_new_ = true;
      texture.layers = layers;
      texture.depth3d = depth3d;
      texture.levels = levels;
      levels_mip_uploaded_ += levels - 1;


      if (ContentIndexed()) {
        RemoveContentTexture(texture, key);
        texture.shape_content = shape_content;
        texture.address = base;
        texture.content_per_measure = DiagReuse();
      }
      const uint32_t host_block = decode_bc ? 1 : tf.block;
      const uint32_t host_bytes = decode_bc ? bo2zm::texture::BcPixelBytes(bc_format, bc_storage) : tf.bytes;
      for (uint32_t n = 0; n < levels; ++n) {
        texture.bytes += uint64_t((std::max(width_host >> n, 1u) + host_block - 1) / host_block) *
                         ((std::max(height_host >> n, 1u) + host_block - 1) / host_block) * host_bytes * layers *
                         (depth3d ? depth3d : 1);
      }
      bytes_textures_ += texture.bytes;


      {
        ++textures_created_;
        bo2zm::waits::g_textures_created.fetch_add(1, std::memory_order_relaxed);
        const uint64_t shape_key = XXH3_64bits_withSeed(&base, sizeof(base), (uint64_t(format) << 40) ^
                                                                              (uint64_t(width) << 20) ^ height);


        if (diag_shapes_.empty()) {
          diag_shapes_.resize(kDiagSlots);
          diag_addresses_.resize(kDiagSlots);
        }
        uint32_t& seen_base = diag_addresses_[(uint32_t(base) * 0x9E3779B1u) >> (32 - kDiagSlotBits)];
        if (seen_base == uint32_t(base) + 1) {
          ++created_in_address_view_;
        }
        seen_base = uint32_t(base) + 1;
        DiagShape& shape = diag_shapes_[size_t(shape_key >> (64 - kDiagSlotBits))];
        const bool new_shape = shape.shape != shape_key;
        if (!new_shape && shape.key != key) {
          ++created_same_shape_other_key_;
          shape.key = key;


          if (std::equal(std::begin(keys), std::end(keys), shape.words.begin())) {
            const uint64_t times = ++keys_incoherent_;
            if (times == 1 || times == 10 || times == 100 || times == 1000 || times % 10000 == 0) {
              REXLOG_ERROR("[native] C3 INCOHERENT texture key ({} times): {:08X} {}x{} format {} with the same five "
                           "words as the previous time and another key; the key does not come from its words",
                           times, base, width, height, format);
            }
          }
          if (warnings_other_key_ < 40) {
            ++warnings_other_key_;
            const auto& before = shape.words;
            REXLOG_INFO("[native] C3 same texture with another key: {:08X} {}x{} format {} | before {:08X} {:08X} "
                        "{:08X} {:02X} {:06X} | now {:08X} {:08X} {:08X} {:02X} {:06X} | changed {:08X} {:08X} {:08X} "
                        "{:02X} {:06X}",
                        base, width, height, format, before[0], before[1], before[2], before[3], before[4], keys[0],
                        keys[1], keys[2], keys[3], keys[4], before[0] ^ keys[0], before[1] ^ keys[1],
                        before[2] ^ keys[2], before[3] ^ keys[3], before[4] ^ keys[4]);
          }
          std::copy(std::begin(keys), std::end(keys), shape.words.begin());
        } else if (new_shape) {
          shape.shape = shape_key;
          shape.key = key;
          std::copy(std::begin(keys), std::end(keys), shape.words.begin());
        }


        if (textures_created_ % 1024 == 0) {
          REXLOG_INFO("[native] C3 diag cache: {} textures created ({} in the cache, {} MB): {} at an address that "
                      "already had another texture; {} with the same address, format and size as another but a "
                      "different key",
                      textures_created_, textures_.size(), bytes_textures_ >> 20, created_in_address_view_,
                      created_same_shape_other_key_);
          AnnotateGpuMemory();
        }
      }
      if (textures_.size() <= 48) {
        REXLOG_INFO("[native] C3: texture {:08X} {}x{} format {} {} order {} pitch {} swizzle {:03X} signs {:02X}{}; "
                    "levels {} (mips at {:08X}, packed from {})",
                    base, width, height, format, ((f[0] >> 31) & 0x1) ? "tiled" : "linear",
                    (f[1] >> 6) & 0x3, ((f[0] >> 22) & 0x1FF) << 5, swizzle, (f[0] >> 2) & 0xFF,
                    cube ? " (cube)" : volume ? " (3D)" : "", levels, dir_mips,
                    level_packed == UINT32_MAX ? std::string("none") : std::to_string(level_packed));
      }
      CutSubstage(32, t_new);
    }
    if (texture.staging_failed_frame == frame_) {
      slot = 0;
      valid_until = frame_;
      return;
    }
    slot = SlotView(texture.image.image, host_format, swizzle, tf.swizzle_host, heap);
    width_host_out = texture.image.width;
    height_host_out = texture.image.height;
    if (texture.frame == frame_) {

      valid_until = std::max<uint64_t>(frame_, texture.next ? texture.next - 1 : 0);
      return;
    }


    if (texture.image.prepared && frame_ < texture.next) {
      valid_until = texture.next - 1;
      return;
    }

    if (TextureUnwritten(texture, key, valid_until)) {
      prepare_outcome_ = 1;
      return;
    }
    texture.frame = frame_;


    if (texture.hash_job == kJobHashPublished) {
      CollectHashes();
    }
    const uint64_t previous_hash_raw = texture.hash_raw;

    struct TextureStopwatch {
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      ~TextureStopwatch() {
        bo2zm::waits::g_ns_textures.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                         .count()),
            std::memory_order_relaxed);
      }
    } stopwatch_texture;


    if (!volume) {
      if (start_raw + extension <= kMemoryPhysical) {


        if (sampling_hashes_ < 0) {
          const int32_t every = REXCVAR_GET(bo2zm_native_hash_sampling);
          sampling_hashes_ = every >= 2 ? std::min<int32_t>(every, 64) : 0;
          REXLOG_INFO("[native] C3: recheck of stable textures by sampling (bo2zm_native_hash_sampling) = {}",
                      sampling_hashes_ ? fmt::format("YES, 1 in {} full; the first {} with both hashes",
                                                      sampling_hashes_, kSamplesACheck)
                                        : std::string("no, always the full hash"));
        }
        const uint64_t bytes_full = extension + extension_mips;
        const uint64_t sample_bytes = BytesSample(extension) + BytesSample(extension_mips);
        const bool with_sample = sampling_hashes_ > 0 && texture.image.prepared && texture.interval >= 4 &&
                                 sample_bytes * 2 <= bytes_full;
        const bool sample_useful = with_sample && texture.interval >= 8 && texture.sample_valid;
        const bool only_sample = sample_useful && samples_checked_ >= kSamplesACheck &&
                                  texture.samples_consecutive + 1u < uint32_t(sampling_hashes_);
        const uint64_t bytes_sample = with_sample ? sample_bytes : 0;

        const uint64_t bytes_hash = only_sample ? bytes_sample : bytes_full + bytes_sample;
        if (frame_hashes_ != frame_) {
          frame_hashes_ = frame_;
          bytes_hash_frame_ = 0;
        }
        if (budget_hashes_ < 0) {
          budget_hashes_ = std::max(REXCVAR_GET(bo2zm_native_hash_kb_per_frame), 0);
          REXLOG_INFO("[native] C3: texture check budget = {} KB per frame{}",
                      budget_hashes_, budget_hashes_ ? "" : " (no limit)");
        }
        if (budget_hashes_ && texture.image.prepared && texture.interval >= 8 &&
            texture.postponements < kPostponementMax && bytes_hash_frame_ > 0 &&
            bytes_hash_frame_ + bytes_hash > uint64_t(budget_hashes_) * 1024) {
          ++texture.postponements;
          texture.next = frame_ + 1 + (key & 1);
          valid_until = texture.next - 1;
          bo2zm::waits::g_hashes_postponed.fetch_add(1, std::memory_order_relaxed);
          prepare_outcome_ = 2;
          return;
        }
        texture.postponements = 0;


        texture.stamp_base = uint32_t(start_raw);
        texture.stamp_base_bytes = uint32_t(extension);
        texture.stamp_mips = uint32_t(dir_mips);
        texture.stamp_mips_bytes = uint32_t(extension_mips);
        texture.check_epoch =
            bo2zm::page_writes::g_enabled.load(std::memory_order_relaxed) ? bo2zm::page_writes::Epoch() : 0;
        bytes_hash_frame_ += bytes_hash;
        bo2zm::waits::g_bytes_hash.fetch_add(bytes_hash, std::memory_order_relaxed);
        const auto start_hash_raw = std::chrono::steady_clock::now();
        const uint8_t* const raw_base = memory_->TranslatePhysical(uint32_t(start_raw));
        const uint8_t* const raw_mips = extension_mips ? memory_->TranslatePhysical(uint32_t(dir_mips)) : nullptr;


        uint64_t sample = 0;
        if (with_sample) {
          sample = HashSample(raw_base, extension, 0);
          if (extension_mips) {
            sample = HashSample(raw_mips, extension_mips, sample);
          }
          bytes_sample_ += bytes_sample;
          ReportSampling(start_hash_raw);
        }
        const bool sample_equal = sample_useful && sample == texture.hash_sample;
        if (only_sample && sample_equal) {
          bo2zm::waits::g_ns_hash_raw.fetch_add(
              uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                            start_hash_raw)
                           .count()),
              std::memory_order_relaxed);
          ++texture.samples_consecutive;
          ++samples_hits_;
          bytes_saved_sample_ += bytes_full - bytes_sample;
          texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
          texture.next = frame_ + texture.interval + (texture.interval == 32 ? (key >> 7) & 7 : 0);
          valid_until = texture.next - 1;
          prepare_outcome_ = 3;
          return;
        }
        if (only_sample) {

          ++samples_different_;
          bytes_hash_frame_ += bytes_full;
          bo2zm::waits::g_bytes_hash.fetch_add(bytes_full, std::memory_order_relaxed);
        }


        if (!decode_bc && !texture.image.prepared && hashes_phase_ != kHashesOff && !diag_mips_ && tiled_fast_ >= 0) {
          std::array<size_t, 16> offsets{};
          std::array<size_t, 16> layer_level{};
          size_t bytes_plan = 0;
          for (uint32_t n = 0; n < texture.levels; ++n) {
            const uint32_t bx_n = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t by_n = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
            offsets[n] = bytes_plan;
            layer_level[n] = size_t(bx_n) * by_n * tf.bytes;
            bytes_plan += layer_level[n] * layers;
          }
          const uint32_t bx_base = (texture.image.width + tf.block - 1) / tf.block;
          const uint64_t mips_in_copy = (extension + 63) & ~uint64_t(63);
          uint32_t n_reads = 0;
          bool plan_ok = true;
          for (uint32_t c = 0; plan_ok && c < (read_base ? layers : 0u); ++c) {
            plan_ok = PlanReadHash(reads_plan_, n_reads, dir_base + c * stride_face, tiled_texture,
                                           pitch_blocks, uint64_t(std::max(pitch_blocks, blocks_x)) * tf.bytes, tf,
                                           base_ox, base_oy, blocks_x, blocks_y, bx_base, layer_level[0] * c,
                                           start_raw, extension, 0);
          }
          for (uint32_t n = 1; plan_ok && n < texture.levels; ++n) {
            const uint32_t s = level_packed == 0 ? 0 : std::min(n, level_packed);
            uint32_t ox = 0;
            uint32_t oy = 0;
            if (n >= level_packed) {
              OffsetPacked(width, height, tf.block, n, ox, oy);
            }
            const uint32_t bx_guest = (std::max(width >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t by_guest = (std::max(height >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
            for (uint32_t c = 0; plan_ok && c < layers; ++c) {
              plan_ok = PlanReadHash(reads_plan_, n_reads,
                                             dir_mips + regions[s].displacement + c * regions[s].stride,
                                             tiled_texture, regions[s].pitch_blocks, regions[s].row_bytes, tf,
                                             ox, oy, std::min(bx_guest, bx_host), std::min(by_guest, by_host),
                                             bx_host, offsets[n] + layer_level[n] * c, dir_mips, extension_mips,
                                             mips_in_copy);
            }
          }
          if (plan_ok && PlanHash(texture, key, n_reads, raw_base, extension, raw_mips, extension_mips,
                                       tf, (f[1] >> 6) & 0x3, bytes_plan, offsets, base, width, height,
                                       format, bytes_upload)) {
            prepare_outcome_ = 7;
            return;
          }
        }

        uint64_t hash_raw = hash_known ? hash_raw_known : XXH3_64bits(raw_base, size_t(extension));
        if (extension_mips && !hash_known) {
          hash_raw = XXH3_64bits_withSeed(raw_mips, size_t(extension_mips), hash_raw);
        }
        bo2zm::waits::g_ns_hash_raw.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                          start_hash_raw)
                         .count()),
            std::memory_order_relaxed);
        if (prepare_new_) {
          CutSubstage(33, t_new);
        }
        const bool full_equal = texture.image.prepared && hash_raw == texture.hash_raw;
        if (sample_useful && !only_sample) {

          ++samples_checked_;
          ++samples_with_two_;
          if (sample_equal && !full_equal) {
            REXLOG_ERROR("[native] C3: the SAMPLE of texture {:08X} {}x{} format {} ({} KB of base and {} KB of mips) "
                         "did not see a change that the full hash did. Hash sampling is switched OFF (after {} rechecks "
                         "with both).",
                         uint32_t(start_raw), width, height, format, extension >> 10, extension_mips >> 10,
                         samples_checked_);
            sampling_hashes_ = 0;
          } else if (samples_checked_ == kSamplesACheck) {
            REXLOG_INFO("[native] C3: hash sampling: {} rechecks with both hashes, no disagreement. Sampling stays on, "
                        "with 1 in {} full.",
                        samples_checked_, sampling_hashes_);
          }
        }
        if (full_equal) {

          if (with_sample && sampling_hashes_ > 0) {
            texture.hash_sample = sample;
            texture.sample_valid = true;
          }
          texture.samples_consecutive = 0;
          texture.interval = std::min<uint32_t>(texture.interval * 2, 32);

          texture.next = frame_ + texture.interval + (texture.interval == 32 ? (key >> 7) & 7 : 0);
          valid_until = texture.next - 1;
          prepare_outcome_ = 4;
          return;
        }
        if (texture.guard_stamps) {
          TextureChangedWithoutStamp(texture, width, height, format);
        }
        texture.sample_valid = false;
        texture.samples_consecutive = 0;
        texture.hash_raw = hash_raw;
        AnnotateContentTexture(texture, key);
      }
    }
    prepare_outcome_ = 6;

    const uint32_t blocks_x_host = (texture.image.width + tf.block - 1) / tf.block;
    const uint32_t blocks_y_host = (texture.image.height + tf.block - 1) / tf.block;
    const size_t bytes_layer = size_t(blocks_x_host) * blocks_y_host * tf.bytes;
    if (volume && UpdateVolume(texture, f, tf, blocks_x, blocks_y, depth3d, blocks_x_host, blocks_y_host,
                                     pitch_blocks, valid_until, bytes_upload)) {
      return;
    }
    if (volume) {
      texture.incremental.reset();
    }
    std::vector<uint8_t>& data = temporal_;

    std::array<size_t, 16> bytes_layer_level{};
    std::array<uint32_t, 16> source_offsets{};
    size_t bytes_data = 0;
    for (uint32_t n = 0; n < texture.levels; ++n) {
      const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
      source_offsets[n] = uint32_t(bytes_data);
      bytes_layer_level[n] = size_t(bx_host) * by_host * tf.bytes;
      bytes_data += bytes_layer_level[n] * layers * (depth3d ? depth3d : 1);
    }
    const size_t previous_capacity = data.capacity();
    try {
      data.assign(bytes_data, 0);
    } catch (const std::bad_alloc&) {


      RemoveContentTexture(texture, key);
      texture.hash_raw = previous_hash_raw;
      texture.sample_valid = false;
      texture.check_epoch = 0;
      texture.interval = 1;
      texture.next = frame_ + 1;
      texture.staging_failed_frame = frame_;
      slot = 0;
      valid_until = frame_;
      const uint64_t failures = ++staging_alloc_failures_;
      if ((failures & (failures - 1)) == 0) {
        try {
          const auto pool = pool_textures_.State();
          REXLOG_WARN("[native] texture staging allocation failed: requested {} bytes, previous capacity {} bytes, "
                      "{} failures; texture {:08X} {}x{} skipped for this frame; cache payload {} MB, "
                      "pool occupied {} MB of {} MB reserved",
                      bytes_data, previous_capacity, failures, base, width, height, bytes_textures_ >> 20,
                      pool.bytes_in_use >> 20, pool.bytes_reserved >> 20);
        } catch (const std::bad_alloc&) {

        }
      }
      return;
    }
    texture.staging_failed_frame = UINT64_MAX;
    const bool tiled = (f[0] >> 31) & 0x1;
    const auto start_volume = volume ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (volume && !ReadVolume(f, tf, blocks_x, blocks_y, depth3d, blocks_x_host,
                                blocks_y_host, pitch_blocks, tiled, data)) {
      Warn(33, "texture outside memory");
      if (!texture.image.prepared) {
        slot = 0;
      }
      return;
    }
    if (volume) {
      VolumeReadInfo& v = volumes_read_[uint64_t(dir_base) << 32 | format << 26 | (depth3d & 0x3FF) << 16 |
                                          (height & 0xFF) << 8 | (width & 0xFF)];
      v.base = uint32_t(dir_base);
      v.format = format;
      v.width = width;
      v.height = height;
      v.depth3d = depth3d;
      ++v.reads;
      v.bytes += data.size();
      v.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                            start_volume)
                           .count());
      v.already_prepared += texture.image.prepared ? 1 : 0;
    }


    for (uint32_t c = 0; c < (volume || !read_base ? 0u : layers); ++c) {
      const uint64_t address = dir_base + c * stride_face;
      if (!ReadLevel(address, tiled, pitch_blocks, uint64_t(std::max(pitch_blocks, blocks_x)) * tf.bytes, tf,
                     base_ox, base_oy, blocks_x, blocks_y, blocks_x_host, data.data() + bytes_layer * c)) {
        Warn(33, "texture outside memory");
        if (!texture.image.prepared) {
          slot = 0;
        }
        return;
      }
    }


    for (uint32_t n = 1; n < texture.levels; ++n) {
      const uint32_t s = level_packed == 0 ? 0 : std::min(n, level_packed);
      uint32_t ox = 0;
      uint32_t oy = 0;
      if (n >= level_packed) {
        OffsetPacked(width, height, tf.block, n, ox, oy);
      }
      const uint32_t bx_guest = (std::max(width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_guest = (std::max(height >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
      for (uint32_t c = 0; c < layers; ++c) {
        const uint64_t address = dir_mips + regions[s].displacement + c * regions[s].stride;
        if (!ReadLevel(address, tiled, regions[s].pitch_blocks, regions[s].row_bytes, tf, ox, oy,
                       std::min(bx_guest, bx_host), std::min(by_guest, by_host), bx_host,
                       data.data() + source_offsets[n] + bytes_layer_level[n] * c)) {
          Warn(39, "mip level outside memory: left at zero");
          break;
        }
      }
    }
    if (prepare_new_) {
      CutSubstage(34, t_new);
    }
    const auto order = static_cast<xenos::Endian>((f[1] >> 6) & 0x3);

    if (tiled_fast_ > 0 && order != xenos::Endian::kNone && (tf.unit_order == 2 || tf.unit_order == 4)) {


      ++orders_seen_;
      const bool check = orders_seen_ <= kLevelsACheck || orders_seen_ % kCheckOneOfEvery == 0;
      if (check) {
        check_tiled_ = data;
      }
      ChangeOrderBytes(data.data(), data.size(), tf.unit_order, uint32_t(order));
      if (check) {
        ++orders_checked_;
        UsualOrder(check_tiled_, tf.unit_order, order);
        if (check_tiled_ != data) {
          REXLOG_ERROR("[native] C3: the fast byte-order change does NOT match (unit {}, order {}, {} bytes). The fast "
                       "byte order and untiling are switched OFF.",
                       tf.unit_order, uint32_t(order), data.size());
          data.swap(check_tiled_);
          tiled_fast_ = 0;
        } else if (orders_checked_ == kLevelsACheck) {
          REXLOG_INFO("[native] C3: fast byte-order change: {} textures checked against the usual one, all equal.",
                      orders_checked_);
        }
      }
    } else {
      UsualOrder(data, tf.unit_order, order);
    }
    if (diag_mips_ && texture.levels > 1 && read_base && !volume) {
      ReviewMips(base, format, width, height, tf, texture, source_offsets, bytes_layer_level, data);
    }


    const auto start_hash_data = std::chrono::steady_clock::now();
    const uint64_t hash = texture.image.prepared ? XXH3_64bits(data.data(), data.size()) : 0;
    bo2zm::waits::g_ns_hash_data.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                      start_hash_data)
                     .count()),
        std::memory_order_relaxed);
    if (texture.image.prepared && hash == texture.hash) {
      texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
      texture.next = frame_ + texture.interval;
      valid_until = texture.next - 1;
      return;
    }

    if (comparison_texture_ == &texture) {
      AnnotateComparisonHash(texture, data);
    }
    std::array<uint32_t, 16> host_offsets = source_offsets;
    if (decode_bc) {
      bo2zm::texture::BcMipUpload converted;
      if (!bo2zm::texture::ConvertBcMips(bc_format, texture.image.width, texture.image.height,
                                         layers, texture.levels, data, converted,
                                         bo2zm::texture::kBcUploadLimit, bc_storage)) {
        RemoveContentTexture(texture, key);
        texture.hash_raw = previous_hash_raw;
        texture.sample_valid = false;
        texture.check_epoch = 0;
        texture.interval = 1; texture.next = frame_ + 1;
        texture.staging_failed_frame = frame_;
        slot = 0; valid_until = frame_;
        Warn(440, "BC mip conversion failed (layout, allocation or 256 MiB staging limit)");
        return;
      }
      for (uint32_t n = 0; n < texture.levels; ++n)
        host_offsets[n] = uint32_t(converted.layout.pixel_offsets[n]);
      data.swap(converted.pixels);
    }
    texture.offset_level = host_offsets;
    texture.interval = 1;
    texture.next = frame_ + 1;
    texture.hash = hash;
    texture.data.swap(data);
    texture.needs_upload = true;
    textures_a_upload_.push_back(&texture);
    bo2zm::waits::g_textures_uploads.fetch_add(1, std::memory_order_relaxed);
    bo2zm::waits::g_bytes_uploaded.fetch_add(texture.data.size(), std::memory_order_relaxed);
    bytes_upload += (texture.data.size() + 3) & ~size_t(3);
    if (prepare_new_) {
      CutSubstage(35, t_new);
    }
  }


  static constexpr uint32_t kReuseLookMax = 8;
  static constexpr uint32_t kReuseDetails = 8;

  bool DiagReuse() {
    if (diag_reuse_ < 0) {
      diag_reuse_ = REXCVAR_GET(bo2zm_native_diag_reuse) ? 1 : 0;
      REXLOG_INFO("[native] C3: measuring new textures with the content of another live one (bo2zm_native_diag_reuse) = "
                  "{}",
                  diag_reuse_ ? "YES (counting only: changes nothing)" : "no");
    }
    return diag_reuse_ > 0;
  }


  static uint64_t KeyContentOf(uint64_t shape_content, VkFormat format, uint32_t levels, uint32_t width,
                               uint32_t height, uint32_t layers, uint64_t hash_raw) {
    const uint64_t shape_parts[4] = {shape_content, (uint64_t(format) << 32) | levels,
                                     (uint64_t(width) << 32) | height, layers};
    return XXH3_64bits_withSeed(shape_parts, sizeof(shape_parts), hash_raw) | 1;
  }
  static uint64_t KeyContent(const CachedTexture& texture) {
    return KeyContentOf(texture.shape_content, texture.image.format, texture.levels, texture.image.width,
                        texture.image.height, texture.layers, texture.hash_raw);
  }


  bool ContentIndexed() { return DiagReuse() || TextureReuse(); }


  void RemoveContentTexture(CachedTexture& texture, uint64_t key) {
    if (!texture.key_content) {
      return;
    }
    const auto [since, until] = live_per_content_.equal_range(texture.key_content);
    for (auto it = since; it != until; ++it) {
      if (it->second == key) {
        live_per_content_.erase(it);
        break;
      }
    }
    if (live_per_content_.find(texture.key_content) == live_per_content_.end() && contents_different_) {
      --contents_different_;
    }
    texture.key_content = 0;
  }


  void AnnotateContentTexture(CachedTexture& texture, uint64_t key) {
    if (!ContentIndexed()) {
      return;
    }
    RemoveContentTexture(texture, key);
    const uint64_t content = KeyContent(texture);
    if (texture.content_per_measure) {
      texture.content_per_measure = false;
      ++reuse_new_;
      reuse_bytes_new_ += texture.bytes;
      const CachedTexture* equal = nullptr;
      const CachedTexture* cold = nullptr;
      uint32_t observed = 0;
      const auto [since, until] = live_per_content_.equal_range(content);
      for (auto it = since; it != until; ++it) {
        if (observed++ >= kReuseLookMax) {
          ++reuse_no_look_;
          break;
        }
        const auto other = textures_.find(it->second);
        if (other == textures_.end() || &other->second == &texture) {
          continue;
        }
        const CachedTexture& t = other->second;
        if (t.key_content != content || t.image.image == VK_NULL_HANDLE || !t.image.prepared ||
            t.hash_job) {
          continue;
        }
        equal = &t;
        if (t.frame != UINT64_MAX && t.frame + kFramesNoUseForRelease < frame_) {
          cold = &t;
          break;
        }
      }
      if (equal) {
        ++reuse_match_;
        reuse_bytes_match_ += texture.bytes;
      }
      if (cold) {
        ++reuse_cold_;
        reuse_bytes_cold_ += texture.bytes;
      }
      if (equal && reuse_details_ < kReuseDetails) {
        ++reuse_details_;
        const CachedTexture& t = cold ? *cold : *equal;
        BO2ZM_REPORT_RING("[native] C3 reuse by content (measurement only): new texture {:08X} ({}x{}, "
                             "VkFormat {}, {} levels, {} layers, {} KB) has the same content and shape as {:08X}, {}",
                             texture.address, texture.image.width, texture.image.height,
                             uint32_t(texture.image.format), texture.levels, texture.layers, texture.bytes >> 10,
                             t.address,
                             cold ? fmt::format("not checked for {} frames", frame_ - t.frame)
                                  : std::string("which is still in use"));
      }
    }
    if (live_per_content_.find(content) == live_per_content_.end()) {
      ++contents_different_;
    }
    live_per_content_.emplace(content, key);
    texture.key_content = content;
  }


  void ReportReuse() {
    if (diag_reuse_ <= 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_reuse_ < std::chrono::seconds(10)) {
      return;
    }
    report_reuse_ = now;
    if (reuse_new_) {
      const uint64_t pointed = uint64_t(live_per_content_.size());
      BO2ZM_REPORT_RING(
          "[native] C3 reuse by content (measurement only), last 10 s: {} new textures with a hash ({:.1f} "
          "MB); {} with the same content and shape as another live one ({:.1f} MB), {} of them with one not checked for "
          "more than {} frames ({:.1f} MB); {} with more than {} equal ones without a full look | in the cache: {} "
          "textures with a hash in {} different contents ({} repeated)",
          reuse_new_, double(reuse_bytes_new_) / 1048576.0, reuse_match_,
          double(reuse_bytes_match_) / 1048576.0, reuse_cold_, kFramesNoUseForRelease,
          double(reuse_bytes_cold_) / 1048576.0, reuse_no_look_, kReuseLookMax, pointed,
          contents_different_, pointed - std::min(pointed, contents_different_));
    }
    reuse_new_ = 0;
    reuse_match_ = 0;
    reuse_cold_ = 0;
    reuse_no_look_ = 0;
    reuse_bytes_new_ = 0;
    reuse_bytes_match_ = 0;
    reuse_bytes_cold_ = 0;
  }


  void ReportSampling(std::chrono::steady_clock::time_point now) {
    if (now - report_sampling_ < std::chrono::seconds(30)) {
      return;
    }
    if (report_sampling_ != std::chrono::steady_clock::time_point{}) {
      BO2ZM_REPORT_RING("[native] C3 hash sampling: {} rechecks settled by the sample ({:.1f} MB read and {:.1f} MB not "
                  "read), {} with a changed sample and {} with both hashes since the previous line; {} with both since "
                  "the start",
                  samples_hits_, double(bytes_sample_) / 1048576.0,
                  double(bytes_saved_sample_) / 1048576.0, samples_different_, samples_with_two_,
                  samples_checked_);
    }
    report_sampling_ = now;
    samples_hits_ = 0;
    samples_different_ = 0;
    samples_with_two_ = 0;
    bytes_sample_ = 0;
    bytes_saved_sample_ = 0;
  }


  void ReviewMips(uint32_t base, uint32_t format, uint32_t width, uint32_t height, const HostTextureFormat& tf,
                   const CachedTexture& texture, const std::array<uint32_t, 16>& source_offsets,
                   const std::array<size_t, 16>& bytes_layer_level,
                   const std::vector<uint8_t>& data) {
    const bool dxt = tf.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || tf.format == VK_FORMAT_BC2_UNORM_BLOCK ||
                     tf.format == VK_FORMAT_BC3_UNORM_BLOCK;
    if (!dxt && tf.format != VK_FORMAT_R8G8B8A8_UNORM) {
      return;
    }
    const auto average_of = [&](uint32_t n, std::array<double, 3>& rgb) {
      const size_t start = source_offsets[n];
      const size_t bytes = bytes_layer_level[n];
      if (start + bytes > data.size() || bytes < 4u * tf.bytes) {
        return false;
      }
      rgb = {0.0, 0.0, 0.0};
      size_t count = 0;
      for (size_t i = 0; i + tf.bytes <= bytes; i += tf.bytes) {
        const uint8_t* p = data.data() + start + i;
        if (dxt) {
          const uint8_t* color = tf.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? p : p + 8;
          for (uint32_t k = 0; k < 2; ++k) {
            const uint16_t v = uint16_t(color[2 * k] | (color[2 * k + 1] << 8));
            rgb[0] += ((v >> 11) & 31) * (255.0 / 31.0);
            rgb[1] += ((v >> 5) & 63) * (255.0 / 63.0);
            rgb[2] += (v & 31) * (255.0 / 31.0);
          }
          count += 2;
        } else {
          rgb[0] += p[0];
          rgb[1] += p[1];
          rgb[2] += p[2];
          count += 1;
        }
      }
      for (double& c : rgb) {
        c /= double(count);
      }
      return true;
    };
    std::array<double, 3> base_rgb{};
    if (!average_of(0, base_rgb)) {
      return;
    }
    ++mips_reviewed_;
    std::string levels = fmt::format("[0]={:.0f}/{:.0f}/{:.0f}", base_rgb[0], base_rgb[1], base_rgb[2]);
    double worst = 0.0;
    for (uint32_t n = 1; n < texture.levels; ++n) {
      std::array<double, 3> rgb{};
      if (!average_of(n, rgb)) {
        break;
      }
      for (uint32_t c = 0; c < 3; ++c) {
        worst = std::max(worst, std::abs(rgb[c] - base_rgb[c]));
      }
      levels += fmt::format(" [{}]={:.0f}/{:.0f}/{:.0f}", n, rgb[0], rgb[1], rgb[2]);
    }
    if (worst > 32.0) {
      if (++mips_rare_ <= 12) {
        REXLOG_WARN("[native] C3 diag mips: texture {:08X} {}x{} format {} with {} levels: one level is {:.0f} away "
                    "from the base; RGB averages {}",
                    base, width, height, format, texture.levels, worst, levels);
      }
    }
    if (mips_reviewed_ % 200 == 0) {
      REXLOG_INFO("[native] C3 diag mips: {} textures reviewed, {} with some level more than 32/255 away from the base",
                  mips_reviewed_, mips_rare_);
    }
  }


  bool ReadLevel(uint64_t address, bool tiled, uint32_t pitch_blocks, uint64_t row_bytes,
                 const HostTextureFormat& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                 uint8_t* target) {
    if (!bx || !by) {
      return true;
    }
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t bound =
        tiled ? uint64_t(std::max<int64_t>(OffsetTiled2D(int32_t((ox + bx + 31) & ~31u),
                                                                     int32_t((oy + by + 31) & ~31u),
                                                                     pitch_blocks, log2),
                                             0))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (address + bound > kMemoryPhysical) {
      return false;
    }
    const uint8_t* source = memory_->TranslatePhysical(uint32_t(address));
    if (!tiled) {
      for (uint32_t y = 0; y < by; ++y) {
        std::memcpy(target + size_t(y) * bx_host * tf.bytes,
                    source + uint64_t(oy + y) * row_bytes + uint64_t(ox) * tf.bytes, size_t(bx) * tf.bytes);
      }
      return true;
    }

    if (tiled_fast_ < 0) {
      tiled_fast_ = REXCVAR_GET(bo2zm_native_fast_tiling) ? 1 : 0;
      REXLOG_INFO("[native] C3: fast untiling (bo2zm_native_fast_tiling) = {}",
                  tiled_fast_ ? "YES, checking the first levels against the usual one" : "no");
    }
    if (tiled_fast_ && tf.bytes == (1u << log2)) {
      switch (log2) {
        case 0: UntileLevel<0>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 1: UntileLevel<1>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 2: UntileLevel<2>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 3: UntileLevel<3>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        default: UntileLevel<4>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
      }


      ++levels_seen_;
      if (levels_seen_ <= kLevelsACheck || levels_seen_ % kCheckOneOfEvery == 0) {
        ++levels_checked_;
        check_tiled_.assign(size_t(by) * bx_host * tf.bytes, 0);
        for (uint32_t y = 0; y < by; ++y) {
          std::memcpy(check_tiled_.data() + size_t(y) * bx_host * tf.bytes,
                      target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes);
        }
        ReadTiledLevelUsual(source, pitch_blocks, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
        bool equal = true;
        for (uint32_t y = 0; y < by && equal; ++y) {
          equal = std::memcmp(check_tiled_.data() + size_t(y) * bx_host * tf.bytes,
                                target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes) == 0;
        }
        if (!equal) {
          REXLOG_ERROR("[native] C3: the fast untiling does NOT match ({}x{} blocks of {} bytes, pitch {}, from {},{}). "
                       "It is switched OFF and the usual one is used.",
                       bx, by, tf.bytes, pitch_blocks, ox, oy);
          tiled_fast_ = 0;
        } else if (levels_checked_ == kLevelsACheck) {
          REXLOG_INFO("[native] C3: fast untiling: {} levels checked against the usual one, all equal. The fast one "
                      "stays.",
                      levels_checked_);
        }
      }
      return true;
    }
    ReadTiledLevelUsual(source, pitch_blocks, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
    return true;
  }


  static void UsualOrder(std::vector<uint8_t>& data, uint32_t unit, xenos::Endian order) {
    if (unit == 2 && order != xenos::Endian::kNone) {
      for (size_t i = 0; i + 1 < data.size(); i += 2) {
        uint16_t v;
        std::memcpy(&v, data.data() + i, 2);
        v = xenos::GpuSwap(v, order);
        std::memcpy(data.data() + i, &v, 2);
      }
    } else if (unit == 4 && order != xenos::Endian::kNone) {
      for (size_t i = 0; i + 3 < data.size(); i += 4) {
        uint32_t v;
        std::memcpy(&v, data.data() + i, 4);
        v = xenos::GpuSwap(v, order);
        std::memcpy(data.data() + i, &v, 4);
      }
    }
  }


  static void ReadTiledLevelUsual(const uint8_t* source, uint32_t pitch_blocks, uint32_t log2, uint32_t bytes,
                                        uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                                        uint8_t* target) {
    for (uint32_t y = 0; y < by; ++y) {
      for (uint32_t x = 0; x < bx; ++x) {
        const int32_t displacement =
            OffsetTiled2D(int32_t(ox + x), int32_t(oy + y), pitch_blocks, log2);
        std::memcpy(target + (size_t(y) * bx_host + x) * bytes, source + displacement, bytes);
      }
    }
  }


  bool UpdateVolume(CachedTexture& texture, const uint32_t* f, const HostTextureFormat& tf, uint32_t blocks_x,
                         uint32_t blocks_y, uint32_t depth3d, uint32_t blocks_x_host, uint32_t blocks_y_host,
                         uint32_t pitch_blocks, uint64_t& valid_until, uint64_t& bytes_upload) {
    if (volume_incremental_ < 0) {
      volume_incremental_ = REXCVAR_GET(bo2zm_native_incremental_volume) ? 1 : 0;
    }
    const auto order = static_cast<xenos::Endian>((f[1] >> 6) & 0x3);
    const bool tiled = (f[0] >> 31) & 0x1;
    if (volume_incremental_ == 0 || !tiled || tf.block != 1 || tf.bytes == 0 || tf.bytes > 16 ||
        (16 % tf.bytes) != 0 || texture.levels != 1 || texture.layers != 1 || !texture.image.prepared ||
        texture.in_flight || texture.hash_job || texture.needs_upload || blocks_x > blocks_x_host ||
        blocks_y > blocks_y_host ||
        (order != xenos::Endian::kNone &&
         !(tiled_fast_ > 0 && (tf.unit_order == 2 || tf.unit_order == 4)))) {
      return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const uint32_t address = uint32_t(uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF);
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const int64_t bound_signed = OffsetTiled3D(int32_t((blocks_x + 31) & ~31u),
                                                            int32_t((blocks_y + 31) & ~31u),
                                                            int32_t((depth3d + 3) & ~3u), pitch_blocks, blocks_y, log2);
    if (bound_signed <= 0 || uint64_t(address) + uint64_t(bound_signed) > kMemoryPhysical) {
      return false;
    }
    const uint32_t bound = uint32_t(bound_signed);
    const uint8_t* live = memory_->TranslatePhysical(address);
    const uint32_t pages = (bound + 4095) / 4096;
    IncrementalVolume* v = texture.incremental.get();
    const bool new_value = !v || v->address != address || v->bound != bound || v->depth3d != depth3d ||
                       v->height != blocks_y_host || v->width_host != blocks_x_host;
    std::vector<uint32_t>& dirty = pages_dirty_;
    dirty.clear();
    if (new_value) {
      texture.incremental = std::make_unique<IncrementalVolume>();
      v = texture.incremental.get();
      v->address = address;
      v->bound = bound;
      v->depth3d = depth3d;
      v->height = blocks_y_host;
      v->width_host = blocks_x_host;
      v->row = blocks_x_host * tf.bytes;
      v->slice = v->row * blocks_y_host;
      v->unit_order = tf.unit_order;
      v->order = uint32_t(order);
      v->raw.assign(live, live + bound);
      v->host.assign(size_t(v->slice) * depth3d, 0);
      const uint32_t span = 16 / tf.bytes;
      std::vector<uint32_t> count(pages + 1, 0);
      for (uint32_t z = 0; z < depth3d; ++z) {
        for (uint32_t y = 0; y < blocks_y; ++y) {
          for (uint32_t x = 0; x < blocks_x; x += span) {
            const int32_t displacement = OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z), pitch_blocks,
                                                                   blocks_y, log2);
            const uint32_t bytes = std::min(span, blocks_x - x) * tf.bytes;
            if (displacement < 0 || uint64_t(displacement) + bytes > bound) {
              texture.incremental.reset();
              return false;
            }
            v->spans.push_back({uint32_t(displacement), z * v->slice + y * v->row + x * tf.bytes,
                                 uint16_t(bytes), uint16_t(x), uint16_t(y), uint16_t(z)});
            for (uint32_t p = uint32_t(displacement) / 4096; p <= (uint32_t(displacement) + bytes - 1) / 4096; ++p) {
              ++count[p];
            }
          }
        }
      }
      v->start_page.assign(pages + 1, 0);
      for (uint32_t p = 0; p < pages; ++p) {
        v->start_page[p + 1] = v->start_page[p] + count[p];
      }
      v->spans_page.assign(v->start_page[pages], 0);
      std::vector<uint32_t> full(v->start_page.begin(), v->start_page.end() - 1);
      for (uint32_t i = 0; i < v->spans.size(); ++i) {
        const IncrementalVolume::Span& t = v->spans[i];
        for (uint32_t p = t.source / 4096; p <= (t.source + t.bytes - 1) / 4096; ++p) {
          v->spans_page[full[p]++] = i;
        }
      }
      uint32_t used = 0;
      for (const IncrementalVolume::Span& t : v->spans) {
        used = std::max(used, (t.source + t.bytes + 4095) / 4096);
      }
      v->pages = used;
      v->hashes.assign(used, 0);
      for (uint32_t p = 0; p < used; ++p) {
        const uint32_t since = p * 4096;
        v->hashes[p] = XXH3_64bits(v->raw.data() + since, std::min<uint32_t>(4096, bound - since));
        dirty.push_back(p);
      }
      ++volume_states_;

      {
        std::vector<uint32_t> watched;
        for (uint32_t p = 0; p < v->pages; ++p) {
          if (v->start_page[p + 1] > v->start_page[p]) {
            watched.push_back(address + p * 4096);
          }
        }
        bo2zm::debug::WatchPhysicalPages(watched);
      }
    } else {


      const int32_t mode = REXCVAR_GET(bo2zm_native_volume_by_hash);
      const bool by_hash = mode == 2 ? (volume_detect_turn_++ & 1) != 0 : mode != 0;
      const auto before_detect = std::chrono::steady_clock::now();


      const bool by_marks = bo2zm::volume_writes::UseMarks();
      const bool check_marks = by_marks && (++volume_mark_looks_ <= 300 || (volume_mark_looks_ & 63) == 0);
      if (by_marks) {
        marked_pages_.clear();
        bo2zm::volume_writes::TakeMarks(address, v->pages, marked_pages_);
        page_marked_.assign(v->pages, 0);
        for (const uint32_t p : marked_pages_) {
          if (p >= v->pages || v->start_page[p + 1] == v->start_page[p]) {
            continue;
          }
          page_marked_[p] = 1;
          const uint32_t since = p * 4096;
          const uint32_t bytes = std::min<uint32_t>(4096, bound - since);
          std::memcpy(v->raw.data() + since, live + since, bytes);
          v->hashes[p] = XXH3_64bits(v->raw.data() + since, bytes);
          dirty.push_back(p);
        }

        for (const uint32_t p : volume_mark_suspects_) {
          if (p < v->pages && page_marked_[p]) {
            ++volume_mark_late_;
          } else {
            ++volume_mark_misses_;
            bo2zm::volume_writes::Missed(address + p * 4096);
          }
        }
        volume_mark_suspects_.clear();
      } else {
        volume_mark_suspects_.clear();
      }
      if (!by_marks || check_marks) {
        for (uint32_t p = 0; p < v->pages; ++p) {
          if (v->start_page[p + 1] == v->start_page[p]) {
            continue;
          }
          const uint32_t since = p * 4096;
          const uint32_t bytes = std::min<uint32_t>(4096, bound - since);
          const bool changed = by_hash ? XXH3_64bits(live + since, bytes) != v->hashes[p]
                                       : std::memcmp(live + since, v->raw.data() + since, bytes) != 0;
          if (changed) {
            if (by_marks) {
              volume_mark_suspects_.push_back(p);
            }
            std::memcpy(v->raw.data() + since, live + since, bytes);
            v->hashes[p] = XXH3_64bits(v->raw.data() + since, bytes);
            dirty.push_back(p);
          }
        }
      }
      const int slot = by_marks ? 2 : by_hash ? 1 : 0;
      volume_detect_ns_[slot] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - before_detect)
                                              .count());
      ++volume_detects_[slot];
      volume_mark_checked_ += check_marks ? 1 : 0;
    }
    if (dirty.empty()) {
      ++volume_no_changes_;
      texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
      texture.next = frame_ + texture.interval;
      valid_until = texture.next - 1;
      volume_ns_ += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      return true;
    }

    const uint32_t width_img = texture.image.width;
    const uint32_t height_img = texture.image.height;
    const uint32_t tiles_x = (width_img + 31) / 32;
    const uint32_t tiles_y = (height_img + 31) / 32;
    std::vector<uint8_t>& tiles = volume_tiles_;
    tiles.assign(size_t(tiles_x) * tiles_y * depth3d, 0);
    for (const uint32_t p : dirty) {
      for (uint32_t k = v->start_page[p]; k < v->start_page[p + 1]; ++k) {
        const IncrementalVolume::Span& t = v->spans[v->spans_page[k]];
        uint8_t* target = v->host.data() + t.target;
        std::memcpy(target, v->raw.data() + t.source, t.bytes);
        if (order != xenos::Endian::kNone) {
          ChangeOrderBytes(target, t.bytes, tf.unit_order, uint32_t(order));
        }
        const uint32_t x_end = t.x + t.bytes / tf.bytes - 1;
        tiles[(size_t(t.z) * tiles_y + (t.y >> 5)) * tiles_x + (t.x >> 5)] = 1;
        tiles[(size_t(t.z) * tiles_y + (t.y >> 5)) * tiles_x + (x_end >> 5)] = 1;
      }
    }


    ++volume_updates_;
    ++volume_updated_report_;


    if (volume_updates_ <= 50 || volume_updates_ % 2048 == 0) {
      std::vector<uint8_t>& expected = volume_check_;
      expected.assign(v->host.size(), 0);
      for (uint32_t z = 0; z < depth3d; ++z) {
        for (uint32_t y = 0; y < blocks_y; ++y) {
          for (uint32_t x = 0; x < blocks_x; ++x) {
            const int32_t d = OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z), pitch_blocks, blocks_y,
                                                      log2);
            std::memcpy(expected.data() + z * v->slice + y * v->row + x * tf.bytes, v->raw.data() + d, tf.bytes);
          }
        }
      }
      if (order != xenos::Endian::kNone) {
        ChangeOrderBytes(expected.data(), expected.size(), tf.unit_order, uint32_t(order));
      }
      ++volume_checked_;
      if (expected != v->host) {
        REXLOG_ERROR("[native] C3: the 3D texture by pages {:08X} does NOT match the whole read ({} pages changed). "
                     "Updating by pages is switched OFF (bo2zm_native_incremental_volume).",
                     address, dirty.size());
        volume_incremental_ = 0;
        texture.incremental.reset();
        return false;
      }
    }


    texture.data.clear();
    texture.regions_partial.clear();
    for (uint32_t z = 0; z < depth3d; ++z) {
      for (uint32_t ly = 0; ly < tiles_y; ++ly) {
        for (uint32_t lx = 0; lx < tiles_x;) {
          if (!tiles[(size_t(z) * tiles_y + ly) * tiles_x + lx]) {
            ++lx;
            continue;
          }
          uint32_t end = lx + 1;
          while (end < tiles_x && tiles[(size_t(z) * tiles_y + ly) * tiles_x + end]) {
            ++end;
          }
          const uint32_t x0 = lx * 32, y0 = ly * 32;
          const uint32_t width = std::min(end * 32, width_img) - x0;
          const uint32_t rows = std::min(y0 + 32, height_img) - y0;
          VkBufferImageCopy region{};
          region.bufferOffset = texture.data.size();
          region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.imageOffset = {int32_t(x0), int32_t(y0), int32_t(z)};
          region.imageExtent = {width, rows, 1};
          for (uint32_t r = 0; r < rows; ++r) {
            const uint8_t* since = v->host.data() + z * v->slice + (y0 + r) * v->row + x0 * tf.bytes;
            texture.data.insert(texture.data.end(), since, since + size_t(width) * tf.bytes);
          }
          texture.regions_partial.push_back(region);
          lx = end;
        }
      }
    }
    texture.hash = 0;
    texture.interval = 1;
    texture.next = frame_ + 1;
    texture.needs_upload = true;
    textures_a_upload_.push_back(&texture);
    bo2zm::waits::g_textures_uploads.fetch_add(1, std::memory_order_relaxed);
    bo2zm::waits::g_bytes_uploaded.fetch_add(texture.data.size(), std::memory_order_relaxed);
    bytes_upload += (texture.data.size() + 3) & ~size_t(3);
    volume_pages_ += dirty.size();
    volume_bytes_ += texture.data.size();
    volume_ns_ += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    return true;
  }
  int32_t volume_incremental_ = -1;
  std::vector<uint32_t> pages_dirty_;
  std::vector<uint8_t> volume_tiles_;
  std::vector<uint8_t> volume_check_;
  uint64_t volume_states_ = 0, volume_no_changes_ = 0, volume_updates_ = 0, volume_checked_ = 0;
  uint64_t volume_updated_report_ = 0;
  uint64_t volume_pages_ = 0, volume_bytes_ = 0, volume_ns_ = 0;

  uint64_t volume_detect_ns_[3] = {}, volume_detects_[3] = {};
  uint32_t volume_detect_turn_ = 0;


  uint64_t volume_mark_looks_ = 0;
  uint64_t volume_mark_checked_ = 0;
  uint64_t volume_mark_misses_ = 0;
  uint64_t volume_mark_late_ = 0;
  std::vector<uint32_t> marked_pages_;
  std::vector<uint8_t> page_marked_;
  std::vector<uint32_t> volume_mark_suspects_;

  bool ReadVolume(const uint32_t* f, const HostTextureFormat& tf, uint32_t blocks_x,
                   uint32_t blocks_y, uint32_t depth3d, uint32_t blocks_x_host,
                   uint32_t blocks_y_host, uint32_t pitch_blocks, bool tiled,
                   std::vector<uint8_t>& data) {
    const uint64_t address = uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF;
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                             : tf.bytes >= 2 ? 1 : 0;
    const uint64_t row = uint64_t((pitch_blocks + 31) & ~uint32_t(31)) * tf.bytes;
    const uint64_t slice = row * ((blocks_y + 31) & ~uint32_t(31));
    const uint64_t bound =
        tiled ? uint64_t(std::max<int64_t>(
                      OffsetTiled3D(int32_t((blocks_x + 31) & ~31u),
                                              int32_t((blocks_y + 31) & ~31u),
                                              int32_t((depth3d + 3) & ~3u), pitch_blocks,
                                              blocks_y, log2),
                      0))
                : slice * depth3d;
    if (address + bound > kMemoryPhysical) {
      return false;
    }
    const uint8_t* source = memory_->TranslatePhysical(uint32_t(address));
    const size_t bytes_slice = size_t(blocks_x_host) * blocks_y_host * tf.bytes;


    static uint32_t volumes_checked = 0;
    const bool check = volumes_checked < 20;
    std::vector<uint8_t> snapshot;
    if (check) {
      snapshot.assign(source, source + std::min<uint64_t>(bound + 64, kMemoryPhysical - address));
      source = snapshot.data();
    }


    if (tiled && tf.bytes >= 1 && tf.bytes <= 16 && (16 % tf.bytes) == 0 && !check) {
      const uint32_t span = 16 / tf.bytes;
      for (uint32_t z = 0; z < depth3d; ++z) {
        for (uint32_t y = 0; y < blocks_y; ++y) {
          uint8_t* row = data.data() + bytes_slice * z + size_t(y) * blocks_x_host * tf.bytes;
          for (uint32_t x = 0; x < blocks_x; x += span) {
            const uint64_t displacement = uint64_t(
                OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z), pitch_blocks, blocks_y, log2));
            const uint32_t n = std::min(span, blocks_x - x);
            std::memcpy(row + size_t(x) * tf.bytes, source + displacement, size_t(n) * tf.bytes);
          }
        }
      }
      return true;
    }
    for (uint32_t z = 0; z < depth3d; ++z) {
      for (uint32_t y = 0; y < blocks_y; ++y) {
        for (uint32_t x = 0; x < blocks_x; ++x) {
          const uint64_t displacement =
              tiled ? uint64_t(OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z),
                                                         pitch_blocks, blocks_y, log2))
                      : z * slice + y * row + uint64_t(x) * tf.bytes;
          std::memcpy(data.data() + bytes_slice * z + (size_t(y) * blocks_x_host + x) * tf.bytes,
                      source + displacement, tf.bytes);
        }
      }
    }
    if (check && tiled && tf.bytes >= 1 && tf.bytes <= 16 && (16 % tf.bytes) == 0) {
      ++volumes_checked;
      const uint32_t span = 16 / tf.bytes;
      std::vector<uint8_t> fast(data.size(), 0);
      for (uint32_t z = 0; z < depth3d; ++z) {
        for (uint32_t y = 0; y < blocks_y; ++y) {
          uint8_t* row = fast.data() + bytes_slice * z + size_t(y) * blocks_x_host * tf.bytes;
          for (uint32_t x = 0; x < blocks_x; x += span) {
            const uint64_t displacement = uint64_t(
                OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z), pitch_blocks, blocks_y, log2));
            std::memcpy(row + size_t(x) * tf.bytes, source + displacement,
                        size_t(std::min(span, blocks_x - x)) * tf.bytes);
          }
        }
      }
      const size_t util = bytes_slice * depth3d;
      const bool equal = std::memcmp(fast.data(), data.data(), std::min(util, data.size())) == 0;
      if (!equal || volumes_checked == 20) {
        REXLOG_INFO("[native] C3 volume in 16-byte spans ({}x{}x{}, {} bytes per texel): {} ({} checked)",
                    blocks_x, blocks_y, depth3d, tf.bytes, equal ? "same as texel by texel" : "DIFFERENT",
                    volumes_checked);
      }
    }
    return true;
  }


  static VkImageCreateInfo InfoImageTexture(VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                                             uint32_t depth3d, uint32_t levels) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = depth3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.flags = layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.format = format;
    info.extent = {width, height, depth3d ? depth3d : 1};
    info.mipLevels = std::max(levels, 1u);
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
  }


  bool TryAllocateTexture(NativeImage& image, const VkImageCreateInfo& info) {
    bool reserved = false;
    if (pool_textures_.IsActive()) {
      VkImage image_pool = VK_NULL_HANDLE;
      if (dfn_.vkCreateImage(device_, &info, nullptr, &image_pool) == VK_SUCCESS) {
        VkMemoryRequirements req{};
        dfn_.vkGetImageMemoryRequirements(device_, image_pool, &req);
        VkDeviceMemory block = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint32_t id_block = 0xFFFFFFFFu;
        if (pool_textures_.Reserve(req, block, offset, id_block) &&
            dfn_.vkBindImageMemory(device_, image_pool, block, offset) == VK_SUCCESS) {
          image.image = image_pool;
          image.guest_memory = VK_NULL_HANDLE;
          image.pool_block = id_block;
          reserved = true;
        } else {
          if (id_block != 0xFFFFFFFFu) {
            pool_textures_.Free(id_block);
          }
          dfn_.vkDestroyImage(device_, image_pool, nullptr);
          pool_textures_.AnnotateDedicated();
        }
      }
    }
    if (!reserved) {
      image.pool_block = 0xFFFFFFFFu;
      reserved = rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
          image.guest_memory);
    }
    return reserved;
  }


  bool CreateTexture(NativeImage& image, VkFormat format, uint32_t width, uint32_t height,
                    uint32_t layers = 1, uint32_t depth3d = 0, uint32_t levels = 1, bool emergency = true) {
    const VkImageCreateInfo info = InfoImageTexture(format, width, height, layers, depth3d, levels);


    bool reserved = TryAllocateTexture(image, info);
    if (reserved && test_no_memory_every_ > 0 &&
        ++texture_reserves_ % uint64_t(test_no_memory_every_) == 0) {

      DestroyImage(image);
      reserved = false;
      REXLOG_INFO("[native] C3: test, running out of memory is faked at allocation {}", texture_reserves_);
    }
    if (!reserved) {


      if (!emergency || !ReleaseTextureMemory(&image) || !TryAllocateTexture(image, info)) {
        REXLOG_ERROR("[native] C3: out of GPU memory for a {}x{} texture ({} levels, {} layers) and {}; {} textures and "
                     "{} MB remain",
                     width, height, std::max(levels, 1u), layers,
                     emergency ? "releasing the cache was not enough"
                                : "without releasing the cache (from CollectBindings; it is recreated at its next "
                                  "check)",
                     textures_.size(), bytes_textures_ >> 20);
        return false;
      }
    }
    image.width = width;
    image.height = height;
    image.format = format;
    image.prepared = false;
    return true;
  }

  bool UploadTexture(CachedTexture& texture) {
    const VkCommandBuffer upload = context_->CommandsUpload();
    if (!upload || !texture.needs_upload) {
      return upload != VK_NULL_HANDLE;
    }
    VkDeviceSize offset;
    if (!texture.regions_partial.empty()) {
      Reserve(texture.data.size(), 16, offset);
      std::memcpy(upload_data_ + offset, texture.data.data(), texture.data.size());
      for (VkBufferImageCopy& region : texture.regions_partial) {
        region.bufferOffset += offset;
      }
      dfn_.vkCmdCopyBufferToImage(upload, upload_, texture.image.image, VK_IMAGE_LAYOUT_GENERAL,
                                  uint32_t(texture.regions_partial.size()), texture.regions_partial.data());
      texture.regions_partial.clear();
      texture.data.clear();
      texture.needs_upload = false;
      ++uploads_texture_;
      return true;
    }


    if (texture.hash_job && texture.hash_job != kJobHashPublished) {
      const size_t bytes = BytesHashPlanned(texture);
      Reserve(bytes, 16, offset);
      PublishHash(texture, upload_data_ + offset, bytes);
    } else {
      Reserve(texture.data.size(), 16, offset);
      std::memcpy(upload_data_ + offset, texture.data.data(), texture.data.size());
    }


    if (texture.in_flight && !PostponeCopy(texture, offset, upload)) {
      CollectBindings(true);
    }
    if (!texture.in_flight && texture.image.image != VK_NULL_HANDLE) {
      if (!texture.image.prepared) {
        Barrier(upload, texture.image.image, texture.layers);
        texture.image.prepared = true;
      }
      RecordCopyTexture(upload, texture, offset);
    }
    texture.needs_upload = false;


    if (texture.data.capacity() > temporal_.capacity()) {
      temporal_.swap(texture.data);
    }
    std::vector<uint8_t>().swap(texture.data);
    ++uploads_texture_;
    return true;
  }


  void RecordCopyTexture(VkCommandBuffer upload, const CachedTexture& texture, VkDeviceSize offset) {
    std::array<VkBufferImageCopy, 16> copies{};
    if (!bo2zm::texture::TextureCopyRegions(texture.image.width, texture.image.height, texture.layers,
          texture.depth3d ? texture.depth3d : 1, texture.levels, texture.offset_level, offset, copies)) {
      REXLOG_ERROR("[native] invalid texture upload regions; copy skipped");
      return;
    }
    dfn_.vkCmdCopyBufferToImage(upload, upload_, texture.image.image, VK_IMAGE_LAYOUT_GENERAL,
                                texture.levels, copies.data());
  }


  VkResult CreateViewTexture(VkImage image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap,
                             VkImageView& view) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = heap == 2   ? VK_IMAGE_VIEW_TYPE_CUBE
                    : heap == 1 ? VK_IMAGE_VIEW_TYPE_3D
                                  : VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.components = MappingComponents(swizzle, swizzle_host);
    info.subresourceRange = {IsDepth(format)
                                 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                 : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                             0, VK_REMAINING_MIP_LEVELS, 0, heap == 2 ? 6u : 1u};
    return dfn_.vkCreateImageView(device_, &info, nullptr, &view);
  }


  static uint64_t KeyView(VkImage image, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap) {
    return XXH3_64bits_withSeed(&image, sizeof(image),
                                (uint64_t(heap) << 40) | (uint64_t(swizzle) << 16) | swizzle_host);
  }


  uint32_t SlotView(VkImage image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host,
                       uint32_t heap = 0) {
    const uint64_t key = KeyView(image, swizzle, swizzle_host, heap);
    if (const auto it = views_.find(key); it != views_.end()) {
      return it->second.slot;
    }


    if (!images_in_flight_.empty() && images_in_flight_.count(image)) {
      return SlotViewInFlight(key, image, format, swizzle, swizzle_host, heap);
    }
    View view;
    view.image = image;
    view.heap = heap;
    if (CreateViewTexture(image, format, swizzle, swizzle_host, heap, view.view) != VK_SUCCESS) {
      Warn(34, "could not create a texture's view");
      return 0;
    }
    view.slot = ReserveSlot(heap);
    if (!view.slot) {
      dfn_.vkDestroyImageView(device_, view.view, nullptr);
      Warn(35, "texture heap full");
      ++slots_full_;
      return 0;
    }
    WriteImage(heap, view.slot, view.view);
    views_.emplace(key, view);
    views_per_image_[image].push_back(key);
    return view.slot;
  }


  uint32_t SlotSampler(const uint32_t* f, bool point, uint32_t mip_min) {


    {
      const uint32_t aniso = (f[3] >> 25) & 0x7;
      const int32_t bias = int32_t(f[4] << 10) >> 22;
      const uint32_t mip_max = (f[4] >> 6) & 0xF;
      const uint32_t index_bias = uint32_t(bias + 512);
      if (!((aniso_seen_ >> aniso) & 1u) || !biases_seen_[index_bias] || !((mip_max_seen_ >> mip_max) & 1u)) {
        aniso_seen_ |= 1u << aniso;
        biases_seen_[index_bias] = true;
        mip_max_seen_ |= 1u << mip_max;
        REXLOG_INFO("[native] C4 filters requested by the game (new value): anisotropic {}, mip bias {:.3f}, maximum "
                    "mip level {}, mag/min/mip filters {}/{}/{}{}",
                    aniso, float(bias) / 32.0f, mip_max, (f[3] >> 19) & 0x3, (f[3] >> 21) & 0x3,
                    (f[3] >> 23) & 0x3, point ? " (point sampling)" : "");
      }
    }


    uint32_t field_filters = (f[3] >> 19) & 0xFFF;
    if (mip_point_test_ && ((field_filters >> 4) & 0x3) == 1) {
      field_filters &= ~(0x3u << 4);
    }


    const uint32_t field_mips = bo2zm::native::SamplerMipRange((f[4] >> 2) & 0xFF, mip_min);


    const int32_t bias_lod = int32_t(f[4] << 10) >> 22;
    const uint64_t key = uint64_t(((f[0] >> 10) & 0x1FF) | (field_filters << 9) | (field_mips << 21) |
                                    ((f[5] & 0x3) << 29) | (point ? 0x80000000u : 0u)) |
                           (uint64_t(uint32_t(bias_lod) & 0x3FF) << 32);
    if (const auto it = samplers_.find(key); it != samplers_.end()) {
      return it->second.second;
    }
    static constexpr VkSamplerAddressMode kModes[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,          VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
    const bool mirror = vulkan_device_->properties().samplerMirrorClampToEdge;
    const auto mode = [&](uint32_t value) {
      const VkSamplerAddressMode m = kModes[value & 0x7];
      return m == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && !mirror
                 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                 : m;
    };
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter =
        ((f[3] >> 19) & 0x3) == 1 && !point ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter =
        ((f[3] >> 21) & 0x3) == 1 && !point ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const uint32_t mip = (field_filters >> 4) & 0x3;
    info.mipmapMode =
        mip == 1 && !point ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = mode(f[0] >> 10);
    info.addressModeV = mode(f[0] >> 13);
    info.addressModeW = mode(f[0] >> 16);
    info.minLod = float(field_mips & 0xF);


    info.maxLod = mip == 2 ? info.minLod + 0.25f : std::max(info.minLod, float((f[4] >> 6) & 0xF));
    info.borderColor = (f[5] & 0x3) == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                         : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;


    const auto& properties_game = vulkan_device_->properties();
    if (!point) {

      info.mipLodBias = std::clamp(float(bias_lod) / 32.0f, -15.0f, 15.0f);
      const uint32_t aniso_game = (f[3] >> 25) & 0x7;
      if (aniso_game >= 2 && aniso_game <= 5 && properties_game.samplerAnisotropy &&
          info.minFilter == VK_FILTER_LINEAR && info.maxLod > info.minLod) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(float(1u << (aniso_game - 1)), properties_game.maxSamplerAnisotropy);
      }
    }


    {
      static const uint32_t aniso_requested = uint32_t(std::max(0, int32_t(REXCVAR_GET(bo2zm_native_anisotropic))));
      const auto& properties = vulkan_device_->properties();
      if (aniso_requested > 1 && properties.samplerAnisotropy && info.minFilter == VK_FILTER_LINEAR &&
          info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR && info.maxLod > info.minLod) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::max(info.maxAnisotropy,
                                      std::max(1.0f, std::min(float(aniso_requested), properties.maxSamplerAnisotropy)));
        if (!aniso_annotated_) {
          aniso_annotated_ = true;
          REXLOG_INFO("[native] C4 anisotropic filtering {} on the linear samplers with mips (requested {}, device cap "
                      "{}); single-level and point ones unchanged",
                      info.maxAnisotropy, aniso_requested, properties.maxSamplerAnisotropy);
        }
      }
    }
    VkSampler sampler;
    if (dfn_.vkCreateSampler(device_, &info, nullptr, &sampler) != VK_SUCCESS) {
      Warn(36, "could not create a sampler");
      return 0;
    }
    const uint32_t slot = ReserveSlot(3);
    if (!slot) {
      dfn_.vkDestroySampler(device_, sampler, nullptr);
      Warn(37, "sampler heap full");
      return 0;
    }
    WriteSampler(slot, sampler);
    samplers_.emplace(key, std::make_pair(sampler, slot));
    return slot;
  }


  uint32_t TestScissor() {
    const int32_t category = REXCVAR_GET(bo2zm_native_test_scissor);
    if (category <= 0) {
      return 0;
    }
    const int32_t alternate = REXCVAR_GET(bo2zm_native_test_scissor_alternate_s);
    if (alternate <= 0) {
      return uint32_t(category);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_ps_)
                              .count();
    const bool with_scissor = (seconds / alternate) % 2 == 1;
    if (with_scissor != alternation_scissor_annotated_) {
      alternation_scissor_annotated_ = with_scissor;
      REXLOG_INFO("[native] C2 test scissor: {}", with_scissor ? "clipping to 1x1" : "normal");
    }
    return with_scissor ? uint32_t(category) : 0;
  }


  void VerdictVegetation(const DrawRequest& p) {
    vegetation_computed_ = false;
    if (!(p.vegetation_game & kVegHas)) {
      return;
    }
    const uint32_t* r = p.registers;
    VegetationDetail a;
    if (r) {
      a.mode = r[gr::XE_GPU_REG_RB_MODECONTROL];
      a.mask = r[gr::XE_GPU_REG_RB_COLOR_MASK];
      a.control = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    }
    a.vs = p.vs ? int32_t(p.vs->number) : -1;
    a.ps = p.ps ? int32_t(p.ps->number) : -1;
    if (p.ps) {
      a.outputs = p.ps->outputs;
      a.discards = p.ps->discards;
    }
    if (r && p.vs && p.ps && (a.mode & 0x7) == uint32_t(xenos::EdramMode::kColorDepth)) {
      bool color = false;
      for (uint32_t i = 0; i < 4; ++i) {
        color = color || (((a.mask >> (i * 4)) & 0xF) && ((p.ps->outputs >> i) & 0x1));
      }
      const bool test_alpha = ((a.control >> 3) & 0x1) && (a.control & 0x7) != 7;
      a.structural = !color && (test_alpha || p.ps->discards || (p.ps->outputs & 0x10));
    }
    a.settings = (cvars_per_frame_ ? ps_only_alpha_frame_ : PsOnlyAlpha()) && no_vegetation_;
    a.occlusion = occlusion_open_;
    vegetation_ring_ = a.structural && a.settings && !a.occlusion;
    vegetation_computed_ = true;
    vegetation_flags_ = p.vegetation_game;
    vegetation_detail_ = a;
    AnnotateVegetationRing(p.vegetation_game, a);
  }


  bool PsOnlyAlpha() {
    const int32_t alternate = REXCVAR_GET(bo2zm_native_ps_alpha_only_alternate_s);
    if (alternate <= 0) {
      return REXCVAR_GET(bo2zm_native_ps_alpha_only);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_ps_)
                              .count();
    const bool only_alpha = (seconds / alternate) % 2 == 1;
    if (only_alpha != alternation_only_alpha_annotated_) {
      alternation_only_alpha_annotated_ = only_alpha;
      REXLOG_INFO("[native] C2 color writes: {}",
                  only_alpha ? "removed in colorless passes" : "all");
    }
    return only_alpha;
  }


  bool NoPsNoColor() {
    const int32_t alternate = REXCVAR_GET(bo2zm_native_no_ps_without_color_alternate_s);
    if (alternate <= 0) {
      return REXCVAR_GET(bo2zm_native_no_ps_without_color);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_ps_)
                              .count();
    const bool no_ps = (seconds / alternate) % 2 == 1;
    if (no_ps != alternation_ps_annotated_) {
      alternation_ps_annotated_ = no_ps;
      REXLOG_INFO("[native] C2 fragment stage: {}",
                  no_ps ? "removed in colorless draws" : "always built");
    }
    return no_ps;
  }


  static float MaxWhole(uint32_t format) {
    switch (xenos::TextureFormat(format)) {
      case xenos::TextureFormat::k_8:
      case xenos::TextureFormat::k_8_A:
      case xenos::TextureFormat::k_8_B:
      case xenos::TextureFormat::k_8_8:
      case xenos::TextureFormat::k_8_8_8_8:
      case xenos::TextureFormat::k_8_8_8_8_A:
      case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
        return 255.0f;
      case xenos::TextureFormat::k_2_10_10_10:
      case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
        return 1023.0f;
      case xenos::TextureFormat::k_16:
      case xenos::TextureFormat::k_16_16:
      case xenos::TextureFormat::k_16_16_16_16:
        return 65535.0f;
      default:
        return 1.0f;
    }
  }
  static void WriteScale(uint32_t* shared, uint32_t reg_index, const uint32_t* fetch) {
    float scale = std::ldexp(1.0f, int32_t(fetch[3] << 13) >> 26);
    if (fetch[3] & 0x1) {
      scale *= MaxWhole(fetch[1] & 0x3F);
    }
    std::memcpy(shared + kWordScale + reg_index, &scale, sizeof(scale));
  }


  static void WriteScaleCoords(uint32_t* shared, uint32_t reg_index, const uint32_t* fetch,
                                        uint32_t width, uint32_t height) {
    float scale[2] = {1.0f, 1.0f};
    if (((fetch[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked)) {
      const uint32_t width_guest = (fetch[2] & 0x1FFF) + 1;
      const uint32_t height_guest = ((fetch[2] >> 13) & 0x1FFF) + 1;
      if (width > width_guest) {
        scale[0] = float(width_guest) / float(width);
      }
      if (height > height_guest) {
        scale[1] = float(height_guest) / float(height);
      }
    }
    std::memcpy(shared + kWordScaleCoords + reg_index * 2, scale, sizeof(scale));
  }

  static void WriteInvSize(uint32_t* shared, uint32_t reg_index, uint32_t width, uint32_t height) {
    const float inv[2] = {width ? 1.0f / float(width) : 0.0f, height ? 1.0f / float(height) : 0.0f};
    std::memcpy(shared + kWordInvSize + reg_index * 2, inv, sizeof(inv));
  }

  bool BeginPass(const uint32_t* r, const uint64_t keys[5], uint32_t pitch,
                   uint64_t key_pass, const ShaderInput* ps) {
    start_pass_ = time_ ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    if (!PrepareEmpty()) {
      return false;
    }
    std::array<NativeImage*, 5> images{};
    for (uint32_t i = 0; i < 4; ++i) {
      if (keys[i]) {
        images[i] = context_->TargetColor(uint32_t(keys[i] >> 24) & 0xFFF,
                                              uint32_t(keys[i] >> 16) & 0xF, pitch);
        if (!images[i]) {
          return Reject(40, "there is no color target");
        }
        if (i == 0) context_->BeginColorPass(*images[i], r, ps);
      }
    }
    if (keys[4]) {
      images[4] = context_->TargetDepth(uint32_t(keys[4] >> 24) & 0xFFF,
                                                  uint32_t(keys[4] >> 16) & 0x1, pitch);
      if (!images[4]) {
        return Reject(41, "there is no depth target");
      }
    }
    (void)r;

    const auto after_targets = time_ ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    if (time_) {
      stages_ns_[9] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          after_targets - start_pass_).count());
    }
    uint32_t width = UINT32_MAX, height = UINT32_MAX;
    std::array<VkImageView, 5> views{};
    uint32_t formats[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
      if (images[i]) {
        width = std::min(width, images[i]->width);
        height = std::min(height, images[i]->height);
        views[i] = images[i]->view;
        formats[i] = uint32_t(images[i]->format);
      }
    }

    const bool is_shadows = pitch >= 1600 && formats[4] && !formats[0] && !formats[1] &&
                            !formats[2] && !formats[3];


    pass_overlay_shadow_ = images[4] != nullptr && context_->OverlayShadowPass(images[4], is_shadows);


    category_pass_ = TargetCategory(pitch, keys);
    context_->MarkGpu(category_pass_);
    const bool shadows_no_load =
        is_shadows && REXCVAR_GET(bo2zm_native_shadow_pass_without_load);

    const bool depth_only_no_load =
        !shadows_no_load && REXCVAR_GET(bo2zm_native_depth_only_no_load) && formats[4] && !formats[0] &&
        !formats[1] && !formats[2] && !formats[3] &&
        pitch >= uint32_t(REXCVAR_GET(bo2zm_native_depth_only_no_load_min_pitch));


    std::array<bool, 5> clears_fused{};
    std::array<VkClearValue, 5> clear_values{};


    std::array<v043::RectRequest, 5> rect_clears{};
    std::array<VkClearValue, 5> rect_values{};
    context_->TakeDeferredClears(images, width, height, clears_fused, clear_values, rect_clears, rect_values);
    uint32_t mode_load = (shadows_no_load || depth_only_no_load) ? kLoadIgnore : kLoadRead;
    bool any_clear_fused = false;
    for (uint32_t i = 0; i < 5; ++i) {
      if (clears_fused[i] && images[i]) {
        mode_load = v040::WithClear(mode_load, i);
        any_clear_fused = true;
      }
    }


    uint32_t height_useful = height;
    if (area_util_ && !any_clear_fused) {
      const auto it = height_useful_.find(pitch);
      if (it != height_useful_.end() && it->second < height) {
        height_useful = it->second;
      }
    }


    bool any_rect = false;
    for (uint32_t i = 0; i < 5; ++i) {
      if (rect_clears[i].active && !images[i]) {
        rect_clears[i].active = false;
      }
      any_rect = any_rect || rect_clears[i].active;
    }
    v043::PassPlan rect_plan;
    rect_plan.area_height = any_clear_fused ? height : height_useful;
    if (any_rect) {
      rect_plan = v043::PlanRectClears(width, height, height_useful, any_clear_fused, rect_clears);
      for (uint32_t i = 0; i < 5; ++i) {
        if (rect_plan.fuse[i]) {
          mode_load = v040::WithClear(mode_load, i);
          clears_fused[i] = true;
          clear_values[i] = rect_values[i];
          bo2zm::native::performance::g_guest_clears_rect_fused.fetch_add(1, std::memory_order_relaxed);
        } else if (rect_clears[i].active && !rect_plan.in_pass[i]) {


          bo2zm::native::performance::g_clears_dropped.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }


    uint32_t width_pass = width;
    pass_fitted_ = false;
    if (any_rect && !any_clear_fused && bo2zm::native::v046::g_fit_enabled.load(std::memory_order_relaxed)) {
      namespace v = bo2zm::native::v046;
      int32_t chosen = -1;
      for (uint32_t i = 0; i < 5 && chosen < 0; ++i) {
        if (rect_clears[i].active && rect_plan.in_pass[i] && rect_clears[i].aspects == v043::FullAspects(i)) {
          chosen = int32_t(i);
        }
      }
      const auto extent = drawn_extent_.find(pitch);
      if (chosen >= 0 && extent != drawn_extent_.end()) {
        const v043::Rect r = rect_clears[uint32_t(chosen)].area;
        if (v::FitAreaToClear(extent->second, r.x, r.y, r.width, r.height, width, rect_plan.area_height)) {
          bool fits = true;
          for (uint32_t i = 0; i < 5; ++i) {
            if (rect_clears[i].active && rect_plan.in_pass[i] && !v043::RectInside(rect_clears[i].area, r.width,
                                                                                  r.height)) {
              fits = false;
            }
          }
          if (fits) {
            for (uint32_t i = 0; i < 5; ++i) {
              const auto& o = rect_clears[i];
              if (o.active && rect_plan.in_pass[i] && o.aspects == v043::FullAspects(i) && o.area.x == 0 &&
                  o.area.y == 0 && o.area.width == r.width && o.area.height == r.height) {
                mode_load = v040::WithClear(mode_load, i);
                clears_fused[i] = true;
                clear_values[i] = rect_values[i];
                rect_plan.in_pass[i] = false;
                v::g_fit_fused.fetch_add(1, std::memory_order_relaxed);
              }
            }
            v::g_fit_texels_saved.fetch_add(
                uint64_t(width) * rect_plan.area_height - uint64_t(r.width) * r.height, std::memory_order_relaxed);
            v::g_fit_passes.fetch_add(1, std::memory_order_relaxed);
            width_pass = r.width;
            rect_plan.area_height = r.height;
            pass_fitted_ = true;
          }
        }
      }
    }

    for (uint32_t i = 0; any_rect && i < 5; ++i) {
      if (!rect_clears[i].active || !rect_plan.in_pass[i]) continue;
      namespace v = bo2zm::native::v046;
      const auto& rc = rect_clears[i].area;
      const uint32_t why = rc.x != 0 || rc.y != 0                  ? v::kMissRectOrigin
                           : rc.width != width                     ? v::kMissRectWidth
                           : rc.height != rect_plan.area_height    ? v::kMissRectShort
                                                                   : v::kMissRectAspects;
      v::g_clear_miss[why].fetch_add(1, std::memory_order_relaxed);
      v::g_clear_miss_sample[why].store(
          (uint64_t(rc.width) << 48) | (uint64_t(rc.height) << 32) | (uint64_t(width) << 16) |
              uint64_t(rect_plan.area_height),
          std::memory_order_relaxed);
    }

    static const bool fuse_aspect_clears =
        REXCVAR_GET(bo2zm_perf_prepass_fuse_rect_clears) || REXCVAR_GET(bo2zm_perf_v046);
    if (fuse_aspect_clears && any_rect && rect_clears[4].active && rect_plan.in_pass[4] && images[4]) {
      const auto& rc = rect_clears[4];
      const bo2zm::native::v046::AspectFuse fuse = bo2zm::native::v046::PlanDepthAspectFuse(
          rc.area.x, rc.area.y, rc.area.width, rc.area.height, uint32_t(rc.aspects), width, rect_plan.area_height,
          FormatHasStencil(VkFormat(formats[4])));
      if (fuse.fuse) {
        mode_load |= fuse.bits;
        clears_fused[4] = true;
        clear_values[4] = rect_values[4];
        rect_plan.in_pass[4] = false;
        (fuse.bits & bo2zm::native::v046::kLoadDepthClearOnly ? bo2zm::native::v046::g_fused_depth_only
                                                              : bo2zm::native::v046::g_fused_stencil_only)
            .fetch_add(1, std::memory_order_relaxed);
      }
    }


    {
      namespace e = bo2zm::native::edram;
      e::Tracker& tracker = e::Get();
      const uint64_t area = uint64_t(width_pass) * rect_plan.area_height;
      for (uint32_t i = 0; i < 5; ++i) {
        if (!images[i]) continue;
        const uint64_t handle = uint64_t(images[i]->image);
        const uint32_t m = v040::AttachmentLoadMode(mode_load, i, kLoadClear);
        const bool one_aspect = i == 4 && (mode_load & (bo2zm::native::v046::kLoadDepthClearOnly |
                                                        bo2zm::native::v046::kLoadStencilClearOnly));
        const uint64_t bytes = area * e::BytesPerTexel(formats[i]);
        if ((m == kLoadClear || m == kLoadIgnore) && !one_aspect) {
          tracker.Clear(handle);
        } else {
          e::g_bytes_loaded.fetch_add(bytes, std::memory_order_relaxed);
          if (!tracker.Read(handle) && edram_mismatch_logged_++ == 0) {
            REXLOG_ERROR("[perf] 0.47 EDRAM dead stores: MISMATCH, pass on {:08X} (attachment {}) loads a surface whose "
                         "previous store was skipped. Off for the rest of the session",
                         uint32_t(keys[i < 4 ? i : 4] >> 24) & 0xFFF, i);
          }
        }
        if (tracker.ShouldDiscard(handle)) {
          mode_load |= 1u << (e::kStoreDiscardShift + i);
          e::g_bytes_not_stored.fetch_add(bytes, std::memory_order_relaxed);
        } else {
          e::g_bytes_stored.fetch_add(bytes, std::memory_order_relaxed);
        }
      }
      pass_store_discard_ = (mode_load >> e::kStoreDiscardShift) & 0x1F;
    }


    {
      namespace v = bo2zm::native::v055;
      pass_has_depth_ = images[4] != nullptr;
      const bool depth_cleared_here =
          v040::AttachmentLoadMode(mode_load, 4, kLoadClear) != kLoadRead ||
          (mode_load & (bo2zm::native::v046::kLoadDepthClearOnly | bo2zm::native::v046::kLoadStencilClearOnly)) != 0 ||
          (any_rect && rect_plan.in_pass[4]);
      pass_opened_nonwriting_ = pass_has_depth_ && !draw_writes_ds_ && !depth_cleared_here;
      pass_depth_readonly_ = false;
      pass_ds_written_ = depth_cleared_here;
      pass_readonly_blocked_ = false;
      pass_learn_key_ = draw_learn_key_;
      pass_readonly_texels_ = 0;
      if (pass_opened_nonwriting_ && readonly_depth_ == 1) {
        v::g_readonly_candidates.fetch_add(1, std::memory_order_relaxed);
        if (!((pass_store_discard_ >> 4) & 0x1)) {
          const auto learned = readonly_learn_.find(pass_learn_key_);
          if (learned != readonly_learn_.end() && v::OpenReadOnly(learned->second)) {
            mode_load |= v::kLoadDepthStoreNone;
            pass_depth_readonly_ = true;
            pass_readonly_texels_ = uint64_t(width_pass) * rect_plan.area_height;
          }
        }
      }
    }
    const bool any_fused_total = std::any_of(clears_fused.begin(), clears_fused.end(), [](bool b) { return b; });
    const VkRenderPass pass = PassOf(formats, mode_load);
    if (pass == VK_NULL_HANDLE) {
      return Reject(42, "could not create the render pass");
    }
    const VkFramebuffer framebuffer = FramebufferOf(pass, views, width, height);
    if (framebuffer == VK_NULL_HANDLE) {
      return Reject(43, "could not create the framebuffer");
    }
    if (time_) {
      stages_ns_[10] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - after_targets).count());
    }
    const auto before_open = time_ ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    const VkCommandBuffer cmd = context_->CommandsJob();
    if (!cmd) {
      return false;
    }
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;

    const uint32_t height_pass = rect_plan.area_height;
    start.renderArea.extent = {width_pass, height_pass};

    std::array<VkClearValue, 5> clear_list{};
    if (any_fused_total) {
      start.clearValueCount = v040::PackClearValues(formats, clears_fused, clear_values, clear_list);
      start.pClearValues = clear_list.data();
    }


    for (const NativeImage* image : images) {
      if (image) {
        context_->AnnotatePassArea(image, width, height_pass);
      }
    }
    context_->ProfilePassBegin(category_pass_, width_pass, height_pass, key_pass, formats, mode_load);

    stats_pass_ = context_->BeginStats(category_pass_);
    const auto before_start = std::chrono::steady_clock::now();
    dfn_.vkCmdBeginRenderPass(cmd, &start, VK_SUBPASS_CONTENTS_INLINE);


    if (any_rect) {
      for (uint32_t i = 0; i < 5; ++i) {
        if (!rect_plan.in_pass[i]) {
          continue;
        }
        VkClearAttachment attachment{};
        attachment.aspectMask = VkImageAspectFlags(rect_clears[i].aspects);
        attachment.colorAttachment = i < 4 ? v043::ColorAttachmentIndex(formats, i) : 0;
        attachment.clearValue = rect_values[i];
        VkClearRect rectangle{};
        rectangle.rect.offset = {rect_clears[i].area.x, rect_clears[i].area.y};
        rectangle.rect.extent = {rect_clears[i].area.width, rect_clears[i].area.height};
        rectangle.baseArrayLayer = 0;
        rectangle.layerCount = 1;
        dfn_.vkCmdClearAttachments(cmd, 1, &attachment, 1, &rectangle);
        bo2zm::native::performance::g_guest_clears_rect_at_begin.fetch_add(1, std::memory_order_relaxed);
      }
    }
    ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_start).count());
    if (time_) {
      stages_ns_[11] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - before_open).count());
    }
    pass_active_ = true;
    ++passes_begun_;
    texels_passes_ += uint64_t(width) * height_pass;
    bo2zm::native::performance::g_passes.fetch_add(1, std::memory_order_relaxed);
    bo2zm::native::performance::g_pass_texels.fetch_add(uint64_t(width) * height_pass, std::memory_order_relaxed);
    {

      const uint32_t category = TargetCategory(pitch, keys);
      texels_per_category_[category] += uint64_t(width) * height_pass;
      ++passes_per_category_[category];
    }
    {


      uint32_t scale_class = display::kClassNone;
      for (const NativeImage* image : images) {
        if (image) {
          scale_class = std::min<uint32_t>(image->scale_class, display::kClasses - 1);
          break;
        }
      }
      display::g_pass_texels[scale_class].fetch_add(uint64_t(width) * height_pass, std::memory_order_relaxed);
      display::g_passes[scale_class].fetch_add(1, std::memory_order_relaxed);
    }
    pass_commands_ = cmd;
    draws_in_pass_ = 0;
    pass_key_ = key_pass;
    std::memcpy(pass_keys_, keys, sizeof(pass_keys_));
    pass_keys_valid_ = true;
    ++pass_series_;
    pass_generation_ = context_->GenerationCommands();
    pass_width_ = width;
    pass_height_ = height;
    pass_area_height_ = height_pass;
    pass_area_width_ = width_pass;
    pass_pitch_ = pitch;
    for (uint32_t i = 0; i < 5; ++i) {
      pass_images_[i] = images[i];
      pass_handles_[i] = images[i] ? images[i]->image : VK_NULL_HANDLE;
    }


    pass_scale_x_ = (is_shadows && pitch && width && width != pitch)
                         ? float(width) / float(pitch)
                         : 1.0f;
    pass_scale_y_ = pass_scale_x_;
    if (pass_scale_x_ == 1.0f) {
      for (const NativeImage* image : images) {
        if (image && ((image->width_guest && image->width_guest != image->width) ||
                      (image->height_guest && image->height_guest != image->height))) {
          pass_scale_x_ = v041::PassScale(image->width, image->width_guest);
          pass_scale_y_ = v041::PassScale(image->height, image->height_guest);
          break;
        }
      }
    }
    std::copy(std::begin(formats), std::end(formats), std::begin(pass_formats_));
    pass_rp_ = pass;


    if (!pipeline_between_passes_) {
      pipeline_bound_ = VK_NULL_HANDLE;
    } else if (pipeline_bound_ != VK_NULL_HANDLE) {
      ++passes_with_pipeline_;
    }
    state_recorded_ = false;
    if (pipeline_bound_ == VK_NULL_HANDLE) {
      eds_valid_ = false;
    }
    return true;
  }


  enum : uint32_t { kLoadRead = 0, kLoadIgnore = 1, kLoadClear = 2 };


  static constexpr uint32_t kLoadModeMask = v040::kLoadModeMask;
  static constexpr uint32_t kLoadClearShift = v040::kLoadClearShift;

  VkRenderPass PassOf(const uint32_t formats[5], uint32_t mode_load = kLoadRead) {


    const uint64_t key =
        XXH3_64bits_withSeed(formats, sizeof(uint32_t) * 5, mode_load);
    if (const auto it = passes_.find(key); it != passes_.end()) {
      return it->second;
    }
    const VkRenderPass pass = CreatePass(formats, mode_load);
    if (pass == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
    passes_.emplace(key, pass);
    return pass;
  }


  static void Indices16Scalar(const uint8_t* data, uint32_t count, uint16_t* output, bool flip, uint32_t& min,
                                 uint32_t& max) {
    min = 0xFFFF;
    max = 0;
    if (flip) {
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t raw;
        std::memcpy(&raw, data + size_t(i) * 2, 2);
        const uint16_t v = std::byteswap(raw);
        output[i] = v;
        min = std::min<uint32_t>(min, v);
        max = std::max<uint32_t>(max, v);
      }
    } else {
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t v;
        std::memcpy(&v, data + size_t(i) * 2, 2);
        output[i] = v;
        min = std::min<uint32_t>(min, v);
        max = std::max<uint32_t>(max, v);
      }
    }
  }


  static void Indices16Neon(const uint8_t* data, uint32_t count, uint16_t* output, bool flip, uint32_t& min,
                              uint32_t& max) {
#if defined(__aarch64__)
    uint32_t i = 0;
    uint32_t mn = 0xFFFF;
    uint32_t mx = 0;
    if (count >= 16) {
      uint16x8_t min_a = vdupq_n_u16(0xFFFF);
      uint16x8_t min_b = min_a;
      uint16x8_t max_a = vdupq_n_u16(0);
      uint16x8_t max_b = max_a;

      if (flip) {
        for (; i + 16 <= count; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(data + size_t(i) * 2)));
          const uint16x8_t b = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(data + size_t(i) * 2 + 16)));
          vst1q_u16(output + i, a);
          vst1q_u16(output + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      } else {
        for (; i + 16 <= count; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vld1q_u8(data + size_t(i) * 2));
          const uint16x8_t b = vreinterpretq_u16_u8(vld1q_u8(data + size_t(i) * 2 + 16));
          vst1q_u16(output + i, a);
          vst1q_u16(output + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      }
      mn = vminvq_u16(vminq_u16(min_a, min_b));
      mx = vmaxvq_u16(vmaxq_u16(max_a, max_b));
    }
    for (; i < count; ++i) {
      uint16_t v;
      std::memcpy(&v, data + size_t(i) * 2, 2);
      if (flip) {
        v = std::byteswap(v);
      }
      output[i] = v;
      mn = std::min<uint32_t>(mn, v);
      mx = std::max<uint32_t>(mx, v);
    }
    min = mn;
    max = mx;
#else
    Indices16Scalar(data, count, output, flip, min, max);
#endif
  }


  void Indices16(const uint8_t* data, uint32_t count, uint16_t* output, bool flip, uint32_t& min,
                   uint32_t& max) {
    if (indices_neon_ < 0) {
      indices_neon_ = REXCVAR_GET(bo2zm_native_indices_neon) ? 1 : 0;
      REXLOG_INFO("[native] C6 16-bit indices: {}",
                  indices_neon_ ? "with NEON; the first 20,000 draws are checked and then 1 in 4,096"
                                : "with the usual loop (bo2zm_native_indices_neon = false)");
    }
    if (indices_neon_ == 0) {
      Indices16Scalar(data, count, output, flip, min, max);
      return;
    }
    Indices16Neon(data, count, output, flip, min, max);
    const uint64_t n = ++indices_neon_draws_;
    if (n > 20000 && (n & 4095) != 0) {
      return;
    }
    indices_neon_test_.resize(count);
    uint32_t mn = 0;
    uint32_t mx = 0;
    Indices16Scalar(data, count, indices_neon_test_.data(), flip, mn, mx);
    ++indices_neon_checked_;
    if (mn != min || mx != max ||
        (count && std::memcmp(indices_neon_test_.data(), output, size_t(count) * 2) != 0)) {

      if (count) {
        std::memcpy(output, indices_neon_test_.data(), size_t(count) * 2);
      }
      min = mn;
      max = mx;
      indices_neon_ = 0;
      REXLOG_ERROR("[native] C6 16-bit indices: MISMATCH between NEON and the usual loop ({} indices, {}; min {} versus "
                   "{}, max {} versus {}). Off for the rest of the session: the usual loop",
                   count, flip ? "flipped" : "not flipped", min, mn, max, mx);
      return;
    }
    if (indices_neon_checked_ == 20000) {
      REXLOG_INFO("[native] C6 16-bit indices: 20,000 draws checked against the usual loop, 0 mismatches; "
                  "still checking 1 in 4,096");
    }
  }


  VkRenderPass CreatePass(const uint32_t formats[5], uint32_t mode_load) const {
    std::array<VkAttachmentDescription, 5> attachments{};
    std::array<VkAttachmentReference, 4> colors{};
    VkAttachmentReference depth{};
    uint32_t n = 0, n_colors = 0;
    for (uint32_t i = 0; i < 5; ++i) {
      if (!formats[i]) {
        continue;
      }
      VkAttachmentDescription& a = attachments[n];
      a.format = VkFormat(formats[i]);
      a.samples = VK_SAMPLE_COUNT_1_BIT;


      const uint32_t mode_attachment = v040::AttachmentLoadMode(mode_load, i, kLoadClear);


      a.loadOp = mode_attachment == kLoadIgnore ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                 : mode_attachment == kLoadClear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                    : VK_ATTACHMENT_LOAD_OP_LOAD;

      const bool discard_store = (mode_load >> (bo2zm::native::edram::kStoreDiscardShift + i)) & 0x1;
      a.storeOp = discard_store ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = i != 4                             ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                        : mode_attachment == kLoadClear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                           : VK_ATTACHMENT_LOAD_OP_LOAD;

      if (i == 4 && (mode_load & bo2zm::native::v046::kLoadDepthClearOnly)) {
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      }
      if (i == 4 && (mode_load & bo2zm::native::v046::kLoadStencilClearOnly)) {
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      }
      a.stencilStoreOp = i == 4 && !discard_store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;

      if (i == 4 && !discard_store && (mode_load & bo2zm::native::v055::kLoadDepthStoreNone)) {
        a.storeOp = VkAttachmentStoreOp(bo2zm::native::v055::kStoreOpNone);
        a.stencilStoreOp = VkAttachmentStoreOp(bo2zm::native::v055::kStoreOpNone);
      }
      a.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      if (i < 4) {
        colors[n_colors++] = {n, VK_IMAGE_LAYOUT_GENERAL};
      } else {
        depth = {n, VK_IMAGE_LAYOUT_GENERAL};
      }
      ++n;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = n_colors;
    subpass.pColorAttachments = colors.data();
    subpass.pDepthStencilAttachment = formats[4] ? &depth : nullptr;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = n;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    const int scene_sync_mode = REXCVAR_GET(bo2zm_native_scene_pass_mode);
    const auto dependencies = scene::ImageDependencies(scene_sync_mode);
    const bool patch_precise = REXCVAR_GET(bo2zm_native_scene_sync_precise);
    const auto patch_dependencies = scene::precise_patch::PassDependenciesFor(
        patch_precise, uint32_t(REXCVAR_GET(bo2zm_native_scene_sync_mask)));
    if (REXCVAR_GET(bo2zm_native_scene_sync)) {
      if (patch_precise) {


        info.dependencyCount = patch_dependencies.count;
        info.pDependencies = patch_dependencies.items.data();
      } else if (REXCVAR_GET(bo2zm_native_scene_pass_dependencies)) {
        scene::SelectImageDependencies(info, dependencies, scene_sync_mode, false,
                                       formats[4] != 0);
      }
    }
    VkRenderPass pass;
    if (dfn_.vkCreateRenderPass(device_, &info, nullptr, &pass) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return pass;
  }

  VkFramebuffer FramebufferOf(VkRenderPass pass, const std::array<VkImageView, 5>& views,
                              uint32_t width, uint32_t height) {
    struct Key {
      VkRenderPass pass;
      std::array<VkImageView, 5> views;
      uint32_t width, height;
    } key{pass, views, width, height};
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));
    if (const auto it = framebuffers_.find(hash); it != framebuffers_.end()) {
      return it->second;
    }
    std::array<VkImageView, 5> attachments{};
    uint32_t n = 0;
    for (VkImageView view : views) {
      if (view != VK_NULL_HANDLE) {
        attachments[n++] = view;
      }
    }
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = n;
    info.pAttachments = attachments.data();
    info.width = width;
    info.height = height;
    info.layers = 1;
    VkFramebuffer framebuffer;
    if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    framebuffers_.emplace(hash, framebuffer);
    framebuffers_views_[hash] = views;
    return framebuffer;
  }


  VkShaderModule ModuleVariant(size_t index, const uint32_t* spirv, size_t bytes) {
    if (!modules_variants_created_[index]) {
      modules_variants_created_[index] = true;
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = bytes;
      info.pCode = spirv;
      if (CreateProfileModule(info, &modules_variants_[index]) != VK_SUCCESS) {
        modules_variants_[index] = VK_NULL_HANDLE;
      }
    }
    return modules_variants_[index];
  }


  int GlowSky() {
    int mode = bo2zm::settings::GlowSky();
    const int32_t alternate = REXCVAR_GET(bo2zm_native_smooth_glow_alternate_s);
    if (alternate > 0) {
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - start_shadows_)
                                .count();
      mode = int((seconds / alternate) % 3);
    }
    mode = std::clamp(mode, 0, 2);
    if (mode != glow_annotated_) {
      glow_annotated_ = mode;
      static constexpr const char* kNames[3] = {"original", "natural", "soft"};
      REXLOG_INFO("[native] sky glow: {} (frame {})", kNames[mode], frame_);
    }
    return mode;
  }


  void ReportZEarly() {
    ++frames_z_;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report_z_).count() < 10) {
      return;
    }
    last_report_z_ = now;
    const uint64_t seen = frames_z_ - frames_z_previous_;
    const double frames = double(seen ? seen : 1);
    std::array<uint64_t, kZCounts> d{};
    uint64_t total = 0;
    for (uint32_t i = 0; i < kZCounts; ++i) {
      d[i] = counts_z_[i] - counts_z_previous_[i];
      counts_z_previous_[i] = counts_z_[i];
      total += d[i];
    }
    frames_z_previous_ = frames_z_;
    ReportSky(frames);
    if (!total) {
      return;
    }
    const uint64_t late = d[kZSet] + d[kZWritesZ] + d[kZStencil] + d[kZOcclusion];
    BO2ZM_REPORT_RING(
        "[native] C6 early Z per frame: {:.0f} draws take advantage of it, {:.0f} CANNOT because they write depth "
        "(those are the size of C88's prepass), {:.0f} because of stencil, {:.0f} inside an occlusion query; {:.0f} "
        "already tested before. Of those that forced shading first, {:.0f} % are fixed{}",
        double(d[kZSet]) / frames, double(d[kZWritesZ]) / frames,
        double(d[kZStencil]) / frames, double(d[kZOcclusion]) / frames,
        double(d[kZAlreadyEarly]) / frames,
        late ? 100.0 * double(d[kZSet]) / double(late) : 0.0,
        z_early_no_module_ ? fmt::format(" ({} shaders without a patched module)", z_early_no_module_)
                               : std::string());
    if (d[kInvisibleBlend] || d[kInvisibleAlpha] || d[kInvisibleOnlyColor]) {
      BO2ZM_REPORT_RING(
          "[native] C6 invisible draws per frame: {:.1f} with the blend that copies the target, {:.1f} with the alpha "
          "test at NEVER; {:.1f} keep only the depth",
          double(d[kInvisibleBlend]) / frames, double(d[kInvisibleAlpha]) / frames,
          double(d[kInvisibleOnlyColor]) / frames);
    }
  }


  void ReportSky(double frames) {
    const uint64_t postponed = skies_postponed_ - sky_postponed_previous_;
    const uint64_t seen = sky_seen_ - sky_seen_previous_;
    if (!seen) {
      return;
    }
    std::array<uint64_t, kSkyReasons> per_reason{};
    uint64_t emitted = 0;
    for (uint32_t i = 0; i < kSkyReasons; ++i) {
      per_reason[i] = sky_emitted_[i] - sky_emitted_previous_[i];
      sky_emitted_previous_[i] = sky_emitted_[i];
      emitted += per_reason[i];
    }
    const uint64_t position_sum = sky_position_sum_ - sky_position_sum_previous_;
    const uint64_t candidates = sky_candidates_ - sky_candidates_previous_;
    sky_candidates_previous_ = sky_candidates_;
    sky_seen_previous_ = sky_seen_;
    sky_postponed_previous_ = skies_postponed_;
    sky_position_sum_previous_ = sky_position_sum_;


    BO2ZM_REPORT_RING("[native] C6 sky: {:.2f} detected by hash and {:.2f} by the dome geometry (480 indices and first "
                "in the pass) per frame. The second has to be 1.00. GUARD: {} ({} frames with a dome looked at, {} with "
                "exactly one, maximum {} in one frame)",
                double(seen) / frames, double(candidates) / frames,
                sky_guard_ == kSkyPostponing  ? "PASSED, postponing"
                : sky_guard_ == kSkyDiscarded ? "REJECTED, nothing is postponed"
                                                     : "still observing",
                sky_guard_frames_, sky_guard_with_one_, sky_guard_max_);
    BO2ZM_REPORT_RING(
        "[native] C6 postponed sky ({}): {:.2f} detected per frame, {:.2f} really postponed ({} not postponable in "
        "total, {} with another sky already pending); emitted {} when a blended draw arrives, {} when an opaque draw "
        "that does not write Z arrives, {} when the pass closes, {} because of another sky; LOST {}; average position "
        "{:.0f} draws ahead of it (maximum {})",
        sky_postpone_on_ ? "on" : "OFF", double(seen) / frames,
        double(postponed) / frames, sky_no_postponable_, sky_two_in_pass_,
        per_reason[kSkyPerBlend], per_reason[kSkyPerNoZ], per_reason[kSkyAtPassEnd],
        per_reason[kSkyPerOtherSky], sky_lost_,
        emitted ? double(position_sum) / double(emitted) : 0.0, sky_position_max_);
  }


  void CloseGuardSkyFrame() {
    const uint32_t in_this = sky_guard_in_frame_;
    sky_guard_in_frame_ = 0;
    if (sky_guard_ != kSkyObserving) {
      return;
    }


    if (in_this == 0) {
      return;
    }
    sky_guard_max_ = std::max(sky_guard_max_, in_this);
    if (in_this == 1) {
      ++sky_guard_with_one_;
    }
    if (++sky_guard_frames_ < kSkyFramesTest) {
      return;
    }


    if (sky_guard_max_ == 1 && sky_guard_with_one_ >= kSkyFramesWithOne) {
      sky_guard_ = kSkyPostponing;
      REXLOG_INFO("[native] C6 sky: guard PASSED ({} of {} frames with exactly one dome, never two). The sky is "
                  "postponed from now on, without changing a pixel",
                  sky_guard_with_one_, sky_guard_frames_);
    } else {
      sky_guard_ = kSkyDiscarded;
      REXLOG_WARN("[native] C6 sky: guard NOT passed (maximum {} domes in one frame, {} of {} frames with exactly one; "
                  "{} were needed). Nothing is postponed: the image stays exactly as it was",
                  sky_guard_max_, sky_guard_with_one_, sky_guard_frames_,
                  kSkyFramesWithOne);
    }
  }

  VkResult CreateProfileModule(const VkShaderModuleCreateInfo& info, VkShaderModule* output) {
    if (ubo_only_ && (info.codeSize % 4 || !UboOnlySpirv({info.pCode, info.codeSize / 4}))) {
      *output = VK_NULL_HANDLE;
      REXLOG_ERROR("[native] shader rejected: Android requires a validated UBO-only library (no Int64, physical pointers or push constants)");
      return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return dfn_.vkCreateShaderModule(device_, &info, nullptr, output);
  }

  bool ShaderProbeFragmentSafe(const ShaderInput& input) {
    if (const auto it = probe_safe_shaders_.find(&input); it != probe_safe_shaders_.end()) return it->second;
    const auto spirv = input.shader ? bo2zm::shaderlib::Spirv(*input.shader) : std::vector<uint32_t>{};
    const bool safe = draw_batch::ShaderSafe(spirv) && shader_probe::CoverageSafe(spirv);
    probe_safe_shaders_.emplace(&input, safe);
    return safe;
  }

  bool DrawBatchShaderSafe(const ShaderInput& input) {
    if (const auto it = batch_safe_shaders_.find(&input); it != batch_safe_shaders_.end()) return it->second;
    const auto spirv = input.shader ? bo2zm::shaderlib::Spirv(*input.shader) : std::vector<uint32_t>{};
    const bool safe = draw_batch::ShaderSafe(spirv);
    batch_safe_shaders_.emplace(&input, safe);
    return safe;
  }

  VkShaderModule ModuleOf(const ShaderInput& input) {
    if (const auto it = modules_.find(&input); it != modules_.end()) {
      return it->second;
    }
    const auto& spirv = bo2zm::shaderlib::Spirv(*input.shader);
    if (draw_batch::enabled.load(std::memory_order_relaxed))
      batch_safe_shaders_.try_emplace(&input, draw_batch::ShaderSafe(spirv));
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (CreateProfileModule(info, &shader_module) != VK_SUCCESS) {
      shader_module = VK_NULL_HANDLE;
    }
    modules_.emplace(&input, shader_module);
    return shader_module;
  }


  VkShaderModule ModuleOnlyAlpha(const ShaderInput& input) {
    if (const auto it = modules_only_alpha_.find(&input); it != modules_only_alpha_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuleOf(input);
    }
    uint32_t removed = 0;
    const std::vector<uint32_t> pruned = PruneColorWrites(bo2zm::shaderlib::Spirv(*input.shader), removed);
    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (!pruned.empty()) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = pruned.size() * sizeof(uint32_t);
      info.pCode = pruned.data();
      if (CreateProfileModule(info, &shader_module) != VK_SUCCESS) {
        shader_module = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[native] C5a: PS n{} without color writes: {} removed, {} words of {} ({})",
                input.number, removed, pruned.size(), input.shader->spirv_words,
                shader_module != VK_NULL_HANDLE ? "module created" : "the normal one is used");
    modules_only_alpha_.emplace(&input, shader_module);
    return shader_module != VK_NULL_HANDLE ? shader_module : ModuleOf(input);
  }


  VkShaderModule ModuleZEarly(const ShaderInput& input) {
    if (const auto it = modules_z_early_.find(&input); it != modules_z_early_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuleOf(input);
    }
    const char* reason = "";
    const std::vector<uint32_t> patched = WithTestsEarly(bo2zm::shaderlib::Spirv(*input.shader), reason);
    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (!patched.empty()) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = patched.size() * sizeof(uint32_t);
      info.pCode = patched.data();
      if (CreateProfileModule(info, &shader_module) != VK_SUCCESS) {
        shader_module = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[native] C6 early Z: PS n{} {} ({} kills, {} words)", input.number,
                shader_module != VK_NULL_HANDLE
                    ? std::string("tests depth before shading")
                    : fmt::format("stays as it was: {}", reason),
                input.kills, input.shader->spirv_words);
    modules_z_early_.emplace(&input, shader_module);
    if (shader_module == VK_NULL_HANDLE) {
      ++z_early_no_module_;
    }
    return shader_module != VK_NULL_HANDLE ? shader_module : ModuleOf(input);
  }


  struct PipelineFixedState {
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    VkPipelineViewportStateCreateInfo view{};
    VkPipelineRasterizationStateCreateInfo rasterization{};
    VkPipelineMultisampleStateCreateInfo sampling{};
    VkPipelineDepthStencilStateCreateInfo depth{};
    std::array<VkPipelineColorBlendAttachmentState, 4> blends{};
    uint32_t n_colors = 0;
    VkPipelineColorBlendStateCreateInfo blend{};
    PipelineFixedState() = default;
    PipelineFixedState(const PipelineFixedState&) = delete;
    PipelineFixedState& operator=(const PipelineFixedState&) = delete;
  };


  void FillStateFixed(const PipelineKey& key, PipelineFixedState& fixed, bool warn = true) {
    VkPipelineInputAssemblyStateCreateInfo& assembly = fixed.assembly;
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VkPrimitiveTopology(key.topology);
    assembly.primitiveRestartEnable = (key.rasterization & 0x8) ? VK_TRUE : VK_FALSE;

    VkPipelineViewportStateCreateInfo& view = fixed.view;
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1;
    view.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo& rasterization = fixed.rasterization;
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = ((key.rasterization & 0x1) ? VK_CULL_MODE_FRONT_BIT : 0) |
                           ((key.rasterization & 0x2) ? VK_CULL_MODE_BACK_BIT : 0);

    rasterization.frontFace =
        (key.rasterization & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    rasterization.depthBiasEnable = (key.rasterization & 0x10) ? VK_TRUE : VK_FALSE;
    rasterization.depthClampEnable = REXCVAR_GET(bo2zm_native_test_depth_clamp) ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo& sampling = fixed.sampling;
    sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    const uint32_t d = key.depth;
    VkPipelineDepthStencilStateCreateInfo& depth = fixed.depth;
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (key.formats[4]) {
      depth.depthTestEnable = (d >> 1) & 0x1;
      depth.depthWriteEnable = ((d >> 1) & 0x1) && ((d >> 2) & 0x1);
      depth.depthCompareOp = VkCompareOp((d >> 4) & 0x7);
      depth.stencilTestEnable = d & 0x1;
      depth.front.failOp = VkStencilOp((d >> 11) & 0x7);
      depth.front.passOp = VkStencilOp((d >> 14) & 0x7);
      depth.front.depthFailOp = VkStencilOp((d >> 17) & 0x7);
      depth.front.compareOp = VkCompareOp((d >> 8) & 0x7);
      if ((d >> 7) & 0x1) {
        depth.back.compareOp = VkCompareOp((d >> 20) & 0x7);
        depth.back.failOp = VkStencilOp((d >> 23) & 0x7);
        depth.back.passOp = VkStencilOp((d >> 26) & 0x7);
        depth.back.depthFailOp = VkStencilOp((d >> 29) & 0x7);
      } else {
        depth.back = depth.front;
      }
    }

    static constexpr VkBlendFactor kFactors[32] = {
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
    };
    const auto operation = [&](uint32_t op) {
      switch (op) {
        case 1:
          return VK_BLEND_OP_SUBTRACT;
        case 2:
          return VK_BLEND_OP_MIN;
        case 3:
          return VK_BLEND_OP_MAX;
        case 4:
          if (warn) {
            Warn(52, "blend with reverse subtract: subtract is used");
          }
          return VK_BLEND_OP_SUBTRACT;
        default:
          return VK_BLEND_OP_ADD;
      }
    };
    std::array<VkPipelineColorBlendAttachmentState, 4>& blends = fixed.blends;
    uint32_t& n_colors = fixed.n_colors;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!key.formats[i]) {
        continue;
      }
      const uint32_t m = key.blend[i];
      VkPipelineColorBlendAttachmentState& s = blends[n_colors++];
      s.colorWriteMask = (key.masks >> (i * 4)) & 0xF;
      const uint32_t source_kind = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
      const uint32_t source_kind_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7,
                     target_a = (m >> 24) & 0x1F;


      s.blendEnable = ColorBlendEnabled(s.colorWriteMask, m);
      s.srcColorBlendFactor = kFactors[source_kind];
      s.dstColorBlendFactor = kFactors[target];
      s.colorBlendOp = operation(op);
      s.srcAlphaBlendFactor = kFactors[source_kind_a];
      s.dstAlphaBlendFactor = kFactors[target_a];
      s.alphaBlendOp = operation(op_a);
    }
    VkPipelineColorBlendStateCreateInfo& blend = fixed.blend;
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = n_colors;
    blend.pAttachments = blends.data();
  }


  static bool SameFaceStencil(const VkStencilOpState& a, const VkStencilOpState& b) {
    return a.failOp == b.failOp && a.passOp == b.passOp && a.depthFailOp == b.depthFailOp && a.compareOp == b.compareOp;
  }


  static const char* MismatchEffective(const PipelineFixedState& a, const PipelineFixedState& b) {
    if (a.assembly.topology != b.assembly.topology) {
      return "topology";
    }
    if (a.assembly.primitiveRestartEnable != b.assembly.primitiveRestartEnable) {
      return "primitiveRestartEnable";
    }
    if (a.view.viewportCount != b.view.viewportCount || a.view.scissorCount != b.view.scissorCount) {
      return "viewportCount or scissorCount";
    }
    const VkPipelineRasterizationStateCreateInfo& ra = a.rasterization;
    const VkPipelineRasterizationStateCreateInfo& rb = b.rasterization;
    if (ra.polygonMode != rb.polygonMode || ra.lineWidth != rb.lineWidth ||
        ra.rasterizerDiscardEnable != rb.rasterizerDiscardEnable || ra.depthClampEnable != rb.depthClampEnable) {
      return "polygonMode, lineWidth, rasterizerDiscardEnable or depthClampEnable";
    }
    if (ra.cullMode != rb.cullMode) {
      return "cullMode";
    }
    if (ra.frontFace != rb.frontFace) {
      return "frontFace";
    }
    if (ra.depthBiasEnable != rb.depthBiasEnable) {
      return "depthBiasEnable";
    }
    if (a.sampling.rasterizationSamples != b.sampling.rasterizationSamples) {
      return "rasterizationSamples";
    }
    const VkPipelineDepthStencilStateCreateInfo& pa = a.depth;
    const VkPipelineDepthStencilStateCreateInfo& pb = b.depth;
    if (pa.depthTestEnable != pb.depthTestEnable) {
      return "depthTestEnable";
    }
    if (pa.depthWriteEnable != pb.depthWriteEnable) {
      return "depthWriteEnable";
    }
    if (pa.depthTestEnable && pa.depthCompareOp != pb.depthCompareOp) {
      return "depthCompareOp";
    }
    if (pa.depthBoundsTestEnable != pb.depthBoundsTestEnable) {
      return "depthBoundsTestEnable";
    }
    if (pa.stencilTestEnable != pb.stencilTestEnable) {
      return "stencilTestEnable";
    }
    if (pa.stencilTestEnable && !SameFaceStencil(pa.front, pb.front)) {
      return "front (stencil)";
    }
    if (pa.stencilTestEnable && !SameFaceStencil(pa.back, pb.back)) {
      return "back (stencil)";
    }
    if (a.n_colors != b.n_colors || a.blend.attachmentCount != b.blend.attachmentCount ||
        a.blend.logicOpEnable != b.blend.logicOpEnable) {
      return "attachmentCount or logicOpEnable";
    }
    for (uint32_t i = 0; i < a.n_colors && i < 4; ++i) {
      const VkPipelineColorBlendAttachmentState& x = a.blends[i];
      const VkPipelineColorBlendAttachmentState& y = b.blends[i];
      if (x.colorWriteMask != y.colorWriteMask) {
        return "colorWriteMask";
      }
      if (x.blendEnable != y.blendEnable) {
        return "blendEnable";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x7) &&
          (x.srcColorBlendFactor != y.srcColorBlendFactor || x.dstColorBlendFactor != y.dstColorBlendFactor ||
           x.colorBlendOp != y.colorBlendOp)) {
        return "color equation";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x8) &&
          (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor || x.dstAlphaBlendFactor != y.dstAlphaBlendFactor ||
           x.alphaBlendOp != y.alphaBlendOp)) {
        return "alpha equation";
      }
    }
    return nullptr;
  }


  static void Canonicalize(PipelineKey& c) {
    constexpr uint32_t kDirect = 0x00010001u;
    uint32_t masks = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!c.formats[i]) {
        c.blend[i] = 0;
        continue;
      }
      const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
      masks |= mask << (i * 4);
      const uint32_t m = c.blend[i];
      const bool writes_rgb = (mask & 0x7) != 0;
      const bool writes_alpha = (mask & 0x8) != 0;
      if (!ColorBlendEnabled(mask, m)) {
        c.blend[i] = kDirect;
        continue;
      }
      uint32_t canonical = m;
      if (!writes_rgb) {
        canonical = (canonical & 0xFFFF0000u) | 0x00000001u;
      }
      if (!writes_alpha) {
        canonical = (canonical & 0x0000FFFFu) | 0x00010000u;
      }
      c.blend[i] = canonical;
    }
    c.masks = masks;
    uint32_t d = c.formats[4] ? c.depth : 0;
    d &= ~0x8u;
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;
    }
    if (!(d & 0x1)) {
      d &= 0x77u;
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;
    }
    c.depth = d;
  }


  bool CheckKeyCanonical(const PipelineKey& raw_hash, const PipelineKey& canonical, uint64_t n) {
    ++canonical_checked_;
    const char* field = nullptr;
    if (raw_hash.vs != canonical.vs || raw_hash.ps != canonical.ps || raw_hash.input != canonical.input ||
        raw_hash.topology != canonical.topology || raw_hash.specialization != canonical.specialization ||
        raw_hash.rasterization != canonical.rasterization || raw_hash.padding != canonical.padding ||
        raw_hash.padding2 != canonical.padding2 ||
        std::memcmp(raw_hash.formats, canonical.formats, sizeof(raw_hash.formats)) != 0) {
      field = "shaders, input, topology, specialization, rasterization or formats";
    } else {
      PipelineFixedState a;
      PipelineFixedState b;
      FillStateFixed(raw_hash, a);
      FillStateFixed(canonical, b);
      field = MismatchEffective(a, b);
    }
    if (field) {
      key_canonical_off_ = true;
      key_canonical_ = false;
      canonical_valid_ = false;
      REXLOG_ERROR("[native] C6 canonical key: MISMATCH in {} (check {}: VS {} PS {}, blend {:08X} -> {:08X}, masks "
                   "{:04X} -> {:04X}, depth {:08X} -> {:08X}). Off for the rest of the session: lookups use the usual "
                   "key",
                   field, n, raw_hash.vs, raw_hash.ps, raw_hash.blend[0], canonical.blend[0], raw_hash.masks,
                   canonical.masks, raw_hash.depth, canonical.depth);
      return false;
    }
    if (n == kCanonicalACheck) {
      BO2ZM_REPORT_RING("[native] C6 canonical key: {} changed keys checked field by field against the usual one, 0 "
                           "mismatches; still checking 1 in 4096",
                           n);
    }
    return true;
  }


  PipelineKey LookupKey(const PipelineKey& key) {
    PipelineKey c = key;
    if (key_canonical_) {
      if (canonical_valid_ && std::memcmp(&key, &canonical_raw_, sizeof(key)) == 0) {
        c = canonical_result_;
      } else {
        Canonicalize(c);
        if (std::memcmp(&c, &key, sizeof(c)) != 0) {
          const uint64_t n = ++canonical_changed_;
          if ((n <= kCanonicalACheck || (n & 4095) == 0) && !CheckKeyCanonical(key, c, n)) {
            c = key;
          }
        }
        canonical_raw_ = key;
        canonical_result_ = c;
        canonical_valid_ = key_canonical_;
      }
    }


    const uint32_t mode_eds = eds_mode_;
    if (mode_eds & kEds12) {
      c.depth = 0;
      c.rasterization = 0;
      c.topology = RepresentativeTopology(c.topology);
    }
    if (mode_eds & kEds3) {
      std::fill(std::begin(c.blend), std::end(c.blend), 0u);
      c.masks = 0;
    }
    c.padding2 = mode_eds;
    return c;
  }


  void ReportKeyCanonical() {
    const auto now = std::chrono::steady_clock::now();
    if (now - canonical_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = canonical_report_ == std::chrono::steady_clock::time_point{};
    canonical_report_ = now;
    const uint64_t changed = canonical_changed_ - canonical_changed_previous_;
    canonical_changed_previous_ = canonical_changed_;
    const uint64_t pipelines = pipelines_.size();
    const uint64_t new_ones = pipelines - std::min<uint64_t>(pipelines, canonical_pipelines_previous_);
    canonical_pipelines_previous_ = pipelines;
    if (first || (!changed && !new_ones)) {
      return;
    }
    BO2ZM_REPORT_RING("[native] C6 canonical key: {}; {} keys changed in 20 s ({} checked since the start, "
                         "{}); {} pipelines in the map, {} new in 20 s",
                         key_canonical_off_ ? "OFF by the guard"
                         : key_canonical_       ? "on"
                                                 : "off",
                         changed, canonical_checked_,
                         canonical_checked_ >= kCanonicalACheck ? "guard passed" : "checking", pipelines,
                         new_ones);
  }


  void ReportPipelineBetweenPasses() {
    const auto now = std::chrono::steady_clock::now();
    if (now - between_passes_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = between_passes_report_ == std::chrono::steady_clock::time_point{};
    between_passes_report_ = now;
    const uint64_t passes = passes_begun_ - between_passes_passes_previous_;
    const uint64_t kept = passes_with_pipeline_ - between_passes_kept_previous_;
    between_passes_passes_previous_ = passes_begun_;
    between_passes_kept_previous_ = passes_with_pipeline_;
    if (first || !passes) {
      return;
    }
    BO2ZM_REPORT_RING("[native] C6 pipeline across passes: {}; {} passes begun in 20 s, {} with a bound "
                         "pipeline that is kept (its first draw does not bind again if the key is the same)",
                         pipeline_between_passes_ ? "kept" : "forgotten, as before", passes, kept);
  }


  struct EdsState {
    uint32_t face = 0;
    uint32_t frontface = 0;
    uint32_t topology = 0;
    uint32_t restart = 0;
    uint32_t bias = 0;
    uint32_t test_z = 0;
    uint32_t writes_z = 0;
    uint32_t function_z = 0;
    uint32_t has_stencil = 0;
    uint32_t ahead[4] = {};
    uint32_t behind[4] = {};
    uint32_t n_colors = 0;
    VkBool32 blend_active[4] = {};
    VkColorBlendEquationEXT equation[4] = {};
    VkColorComponentFlags mask[4] = {};
  };


  static uint32_t RepresentativeTopology(uint32_t topology) {
    switch (ClassTopology(topology)) {
      case 1:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      case 2:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      default:
        return topology;
    }
  }

  static const char* NameModeEds(uint32_t mode) {
    switch (mode & (kEds12 | kEds3)) {
      case kEds12:
        return "EDS1/EDS2 (phase 1)";
      case kEds3:
        return "EDS3 (phase 2)";
      case kEds12 | kEds3:
        return "EDS1/EDS2 and EDS3 (phases 1 and 2)";
      default:
        return "off: all the state in the pipeline, as before";
    }
  }


  void StateEdsOf(const PipelineKey& c, EdsState& e, uint32_t mode) {
    e = EdsState{};
    if (mode & kEds12) {
      e.face = ((c.rasterization & 0x1) ? uint32_t(VK_CULL_MODE_FRONT_BIT) : 0u) |
               ((c.rasterization & 0x2) ? uint32_t(VK_CULL_MODE_BACK_BIT) : 0u);
      e.frontface = (c.rasterization & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      e.restart = (c.rasterization & 0x8) ? 1u : 0u;
      e.bias = (c.rasterization & 0x10) ? 1u : 0u;
      e.topology = c.topology;
      if (c.formats[4]) {
        const uint32_t d = c.depth;
        e.test_z = (d >> 1) & 0x1;
        e.writes_z = ((d >> 1) & 0x1) & ((d >> 2) & 0x1);
        e.function_z = (d >> 4) & 0x7;
        e.has_stencil = d & 0x1;
        e.ahead[0] = (d >> 11) & 0x7;
        e.ahead[1] = (d >> 14) & 0x7;
        e.ahead[2] = (d >> 17) & 0x7;
        e.ahead[3] = (d >> 8) & 0x7;
        if ((d >> 7) & 0x1) {
          e.behind[0] = (d >> 23) & 0x7;
          e.behind[1] = (d >> 26) & 0x7;
          e.behind[2] = (d >> 29) & 0x7;
          e.behind[3] = (d >> 20) & 0x7;
        } else {
          std::memcpy(e.behind, e.ahead, sizeof(e.behind));
        }
      }
    }
    if (mode & kEds3) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (!c.formats[i]) {
          continue;
        }
        const uint32_t a = e.n_colors++;
        const uint32_t m = c.blend[i];
        const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
        const uint32_t source_kind = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
        const uint32_t source_kind_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7, target_a = (m >> 24) & 0x1F;
        e.mask[a] = mask;
        e.blend_active[a] = ColorBlendEnabled(mask, m) ? VK_TRUE : VK_FALSE;
        VkColorBlendEquationEXT& q = e.equation[a];
        q.srcColorBlendFactor = kFactorsBlend[source_kind];
        q.dstColorBlendFactor = kFactorsBlend[target];
        q.colorBlendOp = OperationBlend(op);
        q.srcAlphaBlendFactor = kFactorsBlend[source_kind_a];
        q.dstAlphaBlendFactor = kFactorsBlend[target_a];
        q.alphaBlendOp = OperationBlend(op_a);
      }
    }
  }


  void EmitStateDynamic(VkCommandBuffer cmd, const EdsState& d, bool everything, uint32_t mode) {
    EdsState& g = eds_recorded_;
    eds_calls_[kEdsEverything] += everything ? 1 : 0;
    if (mode & kEds12) {
      if (everything || d.face != g.face) {
        set_face_(cmd, VkCullModeFlags(d.face));
        g.face = d.face;
        ++eds_calls_[kEdsFace];
      }
      if (everything || d.frontface != g.frontface) {
        set_frontface_(cmd, VkFrontFace(d.frontface));
        g.frontface = d.frontface;
        ++eds_calls_[kEdsFrontface];
      }
      if (everything || d.topology != g.topology) {
        set_topology_(cmd, VkPrimitiveTopology(d.topology));
        g.topology = d.topology;
        ++eds_calls_[kEdsTopology];
      }
      if (everything || d.restart != g.restart) {
        set_restart_(cmd, VkBool32(d.restart));
        g.restart = d.restart;
        ++eds_calls_[kEdsRestart];
      }
      if (everything || d.bias != g.bias) {
        set_bias_(cmd, VkBool32(d.bias));
        g.bias = d.bias;
        ++eds_calls_[kEdsBias];
      }
      if (everything || d.test_z != g.test_z) {
        set_test_z_(cmd, VkBool32(d.test_z));
        g.test_z = d.test_z;
        ++eds_calls_[kEdsTestZ];
      }
      if (everything || d.writes_z != g.writes_z) {
        set_writes_z_(cmd, VkBool32(d.writes_z));
        g.writes_z = d.writes_z;
        ++eds_calls_[kEdsWritesZ];
      }
      if (everything || (d.test_z && d.function_z != g.function_z)) {
        set_function_z_(cmd, VkCompareOp(d.function_z));
        g.function_z = d.function_z;
        ++eds_calls_[kEdsFunctionZ];
      }
      if (everything || d.has_stencil != g.has_stencil) {
        set_stencil_(cmd, VkBool32(d.has_stencil));
        g.has_stencil = d.has_stencil;
        ++eds_calls_[kEdsStencil];
      }
      const bool ahead = everything || (d.has_stencil && std::memcmp(d.ahead, g.ahead, sizeof(d.ahead)) != 0);
      const bool behind = everything || (d.has_stencil && std::memcmp(d.behind, g.behind, sizeof(d.behind)) != 0);
      if (ahead && behind && std::memcmp(d.ahead, d.behind, sizeof(d.ahead)) == 0) {
        set_stencil_ops_(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VkStencilOp(d.ahead[0]), VkStencilOp(d.ahead[1]),
                          VkStencilOp(d.ahead[2]), VkCompareOp(d.ahead[3]));
        ++eds_calls_[kEdsStencilOps];
      } else {
        if (ahead) {
          set_stencil_ops_(cmd, VK_STENCIL_FACE_FRONT_BIT, VkStencilOp(d.ahead[0]), VkStencilOp(d.ahead[1]),
                            VkStencilOp(d.ahead[2]), VkCompareOp(d.ahead[3]));
          ++eds_calls_[kEdsStencilOps];
        }
        if (behind) {
          set_stencil_ops_(cmd, VK_STENCIL_FACE_BACK_BIT, VkStencilOp(d.behind[0]), VkStencilOp(d.behind[1]),
                            VkStencilOp(d.behind[2]), VkCompareOp(d.behind[3]));
          ++eds_calls_[kEdsStencilOps];
        }
      }
      if (ahead) {
        std::memcpy(g.ahead, d.ahead, sizeof(g.ahead));
      }
      if (behind) {
        std::memcpy(g.behind, d.behind, sizeof(g.behind));
      }
    }
    if (mode & kEds3) {
      const uint32_t n = d.n_colors;
      if (!n) {
        if (everything) {
          g.n_colors = 0;
        }
      } else {
        const bool other_n = everything || n != g.n_colors;
        bool active = other_n;
        bool equation = other_n;
        bool mask = other_n;
        for (uint32_t a = 0; a < n && a < 4; ++a) {
          active = active || d.blend_active[a] != g.blend_active[a];
          mask = mask || d.mask[a] != g.mask[a];
          if (!equation && d.blend_active[a]) {
            const VkColorBlendEquationEXT& x = d.equation[a];
            const VkColorBlendEquationEXT& y = g.equation[a];
            equation = ((d.mask[a] & 0x7) != 0 &&
                        (x.srcColorBlendFactor != y.srcColorBlendFactor ||
                         x.dstColorBlendFactor != y.dstColorBlendFactor || x.colorBlendOp != y.colorBlendOp)) ||
                       ((d.mask[a] & 0x8) != 0 &&
                        (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor ||
                         x.dstAlphaBlendFactor != y.dstAlphaBlendFactor || x.alphaBlendOp != y.alphaBlendOp));
          }
        }
        if (active) {
          set_blend_active_(cmd, 0, n, d.blend_active);
          std::memcpy(g.blend_active, d.blend_active, sizeof(g.blend_active));
          ++eds_calls_[kEdsBlendActive];
        }
        if (equation) {
          set_equation_(cmd, 0, n, d.equation);
          std::memcpy(g.equation, d.equation, sizeof(g.equation));
          ++eds_calls_[kEdsEquation];
        }
        if (mask) {
          set_mask_(cmd, 0, n, d.mask);
          std::memcpy(g.mask, d.mask, sizeof(g.mask));
          ++eds_calls_[kEdsMask];
        }
        g.n_colors = n;
      }
    }
  }


  bool CheckStateDynamic(const PipelineKey& key, uint64_t n) {
    ++eds_checked_;
    PipelineFixedState fixed;
    FillStateFixed(key, fixed);
    const EdsState& g = eds_recorded_;
    const char* field = nullptr;
    uint32_t pinned = 0;
    uint32_t expected = 0;
    const auto look = [&](const char* label, uint32_t a, uint32_t b) {
      if (!field && a != b) {
        field = label;
        pinned = a;
        expected = b;
      }
    };
    if (eds_mode_ & kEds12) {
      const VkPipelineRasterizationStateCreateInfo& r = fixed.rasterization;
      const VkPipelineDepthStencilStateCreateInfo& z = fixed.depth;
      look("cullMode", g.face, r.cullMode);
      look("frontFace", g.frontface, r.frontFace);
      look("topology", g.topology, fixed.assembly.topology);
      look("primitiveRestartEnable", g.restart, fixed.assembly.primitiveRestartEnable);
      look("depthBiasEnable", g.bias, r.depthBiasEnable);
      look("depthTestEnable", g.test_z, z.depthTestEnable);
      look("depthWriteEnable", g.writes_z, z.depthWriteEnable);
      if (z.depthTestEnable) {
        look("depthCompareOp", g.function_z, z.depthCompareOp);
      }
      look("stencilTestEnable", g.has_stencil, z.stencilTestEnable);
      if (z.stencilTestEnable) {
        look("front.failOp", g.ahead[0], z.front.failOp);
        look("front.passOp", g.ahead[1], z.front.passOp);
        look("front.depthFailOp", g.ahead[2], z.front.depthFailOp);
        look("front.compareOp", g.ahead[3], z.front.compareOp);
        look("back.failOp", g.behind[0], z.back.failOp);
        look("back.passOp", g.behind[1], z.back.passOp);
        look("back.depthFailOp", g.behind[2], z.back.depthFailOp);
        look("back.compareOp", g.behind[3], z.back.compareOp);
      }
    }
    if ((eds_mode_ & kEds3) && fixed.n_colors) {
      look("attachmentCount", g.n_colors, fixed.n_colors);
      for (uint32_t a = 0; a < fixed.n_colors && a < 4; ++a) {
        const VkPipelineColorBlendAttachmentState& s = fixed.blends[a];
        const VkColorBlendEquationEXT& q = g.equation[a];
        look("colorWriteMask", g.mask[a], s.colorWriteMask);
        look("blendEnable", g.blend_active[a], s.blendEnable);
        if (s.blendEnable && (s.colorWriteMask & 0x7)) {
          look("srcColorBlendFactor", q.srcColorBlendFactor, s.srcColorBlendFactor);
          look("dstColorBlendFactor", q.dstColorBlendFactor, s.dstColorBlendFactor);
          look("colorBlendOp", q.colorBlendOp, s.colorBlendOp);
        }
        if (s.blendEnable && (s.colorWriteMask & 0x8)) {
          look("srcAlphaBlendFactor", q.srcAlphaBlendFactor, s.srcAlphaBlendFactor);
          look("dstAlphaBlendFactor", q.dstAlphaBlendFactor, s.dstAlphaBlendFactor);
          look("alphaBlendOp", q.alphaBlendOp, s.alphaBlendOp);
        }
      }
    }
    if (field) {
      eds_off_ = true;
      eds_mode_ = 0;
      eds_valid_ = false;
      pipeline_bound_ = VK_NULL_HANDLE;
      REXLOG_ERROR("[native] C6 dynamic state: MISMATCH in {} (check {}: set {} and the usual pipeline would carry {}; "
                   "VS {} PS {}, topology {}, depth {:08X}, rasterization {:02X}, masks {:04X}). Off for the rest of "
                   "the session: the pipelines with all the fixed state come back",
                   field, n, pinned, expected, key.vs, key.ps, key.topology, key.depth,
                   key.rasterization, key.masks);
      return false;
    }
    if (n == kEdsACheck) {
      BO2ZM_REPORT_RING("[native] C6 dynamic state: {} key changes checked against the state of their usual pipeline, 0 "
                           "mismatches; still checking 1 in 4096",
                           n);
    }
    return true;
  }


  bool SetStateDynamic(VkCommandBuffer cmd, const PipelineKey& key) {
    ++eds_draws_;
    if (eds_valid_ && std::memcmp(&key, &eds_key_, sizeof(key)) == 0) {
      ++eds_repeated_;
      return true;
    }
    EdsState d;
    StateEdsOf(key, d, eds_mode_);
    EmitStateDynamic(cmd, d, !eds_valid_, eds_mode_);
    eds_key_ = key;
    eds_valid_ = true;
    const uint64_t n = ++eds_checkable_;
    if (n <= kEdsACheck || (n & 4095) == 0) {
      return CheckStateDynamic(key, n);
    }
    return true;
  }


  void LoadStateDynamic() {
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();

    const auto request = [&](const char* label) {
      return deferred::Wrap(label, ifn.vkGetDeviceProcAddr(device_, label));
    };
    set_face_ = reinterpret_cast<PFN_vkCmdSetCullMode>(request("vkCmdSetCullMode"));
    set_frontface_ = reinterpret_cast<PFN_vkCmdSetFrontFace>(request("vkCmdSetFrontFace"));
    set_topology_ = reinterpret_cast<PFN_vkCmdSetPrimitiveTopology>(request("vkCmdSetPrimitiveTopology"));
    set_restart_ = reinterpret_cast<PFN_vkCmdSetPrimitiveRestartEnable>(request("vkCmdSetPrimitiveRestartEnable"));
    set_bias_ = reinterpret_cast<PFN_vkCmdSetDepthBiasEnable>(request("vkCmdSetDepthBiasEnable"));
    set_test_z_ = reinterpret_cast<PFN_vkCmdSetDepthTestEnable>(request("vkCmdSetDepthTestEnable"));
    set_writes_z_ = reinterpret_cast<PFN_vkCmdSetDepthWriteEnable>(request("vkCmdSetDepthWriteEnable"));
    set_function_z_ = reinterpret_cast<PFN_vkCmdSetDepthCompareOp>(request("vkCmdSetDepthCompareOp"));
    set_stencil_ = reinterpret_cast<PFN_vkCmdSetStencilTestEnable>(request("vkCmdSetStencilTestEnable"));
    set_stencil_ops_ = reinterpret_cast<PFN_vkCmdSetStencilOp>(request("vkCmdSetStencilOp"));
    const uint32_t api = vulkan_device_->properties().apiVersion;
    eds12_available_ = api >= VK_MAKE_API_VERSION(0, 1, 3, 0) && set_face_ && set_frontface_ && set_topology_ &&
                        set_restart_ && set_bias_ && set_test_z_ && set_writes_z_ && set_function_z_ &&
                        set_stencil_ && set_stencil_ops_;
    REXLOG_INFO("[native] C6 dynamic state: EDS1/EDS2 {} (device API {}.{}.{})",
                eds12_available_ ? "available (core 1.3)" : "NOT available: phase 1 is not used",
                VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
#if defined(REX_UI_VULKAN_DYNAMIC_STATE3)


    set_blend_active_ = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(request("vkCmdSetColorBlendEnableEXT"));
    set_equation_ = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(request("vkCmdSetColorBlendEquationEXT"));
    set_mask_ = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(request("vkCmdSetColorWriteMaskEXT"));
    const auto& p3 = vulkan_device_->properties();
    eds3_available_ = vulkan_device_->extensions().ext_EXT_extended_dynamic_state3 &&
                       p3.extendedDynamicState3ColorBlendEnable && p3.extendedDynamicState3ColorBlendEquation &&
                       p3.extendedDynamicState3ColorWriteMask && set_blend_active_ && set_equation_ && set_mask_;
    REXLOG_INFO("[native] C6 dynamic state: EDS3 (blend, equation and color mask) {}",
                eds3_available_ ? "available"
                                 : "NOT available (the device does not provide it or it is not enabled): phase 2 is not "
                                   "used");
#else
    REXLOG_INFO("[native] C6 dynamic state: EDS3 NOT available: this SDK does not enable "
                "VK_EXT_extended_dynamic_state3 (the SDK's extended dynamic state 3 change is missing); phase 2 is not used");
#endif
  }


  void ReportStateDynamic() {
    const auto now = std::chrono::steady_clock::now();
    if (now - eds_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = eds_report_ == std::chrono::steady_clock::time_point{};
    eds_report_ = now;
    const std::array<uint64_t, kEdsN> n = eds_calls_;
    eds_calls_.fill(0);
    const uint64_t draws = eds_draws_ - eds_draws_previous_;
    const uint64_t repeated = eds_repeated_ - eds_repeated_previous_;
    const uint64_t frames = frame_ - eds_frames_previous_;
    eds_draws_previous_ = eds_draws_;
    eds_repeated_previous_ = eds_repeated_;
    eds_frames_previous_ = frame_;
    if (first || !draws) {
      return;
    }
    uint64_t calls = 0;
    for (size_t k = 0; k < kEdsN; ++k) {
      calls += k == kEdsEverything ? 0 : n[k];
    }
    BO2ZM_REPORT_RING(
        "[native] C6 dynamic state: {}; {} draws in 20 s ({:.0f} per frame; {} repeat the previous key) and "
        "{} vkCmdSet* ({:.3f} per draw; {} times everything: new buffer, sky or a pass that forgets the pipeline) | "
        "cull {}, front face {}, topology {}, restart {}, bias {}, Z test {}, Z write {}, Z function {}, stencil {}, "
        "stencil ops {} | blend {}, equation {}, masks {} | pipelines created: {} usual, {} EDS1/EDS2, {} EDS3, {} with "
        "both | guard: {} checked ({})",
        eds_off_ ? "OFF by the guard" : NameModeEds(eds_mode_), draws,
        frames ? double(draws) / double(frames) : 0.0, repeated, calls,
        double(calls) / double(draws), n[kEdsEverything], n[kEdsFace], n[kEdsFrontface], n[kEdsTopology],
        n[kEdsRestart], n[kEdsBias], n[kEdsTestZ], n[kEdsWritesZ], n[kEdsFunctionZ], n[kEdsStencil],
        n[kEdsStencilOps], n[kEdsBlendActive], n[kEdsEquation], n[kEdsMask], pipelines_per_mode_[0],
        pipelines_per_mode_[kEds12], pipelines_per_mode_[kEds3], pipelines_per_mode_[kEds12 | kEds3],
        eds_checked_, eds_checked_ >= kEdsACheck ? "guard passed" : "checking");
  }


  VkBlendOp OperationBlend(uint32_t op) {
    switch (op) {
      case 1:
        return VK_BLEND_OP_SUBTRACT;
      case 2:
        return VK_BLEND_OP_MIN;
      case 3:
        return VK_BLEND_OP_MAX;
      case 4:
        Warn(52, "blend with reverse subtract: subtract is used");
        return VK_BLEND_OP_SUBTRACT;
      default:
        return VK_BLEND_OP_ADD;
    }
  }

  VkPipeline PipelineOf(const PipelineKey& key, const VertexInput& input,
                        const DrawRequest& p) {


    if (last_key_valid_ && std::memcmp(&last_key_, &key, sizeof(key)) == 0) {
      return last_pipeline_;
    }
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));

    PipelineCell& cell = cells_pipeline_[hash & (kCellsPipeline - 1)];
    if (pipelines_direct_ && cell.pipeline != VK_NULL_HANDLE &&
        std::memcmp(&cell.key, &key, sizeof(key)) == 0) {
      const uint64_t n = ++pipelines_direct_hits_;
      if (!(n <= kPipelinesACheck || (n & 4095) == 0) ||
          CheckCellPipeline(hash, key, cell.pipeline, n)) {
        last_key_ = key;
        last_pipeline_ = cell.pipeline;
        last_key_valid_ = true;
        return cell.pipeline;
      }

    }
    if (pipelines_direct_) {
      ++pipelines_direct_misses_;
    }
    if (const auto it = pipelines_.find(hash); it != pipelines_.end()) {
      if (std::memcmp(&it->second.first, &key, sizeof(key)) == 0) {
        last_key_ = key;
        last_pipeline_ = it->second.second;
        last_key_valid_ = true;
        if (pipelines_direct_) {
          cell.key = key;
          cell.pipeline = it->second.second;
        }
        return it->second.second;
      }
      Warn(50, "pipeline hash collision");
      return VK_NULL_HANDLE;
    }


    const bool map_first_request = map_pipeline_seen_.insert(hash).second;
    if (map_first_request) {
      ++map_first_pipeline_requests_;
    }

    if (!compile_pending_.empty() && compile_pending_.count(hash)) {
      ++draws_waiting_pipeline_;
      return VK_NULL_HANDLE;
    }
    const VkShaderModule vs = ModuleOf(*p.vs);
    VkShaderModule ps = VK_NULL_HANDLE;
    if (key.ps && (key.specialization & shader_probe::kFlat)) {
      ps = ModuleVariant(4, kSpirvBo2zmProbeFlat, sizeof(kSpirvBo2zmProbeFlat));
    } else if (key.ps && bo2zm::native::DebugMagenta()) {
      ps = ModuleVariant(3, kSpirvBo2zmMagenta, sizeof(kSpirvBo2zmMagenta));
    } else if (key.ps && (key.specialization & kSpecGlowNatural)) {
      ps = ModuleVariant(1, kSpirvGlowEnergy, sizeof(kSpirvGlowEnergy));
    } else if (key.ps && (key.specialization & kSpecGlowSmooth)) {
      ps = ModuleVariant(2, kSpirvGlowSmooth, sizeof(kSpirvGlowSmooth));
    } else if (key.ps && (key.specialization & kSpecOnlyAlpha)) {
      ps = ModuleOnlyAlpha(*p.ps);
    } else if (key.ps && (key.specialization & kSpecZEarly)) {
      ps = ModuleZEarly(*p.ps);
    } else if (key.ps) {
      ps = ModuleOf(*p.ps);
    }
    if (vs == VK_NULL_HANDLE || (key.ps && ps == VK_NULL_HANDLE)) {
      Reject(51, "could not create a shader module");
      return VK_NULL_HANDLE;
    }


    uint32_t n_colors = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto start_creation = std::chrono::steady_clock::now();


    VkResult created = VK_ERROR_UNKNOWN;
    if (AsyncCompiles()) {
      static const bool all = REXCVAR_GET(bo2zm_native_test_async_all);
      created = all ? VK_PIPELINE_COMPILE_REQUIRED
                    : CreatePipelineVulkan(key, input, vs, ps, pass_rp_, true, pipeline, n_colors,
                                           VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
      if (created == VK_PIPELINE_COMPILE_REQUIRED) {
        if (map_first_request) {
          ++map_first_pipeline_uncached_;
        }
        QueueCompile(hash, key, input, vs, ps, p.vs, key.ps ? p.ps : nullptr);
        ++draws_waiting_pipeline_;
        return VK_NULL_HANDLE;
      }
    } else {
      created = CreatePipelineVulkan(key, input, vs, ps, pass_rp_, true, pipeline, n_colors);
    }
    if (created != VK_SUCCESS) {
      Reject(53, "could not create a pipeline");
      pipeline = VK_NULL_HANDLE;
    } else {
      ++pipelines_no_save_;
    }
    const uint64_t ns_creation = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - start_creation)
                                              .count());
    ns_pipelines_ += ns_creation;
    AnnotatePipelineCreated(key, input, p.vs, key.ps ? p.ps : nullptr, pipeline, ns_creation);
    pipelines_.emplace(hash, std::make_pair(key, pipeline));
    ++pipelines_per_mode_[key.padding2 & 3];
    if (pipelines_direct_ && pipeline != VK_NULL_HANDLE) {
      cell.key = key;
      cell.pipeline = pipeline;
    }
    if (pipelines_.size() <= 64) {
      REXLOG_INFO("[native] C6: pipeline {} (VS n{} PS n{}, topology {}, {} colors, depth {:08X}, blend {:08X}, "
                  "specialization {})",
                  pipelines_.size(), p.vs->number, key.ps ? int(p.ps->number) : -1,
                  key.topology, n_colors,
                  key.depth, key.blend[0], key.specialization);
    }
    return pipeline;
  }


  VkResult CreatePipelineVulkan(const PipelineKey& key, const VertexInput& input, VkShaderModule vs,
                               VkShaderModule ps, VkRenderPass pass, bool warn, VkPipeline& pipeline,
                               uint32_t& n_colors_output, VkPipelineCreateFlags create_flags = 0) {
    const VkSpecializationMapEntry map{0, 0, sizeof(uint32_t)};
    const VkSpecializationInfo specialization{1, &map, sizeof(uint32_t), &key.specialization};
    const uint32_t vertex_specialization = shader_probe::VertexSpecialization(key.specialization);
    const VkSpecializationInfo vertex_spec{1, &map, sizeof(uint32_t), &vertex_specialization};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage : stages) {
      stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stage.pName = "main";
      stage.pSpecializationInfo = &specialization;
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].pSpecializationInfo = &vertex_spec;
    stages[0].module = vs;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;

    std::vector<VkVertexInputBindingDescription> bindings;
    for (uint32_t i = 0; i < input.bindings.size(); ++i) {
      bindings.push_back({i, input.bindings[i].stride, VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const VertexAttribute& a : input.attributes) {
      attributes.push_back({a.location, a.binding, a.format, a.offset});
    }
    VkPipelineVertexInputStateCreateInfo vertices{};
    vertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertices.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vertices.pVertexBindingDescriptions = bindings.data();
    vertices.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vertices.pVertexAttributeDescriptions = attributes.data();


    PipelineFixedState fixed;
    FillStateFixed(key, fixed, warn);
    const VkPipelineInputAssemblyStateCreateInfo& assembly = fixed.assembly;
    const VkPipelineViewportStateCreateInfo& view = fixed.view;
    const VkPipelineRasterizationStateCreateInfo& rasterization = fixed.rasterization;
    const VkPipelineMultisampleStateCreateInfo& sampling = fixed.sampling;
    const VkPipelineDepthStencilStateCreateInfo& depth = fixed.depth;
    const VkPipelineColorBlendStateCreateInfo& blend = fixed.blend;
    const uint32_t n_colors = fixed.n_colors;


    VkDynamicState dynamic_states[24] = {
        VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,   VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS};
    uint32_t n_dynamic = 7;
    if (key.padding2 & kEds12) {
      for (const VkDynamicState state : kDynamicEds12) {
        dynamic_states[n_dynamic++] = state;
      }
    }
    if (key.padding2 & kEds3) {
      for (const VkDynamicState state : kDynamicEds3) {
        dynamic_states[n_dynamic++] = state;
      }
    }
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = n_dynamic;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = key.ps ? 2 : 1;
    info.pStages = stages;
    info.pVertexInputState = &vertices;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &view;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &sampling;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_pipeline_;
    info.renderPass = pass;
    info.basePipelineIndex = -1;
    info.flags = create_flags;
    n_colors_output = n_colors;
    pipeline = VK_NULL_HANDLE;
    return dfn_.vkCreateGraphicsPipelines(device_, cache_pipelines_, 1, &info, nullptr, &pipeline);
  }


  enum : uint8_t { kListPending = 0, kListPrewarm = 1, kListNoShader = 2, kListFailed = 3,
                   kListOtherMode = 4 };
  static constexpr uint64_t kNsCompiled = 5000000;
  static constexpr uint64_t kNsSlow = 20000000;


  void LoadListPipelines() {
    const std::filesystem::path path = PathFilePipelines();
    std::vector<uint8_t> data = std::move(list_read_);
    list_read_ = {};
    if (data.size() < kHeaderList ||
        data.size() > kHeaderList + kMaxRegistersList * sizeof(PipelineRecord)) {
      data.clear();
    }
    const char* reason = nullptr;
    uint32_t header[4] = {};
    if (data.empty()) {
      reason = "no list: starting in this session";
    } else {
      std::memcpy(header, data.data(), sizeof(header));
      if (header[0] != kMagicListPipelines || header[1] != kVersionListPipelines ||
          header[2] != sizeof(PipelineRecord) ||
          data.size() != kHeaderList + size_t(header[3]) * sizeof(PipelineRecord)) {
        reason = "list from another version or damaged: starting from scratch";
      }
    }
    if (!reason) {
      list_file_.reserve(header[3]);
      for (uint32_t i = 0; i < header[3]; ++i) {
        PipelineRecord r;
        std::memcpy(&r, data.data() + kHeaderList + size_t(i) * sizeof(PipelineRecord), sizeof(r));
        if (r.n_attributes > PipelineRecord::kMaxAttributes || r.n_bindings > PipelineRecord::kMaxBindings) {
          continue;
        }
        if (index_list_.emplace(XXH3_64bits(&r.key, sizeof(r.key)), list_file_.size()).second) {
          list_file_.push_back(r);
        }
      }
      written_list_ = std::move(data);
    }
    LoadExtraListsPipelines();
    state_list_.assign(list_file_.size(), kListPending);
    REXLOG_INFO("[native] C6 prewarm: {} pipelines in the list of {}{}{}", list_file_.size(),
                path.string(), reason ? ": " : "", reason ? reason : "");
  }


  void LoadExtraListsPipelines() {
    const std::filesystem::path folder =
        bo2zm::native::cache::UserFolder() / std::string(REXCVAR_GET(bo2zm_native_prewarm_lists));
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) {
      return;
    }
    uint32_t lists = 0, not_lists = 0, added = 0, known = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
      if (!entry.is_regular_file(error) || entry.path().extension() != ".bin") {
        continue;
      }
      std::vector<uint8_t> data;
      ReadFileWhole(entry.path(), data);
      uint32_t header[4] = {};
      if (data.size() >= kHeaderList) {
        std::memcpy(header, data.data(), sizeof(header));
      }
      if (data.size() < kHeaderList || header[0] != kMagicListPipelines || header[1] != kVersionListPipelines ||
          header[2] != sizeof(PipelineRecord) ||
          data.size() != kHeaderList + size_t(header[3]) * sizeof(PipelineRecord)) {
        ++not_lists;
        continue;
      }
      ++lists;
      for (uint32_t i = 0; i < header[3] && list_file_.size() < kMaxRegistersList; ++i) {
        PipelineRecord r;
        std::memcpy(&r, data.data() + kHeaderList + size_t(i) * sizeof(PipelineRecord), sizeof(r));
        if (r.n_attributes > PipelineRecord::kMaxAttributes || r.n_bindings > PipelineRecord::kMaxBindings) {
          continue;
        }
        if (index_list_.emplace(XXH3_64bits(&r.key, sizeof(r.key)), list_file_.size()).second) {
          list_file_.push_back(r);
          ++added;
        } else {
          ++known;
        }
      }
    }
    REXLOG_INFO("[native] C6 prewarm: {} more pipelines from {} lists in {} ({} already listed, {} files that are not "
                "lists)",
                added, lists, folder.string(), known, not_lists);
  }


  std::vector<uint8_t> SerializeListPipelines() const {
    const size_t until = prewarm_until_.load(std::memory_order_acquire);
    std::vector<const PipelineRecord*> registers;
    registers.reserve(list_file_.size() + list_session_.size());
    for (size_t i = 0; i < list_file_.size(); ++i) {
      if (i < until && state_list_[i] == kListNoShader) {
        continue;
      }
      registers.push_back(&list_file_[i]);
    }
    for (const PipelineRecord& r : list_session_) {
      registers.push_back(&r);
    }
    if (registers.size() > kMaxRegistersList) {
      registers.erase(registers.begin(), registers.begin() + std::ptrdiff_t(registers.size() - kMaxRegistersList));
    }
    std::vector<uint8_t> data(kHeaderList + registers.size() * sizeof(PipelineRecord));
    const uint32_t header[4] = {kMagicListPipelines, kVersionListPipelines, uint32_t(sizeof(PipelineRecord)),
                                  uint32_t(registers.size())};
    std::memcpy(data.data(), header, sizeof(header));
    for (size_t i = 0; i < registers.size(); ++i) {
      std::memcpy(data.data() + kHeaderList + i * sizeof(PipelineRecord), registers[i], sizeof(PipelineRecord));
    }
    return data;
  }


  void AnnotatePipelineCreated(const PipelineKey& key, const VertexInput& input, const ShaderInput* vs,
                               const ShaderInput* ps, VkPipeline pipeline, uint64_t ns) {
    if (key.specialization & shader_probe::kFlat) return;
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));
    if (const auto it = index_list_.find(hash); it != index_list_.end()) {
      const size_t j = it->second;
      if (j < list_file_.size() && j < prewarm_until_.load(std::memory_order_acquire) &&
          state_list_[j] == kListPrewarm) {
        ++ring_prewarmed_;
        ns_ring_prewarmed_ += ns;
        if (ns >= kNsSlow) {
          ++ring_prewarmed_slow_;
        }
      } else {
        ++list_ring_;
        list_ns_ring_ += ns;
      }
      return;
    }
    ++ring_new_;
    ns_ring_new_ += ns;
    if (pipeline == VK_NULL_HANDLE || !vs || !vs->shader || (key.ps && (!ps || !ps->shader)) ||
        input.attributes.size() > PipelineRecord::kMaxAttributes ||
        input.bindings.size() > PipelineRecord::kMaxBindings ||
        list_file_.size() + list_session_.size() >= kMaxRegistersList) {
      return;
    }
    PipelineRecord r;
    r.key = key;
    r.hash_vs = vs->shader->hash;
    r.hash_ps = key.ps ? ps->shader->hash : 0;
    r.n_attributes = uint32_t(input.attributes.size());
    for (uint32_t k = 0; k < r.n_attributes; ++k) {
      const VertexAttribute& a = input.attributes[k];
      r.attributes[k] = {a.location, a.binding, uint32_t(a.format), a.offset};
    }
    r.n_bindings = uint32_t(input.bindings.size());
    for (uint32_t k = 0; k < r.n_bindings; ++k) {
      r.strides[k] = input.bindings[k].stride;
    }
    index_list_.emplace(hash, SIZE_MAX);
    list_session_.push_back(r);
    ++list_no_save_;
    ++ring_a_list_;
  }


  void TryPrewarm() {
    if (prewarm_decided_) {
      return;
    }
    if (list_file_.empty() || cache_pipelines_ == VK_NULL_HANDLE ||
        !REXCVAR_GET(bo2zm_native_prewarm_pipelines)) {
      prewarm_decided_ = true;
      if (!list_file_.empty()) {
        REXLOG_INFO("[native] C6 prewarm: no prewarming ({})",
                    cache_pipelines_ == VK_NULL_HANDLE ? "no pipeline cache"
                                                       : "bo2zm_native_prewarm_pipelines = false");
      }
      return;
    }
    const NativeShaders* library = LibraryActive();
    if (!library || !library->loaded() || layout_pipeline_ == VK_NULL_HANDLE) {
      return;
    }
    prewarm_decided_ = true;
    library_prewarm_ = library;
    prewarm_eds_ = eds_mode_;
    prewarm_start_ = std::chrono::steady_clock::now();
    try {
      prewarm_thread_ = std::thread([this] { LoopPrewarm(false); });
      REXLOG_INFO("[native] C6 prewarm: thread created for {} pipelines of the list",
                  list_file_.size());
      if (REXCVAR_GET(bo2zm_native_prewarm_threads) >= 2 && list_file_.size() >= 2) {
        try {
          prewarm_helper_ = std::thread([this] { LoopPrewarm(true); });
        } catch (const std::system_error& error) {
          REXLOG_WARN("[native] C6 prewarm: no second thread ({})", error.what());
        }
      }
    } catch (const std::system_error& error) {
      REXLOG_WARN("[native] C6 prewarm: could not create the thread ({}); no prewarming", error.what());
    }
  }


  class Warmer {
   public:
    Warmer(VulkanDrawsImpl& draws, const NativeShaders& library) : d_(draws), library_(library) {}
    ~Warmer() {
      for (const auto& [key, shader_module] : modules_) {
        if (shader_module != VK_NULL_HANDLE) {
          d_.dfn_.vkDestroyShaderModule(d_.device_, shader_module, nullptr);
        }
      }
      for (const auto& [key, pass] : passes_) {
        if (pass != VK_NULL_HANDLE) {
          d_.dfn_.vkDestroyRenderPass(d_.device_, pass, nullptr);
        }
      }
    }
    Warmer(const Warmer&) = delete;
    Warmer& operator=(const Warmer&) = delete;


    uint8_t Warm(const PipelineRecord& r, uint64_t& ns, bool probe, bool& compiled) {
      ns = 0;
      compiled = false;
      const ShaderInput* vs = r.key.vs ? library_.PerNumber(r.key.vs - 1) : nullptr;
      const ShaderInput* ps = r.key.ps ? library_.PerNumber(r.key.ps - 1) : nullptr;
      if (!vs || !vs->vertices || !vs->shader || vs->shader->hash != r.hash_vs ||
          (r.key.ps && (!ps || ps->vertices || !ps->shader || ps->shader->hash != r.hash_ps))) {
        return kListNoShader;
      }
      if (r.key.padding2 != d_.prewarm_eds_) {
        return kListOtherMode;
      }
      const VkShaderModule module_vs = Normal(*vs);
      const VkShaderModule module_ps = r.key.ps ? Pixel(r.key, *ps) : VK_NULL_HANDLE;
      const uint64_t key_pass = XXH3_64bits(r.key.formats, sizeof(r.key.formats));
      auto it_pass = passes_.find(key_pass);
      if (it_pass == passes_.end()) {
        it_pass = passes_.emplace(key_pass, d_.CreatePass(r.key.formats, kLoadRead)).first;
      }
      VertexInput input;
      for (uint32_t k = 0; k < r.n_attributes; ++k) {
        const RegisterAttribute& a = r.attributes[k];
        input.attributes.push_back({a.location, a.binding, VkFormat(a.format), a.offset});
      }
      for (uint32_t k = 0; k < r.n_bindings; ++k) {
        input.bindings.push_back({0, r.strides[k]});
      }
      if (module_vs == VK_NULL_HANDLE || (r.key.ps && module_ps == VK_NULL_HANDLE) ||
          it_pass->second == VK_NULL_HANDLE) {
        return kListFailed;
      }
      VkPipeline pipeline = VK_NULL_HANDLE;
      uint32_t n_colors = 0;
      const auto t0 = std::chrono::steady_clock::now();
      VkResult result = VK_PIPELINE_COMPILE_REQUIRED;
      if (probe) {
        result = d_.CreatePipelineVulkan(r.key, input, module_vs, module_ps, it_pass->second, false, pipeline,
                                         n_colors, VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
      }
      if (result == VK_PIPELINE_COMPILE_REQUIRED) {
        compiled = probe;
        pipeline = VK_NULL_HANDLE;
        result = d_.CreatePipelineVulkan(r.key, input, module_vs, module_ps, it_pass->second, false, pipeline,
                                         n_colors);
      }
      ns = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
      if (result != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
        return kListFailed;
      }
      d_.dfn_.vkDestroyPipeline(d_.device_, pipeline, nullptr);
      return kListPrewarm;
    }

   private:
    VkShaderModule Create(const uint32_t* spirv, size_t bytes) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = bytes;
      info.pCode = spirv;
      VkShaderModule shader_module = VK_NULL_HANDLE;
      if (d_.CreateProfileModule(info, &shader_module) != VK_SUCCESS) {
        shader_module = VK_NULL_HANDLE;
      }
      return shader_module;
    }

    VkShaderModule Normal(const ShaderInput& e) {
      auto it = modules_.find(e.number);
      if (it == modules_.end()) {
        const auto& spirv = bo2zm::shaderlib::Spirv(*e.shader);
        it = modules_.emplace(e.number, Create(spirv.data(), spirv.size() * sizeof(uint32_t))).first;
      }
      return it->second;
    }


    VkShaderModule Pixel(const PipelineKey& key, const ShaderInput& e) {
      const uint32_t spec = key.specialization;
      if (spec & shader_probe::kFlat) {
        constexpr uint64_t flat_key=(uint64_t(5)<<32)|0xffffffffull;
        auto it=modules_.find(flat_key);
        if(it==modules_.end())it=modules_.emplace(flat_key,Create(kSpirvBo2zmProbeFlat,sizeof(kSpirvBo2zmProbeFlat))).first;
        return it->second;
      }
      const uint32_t variant = (spec & kSpecGlowNatural) ? 1
                                : (spec & kSpecGlowSmooth) ? 2
                                : (spec & kSpecOnlyAlpha)        ? 3
                                : (spec & kSpecZEarly)       ? 4
                                                                : 0;
      if (variant == 0) {
        return Normal(e);
      }
      const uint64_t key_module = (uint64_t(variant) << 32) | (variant <= 2 ? 0xFFFFFFFFull : uint64_t(e.number));
      auto it = modules_.find(key_module);
      if (it == modules_.end()) {
        VkShaderModule shader_module = VK_NULL_HANDLE;
        if (variant == 1) {
          shader_module = Create(kSpirvGlowEnergy, sizeof(kSpirvGlowEnergy));
        } else if (variant == 2) {
          shader_module = Create(kSpirvGlowSmooth, sizeof(kSpirvGlowSmooth));
        } else if (variant == 3) {
          uint32_t removed = 0;
          const std::vector<uint32_t> pruned = PruneColorWrites(bo2zm::shaderlib::Spirv(*e.shader), removed);
          if (!pruned.empty()) {
            shader_module = Create(pruned.data(), pruned.size() * sizeof(uint32_t));
          }
        } else {
          const char* reason = "";
          const std::vector<uint32_t> patched = WithTestsEarly(bo2zm::shaderlib::Spirv(*e.shader), reason);
          if (!patched.empty()) {
            shader_module = Create(patched.data(), patched.size() * sizeof(uint32_t));
          }
        }
        it = modules_.emplace(key_module, shader_module).first;
      }
      if (it->second != VK_NULL_HANDLE || variant <= 2) {
        return it->second;
      }
      return Normal(e);
    }

    VulkanDrawsImpl& d_;
    const NativeShaders& library_;
    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, VkRenderPass> passes_;
  };


  void LoopPrewarm(bool helper) {
    rex::thread::set_current_thread_name(helper ? "BO2 pipeline prewarm 2" : "BO2 pipeline prewarm");
    int32_t priority = -1;
#if REX_PLATFORM_SWITCH


    for (const int32_t candidate : {0x3F, 0x3E, 0x3D, 0x3C, 0x3B}) {
      if (RexSwitchSetCurrentThreadPriorityOk(int(candidate))) {
        priority = candidate;
        break;
      }
    }
    if (priority < 0) {
      REXLOG_WARN("[native] C6 prewarm: the system accepts no priority from 0x3B to 0x3F: no prewarming");
      if (!helper) {
        prewarm_finished_.store(true, std::memory_order_release);
      } else {
        prewarm_helper_done_.store(true, std::memory_order_release);
      }
      return;
    }
#endif
    if (!helper) {
      prewarm_priority_.store(priority, std::memory_order_relaxed);
    }
    uint32_t done = 0, compiled = 0, no_shader = 0, other_mode = 0, failed = 0;
    uint64_t ns_compiled = 0, ns_waited = 0;
    const size_t n = list_file_.size();
    {
      Warmer warmer(*this, *library_prewarm_);
      bool unused = false;
      if (helper) {
        for (size_t i = n; i-- > 0;) {
          while (bo2zm::cheats::InMatch() && !prewarm_stop_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            ns_waited += 250000000;
          }
          if (prewarm_stop_.load(std::memory_order_relaxed) || i < prewarm_until_.load(std::memory_order_acquire)) {
            break;
          }
          uint64_t ns = 0;
          if (warmer.Warm(list_file_[i], ns, false, unused) == kListPrewarm) {
            ++done;
            if (ns >= kNsCompiled) {
              ++compiled;
              ns_compiled += ns;
            }
          }
        }
      }
      for (size_t i = 0; i < n && !helper; ++i) {
        if (prewarm_stop_.load(std::memory_order_relaxed)) {
          break;
        }
        uint64_t ns = 0;
        const uint8_t state = warmer.Warm(list_file_[i], ns, false, unused);
        if (state == kListNoShader) {
          ++no_shader;
        } else if (state == kListOtherMode) {
          ++other_mode;
        } else if (state == kListPrewarm) {
          ++done;
          if (ns >= kNsCompiled) {
            ++compiled;
            ns_compiled += ns;
          }
        } else {
          ++failed;
        }
        state_list_[i] = state;
        prewarm_done_.store(done, std::memory_order_relaxed);
        prewarm_compiled_.store(compiled, std::memory_order_relaxed);
        prewarm_ns_compiled_.store(ns_compiled, std::memory_order_relaxed);
        prewarm_no_shader_.store(no_shader, std::memory_order_relaxed);
        prewarm_other_mode_.store(other_mode, std::memory_order_relaxed);
        prewarm_failed_.store(failed, std::memory_order_relaxed);
        prewarm_until_.store(i + 1, std::memory_order_release);
      }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - prewarm_start_).count();
    if (helper) {

      prewarm_compiled_helper_.store(compiled, std::memory_order_relaxed);
      prewarm_helper_done_.store(true, std::memory_order_release);
      REXLOG_INFO("[native] C6 prewarm, second thread: {} in {:.1f} s, priority {:#x}: {} pipelines from the list's "
                  "end in the driver's cache, {} really compiled ({:.0f} ms, {:.1f} ms each); {:.1f} s waited for "
                  "matches to end",
                  prewarm_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished", seconds, priority, done,
                  compiled, double(ns_compiled) / 1e6, compiled ? double(ns_compiled) / 1e6 / double(compiled) : 0.0,
                  double(ns_waited) / 1e9);
      return;
    }
    REXLOG_INFO("[native] C6 prewarm: {} in {:.1f} s, priority {:#x}: {} of {} pipelines prewarmed, {} "
                "really compiled ({:.0f} ms, {:.1f} ms each; the rest were already in the cache), {} skipped because "
                "the library no longer has their shaders, {} of another dynamic state mode and {} failed",
                prewarm_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished", seconds, priority,
                done, n, compiled, double(ns_compiled) / 1e6,
                compiled ? double(ns_compiled) / 1e6 / double(compiled) : 0.0, no_shader, other_mode, failed);
    prewarm_finished_.store(true, std::memory_order_release);
  }


  void Precompile() override {
    const NativeShaders* library = LibraryActive();
    const size_t n = list_file_.size();
    if (prewarm_decided_ || !library || !library->loaded() || layout_pipeline_ == VK_NULL_HANDLE ||
        cache_pipelines_ == VK_NULL_HANDLE || n == 0) {
      bo2zm::native::precompile::Begin(0, 0);
      return;
    }
    prewarm_decided_ = true;
    library_prewarm_ = library;
    prewarm_eds_ = eds_mode_;
    prewarm_start_ = std::chrono::steady_clock::now();
#if REX_PLATFORM_SWITCH

    const uint32_t threads = uint32_t(std::clamp(bo2zm::CoresInUse(), 1, 3));
#else
    const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 4u, 10u) - 2;
#endif
    bo2zm::native::precompile::Begin(uint32_t(n), threads);
    std::atomic<size_t> next{0};
    std::atomic<uint32_t> done{0}, compiled{0}, no_shader{0}, other_mode{0}, failed{0};
    std::atomic<uint64_t> ns_compiled{0}, ns_cached{0};
    const auto work = [&](uint32_t index) {
      rex::thread::set_current_thread_name("BO2 pipeline precompile");
#if REX_PLATFORM_SWITCH

      RexSwitchSetCurrentThreadPriorityOk(0x2E);
      RexSwitchSetCurrentThreadCore(int(index % 3));
#else
      (void)index;
#endif
      Warmer warmer(*this, *library);
      for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) {
        uint64_t ns = 0;
        bool was_compiled = false;
        const uint8_t state = warmer.Warm(list_file_[i], ns, true, was_compiled);
        state_list_[i] = state;
        if (state == kListNoShader) {
          no_shader.fetch_add(1, std::memory_order_relaxed);
        } else if (state == kListOtherMode) {
          other_mode.fetch_add(1, std::memory_order_relaxed);
        } else if (state == kListPrewarm) {
          done.fetch_add(1, std::memory_order_relaxed);
          if (was_compiled) {
            compiled.fetch_add(1, std::memory_order_relaxed);
            ns_compiled.fetch_add(ns, std::memory_order_relaxed);
          } else {
            ns_cached.fetch_add(ns, std::memory_order_relaxed);
          }
        } else {
          failed.fetch_add(1, std::memory_order_relaxed);
        }
        bo2zm::native::precompile::Advance(was_compiled);
      }
    };
    std::vector<std::thread> workers;
    for (uint32_t t = 0; t < threads; ++t) {
      try {
        workers.emplace_back(work, t);
      } catch (const std::system_error& error) {
        REXLOG_WARN("[native] C6 precompile: thread {} not created ({})", t, error.what());
        break;
      }
    }
    if (workers.empty()) {
      work(0);
    }
    for (std::thread& worker : workers) {
      worker.join();
    }


    prewarm_done_.store(done.load(), std::memory_order_relaxed);
    prewarm_compiled_.store(compiled.load(), std::memory_order_relaxed);
    prewarm_ns_compiled_.store(ns_compiled.load(), std::memory_order_relaxed);
    prewarm_no_shader_.store(no_shader.load(), std::memory_order_relaxed);
    prewarm_other_mode_.store(other_mode.load(), std::memory_order_relaxed);
    prewarm_failed_.store(failed.load(), std::memory_order_relaxed);
    prewarm_until_.store(n, std::memory_order_release);
    prewarm_finished_.store(true, std::memory_order_release);
    precompiled_ = true;
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - prewarm_start_).count();
    const uint32_t cached = done.load() - compiled.load();
    REXLOG_INFO("[native] C6 precompile: {} pipelines in {:.2f} s on {} threads: {} compiled ({:.0f} ms of the "
                "threads, {:.1f} ms each), {} already in the cache ({:.1f} ms each), {} skipped because the library no "
                "longer has their shaders, {} of another dynamic state mode and {} failed",
                n, seconds, workers.empty() ? size_t(1) : workers.size(), compiled.load(),
                double(ns_compiled.load()) / 1e6,
                compiled.load() ? double(ns_compiled.load()) / 1e6 / double(compiled.load()) : 0.0, cached,
                cached ? double(ns_cached.load()) / 1e6 / double(cached) : 0.0, no_shader.load(),
                other_mode.load(), failed.load());


    pipelines_no_save_ += compiled.load();
    prewarm_counted_ = compiled.load();
    if (compiled.load() > 0) {
      SaveCachePipelines(false);
    } else {
      RequestVouch();
    }
  }


  void ReportPrewarm(std::chrono::steady_clock::time_point now) {
    if (now - prewarm_report_ < std::chrono::seconds(10)) {
      return;
    }
    prewarm_report_ = now;
    if (!prewarm_mismatch_ && ring_prewarmed_ >= 8 &&
        ring_prewarmed_slow_ * 2 >= ring_prewarmed_) {
      prewarm_mismatch_ = true;
      prewarm_stop_.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("[native] C6 prewarm: MISMATCH: of {} pipelines the ring asked for already prewarmed, {} "
                   "took as long as a compiled one (20 ms or more): what the thread prewarms is not what the ring asks "
                   "for. The thread is stopped; nothing else changes",
                   ring_prewarmed_, ring_prewarmed_slow_);
    }
    const size_t until = prewarm_until_.load(std::memory_order_acquire);
    const bool finished = prewarm_finished_.load(std::memory_order_acquire);
    const uint64_t changes = uint64_t(until) + ring_prewarmed_ + list_ring_ + ring_new_ +
                             (finished ? 1 : 0) + (prewarm_decided_ ? 1 : 0);
    if (changes == prewarm_changes_previous_) {
      return;
    }
    prewarm_changes_previous_ = changes;
    const auto average_of = [](uint64_t ns, uint64_t k) { return k ? double(ns) / double(k) / 1e6 : 0.0; };
    const uint32_t compiled = prewarm_compiled_.load(std::memory_order_relaxed);
    BO2ZM_REPORT_RING(
        "[native] C6 pipeline prewarm: list of {}; thread {} (priority {:#x}): {} walked, {} prewarmed, {} "
        "really compiled ({:.0f} ms), {} without their shaders, {} of another mode, {} failed | the ring created {} of "
        "the list already prewarmed ({:.1f} ms on average, {} slow), {} of the list not prewarmed ({:.1f} ms on "
        "average) and {} new ({:.1f} ms on average; {} added to the list)",
        list_file_.size(),
        precompiled_                ? "not needed (the precompile at start walked the list)"
        : !prewarm_thread_.joinable() ? (prewarm_decided_ ? "not launched" : "waiting for the library")
        : finished                    ? (prewarm_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished")
                                       : "running",
        uint32_t(prewarm_priority_.load(std::memory_order_relaxed)), until,
        prewarm_done_.load(std::memory_order_relaxed), compiled,
        double(prewarm_ns_compiled_.load(std::memory_order_relaxed)) / 1e6,
        prewarm_no_shader_.load(std::memory_order_relaxed), prewarm_other_mode_.load(std::memory_order_relaxed),
        prewarm_failed_.load(std::memory_order_relaxed), ring_prewarmed_,
        average_of(ns_ring_prewarmed_, ring_prewarmed_), ring_prewarmed_slow_, list_ring_,
        average_of(list_ns_ring_, list_ring_), ring_new_, average_of(ns_ring_new_, ring_new_),
        ring_a_list_);
  }

  void StopPrewarm() {
    prewarm_stop_.store(true, std::memory_order_relaxed);
    if (prewarm_thread_.joinable()) {
      prewarm_thread_.join();
    }
    if (prewarm_helper_.joinable()) {
      prewarm_helper_.join();
    }
    StopCompiles();
  }


  struct CompileJob {
    uint64_t hash = 0;
    PipelineKey key;
    VertexInput input;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule ps = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    const ShaderInput* vs_input = nullptr;
    const ShaderInput* ps_input = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    uint64_t ns = 0;
  };

  bool AsyncCompiles() {
    if (async_compiles_ < 0) {
      async_compiles_ = REXCVAR_GET(bo2zm_native_async_pipelines) &&
                                vulkan_device_->properties().pipelineCreationCacheControl
                            ? 1
                            : 0;
      REXLOG_INFO("[native] C6 pipelines compiled on a thread (bo2zm_native_async_pipelines) = {}{}",
                  async_compiles_ ? "YES" : "no",
                  vulkan_device_->properties().pipelineCreationCacheControl ? ""
                                                                             : " (the driver lacks pipelineCreationCacheControl)");
    }
    return async_compiles_ > 0;
  }

  void QueueCompile(uint64_t hash, const PipelineKey& key, const VertexInput& input, VkShaderModule vs,
                    VkShaderModule ps, const ShaderInput* vs_input, const ShaderInput* ps_input) {
    CompileJob job;
    job.hash = hash;
    job.key = key;
    job.input = input;
    job.vs = vs;
    job.ps = ps;
    job.pass = pass_rp_;
    job.vs_input = vs_input;
    job.ps_input = ps_input;
    compile_pending_.insert(hash);
    {
      std::lock_guard<std::mutex> lock(compile_mutex_);
      compile_queue_.push_back(std::move(job));
    }
    size_t waiting = 0;
    {
      std::lock_guard<std::mutex> lock(compile_mutex_);
      waiting = compile_queue_.size();
    }

    if (waiting >= 2 && compile_thread_.joinable() &&
        int(compile_helpers_.size()) < REXCVAR_GET(bo2zm_native_compile_threads) - 1) {
      try {
        compile_helpers_.emplace_back([this] { CompilePipelinesLoop(true); });
      } catch (const std::system_error& error) {
        REXLOG_WARN("[native] C6 pipelines compiled on a thread: no second thread ({})", error.what());
      }
    }
    if (!compile_thread_.joinable()) {
      try {
        compile_thread_ = std::thread([this] { CompilePipelinesLoop(false); });
      } catch (const std::system_error& error) {
        REXLOG_ERROR("[native] C6 pipelines compiled on a thread: the thread could not be created ({}); every pipeline "
                     "is compiled on the ring from now on",
                     error.what());
        async_compiles_ = 0;
        std::lock_guard<std::mutex> lock(compile_mutex_);
        for (CompileJob& queued : compile_queue_) {
          uint32_t n_colors = 0;
          queued.result = CreatePipelineVulkan(queued.key, queued.input, queued.vs, queued.ps, queued.pass, true,
                                               queued.pipeline, n_colors);
          compile_done_.push_back(std::move(queued));
        }
        compile_queue_.clear();
        return;
      }
    }
    compile_cv_.notify_all();
  }


  void CompilePipelinesLoop(bool helper) {
    static const bool helpers_in_match = REXCVAR_GET(bo2zm_native_compile_helpers_in_match);
    rex::thread::set_current_thread_name(helper ? "BO2 pipeline compiles 2" : "BO2 pipeline compiles");
#if REX_PLATFORM_SWITCH
    RexSwitchSetCurrentThreadPriorityOk(0x3B);
#endif
    std::unique_lock<std::mutex> lock(compile_mutex_);
    for (;;) {
      compile_cv_.wait(lock, [this, helper] {
        return compile_stop_ ||
               (helper ? compile_queue_.size() >= 2 && (helpers_in_match || !bo2zm::cheats::InMatch())
                       : !compile_queue_.empty());
      });
      if (compile_stop_) {
        return;
      }
      if (helper) {
        compiles_helper_.fetch_add(1, std::memory_order_relaxed);
      }
      CompileJob job = std::move(compile_queue_.front());
      compile_queue_.pop_front();
      lock.unlock();
      const auto start = std::chrono::steady_clock::now();
      uint32_t n_colors = 0;
      job.result = CreatePipelineVulkan(job.key, job.input, job.vs, job.ps, job.pass, false, job.pipeline,
                                          n_colors);
      job.ns = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      lock.lock();
      compile_done_.push_back(std::move(job));
      compiles_ready_.store(true, std::memory_order_release);
    }
  }

  void StopCompiles() {
    {
      std::lock_guard<std::mutex> lock(compile_mutex_);
      compile_stop_ = true;
    }
    compile_cv_.notify_all();
    if (compile_thread_.joinable()) {
      compile_thread_.join();
    }
    for (std::thread& helper : compile_helpers_) {
      if (helper.joinable()) {
        helper.join();
      }
    }
    compile_helpers_.clear();
    std::lock_guard<std::mutex> lock(compile_mutex_);
    for (CompileJob& job : compile_done_) {
      if (job.pipeline != VK_NULL_HANDLE) {
        dfn_.vkDestroyPipeline(device_, job.pipeline, nullptr);
      }
    }
    compile_done_.clear();
    compile_queue_.clear();
  }


  void CollectCompiledPipelines() {
    if (!compiles_ready_.load(std::memory_order_acquire)) [[likely]] {
      return;
    }
    std::deque<CompileJob> done;
    {
      std::lock_guard<std::mutex> lock(compile_mutex_);
      done.swap(compile_done_);
      compiles_ready_.store(false, std::memory_order_relaxed);
    }
    for (CompileJob& job : done) {
      compile_pending_.erase(job.hash);
      if (job.result != VK_SUCCESS) {
        Reject(53, "could not create a pipeline");
        job.pipeline = VK_NULL_HANDLE;
      } else {
        ++pipelines_no_save_;
      }
      ++compiles_async_;
      ns_compiles_async_ += job.ns;
      compile_ns_worst_ = std::max(compile_ns_worst_, job.ns);
      ns_pipelines_ += job.ns;
      AnnotatePipelineCreated(job.key, job.input, job.vs_input, job.ps_input, job.pipeline, job.ns);
      pipelines_.emplace(job.hash, std::make_pair(job.key, job.pipeline));
      ++pipelines_per_mode_[job.key.padding2 & 3];
    }
  }


  void ReportCompiles(std::chrono::steady_clock::time_point now) {
    if (now - compile_report_ < std::chrono::seconds(10)) {
      return;
    }
    compile_report_ = now;
    const uint64_t compiled = compiles_async_ - compiles_reported_;
    const uint64_t waited = draws_waiting_pipeline_ - draws_waiting_reported_;
    if (!compiled && !waited) {
      return;
    }
    const uint64_t ns = ns_compiles_async_ - ns_compiles_reported_;
    compiles_reported_ = compiles_async_;
    draws_waiting_reported_ = draws_waiting_pipeline_;
    ns_compiles_reported_ = ns_compiles_async_;
    const uint32_t helper = compiles_helper_.load(std::memory_order_relaxed);
    BO2ZM_REPORT_RING("[native] C6 pipelines compiled on a thread, last 10 s: {} ({:.1f} ms on average, worst "
                      "{:.1f} ms; {} by the second thread), {} still compiling; {} draws skipped while their pipeline "
                      "compiled",
                      compiled, compiled ? double(ns) / 1e6 / double(compiled) : 0.0, double(compile_ns_worst_) / 1e6,
                      helper - compiles_helper_reported_, compile_pending_.size(), waited);
    compiles_helper_reported_ = helper;
    compile_ns_worst_ = 0;
  }

  void DestroyImage(NativeImage& image) {
    if (image.view != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, image.image, nullptr);


    if (image.pool_block != 0xFFFFFFFFu) pool_textures_.Free(image.pool_block);
    if (image.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, image.guest_memory, nullptr);
    image = NativeImage{};
  }

  struct Heap {
    uint32_t capacity = 0;
    uint32_t next = 1;
    std::vector<uint32_t> free;
  };
  uint64_t slots_full_ = 0;
  uint64_t slots_full_previous_ = 0;
  uint32_t SlotsUsed(uint32_t heap) const {
    const auto& h = heaps_[heap];
    return h.next - 1 - uint32_t(std::min<size_t>(h.free.size(), h.next - 1));
  }

  const VulkanDevice* vulkan_device_;
  const VulkanDevice::Functions& dfn_;
  VkDevice device_;
  rex::memory::Memory* memory_;
  TargetContext* context_;
  FnBufferAddress address_buffer_ = nullptr;
  FnCopyImage copy_image_ = nullptr;

  VkBuffer upload_ = VK_NULL_HANDLE;
  VkDeviceMemory upload_memory_ = VK_NULL_HANDLE;
  uint32_t upload_type_ = UINT32_MAX;
  VkDeviceSize upload_size_real_ = 0;
  uint8_t* upload_data_ = nullptr;
  VkDeviceAddress upload_address_ = 0;
  VkDeviceSize upload_used_ = 0;
  bool upload_coherent_ = false;
  uint64_t epoch_upload_ = 0;
  uint64_t frame_ = 0;

  static constexpr uint32_t kLevelsACheck = 200;
  static constexpr uint32_t kCheckOneOfEvery = 64;
  uint64_t levels_seen_ = 0;
  uint64_t orders_seen_ = 0;
  int tiled_fast_ = -1;
  uint32_t levels_checked_ = 0;
  uint32_t orders_checked_ = 0;
  std::vector<uint8_t> check_tiled_;

  static constexpr uint32_t kPostponementMax = 8;
  int32_t budget_hashes_ = -1;
  uint64_t frame_hashes_ = UINT64_MAX;
  uint64_t bytes_hash_frame_ = 0;

  static constexpr uint64_t kSamplesACheck = 3000;
  int32_t sampling_hashes_ = -1;
  uint64_t samples_checked_ = 0;
  uint64_t samples_with_two_ = 0;
  uint64_t samples_hits_ = 0;
  uint64_t samples_different_ = 0;
  uint64_t bytes_sample_ = 0;
  uint64_t bytes_saved_sample_ = 0;
  std::chrono::steady_clock::time_point report_sampling_{};

  const bool copies_active_ = REXCVAR_GET(bo2zm_native_upload_thread);
  std::array<CopyJob, 8192> copies_{};
  std::atomic<size_t> copies_written_{0};


  std::atomic<size_t> copies_done_{0};
  bool copies_sleeping_ = false;
  bool copies_waiting_ = false;
  std::mutex copies_mutex_;
  std::condition_variable copies_cv_;
  std::condition_variable copies_done_cv_;
  bool copies_stop_ = false;
  std::thread copies_thread_;
  uint64_t copies_in_line_ = 0;
  uint64_t waits_copies_ = 0;
  uint64_t ns_waiting_copies_ = 0;
  std::atomic<size_t> copies_progress_{0};
  std::thread::id copies_producer_;
  bool copies_other_thread_warned_ = false;

  const bool copies_help_requested_ = REXCVAR_GET(bo2zm_native_upload_help);
  int32_t copies_phase_ = 0;
  bool copies_no_thread_ = false;
  std::atomic<size_t> copies_taken_{0};
  std::mutex copies_chunk_mutex_;
  std::array<std::atomic<uint32_t>, 8192> copies_marks_{};
  std::atomic<uint64_t> copies_thread_n_{0};
  std::atomic<uint64_t> copies_thread_bytes_{0};
  size_t copies_verified_ = 0;
  uint64_t copies_checked_ = 0;
  uint64_t copies_waits_observing_ = 0;
  uint64_t copies_helped_ = 0;
  uint64_t copies_bytes_helped_ = 0;
  uint64_t copies_ns_helping_ = 0;
  uint64_t copies_mismatches_ = 0;
  struct CopyReportFigures {
    uint64_t queued = 0, bytes_queued = 0, in_line = 0, bytes_in_line = 0;
    uint64_t helped = 0, bytes_helped = 0, ns_helping = 0, waits_with_help = 0;
    uint64_t waits = 0, no_take = 0, waits_chunk = 0, ns_chunk = 0, ns_chunk_worst = 0;
    uint64_t waits_whole = 0, ns_wait = 0, ns_wait_worst = 0, checked = 0;
  };
  CopyReportFigures copies_inf_{};
  std::chrono::steady_clock::time_point copies_report_{};
  uint64_t copies_thread_n_previous_ = 0;
  uint64_t copies_thread_bytes_previous_ = 0;
  struct UploadBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory guest_memory = VK_NULL_HANDLE;
    VkDeviceSize size_real = 0;
    uint8_t* data = nullptr;
    VkDeviceAddress address = 0;
  };


  std::array<UploadBuffer, kSlotsOfJob> uploads_{};

  bool skin_on_ = false;
  VkPipelineLayout skin_layout_ = VK_NULL_HANDLE;
  VkPipeline skin_pipeline_ = VK_NULL_HANDLE;
  bool skin_bound_ = false;

  VkBuffer skin_table_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory skin_table_memory_ = VK_NULL_HANDLE;
  uint8_t* skin_table_data_ = nullptr;
  VkDeviceAddress skin_table_address_ = 0;
  struct SkinTable {
    uint32_t jobs = 0, groups = 0, dispatched = 0;
  };
  std::array<SkinTable, kSlotsOfJob> skin_tables_{};

  VkBuffer skin_pool_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory skin_pool_memory_ = VK_NULL_HANDLE;
  uint8_t* skin_pool_data_ = nullptr;
  VkDeviceAddress skin_pool_address_ = 0;
  struct SkinPool {
    VkDeviceSize used = 0;
    uint64_t generation = 1;
    uint64_t left = 0;
    VkDeviceSize dirty_lo = ~VkDeviceSize(0), dirty_hi = 0;
  };
  std::array<SkinPool, 2> skin_pools_{};
  uint32_t skin_pool_ = 0;
  struct SkinCacheEntry {
    uint32_t records_guest = 0;
    uint32_t pool = 0;
    uint64_t generation = 0;
    VkDeviceSize offset = 0;
    uint32_t epoch = 0;
    uint32_t uses = 0;
    uint64_t sum = 0;
  };
  std::unordered_map<uint64_t, SkinCacheEntry> skin_cache_;
  std::vector<bo2zm::gpu_skin::JobRef> skin_jobs_;
  struct SkinCheck {
    uint32_t upload;
    VkDeviceSize gpu;
    VkDeviceSize cpu;
    uint32_t vertices;
    uint32_t kernel;
    uint32_t physical;
  };
  std::vector<SkinCheck> skin_checks_;

  std::array<UploadBuffer, kSlotsOfJob> shared_bufs_{};
  bool shared_separate_ = false;
  bool shared_coherent_ = true;
  uint32_t shared_type_ = 0;
  uint8_t* shared_data_ = nullptr;
  VkDeviceMemory shared_memory_ = VK_NULL_HANDLE;
  VkDeviceSize shared_size_real_ = 0;
  VkDeviceSize shared_used_ = 0;
  uint64_t shared_bytes_published_ = 0;

  std::array<VkDescriptorSetLayout, 4> layouts_{};
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 4> sets_{};
  VkPipelineLayout layout_pipeline_ = VK_NULL_HANDLE;

  bool use_ubo_ = false;
  bool ubo_only_ = false;
  bool compact_bc4_ = false, compact_bc5_ = false;
  VkDeviceSize alignment_ubo_ = 256;
  VkDescriptorSetLayout layout_ubo_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ubo_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSlotsOfJob> sets_ubo_{};
  uint32_t slot_current_ = 0;
  bool ubo_bound_ = false;
  uint32_t slot_ubo_bound_ = UINT32_MAX;
  std::array<uint32_t, 3> offsets_ubo_bound_{};

  std::array<uint64_t, 8> set4_changes_{};
  uint64_t set4_first_ = 0;
  uint64_t set4_draws_ = 0;
  std::array<uint64_t, 3> reuploads_vs_{};
  std::array<uint64_t, 3> reuploads_ps_{};
  std::chrono::steady_clock::time_point set4_report_{};
  std::chrono::steady_clock::time_point set4_start_alternation_ = std::chrono::steady_clock::now();
  bool set4_requested_ = true;
  bool set4_requested_annotated_ = false;
#if REX_PLATFORM_SWITCH
  bool set4_mismatch_warned_ = false;
  uint64_t set4_nvk_previous_[9] = {};
#endif

  bool nvk_prefetch_app_ = false;
  bool nvk_draw_annotated_ = false;
  bool nvk_improvements_requested_ = true;
  bool nvk_off_warned_[5] = {};

  bool copies_nvk_annotated_ = false;
  bool copies_nvk_hashes_ = false;
  int32_t copies_nvk_engine_ = -1;
  bool copies_nvk_mismatch_warned_ = false;
  std::chrono::steady_clock::time_point copies_nvk_report_{};
  uint64_t copies_nvk_state_previous_[7] = {};
  uint64_t copies_nvk_copies_previous_[6] = {};

  bool kind_views_annotated_ = false;
  bool kind_views_on_ = false;
  std::chrono::steady_clock::time_point kind_views_report_{};
  uint64_t kind_views_previous_[6] = {};
  std::chrono::steady_clock::time_point nvk_draw_start_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point nvk_report_{};
  uint64_t nvk_partes_previous_[16][2] = {};
  uint64_t nvk_counts_previous_[13] = {};
  uint64_t nvk_improvements_previous_[5][4] = {};
  int32_t alternate_ubo_s_ = 0;
  uint64_t submits_ = 0;
  uint64_t submits_ubo_ = 0;
  std::chrono::steady_clock::time_point start_ubo_ = std::chrono::steady_clock::now();
  std::array<Heap, 4> heaps_{};
  std::array<NativeImage, 3> empty_{};
  VkSampler sampler_empty_ = VK_NULL_HANDLE;
  bool empty_prepared_ = false;

  std::unordered_map<const ShaderInput*, bool> probe_safe_shaders_;
  std::unordered_map<const ShaderInput*, bool> batch_safe_shaders_;
  std::unordered_map<const ShaderInput*, VkShaderModule> modules_;
  std::array<VkShaderModule, 5> modules_variants_{};
  std::array<bool, 5> modules_variants_created_{};
  int glow_annotated_ = 0;
  std::unordered_map<uint64_t, std::pair<PipelineKey, VkPipeline>> pipelines_;

  PipelineKey last_key_{};
  VkPipeline last_pipeline_ = VK_NULL_HANDLE;
  bool last_key_valid_ = false;

  struct PipelineCell {
    PipelineKey key{};
    VkPipeline pipeline = VK_NULL_HANDLE;
  };
  static constexpr size_t kCellsPipeline = 256;
  std::array<PipelineCell, kCellsPipeline> cells_pipeline_{};
  bool pipelines_direct_ = true;
  bool pipelines_direct_off_ = false;
  uint64_t pipelines_direct_hits_ = 0;
  uint64_t pipelines_direct_misses_ = 0;
  uint64_t pipelines_direct_checked_ = 0;
  uint64_t pipelines_direct_hits_previous_ = 0;
  uint64_t pipelines_direct_misses_previous_ = 0;
  std::chrono::steady_clock::time_point pipelines_direct_report_{};
  static constexpr uint64_t kPipelinesACheck = 200000;

  enum : uint32_t {
    kChangeBindings = 0,
    kChangeAfterPass,
    kChangeFirst,
    kChangeIdentical,
    kChangeShaders,
    kChangeInput,
    kChangeFormats,
    kChangeSpecAlpha,
    kChangeSpecZEarly,
    kChangeSpecOther,
    kChangeNoEffect,
    kChangeNoEffectAfterPass,
    kChangeOnlyState,
    kChangeStateAfterPass,
    kChangeEds12,
    kChangeEds12AfterPass,
    kFieldTopology,
    kFieldClassTopology,
    kFieldBlend,
    kFieldMasks,
    kFieldZ,
    kFieldStencil,
    kFieldFace,
    kFieldRestart,
    kFieldBias,
    kChangesN
  };
  std::array<uint64_t, kChangesN> changes_pipeline_{};
  PipelineKey key_bound_{};
  bool key_bound_valid_ = false;
  bool count_changes_pipeline_ = true;
  uint64_t changes_draws_previous_ = 0;
  uint64_t changes_frames_previous_ = 0;
  std::chrono::steady_clock::time_point changes_report_{};

  static constexpr uint64_t kCanonicalACheck = 200000;
  bool key_canonical_ = true;
  bool key_canonical_off_ = false;
  bool canonical_valid_ = false;
  PipelineKey canonical_raw_{};
  PipelineKey canonical_result_{};
  uint64_t canonical_changed_ = 0;
  uint64_t canonical_checked_ = 0;
  uint64_t canonical_changed_previous_ = 0;
  uint64_t canonical_pipelines_previous_ = 0;
  std::chrono::steady_clock::time_point canonical_report_{};

  bool pipeline_between_passes_ = true;
  bool pipeline_between_passes_annotated_ = false;
  uint64_t passes_with_pipeline_ = 0;
  uint64_t between_passes_passes_previous_ = 0;
  uint64_t between_passes_kept_previous_ = 0;
  std::chrono::steady_clock::time_point between_passes_report_{};

  enum : uint32_t { kEds12 = 1, kEds3 = 2 };
  static constexpr VkDynamicState kDynamicEds12[10] = {
      VK_DYNAMIC_STATE_CULL_MODE,           VK_DYNAMIC_STATE_FRONT_FACE,
      VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,  VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,   VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,  VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
      VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
  static constexpr uint64_t kEdsACheck = 200000;
  enum : uint32_t {
    kEdsFace = 0,
    kEdsFrontface,
    kEdsTopology,
    kEdsRestart,
    kEdsBias,
    kEdsTestZ,
    kEdsWritesZ,
    kEdsFunctionZ,
    kEdsStencil,
    kEdsStencilOps,
    kEdsBlendActive,
    kEdsEquation,
    kEdsMask,
    kEdsEverything,
    kEdsN
  };
  PFN_vkCmdSetCullMode set_face_ = nullptr;
  PFN_vkCmdSetFrontFace set_frontface_ = nullptr;
  PFN_vkCmdSetPrimitiveTopology set_topology_ = nullptr;
  PFN_vkCmdSetPrimitiveRestartEnable set_restart_ = nullptr;
  PFN_vkCmdSetDepthBiasEnable set_bias_ = nullptr;
  PFN_vkCmdSetDepthTestEnable set_test_z_ = nullptr;
  PFN_vkCmdSetDepthWriteEnable set_writes_z_ = nullptr;
  PFN_vkCmdSetDepthCompareOp set_function_z_ = nullptr;
  PFN_vkCmdSetStencilTestEnable set_stencil_ = nullptr;
  PFN_vkCmdSetStencilOp set_stencil_ops_ = nullptr;
  bool eds12_available_ = false;
  bool eds_off_ = false;
  bool eds_mode_annotated_ = false;
  uint32_t eds_mode_ = 0;
  bool eds_valid_ = false;
  EdsState eds_recorded_{};
  PipelineKey eds_key_{};
  uint64_t eds_draws_ = 0;
  uint64_t eds_repeated_ = 0;
  uint64_t eds_checkable_ = 0;
  uint64_t eds_checked_ = 0;
  uint64_t eds_draws_previous_ = 0;
  uint64_t eds_repeated_previous_ = 0;
  uint64_t eds_frames_previous_ = 0;
  std::array<uint64_t, kEdsN> eds_calls_{};
  std::array<uint64_t, 4> pipelines_per_mode_{};
  std::chrono::steady_clock::time_point eds_report_{};

  static constexpr VkDynamicState kDynamicEds3[3] = {VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};

  static constexpr VkBlendFactor kFactorsBlend[32] = {
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_SRC_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  PFN_vkCmdSetColorBlendEnableEXT set_blend_active_ = nullptr;
  PFN_vkCmdSetColorBlendEquationEXT set_equation_ = nullptr;
  PFN_vkCmdSetColorWriteMaskEXT set_mask_ = nullptr;
  bool eds3_available_ = false;
  std::unordered_map<uint64_t, VkRenderPass> passes_;
  std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;


  std::unordered_map<uint64_t, std::array<VkImageView, 5>> framebuffers_views_;
  std::vector<VkFramebuffer> fb_retired_;
  bool fb_retire_ = false;
  uint64_t fb_views_forgotten_ = 0;
  uint64_t fb_forgotten_ = 0;
  std::unordered_map<uint64_t, CachedTexture> textures_;


  std::unordered_multimap<uint64_t, uint64_t> live_per_content_;
  uint64_t contents_different_ = 0;
  int32_t diag_reuse_ = -1;
  uint64_t reuse_new_ = 0;
  uint64_t reuse_match_ = 0;
  uint64_t reuse_cold_ = 0;
  uint64_t reuse_no_look_ = 0;
  uint64_t reuse_bytes_new_ = 0;
  uint64_t reuse_bytes_match_ = 0;
  uint64_t reuse_bytes_cold_ = 0;
  uint32_t reuse_details_ = 0;
  std::chrono::steady_clock::time_point report_reuse_{};
  std::unordered_map<uint64_t, View> views_;


  std::unordered_map<VkImage, std::vector<uint64_t>> views_per_image_;
  std::unordered_map<uint64_t, std::pair<VkSampler, uint32_t>> samplers_;

  uint32_t aniso_seen_ = 0;
  bool aniso_annotated_ = false;
  uint32_t diag_mip_min_ = 0;
  uint32_t preset_mip_min_ = 0;

  int32_t shadows_bias_constant_ = 0;
  int32_t shadows_bias_pending_ = 0;
  bool shadows_bias_annotated_ = false;
  uint32_t mip_max_seen_ = 0;
  std::array<bool, 1024> biases_seen_{};
  std::vector<CachedTexture*> textures_a_upload_;
  std::vector<uint8_t> temporal_;
  uint64_t staging_alloc_failures_ = 0;
  std::vector<uint32_t, UninitializedAllocator<uint32_t>> indices_;
  std::vector<uint32_t, UninitializedAllocator<uint32_t>> converted_;
  std::vector<uint16_t, UninitializedAllocator<uint16_t>> indices16_;

  int32_t indices_neon_ = -1;
  uint64_t indices_neon_draws_ = 0;
  uint64_t indices_neon_checked_ = 0;
  std::vector<uint16_t, UninitializedAllocator<uint16_t>> indices_neon_test_;

  bool state_recorded_ = false;
  uint64_t push_recorded_[3] = {};
  VkViewport viewport_recorded_{};
  VkRect2D scissor_recorded_{};
  float blend_recorded_[4] = {};
  float bias_recorded_[2] = {};
  float bias_warned_[2] = {};
  uint32_t warnings_bias_ = 0;
  std::string skip_text_;
  std::unordered_set<uint32_t> skip_ps_;
  uint32_t stencil_recorded_[2] = {};
  bool stencil_recorded_valid_ = false;
  VkIndexType type_indices_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  VkBuffer buffer_indices_recorded_ = VK_NULL_HANDLE;


  int32_t icache_ = -1;
  bool icache_ab_ = false, icache_active_ = false;


  int32_t constants_used_ = -1;
  bool constants_used_ab_ = false, constants_used_active_ = false;


  std::array<uint8_t, kRegistersConstants * 4> constants_vs_shadow_{};
  std::array<uint8_t, kRegistersConstants * 4> constants_ps_shadow_{};
  const ShaderInput* constants_vs_shader_ = nullptr;
  const ShaderInput* constants_ps_shader_ = nullptr;
  int32_t prefetch_ = -1;
  bool prefetch_ab_ = false, prefetch_lines_ = false;
  VkDeviceSize indices_pool_ = 0;
  uint64_t icache_hits_ = 0, icache_bytes_hit_ = 0, icache_filled_ = 0, icache_bytes_filled_ = 0, icache_changed_ = 0;
  uint64_t icache_full_ = 0, icache_guard_seen_ = 0, icache_checked_ = 0, icache_unstamped_ = 0;
  uint64_t icache_unstamped_interval_ = 0;
  std::chrono::steady_clock::time_point icache_next_{};


  static constexpr size_t kSharedBytes = sizeof(uint32_t) * kWordsShared;
  uint32_t shared_buffers_[2][kWordsShared] = {};
  uint32_t shared_buffer_ = 0;
  bool fewer_copies_ = false, shared_swap_ = false;
  uint64_t shared_observed_ = 0;
  uint64_t shared_changed_ = 0;
  VkDeviceSize shared_offset_ = 0;
  uint64_t shared_epoch_ = UINT64_MAX;

  VertexInput input_;
  const VertexInput* input_current_ = &input_;
  std::unordered_map<uint64_t, VertexInput> inputs_cache_;
  uint64_t inputs_cache_hits_ = 0;

  struct InputFront {
    uint64_t generation = 0;
    const ShaderInput* vs = nullptr;
    const VertexInput* input = nullptr;
  };
  static constexpr uint32_t kInputFrontBits = 8;
  std::array<InputFront, size_t(1) << kInputFrontBits> inputs_front_{};
  bool inputs_front_on_ = false;
  bool inputs_front_off_ = false;
  uint64_t inputs_front_hits_ = 0;
  static constexpr uint64_t kInputFrontChecks = 2000;
  uint64_t inputs_front_checks_ = 0;
  uint64_t inputs_front_sampled_ = 0;
  const ShaderInput* input_vs_ = nullptr;
  uint64_t input_generation_ = UINT64_MAX;
  bool input_valid_ = false;

  uint64_t constants_vs_generation_ = UINT64_MAX;
  uint64_t constants_vs_epoch_ = UINT64_MAX;
  VkDeviceSize constants_vs_offset_ = 0;
  uint64_t constants_ps_generation_ = UINT64_MAX;
  uint64_t constants_ps_epoch_ = UINT64_MAX;
  VkDeviceSize constants_ps_offset_ = 0;
  uint32_t constants_vs_bytes_ = 0;
  uint32_t constants_ps_bytes_ = 0;

  bool pass_active_ = false;

  bool occlusion_open_ = false;

  static constexpr uint64_t kWitnessEvery = 32;
  bool measure_visibility_ = false;
  bool witness_pending_ = false;
  uint64_t witness_next_ = 0;
  uint32_t query_occlusion_ = UINT32_MAX;
  uint32_t category_pass_ = kGpuOthers;
  bool submitted_after_shadows_ = false;

  std::string diag_skip_ps_text_;
  bool diag_vertices_repeated_ = false;
  bool diag_stats_draw_ = false;
  uint32_t scissor_test_ = 0;
  std::array<VkBuffer, 16> buffers_vertices_{};

  std::unordered_map<uint32_t, uint32_t> height_useful_;
  bool area_to_tile_ = false;

  int readonly_depth_ = -1;
  bool draw_writes_ds_ = false;
  uint64_t draw_learn_key_ = 0;
  bool pass_has_depth_ = false;
  bool pass_opened_nonwriting_ = false;
  bool pass_depth_readonly_ = false;
  bool pass_ds_written_ = false;
  mutable bool pass_readonly_blocked_ = false;
  uint64_t pass_learn_key_ = 0;
  uint64_t pass_readonly_texels_ = 0;
  std::unordered_map<uint64_t, bo2zm::native::v055::ReadOnlyLearn> readonly_learn_;
  bool area_util_ = true;

  bool framing_cache_ = true;
  bool framing_cache_off_ = false;
  bool framing_valid_ = false;
  uint64_t framing_generation_ = 0;
  uint64_t framing_pass_series_ = 0;
  uint64_t pass_series_ = 0;
  VkViewport framing_viewport_{};
  VkRect2D framing_scissor_{};
  float framing_ndc_[4] = {};
  uint32_t framing_empty_ = 0;
  uint64_t framing_hits_ = 0;
  uint64_t framing_checked_ = 0;
  uint64_t framing_hits_report_ = 0;
  uint64_t framing_calcs_report_ = 0;
  static constexpr uint64_t kFramingsACheck = 200000;
  bool height_useful_memo_active_ = true;
  bool height_useful_memo_off_ = false;
  uint32_t* height_useful_memo_ = nullptr;
  uint32_t height_useful_memo_pitch_ = 0;
  uint64_t height_useful_memo_hits_ = 0;
  uint64_t height_useful_memo_hits_previous_ = 0;
  static constexpr uint64_t kHeightUsefulACheck = 200000;
  bool key_pass_fast_ = true;
  bool key_pass_fast_off_ = false;
  uint64_t pass_keys_[5] = {};
  bool pass_keys_valid_ = false;
  uint64_t keys_pass_fast_ = 0;
  uint64_t keys_pass_fast_previous_ = 0;
  static constexpr uint64_t kKeysPassACheck = 200000;
  bool cvars_per_frame_ = true;
  bool ps_only_alpha_frame_ = true;
  bool no_ps_no_color_frame_ = true;
  std::chrono::steady_clock::time_point minor_report_{};
  bool no_vegetation_ = false;

  uint64_t draws_vegetation_soon_ = 0;

  bool vegetation_computed_ = false;
  bool vegetation_ring_ = false;
  uint16_t vegetation_flags_ = 0;
  VegetationDetail vegetation_detail_;


  enum : uint32_t {
    kSkyPerBlend = 0,
    kSkyPerNoZ,
    kSkyAtPassEnd,
    kSkyPerOtherSky,
    kSkyReasons
  };
  struct PostponedSky {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    bool uses_ubo = false;
    uint64_t push[3] = {};
    std::array<uint32_t, 3> offsets_ubo{};
    uint32_t slot_ubo = 0;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    float bias[2] = {};
    bool with_stencil = false;
    uint32_t stencil[2] = {};
    uint32_t n_bindings = 0;
    std::array<VkDeviceSize, 16> offsets_vertices{};
    bool with_indices = false;
    bool indices_16 = false;
    uint32_t indices = 0;
    uint32_t first_index = 0;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    uint32_t vmin = 0;
    uint32_t count = 0;
    uint32_t ps_more_one = 0;
    uint32_t vs_more_one = 0;
    uint32_t category = 0;
    uint64_t draws_on_postpone = 0;
    uint32_t eds_mode = 0;
    EdsState eds{};
  };
  PostponedSky sky_{};
  bool sky_pending_ = false;
  bool sky_postpone_on_ = true;
  bool sky_annotated_ = false;
  uint64_t draws_in_pass_ = 0;
  uint64_t sky_seen_ = 0;
  uint64_t sky_seen_previous_ = 0;


  uint64_t sky_candidates_ = 0;
  uint64_t sky_candidates_previous_ = 0;


  enum : uint32_t { kSkyObserving = 0, kSkyPostponing = 1, kSkyDiscarded = 2 };
  uint32_t sky_guard_ = kSkyObserving;
  uint32_t sky_guard_frames_ = 0;
  uint32_t sky_guard_with_one_ = 0;
  uint32_t sky_guard_in_frame_ = 0;
  uint32_t sky_guard_max_ = 0;
  uint64_t skies_postponed_ = 0;
  uint64_t sky_postponed_previous_ = 0;
  uint64_t sky_no_postponable_ = 0;
  uint64_t sky_two_in_pass_ = 0;
  uint64_t sky_lost_ = 0;
  std::array<uint64_t, kSkyReasons> sky_emitted_{};
  std::array<uint64_t, kSkyReasons> sky_emitted_previous_{};
  uint64_t sky_position_sum_ = 0;
  uint64_t sky_position_sum_previous_ = 0;
  uint64_t sky_position_max_ = 0;

  std::array<VkDeviceSize, 16> offsets_recorded_{};
  uint32_t bindings_recorded_ = 0;
  uint64_t bindings_saved_ = 0;

  bool vertices_base_cero_ = true;
  bool vertices_base_zero_off_ = false;
  uint64_t base_zero_checked_ = 0;
  uint64_t base_zero_draws_ = 0;
  uint64_t base_cero_total_ = 0;
  uint64_t base_zero_several_bindings_ = 0;
  uint64_t base_zero_misaligned_ = 0;
  uint64_t base_zero_bindings_recorded_ = 0;
  std::chrono::steady_clock::time_point base_zero_report_{};
  static constexpr uint64_t kBaseZeroACheck = 200000;
  std::array<uint64_t, 51> sub_ns_{};
  uint64_t bytes_constants_vs_ = 0, bytes_constants_ps_ = 0;
  std::array<uint64_t, 51> sub_n_{};
  uint64_t sub_samples_ = 0;
  std::chrono::steady_clock::time_point sub_next_{};
  uint64_t submits_after_shadows_ = 0;
  uint32_t stats_pass_ = UINT32_MAX;
  uint32_t warnings_occlusion_draw_ = 0;
  std::chrono::steady_clock::time_point last_warning_occlusion_draw_{};
  VkCommandBuffer pass_commands_ = VK_NULL_HANDLE;
  uint64_t pass_key_ = 0;
  uint64_t pass_generation_ = UINT64_MAX;
  uint32_t pass_width_ = 0;
  uint32_t pass_height_ = 0;
  float pass_scale_x_ = 1.0f;
  float pass_scale_y_ = 1.0f;
  uint64_t clears_depth_in_pass_ = 0;
  std::chrono::steady_clock::time_point start_pass_{};
  uint32_t pass_formats_[5] = {};


  std::array<const NativeImage*, 5> pass_images_{};
  std::array<VkImage, 5> pass_handles_{};
  uint32_t pass_area_height_ = 0;

  uint32_t pass_area_width_ = 0;
  uint32_t pass_pitch_ = 0;
  bool pass_fitted_ = false;
  uint32_t pass_store_discard_ = 0;
  uint32_t edram_mismatch_logged_ = 0;
  std::unordered_map<uint32_t, bo2zm::native::v046::DrawnExtent> drawn_extent_;
  bo2zm::native::v046::DrawnExtent* drawn_extent_memo_ = nullptr;
  uint32_t drawn_extent_memo_pitch_ = 0;
  VkRenderPass pass_rp_ = VK_NULL_HANDLE;
  uint64_t recording_generation_ = UINT64_MAX;
  VkPipeline pipeline_bound_ = VK_NULL_HANDLE;
  bool sets_bound_ = false;

  uint32_t diagnostics_ = 0;
  std::unordered_set<uint32_t> warned_vs_;
  uint32_t warnings_indices_ = 0;
  uint32_t warnings_swizzle_ = 0;
  std::unordered_set<uint64_t> diagnosed_;
  uint64_t drawn_ = 0;
  uint64_t drawn_timed_ = 0;
  uint32_t stopwatch_counter_ = 0;
  bool time_ = false;

  uint64_t cubes_refreshed_ = 0;
  uint64_t cubes_refreshed_previous_ = 0;
  uint64_t frame_report_cubes_ = 0;
  std::chrono::steady_clock::time_point report_cubes_{};
  uint64_t rejected_ = 0;


  std::array<uint64_t, kStagesDraw> stages_ns_{};
  std::unordered_map<uint32_t, uint64_t> causes_;
  uint32_t last_cause_ = 0;
  uint32_t input_cause_ = 0;
  uint64_t uploads_texture_ = 0;
  uint64_t megs_bytes_ = 0;
  uint64_t bytes_textures_ = 0;
  int32_t textures_mb_max_ = 0;


  TexturePool pool_textures_;

  struct BindRequest {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory guest_memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkResult result = VK_NOT_READY;
    uint64_t ns = 0;
  };
  struct TextureInFlight {
    CachedTexture* texture = nullptr;
    uint64_t ticket = 0;
    VkImage image = VK_NULL_HANDLE;
    VkResult result = VK_NOT_READY;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 1;
    uint32_t depth3d = 0;
    uint32_t levels = 1;
    bool copy = false;
    VkDeviceSize copy_offset = 0;
    uint64_t copy_epoch = 0;
    VkCommandBuffer copy_commands = VK_NULL_HANDLE;
  };
  struct ViewInFlight {
    uint64_t key = 0;
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t swizzle = 0;
    uint16_t swizzle_host = 0;
    uint32_t heap = 0;
    uint32_t slot = 0;
  };
  static constexpr uint64_t kQueueBindings = 256;
  static constexpr uint64_t kBindingsACheck = 64;
  static constexpr int32_t kBindingsNoDecide = -1;
  static constexpr int32_t kBindingsOff = 0;
  static constexpr int32_t kBindingsObserving = 1;
  static constexpr int32_t kBindingsApplying = 2;
  std::array<BindRequest, kQueueBindings> queue_bindings_{};
  std::mutex bindings_mutex_;
  std::condition_variable bindings_cv_;
  std::condition_variable bindings_done_cv_;
  uint64_t bindings_requested_ = 0;
  uint64_t bindings_done_ = 0;
  uint64_t bindings_collected_ = 0;
  bool bindings_sleeping_ = false;
  bool bindings_waiting_ = false;
  bool bindings_stop_ = false;
  bool bindings_priority_ok_ = true;
  uint64_t ns_bindings_thread_ = 0;
  uint64_t ns_binding_worst_ = 0;
  uint64_t bindings_queue_full_ = 0;
  std::thread bindings_thread_;
  int32_t bindings_priority_ = 0x2D;

  int32_t bindings_phase_ = kBindingsNoDecide;
  bool bindings_stuck_ = false;
  bool measuring_creation_ = false;
  bool collecting_bindings_ = false;
  std::vector<TextureInFlight> in_flight_;
  std::vector<ViewInFlight> views_in_flight_;
  std::unordered_set<VkImage> images_in_flight_;
  uint64_t bindings_checked_ = 0;
  uint64_t bindings_thread_total_ = 0;
  uint64_t bindings_failed_ = 0;
  uint64_t views_postponed_ = 0;
  uint64_t copies_postponed_ = 0;
  uint64_t slots_lost_ = 0;
  uint64_t ns_wait_bindings_total_ = 0;

  std::chrono::steady_clock::time_point report_bindings_{};
  uint64_t bindings_thread_report_ = 0;
  uint64_t waits_bindings_report_ = 0;
  uint64_t ns_wait_bindings_report_ = 0;
  uint64_t ns_wait_bindings_worst_ = 0;
  uint64_t ns_create_report_ = 0;
  uint64_t created_report_previous_ = 0;
  uint64_t views_postponed_report_ = 0;
  uint64_t copies_postponed_report_ = 0;
  uint64_t ns_bindings_thread_previous_ = 0;
  uint64_t queue_full_previous_ = 0;
  uint32_t reports_no_compensate_ = 0;

  std::array<HashJob, kQueueHashes> queue_hashes_{};
  std::mutex hashes_mutex_;
  std::condition_variable hashes_cv_;
  std::condition_variable hashes_done_cv_;
  uint64_t hashes_published_ = 0;
  uint64_t hashes_taken_ = 0;
  uint64_t hashes_done_ = 0;
  bool hashes_sleeping_ = false;
  bool hashes_waiting_ = false;
  bool hashes_stop_ = false;
  bool hashes_priority_ok_ = true;
  bool hashes_core_ok_ = true;
  int hashes_core_real_ = -1;
  std::thread hashes_thread_;
  int32_t hashes_priority_ = 0x2E;
  int32_t hashes_core_requested_ = -2;
  int32_t hashes_core_ = -1;


  std::unique_ptr<uint8_t[]> snapshots_;
  std::unique_ptr<HashRead[]> reads_hash_;
  uint64_t snapshots_bytes_ = 0;

  int32_t hashes_phase_ = kHashesNoDecide;
  uint64_t hashes_collected_ = 0;
  uint64_t snapshots_used_ = 0;
  uint32_t reads_used_ = 0;
  std::array<HashRead, kReadsPerTexture> reads_plan_{};
  std::vector<HashJob> hashes_planned_;
  uint64_t hashes_planned_pending_ = 0;
  std::vector<HashJob> collected_hash_jobs_;
  std::array<HashComparison, kQueueHashes> comparisons_hash_{};
  const CachedTexture* comparison_texture_ = nullptr;
  uint64_t comparison_sequence_ = 0;
  std::vector<uint8_t> hashes_data_ring_;
  std::vector<uint8_t> hashes_check_ring_;
  bool hashes_fast_off_ring_ = false;
  bool hashes_no_thread_ = false;
  bool hashes_stuck_ = false;
  bool hashes_suspicious_ = false;
  bool collecting_hashes_ = false;
  uint64_t hashes_comparisons_planned_ = 0;
  uint64_t hashes_new_applying_ = 0;
  uint64_t hashes_thread_total_ = 0;
  uint64_t hashes_ring_total_ = 0;
  uint64_t hashes_compared_ = 0;
  uint64_t hashes_no_compare_ = 0;
  uint64_t hashes_discarded_ = 0;
  uint64_t hashes_reuploads_ = 0;

  std::chrono::steady_clock::time_point report_hashes_{};
  uint64_t hashes_planned_report_ = 0;
  uint64_t hashes_comparison_report_ = 0;
  uint64_t hashes_thread_report_ = 0;
  uint64_t hashes_ring_report_ = 0;
  uint64_t ns_hashes_thread_report_ = 0;
  uint64_t ns_hash_raw_thread_report_ = 0;
  uint64_t ns_hashes_ring_report_ = 0;
  uint64_t ns_snapshots_report_ = 0;
  uint64_t bytes_snapshots_report_ = 0;
  uint64_t ns_wait_hashes_report_ = 0;
  uint64_t ns_wait_hashes_worst_ = 0;
  uint64_t waits_hashes_report_ = 0;
  uint64_t hashes_no_room_report_ = 0;
  uint32_t reports_no_compensate_hashes_ = 0;
  int32_t test_no_memory_every_ = 0;
  uint64_t texture_reserves_ = 0;
  bool releasing_on_out_of_memory_ = false;
  uint64_t released_little_a_little_ = 0;
  uint64_t warning_trickle_ = 0;


  std::vector<std::pair<uint64_t, uint64_t>> cold_list_;
  uint64_t cold_scan_frame_ = 0;
  bool cold_scan_full_ = false;
  uint64_t cold_scans_ = 0;
  std::vector<uint64_t> keys_release_;
  uint64_t attempt_eviction_ = 0;
  uint64_t textures_released_ = 0;


  uint64_t textures_created_ = 0;
  uint64_t created_in_address_view_ = 0;
  uint64_t created_same_shape_other_key_ = 0;


  static constexpr uint32_t kDiagSlotBits = 15;
  static constexpr size_t kDiagSlots = size_t(1) << kDiagSlotBits;
  struct DiagShape {
    uint64_t shape = 0;
    uint64_t key = 0;
    std::array<uint32_t, 5> words{};
  };
  std::vector<uint32_t> diag_addresses_;
  std::vector<DiagShape> diag_shapes_;
  uint64_t keys_incoherent_ = 0;
  uint32_t warnings_other_key_ = 0;

  uint64_t passes_begun_ = 0;
  uint64_t submits_full_ = 0;
  uint64_t ns_submits_full_ = 0;
  uint64_t bytes_vertices_ = 0;
  uint64_t bytes_indices_uploaded_ = 0;
  uint64_t samplers_prepared_ = 0;
  uint64_t samplers_cache_ = 0;
  uint64_t ns_passes_ = 0;
  uint64_t ns_vertices_ = 0;
  uint64_t inputs_computed_ = 0;
  uint64_t ns_inputs_ = 0;
  uint64_t inputs_reused_ = 0;
  uint64_t passes_per_generation_ = 0;
  uint64_t passes_per_target_ = 0;
  uint64_t passes_resumed_ = 0;
  uint64_t texels_passes_ = 0;
  std::array<uint64_t, kGpuCategories> texels_per_category_{};
  std::array<uint64_t, kGpuCategories> draws_per_category_{};
  std::chrono::steady_clock::time_point start_alternation_ps_ = std::chrono::steady_clock::now();
  bool alternation_ps_annotated_ = false;
  bool alternation_only_alpha_annotated_ = false;
  bool inv_size_tex_ = false;
  bool pcf_cheap_ = false;
  bool pass_overlay_shadow_ = false;
  bool no_blur_frame_ = true;
  bool mip_point_test_ = false;

  DedupeVertices dedupe_;
  bool dedupe_active_ = true;

  struct VertexPool {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* data = nullptr;
    VkDeviceSize size = 0;
    VkDeviceSize size_real = 0;
    VkDeviceSize used = 0;
    uint64_t frame_left = 0;
    VkDeviceSize dirty_lo = ~VkDeviceSize(0), dirty_hi = 0;
  };
  struct VertexCacheEntry {
    uint64_t key = 0;
    uint64_t stamp = 0;
    uint64_t hash_seen = 0;
    uint64_t hash_pool = 0;
    uint64_t sum_pool = 0;
    bool seen_by_sum = false;
    VkDeviceSize offset = 0;
    uint32_t pool = 0;
    uint32_t generation = 0;
    uint32_t copy_epoch = 0;
    uint32_t seen_epoch = 0;
    uint32_t hits = 0;
    uint16_t min = 0, max = 0;
  };
  std::array<VertexPool, 2> vpools_{};
  uint32_t vpool_ = 0;
  std::array<uint32_t, 2> vpool_generation_{1, 1};
  static constexpr size_t kVertexCacheSets = 65536;
  std::vector<VertexCacheEntry> vcache_;
  std::vector<uint8_t> vcache_scratch_;
  bool vcache_ready_ = false, vcache_active_ = false, vcache_off_ = false, vcache_coherent_ = false;
  bool vcache_by_writes_ = false;

  int32_t tex_by_writes_ = -1;
  bool tex_by_writes_ab_ = false, tex_by_writes_active_ = false;
  uint64_t tex_stamp_seen_ = 0, tex_stamp_skipped_ = 0, tex_stamp_guarded_ = 0, tex_stamp_written_ = 0;
  uint64_t tex_unstamped_ = 0, tex_unstamped_interval_ = 0;

  int32_t tex_young_ = -1;
  bool tex_young_ab_ = false, tex_young_active_ = false;
  uint64_t tex_stamp_skipped_young_ = 0, tex_unstamped_young_ = 0;
  std::chrono::steady_clock::time_point tex_stamps_next_{};


  uint32_t prepare_outcome_ = 0;
  bool prepare_new_ = false;

  int32_t texreuse_ = -1;
  bool texreuse_ab_ = false, texreuse_active_ = false;
  uint64_t texreuse_taken_ = 0, texreuse_bytes_ = 0, texreuse_missed_ = 0, texreuse_taken_total_ = 0;
  std::chrono::steady_clock::time_point texreuse_next_{};
  uint64_t vcache_unstamped_ = 0, vcache_unstamped_interval_ = 0;
  bool vcache_ab_ = false;
  bool vcache_sum_ = true;
  bool vcache_sum_ab_ = false;
  uint64_t vcache_hits_ = 0, vcache_bytes_hit_ = 0, vcache_filled_ = 0, vcache_bytes_filled_ = 0;
  uint64_t vcache_changed_ = 0, vcache_bytes_hashed_ = 0, vcache_ns_hash_ = 0, vcache_full_ = 0;
  uint64_t vcache_switches_ = 0, vcache_guard_seen_ = 0, vcache_checked_ = 0, vcache_evicted_ = 0;
  std::chrono::steady_clock::time_point vcache_report_{};

  static constexpr uint64_t kDedupeChecks = 20000;
  bool dedupe_hash_frame_ = false;
  uint64_t dedupe_hits_frame_start_ = 0;
  uint64_t dedupe_checked_ = 0;
  uint64_t dedupe_discrepancies_seen_ = 0;
  bool dedupe_keep_flush_ = false;


  void GuardDedupe() {
    if (dedupe_hash_frame_) {
      dedupe_checked_ += dedupe_.hits() - dedupe_hits_frame_start_;
    }
    if (dedupe_.discrepancies() != dedupe_discrepancies_seen_) {
      dedupe_discrepancies_seen_ = dedupe_.discrepancies();
      if (!g_syncs_on_flush_waits.exchange(true, std::memory_order_relaxed)) {
        REXLOG_ERROR("[native] C6 upload dedupe MISMATCH: a reused vertex range's guest bytes changed after {} "
                     "checked reuses; the dedupe forgets at every WAIT_REG_MEM again for the rest of the session",
                     dedupe_checked_);
      }
    }
    dedupe_hash_frame_ = dedupe_keep_flush_ && (dedupe_checked_ < kDedupeChecks || (frame_ & 511) == 0);
    dedupe_hits_frame_start_ = dedupe_.hits();
  }

  DedupeIndices dedupe_indices_;
  bool dedupe_indices_on_ = true;
  bool dedupe_indices_off_ = false;
  uint32_t dedupe_indices_sync_view_ = 0;
  const DedupeIndices::Entry* indices_known_ = nullptr;
  bool indices_scanned_ = true;
  const uint8_t* index_data_ = nullptr;
  bool index_flip_ = false;
  uint32_t index_base_ = 0;
  uint32_t index_order_ = 0;
  static constexpr uint64_t kDedupeIndicesChecks = 20000;
  uint64_t dedupe_indices_hits_ = 0;
  uint64_t dedupe_indices_sampled_ = 0;
  uint64_t dedupe_indices_checks_ = 0;
  uint64_t dedupe_indices_converted_ = 0;
  uint64_t bytes_indices_saved_ = 0;
  uint64_t dedupe_indices_report_[4] = {};


  void CheckKnownIndices(uint32_t count, uint32_t min, uint32_t max) {
    ++dedupe_indices_checks_;
    bool same = indices_known_->min == min && indices_known_->max == max;
    if (same && indices_known_->hash) {
      same = XXH3_64bits(index_data_, size_t(count) * 2) == indices_known_->hash;
    }
    if (!same) {
      REXLOG_ERROR("[native] C6 index dedupe MISMATCH after {} checks: range {:08X} ({} indices) had min {} max {}, "
                   "the scan gives {} and {}{}. Off for the rest of the session",
                   dedupe_indices_checks_, index_base_, count, indices_known_->min, indices_known_->max, min, max,
                   indices_known_->min == min && indices_known_->max == max ? " (the uploaded copy differs)" : "");
      dedupe_indices_off_ = true;
      dedupe_indices_on_ = false;
      indices_known_ = nullptr;
      return;
    }
    if (dedupe_indices_checks_ == kDedupeIndicesChecks) {
      REXLOG_INFO("[native] C6 index dedupe: {} reuses checked against a fresh scan, 0 differences: applying (1 in "
                  "4,096 still checked)",
                  kDedupeIndicesChecks);
    }
    indices_scanned_ = true;
  }
  uint32_t dedupe_sync_view_ = 0;
  std::set<uint64_t> warnings_invsize_resolved_;
  bool alternation_scissor_annotated_ = false;
  std::unordered_map<const ShaderInput*, VkShaderModule> modules_only_alpha_;

  std::unordered_map<const ShaderInput*, VkShaderModule> modules_z_early_;
  uint64_t draws_ps_useless_ = 0;
  uint64_t draws_ps_needed_ = 0;
  uint64_t shadows_alpha_active_ = 0;
  uint64_t scene_with_discard_ = 0;
  uint64_t scene_no_discard_ = 0;


  enum : uint32_t {
    kZSet = 0,
    kZWritesZ,
    kZStencil,
    kZOcclusion,
    kZAlreadyEarly,
    kInvisibleBlend,
    kInvisibleAlpha,
    kInvisibleOnlyColor,
    kZCounts
  };
  std::array<uint64_t, kZCounts> counts_z_{};
  std::array<uint64_t, kZCounts> counts_z_previous_{};
  uint64_t z_early_no_module_ = 0;
  uint64_t frames_z_ = 0;
  uint64_t frames_z_previous_ = 0;
  std::chrono::steady_clock::time_point last_report_z_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point start_alternation_z_ = std::chrono::steady_clock::now();
  bool z_early_ = true;
  bool alternation_z_annotated_ = false;
  bool skip_invisible_ = true;
  uint64_t shadows_alpha_off_ = 0;
  std::array<uint64_t, kGpuCategories> triangles_per_category_{};
  std::array<uint64_t, kGpuCategories> passes_per_category_{};
  uint64_t ns_render_pass_ = 0;

  std::chrono::steady_clock::time_point start_shadows_ = std::chrono::steady_clock::now();
  bool shadows_skipped_ = false;

  struct SeenVertices {
    uint64_t frame = UINT64_MAX;
    uint64_t hash = 0;
  };
  std::unordered_map<uint64_t, SeenVertices> vertices_seen_;
  uint64_t bytes_repeated_frame_ = 0;
  uint64_t bytes_equal_earlier_ = 0;
  uint64_t ns_hash_vertices_ = 0;

  struct CacheSampler {
    std::array<uint32_t, 6> fetch{};
    uint64_t frame = UINT64_MAX;
    uint64_t generation = 0;
    uint32_t slot = 0;
    uint32_t heap = 0;
    uint32_t sampler = 0;
    uint32_t mip_min = 0;
    uint32_t generation_slot = 0;
    uint64_t valid_until = 0;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  bool cache_between_frames_ = true;
  bool mipmaps_ = true;
  bool diag_mips_ = false;
  uint64_t mips_reviewed_ = 0;
  uint64_t mips_rare_ = 0;
  uint64_t levels_mip_uploaded_ = 0;
  std::array<CacheSampler, 16> cache_samplers_{};


  std::array<CacheSampler, 4096> cache_fetch_{};
  std::array<uint32_t, 4096> cache_fetch_dir_{};
  uint64_t samplers_cache_fetch_ = 0;
  uint64_t fetch_misses_collision_ = 0;
  uint64_t fetch_misses_empty_ = 0;
  uint64_t fetch_misses_generation_ = 0;

  uint64_t invalidations_global_ = 0;
  uint64_t invalidations_retired_ = 0;
  uint64_t invalidations_targeted_ = 0;
  std::array<uint64_t, 3> invalidations_previous_{};
  uint64_t fetch_misses_expired_ = 0;
  std::array<uint64_t, 9> fetch_report_previous_{};
  std::chrono::steady_clock::time_point fetch_report_{};
  uint64_t generation_textures_ = 0;

  static constexpr uint64_t kReuseTexturesChecks = 20000;
  bool reuse_textures_on_ = true;
  bool reuse_valid_ = false;
  bool reuse_pending_ = false;
  uint32_t reuse_shared_[kWordsShared] = {};
  bool reuse_reads_reflection_ = false;
  const ShaderInput* reuse_ps_ = nullptr;
  uint32_t reuse_mip_min_ = 0;
  uint64_t reuse_generation_fetch_ = 0;
  uint64_t reuse_generation_textures_ = 0;
  uint64_t reuse_frame_ = 0;
  uint64_t reuse_slots_retired_ = 0;
  uint64_t slots_retired_ = 0;
  uint64_t reuse_checks_ = 0;
  uint64_t reuse_sampled_ = 0;
  uint64_t reuse_hits_ = 0;
  uint64_t reuse_by_value_ = 0;
  uint32_t reuse_fetch_[16 * 6] = {};


  bool SameSamplerFetch(const ShaderInput* ps, const uint32_t* r) const {
    if (!ps) {
      return true;
    }
    for (const SamplerShader& sampler : ps->samplers) {
      if (sampler.reg_index >= 16) {
        continue;
      }
      const uint32_t at = uint32_t(sampler.reg_index) * 6;
      if (std::memcmp(r + kRegFetch + at, reuse_fetch_ + at, 6 * sizeof(uint32_t)) != 0) {
        return false;
      }
    }
    return true;
  }


  std::array<std::vector<uint32_t>, 3> generation_slot_;
  uint32_t GenerationSlot(uint32_t heap, uint32_t slot) const {
    return heap < generation_slot_.size() && slot < generation_slot_[heap].size()
               ? generation_slot_[heap][slot]
               : 0;
  }
  void RetireSlot(uint32_t heap, uint32_t slot) {
    if (heap < generation_slot_.size() && slot < generation_slot_[heap].size()) {
      ++generation_slot_[heap][slot];
    }
    ++slots_retired_;
  }


  void CheckReuseTextures(const uint32_t* shared, bool reads_reflection, const ShaderInput* ps) {
    if (std::memcmp(shared, reuse_shared_, sizeof(reuse_shared_)) == 0 &&
        reads_reflection == reuse_reads_reflection_) {
      ++reuse_checks_;
      if (reuse_checks_ == kReuseTexturesChecks) {
        REXLOG_INFO("[native] C6 texture words reuse: {} reuses checked against the sampler loop, 0 differences: "
                    "APPLYING (1 in 1,024 still checked)",
                    reuse_checks_);
      }
      return;
    }
    uint32_t first = 0;
    while (first < kWordsShared && shared[first] == reuse_shared_[first]) {
      ++first;
    }
    REXLOG_WARN("[native] C6 texture words reuse MISMATCH after {} checks: word {} is {:08X}, the reuse had {:08X} "
                "(reflection {} / {}; PS n{}): reuse off for this session",
                reuse_checks_, first, first < kWordsShared ? shared[first] : 0u,
                first < kWordsShared ? reuse_shared_[first] : 0u, reads_reflection, reuse_reads_reflection_,
                ps ? int(ps->number) : -1);
    reuse_textures_on_ = false;
  }
  bool SlotCurrent(const CacheSampler& c) const {
    return c.generation_slot == GenerationSlot(c.heap, c.slot);
  }

  using FnCreatePipelineCache = VkResult(VKAPI_PTR*)(VkDevice, const VkPipelineCacheCreateInfo*,
                                                     const VkAllocationCallbacks*,
                                                     VkPipelineCache*);
  using FnPipelineCacheData = VkResult(VKAPI_PTR*)(VkDevice, VkPipelineCache, size_t*, void*);
  using FnDestroyPipelineCache = void(VKAPI_PTR*)(VkDevice, VkPipelineCache,
                                                    const VkAllocationCallbacks*);
  FnPipelineCacheData data_cache_ = nullptr;
  FnDestroyPipelineCache destroy_cache_ = nullptr;
  VkPipelineCache cache_pipelines_ = VK_NULL_HANDLE;
  uint32_t pipelines_no_save_ = 0;
  uint32_t saved_cache_ = 0;
  size_t bytes_cache_saved_ = 0;
  std::chrono::steady_clock::time_point cache_saved_{};


  std::thread writer_cache_;
  std::mutex writer_mutex_;
  std::condition_variable writer_warning_;
  bool writer_pending_ = false;
  bool writer_stop_ = false;


  bool writer_want_cache_ = false;
  bool writer_in_match_ = false;
  bool writer_flush_ = false;
  int writer_resave_ = 0;
  bool writer_verified_ = false;
  bool writer_vouch_ = false;
  std::atomic<bool> writer_list_waiting_{false};
  std::vector<uint8_t> writer_export_;
  std::vector<std::string> writer_map_events_;
  size_t exported_size_ = 0;
  std::chrono::steady_clock::time_point exported_at_{};
  std::atomic<bool> writer_hurry_{false};
  uint32_t pipelines_seen_ = 0;
  bool pipeline_was_in_match_ = false;
  std::chrono::steady_clock::time_point pipeline_new_at_{};
  std::chrono::steady_clock::time_point resave_at_{};
  uint32_t resaves_ = 0;


  std::vector<uint8_t> written_cache_;
  std::vector<uint8_t> written_list_;
  std::vector<uint8_t> list_read_;
  bool files_old_ = false;
  uint64_t ns_pipelines_ = 0;
  std::unordered_set<uint32_t> warned_;


  std::vector<PipelineRecord> list_file_;
  std::vector<uint8_t> state_list_;
  std::unordered_map<uint64_t, size_t> index_list_;
  std::vector<PipelineRecord> list_session_;
  uint32_t list_no_save_ = 0;
  std::vector<uint8_t> writer_list_;
  std::thread prewarm_thread_;
  std::thread prewarm_helper_;
  std::atomic<uint32_t> prewarm_compiled_helper_{0};
  std::atomic<bool> prewarm_helper_done_{false};
  const NativeShaders* library_prewarm_ = nullptr;
  uint32_t prewarm_eds_ = 0;
  bool prewarm_decided_ = false;
  bool prewarm_mismatch_ = false;
  std::chrono::steady_clock::time_point prewarm_start_{};
  std::chrono::steady_clock::time_point prewarm_report_{};
  uint64_t prewarm_changes_previous_ = 0;
  std::atomic<bool> prewarm_stop_{false};
  std::atomic<bool> prewarm_finished_{false};
  std::atomic<size_t> prewarm_until_{0};
  std::atomic<uint32_t> prewarm_done_{0};
  std::atomic<uint32_t> prewarm_compiled_{0};
  std::atomic<uint64_t> prewarm_ns_compiled_{0};
  std::atomic<uint32_t> prewarm_no_shader_{0};
  std::atomic<uint32_t> prewarm_other_mode_{0};
  std::atomic<uint32_t> prewarm_failed_{0};
  std::atomic<int32_t> prewarm_priority_{-1};
  uint32_t prewarm_counted_ = 0;
  bool precompiled_ = false;
  bool vouch_requested_ = false;
  uint64_t ring_prewarmed_ = 0, ring_prewarmed_slow_ = 0, ns_ring_prewarmed_ = 0;
  uint64_t list_ring_ = 0, list_ns_ring_ = 0;
  uint64_t ring_new_ = 0, ns_ring_new_ = 0, ring_a_list_ = 0;


  map_pipeline::Monitor map_pipeline_meter_;
  std::string map_pipeline_name_;
  std::unordered_set<uint64_t> map_pipeline_seen_;
  uint64_t map_first_pipeline_requests_ = 0;
  uint64_t map_first_pipeline_uncached_ = 0;


  int32_t async_compiles_ = -1;
  std::unordered_set<uint64_t> compile_pending_;
  std::mutex compile_mutex_;
  std::condition_variable compile_cv_;
  std::deque<CompileJob> compile_queue_;
  std::deque<CompileJob> compile_done_;
  std::atomic<bool> compiles_ready_{false};
  bool compile_stop_ = false;
  std::thread compile_thread_;
  std::vector<std::thread> compile_helpers_;
  std::atomic<uint32_t> compiles_helper_{0};
  uint32_t compiles_helper_reported_ = 0;
  uint64_t compiles_async_ = 0, ns_compiles_async_ = 0, compile_ns_worst_ = 0, draws_waiting_pipeline_ = 0;
  uint64_t compiles_reported_ = 0, ns_compiles_reported_ = 0, draws_waiting_reported_ = 0;
  std::chrono::steady_clock::time_point compile_report_{};
};

}

std::unique_ptr<VulkanDraws> VulkanDraws::Create(const VulkanDevice* vulkan_device,
                                                    rex::memory::Memory* guest_memory,
                                                    TargetContext* context) {
  if (!vulkan_device || !guest_memory || !context) {
    return nullptr;
  }
  auto draws = std::make_unique<VulkanDrawsImpl>(vulkan_device, guest_memory, context);
  if (!draws->Init()) {
    REXLOG_ERROR("[native] C6: could not prepare the native draws");
    return nullptr;
  }
  REXLOG_INFO("[native] C6: native draws prepared");
  return draws;
}

}
