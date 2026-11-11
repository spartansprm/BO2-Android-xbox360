// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <locale>
#include <mutex>
#include <sstream>
#include <time.h>

namespace bo2zm::native::pixel_probe {
inline constexpr uint64_t kSecond = 1000000000;
inline constexpr uint64_t kWarmup = 10 * kSecond;
inline constexpr uint64_t kBlock = 30 * kSecond;
inline constexpr uint64_t kSettle = 5 * kSecond;
inline constexpr uint64_t kRestore = 3 * kSecond;
inline constexpr uint64_t kDuration = kWarmup + 4 * kBlock + kRestore;


template <typename Rect>
inline bool Clip(Rect& rect, bool enabled) {
  if (!enabled || !rect.extent.width || !rect.extent.height) return false;
  const bool changed = rect.extent.width > 1 || rect.extent.height > 1;
  rect.extent.width = 1;
  rect.extent.height = 1;
  return changed;
}

enum class Kind { kScissor, kShader };
inline const char* KindName(Kind kind) { return kind==Kind::kShader ? "target_shader" : "scissor"; }

struct Tag {
  Kind kind = Kind::kScissor;
  uint64_t trial = 0, started_ns = 0;
  uint32_t phase = 0;
  bool clip = false, keep = false;
};
struct Counts {
  uint64_t draws = 0, scene_draws = 0, triangles = 0, scene_triangles = 0, clipped_draws = 0;
  uint64_t target_passes = 0, target_draws = 0, target_elements = 0, eligible_draws = 0,
      replaced_draws = 0, replaced_elements = 0, flat_fallbacks = 0, normal_pipeline_misses = 0,
      flat_compile_ns = 0;
  std::array<uint64_t,8> shader_reasons{};
};
struct Block {
  uint64_t frames = 0, wall_ns = 0, process_cpu_ns = 0, ring_cpu_ns = 0;
  uint64_t gpu_jobs = 0, gpu_ns = 0, gpu_pending = 0, gpu_failed = 0;
  uint64_t gpu_jobs_excluded = 0, failed_presents = 0;
  uint64_t target_gpu_jobs = 0, target_gpu_ns = 0, target_gpu_failed = 0, jobs_without_target = 0;
  Counts counts;
};
enum class State { kIdle, kRequested, kWarmup, kRunning, kRestoring, kComplete, kCancelled };
inline const char* Name(State state) {
  switch (state) {
    case State::kIdle: return "idle";
    case State::kRequested: return "requested";
    case State::kWarmup: return "warmup";
    case State::kRunning: return "running";
    case State::kRestoring: return "restoring";
    case State::kComplete: return "complete";
    case State::kCancelled: return "cancelled";
  }
  return "unavailable";
}
inline bool Busy(State state) {
  return state == State::kRequested || state == State::kWarmup ||
         state == State::kRunning || state == State::kRestoring;
}
struct Snapshot {
  Kind kind = Kind::kScissor;
  State state = State::kIdle;
  uint64_t trial = 0, start_ns = 0, updated_ns = 0, phase_remaining_ns = 0;
  uint32_t phase = 0;
  bool process_cpu_available = false, ring_cpu_available = false;
  const char* reason = "";
  std::array<Block, 4> blocks{};
};


class Controller {
 public:
  bool Start(bool client_in_game, bool map_notified, uint64_t native_swaps, Kind kind = Kind::kScissor) {


    if (native_swaps < 2 || (!client_in_game && !map_notified)) return false;
    if (Busy(data_.state)) return false;
    const uint64_t next = data_.trial + 1;
    data_ = {};
    data_.kind = kind;
    data_.trial = next;
    data_.state = State::kRequested;
    previous_ = {};
    return true;
  }
  void Cancel(const char* reason = "user") {
    if (Busy(data_.state)) {
      data_.state = State::kCancelled;
      data_.reason = reason;
      data_.phase_remaining_ns = 0;
    }
  }
  const Snapshot& Read() const { return data_; }

