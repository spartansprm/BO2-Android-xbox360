// SPDX-License-Identifier: GPL-3.0-only
#include "bo2zm_native_pixel_probe.h"
#include <cassert>
#include <cmath>
#include <deque>
#include <iostream>
#include <thread>
#include <atomic>

using namespace bo2zm::native::pixel_probe;

static void TestClip() {
  struct Rect { struct { int x, y; } offset; struct { unsigned width, height; } extent; };
  Rect normal{{17, 29}, {896, 720}};
  assert(!Clip(normal, false) && normal.extent.width == 896 && normal.extent.height == 720);
  assert(Clip(normal, true) && normal.extent.width == 1 && normal.extent.height == 1);
  assert(normal.offset.x == 17 && normal.offset.y == 29);
  assert(!Clip(normal, true));
  Rect empty{{3, 7}, {0, 720}};
  assert(!Clip(empty, true) && empty.extent.width == 0 && empty.extent.height == 720);
  Rect empty_y{{3, 7}, {896, 0}};
  assert(!Clip(empty_y, true) && empty_y.extent.width == 896 && empty_y.extent.height == 0);
}

static void TestFullTrial(bool multiplayer) {
  Controller c;
  assert(c.Read().state == State::kIdle);


  assert(c.Start(multiplayer, !multiplayer, 2));
  assert(!c.Start(multiplayer, !multiplayer, 2));
  const uint64_t initial = 100 * kSecond, step = kSecond / 100;
  uint64_t now = initial, process = kSecond, ring = kSecond;
  Tag frame = c.Boundary(now, process, ring, {}, {}, true);
  std::deque<std::pair<uint64_t, Tag>> pending;
  for (uint64_t index = 1; index <= 13400; ++index) {
    Tag job = frame;
    if (c.Submitted(job, now + step / 10, false)) pending.emplace_back(index + 2, job);

    while (!pending.empty() && pending.front().first <= index) {
      const auto tag = pending.front().second;
      c.Complete(tag, true, tag.clip ? 5 * 1000000 : 45 * 1000000);
      pending.pop_front();
    }
    Counts counts{1000, 900, 100000, 90000, frame.clip ? 900u : 0u};
    now += step;
    process += 2 * step;
    ring += step / 5;
    frame = c.Boundary(now, process, ring, frame, counts, true);
  }
  const auto s = c.Read();
  assert(s.state == State::kComplete && !frame.trial && !frame.clip);
  for (size_t i = 0; i < s.blocks.size(); ++i) {
    const auto& b = s.blocks[i];
    assert(b.frames == 2499 && b.gpu_jobs == 2500 && b.gpu_pending == 0 && b.gpu_failed == 0);
    assert(b.wall_ns == b.frames * step);
    assert(b.process_cpu_ns == 2 * b.wall_ns && b.ring_cpu_ns == b.wall_ns / 5);
    assert(b.counts.draws == b.frames * 1000 && b.counts.scene_draws == b.frames * 900);
    assert(b.counts.scene_triangles == b.frames * 90000);
    const bool clipped = i == 1 || i == 2;
    assert(b.counts.clipped_draws == (clipped ? b.frames * 900 : 0));
    assert(b.gpu_ns == b.gpu_jobs * (clipped ? 5 * 1000000 : 45 * 1000000));
  }
  assert(s.process_cpu_available && s.ring_cpu_available);
  assert(kWarmup == 10 * kSecond && kBlock == 30 * kSecond &&
         kSettle == 5 * kSecond && kRestore == 3 * kSecond && kDuration == 133 * kSecond);
  const auto json = Json(s);
  assert(json.find("\"sequence\":\"ABBA\"") != std::string::npos);
  assert(json.find("\"state\":\"complete\"") != std::string::npos);
  std::cout << json << '\n';
}

