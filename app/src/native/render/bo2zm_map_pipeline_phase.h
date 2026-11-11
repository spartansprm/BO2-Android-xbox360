// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace bo2zm::native::map_pipeline {

struct Counts {
  uint64_t first_requests = 0;
  uint64_t needs_compile = 0;
  uint64_t async_finished = 0;
  uint64_t skipped_draws = 0;
  uint64_t list_prepared = 0;
};

inline Counts Difference(const Counts& now, const Counts& before) {
  const auto diff = [](uint64_t a, uint64_t b) { return a >= b ? a - b : 0; };
  return {diff(now.first_requests, before.first_requests),
          diff(now.needs_compile, before.needs_compile),
          diff(now.async_finished, before.async_finished),
          diff(now.skipped_draws, before.skipped_draws),
          diff(now.list_prepared, before.list_prepared)};
}

enum class Phase : uint8_t { kNone, kLoading, kPlaying, kLobby };
enum class Kind : uint8_t { kMapSelected, kLoadingProgress, kLoadingComplete, kPlayingProgress, kMatchEnded };

struct Observation {
  Kind kind{};
  Phase measured_phase{};
  uint64_t generation = 0;
  uint64_t elapsed_ms = 0;
  Counts phase_total{};
  Counts since_last{};
};

class Monitor {
 public:
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;

  std::optional<Observation> Observe(uint64_t generation, bool playing, const Counts& now, Time t) {
    if (generation == 0) {
      return std::nullopt;
    }
    if (generation != generation_) {
      generation_ = generation;
      phase_ = playing ? Phase::kPlaying : Phase::kLoading;
      started_ = last_report_ = t;
      phase_first_ = previous_ = now;
      return Observation{Kind::kMapSelected, phase_, generation_, 0, {}, {}};
    }
    if (phase_ == Phase::kLoading && playing) {
      const Observation result = Snapshot(Kind::kLoadingComplete, now, t);
      SwitchTo(Phase::kPlaying, now, t);
      return result;
    }
    if (phase_ == Phase::kPlaying && !playing) {
      const Observation result = Snapshot(Kind::kMatchEnded, now, t);
      SwitchTo(Phase::kLobby, now, t);
      return result;
    }
    const auto elapsed = t - last_report_;
    if ((phase_ == Phase::kLoading || phase_ == Phase::kPlaying) &&
        elapsed >= std::chrono::seconds(10)) {
      const Kind kind = phase_ == Phase::kLoading ? Kind::kLoadingProgress : Kind::kPlayingProgress;
      const Observation result = Snapshot(kind, now, t);
      previous_ = now;
      last_report_ = t;
      return result;
    }
    return std::nullopt;
  }

 private:
  Observation Snapshot(Kind kind, const Counts& now, Time t) const {
    const auto elapsed = t >= started_
                             ? std::chrono::duration_cast<std::chrono::milliseconds>(t - started_).count()
                             : 0;
    return {kind, phase_, generation_, uint64_t(elapsed),
            Difference(now, phase_first_), Difference(now, previous_)};
  }
  void SwitchTo(Phase phase, const Counts& now, Time t) {
    phase_ = phase;
    phase_first_ = previous_ = now;
    started_ = last_report_ = t;
  }

  uint64_t generation_ = 0;
  Phase phase_ = Phase::kNone;
  Time started_{};
  Time last_report_{};
  Counts phase_first_{};
  Counts previous_{};
};

}
