#include "../app/src/native/render/bo2zm_map_pipeline_phase.h"

#include <cassert>
#include <chrono>

using bo2zm::native::map_pipeline::Counts;
using bo2zm::native::map_pipeline::Kind;
using bo2zm::native::map_pipeline::Monitor;
using bo2zm::native::map_pipeline::Phase;
using namespace std::chrono_literals;

int main() {
  Monitor meter;
  const Monitor::Time t{};
  assert(!meter.Observe(0, false, Counts{}, t));
  auto event = meter.Observe(1, false, Counts{100, 20, 20, 30, 80}, t);
  assert(event && event->kind == Kind::kMapSelected);
  assert(event->measured_phase == Phase::kLoading);
  assert(event->phase_total.first_requests == 0);


  event = meter.Observe(1, false, Counts{103, 22, 20, 33, 82}, t + 10s);
  assert(event && event->kind == Kind::kLoadingProgress);
  assert(event->since_last.first_requests == 3);
  assert(event->since_last.needs_compile == 2);
  assert(event->phase_total.skipped_draws == 3);
  assert(event->elapsed_ms == 10000);

  event = meter.Observe(1, true, Counts{108, 25, 22, 35, 85}, t + 12s);
  assert(event && event->kind == Kind::kLoadingComplete);
  assert(event->phase_total.first_requests == 8);
  assert(event->phase_total.needs_compile == 5);
  assert(event->phase_total.async_finished == 2);
  assert(event->since_last.first_requests == 5);


  assert(!meter.Observe(1, true, Counts{108, 25, 22, 35, 85}, t + 13s));
  event = meter.Observe(1, true, Counts{111, 27, 26, 42, 88}, t + 22s);
  assert(event && event->kind == Kind::kPlayingProgress);
  assert(event->phase_total.first_requests == 3);
  assert(event->phase_total.needs_compile == 2);
  assert(event->phase_total.async_finished == 4);
  assert(event->phase_total.skipped_draws == 7);

  event = meter.Observe(1, false, Counts{113, 28, 27, 44, 90}, t + 25s);
  assert(event && event->kind == Kind::kMatchEnded);
  assert(event->phase_total.first_requests == 5);
  assert(event->phase_total.skipped_draws == 9);
  assert(!meter.Observe(1, false, Counts{113, 28, 27, 44, 90}, t + 40s));


  event = meter.Observe(2, false, Counts{113, 28, 27, 44, 90}, t + 41s);
  assert(event && event->kind == Kind::kMapSelected);
  event = meter.Observe(2, true, Counts{113, 28, 27, 44, 92}, t + 43s);
  assert(event && event->kind == Kind::kLoadingComplete);
  assert(event->phase_total.first_requests == 0);
  assert(event->phase_total.list_prepared == 2);
  event = meter.Observe(2, false, Counts{113, 28, 27, 44, 93}, t + 55s);
  assert(event && event->kind == Kind::kMatchEnded);
  assert(event->phase_total.needs_compile == 0);
  assert(event->phase_total.skipped_draws == 0);


  event = meter.Observe(3, true, Counts{113, 28, 27, 44, 93}, t + 60s);
  assert(event && event->kind == Kind::kMapSelected);
  assert(event->measured_phase == Phase::kPlaying);
  assert(!meter.Observe(3, true, Counts{113, 28, 27, 44, 93}, t + 65s));


  const auto d = bo2zm::native::map_pipeline::Difference(Counts{}, Counts{1, 1, 1, 1, 1});
  assert(d.first_requests == 0 && d.needs_compile == 0 && d.skipped_draws == 0);
}
