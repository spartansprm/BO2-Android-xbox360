#include "vkstub.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define REXLOG_INFO(...) (void)0
#define REXLOG_WARN(...) (void)0
#define REXLOG_ERROR(...) (void)0
#define REXCVAR_GET(x) (cvar_##x)
inline bool cvar_bo2zm_native_guest_clears_in_pass = true, cvar_bo2zm_native_volume_eager = true;
#include "bo2zm_native_performance.h"
#include "bo2zm_native_v040.h"
#include "bo2zm_native_v043.h"
namespace bo2zm_formats { inline uint32_t Storage(uint32_t f){return f;} inline bool ColorApprox(uint32_t){return true;} }
namespace bo2zm::native {
#include "gen/nativeimage.inc"
using OwnedImage = NativeImage;
struct DepthTarget : OwnedImage { uint64_t draw_frame = 0; uint64_t draw_order = 0; };
struct Resolved { OwnedImage image; };
struct DrawClear { uint32_t rb_surface_info=0, rb_color_info=0, rb_depth_info=0; bool color=false, depth=false, stencil=false;
  int32_t x0=0,y0=0,x1=0,y1=0; float z=0; uint32_t value_stencil=0; float rgba[4]={}; };
struct DeferredClear { VkImage handle = VK_NULL_HANDLE; VkClearValue value{}; bool depth=false, in_pass=false; bool rect=false;
  VkImageAspectFlags aspects=0; VkRect2D area{}; };
constexpr VkImageSubresourceRange kRangeColor{1,0,1,0,1}, kRangeDepth{6,0,1,0,1};
constexpr VkFormat kFormatColor = VK_FORMAT_R8G8B8A8_UNORM;
enum : uint32_t { kGpuClears = 3, kGpuCopies = 2 };
struct VulkanDraws {
  virtual ~VulkanDraws() = default;
  virtual void FinishPass() = 0;
  virtual bool PassActive() const { return false; }
  virtual bool ClearDepthInPass(VkCommandBuffer, const NativeImage&, float, uint32_t) = 0;
  virtual bool ClearAttachmentInPass(VkCommandBuffer, const NativeImage&, VkImageAspectFlags, const VkClearValue&, const VkRect2D&) { return false; }
  virtual bool CanClearInActivePass(const NativeImage&, VkImageAspectFlags, const VkRect2D&) const { return false; }
  virtual bool ClearInActivePass(const NativeImage&, VkImageAspectFlags, const VkClearValue&, const VkRect2D&) { return false; }
};
struct TargetContext {
  virtual ~TargetContext() = default;
  virtual void TakeDeferredClears(const std::array<NativeImage*, 5>& images, uint32_t width, uint32_t height,
                                  std::array<bool, 5>& cleared, std::array<VkClearValue, 5>& values,
                                  std::array<v043::RectRequest, 5>& rects, std::array<VkClearValue, 5>& rect_values) {
    (void)images; (void)width; (void)height; (void)values; (void)rect_values; cleared.fill(false); rects.fill(v043::RectRequest{}); }
  virtual void AnnotatePassArea(const NativeImage*, uint32_t, uint32_t) {}
  virtual void ProfilePassBegin(uint32_t, uint32_t, uint32_t, uint64_t, const uint32_t*) {}
  virtual uint32_t BeginStats(uint32_t) { return 0; }
  virtual bool ClearAsDraw(const DrawClear&) { return false; }
};
class VulkanTargets final : public TargetContext {
 public:
  struct FrontComposition { uint32_t address = 0; uint64_t write_frame = 0; };
  struct PendingCopy { const OwnedImage* source=nullptr; VkImage source_vk=VK_NULL_HANDLE; };
  struct PendingFront { int32_t retained=-1; VkImage source_vk=VK_NULL_HANDLE; };
  struct PendingComposite { uint32_t address=0; OwnedImage* target=nullptr; VkImage source_vk=VK_NULL_HANDLE; };
  struct ResolvedVolume { OwnedImage image; uint32_t depth3d=0, pitch=0, pitch_flat=0; uint64_t write=0; VkImage source=VK_NULL_HANDLE;
    std::vector<VkImageCopy> regions; uint64_t last_use=0; };
  static constexpr uint64_t kVolumeEagerFrames = 2;
  VulkanDraws* draws_ = nullptr; Dfn dfn_{}; VkCommandBuffer commands_job_{};
  void (*copy_image_)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy*) = nullptr;
  void (*clear_depth_)(VkCommandBuffer, VkImage, VkImageLayout, const VkClearDepthStencilValue*, uint32_t, const VkImageSubresourceRange*) = nullptr;
  std::unordered_map<uint64_t, OwnedImage> targets_; std::unordered_map<uint64_t, DepthTarget> depths_;
  std::unordered_map<const NativeImage*, FrontComposition> front_compositions_;
  std::unordered_map<const NativeImage*, DeferredClear> deferred_clears_;
  std::unordered_map<uint32_t, PendingCopy> pending_; std::unordered_map<uint32_t, PendingFront> fronts_pending_;
  bool composite_has_ = false; PendingComposite composite_;
  std::unordered_map<uint32_t, Resolved> resolved_; std::unordered_map<uint32_t, uint64_t> writes_resolved_;
  std::unordered_map<uint32_t, ResolvedVolume> volumes_; std::unordered_set<uint32_t> lent_;
  uint64_t presented_=0, clears_=0, clears_depth_=0, depth_draw_order_=0, volumes_done_=0, clears_in_pass_failed_=0,
           deferred_clears_dropped_log_=0, guest_clears_in_pass_failed_=0;
  bool recording_ = false; int guest_clears_in_pass_on_=-1, volume_eager_on_=-1;
  bool Record() { return true; } void MarkGpu(uint32_t) {} void BeforeImageTransfer(VkCommandBuffer) {}
  bool DeferredClearsOn() { return true; } bool FrontTraceLine() { return false; }
  void ForgetClear(const OwnedImage&) {}
  void FlushDeferredClear(const NativeImage& image) { if (deferred_clears_.erase(&image)) ++flushes; }
  bool DeferClear(OwnedImage& image, const VkClearValue& value, bool depth, bool in_pass) {
    DeferredClear& d = deferred_clears_[&image]; d = {}; d.handle = image.image; d.value = value; d.depth = depth; d.in_pass = in_pass;
    return true; }
  int target_calls = 0, flushes = 0;
  NativeImage* TargetColor(uint32_t base, uint32_t format, uint32_t pitch) { ++target_calls;
    return &targets_.at((uint64_t(base) << 20) | (uint64_t(format) << 16) | pitch); }
  NativeImage* TargetDepth(uint32_t base, uint32_t format, uint32_t pitch) { ++target_calls;
    return &depths_.at((uint64_t(base) << 20) | (uint64_t(format) << 16) | pitch); }
#include "gen/tfuncs.inc"
};
class Draws final : public VulkanDraws {
 public:
  enum : uint32_t { kSkyAtPassEnd = 2, kLoadRead = 0, kLoadIgnore = 1 };
  bool pass_active_=false, sky_pending_=false, area_util_=true, time_=false; VkCommandBuffer pass_commands_{};
  std::array<const NativeImage*, 5> pass_images_{}; std::array<VkImage, 5> pass_handles_{};
  uint32_t pass_width_=0, pass_area_height_=0, pass_formats_[5]={}, query_occlusion_=UINT32_MAX, category_pass_=0, stats_pass_=0;
  Dfn dfn_{}; TargetContext* context_=nullptr; std::unordered_map<uint32_t, uint32_t> height_useful_;
  void EmitSkyPostponed(VkCommandBuffer, uint32_t) { sky_pending_ = false; }
  int finishes = 0, separate = 0, in_pass_clears = 0;
  void FinishPass() override { if (pass_active_) { ++finishes; pass_active_ = false; } }
  bool ClearAttachmentInPass(VkCommandBuffer, const NativeImage&, VkImageAspectFlags, const VkClearValue&, const VkRect2D&) override {
    ++separate; return true; }
  bool PassActive() const override { return pass_active_; }
  bool ClearDepthInPass(VkCommandBuffer, const NativeImage&, float, uint32_t) override { return true; }
#include "gen/dfuncs.inc"
  void BeginPassBlock(std::array<NativeImage*, 5> images, uint32_t width, uint32_t height, uint32_t pitch, uint64_t key_pass,
                      bool shadows_no_load, bool depth_only_no_load) {
    uint32_t formats[5] = {};
    for (uint32_t i = 0; i < 5; ++i) formats[i] = images[i] ? uint32_t(images[i]->format) : 0;
#include "gen/beginpass.inc"
    (void)pass; (void)framebuffer; (void)before_start; (void)mode_load;
  }
};
}

