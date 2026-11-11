// SPDX-License-Identifier: GPL-3.0-only
#include "bo2zm_native_performance.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <iostream>

int main() {
  using namespace bo2zm::native::performance;
  assert(Read().sampled_ns == 0);
  std::atomic<bool> done = false;
  std::thread writer([&] {
    for (uint64_t i = 1; i <= 100000; ++i) {
      Window window;
      window.swaps = i;
      window.jobs = i * 3;
      window.category_jobs = i * 2;
      window.gpu_ms_per_swap = double(i);
      window.gpu_gap_ms_per_swap = double(i) / 2;
      Publish(window);
    }
    done = true;
  });
  uint64_t checks = 0;
  while (!done) {
    const auto window = Read();
    if (window.sampled_ns) {
      assert(window.jobs == window.swaps * 3);
      assert(window.category_jobs == window.swaps * 2);
      assert(window.gpu_ms_per_swap == double(window.swaps));
      assert(window.gpu_gap_ms_per_swap == double(window.swaps) / 2);
      assert(window.sampled_ns <= NowNs());
      ++checks;
    }
  }
  writer.join();
  assert(Read().swaps == 100000);
  std::cout << "100000 publications; " << checks
            << " coherent concurrent GPU-window reads; empty baseline valid\n";
}
