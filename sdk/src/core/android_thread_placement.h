// SPDX-License-Identifier: GPL-3.0-only
//

// tested on the host: tools/test_android_v044.cpp). threading_posix.cpp reads sysfs and calls sched_setaffinity.
//

// four big cores and the little ones almost idle, with the game's threads waiting 15-40 % of the time for a core
// (runqueue) and the native renderer's recorder 20-34 %, while the ring thread waited for the recorder. Light threads
// that do not set the frame time (audio, reports, in-match pipeline compiles, texture hashes) are moved to the little
// cluster, so the big cores are left to the ring, the recorder and the game's main, server, backend and job threads.
// Nothing is pinned to the big cores: the scheduler still decides for everything not on the list.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rex::thread::placement {

// Linux keeps 16 bytes for a thread name, terminator included; pthread_setname_np refuses a longer one with ERANGE

// "GPU native recorder" and the other renderer helpers all appeared as "GPU native ring".
inline constexpr size_t kMaxThreadName = 15;

// The name cut to 15 bytes, never in the middle of a UTF-8 sequence.
inline std::string TruncateThreadName(std::string_view name) {
  if (name.size() <= kMaxThreadName) {
    return std::string(name);
  }
  size_t n = kMaxThreadName;
  while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) {
    --n;  // name[n] is a continuation byte: the sequence it belongs to starts before the cut
  }
  return std::string(name.substr(0, n));
}

// The threads sent to the little cores, by prefix of the name they are given (the full one, before truncation; guest
// threads carry their handle after it: "Sound Mix (F8000034)"). Deliberately left out:
//   - Stream: the fastfile stream decoder; on the little cores map loading would take longer.
//   - BO2 vertex copies / BO2 texture binds: the ring waits for them every frame.
//   - BO2 pipeline precompile / prewarm: they run behind the loading screen, where speed is the point.
//   - Presenter Paint, GPU native recorder, the guest Main / Server / Backend / JQ Worker threads, and the driver's.
inline constexpr std::string_view kLittleCoreThreads =
    "Sound Mix,XMA Decoder,Audio Worker,BO2 reports,BO2 pipeline compiles,BO2 pipeline cache,BO2 texture hashes,"
    "WebM Encode";

// True if `name` starts with one of the comma-separated prefixes in `list` (surrounding spaces ignored; empty items
// never match).
inline bool MatchesList(std::string_view name, std::string_view list) {
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string_view::npos) {
      end = list.size();
    }
    std::string_view item = list.substr(start, end - start);
    while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
    while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
    if (!item.empty() && name.substr(0, item.size()) == item) {
      return true;
    }
    if (end == list.size()) {
      break;
    }
    start = end + 1;
  }
  return false;
}

struct Cpu {
  uint32_t index = 0;
  uint64_t capacity = 0;  // sysfs cpu_capacity, or cpuinfo_max_freq when that is missing; 0 = unknown
};

// The CPUs of the slowest cluster, as a mask over CPUs 0-63, among those allowed to the process. 0 (do nothing) when
// the CPUs are all alike, any capacity is unknown, or the little cluster would hold the whole allowed set.
inline uint64_t LittleMask(const std::vector<Cpu>& cpus, uint64_t allowed) {
  uint64_t lowest = UINT64_MAX, highest = 0;
  size_t known = 0;
  for (const Cpu& c : cpus) {
    if (c.index >= 64 || !((allowed >> c.index) & 1)) {
      continue;
    }
    if (!c.capacity) {
      return 0;
    }
    lowest = c.capacity < lowest ? c.capacity : lowest;
    highest = c.capacity > highest ? c.capacity : highest;
    ++known;
  }
  if (known < 2 || lowest == highest) {
    return 0;
  }
  uint64_t mask = 0;
  for (const Cpu& c : cpus) {
    if (c.index < 64 && ((allowed >> c.index) & 1) && c.capacity == lowest) {
      mask |= uint64_t(1) << c.index;
    }
  }
  return mask == allowed ? 0 : mask;
}

// What to do with a thread when it is named: send it to the little cores, give it back the whole process set (it
// inherited the little-core restriction from the thread that created it but is not on the list), or nothing.
enum class Action { kNone, kLittle, kRestore };
inline Action Decide(std::string_view name, std::string_view list, uint64_t little, uint64_t current) {
  if (!little) {
    return Action::kNone;
  }
  if (MatchesList(name, list)) {
    return current == little ? Action::kNone : Action::kLittle;
  }
  return current == little ? Action::kRestore : Action::kNone;
}

}  // namespace rex::thread::placement