#include <cstdio>
using namespace bo2zm::native;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static int g_clear_attachments = 0, g_barriers = 0;
static VkClearAttachment g_last_att{}; static VkClearRect g_last_rect{};
static void CA(VkCommandBuffer, uint32_t, const VkClearAttachment* a, uint32_t, const VkClearRect* r) { ++g_clear_attachments; g_last_att=*a; g_last_rect=*r; }
static void PB(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const void*, uint32_t, const VkImageMemoryBarrier*) { ++g_barriers; }
static VkRenderPassBeginInfo g_begin{};
static void BRP(VkCommandBuffer, const VkRenderPassBeginInfo* b, VkSubpassContents) { g_begin = *b; }
int main() {
  VulkanTargets t; Draws d;
  t.draws_ = &d; t.recording_ = true; t.dfn_.vkCmdPipelineBarrier = PB;
  d.dfn_.vkCmdClearAttachments = CA; d.dfn_.vkCmdBeginRenderPass = BRP; d.context_ = &t;
  const uint32_t pitch = 880;
  const uint64_t ckey = (uint64_t(0) << 20) | (uint64_t(6) << 16) | pitch, dkey = (uint64_t(0x200) << 20) | (uint64_t(1) << 16) | pitch;
  OwnedImage& color = t.targets_[ckey]; color.image = (VkImage)0x10; color.view = (VkImageView)0x11; color.width = 880; color.height = 768;
  color.prepared = true; color.format = VK_FORMAT_R8G8B8A8_UNORM;
  DepthTarget& depth = t.depths_[dkey]; depth.image = (VkImage)0x20; depth.view = (VkImageView)0x21; depth.width = 880; depth.height = 768;
  depth.prepared = true; depth.format = VkFormat(129); depth.accepts_copy_target = false;
  DrawClear b; b.rb_surface_info = pitch; b.rb_color_info = (6u << 16) | 0; b.rb_depth_info = (1u << 16) | 0x200;
  b.color = true; b.depth = true; b.stencil = true; b.x0 = 0; b.y0 = 0; b.x1 = 880; b.y1 = 720; b.rgba[0] = 0.5f; b.z = 1.0f;

  d.pass_active_ = true; d.pass_commands_ = (VkCommandBuffer)0x1; d.pass_width_ = 880; d.pass_area_height_ = 720;
  d.pass_images_ = {&color, nullptr, nullptr, nullptr, &depth}; d.pass_handles_ = {color.image, nullptr, nullptr, nullptr, depth.image};
  d.pass_formats_[0] = 37; d.pass_formats_[4] = 129; d.sky_pending_ = true;
  CHECK(t.ClearAsDraw(b));
  CHECK(d.finishes == 0 && g_clear_attachments == 2 && t.target_calls == 0 && d.separate == 0 && !d.sky_pending_);
  CHECK(g_last_att.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && g_last_att.colorAttachment == 0 && g_last_rect.rect.extent.height == 720);
  CHECK(t.clears_ == 1 && t.clears_depth_ == 1 && depth.draw_order == 1);

  g_clear_attachments = 0; b.y1 = 768; t.target_calls = 0;
  CHECK(t.ClearAsDraw(b));
  CHECK(d.finishes == 1 && g_clear_attachments == 0 && t.target_calls == 2 && d.separate == 0);

  CHECK(t.deferred_clears_.size() == 2 && !t.deferred_clears_[&color].rect && !t.deferred_clears_[&depth].rect);
  CHECK(t.deferred_clears_[&depth].in_pass);
  t.deferred_clears_.clear();

  OwnedImage other; other.image = (VkImage)0x30;
  d.pass_active_ = true; d.pass_images_ = {&other, nullptr, nullptr, nullptr, nullptr}; d.pass_handles_ = {other.image, nullptr, nullptr, nullptr, nullptr};
  b.color = false; b.stencil = false; b.y1 = 720;
  CHECK(t.ClearAsDraw(b));
  CHECK(d.finishes == 2 && t.deferred_clears_.size() == 1 && t.deferred_clears_[&depth].rect &&
        t.deferred_clears_[&depth].aspects == VK_IMAGE_ASPECT_DEPTH_BIT);

  b.color = true; b.depth = false;
  CHECK(t.ClearAsDraw(b) && t.deferred_clears_.size() == 2 && t.deferred_clears_[&color].rect);


  g_clear_attachments = 0; g_barriers = 0;
  d.height_useful_[pitch] = 720;
  std::array<NativeImage*, 5> images{&color, nullptr, nullptr, nullptr, &depth};
  d.pass_active_ = false;
  d.BeginPassBlock(images, 880, 768, pitch, 0, false, false);
  CHECK(t.deferred_clears_.empty());
  CHECK(g_barriers == 1);
  CHECK(g_begin.renderArea.extent.width == 880 && g_begin.renderArea.extent.height == 720);
  CHECK(g_begin.clearValueCount == 1 && g_begin.pClearValues != nullptr);
  CHECK(g_clear_attachments == 1 && g_last_att.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT && g_last_rect.rect.extent.height == 720);
  CHECK(d.pass_area_height_ == 720 && d.pass_images_[4] == &depth);

  d.pass_active_ = true; d.query_occlusion_ = 5; d.pass_commands_ = (VkCommandBuffer)0x1; d.pass_width_ = 880;
  d.pass_images_ = {&color, nullptr, nullptr, nullptr, &depth}; d.pass_handles_ = {color.image, nullptr, nullptr, nullptr, depth.image};
  const int before = d.finishes; g_clear_attachments = 0;
  CHECK(t.ClearAsDraw(b) && d.finishes == before + 1 && g_clear_attachments == 0);
  d.query_occlusion_ = UINT32_MAX; t.deferred_clears_.clear();

  d.pass_active_ = true; b.color = false; b.depth = true; t.pending_[0x1000].source_vk = depth.image;
  CHECK(t.ClearAsDraw(b) && d.finishes == before + 2);
  t.pending_.clear(); t.deferred_clears_.clear();

  cvar_bo2zm_native_guest_clears_in_pass = false; VulkanTargets t2; t2.draws_ = &d; t2.recording_ = true; t2.dfn_.vkCmdPipelineBarrier = PB;
  t2.targets_ = t.targets_; t2.depths_ = t.depths_;
  OwnedImage& c2 = t2.targets_[ckey];
  d.pass_active_ = true; d.pass_images_ = {&c2, nullptr, nullptr, nullptr, nullptr}; d.pass_handles_ = {c2.image, nullptr, nullptr, nullptr, nullptr};
  b.color = true; b.depth = false; g_barriers = 0; const int sep = d.separate;
  CHECK(t2.ClearAsDraw(b) && d.separate == sep + 1 && g_barriers == 2 && t2.deferred_clears_.empty());
  std::printf(fails ? "harness: %d FAILURES\n" : "harness: all functional checks passed%.0d\n", fails);
  return fails != 0;
}