  Tag Boundary(uint64_t now, uint64_t process_cpu, uint64_t ring_cpu,
               const Tag& frame, const Counts& counts, bool painted) {
    if (data_.state == State::kRequested) {
      data_.start_ns = now;
      data_.state = State::kWarmup;
    }
    if (!Busy(data_.state)) return {};
    const Tag next = At(now);
    if (frame.trial == data_.trial && frame.keep && next.phase == frame.phase &&
        frame.phase >= 1 && frame.phase <= 4 && previous_.trial == frame.trial &&
        now >= frame.started_ns) {
      auto& b = data_.blocks[frame.phase - 1];
      if (painted) {
        ++b.frames;
        b.wall_ns += now - frame.started_ns;
        if (process_cpu && previous_process_cpu_ && process_cpu >= previous_process_cpu_) {
          b.process_cpu_ns += process_cpu - previous_process_cpu_;
          data_.process_cpu_available = true;
        }
        if (ring_cpu && previous_ring_cpu_ && ring_cpu >= previous_ring_cpu_) {
          b.ring_cpu_ns += ring_cpu - previous_ring_cpu_;
          data_.ring_cpu_available = true;
        }
        b.counts.draws += counts.draws;
        b.counts.scene_draws += counts.scene_draws;
        b.counts.triangles += counts.triangles;
        b.counts.scene_triangles += counts.scene_triangles;
        b.counts.clipped_draws += counts.clipped_draws;
        b.counts.target_passes += counts.target_passes;
        b.counts.target_draws += counts.target_draws;
        b.counts.target_elements += counts.target_elements;
        b.counts.eligible_draws += counts.eligible_draws;
        b.counts.replaced_draws += counts.replaced_draws;
        b.counts.replaced_elements += counts.replaced_elements;
        b.counts.flat_fallbacks += counts.flat_fallbacks;
        b.counts.normal_pipeline_misses += counts.normal_pipeline_misses;
        b.counts.flat_compile_ns += counts.flat_compile_ns;
        for(size_t i=0;i<b.counts.shader_reasons.size();++i)b.counts.shader_reasons[i]+=counts.shader_reasons[i];
      } else {
        ++b.failed_presents;
      }
    }
    previous_process_cpu_ = process_cpu;
    previous_ring_cpu_ = ring_cpu;
    previous_ = next;
    data_.updated_ns = now;
    data_.phase = next.phase;
    if (next.phase == 0) {
      data_.state = State::kWarmup;
      data_.phase_remaining_ns = kWarmup - (now - data_.start_ns);
    } else if (next.phase <= 4) {
      data_.state = State::kRunning;
      data_.phase_remaining_ns = kBlock - ((now - data_.start_ns - kWarmup) % kBlock);
    } else {
      data_.state = State::kRestoring;
      const uint64_t elapsed = now - data_.start_ns;
      data_.phase_remaining_ns = elapsed < kDuration ? kDuration - elapsed : 0;
      bool pending = false;
      for (const auto& b : data_.blocks) pending |= b.gpu_pending != 0;
      if (elapsed >= kDuration && !pending) {
        data_.state = State::kComplete;
        return {};
      }
    }
    return next;
  }


  bool Submitted(Tag& tag, uint64_t now, bool mixed) {
    if (tag.trial != data_.trial || !tag.keep || tag.phase < 1 || tag.phase > 4) {
      tag.keep = false;
      return false;
    }
    auto& b = data_.blocks[tag.phase - 1];
    if (mixed || !Busy(data_.state) || At(now).phase != tag.phase) {
      ++b.gpu_jobs_excluded;
      tag.keep = false;
      return false;
    }
    ++b.gpu_pending;
    return true;
  }
  void Complete(const Tag& tag, bool valid, uint64_t gpu_ns, bool target_expected=false, bool target_valid=false, uint64_t target_ns=0) {
    if (!tag.keep || tag.trial != data_.trial || tag.phase < 1 || tag.phase > 4) return;
    auto& b = data_.blocks[tag.phase - 1];
    if (!b.gpu_pending) return;
    --b.gpu_pending;
    if (valid) { ++b.gpu_jobs; b.gpu_ns += gpu_ns; }
    else ++b.gpu_failed;
    if(tag.kind==Kind::kShader) {
      if(!target_expected)++b.jobs_without_target;
      else if(valid && target_valid){++b.target_gpu_jobs;b.target_gpu_ns+=target_ns;}
      else ++b.target_gpu_failed;
    }
  }