static void TestTransitions() {
  Controller c;
  assert(!c.Start(false, false, 2));
  assert(!c.Start(true, false, 0));
  assert(!c.Start(true, false, 1));
  assert(c.Read().state == State::kIdle);
  c.Start(true, false, 2);
  const auto initial = c.Boundary(kSecond, 100, 100, {}, {}, true);
  auto a = c.Boundary(16 * kSecond, 200, 200, initial, {}, true);
  assert(a.phase == 1 && a.keep && !a.clip);
  Tag late = a;
  assert(c.Submitted(late, 17 * kSecond, false));
  auto b = c.Boundary(46 * kSecond, 300, 300, a, {}, true);
  assert(b.phase == 2 && b.keep && b.clip);
  c.Complete(late, true, 123456);
  assert(c.Read().blocks[0].gpu_ns == 123456 && c.Read().blocks[1].gpu_ns == 0);
  Tag mixed = b;
  assert(!c.Submitted(mixed, 47 * kSecond, true) && !mixed.keep);
  Tag crossing = b;
  assert(!c.Submitted(crossing, 72 * kSecond, false) && !crossing.keep);
  auto failure = b;
  assert(c.Submitted(failure, 48 * kSecond, false));
  c.Complete(failure, false, 9999);
  assert(c.Read().blocks[1].gpu_failed == 1 && c.Read().blocks[1].gpu_ns == 0);
  assert(c.Read().blocks[1].gpu_jobs_excluded == 2);
  c.Cancel();
  assert(c.Read().state == State::kCancelled);
  auto cancelled = c.Boundary(49 * kSecond, 400, 400, b, {}, true);
  assert(!cancelled.trial && !cancelled.clip);
  const auto old_trial = b.trial;
  assert(c.Start(true, false, 2));
  auto restarted = c.Boundary(50 * kSecond, 500, 500, {}, {}, true);
  assert(restarted.trial != old_trial && restarted.phase == 0 && !restarted.clip);
  c.Complete(late, true, 999999);
  assert(c.Read().blocks[0].gpu_ns == 0);
  c.Cancel("background_or_exit");
  assert(!c.Boundary(51 * kSecond, 600, 600, restarted, {}, true).clip);

  Controller paused;
  paused.Start(true, false, 2);
  auto start = paused.Boundary(1, 0, 0, {}, {}, true);
  auto jump = paused.Boundary(kDuration + 2, 0, 0, start, {}, true);
  assert(!jump.trial && paused.Read().state == State::kComplete);
  for (const auto& block : paused.Read().blocks) assert(!block.frames && !block.gpu_jobs);


  Controller no_clocks;
  no_clocks.Start(true, false, 2);
  auto n = no_clocks.Boundary(1, 0, 0, {}, {}, true);
  n = no_clocks.Boundary(16 * kSecond, 0, 0, n, {}, true);
  n = no_clocks.Boundary(16 * kSecond + 10000000, 0, 0, n, {}, true);
  assert(no_clocks.Read().blocks[0].frames == 1);
  assert(!no_clocks.Read().process_cpu_available && !no_clocks.Read().ring_cpu_available);
}

static void TestConcurrentPublication() {
  std::atomic<bool> done{false};
  std::thread writer([&]{
    for (int i = 0; i < 10000; ++i) {
      Start(true, false, 2);
      Boundary(uint64_t(i + 1), 0, 0, {}, {}, true);
      Cancel();
    }
    done = true;
  });
  uint64_t reads = 0;
  while (!done) {
    const auto s = Read();
    assert(s.phase <= 5);
    for (const auto& b : s.blocks) assert(!b.frames && !b.gpu_jobs && !b.gpu_pending);
    ++reads;
  }
  writer.join();
  std::cout << "Concurrent trial reads: " << reads << '\n';
}

int main() {
  TestClip(); TestFullTrial(false); TestFullTrial(true); TestTransitions(); TestConcurrentPublication();
  std::cout << "PASS: ABBA timing, settled intervals, delayed GPU tags, cancellation, retries, empty scissors, concurrency\n";
}
