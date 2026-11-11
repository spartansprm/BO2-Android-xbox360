#pragma once

#include <rex/ui/vulkan/device.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bo2zm::native {


inline constexpr uint32_t kBlockPoolInvalid = UINT32_MAX;


inline constexpr uint64_t kUnitPoolBytes = 64ull * 1024ull;


struct TexturePoolState {
  bool active = false;
  uint32_t type_memory = UINT32_MAX;
  uint32_t slabs = 0;
  uint64_t bytes_reserved = 0;
  uint64_t bytes_in_use = 0;
  uint64_t bytes_free = 0;
  uint64_t bytes_larger_gap = 0;
  uint64_t textures_live = 0;
  uint64_t textures_placed = 0;
  uint64_t textures_dedicated = 0;
  uint64_t gaps_failed = 0;
  uint64_t slabs_in_hot = 0;
  uint64_t slabs_failed = 0;
};


class TexturePool {
 public:
  TexturePool() = default;
  ~TexturePool();

  TexturePool(const TexturePool&) = delete;
  TexturePool& operator=(const TexturePool&) = delete;


  bool Start(const rex::ui::vulkan::VulkanDevice* vulkan_device, int32_t mb_cache_max);


  void Finish();

  bool IsActive() const { return active_; }


  bool Reserve(const VkMemoryRequirements& requirements, VkDeviceMemory& memory_out,
                VkDeviceSize& offset_out, uint32_t& block_out);


  void Free(uint32_t block);


  void PerFrame(uint64_t frame);


  void AnnotateDedicated() { ++textures_dedicated_; }

  TexturePoolState State() const;


  std::string Summary() const;

 private:
  struct Slab {
    VkDeviceMemory guest_memory = VK_NULL_HANDLE;
    uint32_t units = 0;
    uint32_t units_in_use = 0;
    std::vector<uint64_t> occupied;
    std::vector<uint32_t> run_length;
  };


  static constexpr uint64_t kSlackBytes = 8ull << 20;

  static constexpr uint64_t kFramesBetweenSlabs = 120;

  static constexpr uint32_t kMaxSlabs = 255;

  bool CreateSlab(bool in_hot);
  bool ChooseMemoryType(uint32_t& type_out, uint64_t& alignment_view_out) const;

  const rex::ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;
  VkDevice device_ = VK_NULL_HANDLE;
  bool active_ = false;
  uint32_t type_memory_ = UINT32_MAX;
  uint64_t slab_bytes_ = 0;
  uint32_t slab_units_ = 0;
  uint32_t slabs_cap_ = 0;
  uint64_t last_growth_ = 0;
  std::vector<Slab> slabs_;

  uint64_t textures_live_ = 0;
  uint64_t textures_placed_ = 0;
  uint64_t textures_dedicated_ = 0;
  uint64_t gaps_failed_ = 0;
  uint64_t slabs_in_hot_ = 0;
  uint64_t slabs_failed_ = 0;
};

}
