#include "bo2zm_native_targets.h"
#include "bo2zm_native_v040.h"
#include "bo2zm_native_v046.h"
#include "bo2zm_native_v055.h"
#include "bo2zm_native_edram.h"
#include "bo2zm_native_resolution.h"
#include "bo2zm_display_state.h"
#include "bo2zm_native_frontbuffer.h"
#include "bo2zm_native_output_sync.h"
#include "bo2zm_native_scene_sync.h"
#include "bo2zm_native_scene_precise_patch.h"
#include "bo2zm_native_gpu_marks.h"
#include "bo2zm_native_pass_profile.h"
#include "bo2zm_native_performance.h"
#include "bo2zm_native_pixel_probe.h"
#include "bo2zm_native_shader_probe.h"
#include "bo2zm_native_readback_stats.h"
#include "bo2zm_output_crosshairs.h"
#include "../bo2zm_render_formats.h"
#include "bo2zm_hitch_waits.h"
#include "bo2zm_native_deferred.h"
#include "bo2zm_native_shaders.h"
#include "bo2zm_reflection_demand.h"

#include "bo2zm_graphics_settings.h"
#include "../../bo2zm_cheats.h"
#include "../../bo2zm_preset.h"

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/graphics/registers.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

#include "bo2zm_native_draws.h"
#include "bo2zm_native_depth_resolve.h"
#if defined(__SWITCH__)
extern "C" void armDCacheFlush(void* address, size_t size);
#endif

#include <algorithm>
#include <array>
#include <chrono>

#if REX_PLATFORM_SWITCH

extern "C" void RexSwitchPerfCount(unsigned id);

extern "C" void RexSwitchPerfHitch(uint64_t start, uint64_t end);
#endif
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(bo2zm_native_scene_sync, false, "BO2",
                    "Native renderer: global image dependencies between scene passes, resolves and clears; "
                    "selected at startup (forced on Android)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_scene_sync_precise, true, "BO2",
                    "Per-hazard precise GPU dependencies; false = original synchronization")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_scene_sync_mask, 95, "BO2",
                     "Hazard mask: 1 color, 2 depth, 4 sampled, 8 transfer before pass, "
                     "16 pass before transfer, 32 vertex, 64 shader writes, "
                     "128 global barrier before transfer")
    .range(0, 255)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_INT32(bo2zm_native_scene_pass_mode, 0, "BO2",
                     "0 original, 1 incoming, 2 outgoing, 3 scoped, 4 scene only, 5 convert only, 6 off, "
                     "7 convert+scene outgoing, 8 convert+scene incoming, "
                     "9 convert+depth scene, 10 convert+color scene")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_scene_pass_dependencies, true, "BO2",
                    "A/B: external image dependencies on scene and convert render passes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_scene_transfer_barriers, true, "BO2",
                    "A/B: memory barriers before image transfers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_scene_output_read_barrier, true, "BO2",
                    "A/B: the output-input barrier before final sampling")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_transfer_dst_only, false, "BO2",
                    "Experimental: preserve full producer stage and access; narrow the consumer "
                    "of pre-transfer memory barriers to TRANSFER read/write only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(bo2zm_native_resolve_without_copy_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates copying and swapping every N seconds, to "
                     "compare captures of the same spot with the game paused");
REXCVAR_DEFINE_INT32(bo2zm_native_shadow_scale, 100, "BO2",
                     "Native renderer: draws 1600x1600 depth targets (shadow maps) at this percentage and "
                     "scales them up when resolving them. BO2's shadow target is 560x720, so this does not act in "
                     "BO2. The value is taken when the first map is created and does not change while running")
    .range(50, 100);


REXCVAR_DEFINE_BOOL(bo2zm_native_deferred_clears, false, "BO2",
                    "Native renderer: whole-image clears of render targets become loadOp = CLEAR of the next pass "
                    "that draws on them, instead of a separate clear plus a load (big saving on tile-based GPUs). Any "
                    "other use records the clear as before. false = always the separate clear")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_guest_clears_in_pass, true, "BO2",
                    "Native renderer: the game's Clear() draws are made inside the open pass or deferred to the next "
                    "pass on the image (loadOp = CLEAR or a clear right after opening) instead of in a pass of their "
                    "own. false = a separate pass per Clear(), using a separate render pass")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_volume_eager, true, "BO2",
                    "Native renderer: rebuild the 3D color-grading table right after the resolve that writes it (no "
                    "pass open) instead of at the first draw that samples it (which splits that pass). false = as "
                    "before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_INT32(bo2zm_native_scene_scale, 100, "BO2",
                     "Native renderer (experimental): extra 3D scene scale in percent (50-100), applied on top of "
                     "bo2zm_native_scene_resolution. The scene render targets (pitch between "
                     "bo2zm_native_scene_scale_min_pitch and _max_pitch) are drawn at this size. 100 = exactly the "
                     "chosen internal resolution. Restart to change")
    .range(50, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_STRING(bo2zm_native_scene_resolution, "880x720", "BO2",
                      "Native renderer: internal resolution of the 3D scene. 880x720 = original, 1280x720 = HD, "
                      "1920x1080 = Full HD. Changes the scene's render targets, depth buffers and resolved textures "
                      "(not the output or the HUD). Restart to change")
    .allowed({"880x720", "1280x720", "1920x1080"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_STRING(bo2zm_native_output_resolution, "1280x720", "BO2",
                      "Native renderer: resolution of the game's composite and front buffer (HUD, menus and final "
                      "image). 1280x720 = original, 1920x1080 = Full HD, auto = from the display. Restart to change")
    .allowed({"1280x720", "1920x1080", "auto"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_native_screen_mode, "original", "BO2",
                      "Native renderer: original = 16:9 with bars; stretch = whole screen, may deform; ultrawide = "
                      "the engine draws a wider field of view and HUD layout for the display's aspect (falls back to "
                      "16:9 if the engine does not confirm it). Restart to change")
    .allowed({"original", "stretch", "ultrawide"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_display_width, 0, "BO2",
                     "Display width in pixels given by the launcher (0 = unknown: the presenter's surface is used)")
    .range(0, 16384)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_display_height, 0, "BO2",
                     "Display height in pixels given by the launcher (0 = unknown)")
    .range(0, 16384)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_output_min_pitch, 1280, "BO2",
                     "Native renderer: smallest render target pitch bo2zm_native_output_resolution acts on (BO2's "
                     "composite: 1280)")
    .range(961, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_output_max_pitch, 1280, "BO2",
                     "Native renderer: largest render target pitch bo2zm_native_output_resolution acts on. Keep it "
                     "below 1600: depth targets of 1600 or more are shadow maps")
    .range(961, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_scene_scale_min_pitch, 800, "BO2",
                     "Native renderer: smallest render target pitch bo2zm_native_scene_scale acts on (BO2's scene: 880)")
    .range(64, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_scene_scale_max_pitch, 960, "BO2",
                     "Native renderer: largest render target pitch bo2zm_native_scene_scale and "
                     "bo2zm_native_scene_resolution act on. Keep it below 1280: the composite, the front buffer and "
                     "the HUD are drawn at 1280")
    .range(64, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DECLARE(bool, bo2zm_perf_prepass_fuse_rect_clears);
REXCVAR_DECLARE(bool, bo2zm_perf_fit_area_to_clear);


REXCVAR_DEFINE_BOOL(bo2zm_perf_edram_dead_stores, false, "BO2",
                    "Android performance: render pass results that are always cleared before anything "
                    "reads them are not written back to memory (storeOp = DONT_CARE), learned per surface and pass. "
                    "false = original behavior (always STORE)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


#if REX_PLATFORM_ANDROID
constexpr bool kPerfV055Default = true;
#else
constexpr bool kPerfV055Default = false;
#endif
REXCVAR_DEFINE_BOOL(bo2zm_perf_v055, kPerfV055Default, "BO2",
                    "Android performance: converted resolves do not load the destination they overwrite, a separate "
                    "full-aspect guest clear is its pass's loadOp = CLEAR, and the pass area is rounded to 16 rows "
                    "instead of 64. Same image. false = original attachment behavior")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_perf_readonly_depth, kPerfV055Default, "BO2",
                    "Android performance: passes that only test the depth close it with storeOp = NONE instead of "
                    "writing it back unchanged, where the device has VK_ATTACHMENT_STORE_OP_NONE. Learned per kind of "
                    "pass opening; a write reopens the pass with STORE first. Same image. false = original attachment behavior")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_perf_v046, false, "BO2",
                    "Android performance: turn on the grouped render optimizations (GPU passes, copies, recorder and "
                    "presentation). false = each one only by its own switch (original behavior by default)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_perf_drop_unread_fronts, false, "BO2",
                    "Android performance: a deferred front buffer copy that no draw samples is dropped (not copied) "
                    "when its render target is written again, after 600 Swaps each preceded by a fresh resolve. A "
                    "later read turns the lazy front buffer off for the session (MISMATCH in the log). false = original behavior")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_perf_lazy_resolves, false, "BO2",
                    "Android performance: a 1:1 color resolve whose last 600 copies were never sampled is deferred: "
                    "recorded before the first draw that samples it, dropped when its source is written again. A "
                    "later read of a dropped one turns this off (MISMATCH in the log). false = original behavior")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_perf_resolve_alias, false, "BO2",
                    "Android performance: a resolve that repeats the previous one (same target, rectangle, size and "
                    "format, nothing drawn or cleared since) is served from the first texture's image instead of "
                    "copied; materialized if the first one changes while it is live. false = original behavior")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_perf_ab_s, 0, "BO2",
                     "Android performance: every N seconds switch the enabled render optimizations "
                     "(front drop, lazy resolves, alias) and present_wait_paint_unlocked OFF/ON in the same match, and "
                     "log both arms' swaps/s and GPU ms per swap ([perf-ab] lines). 0 = off. 30 is a good value")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_perf_pass_profile_serialize, false, "BO2",
                    "Android performance: in the sampled jobs of the GPU pass profile (1 in 64), a full "
                    "barrier after each profiled pass, so a pass's interval holds only its own work (Mali overlaps "
                    "the next passes' vertex work). Normal frames are untouched; not part of bo2zm_perf_v046")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_resolve_without_copy, true, "BO2",
                    "Native renderer (on by default): when resolving a whole target to a texture of the same "
                    "size and format, swaps the images "
                    "instead of copying the pixels. The image does not change; if the game draws to that target again "
                    "without clearing it first, the copy is restored");


REXCVAR_DEFINE_BOOL(bo2zm_native_swap_color, false, "BO2",
                    "Native renderer: when resolving the whole color target to a texture of the same size, swap "
                    "the images instead of copying the pixels. The image does not change; if the game draws on top "
                    "without clearing, it is restored");

REXCVAR_DEFINE_BOOL(bo2zm_native_swap_without_clear, true, "BO2",
                    "Native renderer: swap the image also when the resolve command does not clear the target. "
                    "It takes out the copy of a shadow map resolved a second time without a clear. If the game "
                    "draws on top "
                    "without clearing, it is restored: worst case, the same as now. Watch 'restorations' in the log: it "
                    "has to stay at 0");


REXCVAR_DEFINE_BOOL(bo2zm_native_resolve_valid_content, true, "BO2",
                    "Native renderer: before resolving a depth target whose content went away in a "
                    "swap, bring it back. Fixes the flicker of the menu shadows (the scene sampled the previous frame's "
                    "map). false = the old behavior");


REXCVAR_DEFINE_BOOL(bo2zm_native_restore_useful_area, true, "BO2",
                    "Native renderer: when bringing back the content of a swapped target, copy only the "
                    "rows the game really resolves instead of the whole image. The targets are created with height = "
                    "max(720, pitch) (the scene is 880x880 to draw 880x720), and what is below the useful area is "
                    "never drawn or read");


REXCVAR_DEFINE_BOOL(bo2zm_native_restore_by_swap, false, "BO2",
                    "Native renderer (off by default): bring back the content of a swapped target by "
                    "swapping the images again instead of copying them. DO NOT TURN ON: it leaves texture[1] of the "
                    "shadow map on loan, the one the overlay samples, and it stops receiving shadows. Watch "
                    "'loaned reads' in the log: it has to be 0");


REXCVAR_DEFINE_BOOL(bo2zm_native_lazy_depth, true, "BO2",
                    "Native renderer: the depth the game resolves for the final composite is only "
                    "copied if a draw really samples it (without the blur, none does). The image does not change. "
                    "Checks itself. false = always copy, as before");


REXCVAR_DEFINE_BOOL(bo2zm_native_probe_host_import, false, "BO2",
                    "Test: once, after 3,000 frames, imports guest memory around a recent vertex range as GPU memory "
                    "(VK_EXT_external_memory_host), has the GPU copy 1 MB of it and compares that with the CPU's view "
                    "(\"host import probe\" lines in the log)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_lazy_front, true, "BO2",
                    "Native renderer: the Swap paints the front buffer from its render target (or a "
                    "retained image) instead of copying it to the texture first: one front buffer copy less per "
                    "frame. Same "
                    "image; with FXAA it is copied as always. Checks itself. false = always copy, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_front_partial_clear, true, "BO2",
                    "Native renderer: initialize a Swap-only front buffer source before its first resolved-color "
                    "composition pass. Keeps unwritten borders black; false retains the previous contents");
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_front, false, "BO2",
                    "Diagnostic: log front composition ownership and guard decisions during the one frame "
                    "selected by bo2zm_native_diag_frame_s, up to 512 records. Does not enable clearing");


REXCVAR_DEFINE_BOOL(bo2zm_native_shadow_min, true, "BO2",
                    "Native renderer: the base shadow map is no longer restored by a "
                    "full-size copy; the overlay is drawn onto the cleared target and the scene samples the minimum of "
                    "the two textures. Same image. Needs the library with tfetch2DShadowMin. Checks itself "
                    "(inactive in BO2: no shader samples SHADOWMAP_SAMPLER). false = copy as before");
REXCVAR_DEFINE_INT32(bo2zm_native_shadow_min_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 and the guard already applying, alternates "
                     "every N seconds between the minimum (even spans) and the usual copy (odd spans), to measure the "
                     "net gain on the console with 'C2: GPU per Swap'. 0 = no alternation");


REXCVAR_DEFINE_BOOL(bo2zm_native_skip_repeated_clears, true, "BO2",
                    "Native renderer: skip a clear when the target is already cleared to that same value "
                    "and nothing has been drawn since. Does not change a single pixel; 'clears skipped' in the C2 "
                    "report says how many are saved");


REXCVAR_DEFINE_BOOL(bo2zm_native_diag_clears, false, "BO2",
                    "Native renderer (diagnostic): per target, the cleared area versus the area used "
                    "until the next clear (passes, resolves, restorations and swaps). One line every 20 s ('C2 clears "
                    "per target'). Does not change the image");


REXCVAR_DEFINE_BOOL(bo2zm_native_clear_useful_area, false, "BO2",
                    "Native renderer: color clears only clear the rows really used; the rest is "
                    "cleared before anything uses it. Same image. Needs bo2zm_native_diag_clears. false = the whole "
                    "image, as before");


REXCVAR_DECLARE(int32_t, bo2zm_native_per_draw_stats_s);

REXCVAR_DECLARE(bool, bo2zm_native_cheap_pcf);
REXCVAR_DEFINE_BOOL(bo2zm_native_pipeline_stats, false, "BO2",
                    "Native renderer: counts shaded fragments, vertex invocations and clipped "
                    "primitives per pass type (C2 report). The image does not change, but the queries cost GPU time: "
                    "for measuring only");
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_clear, false, "BO2",
                    "Native renderer: clear each render target with its own color instead of the game's color (tests "
                    "only: checks copy, clear and presentation)");
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_resolved, false, "BO2",
                    "Native renderer: present the resolved textures of each frame as a mosaic, in the order of their "
                    "copies (tests only)");


REXCVAR_DEFINE_INT32(bo2zm_native_diag_readers_s, 0, "BO2",
                     "Native renderer: every this many seconds two whole frames are looked at and, "
                     "for each resolved texture, it writes which pixel shaders read each write before the next one (C2 "
                     "readers lines); what comes out as SPARE is watched afterwards in every frame. Measures only. 0 = "
                     "off")
    .range(0, 3600);


REXCVAR_DEFINE_BOOL(bo2zm_native_lazy_composite, false, "BO2",
                    "Native renderer: the 1:1 copy of the composite scene to its texture (before "
                    "the HUD) is postponed: it is recorded if a draw samples it before its source is written "
                    "again (the raindrops) and dropped if not. Turns on after the bo2zm_native_diag_readers_s watch and "
                    "turns itself off at the first disagreement. false = always copied");
REXCVAR_DEFINE_INT32(bo2zm_native_read_resolved_texels, 4096, "BO2",
                     "Native renderer: resolved textures of up to this many texels are also copied to guest memory, "
                     "which the game reads for its exposure (0 = none; 4096 = up to 64x64)");
REXCVAR_DEFINE_BOOL(bo2zm_native_invalidate_textures_every_copy, false, "BO2",
                    "Native renderer: drop the texture caches on every copy (what was done before). Now "
                    "they are only dropped when a resolved texture is created, remade, prepared or changes its channel "
                    "order");


REXCVAR_DEFINE_INT32(bo2zm_native_work_slots, 3, "BO2",
                     "Native renderer: work slots (2 to 4). With more, the CPU runs more frames ahead of the GPU and "
                     "stalls less; each one costs 64 MB. 2 = the original behavior")
    .range(2, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_zcull, true, "BO2",
                    "Hierarchical depth culling (ZCULL): removes TRANSFER_DST from the game's depth targets so the "
                    "driver gives them a ZCULL plane");

REXCVAR_DEFINE_BOOL(bo2zm_native_output_without_wait, true, "BO2",
                    "Native renderer: paints the output of each Swap rotating 3 slots instead of one, so as not to wait "
                    "for the GPU to finish the previous output (the ring waited there almost one vsync per Swap). "
                    "Does not change the image; false goes back to the old behavior");


REXCVAR_DEFINE_BOOL(bo2zm_native_precise_marks, true, "BO2",
                    "Native renderer (measurement): GPU timestamps between categories with BOTTOM_OF_PIPE (released "
                    "when the previous work finishes) instead of TOP_OF_PIPE (when the command is read): exact split by "
                    "category. Does not change the image");
REXCVAR_DEFINE_BOOL(bo2zm_native_gpu_category_marks, true, "BO2",
                    "Native renderer: GPU timing marks between pass categories. false keeps only the same first "
                    "and last marks and resets two queries per job; no per-category breakdown. Android selects "
                    "this before launch for comparison without changing draws or image dependencies");
REXCVAR_DEFINE_BOOL(bo2zm_native_gpu_pass_profile, false, "BO2",
                    "Measure whole render passes in one of 64 in-game jobs, using existing timestamps")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_precise_marks_alternate_s, 0, "BO2",
                     "Native renderer (measurement): with N > 0 alternates normal marks (even spans) and precise marks "
                     "(odd spans) every N seconds and annotates each switch, to compare in the same run");
REXCVAR_DEFINE_INT32(bo2zm_native_reads_every, 1, "BO2",
                     "Native renderer: of each target, one in N small copies is read back and written to guest memory "
                     "(1 = all); the game uses them for its exposure");

REXCVAR_DEFINE_BOOL(bo2zm_show_fps, false, "BO2",
                    "Shows the frames per second at the top left (in the output pass with the gamma ramp; measured "
                    "every half second)");
REXCVAR_DEFINE_BOOL(bo2zm_native_gamma_ramp, true, "BO2",
                    "Native renderer: applies at the output the gamma ramp the game loads, like the Xbox 360's screen "
                    "(without it, the midtones and shadows come out darker). false: the image as it is")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


namespace bo2zm::guard30 {
void Heartbeat(double ms);
void Report();
}

namespace bo2zm::native {
namespace shaders {


#include "vulkan_spirv/guest_output_bilinear_ps.h"
#include "vulkan_spirv/guest_output_triangle_strip_rect_vs.h"


#include "shaders/bo2zm_output_gamma_ramp_ps.h"
#include "../shaders/bo2zm_resolve_copy_ps.h"
}


REXCVAR_DEFINE_BOOL(bo2zm_native_park_resolves, true, "BO2",
                    "Native renderer: when a resolve address changes size or format, keep the old image for the next "
                    "resolve of that shape instead of destroying it after a full GPU wait. false = destroy, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_float_targets, true, "BO2",
                    "Native renderer: 16_16_16_16_FLOAT render targets as real half-float images (experimental)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanPresenter;

constexpr VkFormat kFormatColor = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kHeightMaxTarget = 2048;


uint32_t HeightTarget(uint32_t pitch) {
  if (pitch <= 256) {
    return kHeightMaxTarget;
  }
  return std::min(kHeightMaxTarget, std::max<uint32_t>(720, (pitch + 15) & ~15u));
}


VkFormat FormatHostTarget(uint32_t format_storage) {


  static const bool floats = REXCVAR_GET(bo2zm_native_float_targets);
  if (!floats) {
    return kFormatColor;
  }
  switch (xenos::ColorRenderTargetFormat(format_storage)) {
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    default:
      return kFormatColor;
  }
}


VkFormat FormatHostResolved(uint32_t info_target) {
  const uint32_t format = (info_target >> 7) & 0x3F;
  const uint32_t number = (info_target >> 13) & 0x7;
  if (format == uint32_t(xenos::ColorFormat::k_16_16_16_16)) {
    return (number == 1 || number == 3) ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM;
  }
  if (format == uint32_t(xenos::ColorFormat::k_16_16_16_16_FLOAT)) {
    return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
  return kFormatColor;
}

bool Is16BitsPerChannel(VkFormat format) {
  return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R16G16B16A16_UNORM ||
         format == VK_FORMAT_R16G16B16A16_SNORM;
}
constexpr VkImageSubresourceRange kRangeColor = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceRange kRangeDepth = {
    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
using FnCopyImage = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                                        VkImageLayout, uint32_t, const VkImageCopy*);
using FnClearDepth = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout,
                                             const VkClearDepthStencilValue*, uint32_t,
                                             const VkImageSubresourceRange*);
using FnBlit = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                                uint32_t, const VkImageBlit*, VkFilter);


int32_t Fixed16p8(float value) {
  if (!(std::abs(value) >= 1.0f / 512.0f)) {
    return 0;
  }
  const double scaled = std::clamp(double(value) * 256.0, -2147483392.0, 2147483392.0);
  return int32_t(std::lround(scaled));
}


int32_t OffsetTiled2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
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


const std::array<uint16_t, 1024>& TableTiled2DTexel4() {
  static const std::array<uint16_t, 1024> table = [] {
    std::array<uint16_t, 1024> t{};
    for (int32_t y = 0; y < 32; ++y) {
      for (int32_t x = 0; x < 32; ++x) {
        t[size_t(y) * 32 + size_t(x)] = uint16_t(OffsetTiled2D(x, y, 32, 2));
      }
    }
    return t;
  }();
  return table;
}

int32_t ExtenderSign15(uint32_t value) {
  return int32_t(value << 17) >> 17;
}


VkClearColorValue ColorDiagnostic(uint32_t base, uint32_t format, uint32_t pitch) {
  uint32_t h = base * 2654435761u ^ pitch * 2246822519u ^ format * 3266489917u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  VkClearColorValue color{};
  for (uint32_t j = 0; j < 3; ++j) {
    color.float32[j] = float((h >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
  }
  color.float32[3] = 1.0f;
  return color;
}

using OwnedImage = NativeImage;

struct DepthTarget : OwnedImage {
  uint64_t draw_frame = UINT64_MAX;
  uint64_t draw_order = 0;
};

struct Resolved {
  OwnedImage image;
  uint32_t format_guest = 0;
  bool swap_rb = false;


  uint64_t reads = 0;

  bo2zm::native::v046::UnreadLearner perf_learn;
};


struct ReadTarget {
  uint32_t base = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t copies = 0;
  uint64_t skipped = 0;
};


struct TargetCopy {
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t copies = 0;
  uint64_t with_draws = 0;
  uint64_t draws = 0;
  uint64_t pixels = 0;
};


struct TargetState {
  bool clear_clean = false;
  uint64_t value_clear = 0;
  uint64_t draws_on_clear = 0;
  uint32_t height_used = 0;
};


struct DeferredClear {
  VkImage handle = VK_NULL_HANDLE;
  VkClearValue value{};
  bool depth = false;
  bool in_pass = false;


  bool rect = false;
  VkImageAspectFlags aspects = 0;
  VkRect2D area{};
};


struct Readback {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory guest_memory = VK_NULL_HANDLE;
  uint8_t* data = nullptr;
  VkDeviceSize bytes = 0;
  bool coherent = true;
};
struct PendingRead {
  Readback* read;
  uint32_t base;
  int32_t x0;
  int32_t y0;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t height_target;
  uint32_t info;
  VkFormat format_host = VK_FORMAT_R8G8B8A8_UNORM;
};


constexpr uint32_t kSlotsOutput = 3;

constexpr uint32_t kMarksPerSlot = bo2zm::native::timing::kMarksPerSlot;

constexpr uint32_t kOcclusionsPerSlot = 32;


constexpr uint32_t kVisibilityPerSlot = 16;
constexpr uint8_t kVisibilityWater = 0;
constexpr uint8_t kVisibilityWitness = 1;

constexpr uint32_t kStatsPerSlot = 64;
constexpr uint32_t kCountersStat = 3;

constexpr uint32_t kStatsDrawPerSlot = 2048;
constexpr uint32_t kLabelsShader = 512;

constexpr uint32_t kBucketsCopy = 4;
constexpr uint32_t kPixelsBucket[kBucketsCopy] = {64 * 64, 320 * 320, 1024 * 1024, 0xFFFFFFFFu};


struct WorkSlot {
  pass_profile::Job pass_profile;
  pixel_probe::Tag pixel_probe_tag;
  bool pixel_probe_mixed = false;
  uint32_t shader_pass_ordinal = 0;
  bool shader_target = false, shader_target_seen = false;
  uint32_t shader_mark_begin = UINT32_MAX, shader_mark_end = UINT32_MAX;
  VkCommandPool pool_job = VK_NULL_HANDLE;
  VkCommandPool pool_upload = VK_NULL_HANDLE;
  VkCommandBuffer job = VK_NULL_HANDLE;
  VkCommandBuffer upload = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
  uint64_t order = 0;


  std::chrono::steady_clock::time_point submitted{};
  std::vector<PendingRead> reads;
  std::vector<uint8_t> categories;
  bool marks_precise = false;
  bool marks_categories = true;


  std::vector<std::pair<uint32_t, uint64_t>> occlusions;

  std::vector<std::pair<uint32_t, uint8_t>> stats;

  std::vector<std::pair<uint32_t, uint16_t>> stats_draw;

  std::vector<std::pair<uint32_t, uint8_t>> visibility;
};


struct GameOcclusionQuery {
  uint32_t base = 0;
  uint64_t samples = 0;
  uint32_t spans_pending = 0;
  bool finished = false;
  bool failed = false;
};

class VulkanTargets final : public NativeTargets, public TargetContext {
 public:
  VulkanTargets(const VulkanDevice* vulkan_device, rex::memory::Memory* guest_memory)
      : vulkan_device_(vulkan_device),
        dfn_(deferred::Table(*vulkan_device)),
        device_(vulkan_device->device()),
        memory_(guest_memory),
        family_(vulkan_device->queue_family_graphics_compute()) {}

  ~VulkanTargets() override {
    WaitGpu();
    draws_.reset();
    for (auto& [key, image] : depths_) {
      Destroy(image);
    }
    for (auto& [key, image] : targets_) {
      Destroy(image);
    }
    for (auto& [key, resolved] : resolved_) {
      Destroy(resolved.image);
    }
    for (auto& [key, resolved] : parked_) {
      Destroy(resolved.image);
    }
    for (auto& [key, volume] : volumes_) {
      Destroy(volume.image);
    }
    Destroy(tiled_);
    for (FrontImage& spare : fronts_images_) {
      Destroy(spare.image);
    }
    reads_pending_.clear();
    for (WorkSlot& slot : slots_) {
      slot.reads.clear();
    }
    for (auto& [key, read] : reads_) {
      DestroyRead(read);
    }
    for (auto& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
      }
    }
    if (pipeline_ != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, pipeline_, nullptr);

    for (auto& [view, fb] : conv_framebuffers_) {
      if (fb != VK_NULL_HANDLE) dfn_.vkDestroyFramebuffer(device_, fb, nullptr);
    }
    for (auto& [format, p] : conv_pipelines_) {
      if (p != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, p, nullptr);
    }
    for (auto& [format, rp] : conv_passes_) {
      if (rp != VK_NULL_HANDLE) dfn_.vkDestroyRenderPass(device_, rp, nullptr);
    }
    if (conv_pool_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, conv_pool_, nullptr);
    if (conv_fs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, conv_fs_, nullptr);
    for (VkPipeline p : pipelines_ramp_) {
      if (p != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, p, nullptr);
    }
    if (fs_ramp_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, fs_ramp_, nullptr);

    for (OutputRamp& ramp : ramps_output_) {
      if (ramp.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, ramp.buffer, nullptr);
      if (ramp.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, ramp.guest_memory, nullptr);
    }
    if (layout_pipeline_ != VK_NULL_HANDLE)
      dfn_.vkDestroyPipelineLayout(device_, layout_pipeline_, nullptr);
    if (pool_descriptors_ != VK_NULL_HANDLE)
      dfn_.vkDestroyDescriptorPool(device_, pool_descriptors_, nullptr);
    if (layout_descriptors_ != VK_NULL_HANDLE)
      dfn_.vkDestroyDescriptorSetLayout(device_, layout_descriptors_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) dfn_.vkDestroySampler(device_, sampler_, nullptr);
    if (vs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, vs_, nullptr);
    if (fs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, fs_, nullptr);
    if (render_pass_output_ != VK_NULL_HANDLE)
      dfn_.vkDestroyRenderPass(device_, render_pass_output_, nullptr);
    for (WorkSlot& slot : slots_) {
      if (slot.fence != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, slot.fence, nullptr);
      if (slot.pool_job != VK_NULL_HANDLE)
        dfn_.vkDestroyCommandPool(device_, slot.pool_job, nullptr);
      if (slot.pool_upload != VK_NULL_HANDLE)
        dfn_.vkDestroyCommandPool(device_, slot.pool_upload, nullptr);
    }
    if (queries_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, queries_, nullptr);
    if (occlusions_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, occlusions_, nullptr);
    if (visibility_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, visibility_, nullptr);
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (fences_output_[i] != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, fences_output_[i], nullptr);
      if (pools_output_[i] != VK_NULL_HANDLE) dfn_.vkDestroyCommandPool(device_, pools_output_[i], nullptr);
    }
  }

  bool Init() {


    copy_image_ = reinterpret_cast<FnCopyImage>(deferred::Wrap(
        "vkCmdCopyImage",
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_, "vkCmdCopyImage")));
    if (!copy_image_) {
      REXLOG_ERROR("[native] C2: the driver does not provide vkCmdCopyImage");
      return false;
    }
    clear_depth_ = reinterpret_cast<FnClearDepth>(deferred::Wrap(
        "vkCmdClearDepthStencilImage",
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkCmdClearDepthStencilImage")));
    blit_ = reinterpret_cast<FnBlit>(deferred::Wrap(
        "vkCmdBlitImage",
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_, "vkCmdBlitImage")));

    const VkFormatFeatureFlags kUsesDepth =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;


    const VkFormatFeatureFlags kUsesScaled =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkFormatFeatureFlags uses_chosen = 0;
    for (const VkFormat candidate : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
      VkFormatProperties properties{};
      vulkan_device_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
          vulkan_device_->physical_device(), candidate, &properties);
      const VkFormatFeatureFlags uses = properties.optimalTilingFeatures & kUsesDepth;
      if (!(uses & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        continue;
      }
      if (format_depth_ == VK_FORMAT_UNDEFINED || uses == kUsesDepth) {
        format_depth_ = candidate;
        uses_chosen = uses;
        depth_scalable_ =
            (properties.optimalTilingFeatures & kUsesScaled) == kUsesScaled;
      }
      if (uses == kUsesDepth) {
        break;
      }
    }
    if (format_depth_ != VK_FORMAT_UNDEFINED) {
      const VkFormatFeatureFlags missing = kUsesDepth & ~uses_chosen;
      REXLOG_INFO("[native] C2: depth format {}{}{}{}",
                  format_depth_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
                  (missing & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? ", without sampling" : "",
                  (missing & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ? ", without copy source" : "",
                  (missing & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ? ", without copy target" : "");
      if (!depth_scalable_) {
        REXLOG_INFO("[native] C2: the driver does not scale depth: the shadow map stays at its size");
      }
    }


    VkCommandPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info_pool.queueFamilyIndex = family_;
    VkCommandBufferAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    reserve.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    reserve.commandBufferCount = 1;
    VkFenceCreateInfo info_fence{};
    info_fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (WorkSlot& slot : slots_) {
      if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &slot.pool_job) !=
              VK_SUCCESS ||
          dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &slot.pool_upload) !=
              VK_SUCCESS ||
          dfn_.vkCreateFence(device_, &info_fence, nullptr, &slot.fence) != VK_SUCCESS) {
        return false;
      }
      reserve.commandPool = slot.pool_job;
      if (dfn_.vkAllocateCommandBuffers(device_, &reserve, &slot.job) != VK_SUCCESS) {
        return false;
      }
      reserve.commandPool = slot.pool_upload;
      if (dfn_.vkAllocateCommandBuffers(device_, &reserve, &slot.upload) != VK_SUCCESS) {
        return false;
      }
    }
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &pools_output_[i]) != VK_SUCCESS) {
        return false;
      }
      reserve.commandPool = pools_output_[i];
      if (dfn_.vkAllocateCommandBuffers(device_, &reserve, &commands_output_[i]) != VK_SUCCESS) {
        return false;
      }
      if (dfn_.vkCreateFence(device_, &info_fence, nullptr, &fences_output_[i]) != VK_SUCCESS) {
        return false;
      }
    }
    slots_used_ = uint32_t(std::clamp(REXCVAR_GET(bo2zm_native_work_slots), 2,
                                          int32_t(slots_.size())));
    REXLOG_INFO("[native] C2: work slots (bo2zm_native_work_slots) = {} of {}", slots_used_,
                slots_.size());
    output_no_wait_ = REXCVAR_GET(bo2zm_native_output_without_wait);
    invalidate_every_copy_ = REXCVAR_GET(bo2zm_native_invalidate_textures_every_copy);
    REXLOG_INFO("[native] C2: texture caches dropped on every copy (bo2zm_native_invalidate_textures_every_copy) = {}",
                invalidate_every_copy_ ? "SI" : "no");
    REXLOG_INFO("[native] C2: output without waiting for the previous one (bo2zm_native_output_without_wait) = {}",
                output_no_wait_ ? "SI" : "no");
    REXLOG_INFO("[native] C2: scene image dependencies and ordered transfers (bo2zm_native_scene_sync) = {}",
                REXCVAR_GET(bo2zm_native_scene_sync) ? "on" : "off");
    REXLOG_INFO("[native] C2: precise GPU patch = {}, mask = {}",
                REXCVAR_GET(bo2zm_native_scene_sync_precise) ? "precise" : "global",
                uint32_t(REXCVAR_GET(bo2zm_native_scene_sync_mask)));
    REXLOG_INFO("[native] v0.34 pass dependencies mode={} (0 full, 1 incoming, 2 outgoing, 3 scoped, "
                "4 scene-only, 5 convert-only, 6 none, 7 scene-out+convert, "
                "8 scene-in+convert, 9 depth-scene+convert, 10 color-scene+convert)",
                REXCVAR_GET(bo2zm_native_scene_pass_mode));
    REXLOG_INFO("[native] v0.34 A/B: pass dependencies={}, transfer barriers={}, output read barrier={}",
                REXCVAR_GET(bo2zm_native_scene_pass_dependencies) ? "on" : "OFF",
                REXCVAR_GET(bo2zm_native_scene_transfer_barriers) ? "on" : "OFF",
                REXCVAR_GET(bo2zm_native_scene_output_read_barrier) ? "on" : "OFF");
    REXLOG_INFO("[native] v0.32: before-transfer barrier consumer scope = {}",
                REXCVAR_GET(bo2zm_native_transfer_dst_only) ? "TRANSFER read/write (experiment)"
                                                           : "ALL_COMMANDS memory read/write (original)");
    pass_profile::enabled.store(REXCVAR_GET(bo2zm_native_gpu_pass_profile), std::memory_order_relaxed);
    pass_profile::serialize.store(REXCVAR_GET(bo2zm_perf_pass_profile_serialize), std::memory_order_relaxed);
    {
      const bool all = REXCVAR_GET(bo2zm_perf_v046);
      perf_fuse_ = all || REXCVAR_GET(bo2zm_perf_prepass_fuse_rect_clears);
      perf_drop_fronts_ = all || REXCVAR_GET(bo2zm_perf_drop_unread_fronts);
      perf_lazy_ = all || REXCVAR_GET(bo2zm_perf_lazy_resolves);
      perf_alias_ = all || REXCVAR_GET(bo2zm_perf_resolve_alias);
      perf_base_drop_fronts_ = perf_drop_fronts_;
      perf_base_lazy_ = perf_lazy_;
      perf_base_alias_ = perf_alias_;
      perf_base_wait_unlocked_ = rex::cvar::GetFlagByName("present_wait_paint_unlocked") == "true";
      perf_base_fit_ = all || REXCVAR_GET(bo2zm_perf_fit_area_to_clear);

      perf_base_edram_ = REXCVAR_GET(bo2zm_perf_edram_dead_stores);
      bo2zm::native::edram::g_enabled.store(perf_base_edram_, std::memory_order_relaxed);
      bo2zm::native::v046::g_fit_enabled.store(perf_base_fit_, std::memory_order_relaxed);
      perf_ab_s_ = REXCVAR_GET(bo2zm_perf_ab_s);
      if (perf_ab_s_ > 0) {
        REXLOG_INFO("[perf-ab] in-session A/B every {} s: front drop {}, lazy resolves {}, alias {}, "
                    "present_wait_paint_unlocked {} alternate OFF/ON (first two windows not averaged)",
                    perf_ab_s_, perf_base_drop_fronts_, perf_base_lazy_, perf_base_alias_, perf_base_wait_unlocked_);
      }
      REXLOG_INFO("[perf] 0.46: all {} | first-pass aspect clears {} | drop unread fronts {} | lazy resolves {} | "
                  "resolve alias {} | fit area to clear {} | pass profile serialize {}",
                  all, perf_fuse_, perf_drop_fronts_, perf_lazy_, perf_alias_,
                  all || REXCVAR_GET(bo2zm_perf_fit_area_to_clear), REXCVAR_GET(bo2zm_perf_pass_profile_serialize));
    }
    marks_precise_ = REXCVAR_GET(bo2zm_native_precise_marks);
    marks_categories_ = REXCVAR_GET(bo2zm_native_gpu_category_marks);
    REXLOG_INFO("[native] GPU timing: {} ({} timestamp queries reset per job)",
                marks_categories_ ? "categories" : "endpoints", timing::ResetCount(marks_categories_));
    alternate_marks_s_ = REXCVAR_GET(bo2zm_native_precise_marks_alternate_s);
    start_marks_ = std::chrono::steady_clock::now();
    REXLOG_INFO("[native] C2: precise GPU marks (bo2zm_native_precise_marks) = {}; alternate every {} s",
                marks_precise_ ? "SI" : "no", alternate_marks_s_);

    {
      const auto& ifn = vulkan_device_->vulkan_instance()->functions();
      uint32_t families = 0;
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &families,
                                                   nullptr);
      std::vector<VkQueueFamilyProperties> queues(families);
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &families,
                                                   queues.data());
      VkPhysicalDeviceProperties physical{};
      ifn.vkGetPhysicalDeviceProperties(vulkan_device_->physical_device(), &physical);
      write_mark_ = reinterpret_cast<FnWriteMark>(
          deferred::Wrap("vkCmdWriteTimestamp", ifn.vkGetDeviceProcAddr(device_, "vkCmdWriteTimestamp")));
      read_queries_ = reinterpret_cast<FnReadQueries>(
          ifn.vkGetDeviceProcAddr(device_, "vkGetQueryPoolResults"));
      if (family_ < families && queues[family_].timestampValidBits && write_mark_ &&
          read_queries_ && physical.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo info_queries{};
        info_queries.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info_queries.queryCount = uint32_t(slots_.size() * kMarksPerSlot);
        if (dfn_.vkCreateQueryPool(device_, &info_queries, nullptr, &queries_) != VK_SUCCESS) {
          queries_ = VK_NULL_HANDLE;
        }
        period_mark_ns_ = physical.limits.timestampPeriod;
      }
      REXLOG_INFO("[native] C2: GPU time per Swap {}",
                  queries_ != VK_NULL_HANDLE ? "measured with timestamps"
                                               : "not available (the queue has no timestamps)");

      VkQueryPoolCreateInfo info_occlusions{};
      info_occlusions.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      info_occlusions.queryType = VK_QUERY_TYPE_OCCLUSION;
      info_occlusions.queryCount = uint32_t(slots_.size() * kOcclusionsPerSlot);
      if (!read_queries_ ||
          dfn_.vkCreateQueryPool(device_, &info_occlusions, nullptr, &occlusions_) != VK_SUCCESS) {
        occlusions_ = VK_NULL_HANDLE;
      }
      occlusion_precise_ = vulkan_device_->properties().occlusionQueryPrecise;
      REXLOG_INFO("[native] C2: host occlusion queries {} ({})",
                  occlusions_ != VK_NULL_HANDLE ? "available" : "not available: fake count",
                  occlusion_precise_ ? "precise" : "not precise: they count whether there was any sample");

      if (bo2zm::reflection_demand::MeasureVisibility() && read_queries_) {
        VkQueryPoolCreateInfo info_visibility{};
        info_visibility.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_visibility.queryType = VK_QUERY_TYPE_OCCLUSION;
        info_visibility.queryCount = uint32_t(slots_.size() * kVisibilityPerSlot);
        if (dfn_.vkCreateQueryPool(device_, &info_visibility, nullptr, &visibility_) != VK_SUCCESS) {
          visibility_ = VK_NULL_HANDLE;
        }
        REXLOG_INFO("[native] C2: reflection visibility: {}",
                    visibility_ != VK_NULL_HANDLE ? "queries available"
                                                   : "NO queries: the reflection is decided by readbacks (like 191)");
      }


      if (vulkan_device_->properties().pipelineStatisticsQuery && read_queries_) {
        VkQueryPoolCreateInfo info_stats{};
        info_stats.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_stats.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info_stats.queryCount = uint32_t(slots_.size() * kStatsPerSlot);
        info_stats.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (dfn_.vkCreateQueryPool(device_, &info_stats, nullptr, &stats_) != VK_SUCCESS) {
          stats_ = VK_NULL_HANDLE;
        }
      }
      if (stats_ != VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo info_draw{};
        info_draw.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_draw.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info_draw.queryCount = uint32_t(slots_.size() * kStatsDrawPerSlot);
        info_draw.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (dfn_.vkCreateQueryPool(device_, &info_draw, nullptr, &stats_draw_) != VK_SUCCESS) {
          stats_draw_ = VK_NULL_HANDLE;
        }
        fragments_per_shader_.assign(size_t(kLabelsShader) * kGpuCategories, 0);
        draws_per_shader_.assign(size_t(kLabelsShader) * kGpuCategories, 0);
      }
      REXLOG_INFO("[native] C2: pipeline statistics per pass {}",
                  stats_ != VK_NULL_HANDLE ? "available (bo2zm_native_pipeline_stats)"
                                                  : "not available");
    }

    VkSamplerCreateInfo info_sampler{};
    info_sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info_sampler.magFilter = VK_FILTER_LINEAR;
    info_sampler.minFilter = VK_FILTER_LINEAR;
    info_sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info_sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info_sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info_sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (dfn_.vkCreateSampler(device_, &info_sampler, nullptr, &sampler_) != VK_SUCCESS) {
      return false;
    }


    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].pImmutableSamplers = &sampler_;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_layout.bindingCount = 3;
    info_layout.pBindings = bindings;
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_layout, nullptr, &layout_descriptors_) !=
        VK_SUCCESS) {
      return false;
    }
    VkPushConstantRange ranges[2]{};
    ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ranges[0].offset = 0;
    ranges[0].size = 16;
    ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ranges[1].offset = 16;
    ranges[1].size = 96;


    VkPipelineLayoutCreateInfo info_pipeline_layout{};
    info_pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info_pipeline_layout.setLayoutCount = 1;
    info_pipeline_layout.pSetLayouts = &layout_descriptors_;
    info_pipeline_layout.pushConstantRangeCount = 2;
    info_pipeline_layout.pPushConstantRanges = ranges;
    if (dfn_.vkCreatePipelineLayout(device_, &info_pipeline_layout, nullptr, &layout_pipeline_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlotsOutput},
                                       {VK_DESCRIPTOR_TYPE_SAMPLER, kSlotsOutput},
                                       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlotsOutput}};
    VkDescriptorPoolCreateInfo info_pool_desc{};
    info_pool_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool_desc.maxSets = kSlotsOutput;
    info_pool_desc.poolSizeCount = 3;
    info_pool_desc.pPoolSizes = sizes;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool_desc, nullptr, &pool_descriptors_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo reserve_desc{};
    reserve_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve_desc.descriptorPool = pool_descriptors_;
    reserve_desc.descriptorSetCount = 1;
    reserve_desc.pSetLayouts = &layout_descriptors_;
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (dfn_.vkAllocateDescriptorSets(device_, &reserve_desc, &descriptors_output_[i]) != VK_SUCCESS) {
        return false;
      }
    }


    ramp_gamma_ = REXCVAR_GET(bo2zm_native_gamma_ramp);
    for (uint32_t i = 0; i < kSlotsOutput && ramp_gamma_; ++i) {
      OutputRamp& ramp = ramps_output_[i];
      uint32_t type = 0;
      void* mapped = nullptr;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device_, sizeof(ramp_values_) + sizeof(bo2zm::native::output::Crosshairs), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
              rex::ui::vulkan::util::MemoryPurpose::kUpload, ramp.buffer, ramp.guest_memory, &type) ||
          dfn_.vkMapMemory(device_, ramp.guest_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        REXLOG_WARN("[native] C2: could not create the gamma ramp buffer: the output goes without it");
        ramp_gamma_ = false;
        break;
      }
      ramp.data = static_cast<uint8_t*>(mapped);
      ramp.type = type;
      VkDescriptorBufferInfo info_buffer{ramp.buffer, 0, sizeof(ramp_values_) + sizeof(bo2zm::native::output::Crosshairs)};
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptors_output_[i];
      write.dstBinding = 2;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write.pBufferInfo = &info_buffer;
      dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    if (!ramp_gamma_) {

      for (OutputRamp& ramp : ramps_output_) {
        if (ramp.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, ramp.buffer, nullptr);
        if (ramp.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, ramp.guest_memory, nullptr);
        ramp = OutputRamp{};
      }
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = sizeof(shaders::guest_output_triangle_strip_rect_vs);
    module_info.pCode = shaders::guest_output_triangle_strip_rect_vs;
    if (dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &vs_) != VK_SUCCESS) {
      return false;
    }
    module_info.codeSize = sizeof(shaders::guest_output_bilinear_ps);
    module_info.pCode = shaders::guest_output_bilinear_ps;
    if (dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &fs_) != VK_SUCCESS) {
      return false;
    }
    if (ramp_gamma_) {
      module_info.codeSize = sizeof(shaders::bo2zm_output_gamma_ramp_ps);
      module_info.pCode = shaders::bo2zm_output_gamma_ramp_ps;
      if (dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &fs_ramp_) != VK_SUCCESS) {
        return false;
      }
    }

    VkAttachmentDescription attachment{};
    attachment.format = VulkanPresenter::kGuestOutputFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VulkanPresenter::kGuestOutputInternalLayout;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo info_rp{};
    info_rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info_rp.attachmentCount = 1;
    info_rp.pAttachments = &attachment;
    info_rp.subpassCount = 1;
    info_rp.pSubpasses = &subpass;
    const auto output_dependencies = bo2zm::native::output::ImageDependencies();
    info_rp.dependencyCount = uint32_t(output_dependencies.size());
    info_rp.pDependencies = output_dependencies.data();
    if (dfn_.vkCreateRenderPass(device_, &info_rp, nullptr, &render_pass_output_) != VK_SUCCESS) {
      return false;
    }

    pipeline_ = CreatePipelineOutput(fs_, nullptr);
    if (pipeline_ == VK_NULL_HANDLE) {
      return false;
    }


    if (ramp_gamma_ && (PipelineRamp(0) == VK_NULL_HANDLE || PipelineRamp(1) == VK_NULL_HANDLE)) {
      return false;
    }
    REXLOG_INFO("[native] C2: the game's gamma ramp at the output (bo2zm_native_gamma_ramp) = {}",
                ramp_gamma_ ? "SI" : "no");

    draws_ = VulkanDraws::Create(vulkan_device_, memory_, this);
    return true;
  }


  VkPipeline CreatePipelineOutput(VkShaderModule fs, const VkSpecializationInfo* special,
                                 VkRenderPass pass = VK_NULL_HANDLE,
                                 bool alpha_blend = false) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = special;
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1;
    view.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo sampling{};
    sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (alpha_blend) {
      blend_attachment.blendEnable = VK_TRUE;
      blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
      blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo info_pipeline{};
    info_pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info_pipeline.stageCount = 2;
    info_pipeline.pStages = stages;
    info_pipeline.pVertexInputState = &input;
    info_pipeline.pInputAssemblyState = &assembly;
    info_pipeline.pViewportState = &view;
    info_pipeline.pRasterizationState = &rasterization;
    info_pipeline.pMultisampleState = &sampling;
    info_pipeline.pColorBlendState = &blend;
    info_pipeline.pDynamicState = &dynamic;
    info_pipeline.layout = layout_pipeline_;
    info_pipeline.renderPass = pass != VK_NULL_HANDLE ? pass : render_pass_output_;
    info_pipeline.basePipelineIndex = -1;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (dfn_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info_pipeline, nullptr, &pipeline) !=
        VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return pipeline;
  }


  VkPipeline PipelineRamp(uint32_t index) {
    VkPipeline& pipeline = pipelines_ramp_[index];
    if (pipeline == VK_NULL_HANDLE && !pipelines_ramp_failed_[index]) {
      const VkSpecializationMapEntry inputs[3] = {{0, 0, sizeof(VkBool32)},
                                                    {1, sizeof(VkBool32), sizeof(VkBool32)},
                                                    {2, 2 * sizeof(VkBool32), sizeof(VkBool32)}};
      const VkBool32 values[3] = {(index & 1) ? VK_TRUE : VK_FALSE, (index & 2) ? VK_TRUE : VK_FALSE,
                                   (index & 4) ? VK_TRUE : VK_FALSE};
      const VkSpecializationInfo special{3, inputs, sizeof(values), values};
      const auto before = std::chrono::steady_clock::now();
      pipeline = CreatePipelineOutput(fs_ramp_, &special);
      if (pipeline == VK_NULL_HANDLE) {
        pipelines_ramp_failed_[index] = true;
        REXLOG_ERROR("[native] C2: could not create variant {} of the output pass", index);
      } else {
        REXLOG_INFO("[native] C2: variant {} of the output pass (exact {}, grading {}, FXAA {}) created in {:.1f} ms",
                    index, (index & 1) ? "si" : "no", (index & 2) ? "si" : "no", (index & 4) ? "si" : "no",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
      }
    }
    return pipeline;
  }


  bool FrontTraceLine() {
    if (!front_trace_active_) return false;
    if (front_trace_lines_ < 512) {
      ++front_trace_lines_;
      return true;
    }
    ++front_trace_suppressed_;
    return false;
  }

  void TraceFrontFrame(bool active) override {
    if (!active) {
      if (front_trace_active_) {
        REXLOG_INFO("[front-trace] end frame {}: records {}, suppressed {}, owners {}",
                    presented_, front_trace_lines_, front_trace_suppressed_, front_compositions_.size());
      }
      front_trace_active_ = false;
      return;
    }
    front_trace_active_ = REXCVAR_GET(bo2zm_native_diag_front);
    if (!front_trace_active_) return;
    front_trace_lines_ = front_trace_suppressed_ = 0;
    REXLOG_INFO("[front-trace] begin frame {}: clear enabled {}, failed {}, owners {}",
                presented_, REXCVAR_GET(bo2zm_native_front_partial_clear), front_partial_clear_failed_,
                front_compositions_.size());
    for (const auto& [image, front] : front_compositions_) {
      if (FrontTraceLine()) {
        REXLOG_INFO("[front-trace] snapshot target {:p} front {:08X} {}x{} write_frame {} frame {} "
                    "swap_only {} sampled {} image {}x{} format {}",
                    static_cast<const void*>(image), front.address, front.width, front.height,
                    front.write_frame, presented_, IsSwapFrontOnly(front.address),
                    front_composition_sampled_.count(front.address) != 0,
                    image->width, image->height, uint32_t(image->format));
      }
    }
  }

  bool Copy(const CopyRegisters& reg) override {


    struct VolumesOnExit {
      VulkanTargets* self;
      ~VolumesOnExit() { self->RefreshVolumesEager(); }
    } volumes_on_exit{this};


    const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
    const uint64_t draws_before = drawn - drawn_last_copy_;
    drawn_last_copy_ = drawn;
    if (draws_) {
      draws_->FinishPass();

      if (invalidate_every_copy_) {
        draws_->InvalidateTextures();
      }
    }
    if (Record()) {
      MarkGpu(kGpuCopies);
    }
    const uint32_t control = reg.rb_copy_control;
    const uint32_t source = control & 0x7;
    const bool clear_color = (control >> 8) & 0x1;
    const uint32_t command = (control >> 20) & 0x3;
    const bool do_copy = command == uint32_t(xenos::CopyCommand::kRaw) ||
                        command == uint32_t(xenos::CopyCommand::kConvert);
    const uint32_t pitch = reg.rb_surface_info & 0x3FFF;
    const uint32_t msaa = (reg.rb_surface_info >> 16) & 0x3;
    if (msaa != uint32_t(xenos::MsaaSamples::k1X) && warned_.insert(1).second) {
      REXLOG_INFO("[native] C2: target with MSAA: used with 1 sample");
    }
    if (source >= xenos::kMaxColorRenderTargets) {


      const bool would_clear = ((control >> 9) & 0x1) && clear_depth_;
      if (do_copy) {
        CopyDepth(reg, pitch, would_clear);
      }
      if (would_clear) {
        ClearDepth(reg, pitch);
      }
      return true;
    }
    const uint32_t info_color = reg.rb_color_info[source];
    const uint32_t format_color = (info_color >> 16) & 0xF;
    if (!bo2zm_formats::ColorApprox(format_color)) {
      return Reject(100 + format_color, "render target format not supported yet");
    }
    int32_t x0, y0, x1, y1;
    if (!Rectangle(reg, pitch, x0, y0, x1, y1)) {
      return false;
    }


    uint32_t pitch_source = pitch;
    int32_t ox0 = x0, oy0 = y0, ox1 = x1, oy1 = y1;
    const bool alias = reg.surface_real != 0;
    if (alias) {
      const auto in_x = [](uint32_t sup) { return ((sup >> 16) & 0x3) >= 2 ? 2 : 1; };
      const auto in_y = [](uint32_t sup) { return ((sup >> 16) & 0x3) >= 1 ? 2 : 1; };
      const uint32_t real = reg.surface_real;
      ox0 = x0 * in_x(reg.rb_surface_info) / in_x(real);
      ox1 = x1 * in_x(reg.rb_surface_info) / in_x(real);
      oy0 = y0 * in_y(reg.rb_surface_info) / in_y(real);
      oy1 = y1 * in_y(reg.rb_surface_info) / in_y(real);
      pitch_source = real & 0x3FFF;
      if (alias_seen_.size() < 16 && alias_seen_.insert((uint64_t(reg.rb_surface_info) << 32) | real).second) {
        REXLOG_INFO("[native] C2: copy through an alias: surface {:08X} [{},{})x[{},{}) read from the drawn one {:08X} "
                    "[{},{})x[{},{})",
                    reg.rb_surface_info, x0, x1, y0, y1, real, ox0, ox1, oy0, oy1);
      }
    }

    OwnedImage* target_render = GetTarget(info_color & 0xFFF, format_color, pitch_source);
    if (!target_render) {
      return false;
    }
    if (do_copy) {
      EdramRead(target_render->image, "a color resolve");
    }


    const bool scaled_source = IsScaled(*target_render);
    AnnotateAreaUseful(*target_render, HostY(*target_render, oy1));
    AnnotateUseClear(*target_render, uint32_t(std::max(HostX(*target_render, ox1), 0)),
                     uint32_t(std::max(HostY(*target_render, oy1), 0)));
    uint32_t base_resolved = 0;
    Resolved* resolved = nullptr;
    uint32_t dx = 0, dy = 0;
    if (do_copy) {
      const uint32_t info_target = reg.rb_copy_dest_info;
      const uint32_t format_target = (info_target >> 7) & 0x3F;
      if ((info_target >> 3) & 0x1) {
        Reject(3, "copy to a 3D or array texture: not yet");
      } else if (!bo2zm_formats::CopyApprox(format_target)) {
        Reject(200 + format_target, "copy format not supported yet");
      } else {
        const uint32_t pitch_target = reg.rb_copy_dest_pitch & 0x3FFF;
        const uint32_t height_target = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;


        const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
        const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
        const uint32_t base =
            reg.rb_copy_dest_base +
            uint32_t(OffsetTiled2D(int32_t(base_x), int32_t(base_y), pitch_target,
                                             bo2zm_formats::Log2BytesCopy(format_target)));
        dx = uint32_t(x0) - base_x;
        dy = uint32_t(y0) - base_y;


        PerfBeforeDestination(base & 0x1FFFFFFF,
                              (uint64_t(uint32_t(x0)) << 48) ^ (uint64_t(uint32_t(y0)) << 32) ^
                                  (uint64_t(uint32_t(x1)) << 16) ^ uint64_t(uint32_t(y1)) ^
                                  (uint64_t(info_target) << 20) ^ (uint64_t(reg.rb_copy_dest_pitch) << 8) ^
                                  (uint64_t(info_color & 0xFFF) << 40));
        if (scaled_source) {


          const uint32_t width_stored =
              v040::StoredSide(pitch_target, target_render->width, target_render->width_guest);
          const uint32_t height_stored =
              v040::StoredSide(height_target, target_render->height, target_render->height_guest);
          resolved = GetResolved(base & 0x1FFFFFFF, width_stored, height_stored, format_target,
                                 (info_target >> 24) & 0x1, FormatHostResolved(info_target), pitch_target,
                                 height_target);
        } else {
          resolved = GetResolved(base & 0x1FFFFFFF, pitch_target, height_target, format_target,
                                 (info_target >> 24) & 0x1, FormatHostResolved(info_target));
        }
        base_resolved = base & 0x1FFFFFFF;
        if (resolved) {
          AnnotateCopy(base & 0x1FFFFFFF, pitch_target, height_target, draws_before);
          if (composite_has_ || composite_expired_ != 0) {

            CompositeBeforeWriteTexture(base & 0x1FFFFFFF,
                                        x0 == 0 && y0 == 0 &&
                                            uint32_t(HostX(resolved->image, x1) - HostX(resolved->image, x0)) >=
                                                resolved->image.width &&
                                            uint32_t(HostY(resolved->image, y1) - HostY(resolved->image, y0)) >=
                                                resolved->image.height);
          }
          if (diag_window_ || !diag_watched_.empty()) {
            DiagWrite(base & 0x1FFFFFFF, info_color & 0xFFF, pitch, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0),
                          resolved->image.width, resolved->image.height, draws_before, false);
          }
        }
        if (resolved && REXCVAR_GET(bo2zm_native_diag_resolved) &&
            resolved_frame_.size() < 64 &&
            std::find(resolved_frame_.begin(), resolved_frame_.end(),
                      base & 0x1FFFFFFF) == resolved_frame_.end()) {
          resolved_frame_.push_back(base & 0x1FFFFFFF);
        }
      }
    }


    if (scaled_source) {
      if (resolved && !IsScaled(resolved->image)) {

        display::g_err_scaled_resolve_rejected.fetch_add(1, std::memory_order_relaxed);
        Reject(31, "resolve of a scaled render target to a texture at the guest's size");
        resolved = nullptr;
      }
      const OwnedImage* const dest = resolved ? &resolved->image : nullptr;
      if (dest && target_render->scale_class == display::kClassScene && source == 0) {
        display::g_scene_texture.Set(dest->width, dest->height);
      }
      if (dest) {
        dx = uint32_t(HostX(*dest, int32_t(dx)));
        dy = uint32_t(HostY(*dest, int32_t(dy)));
      }
      if (alias) {
        if (dest) {
          x0 = HostX(*dest, x0);
          x1 = HostX(*dest, x1);
          y0 = HostY(*dest, y0);
          y1 = HostY(*dest, y1);
        }
      } else {
        x0 = HostX(*target_render, x0);
        x1 = HostX(*target_render, x1);
        y0 = HostY(*target_render, y0);
        y1 = HostY(*target_render, y1);
      }
      ox0 = HostX(*target_render, ox0);
      ox1 = HostX(*target_render, ox1);
      oy0 = HostY(*target_render, oy0);
      oy1 = HostY(*target_render, oy1);
      if (x1 <= x0 || y1 <= y0) {
        resolved = nullptr;
      }
    }
    if (const auto front = front_compositions_.find(target_render); front != front_compositions_.end() &&
        (!resolved || !IsSwapFrontOnly(base_resolved))) {
      if (FrontTraceLine()) {
        REXLOG_INFO("[front-trace] invalidate target {:p} reason intermediate front {:08X} "
                    "write_frame {} frame {} new_dest {:08X} resolved {}",
                    static_cast<const void*>(target_render), front->second.address, front->second.write_frame,
                    presented_, base_resolved, resolved != nullptr);
      }
      front_compositions_.erase(front);
    }
    if (FrontTraceLine()) {
      REXLOG_INFO("[front-trace] copy target {:p} edram {:03X} surface {:08X} source {} dest {:08X} "
                  "resolved {} alias {} source_rect [{},{})x[{},{}) dest_offset {},{} "
                  "target {}x{} format {} dest {}x{} format {} swap_only {} sampled {}",
                  static_cast<const void*>(target_render), info_color & 0xFFF, reg.rb_surface_info,
                  source, base_resolved, resolved != nullptr, alias, x0, x1, y0, y1, dx, dy,
                  target_render->width, target_render->height, uint32_t(target_render->format),
                  resolved ? resolved->image.width : 0, resolved ? resolved->image.height : 0,
                  resolved ? uint32_t(resolved->image.format) : 0, IsSwapFrontOnly(base_resolved),
                  front_composition_sampled_.count(base_resolved) != 0);
    }
    if (!Record()) {
      return false;
    }
    Prepare(*target_render);

    if (do_copy) {
      FlushDeferredClear(*target_render);
    }
    if (resolved && alias) {
      PerfSettleBeforeOtherWrite(base_resolved);
      if (FrontTraceLine()) REXLOG_INFO("[front-trace] route alias target {:p} dest {:08X}",
                                       static_cast<const void*>(target_render), base_resolved);


      Prepare(resolved->image);
      const uint32_t width = std::min(uint32_t(std::max(x1 - x0, 0)),
                                      resolved->image.width > dx ? resolved->image.width - dx : 0u);
      const uint32_t height = std::min(uint32_t(std::max(y1 - y0, 0)),
                                     resolved->image.height > dy ? resolved->image.height - dy : 0u);
      const int32_t sx1 = std::min(ox1, int32_t(target_render->width));
      const int32_t sy1 = std::min(oy1, int32_t(target_render->height));
      if (width && height && blit_ && target_render->format == kFormatColor &&
          resolved->image.format == kFormatColor && ox0 >= 0 && oy0 >= 0 && ox0 < sx1 && oy0 < sy1) {
        ResolveFrontEarlier(base_resolved, dx == 0 && dy == 0 && width == resolved->image.width &&
                                                   height == resolved->image.height);
        VkImageBlit scale{};
        scale.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        scale.srcOffsets[0] = {ox0, oy0, 0};
        scale.srcOffsets[1] = {sx1, sy1, 1};
        scale.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        scale.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
        scale.dstOffsets[1] = {int32_t(dx + width), int32_t(dy + height), 1};
        const bool reduce = uint32_t(sx1 - ox0) > width || uint32_t(sy1 - oy0) > height;
        BeforeImageTransfer(commands_job_);
        blit_(commands_job_, target_render->image, VK_IMAGE_LAYOUT_GENERAL, resolved->image.image,
              VK_IMAGE_LAYOUT_GENERAL, 1, &scale, reduce ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        ++copies_;
        AnnotateCopy(width, height);
        ResolvedWritten(base_resolved, uint64_t(sx1 - ox0) * uint64_t(sy1 - oy0));
        ReadResolved(reg, *resolved, x0, y0, dx, dy, width, height);
      } else if (alias_failed_++ < 8) {
        REXLOG_WARN("[native] C2: copy through an alias not done: {}x{} from [{},{})x[{},{}) of {}x{} format {} to "
                    "{}x{} format {}",
                    width, height, ox0, sx1, oy0, sy1, target_render->width, target_render->height,
                    uint32_t(target_render->format), resolved->image.width, resolved->image.height,
                    uint32_t(resolved->image.format));
      }
    } else if (resolved) {
      Prepare(resolved->image);

      const uint32_t requested_width =
          std::min(uint32_t(x1 - x0), target_render->width - uint32_t(x0));
      const uint32_t requested_height = std::min(uint32_t(y1 - y0), target_render->height - uint32_t(y0));
      const uint32_t fit_width = resolved->image.width > dx ? resolved->image.width - dx : 0;
      const uint32_t fit_height = resolved->image.height > dy ? resolved->image.height - dy : 0;
      const uint32_t width = std::min(requested_width, fit_width);
      const uint32_t height = std::min(requested_height, fit_height);


      const bool shrink = blit_ != nullptr && fit_width && fit_height &&
                           target_render->format == kFormatColor &&
                           resolved->image.format == kFormatColor &&
                           (requested_width * 4 >= fit_width * 5 || requested_height * 4 >= fit_height * 5);
      if (shrink && uint32_t(x0) < target_render->width && uint32_t(y0) < target_render->height) {
        PerfSettleBeforeOtherWrite(base_resolved);
        if (FrontTraceLine()) REXLOG_INFO("[front-trace] route shrink target {:p} dest {:08X}",
                                         static_cast<const void*>(target_render), base_resolved);


        ResolveFrontEarlier(base_resolved, dx == 0 && dy == 0 && fit_width == resolved->image.width &&
                                                   fit_height == resolved->image.height);
        VkImageBlit reduction{};
        reduction.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reduction.srcOffsets[0] = {x0, y0, 0};
        reduction.srcOffsets[1] = {x0 + int32_t(requested_width), y0 + int32_t(requested_height), 1};
        reduction.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reduction.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
        reduction.dstOffsets[1] = {int32_t(dx + fit_width), int32_t(dy + fit_height), 1};
        BeforeImageTransfer(commands_job_);
        blit_(commands_job_, target_render->image, VK_IMAGE_LAYOUT_GENERAL,
              resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &reduction, VK_FILTER_LINEAR);
        ++copies_;
        ++reductions_;
        if (reductions_ <= 4) {
          REXLOG_INFO("[resolution] the {}x{} scene is shrunk to {}x{} when resolving it (supersampling)",
                      requested_width, requested_height, fit_width, fit_height);
        }
        AnnotateCopy(fit_width, fit_height);


        ResolvedWritten(base_resolved, uint64_t(requested_width) * requested_height);
        ReadResolved(reg, *resolved, x0, y0, dx, dy, fit_width, fit_height);
      } else if (width && height && uint32_t(x0) < target_render->width &&
                 uint32_t(y0) < target_render->height && dx < resolved->image.width &&
                 dy < resolved->image.height) {


        const bool whole_color = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
                                  width == target_render->width && height == target_render->height;

        const uint32_t number_target = (reg.rb_copy_dest_info >> 13) & 0x7;
        const int32_t bias_target = int32_t(reg.rb_copy_dest_info << 10) >> 26;
        if (target_render->format != resolved->image.format || bias_target != 0 || number_target >= 2) {
          PerfSettleBeforeOtherWrite(base_resolved);
          if (FrontTraceLine()) REXLOG_INFO("[front-trace] route converted target {:p} dest {:08X} bias {} number {}",
                                           static_cast<const void*>(target_render), base_resolved, bias_target, number_target);
          if (CopyConverted(*target_render, resolved->image, x0, y0, dx, dy, width, height, reg.rb_copy_dest_info)) {
            ++copies_;
            ++copies_converted_;
            AnnotateCopy(width, height);
            ResolvedWritten(base_resolved, uint64_t(width) * height);
            ReadResolved(reg, *resolved, x0, y0, dx, dy, width, height);
          }

        } else {


        if (whole_color && clear_color && ResolveNoCopy() &&
            REXCVAR_GET(bo2zm_native_swap_color)) {
          PerfSettleBeforeOtherWrite(base_resolved);

          if (SwapWithResolved(*target_render, *resolved, reg.rb_copy_dest_base & 0x1FFFFFFF)) {
            if (FrontTraceLine()) REXLOG_INFO("[front-trace] route image-swap target {:p} dest {:08X}",
                                             static_cast<const void*>(target_render), base_resolved);
            ++swaps_color_;
            ReadResolved(reg, *resolved, x0, y0, dx, dy, width, height);
            return true;
          }
        }
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.srcOffset = {x0, y0, 0};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstOffset = {int32_t(dx), int32_t(dy), 0};
        copy.extent = {width, height, 1};
        if (FrontTraceLine()) {
          const auto old = front_compositions_.find(target_render);
          REXLOG_INFO("[front-trace] bit-copy target {:p} front {:08X} extent {}x{} primary {} "
                      "origin {} full_dest {} rgba8 {} old_write_frame {} frame {}",
                      static_cast<const void*>(target_render), base_resolved, width, height, source == 0,
                      x0 == 0 && y0 == 0 && dx == 0 && dy == 0,
                      width == resolved->image.width && height == resolved->image.height,
                      target_render->format == kFormatColor && resolved->image.format == kFormatColor,
                      old == front_compositions_.end() ? UINT64_MAX : old->second.write_frame, presented_);
        }
        if (source == 0 && IsSwapFrontOnly(base_resolved) && !front_composition_sampled_.count(base_resolved) &&
            x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
            width == resolved->image.width && height == resolved->image.height &&
            target_render->format == kFormatColor && resolved->image.format == kFormatColor) {

          front_compositions_[target_render] = {base_resolved, width, height, presented_};
          if (FrontTraceLine()) {
            REXLOG_INFO("[front-trace] learn target {:p} front {:08X} write_frame {}",
                        static_cast<const void*>(target_render), base_resolved, presented_);
          }
        }


        ResolveFrontEarlier(base_resolved,
                                dx == 0 && dy == 0 && width == resolved->image.width && height == resolved->image.height);
        PerfSettleBeforeBitWrite(base_resolved,
                                 dx == 0 && dy == 0 && width == resolved->image.width &&
                                     height == resolved->image.height);
        if (!PostponeCopyFront(base_resolved, *target_render, *resolved, copy) &&
            !PostponeComposite(base_resolved, *target_render, *resolved, copy,
                              (info_color & 0xFFF) | (pitch_source << 12)) &&
            !PerfSkipCopy(base_resolved, *target_render, *resolved, copy)) {
          BeforeImageTransfer(commands_job_);
          copy_image_(commands_job_, target_render->image, VK_IMAGE_LAYOUT_GENERAL,
                         resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
          ++copies_;
          AnnotateCopy(width, height);
          ResolvedWritten(base_resolved, uint64_t(width) * height);
          ReadResolved(reg, *resolved, x0, y0, dx, dy, width, height);
          PerfNoteCopy(base_resolved, *target_render, *resolved, copy);
        }
        }
      }
    }
    if (clear_color) {
      const uint64_t value_raw =
          uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32);


      if (composite_has_ && composite_.source == ((info_color & 0xFFF) | (pitch_source << 12))) {
        CompositeBeforeWriteSource();
      }
      if (!diag_watched_.empty()) {
        DiagSourceWritten((info_color & 0xFFF) | (pitch_source << 12));
      }
      target_render->content_invalid = false;
      const uint64_t value_state =
          REXCVAR_GET(bo2zm_native_diag_clear) ? ~uint64_t(0) : value_raw;
      if (ClearRedundant(*target_render, value_state)) {
        ++clears_skipped_;
        bo2zm::native::performance::g_clears_skipped.fetch_add(1, std::memory_order_relaxed);
      } else {


        RotateFrontBeforeClear(*target_render);
        EdramClear(target_render->image);
        MarkGpu(kGpuClears);
        VkClearColorValue color{};
        if (REXCVAR_GET(bo2zm_native_diag_clear)) {
          color = ColorDiagnostic(info_color & 0xFFF, format_color, pitch_source);
        } else {
          for (uint32_t j = 0; j < 4; ++j) {
            color.float32[j] = float((value_raw >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
          }
        }

        if (!ClearColorAreaUseful(*target_render, color)) {

          VkClearValue deferred{};
          deferred.color = color;
          if (!DeferClear(*target_render, deferred, false, false)) {
            BeforeImageTransfer(commands_job_);
            dfn_.vkCmdClearColorImage(commands_job_, target_render->image,
                                      VK_IMAGE_LAYOUT_GENERAL, &color, 1, &kRangeColor);
          }
          RemoveBand(*target_render);
        }
        ++clears_;
        AnnotateClearDiag(*target_render, info_color & 0xFFF, format_color, pitch_source, false, false);
      }
    }
    if (((control >> 9) & 0x1) && clear_depth_) {
      ClearDepth(reg, pitch);
    }
    return true;
  }

  void ClearDepth(const CopyRegisters& reg, uint32_t pitch) {
    const uint32_t info = reg.rb_depth_info;
    DepthTarget* depth = GetDepth(info & 0xFFF, (info >> 16) & 0x1, pitch);
    if (!depth || !Record()) {
      return;
    }
    EdramClear(depth->image);
    Prepare(*depth);
    BeforeWriteDepth(*depth);
    ShadowMinBeforeClear(*depth);
    depth->content_invalid = false;
    const VkClearDepthStencilValue value{float(reg.rb_depth_clear >> 8) / 16777215.0f,
                                         reg.rb_depth_clear & 0xFF};


    if (depth->accepts_copy_target &&
        ClearRedundant(*depth, uint64_t(reg.rb_depth_clear))) {
      ++clears_skipped_depth_;
      bo2zm::native::performance::g_clears_skipped_depth.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    MarkGpu(kGpuClears);
    ++clears_depth_;

    AnnotateClearDiag(*depth, info & 0xFFF, (info >> 16) & 0x1, pitch, true,
                      !depth->accepts_copy_target);

    VkClearValue deferred{};
    deferred.depthStencil = value;
    if (!depth->accepts_copy_target && draws_) {


      if (DeferClear(*depth, deferred, true, true)) {
        return;
      }


      if (!draws_->ClearDepthInPass(commands_job_, *depth, value.depth,
                                             value.stencil)) {
        if (++clears_in_pass_failed_ <= 8) {
          REXLOG_WARN("[native] C2: could NOT clear the {}x{} depth by opening a pass; it keeps the previous content "
                      "(error {})",
                      depth->width, depth->height, clears_in_pass_failed_);
        }
      }
      return;
    }
    if (DeferClear(*depth, deferred, true, false)) {
      return;
    }
    BeforeImageTransfer(commands_job_);
    clear_depth_(commands_job_, depth->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1,
                        &kRangeDepth);
  }


  bool ClearAsDraw(const DrawClear& b) override {
    const uint32_t pitch = b.rb_surface_info & 0x3FFF;
    if (!draws_ || !pitch || b.x1 <= b.x0 || b.y1 <= b.y0) {
      return false;
    }

    const auto area_in = [&b](const OwnedImage& image, VkRect2D& area) {
      int64_t x0 = b.x0, y0 = b.y0, x1 = b.x1, y1 = b.y1;
      if (image.width_guest && image.height_guest &&
          (image.width_guest != image.width || image.height_guest != image.height)) {
        x0 = x0 * image.width / image.width_guest;
        x1 = (x1 * image.width + image.width_guest - 1) / image.width_guest;
        y0 = y0 * image.height / image.height_guest;
        y1 = (y1 * image.height + image.height_guest - 1) / image.height_guest;
      }
      x0 = std::clamp<int64_t>(x0, 0, image.width);
      x1 = std::clamp<int64_t>(x1, 0, image.width);
      y0 = std::clamp<int64_t>(y0, 0, image.height);
      y1 = std::clamp<int64_t>(y1, 0, image.height);
      if (x0 >= x1 || y0 >= y1) {
        return false;
      }
      area.offset = {int32_t(x0), int32_t(y0)};
      area.extent = {uint32_t(x1 - x0), uint32_t(y1 - y0)};
      return true;
    };

    const auto color_cleared = [this](OwnedImage& image, const VkRect2D& area) {
      ForgetClear(image);
      if (auto front = front_compositions_.find(&image); front != front_compositions_.end()) {
        if (FrontTraceLine()) {
          REXLOG_INFO("[front-trace] guest-clear target {:p} front {:08X} write_frame {} -> {} "
                      "area {},{} {}x{}",
                      static_cast<const void*>(&image), front->second.address, front->second.write_frame,
                      presented_, area.offset.x, area.offset.y, area.extent.width, area.extent.height);
        }
        front->second.write_frame = presented_;
      }
      ++clears_;
    };


    const uint32_t depth_base = b.rb_depth_info & 0xFFF;
    const uint32_t depth_format = (b.rb_depth_info >> 16) & 0x1;
    const uint64_t depth_key = (uint64_t(depth_base) << 20) | (uint64_t(depth_format) << 16) | pitch;
    const uint32_t color_base = b.rb_color_info & 0xFFF;
    const uint32_t color_format = (b.rb_color_info >> 16) & 0xF;
    const uint64_t color_key =
        (uint64_t(color_base) << 20) | (uint64_t(bo2zm_formats::Storage(color_format)) << 16) | pitch;
    const bool has_depth = (b.depth || b.stencil) && depths_.count(depth_key) != 0;
    const bool has_color = b.color && bo2zm_formats::ColorApprox(color_format) && targets_.count(color_key) != 0;
    const VkImageAspectFlags depth_aspects =
        (b.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) | (b.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
    VkClearValue depth_value{};
    depth_value.depthStencil = {std::clamp(b.z, 0.0f, 1.0f), b.value_stencil & 0xFF};
    VkClearValue color_value{};
    for (uint32_t i = 0; i < 4; ++i) {
      color_value.color.float32[i] = b.rgba[i];
    }


    if (draws_->PassActive() && (has_depth || has_color) && GuestClearsInPassOn()) {
      DepthTarget* depth_image = has_depth ? &depths_.find(depth_key)->second : nullptr;
      OwnedImage* color_image = has_color ? &targets_.find(color_key)->second : nullptr;
      VkRect2D depth_area{}, color_area{};
      const auto eligible = [this](const OwnedImage& image, VkImageAspectFlags aspects, const VkRect2D& area) {
        return image.image != VK_NULL_HANDLE && image.prepared && !image.content_invalid && !HasDeferredWork(image) &&
               draws_->CanClearInActivePass(image, aspects, area);
      };
      const bool depth_ok = !depth_image || (area_in(*depth_image, depth_area) &&
                                             eligible(*depth_image, depth_aspects, depth_area));
      const bool color_ok = !color_image || (area_in(*color_image, color_area) &&
                                             eligible(*color_image, VK_IMAGE_ASPECT_COLOR_BIT, color_area));
      if (depth_ok && color_ok) {
        bool done = false;
        if (depth_image) {
          if (draws_->ClearInActivePass(*depth_image, depth_aspects, depth_value, depth_area)) {
            ForgetClear(*depth_image);
            ++clears_depth_;
            depth_image->draw_frame = presented_;
            depth_image->draw_order = ++depth_draw_order_;
            done = true;
          }
        }
        if (color_image) {
          if (draws_->ClearInActivePass(*color_image, VK_IMAGE_ASPECT_COLOR_BIT, color_value, color_area)) {
            color_cleared(*color_image, color_area);
            done = true;
          }
        }
        if (done) {
          return true;
        }

        if (++guest_clears_in_pass_failed_ <= 4) {
          REXLOG_ERROR("[native] C2: a guest Clear() could not be made inside the open pass after all; separate pass");
        }
      }
    }


    if (draws_->PassActive()) {
      bo2zm::native::performance::g_guest_clears_pass_closed.fetch_add(1, std::memory_order_relaxed);
    }
    draws_->FinishPass();
    if (!Record()) {
      return false;
    }
    bool done = false;


    if (has_depth) {
      OwnedImage* image = static_cast<OwnedImage*>(TargetDepth(depth_base, depth_format, pitch));
      if (image) {
        VkRect2D area{};
        const bool inside = area_in(*image, area);
        if (inside && DeferGuestClear(*image, true, depth_aspects, depth_value, area)) {
          ForgetClear(*image);
          ++clears_depth_;
          done = true;
        } else {
          FlushDeferredClear(*image);
          if (inside) {
            MarkGpu(kGpuClears);
            GuestClearBarrier();
            if (draws_->ClearAttachmentInPass(commands_job_, *image, depth_aspects, depth_value, area)) {
              GuestClearBarrier();
              ForgetClear(*image);
              ++clears_depth_;
              done = true;
              bo2zm::native::performance::g_guest_clears_separate.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      }
    }
    if (has_color) {
      OwnedImage* image = static_cast<OwnedImage*>(TargetColor(color_base, color_format, pitch));
      if (image) {
        VkRect2D area{};
        const bool inside = area_in(*image, area);
        if (inside && DeferGuestClear(*image, false, VK_IMAGE_ASPECT_COLOR_BIT, color_value, area)) {
          color_cleared(*image, area);
          done = true;
        } else {
          FlushDeferredClear(*image);
          if (inside) {
            MarkGpu(kGpuClears);
            GuestClearBarrier();
            if (draws_->ClearAttachmentInPass(commands_job_, *image, VK_IMAGE_ASPECT_COLOR_BIT, color_value, area)) {
              GuestClearBarrier();
              color_cleared(*image, area);
              done = true;
              bo2zm::native::performance::g_guest_clears_separate.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      }
    }
    return done;
  }


  void CopyDepth(const CopyRegisters& reg, uint32_t pitch, bool would_clear) {
    const uint32_t info_target = reg.rb_copy_dest_info;
    if ((info_target >> 3) & 0x1) {
      Reject(3, "copy to a 3D or array texture: not yet");
      return;
    }
    int32_t x0, y0, x1, y1;
    if (!copy_image_ || !Rectangle(reg, pitch, x0, y0, x1, y1)) {
      return;
    }
    const uint32_t info = reg.rb_depth_info;
    ++copies_depth_;
    DepthTarget* depth = GetDepth(info & 0xFFF, (info >> 16) & 0x1, pitch);
    if (!depth) {
      return;
    }
    EdramRead(depth->image, "a depth resolve");
    const uint32_t pitch_target = reg.rb_copy_dest_pitch & 0x3FFF;
    const uint32_t height_target = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
    const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
    const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
    const uint32_t base =
        reg.rb_copy_dest_base +
        uint32_t(OffsetTiled2D(int32_t(base_x), int32_t(base_y), pitch_target, 2));
    const uint32_t dx = uint32_t(x0) - base_x;
    const uint32_t dy = uint32_t(y0) - base_y;
    const uint32_t format_texture = ((info >> 16) & 0x1) ? 23 : 22;


    uint32_t width_resolved = pitch_target, height_resolved = height_target;

    if (IsScaled(*depth)) {
      width_resolved = v040::StoredSide(pitch_target, depth->width, depth->width_guest ? depth->width_guest
                                                                                        : depth->width);
      height_resolved = v040::StoredSide(height_target, depth->height, depth->height_guest ? depth->height_guest
                                                                                            : depth->height);
    }


    if (!pending_.empty() && pending_.count(base & 0x1FFFFFFF)) {
      RecordCopyPending(base & 0x1FFFFFFF);
      ++lazy_copied_write_;
    }


    const bool scene_scaled_depth = IsScaled(*depth) && pitch != kSideShadows;
    Resolved* resolved = GetResolved(base & 0x1FFFFFFF, width_resolved, height_resolved,
                                         format_texture, false, format_depth_,
                                         scene_scaled_depth ? pitch_target : 0u,
                                         scene_scaled_depth ? height_target : 0u);
    if (!resolved || !Record()) {
      return;
    }


    AnnotateCopy(base & 0x1FFFFFFF, width_resolved, height_resolved, 0);
    if (diag_window_ || !diag_watched_.empty()) {
      DiagWrite(base & 0x1FFFFFFF, info & 0xFFF, pitch, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0),
                    resolved->image.width, resolved->image.height, 0, true);
    }
    Prepare(*depth);
    Prepare(resolved->image);
    FlushDeferredClear(*depth);
    AnnotateAreaUseful(*depth, HostY(*depth, y1));

    AnnotateUseClear(*depth, uint32_t(std::max(HostX(*depth, x1), 0)), uint32_t(std::max(HostY(*depth, y1), 0)));


    const uint32_t width_guest = depth->width_guest ? depth->width_guest : depth->width;
    const uint32_t height_guest = depth->height_guest ? depth->height_guest : depth->height;
    const bool scaled = width_guest != depth->width || height_guest != depth->height;


    if (!scaled && ((reg.rb_surface_info >> 16) & 3) == 0 && x0 >= 0 && y0 >= 0 &&
        uint32_t(x1) <= width_guest && dx < resolved->image.width && dy < resolved->image.height &&
        uint32_t(y1) > height_guest && depth->draw_frame == presented_) {
      std::array<DepthResolveBank, 64> banks{};
      std::array<OwnedImage*, 64> sources{};
      size_t count_banks = 0;
      for (auto& [key, image] : depths_) {
        if ((key & 0xFFFFF) != ((uint64_t((info >> 16) & 1) << 16) | pitch) ||
            !image.prepared || image.content_invalid || image.resolved_base || image.draw_frame != presented_ ||
            image.draw_order < depth->draw_order || image.width_guest || image.height_guest || image.width != pitch ||
            image.format != resolved->image.format) {
          continue;
        }
        if (count_banks == banks.size()) {
          count_banks = 0;
          break;
        }
        banks[count_banks] = {uint32_t(key >> 20), image.width, image.height};
        sources[count_banks++] = &image;
      }
      const uint32_t width_copy = std::min(uint32_t(x1 - x0), resolved->image.width - dx);
      const uint32_t height_copy = std::min(uint32_t(y1 - y0), resolved->image.height - dy);
      const auto plan = PlanDepthResolve(info & 0xFFF, pitch, uint32_t(y0), height_copy,
                                        std::span(banks.data(), count_banks));
      if (width_copy && plan.count > 1) {
        for (size_t i = 0; i < plan.count; ++i) {
          const auto& region = plan.regions[i];
          auto& source = *sources[region.bank];
          FlushDeferredClear(source);
          VkImageCopy copy{};
          copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
          copy.srcOffset = {x0, int32_t(region.source_y), 0};
          copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
          copy.dstOffset = {int32_t(dx), int32_t(dy + region.destination_y), 0};
          copy.extent = {width_copy, region.height, 1};
          BeforeImageTransfer(commands_job_);
          copy_image_(commands_job_, source.image, VK_IMAGE_LAYOUT_GENERAL, resolved->image.image,
                      VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
          ++copies_;
          AnnotateCopy(width_copy, region.height);
          if (warnings_depth_banks_ < 4) {
            REXLOG_INFO("[native] depth resolve bank {:03X}: source rows {}-{} to destination rows {}-{}",
                        banks[region.bank].base, region.source_y, region.source_y + region.height,
                        dy + region.destination_y, dy + region.destination_y + region.height);
          }
        }
        ResolvedWritten(base & 0x1FFFFFFF, uint64_t(width_copy) * height_copy);
        if (warnings_depth_banks_++ < 4) {
          REXLOG_INFO("[native] depth resolve across {} EDRAM surfaces: {}x{} at {:08X}", plan.count,
                      width_copy, height_copy, base & 0x1FFFFFFF);
        }
        return;
      }
    }
    const uint32_t width = std::min({uint32_t(x1 - x0), width_guest - uint32_t(x0),
                                     resolved->image.width - dx});
    const uint32_t height = std::min({uint32_t(y1 - y0), height_guest - uint32_t(y0),
                                    resolved->image.height - dy});
    if (!width || !height || uint32_t(x0) >= width_guest ||
        uint32_t(y0) >= height_guest || dx >= resolved->image.width ||
        dy >= resolved->image.height) {
      return;
    }


    if (depth->content_invalid) {
      if (REXCVAR_GET(bo2zm_native_resolve_valid_content)) {
        RestoreContent(*depth, true);
      } else {
        ++resolve_content_old_;
      }
    }
    if (scaled) {


      const auto aX = [&](int32_t v) {
        return int32_t(int64_t(v) * depth->width / width_guest);
      };
      const auto aY = [&](int32_t v) {
        return int32_t(int64_t(v) * depth->height / height_guest);
      };
      const uint32_t width_img = std::min(
          {uint32_t(std::max(1, aX(x0 + int32_t(width)) - aX(x0))),
           depth->width - uint32_t(aX(x0)), resolved->image.width - uint32_t(aX(int32_t(dx)))});
      const uint32_t height_img = std::min(
          {uint32_t(std::max(1, aY(y0 + int32_t(height)) - aY(y0))),
           depth->height - uint32_t(aY(y0)), resolved->image.height - uint32_t(aY(int32_t(dy)))});
      if (!width_img || !height_img) {
        return;
      }
      VkImageCopy copy{};
      copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copy.srcOffset = {aX(x0), aY(y0), 0};
      copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copy.dstOffset = {aX(int32_t(dx)), aY(int32_t(dy)), 0};
      copy.extent = {width_img, height_img, 1};
      BeforeImageTransfer(commands_job_);
      copy_image_(commands_job_, depth->image, VK_IMAGE_LAYOUT_GENERAL,
                     resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
      ++copies_;
      ++shadows_uploads_;
      AnnotateCopy(width_img, height_img);
      ResolvedWritten(base & 0x1FFFFFFF, uint64_t(width_img) * height_img);
      return;
    }


    if (ResolveNoCopy()) {
      const bool whole = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
                          width == depth->width && height == depth->height;

      const bool can = whole && (would_clear || REXCVAR_GET(bo2zm_native_swap_without_clear));
      if (can) {
        if (SwapWithResolved(*depth, *resolved, base & 0x1FFFFFFF)) {
          if (!would_clear) {
            ++swaps_no_clear_;
          }
          ShadowMinAfterSwap(*depth, base & 0x1FFFFFFF, would_clear);
          return;
        }
        ++no_swap_[2];
      } else {


        ++no_swap_[whole ? 0 : 1];
        if (warnings_no_swap_ < 8) {
          ++warnings_no_swap_;
          REXLOG_INFO("[native] C2 without a swap: {}x{} of {}x{} at ({},{})->({},{}), clears {} (base {:03X})",
                      width, height, depth->width, depth->height, x0, y0, dx, dy, would_clear,
                      base & 0xFFF);
        }
      }
    }
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.srcOffset = {x0, y0, 0};
    copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.dstOffset = {int32_t(dx), int32_t(dy), 0};
    copy.extent = {width, height, 1};


    ShadowMinCopyFrom(*depth);
    if (PostponeCopyDepth(base & 0x1FFFFFFF, *depth, *resolved, copy)) {
      return;
    }
    BeforeImageTransfer(commands_job_);
    copy_image_(commands_job_, depth->image, VK_IMAGE_LAYOUT_GENERAL,
                   resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    ++copies_;
    AnnotateCopy(width, height);
    ResolvedWritten(base & 0x1FFFFFFF, uint64_t(width) * height);
  }

  void RampGamma(const std::array<std::array<uint16_t, 3>, 256>& ramp) override {
    ramp_game_ = ramp;
    RecomputeRamp();
  }


  void RecomputeRamp() {
    const bo2zm::settings::Postprocess& p = postprocess_;
    grading_ = p.active && (p.saturation != 1.0f || p.vibrance != 0.0f || p.vignette > 0.0f || p.lines > 0.0f);
    const float tint[3] = {p.tint_r, p.tint_g, p.tint_b};
    const float inverse_gamma = p.gamma > 0.0f ? 1.0f / p.gamma : 1.0f;
    for (uint32_t i = 0; i < 256; ++i) {
      for (uint32_t c = 0; c < 3; ++c) {
        float x = float(ramp_game_[i][c] & 0x3FF) / 1023.0f;
        if (p.active) {
          x += c == 0 ? p.temperature * 0.10f : c == 2 ? -p.temperature * 0.10f : 0.0f;
          x += p.brightness;
          x = (x - 0.5f) * p.contrast + 0.5f;
          x = x + (x * tint[c] - x) * p.tint;
          if (!grading_) {
            x = std::clamp(std::pow(std::max(x, 0.0001f), inverse_gamma), 0.0f, 1.0f);
          }
        }
        ramp_values_[i * 4 + c] = x;
      }
      ramp_values_[i * 4 + 3] = 1.0f;
    }
    const size_t extra = 256 * 4;
    ramp_values_[extra] = p.saturation;
    ramp_values_[extra + 1] = p.vibrance;
    ramp_values_[extra + 2] = inverse_gamma;
    ramp_values_[extra + 3] = 0.0f;
    ramp_values_[extra + 4] = p.vignette;
    ramp_values_[extra + 5] = p.lines;
    ramp_values_[extra + 6] = 0.0f;
    ramp_values_[extra + 7] = 0.0f;
    ++version_ramp_;
  }

  bool Present(rex::ui::Presenter* ui_presenter, const SwapTexture& swap, uint32_t width,
                 uint32_t height) override {
    const bool painted = PresentImpl(ui_presenter, swap, width, height);


    if (pixel_probe_frame_.trial || pixel_probe::Busy(pixel_probe::Read().state)) {
      const auto before = pixel_probe_frame_;
      const auto cpu = pixel_probe::CpuClocks();
      pixel_probe_frame_ = pixel_probe::Boundary(pixel_probe::NowNs(), cpu[0], cpu[1],
                                                before, pixel_probe_counts_, painted);
      if (recording_ && (before.trial != pixel_probe_frame_.trial ||
                         before.phase != pixel_probe_frame_.phase)) {
        slots_[slot_].pixel_probe_mixed = true;
      }
      if (before.trial != pixel_probe_frame_.trial || before.phase != pixel_probe_frame_.phase) {
        const auto snapshot = pixel_probe::Read();
        REXLOG_INFO("[pixel-probe] trial {} state {} phase {} mode {}; scene pitch 640-1279; "
                    "10 s warmup, ABBA 30 s/block, first 5 s/block excluded",
                    snapshot.trial, pixel_probe::Name(snapshot.state), snapshot.phase,
                    pixel_probe_frame_.clip ? (pixel_probe_frame_.kind==pixel_probe::Kind::kShader ? "flat_fragment" : "scissor_1x1") : "normal");
      }
    }
    pixel_probe_counts_ = {};
    return painted;
  }

  bool PresentImpl(rex::ui::Presenter* ui_presenter, const SwapTexture& swap, uint32_t width,
                   uint32_t height) {
    bo2zm::reflection_demand::AnnotateSwap();


    OpenWindowDiagnostic();
#if REX_PLATFORM_SWITCH
    RexSwitchPerfCount(0);
#endif

    {
      const auto now = std::chrono::steady_clock::now();
      if (last_swap_ != std::chrono::steady_clock::time_point{}) {
        const double ms = std::chrono::duration<double, std::milli>(now - last_swap_).count();
#if REX_PLATFORM_SWITCH


        if (ms > 45.0) {
          uint64_t end;
          asm volatile("mrs %0, cntpct_el0" : "=r"(end));
          const uint64_t run_length = uint64_t((ms + 33.3) * 19200.0);
          RexSwitchPerfHitch(end > run_length ? end - run_length : 0, end);
        }
#endif
        const uint32_t bucket = ms < 15.0    ? 0
                                : ms < 18.0  ? 1
                                : ms < 25.0  ? 2
                                : ms < 30.0  ? 3
                                : ms < 36.0  ? 4
                                : ms < 50.0  ? 5
                                : ms < 60.0  ? 6
                                : ms < 75.0  ? 7
                                : ms < 100.0 ? 8
                                : ms < 150.0 ? 9
                                             : 10;
        ++buckets_swap_[bucket];

        if (ms > worst_swap_ms_) {
          worst_swap_ms_ = ms;
        }
        bo2zm::guard30::Heartbeat(ms);


        if (ms > 60.0 && warnings_hitch_ < 1000) {
          ++warnings_hitch_;

#if defined(__ANDROID__)
          const double gpu_ms = double(gpu_ns_ - hitch_gpu_ns_) / 1e6;
#else
          const double gpu_ms = double(gpu_ns_ - hitch_gpu_ns_) / 1e6 * 1.627;
#endif

          BO2ZM_REPORT_RING(
              "[hitch] frame of {:.1f} ms (GPU {:.1f} real, record {:.1f}): {} copies, {} clears, {} swaps, {} "
              "restorations, {} waits for the GPU",
              ms, gpu_ms, double(ns_record_ - hitch_ns_record_) / 1e6, copies_ - hitch_copies_,
              clears_ - hitch_clears_, swaps_ - hitch_resolves_,
              restores_ - hitch_restores_, waits_gpu_ - hitch_waits_);


          namespace e = bo2zm::waits;
          const auto delta = [&](e::Kind t) {
            return double(e::g_ns[t].load(std::memory_order_relaxed) - hitch_waits_ns_[t]) / 1e6;
          };
          const auto times = [&](e::Kind t) {
            return e::g_times[t].load(std::memory_order_relaxed) - hitch_waits_times_[t];
          };
          BO2ZM_REPORT_RING(
              "[hitch] waits (ms): game: executor handoff {:.1f} ({}), preparer handoff {:.1f} ({}), room in the ring "
              "{:.1f} ({}), inside sub_826E8EE8 {:.1f} ({}; vtable {:08X}, caller {:08X}) | ring: no work {:.1f} ({}), "
              "WAIT_REG_MEM {:.1f} ({}), GPU fence {:.1f}, output {:.1f} | textures checked {:.1f} MB, postponed {}",
              delta(e::kHandoffExecutor), times(e::kHandoffExecutor), delta(e::kHandoffPreparer),
              times(e::kHandoffPreparer), delta(e::kRoomRing), times(e::kRoomRing),
              delta(e::kGameMid), times(e::kGameMid), e::g_vtable_mid.load(std::memory_order_relaxed),
              e::g_caller_mid.load(std::memory_order_relaxed), delta(e::kRingNoJob),
              times(e::kRingNoJob), delta(e::kRingRegMem), times(e::kRingRegMem),
              double(ns_waits_gpu_ - hitch_ns_waits_gpu_) / 1e6,
              double(ns_wait_output_ - hitch_ns_wait_output_) / 1e6,
              double(e::g_bytes_hash.load(std::memory_order_relaxed) - hitch_bytes_hash_) / 1048576.0,
              e::g_hashes_postponed.load(std::memory_order_relaxed) - hitch_hashes_postponed_);

          const auto diff_of = [](const std::atomic<uint64_t>& a, uint64_t before) {
            return a.load(std::memory_order_relaxed) - before;
          };


          BO2ZM_REPORT_RING("[hitch] ring: {} draws, working {:.1f} ms, textures {:.1f} ms ({} uploads, {:.1f} MB; {} "
                      "created; hashes {:.1f} + {:.1f} ms); waiting for the vertex copy thread {:.1f} ms ({} times), "
                      "helping it {:.1f} ms ({} copies); creating textures {:.1f} ms on the ring, {} bound on the "
                      "thread, waiting for it {:.1f} ms; hashes on the thread: {} textures in {:.1f} ms of the thread, "
                      "copies on the ring {:.1f} ms, {} done by the ring in {:.1f} ms, waiting for it {:.1f} ms",
                      diff_of(e::g_draws, hitch_draws_), double(diff_of(e::g_ns_ring_working, hitch_ns_ring_)) / 1e6,
                      double(diff_of(e::g_ns_textures, hitch_ns_textures_)) / 1e6,
                      diff_of(e::g_textures_uploads, hitch_textures_uploads_),
                      double(diff_of(e::g_bytes_uploaded, hitch_bytes_uploaded_)) / 1048576.0,
                      diff_of(e::g_textures_created, hitch_textures_created_),
                      double(diff_of(e::g_ns_hash_raw, hitch_ns_hash_raw_)) / 1e6,
                      double(diff_of(e::g_ns_hash_data, hitch_ns_hash_data_)) / 1e6,
                      double(diff_of(e::g_ns_waiting_copies, hitch_ns_waiting_copies_)) / 1e6,
                      diff_of(e::g_waits_copies, hitch_waits_copies_),
                      double(diff_of(e::g_ns_helping_copies, hitch_ns_helping_copies_)) / 1e6,
                      diff_of(e::g_copies_helped, hitch_copies_helped_),
                      double(diff_of(e::g_ns_create_textures, hitch_ns_create_textures_)) / 1e6,
                      diff_of(e::g_textures_bound_thread, hitch_textures_bound_thread_),
                      double(diff_of(e::g_ns_waiting_bindings, hitch_ns_waiting_bindings_)) / 1e6,

                      diff_of(e::g_textures_hash_thread, hitch_textures_hash_thread_),
                      double(diff_of(e::g_ns_hash_thread, hitch_ns_hash_thread_)) / 1e6,
                      double(diff_of(e::g_ns_hash_snapshot, hitch_ns_hash_snapshot_)) / 1e6,
                      diff_of(e::g_textures_hash_ring, hitch_textures_hash_ring_),
                      double(diff_of(e::g_ns_hash_ring, hitch_ns_hash_ring_)) / 1e6,
                      double(diff_of(e::g_ns_waiting_hashes, hitch_ns_waiting_hashes_)) / 1e6);


          BO2ZM_REPORT_RING("[hitch] game: the executor without commands {:.1f} ms ({}) | the preparer: filling the "
                               "list {:.1f} ms ({}), of which TreeCull {:.1f} ms ({}); outside the list {:.1f} ms ({})",
                               delta(e::kExecutorNoOrders), times(e::kExecutorNoOrders),
                               delta(e::kPreparerList), times(e::kPreparerList),
                               delta(e::kPreparerScenery), times(e::kPreparerScenery),
                               delta(e::kPreparerOut), times(e::kPreparerOut));
        }
        for (uint32_t t = 0; t < bo2zm::waits::kNumTypes; ++t) {
          hitch_waits_ns_[t] = bo2zm::waits::g_ns[t].load(std::memory_order_relaxed);
          hitch_waits_times_[t] = bo2zm::waits::g_times[t].load(std::memory_order_relaxed);
        }
        hitch_ns_waits_gpu_ = ns_waits_gpu_;
        hitch_draws_ = bo2zm::waits::g_draws.load(std::memory_order_relaxed);
        hitch_ns_ring_ = bo2zm::waits::g_ns_ring_working.load(std::memory_order_relaxed);
        hitch_ns_textures_ = bo2zm::waits::g_ns_textures.load(std::memory_order_relaxed);
        hitch_textures_uploads_ = bo2zm::waits::g_textures_uploads.load(std::memory_order_relaxed);
        hitch_bytes_uploaded_ = bo2zm::waits::g_bytes_uploaded.load(std::memory_order_relaxed);
        hitch_textures_created_ = bo2zm::waits::g_textures_created.load(std::memory_order_relaxed);
        hitch_ns_hash_raw_ = bo2zm::waits::g_ns_hash_raw.load(std::memory_order_relaxed);
        hitch_ns_hash_data_ = bo2zm::waits::g_ns_hash_data.load(std::memory_order_relaxed);
        hitch_bytes_hash_ = bo2zm::waits::g_bytes_hash.load(std::memory_order_relaxed);
        hitch_hashes_postponed_ = bo2zm::waits::g_hashes_postponed.load(std::memory_order_relaxed);
        hitch_ns_waiting_copies_ = bo2zm::waits::g_ns_waiting_copies.load(std::memory_order_relaxed);
        hitch_waits_copies_ = bo2zm::waits::g_waits_copies.load(std::memory_order_relaxed);
        hitch_ns_helping_copies_ = bo2zm::waits::g_ns_helping_copies.load(std::memory_order_relaxed);
        hitch_copies_helped_ = bo2zm::waits::g_copies_helped.load(std::memory_order_relaxed);

        hitch_ns_create_textures_ = bo2zm::waits::g_ns_create_textures.load(std::memory_order_relaxed);
        hitch_textures_bound_thread_ = bo2zm::waits::g_textures_bound_thread.load(std::memory_order_relaxed);
        hitch_ns_waiting_bindings_ = bo2zm::waits::g_ns_waiting_bindings.load(std::memory_order_relaxed);

        hitch_textures_hash_thread_ = bo2zm::waits::g_textures_hash_thread.load(std::memory_order_relaxed);
        hitch_ns_hash_thread_ = bo2zm::waits::g_ns_hash_thread.load(std::memory_order_relaxed);
        hitch_ns_hash_snapshot_ = bo2zm::waits::g_ns_hash_snapshot.load(std::memory_order_relaxed);
        hitch_textures_hash_ring_ = bo2zm::waits::g_textures_hash_ring.load(std::memory_order_relaxed);
        hitch_ns_hash_ring_ = bo2zm::waits::g_ns_hash_ring.load(std::memory_order_relaxed);
        hitch_ns_waiting_hashes_ = bo2zm::waits::g_ns_waiting_hashes.load(std::memory_order_relaxed);
        hitch_ns_wait_output_ = ns_wait_output_;
        hitch_gpu_ns_ = gpu_ns_;
        hitch_ns_record_ = ns_record_;
        hitch_copies_ = copies_;
        hitch_restores_ = restores_;
        hitch_waits_ = waits_gpu_;
        hitch_clears_ = clears_;
        hitch_resolves_ = swaps_;
      }
      last_swap_ = now;
    }
    const uint32_t base = (swap.dword[1] & 0xFFFFF000) & 0x1FFFFFFF;
    DiagReadersOnPresent(base);


    if (!deferred_clears_.empty() && REXCVAR_GET(bo2zm_native_lazy_front)) {
      if (draws_) {
        draws_->FinishPass();
      }
      FlushDeferredClears();
    }


    OwnedImage* const source_front = FrontOnPresent(base, width, height);
    const bool tiled =
        ui_presenter && REXCVAR_GET(bo2zm_native_diag_resolved) && ComposeTiled(base);

    if (!ui_presenter || !SubmitJob(false)) {
      return false;
    }
    if (tiled) {
      bool painted_tiled = false;
      ui_presenter->RefreshGuestOutput(
          1280, 720, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_context) {
            auto& context =
                static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_context);
            painted_tiled = PaintOutput(context, tiled_, 1280, 720);
            return painted_tiled;
          });
      if (painted_tiled) {
        ++presented_;
      }
      return painted_tiled;
    }
    auto it = resolved_.find(base);
    if (it == resolved_.end() || !it->second.image.prepared) {
      display::g_err_present_failed.fetch_add(1, std::memory_order_relaxed);
      Reject(4, "Swap without a resolved texture: the test color is painted");
      return false;
    }
    Resolved& resolved = it->second;


    uint32_t w = 0, h = 0;
    PresentSizeOf(resolved.image, width, height, w, h);


    display::LoadSettingsOnce();
    const auto screen = v042::Screen(display::g_screen.load(std::memory_order_relaxed));
    v042::Display surface = v042::Display{rex::ui::present_stats::surface_width.load(std::memory_order_relaxed),
                                          rex::ui::present_stats::surface_height.load(std::memory_order_relaxed)};
    if (!surface.Known()) surface = display::DisplayOf();
    const v042::PresentAspect aspect = v042::PresentAspectFor(screen, surface, display::UltrawideConfirmed());
    display::g_present_aspect_x.store(aspect.x, std::memory_order_relaxed);
    display::g_present_aspect_y.store(aspect.y, std::memory_order_relaxed);
    display::g_front_texture.Set(resolved.image.width, resolved.image.height);
    display::g_front_painted.Set(w, h);
    if (IsScaled(resolved.image) && width != 0 && resolved.image.width_guest != 0 &&
        resolved.image.width_guest < width) {

      display::g_err_front_size_mismatch.fetch_add(1, std::memory_order_relaxed);
    }
    bool painted = false;


    const auto before_refresh = std::chrono::steady_clock::now();
    auto input_call = before_refresh;
    auto output_call = before_refresh;
    bool call = false;
    ui_presenter->RefreshGuestOutput(
        w, h, aspect.x, aspect.y, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_context) {
          input_call = std::chrono::steady_clock::now();
          call = true;
          auto& context =
              static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_context);
          painted = PaintOutput(context, source_front ? *source_front : resolved.image, w, h,
                                 source_front != nullptr);
          output_call = std::chrono::steady_clock::now();
          return painted;
        });
    if (call) {
      ns_before_call_ += Ns(before_refresh, input_call);
      ns_in_call_ += Ns(input_call, output_call);
      ns_after_call_ += Ns(output_call, std::chrono::steady_clock::now());
      ++refreshes_;
    }
    if (painted) {
      ++presented_;
      PerfAbTick();
      display::g_swaps_painted.fetch_add(1, std::memory_order_relaxed);
    } else {
      display::g_err_present_failed.fetch_add(1, std::memory_order_relaxed);
    }
    if (!probe_done_ && presented_ >= 3000 && REXCVAR_GET(bo2zm_native_probe_host_import)) {
      probe_done_ = true;
      ProbeHostImport(kProbeBytes);
      ProbeHostImport(kProbeBytes * 16);
    }
    return painted;
  }


  static constexpr VkDeviceSize kProbeBytes = VkDeviceSize(1) << 20;
  bool probe_done_ = false;

  void ProbeHostImport(VkDeviceSize bytes) {
    const uint32_t vertex = g_probe_vertex_address.load(std::memory_order_relaxed);
    const uint32_t address = vertex & ~uint32_t(bytes - 1);
    if (vertex == 0 || uint64_t(address) + bytes > 0x20000000ull) {
      REXLOG_WARN("[native] host import probe ({} MB): no vertex address to try", bytes >> 20);
      return;
    }
    uint8_t* const host = memory_->TranslatePhysical(address);
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t sum = 0;
    for (VkDeviceSize i = 0; i < bytes; i += 4096) {
      sum += host[i];
    }
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    const auto properties_of = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
        ifn.vkGetDeviceProcAddr(device_, "vkGetMemoryHostPointerPropertiesEXT"));
    if (!vulkan_device_->extensions().ext_EXT_external_memory_host || !properties_of) {
      REXLOG_WARN("[native] host import probe: VK_EXT_external_memory_host is not available on this device");
      return;
    }
    VkMemoryHostPointerPropertiesEXT properties{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    const VkResult r_properties = properties_of(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                host, &properties);
    uint32_t type = 0;
    if (r_properties != VK_SUCCESS || !rex::bit_scan_forward(properties.memoryTypeBits, &type)) {
      REXLOG_WARN("[native] host import probe ({} MB at guest {:08X}, host {}): no memory type (VkResult {}, types "
                  "{:#x})",
                  bytes >> 20, address, static_cast<void*>(host), int(r_properties), properties.memoryTypeBits);
      return;
    }
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = host;
    VkMemoryAllocateInfo reserve{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    reserve.pNext = &import;
    reserve.allocationSize = bytes;
    reserve.memoryTypeIndex = type;
    VkDeviceMemory imported = VK_NULL_HANDLE;
    const auto t1 = std::chrono::steady_clock::now();
    const VkResult r_import = dfn_.vkAllocateMemory(device_, &reserve, nullptr, &imported);
    const auto t2 = std::chrono::steady_clock::now();
    const auto& types = vulkan_device_->memory_types();
    if (r_import != VK_SUCCESS) {
      REXLOG_WARN("[native] host import probe ({} MB at guest {:08X}): vkAllocateMemory with the host pointer FAILED "
                  "(VkResult {}) in {:.2f} ms; memory types {:#x}",
                  bytes >> 20, address, int(r_import), Ns(t1, t2) / 1e6, properties.memoryTypeBits);
      return;
    }
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.pNext = &external;
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer source = VK_NULL_HANDLE, target = VK_NULL_HANDLE;
    VkDeviceMemory target_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void* mapped = nullptr;
    uint32_t target_type = 0;
    const char* failed = nullptr;
    VkCommandPoolCreateInfo info_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info_pool.queueFamilyIndex = family_;
    VkCommandBufferAllocateInfo info_commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info_commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info_commands.commandBufferCount = 1;
    VkFenceCreateInfo info_fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (dfn_.vkCreateBuffer(device_, &info, nullptr, &source) != VK_SUCCESS ||
        dfn_.vkBindBufferMemory(device_, source, imported, 0) != VK_SUCCESS) {
      failed = "binding the imported memory to a buffer";
    } else if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
                   vulkan_device_, kProbeBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   rex::ui::vulkan::util::MemoryPurpose::kReadback, target, target_memory, &target_type) ||
               dfn_.vkMapMemory(device_, target_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      failed = "the readback buffer";
    } else if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &pool) != VK_SUCCESS ||
               (info_commands.commandPool = pool,
                dfn_.vkAllocateCommandBuffers(device_, &info_commands, &commands) != VK_SUCCESS) ||
               dfn_.vkCreateFence(device_, &info_fence, nullptr, &fence) != VK_SUCCESS) {
      failed = "the command buffer";
    }
    double ms_gpu = 0.0;
    bool match = false;
    size_t first_difference = 0;
    if (!failed) {
#if defined(__SWITCH__)
      armDCacheFlush(host, size_t(bytes));
#endif
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      dfn_.vkBeginCommandBuffer(commands, &begin);
      VkBufferCopy region{0, 0, kProbeBytes};
      dfn_.vkCmdCopyBuffer(commands, source, target, 1, &region);
      dfn_.vkEndCommandBuffer(commands);
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &commands;
      const auto t3 = std::chrono::steady_clock::now();
      VkResult r_submit;
      {
        const auto queue = vulkan_device_->AcquireQueue(family_, 0);
        r_submit = dfn_.vkQueueSubmit(queue.queue(), 1, &submit, fence);
      }
      const VkResult r_wait =
          r_submit == VK_SUCCESS ? dfn_.vkWaitForFences(device_, 1, &fence, VK_TRUE, 2'000'000'000ull) : r_submit;
      ms_gpu = Ns(t3, std::chrono::steady_clock::now()) / 1e6;
      if (r_wait != VK_SUCCESS) {
        failed = "the GPU copy (submit or wait)";
      } else {
        if (!((types.host_coherent >> target_type) & 1)) {
          VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
          range.memory = target_memory;
          range.size = VK_WHOLE_SIZE;
          dfn_.vkInvalidateMappedMemoryRanges(device_, 1, &range);
        }
        const uint8_t* read = static_cast<const uint8_t*>(mapped);
        match = std::memcmp(read, host, size_t(kProbeBytes)) == 0;
        while (!match && first_difference < kProbeBytes && read[first_difference] == host[first_difference]) {
          ++first_difference;
        }
      }
    }
    REXLOG_INFO("[native] host import probe ({} MB at guest {:08X}): imported in {:.2f} ms (memory type {}{}{}; pages "
                "touched in {:.2f} ms){}{}; GPU copy of 1 MB {}{:.2f} ms: {}",
                bytes >> 20, address, Ns(t1, t2) / 1e6, type,
                ((types.host_coherent >> type) & 1) ? ", coherent" : "",
                ((types.host_cached >> type) & 1) ? ", CPU-cached" : "", Ns(t0, t1) / 1e6,
                failed ? "; FAILED at " : "", failed ? failed : "", failed ? "not made, " : "in ", ms_gpu,
                failed ? "no comparison"
                       : match ? "the GPU read the same bytes as the CPU"
                               : fmt::format("DIFFERENT from byte {}", first_difference));
    if (fence != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, fence, nullptr);
    if (pool != VK_NULL_HANDLE) dfn_.vkDestroyCommandPool(device_, pool, nullptr);
    if (mapped) dfn_.vkUnmapMemory(device_, target_memory);
    if (target != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, target, nullptr);
    if (target_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, target_memory, nullptr);
    if (source != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, source, nullptr);
    dfn_.vkFreeMemory(device_, imported, nullptr);
    (void)sum;
  }


  bool CompositeApplicable(uint32_t address, uint32_t source) {
    if (composite_off_ || !REXCVAR_GET(bo2zm_native_lazy_composite)) {
      return false;
    }
    for (const DiagWatched& v : diag_watched_) {
      if (v.address != address || v.source != source) {
        continue;
      }
      if (v.late != 0 || v.writes < kCompositeALook) {
        return false;
      }
      if (composite_applying_ != address) {
        composite_applying_ = address;
        BO2ZM_REPORT_RING("[native] C2 lazy composite: {:08X} from {:03X}/{}: {} writes watched, {} read before their "
                             "source was written again and 0 after: APPLYING (the copy is postponed; it is recorded "
                             "before the first draw that reads it and dropped when its source is written)",
                             address, source & 0xFFF, source >> 12, v.writes, v.before);
      }
      return true;
    }
    return false;
  }


  bool PostponeComposite(uint32_t address, OwnedImage& target, const Resolved& resolved, const VkImageCopy& copy,
                        uint32_t source) {
    if (composite_has_ || REXCVAR_GET(bo2zm_native_diag_resolved) || REXCVAR_GET(bo2zm_native_swap_color)) {
      return false;
    }
    if (copy.srcOffset.x != 0 || copy.srcOffset.y != 0 || copy.dstOffset.x != 0 || copy.dstOffset.y != 0 ||
        copy.extent.width != resolved.image.width || copy.extent.height != resolved.image.height ||
        target.format != kFormatColor || resolved.image.format != kFormatColor) {
      return false;
    }

    const int32_t texels_read = REXCVAR_GET(bo2zm_native_read_resolved_texels);
    if (uint64_t(copy.extent.width) * copy.extent.height <= uint64_t(std::max<int32_t>(texels_read, 0))) {
      return false;
    }
    if (!CompositeApplicable(address, source)) {
      return false;
    }
    composite_.address = address;
    composite_.source = source;
    composite_.target = &target;
    composite_.source_vk = target.image;
    composite_.texture_vk = resolved.image.image;
    composite_.copy = copy;
    composite_has_ = true;
    ResolvedWritten(address);
    ++composite_postponed_;
    return true;
  }


  void RecordComposite() {
    if (!composite_has_) {
      return;
    }
    composite_has_ = false;
    const PendingComposite p = composite_;
    const auto r = resolved_.find(p.address);
    if (r == resolved_.end() || r->second.image.image != p.texture_vk || !p.target ||
        p.target->image != p.source_vk || !copy_image_ || !Record()) {
      composite_expired_ = p.address;
      CompositeMismatch(p.address, "the postponed copy can no longer be recorded (the source or texture image "
                                       "changed)");
      return;
    }
    if (draws_) {
      if (draws_->PassActive()) {
        bo2zm::native::performance::g_pass_splits_copy.fetch_add(1, std::memory_order_relaxed);
      }
      draws_->FinishPass();
    }
    MarkGpu(kGpuCopies);
    BeforeImageTransfer(commands_job_);
    EdramRead(p.source_vk, "a deferred composite copy");
    copy_image_(commands_job_, p.source_vk, VK_IMAGE_LAYOUT_GENERAL, p.texture_vk, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &p.copy);
    ++copies_;
    AnnotateCopy(p.copy.extent.width, p.copy.extent.height);
    ResolvedWritten(p.address, uint64_t(p.copy.extent.width) * p.copy.extent.height);
  }


  void CompositeBeforeWriteSource() {
    if (!composite_has_) {
      return;
    }
    if (composite_off_) {
      RecordComposite();
      ++composite_recorded_others_;
      return;
    }
    composite_has_ = false;
    composite_expired_ = composite_.address;
    composite_pixels_saved_ += uint64_t(composite_.copy.extent.width) * composite_.copy.extent.height;
    ++composite_dropped_source_;
  }


  void CompositeBeforeDraw(const DrawRequest& p) {
    if (!p.ps || !p.registers) {
      return;
    }
    const uint32_t* r = p.registers;
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_index) * 6;
      if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture) ||
          ((f[5] >> 9) & 0x3) != uint32_t(xenos::DataDimension::k2DOrStacked)) {
        continue;
      }
      const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
      if (composite_has_ && base == composite_.address) {
        RecordComposite();
        ++composite_recorded_read_;
      } else if (composite_expired_ != 0 && base == composite_expired_) {
        ++composite_reads_late_;
        CompositeMismatch(base, "a draw samples it after its copy was dropped (it sees the scene from before the "
                                  "composite)");
        composite_expired_ = 0;
      }
    }
    if (composite_has_ && DrawWritesIn(r, composite_.source)) {
      CompositeBeforeWriteSource();
    }
  }


  void CompositeBeforeWriteTexture(uint32_t address, bool whole) {
    if (composite_has_ && composite_.address == address) {
      if (whole) {
        composite_has_ = false;
        composite_pixels_saved_ += uint64_t(composite_.copy.extent.width) * composite_.copy.extent.height;
        ++composite_replaced_;
      } else {
        RecordComposite();
        ++composite_recorded_others_;
      }
    }
    if (whole && composite_expired_ == address) {
      composite_expired_ = 0;
    }
  }


  void CompositeOnDestroy(const OwnedImage& image) {
    if (!composite_has_ || image.image == VK_NULL_HANDLE) {
      return;
    }
    if (image.image == composite_.source_vk) {
      composite_has_ = false;
      composite_expired_ = composite_.address;
    } else if (image.image == composite_.texture_vk) {
      composite_has_ = false;
    }
  }

  void CompositeMismatch(uint32_t address, const char* reason) {
    ++composite_mismatches_;
    if (composite_off_) {
      return;
    }
    composite_off_ = true;
    REXLOG_ERROR("[native] C2 lazy composite: MISMATCH at {:08X}: {}. Off for the rest of the session: always copied, "
                 "as before",
                 address, reason);
  }


  void CompositeReport() {
    if (!composite_postponed_ && !composite_mismatches_) {
      return;
    }
    const uint64_t frames = presented_ - composite_presented_previous_;
    const double mp = double(composite_pixels_saved_ - composite_pixels_previous_) / 1e6;
    const double per_frame = frames ? mp / double(frames) : 0.0;
    BO2ZM_REPORT_RING("[native] C2 lazy composite: {} copies postponed since boot; {} recorded before a "
                         "draw that reads it (exact) and {} before another write; {} dropped when their source was "
                         "written and {} when another whole one covered it: {:.2f} Mpixels per frame not copied "
                         "(~{:.2f} ms real); late reads {}{}",
                         composite_postponed_, composite_recorded_read_, composite_recorded_others_,
                         composite_dropped_source_, composite_replaced_, per_frame, per_frame * 0.60,
                         composite_reads_late_,
                         composite_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the same)");
    composite_pixels_previous_ = composite_pixels_saved_;
    composite_presented_previous_ = presented_;
  }


  void DiagReadersOnPresent(uint32_t front) {
    if (diag_window_) {
      if (resolved_.count(front)) {
        DiagRead(front, kReaderSwap, 0, 0);
      }
      if (++diag_frame_ >= 2) {
        DiagReadersReport();
        diag_window_ = false;
        diag_writes_.clear();
      }
      return;
    }
    const int32_t every = REXCVAR_GET(bo2zm_native_diag_readers_s);
    if (every <= 0) {
      diag_watched_.clear();
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - diag_readers_last_ < std::chrono::seconds(every)) {
      return;
    }
    diag_readers_last_ = now;
    diag_window_ = true;
    diag_frame_ = 0;
    diag_writes_.clear();
  }


  void DiagWrite(uint32_t address, uint32_t source, uint32_t pitch, int32_t x0, int32_t y0,
                     uint32_t requested_width, uint32_t requested_height, uint32_t width, uint32_t height, uint64_t draws,
                     bool depth) {
    const uint32_t of = (source & 0xFFF) | (pitch << 12);
    for (DiagWatched& v : diag_watched_) {
      if (v.address == address) {
        v.pending = of == v.source;
        v.source_written = false;
        v.writes += v.pending ? 1 : 0;
      }
    }
    if (!diag_window_) {
      return;
    }
    std::vector<DiagWriteEntry>& list = diag_writes_[address];
    if (list.size() >= kDiagMaxWrites) {
      ++diag_writes_lost_;
      return;
    }
    DiagWriteEntry e;
    e.frame = int32_t(diag_frame_);
    e.order = 1;
    for (const DiagWriteEntry& other : list) {
      e.order += other.frame == e.frame ? 1 : 0;
    }
    e.source = of;
    e.requested_width = requested_width;
    e.requested_height = requested_height;
    e.width = width;
    e.height = height;
    e.draws = draws;
    e.depth = depth;
    e.whole = x0 == 0 && y0 == 0 && requested_width >= width && requested_height >= height;
    list.push_back(std::move(e));
  }


  void DiagRead(uint32_t address, uint32_t ps, uint32_t reg_index, uint32_t target) {
    std::vector<DiagWriteEntry>& list = diag_writes_[address];
    if (list.empty()) {
      list.emplace_back();
    }
    DiagWriteEntry& e = list.back();
    for (DiagReader& l : e.readers) {
      if (l.ps == ps && l.reg_index == reg_index && l.target == target) {
        ++l.times;
        return;
      }
    }
    if (e.readers.size() >= kDiagMaxReaders) {
      ++e.others;
      return;
    }
    e.readers.push_back(DiagReader{ps, reg_index, target, 1});
  }


  void DiagReadsDraw(const DrawRequest& p) {
    if (!p.ps || !p.registers) {
      return;
    }
    const uint32_t* r = p.registers;
    const uint32_t target = (r[kDiagRegColorInfo] & 0xFFF) | ((r[kDiagRegSurfaceInfo] & 0x3FFF) << 12);
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_index) * 6;
      if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
        continue;
      }
      const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
      const uint32_t dimension = (f[5] >> 9) & 0x3;
      if (dimension == uint32_t(xenos::DataDimension::k2DOrStacked)) {
        if (resolved_.count(base)) {
          DiagRead(base, p.ps->number, s.reg_index, target);
          WatchRead(base, p.ps->number);
        }
      } else if (dimension == uint32_t(xenos::DataDimension::kCube)) {


        const uint32_t height = ((f[2] >> 13) & 0x1FFF) + 1;
        const uint32_t pitch = std::max<uint32_t>(((f[0] >> 22) & 0x1FF) << 5, 1);
        const uint64_t stride =
            (uint64_t((pitch + 31) & ~uint32_t(31)) * 4 * ((height + 31) & ~uint32_t(31)) + 4095) & ~uint64_t(4095);
        for (uint32_t c = 0; c < 6; ++c) {
          const uint32_t face = uint32_t((uint64_t(base) + c * stride) & 0x1FFFFFFF);
          if (resolved_.count(face)) {
            DiagRead(face, p.ps->number, s.reg_index, target);
          }
        }
      }
    }
    DiagSourceWrittenPerDraw(r);
  }


  void DiagWatch(const DrawRequest& p) {
    if (!p.ps || !p.registers) {
      return;
    }
    const uint32_t* r = p.registers;
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_index) * 6;
      if ((f[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
          ((f[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked)) {
        WatchRead(((f[1] >> 12) << 12) & 0x1FFFFFFF, p.ps->number);
      }
    }
    DiagSourceWrittenPerDraw(r);
  }


  void WatchRead(uint32_t address, uint32_t ps) {
    for (DiagWatched& v : diag_watched_) {
      if (v.address != address || !v.pending) {
        continue;
      }
      if (!v.source_written) {
        if (v.before++ == 0) {
          v.ps_before = ps;
          BO2ZM_REPORT_RING("[native] C2 readers: {:08X}, written from {:03X}/{}, is read by PS n{} BEFORE "
                               "its source is written again (after {} watched writes): a postponed copy covers it",
                               address, v.source & 0xFFF, v.source >> 12, ps, v.writes);
        }
      } else if (v.late++ == 0) {
        v.ps_late = ps;
        BO2ZM_REPORT_RING("[native] C2 readers: {:08X}, written from {:03X}/{}, is read by PS n{} AFTER its "
                             "source is written (after {} watched writes): dropping that copy when the source is "
                             "written would NOT be exact",
                             address, v.source & 0xFFF, v.source >> 12, ps, v.writes);
      }
    }
  }


  void DiagSourceWritten(uint32_t target) {
    for (DiagWatched& v : diag_watched_) {
      if (v.pending && !v.source_written && v.source == target) {
        v.source_written = true;
      }
    }
  }


  static bool DrawWritesIn(const uint32_t* r, uint32_t target) {
    const uint32_t mask = r[kDiagRegColorMask];
    const uint32_t pitch = (r[kDiagRegSurfaceInfo] & 0x3FFF) << 12;
    for (uint32_t i = 0; i < 4; ++i) {
      if (((mask >> (4 * i)) & 0xF) != 0 && ((r[kDiagRegsColorInfo[i]] & 0xFFF) | pitch) == target) {
        return true;
      }
    }
    return false;
  }

  void DiagSourceWrittenPerDraw(const uint32_t* r) {
    for (DiagWatched& v : diag_watched_) {
      if (v.pending && !v.source_written && DrawWritesIn(r, v.source)) {
        v.source_written = true;
      }
    }
  }


  void DiagReadersReport() {
    ++diag_windows_;
    std::vector<uint32_t> addresses;
    addresses.reserve(diag_writes_.size());
    for (const auto& [address, list] : diag_writes_) {
      addresses.push_back(address);
    }
    std::sort(addresses.begin(), addresses.end());
    const uint64_t texels_cpu = uint64_t(std::max<int32_t>(REXCVAR_GET(bo2zm_native_read_resolved_texels), 0));
    std::string excess;
    uint32_t writes = 0, new_ones = 0, equal = 0;
    for (const uint32_t address : addresses) {
      const std::vector<DiagWriteEntry>& list = diag_writes_[address];
      const auto r = resolved_.find(address);
      const uint32_t width = r != resolved_.end() ? r->second.image.width : 0;
      const uint32_t height = r != resolved_.end() ? r->second.image.height : 0;


      const bool cpu = width && uint64_t(width) * height <= texels_cpu;
      std::string line;
      uint64_t signature = address;
      for (size_t i = 0; i < list.size(); ++i) {
        const DiagWriteEntry& e = list[i];
        if (e.frame < 0) {
          line += " [before]";
        } else {
          ++writes;
          const bool blit = e.requested_width * 4 >= e.width * 5 || e.requested_height * 4 >= e.height * 5;
          line += fmt::format(" [{}.{}] {} {}x{} from {:03X}/{}{} ({} draws before)", e.frame, e.order,
                               e.depth ? "depth" : (blit ? "blit" : "1:1"), e.requested_width,
                               e.requested_height, e.source & 0xFFF, e.source >> 12, e.whole ? "" : ", partial",
                               e.draws);
        }
        signature = (signature ^ (uint64_t(uint32_t(e.frame + 1)) << 48 | uint64_t(e.order) << 40 | e.source)) *
                0x100000001B3ull;
        if (e.readers.empty()) {
          const bool next = i + 1 < list.size();
          if (cpu) {
            line += " -> no draw (the CPU reads it)";
          } else if (e.frame >= 0 && next && list[i + 1].whole) {
            line += " -> NOBODY before the next one, which covers it whole: SPARE";
            excess += fmt::format(" {:08X} [{}.{}] from {:03X}/{}", address, e.frame, e.order,
                                  e.source & 0xFFF, e.source >> 12);
            signature ^= 0x5A5Au;
            AddWatched(address, e.source);
          } else {
            line += next ? " -> nobody before the next one (which does not cover it whole)"
                               : " -> nobody until the end of the window";
          }
        } else {
          line += " ->";
          for (const DiagReader& l : e.readers) {
            if (l.ps == kReaderSwap) {
              line += fmt::format(" Swap x{}", l.times);
            } else {
              line += fmt::format(" PS n{} s{} x{} at {:03X}/{}", l.ps, l.reg_index, l.times, l.target & 0xFFF,
                                   l.target >> 12);
            }
            signature = (signature ^ (uint64_t(l.ps) << 32 | uint64_t(l.reg_index) << 26 | (l.target & 0x3FFFFFF))) *
                    0x100000001B3ull;
          }
          if (e.others) {
            line += fmt::format(" (and {} more readers)", e.others);
          }
        }
        line += ";";
      }
      uint64_t& previous = diag_signatures_[address];
      if (previous == signature && diag_windows_ > 3) {
        ++equal;
        continue;
      }
      previous = signature;
      ++new_ones;
      BO2ZM_REPORT_RING("[native] C2 readers (window {}): {:08X} {}x{}:{}", diag_windows_, address,
                           width, height, line);
    }
    std::string watched;
    for (const DiagWatched& v : diag_watched_) {
      watched += fmt::format(" {:08X} from {:03X}/{}: {} writes; {} reads before the source is written again{} and {} "
                               "after{};",
                               v.address, v.source & 0xFFF, v.source >> 12, v.writes, v.before,
                               v.before ? fmt::format(" (the first, PS n{})", v.ps_before) : std::string(),
                               v.late,
                               v.late ? fmt::format(" (the first, PS n{}): dropping it would NOT be exact", v.ps_late)
                                         : std::string(v.writes ? " (dropping it when the source is written would be "
                                                                  "exact)"
                                                                    : ""));
    }
    BO2ZM_REPORT_RING("[native] C2 readers of the resolved textures (window {}, 2 frames every {} s): {} "
                         "addresses and {} writes; {} lines written and {} equal to the last one written (not "
                         "repeated){}; writes nobody reads before another that covers them whole:{} | watched in every "
                         "frame:{}",
                         diag_windows_, REXCVAR_GET(bo2zm_native_diag_readers_s), addresses.size(), writes,
                         new_ones, equal,
                         diag_writes_lost_
                             ? fmt::format(" ({} writes not recorded because of the cap of {} per address)",
                                           diag_writes_lost_, kDiagMaxWrites)
                             : std::string(),
                         excess.empty() ? std::string(" none") : excess,
                         watched.empty() ? std::string(" none") : watched);
    CompositeReport();
  }


  void AddWatched(uint32_t address, uint32_t source) {
    for (const DiagWatched& v : diag_watched_) {
      if (v.address == address && v.source == source) {
        return;
      }
    }
    if (diag_watched_.size() < kDiagMaxWatched) {
      DiagWatched v;
      v.address = address;
      v.source = source;
      diag_watched_.push_back(v);
    }
  }


  void OpenWindowDiagnostic() {
    const int32_t every = REXCVAR_GET(bo2zm_native_per_draw_stats_s);
    if (every <= 0) {
      window_diagnostic_ = false;
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (window_diagnostic_) {
      window_diagnostic_ = false;
      return;
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_window_).count() >= every) {
      last_window_ = now;
      window_diagnostic_ = true;
      window_read_ = false;
    }
  }

  void Stats(uint64_t& copies, uint64_t& clears, uint64_t& presented,
                    uint64_t& rejections) const override {
    copies = copies_;
    clears = clears_;
    presented = presented_;
    rejections = rejections_;
  }


  uint64_t CopiesDepth() const override { return copies_depth_; }
  uint64_t ClearsDepth() const override { return clears_depth_; }

  void TimeGpu(uint64_t& nanoseconds, uint64_t& jobs) const override {
    nanoseconds = gpu_ns_;
    jobs = gpu_jobs_;
  }

  uint64_t MarkGpuFinalNs() const override { return uint64_t(double(last_mark_gpu_) * period_mark_ns_); }
  uint64_t JobsMarksPrecise() const override { return gpu_jobs_precise_; }
  uint64_t JobsMarksCategories() const override { return gpu_jobs_categories_; }

  void WaitsGpu(uint64_t& times, uint64_t& nanoseconds) const override {
    times = waits_gpu_;
    nanoseconds = ns_waits_gpu_;
  }

  void DurationJobsGpu(uint64_t& times, uint64_t& nanoseconds) const override {
    times = jobs_gpu_;
    nanoseconds = ns_job_gpu_;
  }

  void CostRecord(uint64_t cost[6]) const override {
    cost[0] = recordings_;
    cost[1] = ns_record_;
    cost[2] = ns_reset_pools_;
    cost[3] = reads_written_;
    cost[4] = texels_written_;
    cost[5] = ns_write_reads_;
  }


  uint64_t waits_gpu_ = 0;
  uint64_t ns_waits_gpu_ = 0;
  uint64_t jobs_gpu_ = 0;
  uint64_t ns_job_gpu_ = 0;

  uint64_t recordings_ = 0;
  uint64_t ns_record_ = 0;
  uint64_t ns_reset_pools_ = 0;
  uint64_t reads_written_ = 0;
  uint64_t texels_written_ = 0;
  uint64_t ns_write_reads_ = 0;

  void TimeGpuPerCategory(
      std::array<uint64_t, kGpuCategories>& nanoseconds) const override {
    nanoseconds = gpu_categories_ns_;
  }

  void StatsPipeline(std::array<uint64_t, kGpuCategories>& fragments,
                            std::array<uint64_t, kGpuCategories>& vertices,
                            std::array<uint64_t, kGpuCategories>& primitives) const override {
    fragments = fragments_category_;
    vertices = vertices_category_;
    primitives = primitives_category_;
  }

  void CopiesPerSize(std::array<uint64_t, 4>& copies, std::array<uint64_t, 4>& pixels) const override {
    copies = copies_bucket_;
    pixels = pixels_bucket_;
  }

  void StatsPerShader(std::vector<uint64_t>& fragments, std::vector<uint64_t>& draws,
                             uint64_t& frames) const override {
    fragments = fragments_per_shader_;
    draws = draws_per_shader_;
    frames = frames_diagnostic_;
  }

  void CostPresent(uint64_t cost[12]) const override {
    const uint64_t values[12] = {waits_output_,   ns_wait_output_,  submits_job_,  ns_latch_job_,
                                  ns_submit_job_, submits_output_,     ns_latch_output_, ns_submit_output_,
                                  refreshes_,        ns_before_call_, ns_in_call_,     ns_after_call_};
    std::copy(std::begin(values), std::end(values), cost);
  }

  void IntervalsBetweenSwaps(std::array<uint64_t, kBucketsSwap>& buckets, uint64_t& overlaps,
                            double& worst_ms) const override {
    buckets = buckets_swap_;
    worst_ms = worst_swap_ms_;
    worst_swap_ms_ = 0.0;
    overlaps = overlaps_gpu_;
  }

  bool Draw(const DrawRequest& request) override {
    if (composite_has_ || composite_expired_ != 0) {
      CompositeBeforeDraw(request);
    }
    if (diag_window_) {
      DiagReadsDraw(request);
    } else if (!diag_watched_.empty()) {
      DiagWatch(request);
    }
    return draws_ && draws_->Draw(request);
  }

  DrawStats DrawsStats() const override {
    return draws_ ? draws_->Stats() : DrawStats{};
  }

  void WaitUploads() override {
    if (draws_) {
      draws_->WaitUploads();
    }
  }

  size_t CopiesPending() const override {
    return draws_ ? draws_->CopiesPending() : 0;
  }

  void Precompile() override {
    if (draws_) {
      draws_->Precompile();
    }
  }


  VkCommandBuffer CommandsJob() override {
    return Record() ? commands_job_ : VK_NULL_HANDLE;
  }

  uint32_t PixelProbeMode() const override {
    return !pixel_probe_frame_.trial ? 0 : pixel_probe_frame_.kind==pixel_probe::Kind::kShader ? 1 : pixel_probe_frame_.clip ? 2 : 1;
  }
  uint32_t ShaderProbeMode() const override {
    return pixel_probe_frame_.trial && pixel_probe_frame_.kind == pixel_probe::Kind::kShader &&
        slots_[slot_].shader_target ? (pixel_probe_frame_.clip ? 2u : 1u) : 0u;
  }
  void ShaderProbeResult(uint32_t reason, uint32_t elements, bool replaced,
                         bool fallback, uint64_t compile_ns, bool normal_miss = false) override {
    if (!ShaderProbeMode()) return;
    auto& c=pixel_probe_counts_;
    if(normal_miss){++c.normal_pipeline_misses;c.flat_compile_ns+=compile_ns;return;}
    ++c.target_draws;c.target_elements+=elements;
    if(reason<c.shader_reasons.size())++c.shader_reasons[reason];
    c.eligible_draws+=reason==uint32_t(shader_probe::Reason::kEligible);
    c.replaced_draws+=replaced;if(replaced)c.replaced_elements+=elements;
    c.flat_fallbacks+=fallback;c.flat_compile_ns+=compile_ns;
  }
  void PixelProbeDraw(uint32_t category, uint32_t vertices, bool clipped) override {
    if (!pixel_probe_frame_.trial) return;
    ++pixel_probe_counts_.draws;
    pixel_probe_counts_.triangles += vertices / 3;
    if (category == kGpuReflection) {
      ++pixel_probe_counts_.scene_draws;
      pixel_probe_counts_.scene_triangles += vertices / 3;
    }
    pixel_probe_counts_.clipped_draws += clipped;
  }


  void UploadBarrier() {
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands_upload_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  }

  VkCommandBuffer CommandsUpload() override {
    if (!Record()) {
      return VK_NULL_HANDLE;
    }
    if (!recording_upload_) {
      VkCommandBufferBeginInfo start{};
      start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (dfn_.vkBeginCommandBuffer(commands_upload_, &start) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
      }
      recording_upload_ = true;
      UploadBarrier();
    }
    return commands_upload_;
  }

  uint64_t GenerationCommands() const override { return generation_commands_; }

  NativeImage* TargetColor(uint32_t base, uint32_t format, uint32_t pitch) override {
    OwnedImage* image = GetTarget(base, format, pitch);
    if (!image || !Record()) {
      return nullptr;
    }
    Prepare(*image);
    BeforeWriteColor(*image);


    RestoreContent(*image);
    return image;
  }

  void BeginColorPass(NativeImage& image, const uint32_t* r, const ShaderInput* ps) override {
    const auto found = front_compositions_.find(&image);
    if (found == front_compositions_.end() || !r) {
      if (FrontTraceLine()) {
        REXLOG_INFO("[front-trace] pass target {:p} {}x{} format {} frame {} owner {} registers {} PS n{}",
                    static_cast<const void*>(&image), image.width, image.height, uint32_t(image.format),
                    presented_, found != front_compositions_.end(), r != nullptr, ps ? int(ps->number) : -1);
      }
      return;
    }
    FrontComposition& front = found->second;
    const bool already_written = front.write_frame == presented_;
    if (already_written && !front_trace_active_) return;
    namespace g = rex::graphics;
    FrontWriteRect rect{0, 0, float(front.width), float(front.height)};
    const uint32_t vte = r[g::XE_GPU_REG_PA_CL_VTE_CNTL];
    const bool clipped = !(r[g::XE_GPU_REG_PA_CL_CLIP_CNTL] & (1u << 16));


    if ((vte & 0xF) == 0xF && clipped) {
      const auto value = [](uint32_t bits) {
        float result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
      };
      const float sx = std::abs(value(r[g::XE_GPU_REG_PA_CL_VPORT_XSCALE]));
      const float sy = std::abs(value(r[g::XE_GPU_REG_PA_CL_VPORT_YSCALE]));
      float cx = value(r[g::XE_GPU_REG_PA_CL_VPORT_XOFFSET]);
      float cy = value(r[g::XE_GPU_REG_PA_CL_VPORT_YOFFSET]);
      const int32_t wx = ExtenderSign15(r[g::XE_GPU_REG_PA_SC_WINDOW_OFFSET] & 0x7FFF);
      const int32_t wy = ExtenderSign15((r[g::XE_GPU_REG_PA_SC_WINDOW_OFFSET] >> 16) & 0x7FFF);
      if (r[g::XE_GPU_REG_PA_SU_SC_MODE_CNTL] & (1u << 16)) {
        cx += float(wx);
        cy += float(wy);
      }

      if (std::isfinite(cx) && std::isfinite(cy) && std::isfinite(sx) && std::isfinite(sy) && sx > 0 && sy > 0) {
        rect.left = std::max(0.0f, cx - sx);
        rect.top = std::max(0.0f, cy - sy);
        rect.right = std::min(float(front.width), cx + sx);
        rect.bottom = std::min(float(front.height), cy + sy);
      }
    }
    bool resolved_input = false;
    if (ps) {
      for (const auto& sampler : ps->samplers) {
        if (sampler.reg_index >= 16) continue;
        const uint32_t* fetch = r + g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + sampler.reg_index * 6;
        if ((fetch[0] & 3) != uint32_t(xenos::FetchConstantType::kTexture)) continue;
        const auto input = resolved_.find(fetch[1] & 0x1FFFF000u);
        if (FrontTraceLine()) {
          REXLOG_INFO("[front-trace] input target {:p} PS n{} t{} address {:08X} resolved {} prepared {} "
                      "depth {} presented {}",
                      static_cast<const void*>(&image), ps->number, sampler.reg_index, fetch[1] & 0x1FFFF000u,
                      input != resolved_.end(), input != resolved_.end() && input->second.image.prepared,
                      input != resolved_.end() && IsDepthFormat(input->second.image.format),
                      input != resolved_.end() && fronts_presented_.count(input->first) != 0);
        }
        if (input != resolved_.end() && input->second.image.prepared &&
            !IsDepthFormat(input->second.image.format) && !fronts_presented_.count(input->first)) {
          resolved_input = true;
          break;
        }
      }
    }
    const uint32_t mask = r[g::XE_GPU_REG_RB_COLOR_MASK];
    const uint32_t blend = r[g::XE_GPU_REG_RB_BLENDCONTROL0];
    const uint32_t color_control = r[g::XE_GPU_REG_RB_COLORCONTROL];
    const bool opaque_rgba = FrontOpaqueRGBAWrite(mask, blend, color_control,
                                                 ps && (ps->outputs & 1), ps && ps->discards);
    const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
    const auto clear = state_target_.find(&image);
    const bool clear_clean = clear != state_target_.end() && clear->second.clear_clean;
    const uint64_t clear_draw = clear != state_target_.end() ? clear->second.draws_on_clear : 0;
    const bool known_clear = draws_ && FrontKnownClearValid(clear_clean, clear_draw, drawn);
    const bool eligible = !front_partial_clear_failed_ && REXCVAR_GET(bo2zm_native_front_partial_clear) &&
                          IsSwapFrontOnly(front.address) && !front_composition_sampled_.count(front.address) &&
                          image.format == kFormatColor &&
                          front.width <= image.width && front.height <= image.height;
    if (FrontTraceLine()) {
      const bool provenance = IsSwapFrontOnly(front.address) && !front_composition_sampled_.count(front.address) &&
                              image.format == kFormatColor && front.width <= image.width && front.height <= image.height;
      REXLOG_INFO("[front-trace] pass target {:p} front {:08X} write_frame {} frame {} "
                  "decision {} enabled {} failed {} swap_only {} sampled {} rgba8 {} fits {} input {} "
                  "opaque_rgba {} known_clear {} "
                  "viewport [{},{})x[{},{}) vte {:08X} clip {:08X} scissor {:08X}/{:08X} PS n{}",
                  static_cast<const void*>(&image), front.address, front.write_frame, presented_,
                  FrontDecisionName(front.Inspect(presented_, provenance, rect, resolved_input, opaque_rgba, known_clear)),
                  REXCVAR_GET(bo2zm_native_front_partial_clear), front_partial_clear_failed_,
                  IsSwapFrontOnly(front.address), front_composition_sampled_.count(front.address) != 0,
                  image.format == kFormatColor, front.width <= image.width && front.height <= image.height,
                  resolved_input, opaque_rgba, known_clear, rect.left, rect.right, rect.top, rect.bottom, vte,
                  r[g::XE_GPU_REG_PA_CL_CLIP_CNTL], r[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
                  r[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR], ps ? int(ps->number) : -1);
    }
    if (FrontTraceLine()) {
      REXLOG_INFO("[front-trace] write-state target {:p} mask {:08X} blend {:08X} colorctl {:08X} "
                  "outputs {:X} discard {} clear_clean {} clear_draw {} draw {} clear_value {:016X}",
                  static_cast<const void*>(&image), mask, blend, color_control, ps ? ps->outputs : 0,
                  ps && ps->discards, clear_clean, clear_draw, drawn,
                  clear != state_target_.end() ? clear->second.value_clear : uint64_t(0));
    }
    if (already_written) return;
    if (!front.Begin(presented_, eligible, rect, resolved_input, opaque_rgba, known_clear)) return;

    FlushDeferredClear(image);


    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    const auto order = [&]() {
      dfn_.vkCmdPipelineBarrier(commands_job_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    };
    order();
    MarkGpu(kGpuClears);
    const VkClearValue black{};
    const VkRect2D area{{0, 0}, {front.width, front.height}};
    if (draws_ && draws_->ClearAttachmentInPass(commands_job_, image, VK_IMAGE_ASPECT_COLOR_BIT, black, area)) {
      order();
      ForgetClear(image);
      ++clears_;
      if (++front_partial_clears_ == 1) {
        REXLOG_INFO("[native] C2: partial front composition initialized black ({}x{}, front {:08X})",
                    front.width, front.height, front.address);
      }
    } else {

      front_partial_clear_failed_ = true;
      REXLOG_ERROR("[native] C2: could not initialize partial front composition ({}x{}, front {:08X}); "
                   "automatic front initialization disabled, retaining contents",
                   front.width, front.height, front.address);
    }
  }

  NativeImage* TargetDepth(uint32_t base, uint32_t format, uint32_t pitch) override {
    DepthTarget* image = GetDepth(base, format, pitch);
    if (!image || !Record()) {
      return nullptr;
    }
    Prepare(*image);
    BeforeWriteDepth(*image);


    RestoreContent(*image);
    image->draw_frame = presented_;
    image->draw_order = ++depth_draw_order_;
    return image;
  }

  const NativeImage* TextureResolved(uint32_t address) override {
    const auto it = resolved_.find(address);


    if (!lent_.empty() && lent_.count(address)) {
      ++lent_read_;
    }
    if (it == resolved_.end()) {
      return nullptr;
    }


    ++it->second.reads;
    if (address == bo2zm::reflection_demand::kAddress) {
      bo2zm::reflection_demand::AnnotateRead();
    }


    if (it->second.image.format == format_depth_) {
      AnnotateReadDepth(address);
    } else if (!fronts_presented_.empty() && fronts_presented_.count(address)) {
      AnnotateReadFront(address);
    }

    it->second.perf_learn.OnRead();
    if (!perf_lazy_pending_.empty() || !perf_lazy_expired_.empty()) {
      PerfLazyOnRead(address);
    }
    if (!perf_alias_live_.empty()) {
      if (const NativeImage* twin = PerfAliasServe(address)) {
        return twin;
      }
    }
    return it->second.image.prepared ? &it->second.image : nullptr;
  }


  bool CopyConverted(OwnedImage& source, OwnedImage& target, int32_t x0, int32_t y0, uint32_t dx, uint32_t dy,
                       uint32_t width, uint32_t height, uint32_t info_target) {
    if (!width || !height || source.view == VK_NULL_HANDLE || target.view == VK_NULL_HANDLE ||
        IsDepthFormat(source.format) || IsDepthFormat(target.format)) {
      return false;
    }
    if (conv_fs_ == VK_NULL_HANDLE && !conv_failed_) {
      VkShaderModuleCreateInfo module_info{};
      module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      module_info.codeSize = sizeof(shaders::kSpirvBo2zmCopyResolve);
      module_info.pCode = shaders::kSpirvBo2zmCopyResolve;
      VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kConvSets},
                                         {VK_DESCRIPTOR_TYPE_SAMPLER, kConvSets},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kConvSets}};
      VkDescriptorPoolCreateInfo info_pool{};
      info_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      info_pool.maxSets = kConvSets;
      info_pool.poolSizeCount = 3;
      info_pool.pPoolSizes = sizes;
      bool ok = dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &conv_fs_) == VK_SUCCESS &&
                dfn_.vkCreateDescriptorPool(device_, &info_pool, nullptr, &conv_pool_) == VK_SUCCESS;
      for (uint32_t i = 0; ok && i < kConvSets; ++i) {
        VkDescriptorSetAllocateInfo reserve{};
        reserve.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        reserve.descriptorPool = conv_pool_;
        reserve.descriptorSetCount = 1;
        reserve.pSetLayouts = &layout_descriptors_;
        ok = dfn_.vkAllocateDescriptorSets(device_, &reserve, &conv_sets_[i]) == VK_SUCCESS;
      }
      if (!ok) {
        conv_failed_ = true;
        REXLOG_ERROR("[native] BO2: could not prepare the converted copy of the resolves");
      }
    }
    if (conv_failed_) {
      return false;
    }

    VkRenderPass& pass = conv_passes_[target.format];
    if (pass == VK_NULL_HANDLE) {
      VkAttachmentDescription attachment{};
      attachment.format = target.format;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;


      attachment.loadOp = REXCVAR_GET(bo2zm_perf_v055) ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &reference;
      VkRenderPassCreateInfo info_rp{};
      info_rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info_rp.attachmentCount = 1;
      info_rp.pAttachments = &attachment;
      info_rp.subpassCount = 1;
      info_rp.pSubpasses = &subpass;
      const int scene_sync_mode = REXCVAR_GET(bo2zm_native_scene_pass_mode);
      const auto dependencies = scene::ImageDependencies(scene_sync_mode);
      const bool patch_precise = REXCVAR_GET(bo2zm_native_scene_sync_precise);
      const auto patch_dependencies = scene::precise_patch::PassDependenciesFor(
          patch_precise, uint32_t(REXCVAR_GET(bo2zm_native_scene_sync_mask)));
      if (REXCVAR_GET(bo2zm_native_scene_sync)) {
        if (patch_precise) {
          info_rp.dependencyCount = patch_dependencies.count;
          info_rp.pDependencies = patch_dependencies.items.data();
        } else if (REXCVAR_GET(bo2zm_native_scene_pass_dependencies)) {
          scene::SelectImageDependencies(info_rp, dependencies, scene_sync_mode, true);
        }
      }
      if (dfn_.vkCreateRenderPass(device_, &info_rp, nullptr, &pass) != VK_SUCCESS) {
        pass = VK_NULL_HANDLE;
        return false;
      }
    }
    VkPipeline& pipeline = conv_pipelines_[target.format];
    if (pipeline == VK_NULL_HANDLE) {
      pipeline = CreatePipelineOutput(conv_fs_, nullptr, pass);
      if (pipeline == VK_NULL_HANDLE) {
        return false;
      }
      REXLOG_INFO("[native] BO2: converted copy of resolves to format {}", uint32_t(target.format));
    }
    VkFramebuffer& framebuffer = conv_framebuffers_[target.view];
    if (framebuffer == VK_NULL_HANDLE) {
      VkFramebufferCreateInfo info_fb{};
      info_fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info_fb.renderPass = pass;
      info_fb.attachmentCount = 1;
      info_fb.pAttachments = &target.view;
      info_fb.width = target.width;
      info_fb.height = target.height;
      info_fb.layers = 1;
      if (dfn_.vkCreateFramebuffer(device_, &info_fb, nullptr, &framebuffer) != VK_SUCCESS) {
        framebuffer = VK_NULL_HANDLE;
        return false;
      }
    }
    const VkDescriptorSet set = conv_sets_[conv_next_++ % kConvSets];
    VkDescriptorImageInfo info_image{VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &info_image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);


    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                              1, &barrier, 0, nullptr, 0, nullptr);
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = {{int32_t(dx), int32_t(dy)}, {width, height}};
    MarkGpu(kGpuCopies);
    if (REXCVAR_GET(bo2zm_perf_v055)) {
      bo2zm::native::v055::g_converted_no_load.fetch_add(1, std::memory_order_relaxed);
      bo2zm::native::v055::g_converted_texels.fetch_add(uint64_t(width) * height, std::memory_order_relaxed);
    }
    dfn_.vkCmdBeginRenderPass(commands_job_, &start, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdBindPipeline(commands_job_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    dfn_.vkCmdBindDescriptorSets(commands_job_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 1,
                                 &set, 0, nullptr);
    const VkViewport view{float(dx), float(dy), float(width), float(height), 0.0f, 1.0f};
    const VkRect2D scissor{{int32_t(dx), int32_t(dy)}, {width, height}};
    dfn_.vkCmdSetViewport(commands_job_, 0, 1, &view);
    dfn_.vkCmdSetScissor(commands_job_, 0, 1, &scissor);
    const float rectangle[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
    dfn_.vkCmdPushConstants(commands_job_, layout_pipeline_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(rectangle),
                            rectangle);
    const uint32_t number = (info_target >> 13) & 0x7;
    const int32_t bias = int32_t(info_target << 10) >> 26;
    {
      const uint64_t type = (uint64_t(source.format) << 40) ^ (uint64_t(target.format) << 20) ^ info_target;
      if (conv_views_.size() < 32 && conv_views_.insert(type).second) {
        REXLOG_INFO("[native] BO2 converted copy: source format {} -> target format {} (info {:08X}: guest format {}, "
                    "number {}, bias {}), {}x{}",
                    uint32_t(source.format), uint32_t(target.format), info_target, (info_target >> 7) & 0x3F,
                    number, bias, width, height);
      }
    }
    struct {
      int32_t displacement[2];
      float scale;
      uint32_t mode;
    } constants{{x0 - int32_t(dx), y0 - int32_t(dy)},
                 std::ldexp(1.0f, bias),
                 target.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 4u : number <= 3 ? number : 0u};
    dfn_.vkCmdPushConstants(commands_job_, layout_pipeline_, VK_SHADER_STAGE_FRAGMENT_BIT, 16,
                            sizeof(constants), &constants);
    dfn_.vkCmdDraw(commands_job_, 4, 1, 0, 0);
    dfn_.vkCmdEndRenderPass(commands_job_);


    ++generation_commands_;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 1, &barrier, 0, nullptr, 0, nullptr);
    return true;
  }


  const NativeImage* TextureResolved3D(uint32_t address, uint32_t format, uint32_t width, uint32_t height,
                                        uint32_t depth3d, uint32_t pitch, bool tiled) override {
    const NativeImage* flat = TextureResolved(address);
    if (!flat || !copy_image_ || flat->format != kFormatColor || flat->width_guest ||
        format != uint32_t(xenos::TextureFormat::k_8_8_8_8)) {
      return nullptr;
    }
    const auto e = writes_resolved_.find(address);
    const uint64_t write = e != writes_resolved_.end() ? e->second : 0;
    auto it = volumes_.find(address);
    if (it != volumes_.end()) {
      ResolvedVolume& v = it->second;
      if (v.image.width != width || v.image.height != height || v.depth3d != depth3d || v.pitch != pitch ||
          v.pitch_flat != flat->width) {
        return nullptr;
      }
      if (v.write == write && v.source == flat->image) {
        v.last_use = presented_;
        return &v.image;
      }
    } else {
      if (!tiled || width > 256 || height > 256 || depth3d > 256) {
        if (warned_.insert(0x3D0000).second) {
          REXLOG_WARN("[native] BO2: 3D texture {}x{}x{} over a resolved one at {:08X} {}: not handled", width, height,
                      depth3d, address, tiled ? "too large" : "linear");
        }
        return nullptr;
      }
      ResolvedVolume v;
      if (!RegionsVolume(*flat, width, height, depth3d, pitch, v.regions, v.block)) {
        if (warned_.insert(0x3D0001).second) {
          REXLOG_WARN("[native] BO2: 3D texture {}x{}x{} at {:08X}: the resolved {}x{} does not cover its "
                      "memory", width,
                      height, depth3d, address, flat->width, flat->height);
        }
        return nullptr;
      }
      VkImageCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      info.imageType = VK_IMAGE_TYPE_3D;
      info.format = kFormatColor;
      info.extent = {width, height, depth3d};
      info.mipLevels = 1;
      info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(vulkan_device_, info,
                                                                 rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                                 v.image.image, v.image.guest_memory)) {
        return nullptr;
      }
      v.image.width = width;
      v.image.height = height;
      v.image.format = kFormatColor;
      v.depth3d = depth3d;
      v.pitch = pitch;
      v.pitch_flat = flat->width;
      REXLOG_INFO("[native] BO2: 3D texture {}x{}x{} at {:08X} made from the resolved {}x{} ({} regions of {}x{})",
                  width, height, depth3d, address, flat->width, flat->height, v.regions.size(), v.block & 0xFFFF,
                  v.block >> 16);
      it = volumes_.emplace(address, std::move(v)).first;
    }
    ResolvedVolume& v = it->second;
    v.last_use = presented_;
    if (draws_) {

      if (draws_->PassActive()) {
        bo2zm::native::performance::g_pass_splits_volume.fetch_add(1, std::memory_order_relaxed);
      }
      draws_->FinishPass();
    }
    if (!BuildVolume(v, *flat, write)) {
      return nullptr;
    }
    return &v.image;
  }


private:
  struct ResolvedVolume;
public:
  bool BuildVolume(ResolvedVolume& v, const NativeImage& flat, uint64_t write) {
    if (!Record()) {
      return false;
    }


    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    VkImageMemoryBarrier first{};
    first.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    first.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    first.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    first.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    first.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    first.image = v.image.image;
    first.subresourceRange = kRangeColor;
    first.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_,
                              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, v.image.prepared ? 0 : 1,
                              &first);
    v.image.prepared = true;
    MarkGpu(kGpuCopies);
    BeforeImageTransfer(commands_job_);
    copy_image_(commands_job_, flat.image, VK_IMAGE_LAYOUT_GENERAL, v.image.image, VK_IMAGE_LAYOUT_GENERAL,
                   uint32_t(v.regions.size()), v.regions.data());
    VkMemoryBarrier later{};
    later.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    later.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    later.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                              &later, 0, nullptr, 0, nullptr);
    v.image.swap_rb = flat.swap_rb;
    v.write = write;
    v.source = flat.image;
    ++volumes_done_;
    return true;
  }


  void RefreshVolumesEager() {
    if (volumes_.empty() || !copy_image_ || !recording_ || (draws_ && draws_->PassActive())) {
      return;
    }
    if (volume_eager_on_ < 0) {
      volume_eager_on_ = REXCVAR_GET(bo2zm_native_volume_eager) ? 1 : 0;
      REXLOG_INFO("[native] C2: 3D color-grading table rebuilt right after its resolve (bo2zm_native_volume_eager) "
                  "= {}", volume_eager_on_ ? "yes" : "no, at the first draw that samples it");
    }
    if (!volume_eager_on_) {
      return;
    }
    for (auto& [address, v] : volumes_) {
      if (presented_ - v.last_use > kVolumeEagerFrames) {
        continue;
      }
      const auto r = resolved_.find(address);
      if (r == resolved_.end() || !r->second.image.prepared) {
        continue;
      }
      const NativeImage& flat = r->second.image;
      if (flat.format != kFormatColor || flat.width_guest || v.pitch_flat != flat.width) {
        continue;
      }
      if ((composite_has_ && composite_.address == address) || pending_.count(address) ||
          fronts_pending_.count(address) || (!lent_.empty() && lent_.count(address))) {
        continue;
      }
      const auto e = writes_resolved_.find(address);
      const uint64_t write = e != writes_resolved_.end() ? e->second : 0;
      if (v.write == write && v.source == flat.image) {
        continue;
      }
      if (BuildVolume(v, flat, write)) {
        bo2zm::native::performance::g_volumes_eager.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }


  static bool RegionsVolume(const OwnedImage& flat, uint32_t width, uint32_t height, uint32_t depth3d, uint32_t pitch,
                              std::vector<VkImageCopy>& regions, uint32_t& block) {
    std::unordered_map<int32_t, uint32_t> inverse;
    inverse.reserve(size_t(flat.width) * flat.height);
    for (uint32_t v = 0; v < flat.height; ++v) {
      for (uint32_t u = 0; u < flat.width; ++u) {
        inverse.emplace(OffsetTiled2D(int32_t(u), int32_t(v), flat.width, 2), u | (v << 16));
      }
    }
    const auto where = [&](uint32_t x, uint32_t y, uint32_t z, uint32_t& uv) {
      const auto it = inverse.find(OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z), pitch, height, 2));
      if (it == inverse.end()) {
        return false;
      }
      uv = it->second;
      return true;
    };
    static constexpr uint32_t kBlocks[][2] = {{8, 8}, {8, 4}, {8, 2}, {4, 1}, {1, 1}};
    for (const auto& b : kBlocks) {
      const uint32_t bw = b[0], bh = b[1];
      if (width % bw || height % bh) {
        continue;
      }
      regions.clear();
      bool whole = true;
      for (uint32_t z = 0; z < depth3d && whole; ++z) {
        for (uint32_t by = 0; by < height && whole; by += bh) {
          for (uint32_t bx = 0; bx < width && whole; bx += bw) {
            uint32_t uv0 = 0;
            if (!where(bx, by, z, uv0)) {
              return false;
            }
            for (uint32_t y = by; y < by + bh && whole; ++y) {
              for (uint32_t x = bx; x < bx + bw; ++x) {
                uint32_t uv = 0;
                if (!where(x, y, z, uv)) {
                  return false;
                }
                if (uv != uv0 + (x - bx) + ((y - by) << 16)) {
                  whole = false;
                  break;
                }
              }
            }
            VkImageCopy r{};
            r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.srcOffset = {int32_t(uv0 & 0xFFFF), int32_t(uv0 >> 16), 0};
            r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.dstOffset = {int32_t(bx), int32_t(by), int32_t(z)};
            r.extent = {bw, bh, 1};
            regions.push_back(r);
          }
        }
      }
      if (whole) {
        block = bw | (bh << 16);
        return true;
      }
    }
    return false;
  }


  void DeadDepthReads(bool dead) override { reads_depth_dead_ = dead; }


  bool OverlayShadowPass(const NativeImage* depth, bool only_depth) override {
    if (!depth || depth != sm_target_cycle_ || sm_state_ != kSmOverlay) {
      return false;
    }
    if (!only_depth) {
      ShadowMinMissCycle("the overlay pass has a color target", 0, true);
    }
    return true;
  }


  void OverlayShadowDraw(bool exact, uint32_t control_z) override {
    ++sm_draws_overlay_;
    if (!exact) {
      ++sm_draws_no_exact_;
      ShadowMinMissCycle("an overlay draw does not leave the minimum (the datum is its Z control)", control_z, true);
    }
  }


  uint32_t AddressShadowOverlay() const override {
    return (sm_virtual_ || (sm_on_ && sm_phase_ != kSmOff)) ? sm_overlay_ : 0;
  }


  const NativeImage* PartnerShadowOverlay(bool capable, uint32_t ps) override {
    if (sm_virtual_) {

      const auto it = resolved_.find(sm_base_);
      const bool base_ready = it != resolved_.end() && it->second.image.prepared;
      if (!sm_virtual_valid_ || !capable || !base_ready) {
        ++sm_reads_unable_;
        ShadowMinMismatch(!sm_virtual_valid_ ? "the overlay texture alone is sampled and the base one is already "
                                                     "from another cycle (the datum is the pixel shader)"
                               : !capable ? "a pixel shader without tfetch2DShadowMin samples the overlay "
                                          "alone (the datum is its number)"
                                        : "the world texture is not there (the datum is the pixel shader)",
                               ps);
        return nullptr;
      }
      ++sm_reads_min_;
      return &it->second.image;
    }


    if (sm_state_ != kSmFree) {
      ++sm_reads_a_offbeat_;
      ShadowMinNoFit("the overlay texture is sampled between the two resolves (the datum is the pixel shader)",
                         ps);
    }
    if (!capable) {
      ++sm_reads_unable_;
      ShadowMinNoFit("a pixel shader without tfetch2DShadowMin samples the overlay texture: library without minimum? "
                         "(the datum is its number)",
                         ps);
      return nullptr;
    }
    const auto it = resolved_.find(sm_overlay_);
    if (!sm_on_ || sm_phase_ == kSmOff || it == resolved_.end() || !it->second.image.prepared) {
      ++sm_reads_normal_;
      return nullptr;
    }

    ++sm_reads_yes_same_;
    return &it->second.image;
  }


  struct ClearUse {
    uint32_t base = 0;
    uint32_t format = 0;
    uint32_t pitch = 0;
    bool depth = false;
    bool in_pass = false;
    bool open = false;
    uint32_t used_width = 0;
    uint32_t used_height = 0;
    uint32_t max_width = 0;
    uint32_t max_height = 0;
    uint64_t cycles = 0;
    uint64_t clears = 0;
    uint64_t pixels_clears = 0;
    uint64_t pixels_used = 0;

    bool band = false;
    uint32_t band_since = 0;
    VkImage band_image = VK_NULL_HANDLE;
    VkClearColorValue band_color{};
    uint32_t bands_completed = 0;
    bool area_useful_off = false;
  };


  void AnnotatePassArea(const NativeImage* image, uint32_t width, uint32_t height) override {
    if (image) {
      AnnotateUseClear(*image, width, height);
    }
  }


  void AnnotateClearDiag(const OwnedImage& image, uint32_t base, uint32_t format, uint32_t pitch, bool depth,
                         bool in_pass) {
    if (!diag_clears_) {
      return;
    }
    ClearUse& u = use_clears_[&image];
    if (u.open) {
      u.pixels_used += uint64_t(u.used_width) * u.used_height;
      ++u.cycles;
    }
    u.base = base;
    u.format = format;
    u.pitch = pitch;
    u.depth = depth;
    u.in_pass = in_pass;
    ++u.clears;
    u.pixels_clears += uint64_t(image.width) * image.height;
    u.used_width = 0;
    u.used_height = 0;
    u.open = true;
  }


  void AnnotateUseClear(const OwnedImage& image, uint32_t width, uint32_t height) {


    if (use_clears_.empty() || (!diag_clears_ && !bands_pending_)) {
      return;
    }
    const auto it = use_clears_.find(&image);
    if (it == use_clears_.end()) {
      return;
    }
    ClearUse& u = it->second;
    width = std::min(width, image.width);
    height = std::min(height, image.height);


    if (u.band && (u.band_image != image.image || height >= u.band_since)) {
      CompleteBand(image, u);
    }
    if (!diag_clears_) {
      return;
    }
    u.used_width = std::max(u.used_width, width);
    u.used_height = std::max(u.used_height, height);
    u.max_width = std::max(u.max_width, width);
    u.max_height = std::max(u.max_height, height);
  }


  bool ClearColorAreaUseful(OwnedImage& target, const VkClearColorValue& color) {
    if (!clear_area_useful_ || !diag_clears_ || !draws_ || target.format != kFormatColor) {
      return false;
    }
    const auto it = use_clears_.find(&target);
    if (it == use_clears_.end()) {
      return false;
    }
    ClearUse& u = it->second;
    if (u.area_useful_off || u.cycles < kAreaUsefulCycles || u.max_height == 0) {
      return false;
    }


    const uint32_t height = std::min(target.height, (u.max_height + 64u) & ~63u);
    if (height >= target.height) {
      return false;
    }
    if (!draws_->ClearColorInPass(commands_job_, target, color, VkRect2D{{0, 0}, {target.width, height}})) {
      return false;
    }
    if (!u.band) {
      ++bands_pending_;
    }
    u.band = true;
    u.band_since = height;
    u.band_image = target.image;
    u.band_color = color;
    ++area_useful_clears_;
    area_useful_pixels_ += uint64_t(target.width) * (target.height - height);
    return true;
  }


  void RemoveBand(const OwnedImage& target) {
    if (use_clears_.empty()) {
      return;
    }
    const auto it = use_clears_.find(&target);
    if (it != use_clears_.end() && it->second.band) {
      it->second.band = false;
      if (bands_pending_) {
        --bands_pending_;
      }
    }
  }


  void CompleteBand(const OwnedImage& image, ClearUse& u) {
    u.band = false;
    if (bands_pending_) {
      --bands_pending_;
    }
    ++u.bands_completed;
    ++area_useful_completed_;
    if (u.bands_completed >= 3) {
      u.area_useful_off = true;
    }
    const bool same_image = u.band_image == image.image;
    const uint32_t height_band = image.height > u.band_since ? image.height - u.band_since : 0;
    if (same_image && (!height_band || (draws_ && Record() &&
                                         draws_->ClearColorInPass(
                                             commands_job_, image, u.band_color,
                                             VkRect2D{{0, int32_t(u.band_since)}, {image.width, height_band}})))) {
      return;
    }

    ++area_useful_misses_;
    if (!area_useful_global_off_) {
      area_useful_global_off_ = true;
      clear_area_useful_ = false;
      REXLOG_ERROR("[native] C2 clear useful area: MISMATCH, could not complete the {}x{} band from row {} ({}). Off "
                   "for the rest of the session: the whole image is cleared",
                   image.width, image.height, u.band_since, same_image ? "the pass failed" : "the image changed");
    }
  }


  void ReportClears() {
    diag_clears_ = REXCVAR_GET(bo2zm_native_diag_clears);
    clear_area_useful_ = REXCVAR_GET(bo2zm_native_clear_useful_area) && !area_useful_global_off_;
    const auto now = std::chrono::steady_clock::now();
    if (report_clears_ == std::chrono::steady_clock::time_point{}) {
      report_clears_ = now;
      presented_report_clears_ = presented_;
      return;
    }
    if (now - report_clears_ < std::chrono::seconds(20)) {
      return;
    }
    report_clears_ = now;
    const uint64_t frames = presented_ - presented_report_clears_;
    presented_report_clears_ = presented_;
    if (diag_clears_ && frames > 0 && !use_clears_.empty()) {
      struct ReportRow {
        const ClearUse* use;
        uint32_t width;
        uint32_t height;
        double clear;
        double used;
      };
      std::vector<ReportRow> rows;
      double total_clear = 0.0, total_used = 0.0;
      for (const auto& [image, u] : use_clears_) {
        if (!u.clears) {
          continue;
        }
        const double clear = double(u.pixels_clears) / 1e6 / double(frames);

        const double used = std::min(clear, double(u.pixels_used) / 1e6 / double(frames));
        rows.push_back({&u, image->width, image->height, clear, used});
        total_clear += clear;
        total_used += used;
      }
      std::sort(rows.begin(), rows.end(),
                [](const ReportRow& a, const ReportRow& b) { return a.clear - a.used > b.clear - b.used; });
      std::string list;
      for (const ReportRow& row : rows) {
        const ClearUse& u = *row.use;
        list += fmt::format(" | {:03X}/{} {} {}x{}{}: {:.2f} per frame, {:.2f} Mpixels cleared, {:.2f} used (up to "
                             "{}x{})",
                             u.base, u.pitch, u.depth ? "depth" : "color", row.width, row.height,
                             u.in_pass ? " (per pass, ZCULL)" : "", double(u.clears) / double(frames),
                             row.clear, row.used, u.max_width, u.max_height);
      }
      BO2ZM_REPORT_RING("[native] C2 clears per target ({} frames): {:.2f} Mpixels cleared and {:.2f} used "
                           "per frame (spare {:.2f}: ~{:.2f} ms real){}",
                           frames, total_clear, total_used, total_clear - total_used,
                           (total_clear - total_used) * 0.075, list);
    }
    if (area_useful_clears_ || area_useful_misses_) {
      const double mp =
          double(area_useful_pixels_ - area_useful_pixels_previous_) / 1e6 / double(std::max<uint64_t>(frames, 1));
      BO2ZM_REPORT_RING("[native] C2 clear useful area: {} color clears trimmed since boot; {:.2f} Mpixels "
                           "per frame not cleared (~{:.2f} ms real); {} bands completed before a use; {} failures{}",
                           area_useful_clears_, mp, mp * 0.075, area_useful_completed_, area_useful_misses_,
                           area_useful_global_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the same)");
      area_useful_pixels_previous_ = area_useful_pixels_;
    }
    for (auto& [image, u] : use_clears_) {
      u.clears = 0;
      u.pixels_clears = 0;
      u.pixels_used = 0;
    }
  }


  void AnnotateReadDepth(uint32_t address) {
    if (reads_depth_dead_) {
      last_read_dead_[address] = presented_;
      if (!pending_.empty()) {
        const auto p = pending_.find(address);
        if (p != pending_.end()) {
          p->second.read_dead = true;
        }
      }
      return;
    }
    last_read_live_[address] = presented_;
    if (!pending_.empty() && pending_.count(address)) {
      RecordCopyPending(address);
      ++lazy_copied_read_;
    }
    if (!expired_.empty() && expired_.erase(address)) {
      ++lazy_reads_late_;
      if (!lazy_off_) {
        lazy_off_ = true;
        REXLOG_ERROR("[native] C2 lazy depth: MISMATCH, a draw samples {:08X} after its copy was dropped (late read: it "
                     "reads an old depth). Off for the rest of the session: always copied",
                     address);
      }
    }
  }


  void RecordCopyPending(uint32_t address) {
    const auto p = pending_.find(address);
    if (p == pending_.end()) {
      return;
    }
    const PendingCopy pending = p->second;
    pending_.erase(p);
    const auto r = resolved_.find(address);
    if (r == resolved_.end() || r->second.image.image != pending.target_vk || !pending.source ||
        pending.source->image != pending.source_vk || !copy_image_ || !Record()) {
      expired_.insert(address);
      return;
    }
    if (draws_) {
      if (draws_->PassActive()) {
        bo2zm::native::performance::g_pass_splits_copy.fetch_add(1, std::memory_order_relaxed);
      }
      draws_->FinishPass();
    }
    MarkGpu(kGpuCopies);
    BeforeImageTransfer(commands_job_);
    EdramRead(pending.source_vk, "a deferred depth copy");
    copy_image_(commands_job_, pending.source_vk, VK_IMAGE_LAYOUT_GENERAL, pending.target_vk,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &pending.copy);
    ++copies_;
    AnnotateCopy(pending.copy.extent.width, pending.copy.extent.height);
    ResolvedWritten(address, uint64_t(pending.copy.extent.width) * pending.copy.extent.height);
  }


  void BeforeWriteDepth(const OwnedImage& image) {
    if (pending_.empty() || image.image == VK_NULL_HANDLE) {
      return;
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->second.source_vk != image.image) {
        ++it;
        continue;
      }
      const uint32_t address = it->first;


      if (!it->second.read_dead) {
        ++it;
        RecordCopyPending(address);
        ++lazy_copied_write_;
      } else {
        lazy_pixels_saved_ += uint64_t(it->second.copy.extent.width) * it->second.copy.extent.height;
        it = pending_.erase(it);
        expired_.insert(address);
        ++lazy_dropped_;
      }
    }
  }


  bool PostponeCopyDepth(uint32_t address, const OwnedImage& source, const Resolved& resolved,
                               const VkImageCopy& copy) {
    if (lazy_off_ || !REXCVAR_GET(bo2zm_native_lazy_depth)) {
      return false;
    }
    const auto dead = last_read_dead_.find(address);
    if (dead == last_read_dead_.end() || presented_ - dead->second > kLazyFrames) {
      return false;
    }
    const auto live = last_read_live_.find(address);
    if (live != last_read_live_.end() && presented_ - live->second <= kLazyFrames) {
      return false;
    }
    PendingCopy& pending = pending_[address];
    pending.source = &source;
    pending.source_vk = source.image;
    pending.target_vk = resolved.image.image;
    pending.copy = copy;
    pending.frame = presented_;
    pending.read_dead = false;
    ResolvedWritten(address);
    ++lazy_postponed_;
    return true;
  }


  struct PendingFront {
    OwnedImage* target = nullptr;
    VkImage source_vk = VK_NULL_HANDLE;
    int32_t retained = -1;
    VkImage texture_vk = VK_NULL_HANDLE;
    VkImageCopy copy{};
  };
  struct FrontImage {
    OwnedImage image;
    uint32_t retained_per = 0;
  };


  void ForgetFrontComposition(uint32_t address, const char* reason) {
    for (auto it = front_compositions_.begin(); it != front_compositions_.end();) {
      if (it->second.address == address) {
        if (FrontTraceLine()) {
          REXLOG_INFO("[front-trace] invalidate target {:p} front {:08X} reason {} write_frame {} frame {}",
                      static_cast<const void*>(it->first), address, reason, it->second.write_frame, presented_);
        }
        it = front_compositions_.erase(it);
      } else ++it;
    }
  }

  bool IsSwapFrontOnly(uint32_t address) const {
    const auto presented = fronts_presented_.find(address);
    if (presented == fronts_presented_.end() || presented_ - presented->second > kFrontFrames) {
      return false;
    }
    const auto read = fronts_read_.find(address);
    return read == fronts_read_.end() || presented_ - read->second > kFrontFrames;
  }


  void FreeRetained(const PendingFront& pending) {
    if (pending.retained >= 0 && size_t(pending.retained) < fronts_images_.size()) {
      fronts_images_[size_t(pending.retained)].retained_per = 0;
    }
  }


  OwnedImage* SourceFront(const PendingFront& pending) {
    OwnedImage* source = nullptr;
    if (pending.retained >= 0) {
      if (size_t(pending.retained) < fronts_images_.size()) {
        source = &fronts_images_[size_t(pending.retained)].image;
      }
    } else {
      source = pending.target;
    }
    return source && source->image == pending.source_vk && source->prepared ? source : nullptr;
  }


  void RecordCopyFront(uint32_t address) {
    const auto p = fronts_pending_.find(address);
    if (p == fronts_pending_.end()) {
      return;
    }
    PerfMaterializeAliasesOf(address);
    const PendingFront pending = p->second;
    fronts_pending_.erase(p);
    const auto r = resolved_.find(address);
    OwnedImage* const source = SourceFront(pending);
    if (r == resolved_.end() || r->second.image.image != pending.texture_vk || !source || !copy_image_ ||
        !Record()) {
      FreeRetained(pending);
      fronts_expired_.insert(address);
      return;
    }
    if (draws_) {
      if (draws_->PassActive()) {
        bo2zm::native::performance::g_pass_splits_copy.fetch_add(1, std::memory_order_relaxed);
      }
      draws_->FinishPass();
    }
    MarkGpu(kGpuCopies);
    BeforeImageTransfer(commands_job_);
    EdramRead(pending.source_vk, "a deferred front buffer copy");
    copy_image_(commands_job_, pending.source_vk, VK_IMAGE_LAYOUT_GENERAL, pending.texture_vk,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &pending.copy);
    ++copies_;
    AnnotateCopy(pending.copy.extent.width, pending.copy.extent.height);
    ResolvedWritten(address, uint64_t(pending.copy.extent.width) * pending.copy.extent.height);
    fronts_expired_.erase(address);
    FreeRetained(pending);
  }


  void ResolveFrontEarlier(uint32_t address, bool whole) {
    if (whole && !fronts_expired_.empty()) {
      fronts_expired_.erase(address);
    }
    if (fronts_pending_.empty()) {
      return;
    }
    const auto p = fronts_pending_.find(address);
    if (p == fronts_pending_.end()) {
      return;
    }
    if (!whole) {
      RecordCopyFront(address);
      ++front_copied_write_;
      return;
    }
    front_pixels_saved_ += uint64_t(p->second.copy.extent.width) * p->second.copy.extent.height;
    FreeRetained(p->second);
    fronts_pending_.erase(p);
    ++front_replaced_;
  }


  bool PostponeCopyFront(uint32_t address, OwnedImage& target, const Resolved& resolved, const VkImageCopy& copy) {
    if (front_off_ || !REXCVAR_GET(bo2zm_native_lazy_front) || REXCVAR_GET(bo2zm_native_diag_resolved)) {
      return false;
    }
    if (copy.srcOffset.x != 0 || copy.srcOffset.y != 0 || copy.dstOffset.x != 0 || copy.dstOffset.y != 0 ||
        copy.extent.width != resolved.image.width || copy.extent.height != resolved.image.height ||
        target.format != kFormatColor || resolved.image.format != kFormatColor) {
      return false;
    }

    const int32_t texels_read = REXCVAR_GET(bo2zm_native_read_resolved_texels);
    if (uint64_t(copy.extent.width) * copy.extent.height <= uint64_t(std::max<int32_t>(texels_read, 0))) {
      return false;
    }
    if (!IsSwapFrontOnly(address)) {
      return false;
    }
    if (perf_drop_fronts_) {
      perf_front_learn_[address].OnResolve();
    }
    PendingFront& pending = fronts_pending_[address];
    pending.target = &target;
    pending.source_vk = target.image;
    pending.retained = -1;
    pending.texture_vk = resolved.image.image;
    pending.copy = copy;
    ResolvedWritten(address);
    fronts_expired_.erase(address);
    ++front_postponed_;
    return true;
  }


  void EdramRead(VkImage image, const char* where) {
    if (image != VK_NULL_HANDLE && !bo2zm::native::edram::Get().Read(uint64_t(image)) && edram_mismatch_logged_++ == 0) {
      REXLOG_ERROR("[perf] 0.47 EDRAM dead stores: MISMATCH, {} reads a surface whose previous store was skipped. "
                   "Off for the rest of the session",
                   where);
    }
  }
  void EdramClear(VkImage image) {
    if (image != VK_NULL_HANDLE) bo2zm::native::edram::Get().Clear(uint64_t(image));
  }
  void EdramTouch(VkImage image) {
    if (image != VK_NULL_HANDLE) bo2zm::native::edram::Get().Touch(uint64_t(image));
  }


  bool PerfRecordWholeCopy(VkImage from, VkImage to, uint32_t width, uint32_t height) {
    if (!copy_image_ || !Record()) {
      return false;
    }
    if (draws_) {
      if (draws_->PassActive()) {
        bo2zm::native::performance::g_pass_splits_copy.fetch_add(1, std::memory_order_relaxed);
      }
      draws_->FinishPass();
    }
    MarkGpu(kGpuCopies);
    BeforeImageTransfer(commands_job_);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    copy_image_(commands_job_, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    ++copies_;
    AnnotateCopy(width, height);
    return true;
  }

  void PerfMismatch(const char* what, uint32_t address, bool& off) {
    if (!off) {
      off = true;
      REXLOG_ERROR("[perf] 0.46 {}: MISMATCH at {:08X} (the content was needed after all). Off for the rest of the "
                   "session: always copied from now on",
                   what, address);
    }
  }


  bool PerfDropFront(uint32_t address) {
    if (!perf_drop_fronts_ || front_off_) {
      return false;
    }
    const auto learn = perf_front_learn_.find(address);
    const auto p = fronts_pending_.find(address);
    if (learn == perf_front_learn_.end() || !learn->second.MayDrop() || !IsSwapFrontOnly(address) ||
        p == fronts_pending_.end() || p->second.retained >= 0) {
      bo2zm::native::v046::g_fronts_kept.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const uint64_t pixels = uint64_t(p->second.copy.extent.width) * p->second.copy.extent.height;
    front_pixels_saved_ += pixels;
    bo2zm::native::v046::g_pixels_not_copied.fetch_add(pixels, std::memory_order_relaxed);
    FreeRetained(p->second);
    fronts_pending_.erase(p);
    fronts_expired_.insert(address);
    bo2zm::native::v046::g_fronts_dropped.fetch_add(1, std::memory_order_relaxed);
    return true;
  }


  void PerfLazySourceWritten(VkImage source) {
    for (auto it = perf_lazy_pending_.begin(); it != perf_lazy_pending_.end();) {
      if (it->second.source_vk != source) {
        ++it;
        continue;
      }
      bo2zm::native::v046::g_pixels_not_copied.fetch_add(
          uint64_t(it->second.copy.extent.width) * it->second.copy.extent.height, std::memory_order_relaxed);
      bo2zm::native::v046::g_lazy_dropped.fetch_add(1, std::memory_order_relaxed);
      perf_lazy_expired_.insert(it->first);
      it = perf_lazy_pending_.erase(it);
    }
  }


  void PerfLazyRecord(uint32_t address) {
    const auto p = perf_lazy_pending_.find(address);
    if (p == perf_lazy_pending_.end()) {
      return;
    }
    const PerfLazy pending = p->second;
    perf_lazy_pending_.erase(p);
    const auto r = resolved_.find(address);
    if (r == resolved_.end() || r->second.image.image != pending.texture_vk || !pending.target ||
        pending.target->image != pending.source_vk || !copy_image_ || !Record()) {
      bo2zm::native::v046::g_lazy_mismatch.fetch_add(1, std::memory_order_relaxed);
      PerfMismatch("lazy resolve (the source or texture image changed)", address, perf_lazy_off_);
      return;
    }
    if (draws_) {
      if (draws_->PassActive()) {
        bo2zm::native::performance::g_pass_splits_copy.fetch_add(1, std::memory_order_relaxed);
      }
      draws_->FinishPass();
    }
    MarkGpu(kGpuCopies);
    BeforeImageTransfer(commands_job_);
    EdramRead(pending.source_vk, "a deferred resolve");
    copy_image_(commands_job_, pending.source_vk, VK_IMAGE_LAYOUT_GENERAL, pending.texture_vk,
                VK_IMAGE_LAYOUT_GENERAL, 1, &pending.copy);
    ++copies_;
    AnnotateCopy(pending.copy.extent.width, pending.copy.extent.height);
    bo2zm::native::v046::g_lazy_recorded.fetch_add(1, std::memory_order_relaxed);
  }


  void PerfLazyOnRead(uint32_t address) {
    if (perf_lazy_pending_.count(address)) {
      PerfLazyRecord(address);
      return;
    }
    if (perf_lazy_expired_.erase(address)) {
      bo2zm::native::v046::g_lazy_mismatch.fetch_add(1, std::memory_order_relaxed);
      PerfMismatch("lazy resolve (a dropped copy is sampled)", address, perf_lazy_off_);
    }
  }


  const NativeImage* PerfAliasServe(uint32_t address) {
    const auto a = perf_alias_live_.find(address);
    if (a == perf_alias_live_.end()) {
      return nullptr;
    }
    const auto twin = resolved_.find(a->second.of);
    const auto self = resolved_.find(address);
    if (twin == resolved_.end() || twin->second.image.image != a->second.of_image || !twin->second.image.prepared ||
        self == resolved_.end() || self->second.image.image != a->second.self_image) {
      perf_alias_live_.erase(a);
      bo2zm::native::v046::g_alias_mismatch.fetch_add(1, std::memory_order_relaxed);
      PerfMismatch("resolve alias (the twin image changed outside a resolve)", address, perf_alias_off_);
      if (draws_) {
        draws_->InvalidateTexturesIn(address);
      }
      return nullptr;
    }
    return &twin->second.image;
  }


  void PerfMaterialize(uint32_t address) {
    const auto a = perf_alias_live_.find(address);
    if (a == perf_alias_live_.end()) {
      return;
    }
    const PerfAlias alias = a->second;
    perf_alias_live_.erase(a);
    const auto twin = resolved_.find(alias.of);
    const auto self = resolved_.find(address);
    if (twin == resolved_.end() || twin->second.image.image != alias.of_image || self == resolved_.end() ||
        self->second.image.image != alias.self_image ||
        !PerfRecordWholeCopy(alias.of_image, alias.self_image, alias.width, alias.height)) {
      bo2zm::native::v046::g_alias_mismatch.fetch_add(1, std::memory_order_relaxed);
      PerfMismatch("resolve alias (could not materialize)", address, perf_alias_off_);
    } else {
      bo2zm::native::v046::g_alias_materialized.fetch_add(1, std::memory_order_relaxed);
    }
    if (draws_) {
      draws_->InvalidateTexturesIn(address);
    }
  }


  bool PerfSwapMaterialize(uint32_t address) {
    const auto a = perf_alias_live_.find(address);
    if (a == perf_alias_live_.end()) {
      return false;
    }
    const PerfAlias alias = a->second;
    const auto twin = resolved_.find(alias.of);
    const auto self = resolved_.find(address);
    if (twin == resolved_.end() || self == resolved_.end() || twin->second.image.image != alias.of_image ||
        self->second.image.image != alias.self_image || twin->second.image.width != self->second.image.width ||
        twin->second.image.height != self->second.image.height ||
        twin->second.image.format != self->second.image.format || !self->second.image.prepared ||
        twin->second.image.pool_block != self->second.image.pool_block) {
      return false;
    }
    perf_alias_live_.erase(a);
    NativeImage& x = twin->second.image;
    NativeImage& y = self->second.image;
    std::swap(x.image, y.image);
    std::swap(x.guest_memory, y.guest_memory);
    std::swap(x.view, y.view);
    std::swap(x.prepared, y.prepared);
    if (draws_) {
      draws_->InvalidateTexturesIn(alias.of);
      draws_->InvalidateTexturesIn(address);
    }
    bo2zm::native::v046::g_alias_swapped.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void PerfMaterializeAliasesOf(uint32_t twin, uint64_t request = 0) {
    if (perf_alias_live_.empty()) {
      return;
    }
    std::vector<uint32_t> users;
    for (const auto& [address, alias] : perf_alias_live_) {
      if (alias.of == twin) {
        users.push_back(address);
      }
    }
    for (uint32_t address : users) {
      const auto a = perf_alias_live_.find(address);
      if (request != 0 && a != perf_alias_live_.end() && a->second.twin_request == request &&
          PerfSwapMaterialize(address)) {
        continue;
      }
      PerfMaterialize(address);
    }
  }

  void PerfMaterializeAll() {
    while (!perf_alias_live_.empty()) {
      PerfMaterialize(perf_alias_live_.begin()->first);
    }
  }


  void PerfSettleAll() {
    PerfMaterializeAll();
    while (!perf_lazy_pending_.empty()) {
      PerfLazyRecord(perf_lazy_pending_.begin()->first);
    }
  }


  void PerfBeforeDestination(uint32_t address, uint64_t request) {
    if (!perf_alias_live_.empty()) {
      PerfMaterializeAliasesOf(address, request);
      const auto self = perf_alias_live_.find(address);
      if (self != perf_alias_live_.end() && self->second.request != request) {
        PerfMaterialize(address);
      }
    }
    const auto lazy = perf_lazy_pending_.find(address);
    if (lazy != perf_lazy_pending_.end() && lazy->second.request != request) {
      PerfLazyRecord(address);
    }
    perf_request_ = request;
  }


  void PerfSettleBeforeOtherWrite(uint32_t address) {
    if (perf_alias_live_.count(address)) {
      PerfMaterialize(address);
    }
    if (perf_lazy_pending_.count(address)) {
      PerfLazyRecord(address);
    }
  }


  void PerfSettleBeforeBitWrite(uint32_t address, bool whole) {
    if (perf_alias_live_.count(address)) {
      if (whole) {
        perf_alias_live_.erase(address);
        bo2zm::native::v046::g_alias_dropped.fetch_add(1, std::memory_order_relaxed);
        if (draws_) {
          draws_->InvalidateTexturesIn(address);
        }
      } else {
        PerfMaterialize(address);
      }
    }
    const auto lazy = perf_lazy_pending_.find(address);
    if (lazy != perf_lazy_pending_.end()) {
      if (whole) {
        bo2zm::native::v046::g_pixels_not_copied.fetch_add(
            uint64_t(lazy->second.copy.extent.width) * lazy->second.copy.extent.height, std::memory_order_relaxed);
        bo2zm::native::v046::g_lazy_dropped.fetch_add(1, std::memory_order_relaxed);
        perf_lazy_pending_.erase(lazy);
      } else {
        PerfLazyRecord(address);
      }
    }
    if (whole) {
      perf_lazy_expired_.erase(address);
    }
  }


  bool PerfSkipCopy(uint32_t address, OwnedImage& target, Resolved& resolved, const VkImageCopy& copy) {
    const bool learned_unread = resolved.perf_learn.OnCopy();
    if (!perf_alias_ && !perf_lazy_) {
      return false;
    }
    const bool whole = copy.dstOffset.x == 0 && copy.dstOffset.y == 0 && copy.extent.width == resolved.image.width &&
                       copy.extent.height == resolved.image.height;

    const int32_t texels_read = REXCVAR_GET(bo2zm_native_read_resolved_texels);
    const bool small =
        uint64_t(copy.extent.width) * copy.extent.height <= uint64_t(std::max<int32_t>(texels_read, 0));


    if (!whole || small || target.format != resolved.image.format || !resolved.image.prepared ||
        fronts_presented_.count(address)) {
      return false;
    }
    const uint64_t pixels = uint64_t(copy.extent.width) * copy.extent.height;
    if (perf_alias_ && !perf_alias_off_) {
      bo2zm::native::v046::CopyFacts now;
      now.source_image = uint64_t(target.image);
      now.source_epoch = perf_epoch_;
      now.src_x = copy.srcOffset.x;
      now.src_y = copy.srcOffset.y;
      now.width = copy.extent.width;
      now.height = copy.extent.height;
      now.format = uint32_t(target.format);
      now.dest_width = resolved.image.width;
      now.dest_height = resolved.image.height;
      now.whole_dest = true;
      now.dest_address = address;
      const uint32_t diff = bo2zm::native::v046::ResolveDifference(perf_last_copy_, now);
      for (uint32_t r = 0; r < bo2zm::native::v046::kResolveDiffReasons; ++r) {
        if (diff & (1u << r)) {
          bo2zm::native::v046::g_alias_diff[r].fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (bo2zm::native::v046::SameResolve(perf_last_copy_, now)) {
        const auto twin = resolved_.find(perf_last_copy_.dest_address);
        if (twin != resolved_.end() && twin->second.image.image == perf_last_texture_ &&
            twin->second.image.prepared && twin->second.image.format == resolved.image.format &&
            twin->second.swap_rb == resolved.swap_rb && twin->second.image.swap_rb == resolved.image.swap_rb &&
            !perf_alias_live_.count(perf_last_copy_.dest_address) &&
            !fronts_presented_.count(perf_last_copy_.dest_address)) {
          PerfAlias& alias = perf_alias_live_[address];
          alias.of = perf_last_copy_.dest_address;
          alias.of_image = twin->second.image.image;
          alias.self_image = resolved.image.image;
          alias.width = resolved.image.width;
          alias.height = resolved.image.height;
          alias.request = perf_request_;
          alias.twin_request = perf_last_request_;
          ResolvedWritten(address);
          if (draws_) {
            draws_->InvalidateTexturesIn(address);
          }
          bo2zm::native::v046::g_alias_served.fetch_add(1, std::memory_order_relaxed);
          bo2zm::native::v046::g_pixels_not_copied.fetch_add(pixels, std::memory_order_relaxed);
          return true;
        }
        bo2zm::native::v046::g_alias_rejected.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (perf_lazy_ && !perf_lazy_off_ && learned_unread && target.format == kFormatColor) {
      PerfLazy& pending = perf_lazy_pending_[address];
      pending.target = &target;
      pending.source_vk = target.image;
      pending.texture_vk = resolved.image.image;
      pending.copy = copy;
      pending.request = perf_request_;
      perf_lazy_expired_.erase(address);
      ResolvedWritten(address);
      bo2zm::native::v046::g_lazy_deferred.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }


  void PerfNoteCopy(uint32_t address, const OwnedImage& target, const Resolved& resolved, const VkImageCopy& copy) {
    if (!perf_alias_) {
      return;
    }
    perf_last_copy_ = {};
    perf_last_copy_.source_image = uint64_t(target.image);
    perf_last_copy_.source_epoch = perf_epoch_;
    perf_last_copy_.src_x = copy.srcOffset.x;
    perf_last_copy_.src_y = copy.srcOffset.y;
    perf_last_copy_.width = copy.extent.width;
    perf_last_copy_.height = copy.extent.height;
    perf_last_copy_.format = uint32_t(target.format);
    perf_last_copy_.dest_width = resolved.image.width;
    perf_last_copy_.dest_height = resolved.image.height;
    perf_last_copy_.whole_dest = copy.dstOffset.x == 0 && copy.dstOffset.y == 0 &&
                                 copy.extent.width == resolved.image.width &&
                                 copy.extent.height == resolved.image.height;
    perf_last_copy_.dest_address = address;
    perf_last_texture_ = resolved.image.image;
    perf_last_request_ = perf_request_;
  }

  void PerfReport() {
    namespace p = bo2zm::native::v046;
    const uint64_t presented = presented_ - perf_report_presented_;
    perf_report_presented_ = presented_;
    perf_report_presented_frames_ = presented;
    REXLOG_INFO("[perf] 0.46, last {} frames: fused depth-only/stencil-only clears {}/{}; fronts dropped {} kept {}; "
                "lazy resolves deferred {} recorded {} dropped {} mismatches {}; aliases served {} handed over by "
                "image swap {} materialized by copy {} replaced {} rejected {} mismatches {}; {:.2f} Mpixels per "
                "frame not copied; switches: fuse {} fronts {} lazy {} alias {}",
                presented, p::g_fused_depth_only.exchange(0), p::g_fused_stencil_only.exchange(0),
                p::g_fronts_dropped.exchange(0), p::g_fronts_kept.exchange(0), p::g_lazy_deferred.exchange(0),
                p::g_lazy_recorded.exchange(0), p::g_lazy_dropped.exchange(0), p::g_lazy_mismatch.exchange(0),
                p::g_alias_served.exchange(0), p::g_alias_swapped.exchange(0), p::g_alias_materialized.exchange(0),
                p::g_alias_dropped.exchange(0),
                p::g_alias_rejected.exchange(0), p::g_alias_mismatch.exchange(0),
                presented ? double(p::g_pixels_not_copied.exchange(0)) / 1e6 / double(presented) : 0.0,
                perf_fuse_ ? "on" : "off", perf_drop_fronts_ ? (front_off_ ? "OFF(mismatch)" : "on") : "off",
                perf_lazy_ ? (perf_lazy_off_ ? "OFF(mismatch)" : "on") : "off",
                perf_alias_ ? (perf_alias_off_ ? "OFF(mismatch)" : "on") : "off");
    {
      namespace v = bo2zm::native::v046;
      const auto sample = [](uint32_t k) {
        const uint64_t x = v::g_clear_miss_sample[k].load(std::memory_order_relaxed);
        return fmt::format("{}x{} vs {}x{}", x >> 48, (x >> 32) & 0xFFFF, (x >> 16) & 0xFFFF, x & 0xFFFF);
      };
      {
        namespace e = bo2zm::native::edram;
        const double frames = perf_report_presented_frames_ ? double(perf_report_presented_frames_) : 1.0;
        REXLOG_INFO("[perf] 0.47 EDRAM traffic per frame (estimate, render area x texel size): loaded {:.1f} MB, "
                    "stored {:.1f} MB, store skipped {:.1f} MB ({} stores); results later needed {} / cleared unread "
                    "{}; mismatches {}; switch {}",
                    double(e::g_bytes_loaded.exchange(0)) / 1e6 / frames,
                    double(e::g_bytes_stored.exchange(0)) / 1e6 / frames,
                    double(e::g_bytes_not_stored.exchange(0)) / 1e6 / frames, e::g_discards.exchange(0),
                    e::g_live_seen.exchange(0), e::g_dead_seen.exchange(0), e::g_mismatches.exchange(0),
                    e::g_off.load() ? "OFF(mismatch)" : (e::g_enabled.load() ? "on" : "off"));
      }
      {

        namespace w = bo2zm::native::v055;
        const double frames = perf_report_presented_frames_ ? double(perf_report_presented_frames_) : 1.0;
        const int state = w::g_readonly_state.load(std::memory_order_relaxed);
        REXLOG_INFO("[perf] 0.55 per frame: read-only depth passes {:.2f} of {:.2f} candidates, {:.2f} MB of "
                    "depth-stencil not written back (reopened for a write {}, kinds banned {}; {}); converted "
                    "resolves without a load {:.2f} ({:.2f} MB not read); separate guest clears as loadOp {:.2f} "
                    "({:.2f} MB not read)",
                    double(w::g_readonly_passes.exchange(0)) / frames,
                    double(w::g_readonly_candidates.exchange(0)) / frames,
                    double(w::g_readonly_texels.exchange(0)) * 4.0 / 1e6 / frames, w::g_readonly_misses.exchange(0),
                    w::g_readonly_banned.load(std::memory_order_relaxed),
                    state == 1 ? "on" : state == 2 ? "no STORE_OP_NONE on this device" : "off",
                    double(w::g_converted_no_load.exchange(0)) / frames,
                    double(w::g_converted_texels.exchange(0)) * 4.0 / 1e6 / frames,
                    double(w::g_separate_clears_as_load.exchange(0)) / frames,
                    double(w::g_separate_clear_texels.exchange(0)) * 4.0 / 1e6 / frames);
      }
      REXLOG_INFO("[perf] 0.46 fitted passes {} (clears taken as loadOp {}), {:.2f} Mtexels per frame not loaded, "
                  "draws clipped {} (each bans its render target)",
                  v::g_fit_passes.exchange(0), v::g_fit_fused.exchange(0),
                  perf_report_presented_frames_ ? double(v::g_fit_texels_saved.exchange(0)) / 1e6 /
                                                      double(perf_report_presented_frames_)
                                                : 0.0,
                  v::g_fit_clipped.exchange(0));
      REXLOG_INFO("[perf] 0.46 guest clears not taken as loadOp = CLEAR: rectangle not at the origin {} ({}), "
                  "narrower than the area {} ({}), shorter than the area {} ({}), one aspect only {} ({}); whole-image "
                  "clear on a smaller pass {} (image vs pass {}); other {}",
                  v::g_clear_miss[v::kMissRectOrigin].exchange(0), sample(v::kMissRectOrigin),
                  v::g_clear_miss[v::kMissRectWidth].exchange(0), sample(v::kMissRectWidth),
                  v::g_clear_miss[v::kMissRectShort].exchange(0), sample(v::kMissRectShort),
                  v::g_clear_miss[v::kMissRectAspects].exchange(0), sample(v::kMissRectAspects),
                  v::g_clear_miss[v::kMissWholeSize].exchange(0), sample(v::kMissWholeSize),
                  v::g_clear_miss[v::kMissOther].exchange(0));
    }
    if (perf_alias_) {
      namespace v = bo2zm::native::v046;
      REXLOG_INFO("[perf] 0.46 alias candidates not equal to the previous resolve, by reason: no previous {}, other "
                  "target {}, something drawn/cleared in between {}, other rectangle {}, other size {}, other format "
                  "{}, other texture size {}, not whole {}, same address {}",
                  v::g_alias_diff[0].exchange(0), v::g_alias_diff[1].exchange(0), v::g_alias_diff[2].exchange(0),
                  v::g_alias_diff[3].exchange(0), v::g_alias_diff[4].exchange(0), v::g_alias_diff[5].exchange(0),
                  v::g_alias_diff[6].exchange(0), v::g_alias_diff[7].exchange(0), v::g_alias_diff[8].exchange(0));
    }
  }


  void PerfAbTick() {
    if (perf_ab_s_ <= 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (perf_ab_window_start_ == std::chrono::steady_clock::time_point{}) {
      perf_ab_window_start_ = now;
      perf_ab_presented0_ = presented_;
      perf_ab_gpu0_ = gpu_ns_;
      perf_ab_on_ = false;
      PerfAbApply(false);
      return;
    }
    const double seconds = std::chrono::duration<double>(now - perf_ab_window_start_).count();
    if (seconds < double(perf_ab_s_)) {
      return;
    }
    const uint64_t swaps = presented_ - perf_ab_presented0_;
    const uint64_t gpu = gpu_ns_ - perf_ab_gpu0_;
    PerfAbArm& arm = perf_ab_arms_[perf_ab_on_ ? 1 : 0];
    ++perf_ab_windows_;
    if (perf_ab_windows_ > 2) {
      arm.seconds += seconds;
      arm.swaps += swaps;
      arm.gpu_ns += gpu;
      const double fps = seconds > 0 ? double(swaps) / seconds : 0.0;
      ++arm.windows;
      arm.fps_sum += fps;
      arm.fps_sq += fps * fps;
    }
    const auto spread = [](const PerfAbArm& a) {
      if (a.windows < 2) return 0.0;
      const double mean = a.fps_sum / a.windows;
      return std::sqrt(std::max(0.0, (a.fps_sq - a.windows * mean * mean) / (a.windows - 1)));
    };
    const auto avg = [](const PerfAbArm& a, bool fps) {
      return fps ? (a.seconds > 0 ? double(a.swaps) / a.seconds : 0.0)
                 : (a.swaps ? double(a.gpu_ns) / 1e6 / double(a.swaps) : 0.0);
    };
    REXLOG_INFO("[perf-ab] window {} {}: {:.1f} swaps/s, GPU {:.2f} ms per swap | running averages: OFF {:.2f} swaps/s "
                "{:.2f} ms GPU ({:.0f} s, {} windows, sd {:.2f}), ON {:.2f} swaps/s {:.2f} ms GPU ({:.0f} s, {} windows, "
                "sd {:.2f})",
                perf_ab_windows_, perf_ab_on_ ? "ON " : "OFF", seconds > 0 ? double(swaps) / seconds : 0.0,
                swaps ? double(gpu) / 1e6 / double(swaps) : 0.0, avg(perf_ab_arms_[0], true),
                avg(perf_ab_arms_[0], false), perf_ab_arms_[0].seconds, perf_ab_arms_[0].windows,
                spread(perf_ab_arms_[0]), avg(perf_ab_arms_[1], true), avg(perf_ab_arms_[1], false),
                perf_ab_arms_[1].seconds, perf_ab_arms_[1].windows, spread(perf_ab_arms_[1]));
    perf_ab_on_ = !perf_ab_on_;
    PerfAbApply(perf_ab_on_);
    perf_ab_window_start_ = now;
    perf_ab_presented0_ = presented_;
    perf_ab_gpu0_ = gpu_ns_;
  }

  void PerfAbApply(bool on) {
    perf_drop_fronts_ = perf_base_drop_fronts_ && on;
    perf_lazy_ = perf_base_lazy_ && on;
    bo2zm::native::v046::g_fit_enabled.store(perf_base_fit_ && on, std::memory_order_relaxed);
    bo2zm::native::edram::g_enabled.store(perf_base_edram_ && on, std::memory_order_relaxed);
    if (perf_alias_ && perf_base_alias_ && !on) {
      perf_alias_ = false;
      PerfSettleAll();
    } else {
      perf_alias_ = perf_base_alias_ && on;
    }
    if (!perf_lazy_ && !perf_lazy_pending_.empty()) {
      PerfSettleAll();
    }
    if (perf_base_wait_unlocked_) {
      rex::cvar::SetFlagByName("present_wait_paint_unlocked", on ? "true" : "false");
    }
  }

  void BeforeWriteColor(const OwnedImage& target) {
    ++perf_epoch_;
    if (!perf_lazy_pending_.empty() && target.image != VK_NULL_HANDLE) {
      PerfLazySourceWritten(target.image);
    }
    if (fronts_pending_.empty() || target.image == VK_NULL_HANDLE) {
      return;
    }
    for (auto it = fronts_pending_.begin(); it != fronts_pending_.end();) {
      if (it->second.retained >= 0 || it->second.source_vk != target.image) {
        ++it;
        continue;
      }
      const uint32_t address = it->first;
      ++it;
      if (PerfDropFront(address)) {
        continue;
      }
      RecordCopyFront(address);
      ++front_copied_write_;
    }
  }


  void RotateFrontBeforeClear(OwnedImage& target) {
    ++perf_epoch_;
    if (!perf_lazy_pending_.empty() && target.image != VK_NULL_HANDLE) {
      PerfLazySourceWritten(target.image);
    }
    if (fronts_pending_.empty() || target.image == VK_NULL_HANDLE) {
      return;
    }
    uint32_t base = 0;
    uint32_t how_many = 0;
    for (const auto& [address, pending] : fronts_pending_) {
      if (pending.retained < 0 && pending.source_vk == target.image) {
        base = address;
        ++how_many;
      }
    }
    if (how_many == 0) {
      return;
    }
    int32_t free = -1;
    if (how_many == 1) {
      for (size_t i = 0; i < fronts_images_.size(); ++i) {
        const OwnedImage& candidate = fronts_images_[i].image;
        if (fronts_images_[i].retained_per == 0 && candidate.image != VK_NULL_HANDLE &&
            candidate.width == target.width && candidate.height == target.height &&
            candidate.format == target.format) {
          free = int32_t(i);
          break;
        }
      }
      if (free < 0 && fronts_images_.size() < kFrontImagesMax) {
        FrontImage fresh;

        if (Create(fresh.image, target.width, target.height,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  target.format)) {
          Prepare(fresh.image);
          fronts_images_.push_back(fresh);
          free = int32_t(fronts_images_.size()) - 1;
          REXLOG_INFO("[native] C2 lazy front buffer: spare image {} of {}x{} for the front buffer's target",
                      fronts_images_.size(), target.width, target.height);
        }
      }
    }
    if (free < 0) {
      ++front_no_spare_;
      BeforeWriteColor(target);
      return;
    }
    OwnedImage& spare = fronts_images_[size_t(free)].image;
    std::swap(target.image, spare.image);
    std::swap(target.guest_memory, spare.guest_memory);
    std::swap(target.view, spare.view);
    std::swap(target.prepared, spare.prepared);
    fronts_images_[size_t(free)].retained_per = base;
    fronts_pending_[base].retained = free;
    if (draws_) {
      draws_->InvalidateImages(target.image, spare.image);
    }
    ++front_rotations_;
  }


  void AnnotateReadFront(uint32_t address) {
    fronts_read_[address] = presented_;
    if (perf_drop_fronts_) {
      perf_front_learn_[address].OnSampled();
    }
    front_composition_sampled_.insert(address);
    ForgetFrontComposition(address, "sampled");
    if (!fronts_pending_.empty() && fronts_pending_.count(address)) {
      RecordCopyFront(address);
      ++front_copied_read_;
    }
    if (!fronts_expired_.empty() && fronts_expired_.erase(address)) {
      ++front_reads_late_;
      if (!front_off_) {
        front_off_ = true;
        REXLOG_ERROR("[native] C2 lazy front buffer: MISMATCH, a draw samples the front buffer {:08X} without its last "
                     "copy. Off for the rest of the session: always copied",
                     address);
      }
    }
  }


  OwnedImage* FrontOnPresent(uint32_t address, uint32_t width, uint32_t height) {
    if (FrontTraceLine()) {
      REXLOG_INFO("[front-trace] swap front {:08X} {}x{} frame {} owners {}",
                  address, width, height, presented_, front_compositions_.size());
    }
    fronts_presented_[address] = presented_;
    if (perf_alias_live_.count(address) || perf_lazy_pending_.count(address)) {
      PerfSettleBeforeOtherWrite(address);
    }
    if (perf_drop_fronts_) {
      perf_front_learn_[address].OnSwap();
    }
    if (!fronts_expired_.empty() && fronts_expired_.erase(address)) {
      ++front_reads_late_;
      if (!front_off_) {
        front_off_ = true;
        REXLOG_ERROR("[native] C2 lazy front buffer: MISMATCH, the Swap paints the front buffer {:08X} without its last "
                     "copy. Off for the rest of the session: always copied",
                     address);
      }
    }
    if (fronts_pending_.empty()) {
      return nullptr;
    }
    if (REXCVAR_GET(bo2zm_native_diag_resolved)) {

      while (!fronts_pending_.empty()) {
        RecordCopyFront(fronts_pending_.begin()->first);
        ++front_copied_swap_;
      }
      return nullptr;
    }
    const auto p = fronts_pending_.find(address);
    if (p == fronts_pending_.end()) {
      return nullptr;
    }
    const auto r = resolved_.find(address);
    OwnedImage* const source = SourceFront(p->second);
    bool can = source && r != resolved_.end() && r->second.image.image == p->second.texture_vk &&
                 r->second.image.prepared && ramp_gamma_ && !bo2zm::settings::AntialiasingFxaa() &&
                 !front_off_;
    if (can) {


      const OwnedImage& texture = r->second.image;
      uint32_t w = 0, h = 0;
      PresentSizeOf(texture, width, height, w, h);
      can = w == texture.width && h == texture.height && p->second.copy.extent.width == w &&
              p->second.copy.extent.height == h;
      if (!can && IsScaled(texture)) {

        display::g_err_lazy_front_fallback.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (!can) {
      RecordCopyFront(address);
      ++front_copied_swap_;
      return nullptr;
    }
    if (p->second.retained >= 0) {
      ++front_painted_retained_;
    } else {
      ++front_painted_target_;
    }


    if (draws_) {
      draws_->FinishPass();
    }
    EdramRead(source->image, "the Swap painting from the render target");
    return source;
  }


  bool SubmitYWait() override { return SubmitJob(false) && Record(); }


  bool WaitGpuIdle() override {
    WaitGpu();
    return Record();
  }

  void MarkGpu(uint32_t category) override {
    if (category != kGpuCopies) {
      ++perf_epoch_;
    }
    if (queries_ == VK_NULL_HANDLE || !recording_) {
      return;
    }
    WorkSlot& slot = slots_[slot_];
    if (slot.pass_profile.active != pass_profile::kNone && slot.categories.size() + 2 >= kMarksPerSlot) return;
    timing::Category(write_mark_, commands_job_, queries_, slot_ * kMarksPerSlot,
                     slot.categories, uint8_t(category), slot.marks_categories, slot.marks_precise);
  }

  void ProfilePassBegin(uint32_t category, uint32_t width, uint32_t height,
                        uint64_t target, const uint32_t* formats, uint32_t load_mode) override {
    if (!recording_) return;
    WorkSlot& slot = slots_[slot_];
    slot.shader_target=shader_probe::Target(target,slot.shader_pass_ordinal++,width,height,{formats,5});
    if(slot.shader_target && pixel_probe_frame_.trial && pixel_probe_frame_.kind==pixel_probe::Kind::kShader)
      ++pixel_probe_counts_.target_passes;
    if(queries_==VK_NULL_HANDLE)return;
    if(slot.shader_target && slot.pixel_probe_tag.trial && slot.pixel_probe_tag.kind==pixel_probe::Kind::kShader) {
      slot.shader_target_seen=true;slot.shader_mark_begin=uint32_t(slot.categories.size());
      timing::Category(write_mark_,commands_job_,queries_,slot_*kMarksPerSlot,slot.categories,uint8_t(category),true,true,true);
    }
    pass_profile::Meta meta{category, width, height, 0, target};
    std::copy(formats, formats + 5, meta.formats.begin());
    meta.load = load_mode;
    if (slot.pass_profile.Begin(meta, uint32_t(slot.categories.size())))
      timing::Category(write_mark_, commands_job_, queries_, slot_ * kMarksPerSlot,
          slot.categories, uint8_t(category), true, true, true);
  }
  void ProfilePassEnd() override {
    if (!recording_ || queries_ == VK_NULL_HANDLE) return;
    WorkSlot& slot = slots_[slot_];
    if(slot.shader_target && slot.pixel_probe_tag.trial && slot.pixel_probe_tag.kind==pixel_probe::Kind::kShader) {
      slot.shader_mark_end=uint32_t(slot.categories.size());
      timing::Category(write_mark_,commands_job_,queries_,slot_*kMarksPerSlot,slot.categories,uint8_t(kGpuOthers),true,true,true);
    }
    slot.shader_target=false;
    if (slot.pass_profile.End(uint32_t(slot.categories.size()))) {
      timing::Category(write_mark_, commands_job_, queries_, slot_ * kMarksPerSlot,
          slot.categories, uint8_t(kGpuOthers), true, true, true);

      if (pass_profile::serialize.load(std::memory_order_relaxed) && slot.pass_profile.sampled) {
        VkMemoryBarrier all{};
        all.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        all.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        all.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        dfn_.vkCmdPipelineBarrier(commands_job_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &all, 0, nullptr, 0, nullptr);
      }
    }
  }
  void ProfilePassDraw(uint32_t vs, uint32_t ps, uint32_t elements) override {
    slots_[slot_].pass_profile.Draw(vs, ps, elements);
  }


  void BeginOcclusion(uint32_t base) override {
    if (occlusion_open_ != 0) {
      uint64_t discarded = 0;
      FinishOcclusion(0, discarded);
    }
    if (!draws_ || occlusions_ == VK_NULL_HANDLE) {
      return;
    }
    if (occlusions_game_.size() >= 256) {
      occlusions_game_.clear();
    }
    occlusion_open_ = next_occlusion_++;
    occlusions_game_[occlusion_open_].base = base;
    draws_->OcclusionOpen(true);
  }

  bool FinishOcclusion(uint32_t base, uint64_t& samples) override {
    if (occlusion_open_ != 0) {
      if (draws_) {
        draws_->OcclusionOpen(false);
      }
      const auto it = occlusions_game_.find(occlusion_open_);
      occlusion_open_ = 0;
      if (it != occlusions_game_.end()) {
        it->second.finished = true;
        if (it->second.spans_pending == 0) {
          PublishOcclusion(it);
        }
      }
    }
    const auto measure = occlusion_per_base_.find(base);
    if (measure == occlusion_per_base_.end()) {
      return false;
    }
    samples = measure->second;
    return true;
  }

  void StatsOcclusion(uint64_t values[5]) const override {
    std::copy(std::begin(stats_occlusion_), std::end(stats_occlusion_), values);
  }

  uint32_t BeginQueryOcclusion() override {
    if (occlusions_ == VK_NULL_HANDLE || occlusion_open_ == 0 || !recording_) {
      return UINT32_MAX;
    }
    const auto it = occlusions_game_.find(occlusion_open_);
    if (it == occlusions_game_.end()) {
      return UINT32_MAX;
    }
    WorkSlot& slot = slots_[slot_];
    if (slot.occlusions.size() >= kOcclusionsPerSlot) {
      ++stats_occlusion_[1];
      it->second.failed = true;
      return UINT32_MAX;
    }
    const uint32_t index = slot_ * kOcclusionsPerSlot + uint32_t(slot.occlusions.size());
    dfn_.vkCmdBeginQuery(commands_job_, occlusions_, index,
                         occlusion_precise_ ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    slot.occlusions.emplace_back(index, occlusion_open_);
    ++it->second.spans_pending;
    ++stats_occlusion_[0];
    return index;
  }

  void FinishQueryOcclusion(uint32_t index) override {
    if (occlusions_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_job_, occlusions_, index);
    }
  }


  uint32_t BeginQueryVisibility(bool witness) override {
    if (visibility_ == VK_NULL_HANDLE || !recording_) {
      return UINT32_MAX;
    }
    WorkSlot& slot = slots_[slot_];
    if (slot.visibility.size() >= kVisibilityPerSlot) {
      ++visibility_no_room_;
      return UINT32_MAX;
    }
    const uint32_t index = slot_ * kVisibilityPerSlot + uint32_t(slot.visibility.size());
    dfn_.vkCmdBeginQuery(commands_job_, visibility_, index, 0);
    slot.visibility.emplace_back(index, witness ? kVisibilityWitness : kVisibilityWater);
    return index;
  }

  void FinishQueryVisibility(uint32_t index) override {
    if (visibility_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_job_, visibility_, index);
    }
  }


  uint32_t BeginStats(uint32_t category) override {


    if (stats_ == VK_NULL_HANDLE || !recording_ ||
        !REXCVAR_GET(bo2zm_native_pipeline_stats) ||
        REXCVAR_GET(bo2zm_native_per_draw_stats_s) > 0) {
      return UINT32_MAX;
    }
    WorkSlot& slot = slots_[slot_];
    if (slot.stats.size() >= kStatsPerSlot) {
      ++stats_no_room_;
      if (stats_no_room_ % 200 == 1) {
        REXLOG_WARN("[native] C2: no room for pass statistics ({} times): the job has "
                    "more than {} passes", stats_no_room_, kStatsPerSlot);
      }
      return UINT32_MAX;
    }
    const uint32_t index =
        slot_ * kStatsPerSlot + uint32_t(slot.stats.size());
    dfn_.vkCmdBeginQuery(commands_job_, stats_, index, 0);
    slot.stats.emplace_back(index, uint8_t(category));
    return index;
  }

  void FinishStats(uint32_t index) override {
    if (stats_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_job_, stats_, index);
    }
  }


  uint32_t BeginStatsDraw(uint32_t label, uint32_t category) override {
    if (stats_draw_ == VK_NULL_HANDLE || !recording_ || !window_diagnostic_) {
      return UINT32_MAX;
    }
    WorkSlot& slot = slots_[slot_];
    if (slot.stats_draw.size() >= kStatsDrawPerSlot) {
      ++stats_draw_no_room_;
      if (stats_draw_no_room_ % 2000 == 1) {
        REXLOG_WARN("[native] C2: no room for per-draw statistics ({} times)",
                    stats_draw_no_room_);
      }
      return UINT32_MAX;
    }
    const uint32_t index = slot_ * kStatsDrawPerSlot +
                            uint32_t(slot.stats_draw.size());
    dfn_.vkCmdBeginQuery(commands_job_, stats_draw_, index, 0);
    slot.stats_draw.emplace_back(
        index, uint16_t((category % kGpuCategories) * kLabelsShader + label % kLabelsShader));
    return index;
  }

  void FinishStatsDraw(uint32_t index) override {
    if (stats_draw_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_job_, stats_draw_, index);
    }
  }

 private:
  bool Reject(uint32_t cause, const char* text) {
    ++rejections_;
    if (warned_.insert(cause).second) {
      REXLOG_WARN("[native] C2: {} (cause {})", text, cause);
    }
    return false;
  }


  bool Rectangle(const CopyRegisters& reg, uint32_t pitch, int32_t& x0, int32_t& y0,
                  int32_t& x1, int32_t& y1) {
    const uint32_t type = reg.fetch_vertices[0] & 0x3;
    const uint32_t address = reg.fetch_vertices[0] >> 2;
    const auto order = static_cast<xenos::Endian>(reg.fetch_vertices[1] & 0x3);
    const uint32_t size = (reg.fetch_vertices[1] >> 2) & 0xFFFFFF;
    if (type != uint32_t(xenos::FetchConstantType::kVertex) || size != 3 * 2) {
      return Reject(5, "copy vertices in an unsupported format");
    }
    const uint8_t* vertices = memory_->TranslatePhysical(address * 4);
    const float mid_pixel =
        (reg.pa_su_vtx_cntl & 0x1) == uint32_t(xenos::PixelCenter::kD3DZero) ? 0.5f : 0.0f;
    int32_t fixed[6];
    for (int i = 0; i < 6; ++i) {
      float value;
      std::memcpy(&value, vertices + i * 4, sizeof(value));
      fixed[i] = Fixed16p8(xenos::GpuSwap(value, order) + mid_pixel);
    }
    x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
    y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
    x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
    y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;

    const int32_t offset_x = ExtenderSign15(reg.pa_sc_window_offset & 0x7FFF);
    const int32_t offset_y = ExtenderSign15((reg.pa_sc_window_offset >> 16) & 0x7FFF);
    if ((reg.pa_su_sc_mode_cntl >> 16) & 0x1) {
      x0 += offset_x;
      y0 += offset_y;
      x1 += offset_x;
      y1 += offset_y;
    }

    int32_t left = int32_t(reg.pa_sc_window_scissor_tl & 0x3FFF);
    int32_t up = int32_t((reg.pa_sc_window_scissor_tl >> 16) & 0x3FFF);
    int32_t right = int32_t(reg.pa_sc_window_scissor_br & 0x3FFF);
    int32_t down = int32_t((reg.pa_sc_window_scissor_br >> 16) & 0x3FFF);
    if (!((reg.pa_sc_window_scissor_tl >> 31) & 0x1)) {
      left += offset_x;
      up += offset_y;
      right += offset_x;
      down += offset_y;
    }
    left = std::max(left, 0);
    up = std::max(up, 0);
    right = std::max(right, left);
    down = std::max(down, up);
    x0 = std::clamp(x0, left, right);
    y0 = std::clamp(y0, up, down);
    x1 = std::clamp(x1, left, right);
    y1 = std::clamp(y1, up, down);

    x0 &= ~int32_t(7);
    y0 &= ~int32_t(7);
    x1 = (x1 + 7) & ~int32_t(7);
    y1 = (y1 + 7) & ~int32_t(7);
    const int32_t pitch_aligned = int32_t(pitch & ~uint32_t(7));
    x0 = std::min(x0, pitch_aligned);
    x1 = std::min(x1, pitch_aligned);
    if (x0 >= x1 || y0 >= y1) {
      return Reject(6, "empty copy rectangle");
    }
    return true;
  }

  OwnedImage* GetTarget(uint32_t base, uint32_t format, uint32_t pitch) {


    format = bo2zm_formats::Storage(format);
    if (!pitch) {
      Reject(7, "render target with pitch 0");
      return nullptr;
    }
    const uint64_t key = (uint64_t(base) << 20) | (uint64_t(format) << 16) | pitch;
    auto it = targets_.find(key);
    if (it != targets_.end()) {
      return &it->second;
    }


    const uint32_t height = HeightTarget(pitch);


    uint32_t width_image = pitch, height_image = height;
    const uint32_t scale_class = ScaleClassOf(pitch);
    const bool scaled = scale_class != display::kClassNone;
    if (scaled) {
      width_image = ScaledSideX(pitch, scale_class);
      height_image = ScaledSideY(height, scale_class);
    }
    OwnedImage image;
    if (!Create(image, width_image, height_image,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               FormatHostTarget(format))) {
      if (scaled) display::g_err_target_create.fetch_add(1, std::memory_order_relaxed);
      Reject(8, "could not create a render target");
      return nullptr;
    }
    if (scaled) {
      image.width_guest = pitch;
      image.height_guest = height;
      image.scale_class = uint8_t(scale_class);
      bo2zm::native::performance::g_scaled_targets.fetch_add(1, std::memory_order_relaxed);
      (scale_class == display::kClassScene ? display::g_scene_color : display::g_composite_color)
          .Set(width_image, height_image);
      REXLOG_INFO("[native] C2: render target base {:03X} ({}) drawn at {}x{} (the game asks for {}x{})", base,
                  scale_class == display::kClassScene ? "scene" : "composite", width_image, height_image, pitch,
                  height);
    }
    const VkClearColorValue diag = ColorDiagnostic(base, format, pitch);
    REXLOG_INFO("[native] C2: render target base {:03X}, format {}, {}x{} (diagnostic color {:02X}{:02X}{:02X})",
                base, format, pitch, height, uint32_t(std::lround(diag.float32[0] * 255.0f)),
                uint32_t(std::lround(diag.float32[1] * 255.0f)),
                uint32_t(std::lround(diag.float32[2] * 255.0f)));
    return &targets_.emplace(key, image).first->second;
  }

  DepthTarget* GetDepth(uint32_t base, uint32_t format, uint32_t pitch) {
    if (!pitch || format_depth_ == VK_FORMAT_UNDEFINED) {
      Reject(11, "no depth target (pitch 0 or format not available)");
      return nullptr;
    }
    const uint64_t key = (uint64_t(base) << 20) | (uint64_t(format) << 16) | pitch;
    auto it = depths_.find(key);
    if (it != depths_.end()) {
      return &it->second;
    }
    const uint32_t height = HeightTarget(pitch);


    uint32_t width_image = pitch, height_image = height;
    uint32_t width_guest = 0, height_guest = 0;
    uint32_t scale_class_depth = display::kClassNone;
    if (pitch == kSideShadows && height == kSideShadows) {
      if (scale_shadows_ == 0) {
        scale_shadows_ = uint32_t(std::clamp(REXCVAR_GET(bo2zm_native_shadow_scale), 50, 100));
        if (!blit_ || !depth_scalable_) {
          scale_shadows_ = 100;
        }
      }
      if (scale_shadows_ < 100) {
        width_image = std::max<uint32_t>(64, ((pitch * scale_shadows_ / 100) + 15) & ~15u);
        height_image = std::max<uint32_t>(64, ((height * scale_shadows_ / 100) + 15) & ~15u);
        width_guest = pitch;
        height_guest = height;
        REXLOG_INFO("[native] C2: shadow map at {} %: drawn and resolved at {}x{} (the game asks for {}x{})",
                    scale_shadows_, width_image, height_image, pitch, height);
      }
    } else if (const uint32_t depth_class = ScaleClassOf(pitch); depth_class != display::kClassNone) {


      width_image = ScaledSideX(pitch, depth_class);
      height_image = ScaledSideY(height, depth_class);
      width_guest = pitch;
      height_guest = height;
      scale_class_depth = depth_class;
      bo2zm::native::performance::g_scaled_targets.fetch_add(1, std::memory_order_relaxed);
      (depth_class == display::kClassScene ? display::g_scene_depth : display::g_composite_depth)
          .Set(width_image, height_image);
      REXLOG_INFO("[native] C2: depth target base {:03X} ({}) drawn at {}x{} (the game asks for {}x{})", base,
                  depth_class == display::kClassScene ? "scene" : "composite", width_image, height_image, pitch,
                  height);
    }


    const bool zcull = REXCVAR_GET(bo2zm_native_zcull);
    const bool is_shadows_map = (pitch == kSideShadows && height == kSideShadows);
    const bool with_transfer_dst = is_shadows_map || !zcull;
    VkImageUsageFlags use_depth = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
    if (with_transfer_dst) {
      use_depth |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    DepthTarget image;


    if (!Create(image, width_image, height_image, use_depth, format_depth_)) {
      if (scale_class_depth != display::kClassNone) display::g_err_target_create.fetch_add(1, std::memory_order_relaxed);
      Reject(12, "could not create a depth target");
      return nullptr;
    }
    image.width_guest = width_guest;
    image.height_guest = height_guest;
    image.scale_class = uint8_t(scale_class_depth);
    image.accepts_copy_target = with_transfer_dst;
    REXLOG_INFO("[native] C2: depth target base {:03X}, format {}, {}x{}", base,
                format, pitch, height);
    return &depths_.emplace(key, image).first->second;
  }


  Resolved* GetResolved(uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                            bool swap_rb, VkFormat format_host = kFormatColor, uint32_t width_guest = 0,
                            uint32_t height_guest = 0) {
    if (!width || !height) {
      Reject(9, "copy to a texture of size 0");
      return nullptr;
    }
    auto it = resolved_.find(base);
    if (it != resolved_.end()) {
      if (it->second.image.width == width && it->second.image.height == height &&
          it->second.image.format == format_host && it->second.image.width_guest == width_guest &&
          it->second.image.height_guest == height_guest) {
        if (draws_ && (it->second.format_guest != format || it->second.swap_rb != swap_rb)) {
          draws_->InvalidateTexturesIn(base);
        }
        it->second.format_guest = format;
        it->second.swap_rb = swap_rb;
        it->second.image.swap_rb = swap_rb;
        return &it->second;
      }
      ForgetFrontComposition(base, "reshape");
      front_composition_sampled_.erase(base);


      if (park_resolved_ && !lent_.count(base) && parked_.size() < 32) {
        const auto key = [base](uint32_t w, uint32_t h, VkFormat f) {
          return (uint64_t(base) << 32) ^ (uint64_t(w) << 20) ^ (uint64_t(h) << 6) ^ uint64_t(f);
        };
        Resolved old = it->second;
        const uint64_t key_old = key(old.image.width, old.image.height, old.image.format);
        const auto parked = parked_.find(key(width, height, format_host));
        if (parked != parked_.end()) {
          it->second = parked->second;
          parked_.erase(parked);
          parked_.emplace(key_old, old);
          it->second.image.width_guest = width_guest;
          it->second.image.height_guest = height_guest;
          it->second.format_guest = format;
          it->second.swap_rb = swap_rb;
          it->second.image.swap_rb = swap_rb;
          if (++resolved_unparked_ == 1 || resolved_unparked_ % 10000 == 0) {
            REXLOG_INFO("[native] C2: resolved {:08X} {}x{} taken from the parked ones ({} times, {} parked)", base,
                        width, height, resolved_unparked_, parked_.size());
          }
          if (draws_) {
            draws_->InvalidateTexturesIn(base);
          }
          ShadowMinResolvedWritten(base);
          return &it->second;
        }
        parked_.emplace(key_old, old);
        resolved_.erase(it);
        ShadowMinResolvedWritten(base);
      } else {

      SubmitJob(true);
      WaitGpu();
      if (draws_) {
        draws_->ForgetImage(it->second.image.image);
      }
      Destroy(it->second.image);
      resolved_.erase(it);

      if (!lent_.empty()) {
        lent_.erase(base);
      }
      ShadowMinResolvedWritten(base);
      }
    }
    Resolved resolved;


    const VkImageUsageFlags use_target = IsDepthFormat(format_host)
                                              ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                              : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!Create(resolved.image, width, height,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | use_target,
               format_host)) {
      Reject(10, "could not create a resolved texture");
      return nullptr;
    }
    resolved.format_guest = format;
    resolved.swap_rb = swap_rb;
    resolved.image.swap_rb = swap_rb;
    resolved.image.width_guest = width_guest;
    resolved.image.height_guest = height_guest;
    REXLOG_INFO("[native] C2: resolved texture at {:08X}, {}x{}, format {}{}", base, width, height,
                format, width_guest ? " (scaled scene)" : "");
    if (draws_) {
      draws_->InvalidateTexturesIn(base);
    }
    return &resolved_.emplace(base, resolved).first->second;
  }


  void AnnotateCopy(uint32_t width, uint32_t height) {
    const uint64_t pixels = uint64_t(width) * height;
    for (uint32_t c = 0; c < kBucketsCopy; ++c) {
      if (pixels <= kPixelsBucket[c]) {
        ++copies_bucket_[c];
        pixels_bucket_[c] += pixels;
        return;
      }
    }
  }


  bool ResolveNoCopy() {
    const int32_t alternate = REXCVAR_GET(bo2zm_native_resolve_without_copy_alternate_s);
    if (alternate <= 0) {
      return REXCVAR_GET(bo2zm_native_resolve_without_copy);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_)
                              .count();
    const bool no_copy = (seconds / alternate) % 2 == 1;
    if (no_copy != alternation_annotated_) {
      alternation_annotated_ = no_copy;
      REXLOG_INFO("[native] C2 resolve: {}", no_copy ? "no copy (swap)" : "copying");
    }
    return no_copy;
  }

  static bool IsDepthFormat(VkFormat format) {
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT ||
           format == VK_FORMAT_X8_D24_UNORM_PACK32;
  }


  bool SwapWithResolved(OwnedImage& target, Resolved& resolved, uint32_t base) {
    ++perf_epoch_;
    EdramRead(target.image, "a resolve by image swap");
    EdramRead(resolved.image.image, "a resolve by image swap");
    PerfSettleAll();
    if (target.width != resolved.image.width || target.height != resolved.image.height ||
        target.format != resolved.image.format) {
      return false;
    }
    FlushDeferredClear(target);


    if (!target.accepts_copy_target) {
      return false;
    }


    AnnotateUseClear(target, target.width, target.height);


    if (!pending_.empty()) {
      BeforeWriteDepth(target);
      if (pending_.erase(base)) {
        ++lazy_replaced_;
      }
    }


    if (!fronts_pending_.empty()) {
      BeforeWriteColor(target);
      ResolveFrontEarlier(base, true);
    }
    std::swap(target.image, resolved.image.image);
    std::swap(target.guest_memory, resolved.image.guest_memory);
    std::swap(target.view, resolved.image.view);
    std::swap(target.prepared, resolved.image.prepared);
    resolved.image.swap_rb = resolved.swap_rb;
    resolved.image.content_invalid = false;
    target.content_invalid = true;
    target.resolved_base = base;
    ++swaps_;
    ResolvedWritten(base);
    ForgetClear(target);
    if (draws_) {


      draws_->InvalidateImages(target.image, resolved.image.image);
    }
    return true;
  }


  void RestoreContent(OwnedImage& target, bool for_resolve = false) {
    ++perf_epoch_;
    EdramTouch(target.image);
    if (!perf_alias_live_.empty() || !perf_lazy_pending_.empty()) {
      PerfSettleAll();
    }
    if (!target.content_invalid) {
      return;
    }
    FlushDeferredClear(target);
    target.content_invalid = false;


    if (!pending_.empty()) {
      if (pending_.count(target.resolved_base)) {
        RecordCopyPending(target.resolved_base);
        ++lazy_copied_read_;
      }
      BeforeWriteDepth(target);
    }


    if (!fronts_pending_.empty()) {
      if (fronts_pending_.count(target.resolved_base)) {
        RecordCopyFront(target.resolved_base);
        ++front_copied_read_;
      }
      BeforeWriteColor(target);
    }
    auto it = resolved_.find(target.resolved_base);
    if (it == resolved_.end() || it->second.image.width != target.width ||
        it->second.image.height != target.height || it->second.image.format != target.format ||
        !copy_image_ || !Record()) {
      if (for_resolve) {
        ++resolve_no_source_;
      }
      return;
    }
    if (for_resolve) {

      CopyBackForResolve(target, it->second.image);
      return;
    }


    if (REXCVAR_GET(bo2zm_native_restore_by_swap)) {
      if (!target.accepts_copy_target) {
        ++no_swap_no_transfer_dst_;
      } else if (!target.prepared) {
        ++no_swap_target_no_prepare_;
      } else if (!it->second.image.prepared) {
        ++no_swap_resolved_no_prepare_;
      } else if (target.width != it->second.image.width ||
                 target.height != it->second.image.height) {


        ++no_swap_sizes_different_;
      }
    }
    if (REXCVAR_GET(bo2zm_native_restore_by_swap) && target.accepts_copy_target &&
        target.prepared && it->second.image.prepared) {
      const uint32_t base = target.resolved_base;
      std::swap(target.image, it->second.image.image);
      std::swap(target.guest_memory, it->second.image.guest_memory);
      std::swap(target.view, it->second.image.view);
      std::swap(target.prepared, it->second.image.prepared);
      it->second.image.content_invalid = false;
      if (draws_) {
        draws_->InvalidateImages(target.image, it->second.image.image);
      }
      lent_.insert(base);
      ++loans_;
      ++restores_;
      pixels_restore_saved_ += uint64_t(target.width) * target.height;
      ForgetClear(target);
      return;
    }


    if (ShadowMinInsteadOfRestore(target)) {
      return;
    }
    Prepare(it->second.image);
    Prepare(target);
    const VkImageAspectFlags aspect =
        IsDepthFormat(target.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;


    uint32_t height = target.height;
    if (REXCVAR_GET(bo2zm_native_restore_useful_area)) {
      const auto e = state_target_.find(&target);
      if (e != state_target_.end() && e->second.height_used &&
          e->second.height_used < target.height) {
        height = e->second.height_used;
        ++restores_clipped_;
        pixels_restore_saved_ += uint64_t(target.width) * (target.height - height);
      }
    }
    VkImageCopy copy{};
    copy.srcSubresource = {aspect, 0, 0, 1};
    copy.dstSubresource = {aspect, 0, 0, 1};
    copy.extent = {target.width, height, 1};
    AnnotateUseClear(target, target.width, height);
    MarkGpu(kGpuCopies);
    BeforeImageTransfer(commands_job_);
    copy_image_(commands_job_, it->second.image.image, VK_IMAGE_LAYOUT_GENERAL,
                   target.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    ++restores_;
    pixels_restored_ += uint64_t(target.width) * height;
    AnnotateCopy(target.width, height);
    ForgetClear(target);
  }


  void CopyBackForResolve(OwnedImage& target, OwnedImage& source) {
    ++perf_epoch_;
    EdramTouch(target.image);
    EdramRead(source.image, "a copy back for a resolve");
    if (!perf_alias_live_.empty() || !perf_lazy_pending_.empty()) {
      PerfSettleAll();
    }
    Prepare(source);
    Prepare(target);
    const VkImageAspectFlags aspect =
        IsDepthFormat(target.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    constexpr VkPipelineStageFlags kWrites =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_, kWrites, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                              nullptr, 0, nullptr);
    VkImageCopy copy{};
    copy.srcSubresource = {aspect, 0, 0, 1};
    copy.dstSubresource = {aspect, 0, 0, 1};
    copy.extent = {target.width, target.height, 1};
    AnnotateUseClear(target, target.width, target.height);
    MarkGpu(kGpuCopies);
    BeforeImageTransfer(commands_job_);
    copy_image_(commands_job_, source.image, VK_IMAGE_LAYOUT_GENERAL, target.image, VK_IMAGE_LAYOUT_GENERAL,
                   1, &copy);


    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              0, 1, &barrier, 0, nullptr, 0, nullptr);
    ++restores_for_resolve_;
    pixels_restored_ += uint64_t(target.width) * target.height;
    AnnotateCopy(target.width, target.height);
    ForgetClear(target);
  }


  void ShadowMinMismatch(const char* reason, uint32_t datum) {
    ++sm_mismatches_;
    if (sm_phase_ != kSmOff) {
      sm_phase_ = kSmOff;
      REXLOG_ERROR("[native] C2 shadow by minimum: MISMATCH ({}; datum {:08X}). Turned off for the session: the shadow "
                   "map is copied again as before",
                   reason, datum);
    }
  }


  void ShadowMinNoFit(const char* reason, uint32_t datum) {
    if (sm_phase_ != kSmOff) {
      sm_phase_ = kSmOff;
      REXLOG_WARN("[native] C2 shadow by minimum: not applied in this session ({}; datum {:08X}); the shadow map is "
                  "still copied as always",
                  reason, datum);
    }
  }


  void ShadowMinMissCycle(const char* reason, uint32_t datum, bool permanent) {
    sm_cycle_clean_ = false;
    if (sm_cycle_clear_) {
      ShadowMinMismatch(reason, datum);
    } else if (permanent) {
      ShadowMinNoFit(reason, datum);
    }
  }


  bool ShadowMinOn() {
    if (!REXCVAR_GET(bo2zm_native_shadow_min) || !REXCVAR_GET(bo2zm_native_cheap_pcf)) {
      return false;
    }
    const int32_t alternate = REXCVAR_GET(bo2zm_native_shadow_min_alternate_s);
    if (alternate <= 0 || sm_phase_ != kSmApplying) {
      return true;
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                           sm_start_alternation_)
                              .count();
    const bool with_min = (seconds / alternate) % 2 == 0;
    if (with_min != sm_span_annotated_) {
      sm_span_annotated_ = with_min;
      REXLOG_INFO("[native] C2 shadow by minimum (alternation): {}", with_min ? "with the minimum" : "copying");
    }
    return with_min;
  }


  void ShadowMinAfterSwap(const OwnedImage& target, uint32_t base, bool would_clear) {
    if (target.width != kSideShadows || target.height != kSideShadows) {
      return;
    }
    if (!would_clear) {
      if (sm_state_ == kSmOverlay && sm_cycle_clear_) {
        ShadowMinMismatch("the target with the overlay alone is resolved without clearing", base);
      }
      if (sm_state_ != kSmFree) {
        sm_clean_consecutive_ = 0;
      }
      sm_state_ = kSmAfterBase;
      sm_target_cycle_ = &target;
      sm_base_cycle_ = base;
      sm_cycle_clean_ = true;
      sm_cycle_clear_ = false;
      sm_base_rewritten_ = false;
      sm_on_ = ShadowMinOn();
      return;
    }
    if (sm_state_ != kSmOverlay || &target != sm_target_cycle_) {
      if (sm_state_ != kSmFree) {
        sm_clean_consecutive_ = 0;
      }
      sm_state_ = kSmFree;
      return;
    }
    sm_state_ = kSmFree;
    ++sm_cycles_;
    if (sm_cycle_clear_) {

      if (base != sm_overlay_) {
        ShadowMinMismatch("the overlay resolve goes to another texture", base);
        sm_overlay_ = base;
      }
      sm_virtual_ = true;
      sm_virtual_valid_ = !sm_base_rewritten_;
      if (sm_base_rewritten_) {
        ShadowMinMismatch("the world texture was written again inside the cycle", sm_base_);
      }
      ++sm_cycles_applied_;
      return;
    }


    if (base != sm_overlay_ || sm_base_cycle_ != sm_base_ || sm_target_cycle_ != sm_target_) {
      if (sm_phase_ == kSmApplying) {


        ShadowMinNoFit("a shadow map cycle different from the learned one appears (the datum is its texture)", base);
        return;
      }
      sm_overlay_ = base;
      sm_base_ = sm_base_cycle_;
      sm_target_ = sm_target_cycle_;
      sm_clean_consecutive_ = 0;
    }
    sm_clean_consecutive_ = (sm_cycle_clean_ && !sm_base_rewritten_) ? sm_clean_consecutive_ + 1 : 0;
    if (sm_phase_ == kSmObserving && sm_on_ && sm_clean_consecutive_ >= kShadowMinCycles) {
      sm_phase_ = kSmApplying;
      REXLOG_INFO("[native] C2 shadow by minimum: APPLYING after {} clean cycles in a row (target {}x{}, base at "
                  "{:08X}, overlay at {:08X}): the overlay pass is drawn onto the cleared target and not copied",
                  sm_clean_consecutive_, target.width, target.height, sm_base_, sm_overlay_);
    }
  }


  bool ShadowMinInsteadOfRestore(OwnedImage& target) {
    if (&target != sm_target_cycle_ || sm_state_ != kSmAfterBase) {
      if (&target == sm_target_cycle_ && sm_state_ == kSmOverlay) {
        ShadowMinMissCycle("the overlay target is restored again", target.resolved_base, false);
      }
      return false;
    }
    if (target.resolved_base != sm_base_cycle_) {
      ShadowMinMissCycle("the target is restored from another texture", target.resolved_base, false);
      return false;
    }
    sm_state_ = kSmOverlay;


    if (sm_phase_ != kSmApplying || !sm_on_ || &target != sm_target_ || sm_base_cycle_ != sm_base_ ||
        !sm_overlay_ || !clear_depth_ || !target.accepts_copy_target) {
      return false;
    }
    Prepare(target);
    MarkGpu(kGpuClears);
    const VkClearDepthStencilValue value{1.0f, 0};
    BeforeImageTransfer(commands_job_);
    clear_depth_(commands_job_, target.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &kRangeDepth);
    AnnotateUseClear(target, target.width, target.height);
    ForgetClear(target);
    sm_cycle_clear_ = true;
    sm_pixels_saved_ += uint64_t(target.width) * target.height;
    return true;
  }


  void ShadowMinCopyFrom(const OwnedImage& source) {
    if (&source == sm_target_cycle_ && sm_state_ != kSmFree) {
      ShadowMinMissCycle("the shadow map target is copied from in the middle of the cycle", 0, false);
    }
  }


  void ShadowMinBeforeClear(const OwnedImage& target) {
    if (&target == sm_target_cycle_ && sm_state_ != kSmFree) {
      sm_clean_consecutive_ = 0;
      sm_state_ = kSmFree;
    }
  }


  void ShadowMinResolvedWritten(uint32_t base) {
    if (sm_virtual_) {
      if (base == sm_overlay_) {
        sm_virtual_ = false;
      } else if (base == sm_base_) {
        sm_virtual_valid_ = false;
      }
    }
    if (sm_state_ != kSmFree && base == sm_base_cycle_) {
      sm_base_rewritten_ = true;
    }
  }


  void ReportShadowMin() {
    if (!sm_cycles_ && !sm_mismatches_ && !sm_reads_unable_ && !sm_reads_a_offbeat_) {
      return;
    }
    const double frames = double(presented_ - presented_report_copies_);
    const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
    const double mp = double(sm_pixels_saved_ - sm_pixels_saved_previous_) / 1e6 * perFrm;
    static constexpr const char* kPhases[3] = {"observing", "APPLYING", "off"};
    BO2ZM_REPORT_RING(
        "[native] C2 shadow by minimum: phase {}; {} cycles since boot ({} clean in a row, {} applied); "
        "{:.2f} Mpixels per frame cleared instead of copied (~{:.2f} ms real of copy less and ~{:.2f} of clear more); "
        "overlay draws {} ({} not exact); reads of {:08X} since boot: {} with the paired base, {} with itself, "
        "{} without the minimum, {} between the two resolves, {} without tfetch2DShadowMin; MISMATCHES {}{}",
        kPhases[sm_phase_ < 3 ? sm_phase_ : 2], sm_cycles_, sm_clean_consecutive_, sm_cycles_applied_, mp, mp * 0.60,
        mp * 0.075, sm_draws_overlay_, sm_draws_no_exact_, sm_overlay_, sm_reads_min_, sm_reads_yes_same_,
        sm_reads_normal_, sm_reads_a_offbeat_, sm_reads_unable_, sm_mismatches_,
        sm_mismatches_ ? " *** THE IMAGE MAY HAVE CHANGED: see the ERROR in the log ***" : " (0 = the image is the same)");
    sm_pixels_saved_previous_ = sm_pixels_saved_;
  }


  void ResolvedWritten(uint32_t base, uint64_t pixels = 0) {


    if (draws_) {
      draws_->InvalidateTexturesIn(base);
    }
    ShadowMinResolvedWritten(base);
    if (!volumes_.empty()) {
      writes_resolved_[base] = ++counter_writes_;
    }
    if (!lent_.empty()) {
      lent_.erase(base);
    }
    if (!expired_.empty()) {
      expired_.erase(base);
    }
    if (pixels) {
      const auto c = copies_per_target_.find(base);
      if (c != copies_per_target_.end()) {
        c->second.pixels += pixels;
      }
    }
  }


  void ForgetClear(const OwnedImage& target) {
    const auto e = state_target_.find(&target);
    if (e == state_target_.end()) {
      return;
    }
    if (FrontKnownClearValid(e->second.clear_clean, e->second.draws_on_clear, draws_ ? draws_->Drawn() : 0)) {
      ++clears_useless_;
      bo2zm::native::performance::g_clears_useless.fetch_add(1, std::memory_order_relaxed);
      pixels_clears_useless_ += uint64_t(target.width) * target.height;
    }
    e->second.clear_clean = false;
  }


  void AnnotateAreaUseful(const OwnedImage& target, int32_t y1) {
    if (y1 > 0) {
      auto& e = state_target_[&target];
      e.height_used = std::max(e.height_used, std::min(uint32_t(y1), target.height));
    }
  }


  bool ClearRedundant(const OwnedImage& target, uint64_t value) {
    auto& e = state_target_[&target];
    const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
    if (REXCVAR_GET(bo2zm_native_skip_repeated_clears) && e.value_clear == value &&
        FrontKnownClearValid(e.clear_clean, e.draws_on_clear, drawn)) {
      pixels_clears_skipped_ += uint64_t(target.width) * target.height;
      return true;
    }
    e.clear_clean = true;
    e.value_clear = value;
    e.draws_on_clear = drawn;
    return false;
  }

  bool Create(OwnedImage& image, uint32_t width, uint32_t height, VkImageUsageFlags use,
             VkFormat format = kFormatColor) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = use;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
            image.guest_memory)) {


      if (!draws_ || !draws_->ReleaseTexturesOnOutOfMemory() ||
          !rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
              image.guest_memory)) {
        REXLOG_ERROR("[native] C2: out of GPU memory for a {}x{} target (format {}) and releasing the cache was not "
                     "enough",
                     width, height, uint32_t(format));
        return false;
      }
    }
    VkImageViewCreateInfo info_view{};
    info_view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info_view.image = image.image;
    info_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info_view.format = format;
    info_view.subresourceRange = IsDepthFormat(format) ? kRangeDepth : kRangeColor;
    if (dfn_.vkCreateImageView(device_, &info_view, nullptr, &image.view) != VK_SUCCESS) {
      Destroy(image);
      return false;
    }
    image.width = width;
    image.height = height;
    image.format = format;
    image.prepared = false;
    return true;
  }

  void Destroy(OwnedImage& image) {
    if (image.image != VK_NULL_HANDLE) {
      bo2zm::native::edram::Get().Forget(uint64_t(image.image));
    }
    deferred_clears_.erase(&image);
    if (front_compositions_.count(&image) && FrontTraceLine()) {
      REXLOG_INFO("[front-trace] invalidate target {:p} reason destroy frame {}",
                  static_cast<const void*>(&image), presented_);
    }
    front_compositions_.erase(&image);
    CompositeOnDestroy(image);


    if (!fronts_pending_.empty() && image.image != VK_NULL_HANDLE) {
      for (auto it = fronts_pending_.begin(); it != fronts_pending_.end();) {
        if (it->second.texture_vk == image.image) {
          FreeRetained(it->second);
          it = fronts_pending_.erase(it);
        } else if (it->second.source_vk == image.image) {
          fronts_expired_.insert(it->first);
          FreeRetained(it->second);
          it = fronts_pending_.erase(it);
        } else {
          ++it;
        }
      }
    }


    if (!pending_.empty() && image.image != VK_NULL_HANDLE) {
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.source_vk == image.image || it->second.target_vk == image.image) {
          expired_.insert(it->first);
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }


    if (draws_ && image.view != VK_NULL_HANDLE) {
      draws_->ForgetView(image.view);
    }
    if (image.view != VK_NULL_HANDLE) {
      const auto fb = conv_framebuffers_.find(image.view);
      if (fb != conv_framebuffers_.end()) {
        if (fb->second != VK_NULL_HANDLE) dfn_.vkDestroyFramebuffer(device_, fb->second, nullptr);
        conv_framebuffers_.erase(fb);
      }
    }
    if (image.view != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, image.image, nullptr);
    if (image.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, image.guest_memory, nullptr);
    image = OwnedImage{};
  }

  void BeforeImageTransfer(VkCommandBuffer commands) const {
    if (!REXCVAR_GET(bo2zm_native_scene_sync)) return;
    if (REXCVAR_GET(bo2zm_native_scene_sync_precise)) {
      scene::precise_patch::BeforeTransfer(dfn_.vkCmdPipelineBarrier, commands, true,
                                          uint32_t(REXCVAR_GET(bo2zm_native_scene_sync_mask)));
    } else if (REXCVAR_GET(bo2zm_native_scene_transfer_barriers)) {
      scene::BeforeTransfer(dfn_.vkCmdPipelineBarrier, commands,
                            REXCVAR_GET(bo2zm_native_transfer_dst_only));
    }
  }


  static v042::Screen LayoutScreen() {
    const auto screen = v042::Screen(display::g_screen.load(std::memory_order_relaxed));
#if defined(BO2_GAME_CAMPAIGN)
    if (screen == v042::Screen::kUltrawide) return v042::Screen::kOriginal;
#endif
    return screen;
  }


  const v041::SceneScale& SceneScaleOf() {
    if (!scene_scale_ready_) {
      scene_scale_ready_ = true;
      const std::string text = REXCVAR_GET(bo2zm_native_scene_resolution);
      v041::Resolution resolution;
      if (!v041::ParsePreset(text, resolution)) {
        REXLOG_WARN("[native] C2: bo2zm_native_scene_resolution = '{}' is not 880x720, 1280x720 or 1920x1080: the "
                    "original 880x720 is used", text);
      }


      {
        display::LoadSettingsOnce();
        const v041::Resolution preset = resolution;
        resolution = v042::SceneSize(resolution, LayoutScreen(), display::DisplayOf());
        if (resolution.width != preset.width) {
          REXLOG_INFO("[native] C2: true full screen: the {}x{} internal resolution is drawn {}x{} wide for the "
                      "display's aspect", preset.width, preset.height, resolution.width, resolution.height);
        }
      }
      scene_pct_ = uint32_t(std::clamp(REXCVAR_GET(bo2zm_native_scene_scale), 50, 100));
      scene_pitch_min_ = uint32_t(std::max(REXCVAR_GET(bo2zm_native_scene_scale_min_pitch), 1));
      scene_pitch_max_ = uint32_t(std::max(REXCVAR_GET(bo2zm_native_scene_scale_max_pitch), 1));
      scene_scale_ = v041::MakeSceneScale(resolution, scene_pct_);
      scene_host_ = v041::SceneHost(scene_scale_);
      bo2zm::native::performance::g_scene_scale_pct.store(scene_pct_, std::memory_order_relaxed);
      display::g_scene_requested_width.store(resolution.width, std::memory_order_relaxed);
      display::g_scene_requested_height.store(resolution.height, std::memory_order_relaxed);
      bo2zm::native::performance::g_scene_preset_width.store(resolution.width, std::memory_order_relaxed);
      bo2zm::native::performance::g_scene_preset_height.store(resolution.height, std::memory_order_relaxed);
      bo2zm::native::performance::g_scene_width.store(scene_host_.width, std::memory_order_relaxed);
      bo2zm::native::performance::g_scene_height.store(scene_host_.height, std::memory_order_relaxed);
      REXLOG_INFO("[native] C2: scene internal resolution {}x{} at {} % for render targets of pitch {}-{}: the "
                  "game's 880x720 scene is drawn at {}x{} (x {}/{}, y {}/{}){}",
                  resolution.width, resolution.height, scene_pct_, scene_pitch_min_, scene_pitch_max_,
                  scene_host_.width, scene_host_.height, scene_scale_.x.num, scene_scale_.x.den, scene_scale_.y.num,
                  scene_scale_.y.den, scene_scale_.Identity() ? " (nothing scaled)" : "");
    }
    return scene_scale_;
  }

  const v041::SceneScale& OutputScaleOf() {
    if (!output_scale_ready_) {
      output_scale_ready_ = true;
      display::LoadSettingsOnce();
      namespace v = v042;
      const auto output = v::Output(display::g_output.load(std::memory_order_relaxed));
      const auto screen = LayoutScreen();
      v::Display d = display::DisplayOf();
      const char* display_source = "launcher";
      if (!d.Known()) {
        d = v::Landscape(rex::ui::present_stats::surface_width.load(std::memory_order_relaxed),
                         rex::ui::present_stats::surface_height.load(std::memory_order_relaxed));
        display_source = d.Known() ? "presenter surface" : "unknown (auto = 1920x1080)";
      }
      output_pitch_min_ = uint32_t(std::max(REXCVAR_GET(bo2zm_native_output_min_pitch), 1));
      output_pitch_max_ = uint32_t(std::max(REXCVAR_GET(bo2zm_native_output_max_pitch), 1));
      output_host_ = v::OutputSize(output, screen, d);
      output_scale_ = v::OutputScale(output_host_);
      display::g_output_width.store(output_host_.width, std::memory_order_relaxed);
      display::g_output_height.store(output_host_.height, std::memory_order_relaxed);
      REXLOG_INFO("[native] C2: output {} with screen mode {} (display {}x{}, {}): composite and front buffer of pitch "
                  "{}-{} drawn at {}x{} (x {}/{}, y {}/{}){}",
                  v::OutputKey(output), v::ScreenKey(screen), d.width, d.height, display_source, output_pitch_min_,
                  output_pitch_max_, output_host_.width, output_host_.height, output_scale_.x.num,
                  output_scale_.x.den, output_scale_.y.num, output_scale_.y.den,
                  output_scale_.Identity() ? " (nothing scaled)" : "");
    }
    return output_scale_;
  }

  uint32_t ScaleClassOf(uint32_t pitch) {
    if (!SceneScaleOf().Identity() && pitch >= scene_pitch_min_ && pitch <= scene_pitch_max_) {
      return display::kClassScene;
    }
    if (!OutputScaleOf().Identity() && pitch >= output_pitch_min_ && pitch <= output_pitch_max_) {
      return display::kClassComposite;
    }
    return display::kClassNone;
  }
  bool ScalesPitch(uint32_t pitch) { return ScaleClassOf(pitch) != display::kClassNone; }
  const v041::SceneScale& ScaleOfClass(uint32_t scale_class) {
    return scale_class == display::kClassComposite ? OutputScaleOf() : SceneScaleOf();
  }
  uint32_t ScaledSideX(uint32_t side, uint32_t scale_class) {
    return v041::ScaledSide(side, ScaleOfClass(scale_class).x);
  }
  uint32_t ScaledSideY(uint32_t side, uint32_t scale_class) {
    return v041::ScaledSide(side, ScaleOfClass(scale_class).y);
  }


  static void PresentSizeOf(const OwnedImage& texture, uint32_t width_guest, uint32_t height_guest, uint32_t& w,
                            uint32_t& h) {
    w = width_guest ? uint32_t(std::max(HostX(texture, int32_t(width_guest)), 1)) : texture.width;
    h = height_guest ? uint32_t(std::max(HostY(texture, int32_t(height_guest)), 1)) : texture.height;
    w = std::min(w, texture.width);
    h = std::min(h, texture.height);
  }


  static int32_t HostX(const OwnedImage& image, int32_t v) {
    return v040::ToHost(v, image.width, image.width_guest);
  }
  static int32_t HostY(const OwnedImage& image, int32_t v) {
    return v040::ToHost(v, image.height, image.height_guest);
  }
  static bool IsScaled(const OwnedImage& image) {
    return (image.width_guest && image.width_guest != image.width) ||
           (image.height_guest && image.height_guest != image.height);
  }


  bool DeferredClearsOn() {
    if (deferred_clears_on_ < 0) {
      deferred_clears_on_ = REXCVAR_GET(bo2zm_native_deferred_clears) ? 1 : 0;
      REXLOG_INFO("[native] C2: deferred clears (bo2zm_native_deferred_clears) = {}",
                  deferred_clears_on_ ? "on: whole-image clears become loadOp = CLEAR of the next pass"
                                      : "off: separate clears, as before");
    }
    return deferred_clears_on_ == 1;
  }


  bool DeferClear(OwnedImage& image, const VkClearValue& value, bool depth, bool in_pass) {
    ++perf_epoch_;
    if (!DeferredClearsOn() || image.view == VK_NULL_HANDLE || image.image == VK_NULL_HANDLE || !image.prepared ||
        !recording_ || (draws_ && draws_->PassActive())) {
      return false;
    }
    DeferredClear& d = deferred_clears_[&image];
    d.handle = image.image;
    d.value = value;
    d.depth = depth;
    d.in_pass = in_pass;
    bo2zm::native::performance::g_clears_deferred.fetch_add(1, std::memory_order_relaxed);
    return true;
  }


  bool GuestClearsInPassOn() {
    if (guest_clears_in_pass_on_ < 0) {
      guest_clears_in_pass_on_ = REXCVAR_GET(bo2zm_native_guest_clears_in_pass) ? 1 : 0;
      REXLOG_INFO("[native] C2: guest Clear() (bo2zm_native_guest_clears_in_pass) = {}",
                  guest_clears_in_pass_on_
                      ? (DeferredClearsOn() ? "inside the open pass, or deferred to the next pass on the image"
                                            : "inside the open pass only (bo2zm_native_deferred_clears is off)")
                      : "a separate pass per Clear(), as before");
    }
    return guest_clears_in_pass_on_ == 1;
  }


  void GuestClearBarrier() {
    VkMemoryBarrier m{};
    m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    m.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_TRANSFER_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    constexpr VkPipelineStageFlags kStages =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_, kStages, kStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &m, 0,
                              nullptr, 0, nullptr);
  }


  bool DeferGuestClear(OwnedImage& image, bool depth, VkImageAspectFlags aspects, const VkClearValue& value,
                       const VkRect2D& area) {
    if (!GuestClearsInPassOn() || !DeferredClearsOn() || image.view == VK_NULL_HANDLE ||
        image.image == VK_NULL_HANDLE || !image.prepared || !recording_ || (draws_ && draws_->PassActive())) {
      return false;
    }
    const uint32_t slot = depth ? v043::kSlotDepth : 0;
    if (!v043::AspectsFitSlot(uint32_t(aspects), slot)) {
      return false;
    }
    const v043::Rect rect{area.offset.x, area.offset.y, area.extent.width, area.extent.height};
    if (!v043::RectInside(rect, image.width, image.height)) {
      return false;
    }
    if (v043::WholeImageClear(rect, uint32_t(aspects), slot, image.width, image.height)) {

      if (!DeferClear(image, value, depth, depth && !image.accepts_copy_target)) {
        return false;
      }
    } else {
      FlushDeferredClear(image);
      DeferredClear& d = deferred_clears_[&image];
      d = DeferredClear{};
      d.handle = image.image;
      d.value = value;
      d.depth = depth;
      d.rect = true;
      d.aspects = aspects;
      d.area = area;
      bo2zm::native::performance::g_clears_deferred.fetch_add(1, std::memory_order_relaxed);
    }
    bo2zm::native::performance::g_guest_clears_deferred.fetch_add(1, std::memory_order_relaxed);
    return true;
  }


  bool HasDeferredWork(const OwnedImage& image) const {
    if (!deferred_clears_.empty() && deferred_clears_.count(&image)) {
      return true;
    }
    for (const auto& [address, pending] : pending_) {
      if (pending.source_vk == image.image) {
        return true;
      }
    }
    for (const auto& [address, pending] : fronts_pending_) {
      if (pending.retained < 0 && pending.source_vk == image.image) {
        return true;
      }
    }
    return composite_has_ && (composite_.target == &image || composite_.source_vk == image.image);
  }


  void RecordDeferredClear(const OwnedImage& image, const DeferredClear& d) {
    ++perf_epoch_;
    if (!d.rect) {
      EdramClear(image.image);
    }
    if (!d.depth && !perf_lazy_pending_.empty() && image.image != VK_NULL_HANDLE) {
      PerfLazySourceWritten(image.image);
    }
    if (image.image != d.handle || image.image == VK_NULL_HANDLE || !recording_) {


      bo2zm::native::performance::g_clears_dropped.fetch_add(1, std::memory_order_relaxed);
      if (++deferred_clears_dropped_log_ <= 8) {
        REXLOG_ERROR("[native] C2 deferred clears: a pending clear of a {}x{} image was DROPPED ({}). Report this "
                     "with the log; --bo2zm_native_deferred_clears=false restores the previous path",
                     image.width, image.height, !recording_ ? "nothing recording" : "its image changed");
      }
      return;
    }
    bo2zm::native::performance::g_clears_flushed.fetch_add(1, std::memory_order_relaxed);
    if (d.rect) {

      bo2zm::native::performance::g_guest_clears_separate.fetch_add(1, std::memory_order_relaxed);
      MarkGpu(kGpuClears);
      GuestClearBarrier();
      if (!draws_ || !draws_->ClearAttachmentInPass(commands_job_, image, d.aspects, d.value, d.area)) {
        if (++clears_in_pass_failed_ <= 8) {
          REXLOG_WARN("[native] C2: could NOT make a deferred guest clear of a {}x{} image ({}x{} at {},{}; error {})",
                      image.width, image.height, d.area.extent.width, d.area.extent.height, d.area.offset.x,
                      d.area.offset.y, clears_in_pass_failed_);
        }
        return;
      }
      GuestClearBarrier();
      return;
    }
    if (d.depth) {
      if (d.in_pass) {
        if (!draws_ || !draws_->ClearDepthInPass(commands_job_, image, d.value.depthStencil.depth,
                                                  d.value.depthStencil.stencil)) {
          if (++clears_in_pass_failed_ <= 8) {
            REXLOG_WARN("[native] C2: could NOT clear the {}x{} depth by opening a pass (deferred clear, error {})",
                        image.width, image.height, clears_in_pass_failed_);
          }
        }
      } else if (clear_depth_) {
        BeforeImageTransfer(commands_job_);
        clear_depth_(commands_job_, image.image, VK_IMAGE_LAYOUT_GENERAL, &d.value.depthStencil, 1, &kRangeDepth);
      }
      return;
    }
    BeforeImageTransfer(commands_job_);
    dfn_.vkCmdClearColorImage(commands_job_, image.image, VK_IMAGE_LAYOUT_GENERAL, &d.value.color, 1,
                              &kRangeColor);
  }


  void FlushDeferredClears() {
    if (deferred_clears_.empty()) {
      return;
    }
    if (draws_ && draws_->PassActive()) {
      if (++deferred_clears_in_pass_log_ <= 4) {
        REXLOG_ERROR("[native] C2 deferred clears: {} pending while a pass is open; kept for the next flush",
                     deferred_clears_.size());
      }
      return;
    }


    if (!Record()) {
      return;
    }
    std::vector<std::pair<const NativeImage*, DeferredClear>> pending(deferred_clears_.begin(),
                                                                       deferred_clears_.end());
    deferred_clears_.clear();
    for (const auto& [image, d] : pending) {
      RecordDeferredClear(*image, d);
    }
  }


  void FlushDeferredClear(const NativeImage& image) {
    if (deferred_clears_.empty()) {
      return;
    }
    const auto it = deferred_clears_.find(&image);
    if (it == deferred_clears_.end()) {
      return;
    }
    if (draws_ && draws_->PassActive()) {

      if (++deferred_clears_in_pass_log_ <= 4) {
        REXLOG_ERROR("[native] C2 deferred clears: a {}x{} image with a pending clear is used while a pass is open; "
                     "kept pending", image.width, image.height);
      }
      return;
    }
    if (!Record()) {
      return;
    }
    const DeferredClear d = it->second;
    deferred_clears_.erase(it);
    RecordDeferredClear(image, d);
  }

  void TakeDeferredClears(const std::array<NativeImage*, 5>& images, uint32_t width, uint32_t height,
                          std::array<bool, 5>& cleared, std::array<VkClearValue, 5>& values,
                          std::array<v043::RectRequest, 5>& rects, std::array<VkClearValue, 5>& rect_values) override {
    cleared.fill(false);
    rects.fill(v043::RectRequest{});
    if (deferred_clears_.empty()) {
      return;
    }
    bool fused = false;
    for (uint32_t i = 0; i < 5; ++i) {
      const NativeImage* image = images[i];
      if (!image) {
        continue;
      }
      const auto it = deferred_clears_.find(image);
      if (it == deferred_clears_.end()) {
        continue;
      }
      const DeferredClear d = it->second;
      deferred_clears_.erase(it);
      if (d.rect) {


        const v043::Rect rect{d.area.offset.x, d.area.offset.y, d.area.extent.width, d.area.extent.height};
        if (image->image == d.handle && d.depth == (i == 4) && v043::AspectsFitSlot(uint32_t(d.aspects), i) &&
            v043::RectInside(rect, width, height)) {
          rects[i].active = true;
          rects[i].aspects = uint32_t(d.aspects);
          rects[i].area = rect;
          rect_values[i] = d.value;
          fused = true;
        } else {
          RecordDeferredClear(*image, d);
        }
        continue;
      }

      if (image->image == d.handle && image->width == width && image->height == height && d.depth == (i == 4)) {
        cleared[i] = true;
        values[i] = d.value;
        fused = true;
        bo2zm::native::performance::g_clears_fused.fetch_add(1, std::memory_order_relaxed);
      } else {
        namespace v = bo2zm::native::v046;
        const bool size = image->image == d.handle && d.depth == (i == 4);
        v::g_clear_miss[size ? v::kMissWholeSize : v::kMissOther].fetch_add(1, std::memory_order_relaxed);
        if (size) {
          v::g_clear_miss_sample[v::kMissWholeSize].store(
              (uint64_t(image->width) << 48) | (uint64_t(image->height) << 32) | (uint64_t(width) << 16) | height,
              std::memory_order_relaxed);
        }
        RecordDeferredClear(*image, d);
      }
    }


    if (fused) {


      VkMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      constexpr VkPipelineStageFlags kBefore =
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      constexpr VkPipelineStageFlags kAfter = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dfn_.vkCmdPipelineBarrier(commands_job_, kBefore, kAfter, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
  }


  void Prepare(OwnedImage& image) {
    ++perf_epoch_;
    if (image.prepared) {
      return;
    }


    const VkCommandBuffer commands = CommandsUpload();
    if (commands == VK_NULL_HANDLE) {
      return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    const bool depth = image.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                             image.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    barrier.image = image.image;
    barrier.subresourceRange = depth ? kRangeDepth : kRangeColor;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
    if (depth) {


      if (!image.accepts_copy_target && draws_) {
        if (!draws_->ClearDepthInPass(commands, image, 1.0f, 0) &&
            ++clears_in_pass_failed_ <= 8) {
          REXLOG_WARN("[native] C2: could NOT prepare the {}x{} depth by opening a clear pass (error {})",
                      image.width, image.height, clears_in_pass_failed_);
        }
      } else if (clear_depth_) {
        const VkClearDepthStencilValue far_depth{1.0f, 0};
        BeforeImageTransfer(commands);
        clear_depth_(commands, image.image, VK_IMAGE_LAYOUT_GENERAL, &far_depth, 1,
                            &kRangeDepth);
      }
    } else {
      const VkClearColorValue cero{};
      BeforeImageTransfer(commands);
      dfn_.vkCmdClearColorImage(commands, image.image, VK_IMAGE_LAYOUT_GENERAL, &cero,
                                1, &kRangeColor);
    }
    image.prepared = true;
    if (draws_) {
      draws_->InvalidateTextures();
    }
  }

  bool Record() {
    if (recording_) {
      return true;
    }
    const auto before_record = std::chrono::steady_clock::now();
    const bool recording = BeginRecording();
    ns_record_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_record).count());
    ++recordings_;
    return recording;
  }

  bool BeginRecording() {


    slot_ = (slot_ + 1) % slots_used_;
    WorkSlot& slot = slots_[slot_];
    Complete(slot);
    slot.pixel_probe_tag = pixel_probe_frame_;
    slot.pixel_probe_mixed = false;
    slot.shader_pass_ordinal=0;slot.shader_target=false;slot.shader_target_seen=false;
    slot.shader_mark_begin=slot.shader_mark_end=UINT32_MAX;
    const bool profile_allowed = pass_profile::enabled.load(std::memory_order_relaxed) &&
        queries_ != VK_NULL_HANDLE && pass_profile::foreground.load(std::memory_order_acquire) &&
        bo2zm::preset::ClientInGame() && !pixel_probe::Busy(pixel_probe::Read().state);
    slot.pass_profile = pass_scheduler_.Next(performance::NowNs(), profile_allowed,
        pass_profile::generation.load(std::memory_order_acquire));
    slot.marks_categories = slot.pass_profile.sampled || marks_categories_;
    const bool shader_trial=slot.pixel_probe_tag.trial && slot.pixel_probe_tag.kind==pixel_probe::Kind::kShader;
    if(shader_trial)slot.marks_categories=false;
    pass_profile::supported.store(queries_ != VK_NULL_HANDLE, std::memory_order_relaxed);
    deferred::Finish();
    const auto before_pools = std::chrono::steady_clock::now();
    dfn_.vkResetCommandPool(device_, slot.pool_job, 0);
    dfn_.vkResetCommandPool(device_, slot.pool_upload, 0);
    ns_reset_pools_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_pools).count());
    commands_job_ = slot.job;
    commands_upload_ = slot.upload;
    recording_upload_ = false;
    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(commands_job_, &start) != VK_SUCCESS) {
      return false;
    }
    deferred::Begin(commands_job_);
    slot.categories.clear();
    if (queries_ != VK_NULL_HANDLE) {
      dfn_.vkCmdResetQueryPool(commands_job_, queries_, slot_ * kMarksPerSlot,
                               shader_trial ? 4u : timing::ResetCount(slot.marks_categories));
    }
    if (occlusions_ != VK_NULL_HANDLE) {
      DiscardOcclusions(slot);
      dfn_.vkCmdResetQueryPool(commands_job_, occlusions_, slot_ * kOcclusionsPerSlot,
                               kOcclusionsPerSlot);
    }


    if (visibility_ != VK_NULL_HANDLE) {
      if (!slot.visibility.empty()) {
        bo2zm::reflection_demand::AnnotateVisible(false);
        slot.visibility.clear();
      }
      dfn_.vkCmdResetQueryPool(commands_job_, visibility_, slot_ * kVisibilityPerSlot,
                               kVisibilityPerSlot);
    }


    slot.stats.clear();
    slot.stats_draw.clear();
    if (stats_ != VK_NULL_HANDLE && REXCVAR_GET(bo2zm_native_pipeline_stats) &&
        REXCVAR_GET(bo2zm_native_per_draw_stats_s) <= 0) {
      dfn_.vkCmdResetQueryPool(commands_job_, stats_,
                               slot_ * kStatsPerSlot, kStatsPerSlot);
    }
    if (stats_draw_ != VK_NULL_HANDLE && window_diagnostic_) {
      dfn_.vkCmdResetQueryPool(commands_job_, stats_draw_,
                               slot_ * kStatsDrawPerSlot,
                               kStatsDrawPerSlot);
    }

    if (alternate_marks_s_ > 0) {
      const int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - start_marks_)
                                   .count();
      const bool precise = (seconds / alternate_marks_s_) % 2 == 1;
      if (precise != marks_precise_) {
        marks_precise_ = precise;
        REXLOG_INFO("[native] marks test: {} (job {})", precise ? "precise" : "normal",
                    generation_commands_);
      }
    }
    slot.marks_precise = slot.pass_profile.sampled || marks_precise_;
    recording_ = true;
    MarkGpu(kGpuOthers);
    ++generation_commands_;
    if (draws_) {
      draws_->UseSlot(slot_);
    }
    return true;
  }


  void Complete(WorkSlot& slot) {
    for (WorkSlot& other : slots_) {
      if (&other != &slot && other.pending && other.order < slot.order &&
          dfn_.vkGetFenceStatus(device_, other.fence) == VK_SUCCESS) {
        CompleteOne(other);
      }
    }
    CompleteOne(slot);
  }

  void CompleteOne(WorkSlot& slot) {
    if (!slot.pending) {
      return;
    }
    const auto before_wait = std::chrono::steady_clock::now();
    dfn_.vkWaitForFences(device_, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    ns_waits_gpu_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - before_wait)
                                    .count());
    ++waits_gpu_;
    if (slot.submitted.time_since_epoch().count()) {
      ns_job_gpu_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - slot.submitted)
                                      .count());
      ++jobs_gpu_;
    }
    dfn_.vkResetFences(device_, 1, &slot.fence);
    slot.pending = false;
    const uint32_t marked = uint32_t(slot.categories.size());
    bool probe_gpu_valid = false;
    uint64_t probe_gpu_ns = 0;
    bool shader_target_valid=false;uint64_t shader_target_ns=0;
    if (queries_ != VK_NULL_HANDLE && marked >= 2) {
      std::array<uint64_t, kMarksPerSlot> marks{};
      const uint32_t index = uint32_t(&slot - slots_.data());
      if (read_queries_(device_, queries_, index * kMarksPerSlot, marked,
                          sizeof(uint64_t) * marked, marks.data(), sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
          marks[marked - 1] >= marks[0]) {
        pass_profile::Complete(slot.pass_profile, {marks.data(), marked}, slot.categories, period_mark_ns_,
            slot.pass_profile.generation == pass_profile::generation.load(std::memory_order_acquire) &&
            pass_profile::foreground.load(std::memory_order_acquire) && bo2zm::preset::ClientInGame() &&
            !slot.pixel_probe_mixed && !pixel_probe::Busy(pixel_probe::Read().state), true);
        if(slot.shader_target_seen && slot.shader_mark_begin < slot.shader_mark_end &&
            slot.shader_mark_end < marked-1 && marks[slot.shader_mark_end]>=marks[slot.shader_mark_begin] &&
            marks[slot.shader_mark_begin]>=marks[0] && marks[slot.shader_mark_end]<=marks[marked-1]) {
          shader_target_valid=true;
          shader_target_ns=uint64_t(double(marks[slot.shader_mark_end]-marks[slot.shader_mark_begin])*period_mark_ns_);
        }
        for (uint32_t i = 0; i + 1 < marked; ++i) {
          if (marks[i + 1] >= marks[i] && slot.categories[i] < kGpuCategories) {
            gpu_categories_ns_[slot.categories[i]] +=
                uint64_t(double(marks[i + 1] - marks[i]) * period_mark_ns_);
          }
        }


        if (mark_end_previous_ != 0) {
          if (marks[0] >= mark_end_previous_) {
            gpu_categories_ns_[kGpuGapBetweenJobs] +=
                uint64_t(double(marks[0] - mark_end_previous_) * period_mark_ns_);
          } else {
            ++overlaps_gpu_;
          }
        }
        mark_end_previous_ = marks[marked - 1];
        last_mark_gpu_ = marks[marked - 1];
        gpu_ns_ += uint64_t(double(marks[marked - 1] - marks[0]) * period_mark_ns_);
        probe_gpu_valid = true;
        probe_gpu_ns = uint64_t(double(marks[marked - 1] - marks[0]) * period_mark_ns_);
        ++gpu_jobs_;
        if (slot.marks_categories) {
          ++gpu_jobs_categories_;
        }
        if (slot.marks_precise && slot.marks_categories) {
          ++gpu_jobs_precise_;
        }
      } else {
        pass_profile::Complete(slot.pass_profile, {}, {}, period_mark_ns_, true, false);
        mark_end_previous_ = 0;
      }
    } else {
      mark_end_previous_ = 0;
    }
    slot.pass_profile = {};
    pixel_probe::Complete(slot.pixel_probe_tag, probe_gpu_valid, probe_gpu_ns,slot.shader_target_seen,shader_target_valid,shader_target_ns);
    slot.pixel_probe_tag = {};
    slot.categories.clear();
    ReadOcclusions(slot);
    ReadVisibility(slot);
    ReadStats(slot);
    ReadStatsDraw(slot);
    WriteReads(slot.reads);
  }


  void ReadStatsDraw(WorkSlot& slot) {
    if (slot.stats_draw.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.stats_draw.size());
    std::vector<uint64_t> values(size_t(n) * kCountersStat, 0);
    if (stats_draw_ != VK_NULL_HANDLE && read_queries_ &&
        read_queries_(device_, stats_draw_, slot.stats_draw.front().first, n,
                        sizeof(uint64_t) * values.size(), values.data(),
                        sizeof(uint64_t) * kCountersStat,
                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint16_t label = slot.stats_draw[i].second;
        fragments_per_shader_[label] += values[i * kCountersStat + 2];
        ++draws_per_shader_[label];
      }

      if (!window_read_) {
        window_read_ = true;
        ++frames_diagnostic_;
      }
    }
    slot.stats_draw.clear();
  }


  void ReadStats(WorkSlot& slot) {
    if (slot.stats.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.stats.size());
    std::array<uint64_t, kStatsPerSlot * kCountersStat> values{};
    if (stats_ != VK_NULL_HANDLE && read_queries_ &&
        read_queries_(device_, stats_, slot.stats.front().first, n,
                        sizeof(uint64_t) * kCountersStat * n, values.data(),
                        sizeof(uint64_t) * kCountersStat,
                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint8_t c = slot.stats[i].second;
        if (c < kGpuCategories) {
          vertices_category_[c] += values[i * kCountersStat + 0];
          primitives_category_[c] += values[i * kCountersStat + 1];
          fragments_category_[c] += values[i * kCountersStat + 2];
        }
      }
    }
    slot.stats.clear();
  }


  void ReadOcclusions(WorkSlot& slot) {
    if (slot.occlusions.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.occlusions.size());
    std::array<uint64_t, kOcclusionsPerSlot> counts{};
    const bool read = occlusions_ != VK_NULL_HANDLE && read_queries_ &&
                        read_queries_(device_, occlusions_, slot.occlusions.front().first, n,
                                        sizeof(uint64_t) * n, counts.data(), sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = occlusions_game_.find(slot.occlusions[i].second);
      if (it == occlusions_game_.end()) {
        continue;
      }
      if (read) {
        it->second.samples += counts[i];
      } else {
        it->second.failed = true;
      }
      if (it->second.spans_pending) {
        --it->second.spans_pending;
      }
      if (it->second.finished && it->second.spans_pending == 0) {
        PublishOcclusion(it);
      }
    }
    slot.occlusions.clear();
  }


  void ReadVisibility(WorkSlot& slot) {
    if (slot.visibility.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.visibility.size());
    std::array<uint64_t, kVisibilityPerSlot> counts{};
    const bool read = visibility_ != VK_NULL_HANDLE && read_queries_ &&
                        read_queries_(device_, visibility_, slot.visibility.front().first, n,
                                        sizeof(uint64_t) * n, counts.data(), sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    for (uint32_t i = 0; i < n; ++i) {
      if (slot.visibility[i].second == kVisibilityWitness) {
        if (read) {
          bo2zm::reflection_demand::AnnotateWitness(counts[i] != 0);
        }
      } else if (!read) {
        bo2zm::reflection_demand::AnnotateVisible(false);
      } else if (counts[i] != 0) {
        bo2zm::reflection_demand::AnnotateVisible(true);
      } else {
        bo2zm::reflection_demand::AnnotateHidden();
      }
    }
    slot.visibility.clear();
  }


  void DiscardOcclusions(WorkSlot& slot) {
    for (const auto& [index, id] : slot.occlusions) {
      auto it = occlusions_game_.find(id);
      if (it != occlusions_game_.end()) {
        it->second.failed = true;
        if (it->second.spans_pending) {
          --it->second.spans_pending;
        }
        if (it->second.finished && it->second.spans_pending == 0) {
          PublishOcclusion(it);
        }
      }
    }
    slot.occlusions.clear();
  }

  void PublishOcclusion(std::unordered_map<uint64_t, GameOcclusionQuery>::iterator it) {
    if (!it->second.failed) {
      if (occlusion_per_base_.size() >= 64 && !occlusion_per_base_.count(it->second.base)) {
        occlusion_per_base_.clear();
      }
      occlusion_per_base_[it->second.base] = it->second.samples;
      ++stats_occlusion_[2];
      stats_occlusion_[3] += it->second.samples;
      stats_occlusion_[4] = std::max(stats_occlusion_[4], it->second.samples);
    }
    occlusions_game_.erase(it);
  }

  bool SubmitJob(bool wait) {
    if (recording_) {
      deferred::Kick();
      if (draws_) {
        draws_->BeforeSubmit();
      }
      recording_ = false;
      std::array<VkCommandBuffer, 2> buffers{};
      uint32_t n = 0;
      if (recording_upload_) {
        recording_upload_ = false;
        UploadBarrier();
        if (dfn_.vkEndCommandBuffer(commands_upload_) != VK_SUCCESS) {
          return false;
        }
        buffers[n++] = commands_upload_;
      }
      if (queries_ != VK_NULL_HANDLE) {

        WorkSlot& marked = slots_[slot_];
        timing::End(write_mark_, commands_job_, queries_, slot_ * kMarksPerSlot, marked.categories);
      }
      deferred::Finish();
      deferred::Report();
      if (dfn_.vkEndCommandBuffer(commands_job_) != VK_SUCCESS) {
        return false;
      }
      buffers[n++] = commands_job_;
      VkSubmitInfo submit{};
      submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit.commandBufferCount = n;
      submit.pCommandBuffers = buffers.data();
      {
        const auto before_latch = std::chrono::steady_clock::now();
        const auto queue = vulkan_device_->AcquireQueue(family_, 0);
        const auto before_submit = std::chrono::steady_clock::now();
        if (dfn_.vkQueueSubmit(queue.queue(), 1, &submit, slots_[slot_].fence) != VK_SUCCESS) {
          return false;
        }
        ns_latch_job_ += Ns(before_latch, before_submit);
        ns_submit_job_ += Ns(before_submit, std::chrono::steady_clock::now());
        ++submits_job_;
      }
      WorkSlot& slot = slots_[slot_];
      slot.pending = true;
      slot.order = ++submits_;
      slot.submitted = std::chrono::steady_clock::now();
      pixel_probe::Submitted(slot.pixel_probe_tag, pixel_probe::NowNs(), slot.pixel_probe_mixed);

      slot.reads.insert(slot.reads.end(), reads_pending_.begin(),
                             reads_pending_.end());
      reads_pending_.clear();
    }
    if (wait) {
      Complete(slots_[slot_]);
    }
    return true;
  }

  void WaitGpu() {
    if (recording_) {
      SubmitJob(false);
    }
    for (WorkSlot& slot : slots_) {
      Complete(slot);
    }
    WaitOutputs();
  }


  void WaitOutputs() {
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (outputs_pending_[i]) {
        dfn_.vkWaitForFences(device_, 1, &fences_output_[i], VK_TRUE, UINT64_MAX);
        dfn_.vkResetFences(device_, 1, &fences_output_[i]);
        outputs_pending_[i] = false;
      }
    }
  }


  bool ComposeTiled(uint32_t base_swap) {
    std::vector<uint32_t> bases;
    bases.swap(resolved_frame_);
    if (bases.empty() || !blit_) {
      return false;
    }
    if (tiled_.image == VK_NULL_HANDLE &&
        !Create(tiled_, 1280, 720, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      blit_ = nullptr;
      return false;
    }
    if (draws_) {
      draws_->FinishPass();
    }
    if (!Record()) {
      return false;
    }
    Prepare(tiled_);
    const VkClearColorValue depth3d{{0.12f, 0.0f, 0.12f, 1.0f}};
    BeforeImageTransfer(commands_job_);
    dfn_.vkCmdClearColorImage(commands_job_, tiled_.image, VK_IMAGE_LAYOUT_GENERAL, &depth3d,
                              1, &kRangeColor);
    std::string list;
    for (size_t i = 0; i < bases.size() && i < 16; ++i) {
      const auto it = resolved_.find(bases[i]);
      if (it == resolved_.end() || !it->second.image.prepared ||
          it->second.image.format != kFormatColor) {
        continue;
      }
      const OwnedImage& image = it->second.image;
      const int32_t x = int32_t(i % 4) * 320;
      const int32_t y = int32_t(i / 4) * 180;
      VkImageBlit copy{};
      copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.srcOffsets[1] = {int32_t(image.width), int32_t(image.height), 1};
      copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.dstOffsets[0] = {x + 2, y + 2, 0};
      copy.dstOffsets[1] = {x + 318, y + 178, 1};
      BeforeImageTransfer(commands_job_);
      EdramRead(image.image, "the resolved-texture diagnostic grid");
      blit_(commands_job_, image.image, VK_IMAGE_LAYOUT_GENERAL, tiled_.image,
            VK_IMAGE_LAYOUT_GENERAL, 1, &copy, VK_FILTER_LINEAR);
      list += fmt::format(" {}{}:{:08X} {}x{}", i, bases[i] == base_swap ? "*" : "", bases[i],
                           image.width, image.height);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_warning_tiled_ >= std::chrono::seconds(10)) {
      last_warning_tiled_ = now;
      REXLOG_INFO("[native] diag resolved: {} in the frame (* = the Swap's):{}", bases.size(),
                  list);
    }
    return true;
  }


  void AnnotateCopy(uint32_t base, uint32_t width, uint32_t height, uint64_t draws) {
    if (base == bo2zm::reflection_demand::kAddress) {
      bo2zm::reflection_demand::AnnotateCopy();
    }
    TargetCopy& d = copies_per_target_[base];
    d.width = width;
    d.height = height;
    ++d.copies;
    d.draws += draws;
    d.with_draws += draws != 0 ? 1 : 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - report_copies_ < std::chrono::seconds(10)) {
      return;
    }
    report_copies_ = now;
    BO2ZM_REPORT_RING("[native] C2 output gamma diagnostic: ramp enabled {}; game RGB10 at 0 "
                     "({},{},{}), 64 ({},{},{}), 128 ({},{},{}), 192 ({},{},{}), 255 ({},{},{}); "
                     "output table RGB at 128 ({:.6f},{:.6f},{:.6f}); postprocess {} brightness {:.3f} "
                     "contrast {:.3f} gamma {:.3f}",
                     ramp_gamma_, ramp_game_[0][0], ramp_game_[0][1], ramp_game_[0][2],
                     ramp_game_[64][0], ramp_game_[64][1], ramp_game_[64][2],
                     ramp_game_[128][0], ramp_game_[128][1], ramp_game_[128][2],
                     ramp_game_[192][0], ramp_game_[192][1], ramp_game_[192][2],
                     ramp_game_[255][0], ramp_game_[255][1], ramp_game_[255][2],
                     ramp_values_[128 * 4], ramp_values_[128 * 4 + 1], ramp_values_[128 * 4 + 2],
                     postprocess_.active, postprocess_.brightness, postprocess_.contrast, postprocess_.gamma);
    std::vector<std::pair<uint32_t, TargetCopy>> order(copies_per_target_.begin(),
                                                         copies_per_target_.end());
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string list;
    uint32_t written = 0;
    for (const auto& [address, c] : order) {
      if (++written > 48) {
        list += fmt::format(" (and {} more addresses)", order.size() - 48);
        break;
      }
      const auto r = resolved_.find(address);
      const uint64_t reads = r != resolved_.end() ? r->second.reads : 0;
      if (r != resolved_.end()) {
        r->second.reads = 0;
      }
      list += fmt::format(" {:08X} {}x{}: {} copies, {} with draws ({:.1f} draws per copy), {:.2f} Mpixels, {} "
                           "reads{};",
                           address, c.width, c.height, c.copies, c.with_draws,
                           c.copies ? double(c.draws) / double(c.copies) : 0.0,
                           double(c.pixels) / 1e6, reads,
                           c.copies && !reads ? " *** NOBODY READS IT ***" : "");
    }
    copies_per_target_.clear();
    BO2ZM_REPORT_RING("[native] C2 faces resolved since the previous report:{}", list);
    PerfReport();
    if (swaps_ || restores_) {


      BO2ZM_REPORT_RING("[native] C2 resolves without a copy: {} swaps ({} without a clear, {} of COLOR) and {} "
                  "restorations since boot -> {}",
                  swaps_, swaps_no_clear_, swaps_color_, restores_,
                  restores_ == 0 ? "not a single restoration: the saving is clean"
                                       : "*** there are restorations: the saving is NOT clean ***");


      if (restores_for_resolve_ || resolve_no_source_ || resolve_content_old_) {
        BO2ZM_REPORT_RING("[native] C2 resolve with valid content: {} times the content was brought back "
                             "before resolving (before, the resolve read the previous frame's image: the menu flicker); "
                             "{} without a source; {} read the stale image (setting off)",
                             restores_for_resolve_, resolve_no_source_, resolve_content_old_);
      }


      const double frames = double(presented_ - presented_report_copies_);
      const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp_rest = double(pixels_restored_ - pixels_restored_previous_) / 1e6;
      const double mp_saving =
          double(pixels_restore_saved_ - pixels_restore_saved_previous_) / 1e6;
      BO2ZM_REPORT_RING("[native] C2 restorations per frame: {:.2f} Mpixels copied ({:.2f} ms real) and {:.2f} Mpixels "
                  "saved ({:.2f} ms); since boot: {} trimmed to the useful area and {} by swap (loaned reads {}{})",
                  mp_rest * perFrm, mp_rest * perFrm * 0.78, mp_saving * perFrm,
                  mp_saving * perFrm * 0.78, restores_clipped_, loans_,
                  lent_read_,
                  lent_read_ ? " *** THE LOAN IS NOT SAFE ***" : ", clean");

      if (no_swap_no_transfer_dst_ || no_swap_target_no_prepare_ ||
          no_swap_resolved_no_prepare_ || no_swap_sizes_different_) {
        BO2ZM_REPORT_RING(
            "[native] C2 restore by swap, times it could NOT: {} without TRANSFER_DST, {} with the target not prepared, "
            "{} with the resolved one not prepared; and of those that qualified, {} had different sizes (those would "
            "break the image)",
            no_swap_no_transfer_dst_, no_swap_target_no_prepare_,
            no_swap_resolved_no_prepare_, no_swap_sizes_different_);
      }
      pixels_restored_previous_ = pixels_restored_;
      pixels_restore_saved_previous_ = pixels_restore_saved_;
    }
    if (clears_skipped_ || clears_skipped_depth_) {


      const double frames = double(presented_ - presented_report_copies_);
      const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(pixels_clears_skipped_ - pixels_clears_skipped_previous_) / 1e6;
      BO2ZM_REPORT_RING("[native] C2 clears skipped: {} color and {} depth since boot; {:.2f} Mpixels per frame not "
                  "cleared ({:.2f} ms real)",
                  clears_skipped_, clears_skipped_depth_, mp * perFrm,
                  mp * perFrm * 0.075);
      pixels_clears_skipped_previous_ = pixels_clears_skipped_;
    }
    if (clears_useless_) {


      const double frames = double(presented_ - presented_report_copies_);
      const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(pixels_clears_useless_ - pixels_clears_useless_previous_) / 1e6;
      BO2ZM_REPORT_RING("[native] C2 clears that were of no use (the target changed content without anything being "
                  "drawn): {} since boot; {:.2f} Mpixels per frame ({:.2f} ms real)",
                  clears_useless_, mp * perFrm, mp * perFrm * 0.075);
      pixels_clears_useless_previous_ = pixels_clears_useless_;
    }
    if (lazy_postponed_ || lazy_reads_late_) {

      const double frames = double(presented_ - presented_report_copies_);
      const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(lazy_pixels_saved_ - lazy_pixels_saved_previous_) / 1e6;
      BO2ZM_REPORT_RING("[native] C2 lazy depth: {} depth resolves postponed since boot; {} recorded when "
                           "sampled, {} before their source was written again or another resolve, {} replaced by a swap "
                           "and {} dropped without copying (nobody sampled them): {:.2f} Mpixels per frame not copied "
                           "(~{:.2f} ms real); late reads {}{}",
                           lazy_postponed_, lazy_copied_read_, lazy_copied_write_,
                           lazy_replaced_, lazy_dropped_, mp * perFrm, mp * perFrm * 0.60,
                           lazy_reads_late_,
                           lazy_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the same)");
      lazy_pixels_saved_previous_ = lazy_pixels_saved_;
    }
    if (front_postponed_ || front_reads_late_) {
      const double frames = double(presented_ - presented_report_copies_);
      const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(front_pixels_saved_ - front_pixels_saved_previous_) / 1e6;
      BO2ZM_REPORT_RING("[native] C2 lazy front buffer: {} front buffer copies postponed since boot; Swaps "
                           "painted from the target {} and from a retained image {}; {} clears onto a spare image ({} "
                           "without a spare); recorded: {} when sampled, {} before the target was written again or "
                           "another resolve, {} at the Swap (FXAA, no ramp or another output); {} dropped without "
                           "copying (another resolve covers them whole): {:.2f} Mpixels per frame not copied (~{:.2f} "
                           "ms real); {} spare images; late reads {}{}",
                           front_postponed_, front_painted_target_, front_painted_retained_,
                           front_rotations_, front_no_spare_, front_copied_read_,
                           front_copied_write_, front_copied_swap_, front_replaced_, mp * perFrm,
                           mp * perFrm * 0.60, fronts_images_.size(), front_reads_late_,
                           front_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the same)");
      front_pixels_saved_previous_ = front_pixels_saved_;
    }
    ReportShadowMin();
    ReportClears();
    presented_report_copies_ = presented_;
    if (no_swap_[0] || no_swap_[1] || no_swap_[2]) {
      BO2ZM_REPORT_RING("[native] C2 depth copies without a swap since the previous report: {} because the command does "
                  "not clear the target, {} because it is not resolved whole, {} because it could not",
                  no_swap_[0], no_swap_[1], no_swap_[2]);
      no_swap_ = {};
    }
  }


  void ReadResolved(const CopyRegisters& reg, const Resolved& resolved, int32_t x0, int32_t y0,
                    uint32_t dx, uint32_t dy, uint32_t width, uint32_t height) {
    const int32_t max = REXCVAR_GET(bo2zm_native_read_resolved_texels);


    if (IsScaled(resolved.image)) {
      if (max > 0 && uint64_t(width) * height <= uint64_t(max)) {
        bo2zm::native::performance::g_scaled_readbacks_skipped.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    if (max <= 0 || uint64_t(width) * height > uint64_t(max) ||
        (resolved.image.format != kFormatColor && !Is16BitsPerChannel(resolved.image.format))) {
      return;
    }
    const uint64_t key_target =
        (uint64_t(reg.rb_copy_dest_base) << 28) ^ (uint64_t(width) << 14) ^ uint64_t(height);
    ReadTarget& target = reads_per_target_[key_target];
    target.base = reg.rb_copy_dest_base;
    target.width = width;
    target.height = height;
    const uint64_t every = uint64_t(std::max(REXCVAR_GET(bo2zm_native_reads_every), 1));
    if (target.copies++ % every != 0) {
      ++target.skipped;
      return;
    }
    const VkDeviceSize bytes = VkDeviceSize(width) * height * (Is16BitsPerChannel(resolved.image.format) ? 8 : 4);
    const uint64_t key = (uint64_t(reg.rb_copy_dest_base & 0x1FFFFFFF) << 32) ^ (uint64_t(uint32_t(x0)) << 16) ^
                           uint64_t(uint32_t(y0));


    Readback* pointer = &reads_[key | (uint64_t(slot_) << 61)];
    if (pointer->bytes < bytes && pointer->buffer != VK_NULL_HANDLE) {

      SubmitJob(true);
      WaitGpu();
      DestroyRead(*pointer);
      if (!Record()) {
        return;
      }
      pointer = &reads_[key | (uint64_t(slot_) << 61)];
      if (pointer->bytes < bytes && pointer->buffer != VK_NULL_HANDLE) {
        DestroyRead(*pointer);
      }
    }
    Readback& read = *pointer;
    if (read.bytes < bytes) {
      uint32_t type = 0;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              rex::ui::vulkan::util::MemoryPurpose::kReadback, read.buffer, read.guest_memory, &type)) {
        read = Readback{};
        Reject(16, "could not create a readback buffer for a resolved texture");
        return;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, read.guest_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        DestroyRead(read);
        Reject(16, "could not create a readback buffer for a resolved texture");
        return;
      }
      read.data = static_cast<uint8_t*>(mapped);
      read.bytes = bytes;
      read.coherent = (vulkan_device_->memory_types().host_coherent >> type) & 0x1;
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {int32_t(dx), int32_t(dy), 0};
    copy.imageExtent = {width, height, 1};
    BeforeImageTransfer(commands_job_);
    dfn_.vkCmdCopyImageToBuffer(commands_job_, resolved.image.image, VK_IMAGE_LAYOUT_GENERAL,
                                read.buffer, 1, &copy);
    reads_pending_.push_back({&read, reg.rb_copy_dest_base, x0, y0, width, height,
                                    reg.rb_copy_dest_pitch & 0x3FFF,
                                    (reg.rb_copy_dest_pitch >> 16) & 0x3FFF, reg.rb_copy_dest_info,
                                    resolved.image.format});
    if (reads_done_++ == 0) {
      REXLOG_INFO("[native] C2: readback of resolved textures of up to {} texels (the first: {:08X}, {}x{})",
                  max, reg.rb_copy_dest_base, width, height);
    }
  }


  void WriteRead64(const PendingRead& p, const Readback& read) {
    const uint32_t format = (p.info >> 7) & 0x3F;
    const uint32_t number = (p.info >> 13) & 0x7;
    const int32_t bias = int32_t(p.info << 10) >> 26;
    const bool swap = (p.info >> 24) & 0x1;
    const uint32_t order_copy = p.info & 0x7;
    const auto order = order_copy <= 3 ? static_cast<xenos::Endian>(order_copy) : xenos::Endian::k8in32;
    const float scale = std::ldexp(1.0f, bias);
    const bool float_value = format == uint32_t(xenos::ColorFormat::k_16_16_16_16_FLOAT);
    const auto pack = [&](float x) -> uint16_t {
      x *= scale;
      if (float_value) {
        return rex::float_to_xenos_half(x);
      }
      switch (number) {
        case 1:
          return uint16_t(int16_t(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f)));
        case 2:
          return uint16_t(std::lround(std::clamp(x, 0.0f, 65535.0f)));
        case 3:
          return uint16_t(int16_t(std::lround(std::clamp(x, -32768.0f, 32767.0f))));
        default:
          return uint16_t(std::lround(std::clamp(x, 0.0f, 1.0f) * 65535.0f));
      }
    };
    uint8_t* const physical = memory_->TranslatePhysical(0);
    const uint64_t base = uint64_t(p.base & 0x1FFFFFFF);


    {
      auto& last = reads64_report_[p.base];
      const auto now = std::chrono::steady_clock::now();
      if (now - last >= std::chrono::seconds(10)) {
        last = now;
        const size_t n = size_t(p.width) * p.height;
        if (format == uint32_t(xenos::ColorFormat::k_16_16_16_16) && number == 2 &&
            p.format_host == VK_FORMAT_R16G16B16A16_UNORM) {
          const auto s = bo2zm::native::MeasureUint16Readback(read.data, n);
          REXLOG_INFO("[native] BO2 luminance UINT16 {:08X} {}x{} bias {} host {}: {} texels; "
                      "mean ({:.6f},{:.6f},{:.6f},{:.6f}), min ({},{},{},{}), max ({},{},{},{}), "
                      "nonzero ({},{},{},{}) before endian/swap; no second bias applied",
                      p.base, p.width, p.height, bias, uint32_t(p.format_host), n,
                      s.Mean(0), s.Mean(1), s.Mean(2), s.Mean(3),
                      s.minimum[0], s.minimum[1], s.minimum[2], s.minimum[3],
                      s.maximum[0], s.maximum[1], s.maximum[2], s.maximum[3],
                      s.nonzero[0], s.nonzero[1], s.nonzero[2], s.nonzero[3]);
        } else {
          REXLOG_INFO("[native] BO2 64-bit readback {:08X} {}x{} format {} number {} bias {} host {}: "
                      "{} texels (not the UINT16 luminance statistics path)",
                      p.base, p.width, p.height, format, number, bias, uint32_t(p.format_host), n);
        }
      }
    }
    for (uint32_t j = 0; j < p.height; ++j) {
      const uint32_t ty = uint32_t(p.y0) + j;
      if (p.height_target && ty >= p.height_target) {
        break;
      }
      for (uint32_t i = 0; i < p.width; ++i) {
        const uint32_t tx = uint32_t(p.x0) + i;
        if (tx >= p.pitch) {
          break;
        }
        const uint64_t address =
            base + uint64_t(std::max(OffsetTiled2D(int32_t(tx), int32_t(ty), p.pitch, 3), 0));
        if (address + 8 > 0x20000000) {
          continue;
        }
        if (Is16BitsPerChannel(p.format_host)) {

          uint16_t v[4];
          std::memcpy(v, read.data + (size_t(j) * p.width + i) * 8, sizeof(v));
          if (swap) {
            std::swap(v[0], v[2]);
          }
          const uint32_t raw[2] = {xenos::GpuSwap(uint32_t(v[0]) | (uint32_t(v[1]) << 16), order),
                                      xenos::GpuSwap(uint32_t(v[2]) | (uint32_t(v[3]) << 16), order)};
          std::memcpy(physical + address, raw, sizeof(raw));
          continue;
        }
        const uint8_t* s = read.data + (size_t(j) * p.width + i) * 4;
        float c[4] = {s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f, s[3] / 255.0f};
        if (swap) {
          std::swap(c[0], c[2]);
        }
        const uint32_t words[2] = {
            xenos::GpuSwap(uint32_t(pack(c[0])) | (uint32_t(pack(c[1])) << 16), order),
            xenos::GpuSwap(uint32_t(pack(c[2])) | (uint32_t(pack(c[3])) << 16), order)};
        std::memcpy(physical + address, words, sizeof(words));
      }
    }
  }

  std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> reads64_report_;

  void WriteReads(std::vector<PendingRead>& reads) {
    const auto before_reads = std::chrono::steady_clock::now();
    if (before_reads - report_reads_ >= std::chrono::seconds(10)) {
      std::string list;
      for (auto it = reads_per_target_.begin(); it != reads_per_target_.end();) {
        ReadTarget& d = it->second;
        if (!d.copies) {
          it = reads_per_target_.erase(it);
          continue;
        }
        list += fmt::format(" {:08X} {}x{} {}/{};", d.base, d.width, d.height, d.copies - d.skipped,
                             d.copies);
        d.copies = 0;
        d.skipped = 0;
        ++it;
      }
      if (!list.empty()) {
        BO2ZM_REPORT_RING("[native] C2 readbacks per target (done/copies):{}", list);
      }
      report_reads_ = before_reads;
    }
    for (const PendingRead& p : reads) {
      const Readback& read = *p.read;
      if (!read.data) {
        continue;
      }
      ++reads_written_;
      texels_written_ += uint64_t(p.width) * p.height;
      if (!read.coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = read.guest_memory;
        range.size = VK_WHOLE_SIZE;
        dfn_.vkInvalidateMappedMemoryRanges(device_, 1, &range);
      }

      if (bo2zm_formats::Log2BytesCopy((p.info >> 7) & 0x3F) == 3) {
        WriteRead64(p, read);
        continue;
      }
      const bool swap = (p.info >> 24) & 0x1;
      const uint32_t order_copy = p.info & 0x7;
      const auto order =
          order_copy <= 3 ? static_cast<xenos::Endian>(order_copy) : xenos::Endian::k8in32;


      const uint32_t permutation =
          xenos::GpuSwap(swap ? uint32_t(0x03000102) : uint32_t(0x03020100), order);
      const uint8_t b0 = uint8_t(permutation), b1 = uint8_t(permutation >> 8),
                    b2 = uint8_t(permutation >> 16), b3 = uint8_t(permutation >> 24);
      uint8_t* const physical = memory_->TranslatePhysical(0);
      const uint64_t base = uint64_t(p.base & 0x1FFFFFFF);
      const auto& table = TableTiled2DTexel4();
      const uint64_t grid_tiles_per_row = ((p.pitch + 31) & ~uint32_t(31)) >> 5;
      for (uint32_t j = 0; j < p.height; ++j) {
        const uint32_t ty = uint32_t(p.y0) + j;
        if (p.height_target && ty >= p.height_target) {
          break;
        }
        const uint64_t row = base + ((uint64_t(ty >> 5) * grid_tiles_per_row) << 12);
        const uint16_t* const local = table.data() + size_t(ty & 31) * 32;
        const uint8_t* s = read.data + size_t(j) * p.width * 4;
        for (uint32_t i = 0; i < p.width; ++i, s += 4) {
          const uint32_t tx = uint32_t(p.x0) + i;
          if (tx >= p.pitch) {
            break;
          }
          const uint64_t address = row + (uint64_t(tx >> 5) << 12) + local[tx & 31];
          if (address + 4 > 0x20000000) {
            continue;
          }
          const uint8_t t0 = s[b0], t1 = s[b1], t2 = s[b2], t3 = s[b3];
          uint8_t* const d = physical + address;
          d[0] = t0;
          d[1] = t1;
          d[2] = t2;
          d[3] = t3;
        }
      }
    }
    reads.clear();
    ns_write_reads_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_reads).count());
  }

  void DestroyRead(Readback& read) {
    if (read.guest_memory != VK_NULL_HANDLE) {
      if (read.data) {
        dfn_.vkUnmapMemory(device_, read.guest_memory);
      }
      dfn_.vkFreeMemory(device_, read.guest_memory, nullptr);
    }
    if (read.buffer != VK_NULL_HANDLE) {
      dfn_.vkDestroyBuffer(device_, read.buffer, nullptr);
    }
    read = Readback{};
  }


  uint32_t TenthsFps() {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - fps_since_).count();
    if (seconds >= 0.5) {
      fps_tenths_ = uint32_t(double(presented_ - fps_presented_) * 10.0 / seconds + 0.5);
      fps_since_ = now;
      fps_presented_ = presented_;
    }
    return fps_tenths_;
  }

  bool PaintOutput(VulkanPresenter::VulkanGuestOutputRefreshContext& context, OwnedImage& source,
                    uint32_t width, uint32_t height, bool since_target = false) {


    const uint32_t s = output_no_wait_ ? (output_current_ + 1) % kSlotsOutput : 0;
    if (outputs_pending_[s]) {
      const auto before_wait = std::chrono::steady_clock::now();
      dfn_.vkWaitForFences(device_, 1, &fences_output_[s], VK_TRUE, UINT64_MAX);
      ns_wait_output_ += Ns(before_wait, std::chrono::steady_clock::now());
      ++waits_output_;
      dfn_.vkResetFences(device_, 1, &fences_output_[s]);
      outputs_pending_[s] = false;
    }
    output_current_ = s;

    if (ramp_gamma_) {
      const uint64_t version_postprocess = bo2zm::settings::VersionPostprocess();
      if (version_postprocess != version_postprocess_) {
        version_postprocess_ = version_postprocess;
        postprocess_ = bo2zm::settings::ReadPostprocess();
        RecomputeRamp();
        const auto& p = postprocess_;
        REXLOG_INFO("[native] C2: postprocess {}: brightness {:.2f}, contrast {:.2f}, saturation {:.2f}, vibrance "
                    "{:.2f}, temperature {:.2f}, gamma {:.2f}, tint {:.2f}/{:.2f}/{:.2f} at {:.2f}, vignette {:.2f}, "
                    "lines {:.2f}; {}",
                    p.active ? "on" : "off", p.brightness, p.contrast, p.saturation, p.vibrance,
                    p.temperature, p.gamma, p.tint_r, p.tint_g, p.tint_b, p.tint, p.vignette, p.lines,
                    grading_ ? "with per-pixel work (saturation, vibrance, vignette or lines)"
                                : "everything in the table, no per-pixel work");
      }
    }
    bo2zm::native::output::Crosshairs crosshairs;
    if (ramp_gamma_) {
      bo2zm::cheats::CrosshairOverlay records[4]{};
      const int count = bo2zm::cheats::GetCrosshairOverlays(records, 4);
      crosshairs = bo2zm::native::output::PackCrosshairs(records, count);
    }

    if (ramp_gamma_) {
      OutputRamp& ramp = ramps_output_[s];
      const bool table_changed = ramp.version != version_ramp_;
      const bool overlays_changed = std::memcmp(&ramp.crosshairs, &crosshairs, sizeof(crosshairs)) != 0;
      if (table_changed) std::memcpy(ramp.data, ramp_values_.data(), sizeof(ramp_values_));
      if (table_changed || overlays_changed) {
        std::memcpy(ramp.data + sizeof(ramp_values_), &crosshairs, sizeof(crosshairs));
        rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, ramp.guest_memory, ramp.type);
        ramp.version = version_ramp_;
        ramp.crosshairs = crosshairs;
      }
    }
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const auto& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == context.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      auto& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) {
        WaitOutputs();
        dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView view = context.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_output_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = width;
      info.height = height;
      info.layers = 1;
      if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = context.image_version();
      framebuffer = f.framebuffer;
    }

    VkDescriptorImageInfo info_image{};
    info_image.imageView = source.view;
    info_image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptors_output_[s];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &info_image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);



    dfn_.vkResetCommandPool(device_, pools_output_[s], 0);
    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(commands_output_[s], &start) != VK_SUCCESS) {
      return false;
    }
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_output_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent = {width, height};
    if (REXCVAR_GET(bo2zm_native_scene_sync) &&
        REXCVAR_GET(bo2zm_native_scene_output_read_barrier)) {


      if (REXCVAR_GET(bo2zm_native_scene_sync_precise)) {
        scene::precise_patch::BeforeOutputRead(dfn_.vkCmdPipelineBarrier, commands_output_[s], true);
      } else {
        scene::BeforeOutputRead(dfn_.vkCmdPipelineBarrier, commands_output_[s]);
      }
    } else if (since_target) {


      VkMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      dfn_.vkCmdPipelineBarrier(commands_output_[s],
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    dfn_.vkCmdBeginRenderPass(commands_output_[s], &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    dfn_.vkCmdSetViewport(commands_output_[s], 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, {width, height}};
    dfn_.vkCmdSetScissor(commands_output_[s], 0, 1, &scissor);
    VkPipeline pipeline = pipeline_;
    if (ramp_gamma_) {


      const bool exact = since_target || (width == source.width && height == source.height);
      const bool fxaa = !since_target && bo2zm::settings::AntialiasingFxaa();
      if (fxaa != fxaa_annotated_) {
        fxaa_annotated_ = fxaa;
        REXLOG_INFO("[native] C2: antialiasing at the output: {}", fxaa ? "FXAA" : "none");
      }
      pipeline = PipelineRamp((fxaa ? 4 : 0) + (grading_ ? 2 : 0) + (exact ? 1 : 0));
      if (pipeline == VK_NULL_HANDLE) {
        pipeline = pipelines_ramp_[exact ? 1 : 0];
      }
      if (!exact && !warned_ramp_bilinear_) {
        warned_ramp_bilinear_ = true;
        REXLOG_INFO("[native] C2: {}x{} output from a {}x{} source: ramp with bilinear sampling", width,
                    height, source.width, source.height);
      }
    }
    dfn_.vkCmdBindPipeline(commands_output_[s], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    dfn_.vkCmdBindDescriptorSets(commands_output_[s], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 layout_pipeline_, 0, 1, &descriptors_output_[s], 0, nullptr);
    const float rectangle[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
    dfn_.vkCmdPushConstants(commands_output_[s], layout_pipeline_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                            sizeof(rectangle), rectangle);
    struct {
      int32_t displacement[2];
      float inverse[2];
      uint32_t counter[4];
      uint32_t note[4];
      uint32_t banner[8];
      uint32_t banner_info[4];
    } bilinear = {{0, 0}, {1.0f / float(width), 1.0f / float(height)}, {}, {}, {}, {}};

    const uint32_t overlays = (ramp_gamma_ && REXCVAR_GET(bo2zm_show_fps) ? 1u : 0u) |
                              (ramp_gamma_ && crosshairs.Any() ? 2u : 0u);
    if (overlays) {
      bilinear.counter[0] = (overlays & 1u) ? TenthsFps() : 0;
      bilinear.counter[1] = std::max<uint32_t>(2, height / 240);
      bilinear.counter[2] = overlays;
    }
    if (ramp_gamma_ && bo2zm::preset::OnScreenNote(bilinear.note)) {
      bilinear.counter[3] = std::max<uint32_t>(2, height / 240);
    }
    if (ramp_gamma_ && bo2zm::preset::OnScreenBanner(bilinear.banner)) {
      bilinear.banner_info[0] = std::max<uint32_t>(3, height / 180);
    }
    dfn_.vkCmdPushConstants(commands_output_[s], layout_pipeline_, VK_SHADER_STAGE_FRAGMENT_BIT, 16,
                            sizeof(bilinear), &bilinear);
    dfn_.vkCmdDraw(commands_output_[s], 4, 1, 0, 0);
    dfn_.vkCmdEndRenderPass(commands_output_[s]);
    if (since_target) {


      dfn_.vkCmdPipelineBarrier(commands_output_[s], VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                nullptr, 0, nullptr, 0, nullptr);
    }
    if (dfn_.vkEndCommandBuffer(commands_output_[s]) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_output_[s];
    {
      const auto before_latch = std::chrono::steady_clock::now();
      const auto queue = vulkan_device_->AcquireQueue(family_, 0);
      const auto before_submit = std::chrono::steady_clock::now();
      if (dfn_.vkQueueSubmit(queue.queue(), 1, &submit, fences_output_[s]) != VK_SUCCESS) {
        return false;
      }
      ns_latch_output_ += Ns(before_latch, before_submit);
      ns_submit_output_ += Ns(before_submit, std::chrono::steady_clock::now());
      ++submits_output_;
    }
    outputs_pending_[s] = true;
    context.SetIs8bpc(true);
    return true;
  }

  struct Framebuffer {
    uint64_t version = UINT64_MAX;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };

  const VulkanDevice* vulkan_device_;
  const VulkanDevice::Functions& dfn_;
  VkDevice device_;
  rex::memory::Memory* memory_;
  uint32_t family_;

  std::array<VkCommandPool, kSlotsOutput> pools_output_{};
  FnCopyImage copy_image_ = nullptr;
  uint32_t reductions_ = 0;

  VkCommandBuffer commands_job_ = VK_NULL_HANDLE;
  VkCommandBuffer commands_upload_ = VK_NULL_HANDLE;


  std::array<WorkSlot, 4> slots_{};
  uint32_t slots_used_ = 3;
  uint32_t slot_ = 1;
  pass_profile::Scheduler pass_scheduler_;
  pixel_probe::Tag pixel_probe_frame_;
  pixel_probe::Counts pixel_probe_counts_;
  uint64_t submits_ = 0;
  std::array<VkCommandBuffer, kSlotsOutput> commands_output_{};
  std::array<VkFence, kSlotsOutput> fences_output_{};
  bool recording_ = false;
  std::array<bool, kSlotsOutput> outputs_pending_{};
  uint32_t output_current_ = 0;
  bool output_no_wait_ = false;
  bool marks_precise_ = false;
  bool marks_categories_ = true;
  int32_t alternate_marks_s_ = 0;
  std::chrono::steady_clock::time_point start_marks_{};
  bool invalidate_every_copy_ = false;
  bool recording_upload_ = false;
  uint64_t generation_commands_ = 0;
  FnClearDepth clear_depth_ = nullptr;

  using FnWriteMark = PFN_vkCmdWriteTimestamp;
  using FnReadQueries = VkResult(VKAPI_PTR*)(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t,
                                              void*, VkDeviceSize, VkQueryResultFlags);
  FnWriteMark write_mark_ = nullptr;
  FnReadQueries read_queries_ = nullptr;
  VkQueryPool queries_ = VK_NULL_HANDLE;


  VkQueryPool occlusions_ = VK_NULL_HANDLE;

  VkQueryPool stats_ = VK_NULL_HANDLE;
  std::array<uint64_t, kGpuCategories> fragments_category_{};
  std::array<uint64_t, kGpuCategories> vertices_category_{};
  std::array<uint64_t, kGpuCategories> primitives_category_{};
  uint64_t stats_no_room_ = 0;
  VkQueryPool stats_draw_ = VK_NULL_HANDLE;
  std::vector<uint64_t> fragments_per_shader_;
  std::vector<uint64_t> draws_per_shader_;
  uint64_t frames_diagnostic_ = 0;
  uint64_t stats_draw_no_room_ = 0;
  std::array<uint64_t, 3> no_swap_{};
  uint32_t warnings_no_swap_ = 0;
  std::array<uint64_t, kBucketsCopy> copies_bucket_{};
  std::array<uint64_t, kBucketsCopy> pixels_bucket_{};
  bool window_diagnostic_ = false;
  bool window_read_ = false;
  std::chrono::steady_clock::time_point last_window_ = std::chrono::steady_clock::now();
  bool occlusion_precise_ = false;
  VkQueryPool visibility_ = VK_NULL_HANDLE;
  uint64_t visibility_no_room_ = 0;
  uint64_t occlusion_open_ = 0;
  uint64_t next_occlusion_ = 1;
  std::unordered_map<uint64_t, GameOcclusionQuery> occlusions_game_;
  std::unordered_map<uint32_t, uint64_t> occlusion_per_base_;
  uint64_t stats_occlusion_[5] = {};
  double period_mark_ns_ = 0.0;
  uint64_t gpu_ns_ = 0;
  uint64_t gpu_jobs_ = 0;
  uint64_t gpu_jobs_categories_ = 0;
  uint64_t gpu_jobs_precise_ = 0;
  std::array<uint64_t, kGpuCategories> gpu_categories_ns_{};
  uint64_t mark_end_previous_ = 0;
  uint64_t last_mark_gpu_ = 0;

  uint64_t waits_output_ = 0, ns_wait_output_ = 0;
  uint64_t submits_job_ = 0, ns_latch_job_ = 0, ns_submit_job_ = 0;
  uint64_t submits_output_ = 0, ns_latch_output_ = 0, ns_submit_output_ = 0;
  uint64_t refreshes_ = 0, ns_before_call_ = 0, ns_in_call_ = 0, ns_after_call_ = 0;
  static uint64_t Ns(std::chrono::steady_clock::time_point since, std::chrono::steady_clock::time_point until) {
    return until > since ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(until - since).count()) : 0;
  }
  uint64_t overlaps_gpu_ = 0;
  std::array<uint64_t, kBucketsSwap> buckets_swap_{};
  mutable double worst_swap_ms_ = 0.0;
  std::chrono::steady_clock::time_point last_swap_{};

  FnBlit blit_ = nullptr;


  static constexpr uint32_t kSideShadows = 1600;
  bool depth_scalable_ = false;
  uint32_t scale_shadows_ = 0;

  bool scene_scale_ready_ = false;
  v041::SceneScale scene_scale_{};
  v041::Resolution scene_host_{};
  uint32_t scene_pct_ = 100;

  bool output_scale_ready_ = false;
  v041::SceneScale output_scale_{};
  v041::Resolution output_host_{1280, 720};
  uint32_t output_pitch_min_ = 1280;
  uint32_t output_pitch_max_ = 1280;
  uint32_t scene_pitch_min_ = 0;
  uint32_t scene_pitch_max_ = 0;


  std::unordered_map<const NativeImage*, DeferredClear> deferred_clears_;
  int deferred_clears_on_ = -1;
  int guest_clears_in_pass_on_ = -1;
  uint64_t guest_clears_in_pass_failed_ = 0;
  uint64_t deferred_clears_dropped_log_ = 0;
  uint64_t deferred_clears_in_pass_log_ = 0;
  uint64_t shadows_uploads_ = 0;
  std::vector<uint32_t> resolved_frame_;
  OwnedImage tiled_;
  std::chrono::steady_clock::time_point last_warning_tiled_{};

  std::unordered_map<uint64_t, Readback> reads_;
  std::unordered_map<uint64_t, ReadTarget> reads_per_target_;
  std::chrono::steady_clock::time_point report_reads_{};
  std::vector<PendingRead> reads_pending_;
  uint64_t reads_done_ = 0;

  uint64_t drawn_last_copy_ = 0;
  std::unordered_map<uint32_t, TargetCopy> copies_per_target_;
  std::chrono::steady_clock::time_point report_copies_{};
  VkFormat format_depth_ = VK_FORMAT_UNDEFINED;

  std::unique_ptr<VulkanDraws> draws_;

  std::unordered_map<uint64_t, OwnedImage> targets_;
  std::unordered_map<uint32_t, Resolved> resolved_;


  std::unordered_map<uint64_t, Resolved> parked_;
  bool park_resolved_ = REXCVAR_GET(bo2zm_native_park_resolves);
  uint64_t resolved_unparked_ = 0;

  struct DiagReader {
    uint32_t ps = 0;
    uint32_t reg_index = 0;
    uint32_t target = 0;
    uint32_t times = 0;
  };
  struct DiagWriteEntry {
    int32_t frame = -1;
    uint32_t order = 0;
    uint32_t source = 0;
    uint32_t requested_width = 0;
    uint32_t requested_height = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t draws = 0;
    bool depth = false;
    bool whole = false;
    uint32_t others = 0;
    std::vector<DiagReader> readers;
  };

  struct DiagWatched {
    uint32_t address = 0;
    uint32_t source = 0;
    bool pending = false;
    bool source_written = false;
    uint64_t writes = 0;
    uint64_t before = 0;
    uint64_t late = 0;
    uint32_t ps_before = 0;
    uint32_t ps_late = 0;
  };
  static constexpr uint32_t kReaderSwap = UINT32_MAX;
  static constexpr size_t kDiagMaxWrites = 32;
  static constexpr size_t kDiagMaxReaders = 12;
  static constexpr size_t kDiagMaxWatched = 4;
  static constexpr uint32_t kDiagRegSurfaceInfo = 0x2000;
  static constexpr uint32_t kDiagRegColorInfo = 0x2001;
  static constexpr uint32_t kDiagRegFetch = 0x4800;
  static constexpr uint32_t kDiagRegColorMask = 0x2104;
  static constexpr uint32_t kDiagRegsColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
  bool diag_window_ = false;
  uint32_t diag_frame_ = 0;
  uint64_t diag_windows_ = 0;
  uint64_t diag_writes_lost_ = 0;
  std::chrono::steady_clock::time_point diag_readers_last_ = std::chrono::steady_clock::now();
  std::unordered_map<uint32_t, std::vector<DiagWriteEntry>> diag_writes_;
  std::unordered_map<uint32_t, uint64_t> diag_signatures_;
  std::vector<DiagWatched> diag_watched_;

  struct PendingComposite {
    uint32_t address = 0;
    uint32_t source = 0;
    OwnedImage* target = nullptr;
    VkImage source_vk = VK_NULL_HANDLE;
    VkImage texture_vk = VK_NULL_HANDLE;
    VkImageCopy copy{};
  };
  static constexpr uint64_t kCompositeALook = 900;
  PendingComposite composite_;
  bool composite_has_ = false;
  bool composite_off_ = false;
  uint32_t composite_expired_ = 0;
  uint32_t composite_applying_ = 0;
  uint64_t composite_postponed_ = 0;
  uint64_t composite_recorded_read_ = 0;
  uint64_t composite_recorded_others_ = 0;
  uint64_t composite_dropped_source_ = 0;
  uint64_t composite_replaced_ = 0;
  uint64_t composite_reads_late_ = 0;
  uint64_t composite_mismatches_ = 0;
  uint64_t composite_pixels_saved_ = 0;
  uint64_t composite_pixels_previous_ = 0;
  uint64_t composite_presented_previous_ = 0;
  std::unordered_map<uint64_t, DepthTarget> depths_;
  uint64_t depth_draw_order_ = 0;
  uint64_t warnings_depth_banks_ = 0;

  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_descriptors_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_descriptors_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSlotsOutput> descriptors_output_{};
  VkShaderModule vs_ = VK_NULL_HANDLE;
  VkShaderModule fs_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_output_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;

  struct OutputRamp {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory guest_memory = VK_NULL_HANDLE;
    uint8_t* data = nullptr;
    uint32_t type = 0;
    uint64_t version = UINT64_MAX;
    bo2zm::native::output::Crosshairs crosshairs;
  };
  bool ramp_gamma_ = false;
  std::array<OutputRamp, kSlotsOutput> ramps_output_{};
  VkShaderModule fs_ramp_ = VK_NULL_HANDLE;


  std::array<VkPipeline, 8> pipelines_ramp_{};
  std::array<bool, 8> pipelines_ramp_failed_{};
  bool warned_ramp_bilinear_ = false;



  std::array<std::array<uint16_t, 3>, 256> ramp_game_ = [] {
    std::array<std::array<uint16_t, 3>, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t[i].fill(uint16_t(i * 0x3FF / 0xFF));
    }
    return t;
  }();


  static constexpr size_t kFloatsRamp = 256 * 4 + 8;
  std::array<float, kFloatsRamp> ramp_values_{};
  uint64_t version_ramp_ = 0;

  bo2zm::settings::Postprocess postprocess_{};
  uint64_t version_postprocess_ = 0;
  bool grading_ = false;
  bool fxaa_annotated_ = false;
  std::array<Framebuffer, VulkanPresenter::kMaxActiveGuestOutputImageVersions> framebuffers_{};
  size_t next_framebuffer_ = 0;

  std::chrono::steady_clock::time_point start_alternation_ = std::chrono::steady_clock::now();
  bool alternation_annotated_ = false;
  uint64_t swaps_ = 0;
  uint64_t restores_ = 0;
  uint64_t restores_for_resolve_ = 0;
  uint64_t resolve_no_source_ = 0;
  uint64_t resolve_content_old_ = 0;
  uint64_t swaps_no_clear_ = 0;
  uint64_t swaps_color_ = 0;

  std::unordered_map<const OwnedImage*, TargetState> state_target_;

  static constexpr uint32_t kConvSets = 256;
  VkShaderModule conv_fs_ = VK_NULL_HANDLE;
  VkDescriptorPool conv_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kConvSets> conv_sets_{};
  uint64_t conv_next_ = 0;
  bool conv_failed_ = false;
  std::unordered_map<VkFormat, VkRenderPass> conv_passes_;
  std::unordered_map<VkFormat, VkPipeline> conv_pipelines_;
  std::unordered_map<VkImageView, VkFramebuffer> conv_framebuffers_;
  uint64_t copies_converted_ = 0;
  std::unordered_set<uint64_t> conv_views_;

  std::unordered_set<uint64_t> alias_seen_;
  uint32_t alias_failed_ = 0;

  struct ResolvedVolume {
    OwnedImage image;
    uint32_t depth3d = 0;
    uint32_t pitch = 0;
    uint32_t pitch_flat = 0;
    uint32_t block = 0;
    uint64_t write = 0;
    VkImage source = VK_NULL_HANDLE;
    std::vector<VkImageCopy> regions;
    uint64_t last_use = 0;
  };
  static constexpr uint64_t kVolumeEagerFrames = 2;
  int volume_eager_on_ = -1;
  std::unordered_map<uint32_t, ResolvedVolume> volumes_;
  std::unordered_map<uint32_t, uint64_t> writes_resolved_;
  uint64_t counter_writes_ = 0;
  uint64_t volumes_done_ = 0;
  uint64_t clears_skipped_ = 0;
  uint64_t clears_skipped_depth_ = 0;
  uint64_t pixels_clears_skipped_ = 0;
  uint64_t clears_useless_ = 0;
  uint64_t pixels_clears_useless_ = 0;
  uint64_t restores_clipped_ = 0;
  uint64_t pixels_restored_ = 0;
  uint64_t pixels_restore_saved_ = 0;
  uint64_t loans_ = 0;
  uint64_t lent_read_ = 0;
  std::unordered_set<uint32_t> lent_;

  enum : uint32_t { kSmFree = 0, kSmAfterBase = 1, kSmOverlay = 2 };
  enum : uint32_t { kSmObserving = 0, kSmApplying = 1, kSmOff = 2 };
  static constexpr uint64_t kShadowMinCycles = 300;
  uint32_t sm_phase_ = kSmObserving;
  uint32_t sm_state_ = kSmFree;
  bool sm_on_ = false;
  bool sm_span_annotated_ = true;
  std::chrono::steady_clock::time_point sm_start_alternation_ = std::chrono::steady_clock::now();
  const OwnedImage* sm_target_cycle_ = nullptr;
  uint32_t sm_base_cycle_ = 0;
  bool sm_cycle_clean_ = false;
  bool sm_cycle_clear_ = false;
  bool sm_base_rewritten_ = false;
  const OwnedImage* sm_target_ = nullptr;
  uint32_t sm_base_ = 0;
  uint32_t sm_overlay_ = 0;
  bool sm_virtual_ = false;
  bool sm_virtual_valid_ = false;
  uint64_t sm_clean_consecutive_ = 0;
  uint64_t sm_cycles_ = 0;
  uint64_t sm_cycles_applied_ = 0;
  uint64_t sm_pixels_saved_ = 0;
  uint64_t sm_pixels_saved_previous_ = 0;
  uint64_t sm_draws_overlay_ = 0;
  uint64_t sm_draws_no_exact_ = 0;
  uint64_t sm_reads_min_ = 0;
  uint64_t sm_reads_yes_same_ = 0;
  uint64_t sm_reads_normal_ = 0;
  uint64_t sm_reads_a_offbeat_ = 0;
  uint64_t sm_reads_unable_ = 0;
  uint64_t sm_mismatches_ = 0;

  std::unordered_map<const OwnedImage*, ClearUse> use_clears_;
  bool diag_clears_ = REXCVAR_GET(bo2zm_native_diag_clears);
  std::chrono::steady_clock::time_point report_clears_{};
  uint64_t presented_report_clears_ = 0;

  bool clear_area_useful_ = REXCVAR_GET(bo2zm_native_clear_useful_area);
  bool area_useful_global_off_ = false;
  uint32_t bands_pending_ = 0;
  uint64_t area_useful_clears_ = 0;
  uint64_t area_useful_completed_ = 0;
  uint64_t area_useful_misses_ = 0;
  uint64_t area_useful_pixels_ = 0;
  uint64_t area_useful_pixels_previous_ = 0;
  static constexpr uint64_t kAreaUsefulCycles = 120;

  struct PendingCopy {
    const OwnedImage* source = nullptr;
    VkImage source_vk = VK_NULL_HANDLE;
    VkImage target_vk = VK_NULL_HANDLE;
    VkImageCopy copy{};
    uint64_t frame = 0;
    bool read_dead = false;
  };
  std::unordered_map<uint32_t, PendingCopy> pending_;
  std::unordered_map<uint32_t, uint64_t> last_read_live_;
  std::unordered_map<uint32_t, uint64_t> last_read_dead_;
  std::unordered_set<uint32_t> expired_;
  bool reads_depth_dead_ = false;
  bool lazy_off_ = false;
  uint64_t lazy_postponed_ = 0;
  uint64_t lazy_copied_read_ = 0;
  uint64_t lazy_copied_write_ = 0;
  uint64_t lazy_replaced_ = 0;
  uint64_t lazy_dropped_ = 0;
  uint64_t lazy_reads_late_ = 0;
  uint64_t lazy_pixels_saved_ = 0;
  uint64_t lazy_pixels_saved_previous_ = 0;
  static constexpr uint64_t kLazyFrames = 30;

  std::unordered_map<uint32_t, PendingFront> fronts_pending_;
  std::vector<FrontImage> fronts_images_;
  std::unordered_map<uint32_t, uint64_t> fronts_presented_;
  std::unordered_map<uint32_t, uint64_t> fronts_read_;

  std::unordered_map<const NativeImage*, FrontComposition> front_compositions_;
  std::unordered_set<uint32_t> front_composition_sampled_;
  uint64_t front_partial_clears_ = 0;
  bool front_partial_clear_failed_ = false;
  bool front_trace_active_ = false;
  uint64_t front_trace_lines_ = 0, front_trace_suppressed_ = 0;
  std::unordered_set<uint32_t> fronts_expired_;

  struct PerfLazy {
    OwnedImage* target = nullptr;
    VkImage source_vk = VK_NULL_HANDLE;
    VkImage texture_vk = VK_NULL_HANDLE;
    VkImageCopy copy{};
    uint64_t request = 0;
  };
  struct PerfAlias {
    uint32_t of = 0;
    VkImage of_image = VK_NULL_HANDLE;
    VkImage self_image = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    uint64_t request = 0;
    uint64_t twin_request = 0;
  };
  bool perf_fuse_ = false, perf_drop_fronts_ = false, perf_lazy_ = false, perf_alias_ = false;
  bool perf_lazy_off_ = false, perf_alias_off_ = false;

  struct PerfAbArm {
    double seconds = 0;
    uint64_t swaps = 0, gpu_ns = 0;
    uint32_t windows = 0;
    double fps_sum = 0, fps_sq = 0;
  };
  bool perf_base_drop_fronts_ = false, perf_base_lazy_ = false, perf_base_alias_ = false;
  bool perf_base_wait_unlocked_ = false, perf_ab_on_ = false, perf_base_fit_ = false;
  int32_t perf_ab_s_ = 0;
  uint64_t perf_ab_windows_ = 0, perf_ab_presented0_ = 0, perf_ab_gpu0_ = 0;
  std::chrono::steady_clock::time_point perf_ab_window_start_{};
  PerfAbArm perf_ab_arms_[2];
  uint64_t perf_epoch_ = 1;
  uint64_t perf_request_ = 0;
  uint64_t perf_last_request_ = 0;
  uint64_t perf_report_presented_ = 0;
  uint64_t perf_report_presented_frames_ = 0;
  uint32_t edram_mismatch_logged_ = 0;
  bool perf_base_edram_ = false;
  std::unordered_map<uint32_t, bo2zm::native::v046::FrontLearner> perf_front_learn_;
  std::unordered_map<uint32_t, PerfLazy> perf_lazy_pending_;
  std::unordered_set<uint32_t> perf_lazy_expired_;
  std::unordered_map<uint32_t, PerfAlias> perf_alias_live_;
  bo2zm::native::v046::CopyFacts perf_last_copy_{};
  VkImage perf_last_texture_ = VK_NULL_HANDLE;
  bool front_off_ = false;
  uint64_t front_postponed_ = 0;
  uint64_t front_painted_target_ = 0;
  uint64_t front_painted_retained_ = 0;
  uint64_t front_rotations_ = 0;
  uint64_t front_no_spare_ = 0;
  uint64_t front_copied_read_ = 0;
  uint64_t front_copied_write_ = 0;
  uint64_t front_copied_swap_ = 0;
  uint64_t front_replaced_ = 0;
  uint64_t front_reads_late_ = 0;
  uint64_t front_pixels_saved_ = 0;
  uint64_t front_pixels_saved_previous_ = 0;
  static constexpr uint64_t kFrontFrames = 8;
  static constexpr size_t kFrontImagesMax = 3;

  uint64_t pixels_restored_previous_ = 0;
  uint64_t pixels_restore_saved_previous_ = 0;
  uint64_t pixels_clears_skipped_previous_ = 0;
  uint64_t pixels_clears_useless_previous_ = 0;
  uint64_t presented_report_copies_ = 0;

  uint64_t hitch_copies_ = 0, hitch_clears_ = 0, hitch_resolves_ = 0;
  uint64_t hitch_restores_ = 0, hitch_waits_ = 0;

  uint64_t hitch_waits_ns_[bo2zm::waits::kNumTypes] = {};
  uint64_t hitch_waits_times_[bo2zm::waits::kNumTypes] = {};
  uint64_t hitch_ns_waits_gpu_ = 0, hitch_ns_wait_output_ = 0;
  uint64_t hitch_bytes_hash_ = 0, hitch_hashes_postponed_ = 0;
  uint64_t hitch_draws_ = 0, hitch_ns_ring_ = 0, hitch_ns_textures_ = 0;
  uint64_t hitch_textures_uploads_ = 0, hitch_bytes_uploaded_ = 0, hitch_textures_created_ = 0;
  uint64_t hitch_ns_hash_raw_ = 0, hitch_ns_hash_data_ = 0;
  uint64_t hitch_ns_waiting_copies_ = 0, hitch_waits_copies_ = 0;
  uint64_t hitch_ns_helping_copies_ = 0, hitch_copies_helped_ = 0;
  uint64_t hitch_ns_create_textures_ = 0, hitch_textures_bound_thread_ = 0;
  uint64_t hitch_ns_waiting_bindings_ = 0;
  uint64_t hitch_textures_hash_thread_ = 0, hitch_ns_hash_thread_ = 0, hitch_ns_hash_snapshot_ = 0;
  uint64_t hitch_textures_hash_ring_ = 0, hitch_ns_hash_ring_ = 0, hitch_ns_waiting_hashes_ = 0;

  uint64_t no_swap_no_transfer_dst_ = 0;
  uint64_t no_swap_target_no_prepare_ = 0;
  uint64_t no_swap_resolved_no_prepare_ = 0;
  uint64_t no_swap_sizes_different_ = 0;
  uint64_t hitch_gpu_ns_ = 0;
  uint64_t hitch_ns_record_ = 0;
  uint32_t warnings_hitch_ = 0;
  uint64_t copies_ = 0;
  uint64_t copies_depth_ = 0;
  uint64_t clears_depth_ = 0;
  uint64_t clears_in_pass_failed_ = 0;
  uint64_t clears_ = 0;
  uint64_t presented_ = 0;

  std::chrono::steady_clock::time_point fps_since_{};
  uint64_t fps_presented_ = 0;
  uint32_t fps_tenths_ = 0;
  uint64_t rejections_ = 0;
  std::unordered_set<uint32_t> warned_;
};

}

std::unique_ptr<NativeTargets> NativeTargets::Create(const VulkanDevice* vulkan_device,
                                                        rex::memory::Memory* guest_memory) {
  if (!vulkan_device || !guest_memory) {
    return nullptr;
  }
  auto targets = std::make_unique<VulkanTargets>(vulkan_device, guest_memory);
  if (!targets->Init()) {
    REXLOG_ERROR("[native] C2: could not prepare the target presentation");
    return nullptr;
  }
  return targets;
}

}
