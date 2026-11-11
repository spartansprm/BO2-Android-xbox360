#pragma once

#include "bo2zm_native_draws.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class Presenter;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace bo2zm::native {


struct CopyRegisters {
  uint32_t rb_surface_info = 0;
  uint32_t rb_color_info[4] = {};
  uint32_t rb_depth_info = 0;
  uint32_t rb_copy_control = 0;
  uint32_t rb_copy_dest_base = 0;
  uint32_t rb_copy_dest_pitch = 0;
  uint32_t rb_copy_dest_info = 0;
  uint32_t rb_color_clear = 0;
  uint32_t rb_color_clear_lo = 0;
  uint32_t rb_depth_clear = 0;
  uint32_t pa_sc_window_offset = 0;
  uint32_t pa_sc_window_scissor_tl = 0;
  uint32_t pa_sc_window_scissor_br = 0;
  uint32_t pa_su_sc_mode_cntl = 0;
  uint32_t pa_su_vtx_cntl = 0;

  uint32_t fetch_vertices[2] = {};


  uint32_t surface_real = 0;
};


struct DrawClear {
  uint32_t rb_surface_info = 0;
  uint32_t rb_color_info = 0;
  uint32_t rb_depth_info = 0;
  bool color = false;
  bool depth = false;
  bool stencil = false;

  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float z = 0.0f;
  uint32_t value_stencil = 0;
  float rgba[4] = {};
};


struct SwapTexture {
  uint32_t dword[6] = {};
};

class NativeTargets {
 public:

  static std::unique_ptr<NativeTargets> Create(const rex::ui::vulkan::VulkanDevice* vulkan_device,
                                                rex::memory::Memory* guest_memory);
  virtual ~NativeTargets() = default;


  virtual bool Copy(const CopyRegisters& registers) = 0;

  virtual void TraceFrontFrame(bool active) { (void)active; }

  virtual bool ClearAsDraw(const DrawClear& clear) {
    (void)clear;
    return false;
  }


  virtual bool Present(rex::ui::Presenter* ui_presenter, const SwapTexture& swap, uint32_t width,
                         uint32_t height) = 0;


  virtual void RampGamma(const std::array<std::array<uint16_t, 3>, 256>& ramp) = 0;


  virtual void BeginOcclusion(uint32_t base) = 0;
  virtual bool FinishOcclusion(uint32_t base, uint64_t& samples) = 0;


  virtual void StatsOcclusion(uint64_t values[5]) const = 0;


  virtual bool Draw(const DrawRequest& request) = 0;
  virtual DrawStats DrawsStats() const = 0;

  virtual void WaitUploads() = 0;

  virtual size_t CopiesPending() const { return 0; }

  virtual void Precompile() {}


  virtual uint64_t CopiesDepth() const = 0;

  virtual uint64_t ClearsDepth() const = 0;
  virtual void Stats(uint64_t& copies, uint64_t& clears, uint64_t& presented,
                            uint64_t& rejections) const = 0;

  virtual void TimeGpu(uint64_t& nanoseconds, uint64_t& jobs) const = 0;


  virtual uint64_t MarkGpuFinalNs() const = 0;

  virtual uint64_t JobsMarksPrecise() const = 0;

  virtual uint64_t JobsMarksCategories() const { return 0; }

  virtual void WaitsGpu(uint64_t& times, uint64_t& nanoseconds) const = 0;


  virtual void DurationJobsGpu(uint64_t& times, uint64_t& nanoseconds) const = 0;


  virtual void CostRecord(uint64_t cost[6]) const = 0;

  virtual void TimeGpuPerCategory(
      std::array<uint64_t, kGpuCategories>& nanoseconds) const = 0;


  virtual void StatsPipeline(std::array<uint64_t, kGpuCategories>& fragments,
                                    std::array<uint64_t, kGpuCategories>& vertices,
                                    std::array<uint64_t, kGpuCategories>& primitives) const = 0;


  virtual void CopiesPerSize(std::array<uint64_t, 4>& copies,
                               std::array<uint64_t, 4>& pixels) const = 0;
  virtual void StatsPerShader(std::vector<uint64_t>& fragments,
                                     std::vector<uint64_t>& draws,
                                     uint64_t& frames) const = 0;


  virtual void IntervalsBetweenSwaps(std::array<uint64_t, kBucketsSwap>& buckets, uint64_t& overlaps,
                                    double& worst_ms) const = 0;


  virtual void CostPresent(uint64_t cost[12]) const = 0;
};

}
