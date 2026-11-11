#pragma once

#include <rex/ui/vulkan/device.h>

#include "bo2zm_native_v043.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace bo2zm::native {


extern std::atomic<uint32_t> g_probe_vertex_address;

struct ShaderInput;


struct NativeImage {
  VkImage image = VK_NULL_HANDLE;


  VkDeviceMemory guest_memory = VK_NULL_HANDLE;


  uint32_t pool_block = 0xFFFFFFFFu;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;


  uint32_t width_guest = 0;
  uint32_t height_guest = 0;


  bool accepts_copy_target = true;
  VkFormat format = VK_FORMAT_UNDEFINED;
  bool prepared = false;


  bool swap_rb = false;


  bool content_invalid = false;
  uint32_t resolved_base = 0;


  uint8_t scale_class = 0;
};


inline constexpr uint32_t kGpuOthers = 0;
inline constexpr uint32_t kGpuShadows = 1;
inline constexpr uint32_t kGpuScene = 2;
inline constexpr uint32_t kGpuReflection = 3;
inline constexpr uint32_t kGpu320 = 4;
inline constexpr uint32_t kGpuSmaller = 5;
inline constexpr uint32_t kGpuCopies = 6;
inline constexpr uint32_t kGpuClears = 7;


inline constexpr uint32_t kGpuSceneNoDepth = 8;

inline constexpr uint32_t kGpuGapBetweenJobs = 9;
inline constexpr uint32_t kGpuCategories = 10;


inline constexpr uint32_t kBucketsSwap = 11;


inline constexpr uint32_t kStagesDraw = 12;


class TargetContext {
 public:
  virtual ~TargetContext() = default;

  virtual VkCommandBuffer CommandsJob() = 0;

  virtual VkCommandBuffer CommandsUpload() = 0;


  virtual uint32_t ShaderProbeMode() const { return 0; }
  virtual void ShaderProbeResult(uint32_t reason, uint32_t elements, bool replaced,
                                 bool fallback, uint64_t compile_ns, bool normal_miss = false) {}
  virtual uint32_t PixelProbeMode() const { return 0; }
  virtual void PixelProbeDraw(uint32_t category, uint32_t vertices, bool clipped) {
    (void)category; (void)vertices; (void)clipped;
  }

  virtual uint64_t GenerationCommands() const = 0;

  virtual NativeImage* TargetColor(uint32_t base, uint32_t format, uint32_t pitch) = 0;
  virtual void BeginColorPass(NativeImage& image, const uint32_t* registers, const ShaderInput* pixel_shader) {
    (void)image;
    (void)registers;
    (void)pixel_shader;
  }
  virtual NativeImage* TargetDepth(uint32_t base, uint32_t format, uint32_t pitch) = 0;

  virtual const NativeImage* TextureResolved(uint32_t address) = 0;


  virtual const NativeImage* TextureResolved3D(uint32_t address, uint32_t format, uint32_t width, uint32_t height,
                                                uint32_t depth3d, uint32_t pitch, bool tiled) {
    (void)address;
    (void)format;
    (void)width;
    (void)height;
    (void)depth3d;
    (void)pitch;
    (void)tiled;
    return nullptr;
  }


  virtual void DeadDepthReads(bool dead) { (void)dead; }


  virtual void TakeDeferredClears(const std::array<NativeImage*, 5>& images, uint32_t width, uint32_t height,
                                  std::array<bool, 5>& cleared, std::array<VkClearValue, 5>& values,
                                  std::array<v043::RectRequest, 5>& rects, std::array<VkClearValue, 5>& rect_values) {
    (void)images;
    (void)width;
    (void)height;
    (void)values;
    (void)rect_values;
    cleared.fill(false);
    rects.fill(v043::RectRequest{});
  }


  virtual void AnnotatePassArea(const NativeImage* image, uint32_t width, uint32_t height) {
    (void)image;
    (void)width;
    (void)height;
  }


  virtual bool OverlayShadowPass(const NativeImage* depth, bool only_depth) {
    (void)depth;
    (void)only_depth;
    return false;
  }


  virtual void OverlayShadowDraw(bool exact, uint32_t control_z) {
    (void)exact;
    (void)control_z;
  }

  virtual uint32_t AddressShadowOverlay() const { return 0; }


  virtual const NativeImage* PartnerShadowOverlay(bool capable, uint32_t ps) {
    (void)capable;
    (void)ps;
    return nullptr;
  }

  virtual bool SubmitYWait() = 0;


  virtual bool WaitGpuIdle() = 0;

  virtual void MarkGpu(uint32_t category) = 0;


  virtual void ProfilePassBegin(uint32_t category, uint32_t width, uint32_t height,
                                uint64_t target, const uint32_t* formats, uint32_t load_mode = 0) {}
  virtual void ProfilePassEnd() {}
  virtual void ProfilePassDraw(uint32_t vs, uint32_t ps, uint32_t elements) {}


  virtual uint32_t BeginQueryOcclusion() = 0;
  virtual void FinishQueryOcclusion(uint32_t index) = 0;


  virtual uint32_t BeginQueryVisibility(bool witness) {
    (void)witness;
    return UINT32_MAX;
  }
  virtual void FinishQueryVisibility(uint32_t index) { (void)index; }


  virtual uint32_t BeginStats(uint32_t category) = 0;
  virtual void FinishStats(uint32_t index) = 0;


  virtual uint32_t BeginStatsDraw(uint32_t label, uint32_t category) = 0;
  virtual void FinishStatsDraw(uint32_t index) = 0;
};

