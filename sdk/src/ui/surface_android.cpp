// SPDX-License-Identifier: BSD-3-Clause
#include <rex/ui/surface_android.h>
namespace rex::ui {
bool AndroidNativeWindowSurface::GetSizeImpl(uint32_t& width, uint32_t& height) const {
  const int w = ANativeWindow_getWidth(window_), h = ANativeWindow_getHeight(window_);
  if (w <= 0 || h <= 0) { width = height = 0; return false; }
  width = static_cast<uint32_t>(w); height = static_cast<uint32_t>(h); return true;
}
}
