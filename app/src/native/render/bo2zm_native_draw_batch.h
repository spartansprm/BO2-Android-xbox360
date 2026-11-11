// SPDX-License-Identifier: GPL-3.0-only


#pragma once

#include <atomic>
#include <cstring>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>

namespace bo2zm::native::draw_batch {


inline bool SameConstants(bool full_layout, uint64_t epoch, uint64_t previous_epoch,
                          uint64_t generation, uint64_t previous_generation,
                          std::span<const uint8_t> previous, std::span<const uint8_t> current) {
  return full_layout && epoch == previous_epoch && generation != previous_generation &&
         current.size() <= previous.size() &&
         (current.empty() || std::memcmp(previous.data(), current.data(), current.size()) == 0);
}

struct Draw {
  bool indexed = false;
  uint32_t count = 0, instances = 1, first = 0;
  int32_t vertex_offset = 0;
  uint32_t first_instance = 0;
};

struct Counters {
  uint64_t requested = 0, emitted = 0, merged = 0, eligible = 0;
  uint64_t state_boundaries = 0, range_breaks = 0;
};


class Coalescer {
 public:
  template <typename Emit>
  void Push(const Draw& draw, bool safe, Emit&& emit) {
    ++counters.requested;
    const bool eligible = safe && draw.instances == 1 && draw.count && draw.count % 3 == 0;
    counters.eligible += eligible;
    if (eligible && pending_ && Compatible(draw)) {
      pending_draw_.count += draw.count;
      ++counters.merged;
      return;
    }
    if (pending_) ++counters.range_breaks;
    Flush(emit);
    if (eligible) {
      pending_draw_ = draw;
      pending_ = true;
    } else {
      emit(draw);
      ++counters.emitted;
    }
  }

  template <typename Emit>
  void Flush(Emit&& emit) {
    if (!pending_) return;
    emit(pending_draw_);
    pending_ = false;
    ++counters.emitted;
  }

  template <typename Emit>
  void Boundary(Emit&& emit) {
    counters.state_boundaries += pending_;
    Flush(emit);
  }

  Counters counters;

 private:
  bool Compatible(const Draw& d) const {
    const auto& p = pending_draw_;
    constexpr auto max = std::numeric_limits<uint32_t>::max();
    return p.indexed == d.indexed && p.first_instance == d.first_instance &&
           p.vertex_offset == d.vertex_offset && p.count <= max - p.first &&
           p.first + p.count == d.first && d.count <= max - p.count &&
           d.count <= max - d.first;
  }
  Draw pending_draw_{};
  bool pending_ = false;
};


inline bool ShaderSafe(std::span<const uint32_t> w) {
  if (w.size() < 5 || w[0] != 0x07230203 || !w[3]) return false;
  std::unordered_map<uint32_t, uint32_t> pointer_types, pointers;
  bool stage = false;
  for (size_t i = 5; i < w.size();) {
    const uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
    if (!n || n > w.size() - i) return false;
    if (op == 17) {
      if (n != 2 || (w[i+1] != 1 && w[i+1] != 50 && w[i+1] != 5302)) return false;
    } else if (op == 15) {
      if (n < 4 || (w[i+1] != 0 && w[i+1] != 4)) return false;
      stage = true;
    } else if (op == 11) {
      if (n < 3) return false;
      std::string name;
      bool terminated = false;
      for (size_t j = i+2; j < i+n && !terminated; ++j) {
        for (uint32_t b = 0; b < 4; ++b) {
          const char c = char(w[j] >> (b*8));
          if (!c) { terminated = true; break; }
          name += c;
        }
      }
      if (!terminated || name != "GLSL.std.450") return false;
    } else if (op == 32) {
      if (n != 4) return false;
      pointer_types[w[i+1]] = w[i+2];
    } else if (op == 71 || op == 72) {
      const size_t decoration = i + (op == 71 ? 2 : 3);
      if (decoration >= i+n) return false;
      if (w[decoration] == 11) {
        if (decoration+1 >= i+n) return false;
        const uint32_t builtin = w[decoration+1];
        if (builtin == 7 || builtin == 4426 || (builtin >= 36 && builtin <= 41)) return false;
      }
    }


    if (op == 63 || op == 64 || op == 99 || op == 224 || op == 225 ||
        (op >= 227 && op <= 242) || op == 318 || op == 319) return false;
    i += n;
  }
  if (!stage) return false;
  for (size_t i = 5; i < w.size(); i += w[i] >> 16) {
    const uint32_t n = w[i] >> 16, op = w[i] & 0xffff;


    if (n >= 3 && (op == 55 || op == 57 || op == 59 || op == 65 || op == 66 ||
                   op == 67 || op == 70 || op == 83 || op == 169 || op == 245)) {
      if (const auto t = pointer_types.find(w[i+1]); t != pointer_types.end())
        pointers[w[i+2]] = t->second;
    }
  }
  for (size_t i = 5; i < w.size(); i += w[i] >> 16) {
    if ((w[i] & 0xffff) != 62) continue;
    if ((w[i] >> 16) < 3) return false;
    const auto p = pointers.find(w[i+1]);

    if (p == pointers.end() || (p->second != 3 && p->second != 6 && p->second != 7)) return false;
  }
  return true;
}


inline std::atomic<uint64_t> sequence{0}, requested{0}, emitted{0}, merged{0}, eligible{0},
    state_boundaries{0}, range_breaks{0}, constants_reused{0};
inline std::atomic<bool> enabled{false}, active{false};
inline void Publish(const Counters& c) {
  sequence.fetch_add(1, std::memory_order_acq_rel);
  requested.store(c.requested, std::memory_order_relaxed);
  emitted.store(c.emitted, std::memory_order_relaxed);
  merged.store(c.merged, std::memory_order_relaxed);
  eligible.store(c.eligible, std::memory_order_relaxed);
  state_boundaries.store(c.state_boundaries, std::memory_order_relaxed);
  range_breaks.store(c.range_breaks, std::memory_order_relaxed);
  sequence.fetch_add(1, std::memory_order_release);
}
inline std::string Json() {
  for (int retry = 0; retry < 4; ++retry) {
    const auto before = sequence.load(std::memory_order_acquire);
    if (before & 1) continue;
    const Counters c{requested.load(std::memory_order_relaxed), emitted.load(std::memory_order_relaxed),
        merged.load(std::memory_order_relaxed), eligible.load(std::memory_order_relaxed),
        state_boundaries.load(std::memory_order_relaxed), range_breaks.load(std::memory_order_relaxed)};
    std::atomic_thread_fence(std::memory_order_acquire);
    if (before != sequence.load(std::memory_order_acquire)) continue;
    return "{\"available\":true,\"enabled\":" + std::string(enabled.load()?"true":"false") +
        ",\"recorder_active\":" + (active.load()?"true":"false") +
        ",\"scope\":\"cumulative deferred job-buffer draws; not game objects or displayed FPS\"" +
        ",\"requested_total\":" + std::to_string(c.requested) +
        ",\"emitted_total\":" + std::to_string(c.emitted) +
        ",\"merged_total\":" + std::to_string(c.merged) +
        ",\"eligible_total\":" + std::to_string(c.eligible) +
        ",\"state_boundaries_total\":" + std::to_string(c.state_boundaries) +
        ",\"range_breaks_total\":" + std::to_string(c.range_breaks) +
        ",\"constant_uploads_reused_total\":" + std::to_string(constants_reused.load()) + '}';
  }
  return "{\"available\":false}";
}
}