 private:
  Tag At(uint64_t now) const {
    Tag tag;
    tag.kind = data_.kind;
    tag.trial = data_.trial;
    tag.started_ns = now;
    const uint64_t elapsed = now >= data_.start_ns ? now - data_.start_ns : 0;
    if (elapsed < kWarmup) return tag;
    if (elapsed >= kWarmup + 4 * kBlock) { tag.phase = 5; return tag; }
    tag.phase = uint32_t((elapsed - kWarmup) / kBlock) + 1;
    tag.clip = tag.phase == 2 || tag.phase == 3;
    tag.keep = ((elapsed - kWarmup) % kBlock) >= kSettle;
    return tag;
  }
  Snapshot data_;
  Tag previous_;
  uint64_t previous_process_cpu_ = 0, previous_ring_cpu_ = 0;
};

inline std::mutex mutex;
inline Controller controller;
inline bool Start(bool client_in_game, bool map_notified, uint64_t native_swaps, Kind kind = Kind::kScissor) {
  std::lock_guard lock(mutex);
  return controller.Start(client_in_game, map_notified, native_swaps,kind);
}
inline void Cancel(const char* reason = "user") { std::lock_guard lock(mutex); controller.Cancel(reason); }
inline Snapshot Read() { std::lock_guard lock(mutex); return controller.Read(); }
inline Tag Boundary(uint64_t now, uint64_t process, uint64_t ring, const Tag& frame,
                    const Counts& counts, bool painted) {
  std::lock_guard lock(mutex);
  return controller.Boundary(now, process, ring, frame, counts, painted);
}
inline void Submitted(Tag& tag, uint64_t now, bool mixed) {
  if (!tag.trial) return;
  std::lock_guard lock(mutex);
  controller.Submitted(tag, now, mixed);
}
inline void Complete(const Tag& tag, bool valid, uint64_t ns, bool target_expected=false, bool target_valid=false, uint64_t target_ns=0) {
  if (!tag.trial) return;
  std::lock_guard lock(mutex);
  controller.Complete(tag, valid, ns,target_expected,target_valid,target_ns);
}
inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline std::array<uint64_t, 2> CpuClocks() {
#if defined(__ANDROID__) || defined(__linux__)
  auto read = [](clockid_t clock) -> uint64_t {
  timespec value{};
  if (clock_gettime(clock, &value) != 0) return 0;
  return uint64_t(value.tv_sec) * kSecond + uint64_t(value.tv_nsec);
  };
  return {read(CLOCK_PROCESS_CPUTIME_ID), read(CLOCK_THREAD_CPUTIME_ID)};
#else
  return {};
#endif
}

inline std::string Json(const Snapshot& s) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "{\"kind\":\"" << KindName(s.kind) << "\",\"schema\":1,\"state\":\"" << Name(s.state) << "\",\"trial\":" << s.trial
      << ",\"sequence\":\"ABBA\",\"phase\":" << s.phase
      << ",\"start_monotonic_ns\":\"" << s.start_ns << "\",\"updated_monotonic_ns\":\"" << s.updated_ns
      << "\",\"phase_remaining_ms\":" << s.phase_remaining_ns / 1000000
      << ",\"warmup_seconds\":10,\"block_seconds\":30,\"settle_seconds\":5,\"restore_seconds\":3"
      << ",\"target_pitch_min\":640,\"target_pitch_max\":1279"
      << ",\"target_shader_pass\":{\"hash\":\"7e08c83e9feb2809\",\"ordinal\":3,\"width\":880,\"height\":768}"
      << ",\"shader_probe_scope\":\"eligible opaque fragment replacement; VS varying/output work and pipeline switching may also change; not separate texture/arithmetic timing\""
      << ",\"shader_reason_names\":[\"eligible\",\"no_color_shader\",\"shader_depth\",\"discard\",\"alpha_test\",\"blend\",\"query\",\"unsafe_shader\"]"
      << ",\"process_cpu_clock_available\":" << (s.process_cpu_available ? "true" : "false")
      << ",\"ring_cpu_clock_available\":" << (s.ring_cpu_available ? "true" : "false")
      << ",\"cancel_reason\":\"" << s.reason << "\",\"blocks\":[";
  for (size_t i = 0; i < s.blocks.size(); ++i) {
    const auto& b = s.blocks[i];
    if (i) out << ',';
    out << "{\"phase\":" << i + 1 << ",\"mode\":\"" << ((i == 1 || i == 2) ? (s.kind==Kind::kShader ? "flat_fragment" : "scissor_1x1") : "normal")
        << "\",\"frames\":" << b.frames << ",\"frame_wall_ns\":" << b.wall_ns
        << ",\"process_cpu_ns\":" << b.process_cpu_ns << ",\"ring_cpu_ns\":" << b.ring_cpu_ns
        << ",\"draws\":" << b.counts.draws << ",\"scene_draws\":" << b.counts.scene_draws
        << ",\"triangles_approx\":" << b.counts.triangles << ",\"scene_triangles_approx\":" << b.counts.scene_triangles
        << ",\"clipped_draws\":" << b.counts.clipped_draws
        << ",\"target_passes\":" << b.counts.target_passes << ",\"target_draws\":" << b.counts.target_draws
        << ",\"target_elements\":" << b.counts.target_elements << ",\"eligible_draws\":" << b.counts.eligible_draws
        << ",\"replaced_draws\":" << b.counts.replaced_draws << ",\"replaced_elements\":" << b.counts.replaced_elements
        << ",\"flat_fallbacks\":" << b.counts.flat_fallbacks << ",\"normal_pipeline_misses\":" << b.counts.normal_pipeline_misses
        << ",\"flat_compile_ns\":" << b.counts.flat_compile_ns
        << ",\"target_gpu_jobs\":" << b.target_gpu_jobs << ",\"target_gpu_ns\":" << b.target_gpu_ns
        << ",\"target_gpu_failed\":" << b.target_gpu_failed << ",\"jobs_without_target\":" << b.jobs_without_target
        << ",\"shader_reasons\":[";
    for(size_t r=0;r<b.counts.shader_reasons.size();++r){if(r)out<<',';out<<b.counts.shader_reasons[r];}
    out << ']'
        << ",\"gpu_jobs\":" << b.gpu_jobs << ",\"gpu_ns\":" << b.gpu_ns
        << ",\"gpu_pending\":" << b.gpu_pending << ",\"gpu_failed\":" << b.gpu_failed
        << ",\"gpu_jobs_excluded\":" << b.gpu_jobs_excluded << ",\"failed_presents\":" << b.failed_presents << '}';
  }
  out << "],\"scope\":\"frame cadence is not display FPS; GPU intervals are not utilization; geometry counters flag workload changes and occlusion feedback\"}";
  return out.str();
}
}
