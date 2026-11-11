// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <android/native_window.h>
#include <rex/ui/surface.h>
namespace rex::ui {
class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window) : window_(window) { ANativeWindow_acquire(window_); }
  ~AndroidNativeWindowSurface() override { ANativeWindow_release(window_); }
  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }
 protected:
  bool GetSizeImpl(uint32_t& width, uint32_t& height) const override;
 private:
  ANativeWindow* window_;
};
}
