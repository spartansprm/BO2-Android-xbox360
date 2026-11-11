// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <cstdint>
#include <mutex>

namespace bo2zm::android {
struct MouseCounts { int32_t x = 0, y = 0; };


class TouchLook {
  std::mutex mutex_;
  int owner_ = -1;
  float last_x_ = 0, last_y_ = 0, aspect_ = 1, dx_ = 0, dy_ = 0, fraction_x_ = 0, fraction_y_ = 0;
  bool new_gesture_ = false;
  void Reset() {
    owner_ = -1; dx_ = dy_ = fraction_x_ = fraction_y_ = 0; new_gesture_ = false;
  }
 public:
  void Cancel() { std::lock_guard lock(mutex_); Reset(); }
  void Touch(int action, int pointer, float x, float y, float width, float height) {
    std::lock_guard lock(mutex_);
    if (action == 3) { Reset(); return; }
    if (pointer < 0 || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
        !std::isfinite(height) || width < 1 || height < 1 || width > 100000 || height > 100000 ||
        std::abs(x) > 1024 || std::abs(y) > 1024) return;
    const float px = x * 1280.0f, py = y * 720.0f;
    if (action == 0) {
      if (owner_ != -1) return;
      owner_ = pointer; last_x_ = px; last_y_ = py;
      aspect_ = (width / height) * .5625f; new_gesture_ = true;
    } else if ((action == 1 || action == 2) && owner_ == pointer) {
      dx_ = std::fma(px - last_x_, aspect_, dx_);
      dy_ += py - last_y_; last_x_ = px; last_y_ = py;
      if (action == 2) owner_ = -1;
    }
  }
  MouseCounts Consume() {
    std::lock_guard lock(mutex_);
    if (new_gesture_) { fraction_x_ = fraction_y_ = 0; new_gesture_ = false; }

    const float x = dx_ * 1.5f + fraction_x_, y = dy_ * 1.5f + fraction_y_;
    dx_ = dy_ = 0;

    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 100000000 || std::abs(y) > 100000000) {
      Reset(); return {};
    }
    MouseCounts result{int32_t(x), int32_t(y)};
    fraction_x_ = x - float(result.x); fraction_y_ = y - float(result.y);
    if (owner_ == -1) fraction_x_ = fraction_y_ = 0;
    return result;
  }
};
inline TouchLook& RelativeTouch() { static TouchLook value; return value; }
}