struct DrawRequest {
  const uint32_t* registers = nullptr;
  const ShaderInput* vs = nullptr;
  const ShaderInput* ps = nullptr;
  std::span<const uint32_t> vs_microcode;
  uint64_t generation_vs = 0;
  uint64_t generation_constants_vs = 0;
  uint64_t generation_constants_ps = 0;


  uint64_t generation_fetch = 0;
  uint64_t generation_framing = 0;


  uint16_t vegetation_game = 0;
};

struct DrawStats {
  uint64_t drawn = 0;
  uint64_t rejected = 0;
  uint64_t pipelines = 0;
  uint64_t textures = 0;
  uint64_t uploads_texture = 0;
  uint64_t megs_uploaded = 0;
  uint64_t megs_textures = 0;
  uint64_t ms_pipelines = 0;

  uint64_t passes = 0;
  uint64_t submits_full = 0;
  uint64_t ns_submits_full = 0;
  uint64_t bytes_vertices = 0;

  uint64_t dedupe_hits = 0;
  uint64_t dedupe_bytes = 0;
  uint64_t dedupe_collisions = 0;
  uint64_t bytes_indices = 0;
  uint64_t samplers = 0;
  uint64_t samplers_cache = 0;
  uint64_t ns_passes = 0;
  uint64_t ns_vertices = 0;
  uint64_t inputs_computed = 0;
  uint64_t ns_inputs = 0;
  uint64_t inputs_reused = 0;
  uint64_t passes_per_generation = 0;
  uint64_t passes_per_target = 0;
  uint64_t passes_resumed = 0;
  uint64_t ns_render_pass = 0;
  uint64_t texels_passes = 0;

  std::array<uint64_t, kGpuCategories> texels_per_category{};

  std::array<uint64_t, kGpuCategories> passes_per_category{};


  std::array<uint64_t, kGpuCategories> draws_per_category{};

  uint64_t draws_ps_useless = 0;
  uint64_t draws_ps_needed = 0;

  uint64_t shadows_alpha_active = 0;
  uint64_t shadows_alpha_off = 0;
  std::array<uint64_t, kGpuCategories> triangles_per_category{};


  uint64_t submits_ubo = 0;
  uint64_t submits = 0;


  uint64_t shared_observed = 0;
  uint64_t shared_changed = 0;
  uint64_t bytes_repeated_frame = 0;
  uint64_t bytes_equal_earlier = 0;
  uint64_t ns_hash_vertices = 0;

  std::vector<std::pair<uint32_t, uint64_t>> causes;


  std::array<uint64_t, kStagesDraw> stages_ns{};

  uint64_t scene_with_discard = 0;
  uint64_t scene_no_discard = 0;

  uint64_t drawn_timed = 0;


  uint64_t vegetation_soon = 0;
};

class VulkanDraws {
 public:

  static std::unique_ptr<VulkanDraws> Create(const rex::ui::vulkan::VulkanDevice* vulkan_device,
                                              rex::memory::Memory* guest_memory,
                                              TargetContext* context);
  virtual ~VulkanDraws() = default;

  virtual bool Draw(const DrawRequest& request) = 0;

  virtual void FinishPass() = 0;


  virtual bool PassActive() const { return false; }


  virtual bool ClearDepthInPass(VkCommandBuffer commands, const NativeImage& image,
                                       float depth, uint32_t stencil) = 0;


  virtual bool ClearColorInPass(VkCommandBuffer commands, const NativeImage& image, const VkClearColorValue& color,
                                 const VkRect2D& area) {
    (void)commands;
    (void)image;
    (void)color;
    (void)area;
    return false;
  }


  virtual bool ClearAttachmentInPass(VkCommandBuffer commands, const NativeImage& image, VkImageAspectFlags aspects,
                                   const VkClearValue& value, const VkRect2D& area) {
    (void)commands;
    (void)image;
    (void)aspects;
    (void)value;
    (void)area;
    return false;
  }


  virtual bool CanClearInActivePass(const NativeImage& image, VkImageAspectFlags aspects, const VkRect2D& area) const {
    (void)image;
    (void)aspects;
    (void)area;
    return false;
  }


  virtual bool ClearInActivePass(const NativeImage& image, VkImageAspectFlags aspects, const VkClearValue& value,
                                 const VkRect2D& area) {
    (void)image;
    (void)aspects;
    (void)value;
    (void)area;
    return false;
  }

  virtual void BeforeSubmit() = 0;


  virtual void UseSlot(uint32_t slot) = 0;

  virtual void ForgetImage(VkImage image) = 0;


  virtual void ForgetView(VkImageView view) { (void)view; }


  virtual bool ReleaseTexturesOnOutOfMemory() = 0;

  virtual void InvalidateTextures() = 0;


  virtual void InvalidateTexturesIn(uint32_t address) = 0;


  virtual void InvalidateImages(VkImage a, VkImage b) = 0;
  virtual DrawStats Stats() const = 0;

  virtual uint64_t Drawn() const = 0;

  virtual void WaitUploads() = 0;

  virtual size_t CopiesPending() const { return 0; }


  virtual void OcclusionOpen(bool open) = 0;


  virtual void Precompile() {}
};


extern std::atomic<uint32_t> g_syncs_ring;


extern std::atomic<bool> g_syncs_on_flush_waits;


void ReportDeferred(std::string line);


#define BO2ZM_REPORT_RING(...) ::bo2zm::native::ReportDeferred(fmt::format(__VA_ARGS__))

}
