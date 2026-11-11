#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace rex::ui {
class Presenter;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace bo2zm::native {

class ProgressScreen {
 public:
  ProgressScreen(rex::ui::Presenter* presenter, const rex::ui::vulkan::VulkanDevice* device);
  ~ProgressScreen();
  ProgressScreen(const ProgressScreen&) = delete;
  ProgressScreen& operator=(const ProgressScreen&) = delete;


  void Update(uint64_t done, uint64_t total);

  void Finish();

  uint32_t frames() const;

 private:
  struct Vulkan;
  void Draw(int percent, bool blank);
  std::unique_ptr<Vulkan> vk_;
  std::chrono::steady_clock::time_point last_{};
  bool drawn_ = false;
  bool captured_ = false;
};

}
