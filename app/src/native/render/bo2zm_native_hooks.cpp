#include "bo2zm_native_hooks.h"

#include "bo2zm_native_shaders.h"
#if defined(__ANDROID__)
#include "shader_capture.h"
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include "bo2zm_deferred_report.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>
#include <array>
#include <string>

REXCVAR_DEFINE_BOOL(bo2zm_native_d3d_shadow, false, "BO2",
                    "Native renderer (Direct3D-level renderer): on 1 in 64 draws it snapshots the "
                    "device's register mirror and the ring compares it with what it reads from the packets ('D3D "
                    "shadow' line). Does not change what is drawn")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_d3d_marker_record, false, "BO2",
                    "Native renderer, Direct3D-level renderer: each DrawIndexedVertices writes its "
                    "record (VS, PS, arguments) into the command stream and the ring draws with it instead of its own "
                    "shader identity. Starts by checking against that identity and turns itself off at the first "
                    "disagreement. Off by default again: Origins binds pixel shaders whose program the "
                    "ring loads differently (the record's PS is not the IM_LOAD's: case 3, PC runs), and a variant "
                    "that first appears after the checks could draw a few frames with the record's shader; the "
                    "records measured no gain. false = the ring identifies every draw itself")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_d3d_game_vegetation, true, "BO2",
                    "Native renderer: the DrawVertices and DrawIndexedVertices the ring would drop "
                    "as shadow-map vegetation (no color, with an alpha test or discard) are skipped whole on the game "
                    "thread: no FlushState, DRAW_INDX or record. Starts by observing (the ring checks each draw's "
                    "verdict) and turns itself off at the first disagreement. false = as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DECLARE(bool, bo2zm_native_ps_alpha_only);
REXCVAR_DECLARE(int32_t, bo2zm_native_ps_alpha_only_alternate_s);
REXCVAR_DECLARE(bool, bo2zm_shadows_without_vegetation);

namespace bo2zm::native {
namespace {


constexpr uint32_t kDeviceVs = 13072;
constexpr uint32_t kDevicePs = 13068;


constexpr uint32_t kSizeQueue = uint32_t(1) << 16;

std::atomic<NativeShaders*> g_shaders{nullptr};

std::mutex g_mutex;
std::unordered_map<uint32_t, const ShaderInput*> g_objects;


std::unordered_map<uint32_t, uint32_t> g_object_numbers;
std::atomic<uint64_t> g_generation{0};
std::atomic<uint64_t> g_created_vs{0};
std::atomic<uint64_t> g_known_vs{0};
std::atomic<uint64_t> g_created_ps{0};
std::atomic<uint64_t> g_known_ps{0};
std::atomic<uint32_t> g_warnings{0};

DrawRecord g_queue[kSizeQueue];
std::atomic<uint32_t> g_write{0};
std::atomic<uint32_t> g_read{0};
std::atomic<uint64_t> g_lost{0};

uint32_t ReadBE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}


constexpr uint32_t kShotEvery = 64;
constexpr uint32_t kShots = 256;
constexpr uint32_t kDeviceFetch = 0x480;
constexpr uint32_t kDeviceConstantsVs = 0x780;
constexpr uint32_t kDeviceConstantsPs = 0x1780;

std::atomic<uint64_t> g_mask_group[kGroupsMirror];
std::atomic<uint32_t> g_offset_group[kGroupsMirror];

struct Shot {
  std::atomic<uint64_t> sequence{0};
  MirrorSnapshot data;
};
Shot g_shots[kShots];

std::atomic<uint64_t> g_next_shot{1};
uint32_t g_counter_shots = 0;


inline const uint8_t* Guest(const uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}


struct StreamConstants {
  uint32_t value[2048] = {};
  uint64_t known[32] = {};
  uint8_t from[2048] = {};
  void Set(uint32_t r, uint32_t v, uint8_t source) {
    value[r] = v;
    known[r >> 6] |= uint64_t(1) << (r & 63);
    from[r] = source;
  }
};
std::mutex g_stream_constants_mutex;
std::unordered_map<uint32_t, std::unique_ptr<StreamConstants>> g_stream_constants;
std::unordered_map<uint32_t, std::unique_ptr<StreamConstants>> g_buffer_constants;
std::unordered_map<uint32_t, uint32_t> g_buffer_of_device;
struct PendingConstants {
  uint32_t device, pointer, index, words;
};
thread_local std::vector<PendingConstants> t_pending_constants;

std::atomic<uint64_t> g_stream_begins{0}, g_stream_ends{0}, g_stream_ends_unknown{0}, g_stream_runs{0},
    g_stream_runs_unknown{0};


StreamConstants& StreamOf(std::unordered_map<uint32_t, std::unique_ptr<StreamConstants>>& map, uint32_t key) {
  std::unique_ptr<StreamConstants>& slot = map[key];
  if (!slot) {
    slot = std::make_unique<StreamConstants>();
  }
  return *slot;
}


void ApplyGameConstants(const uint8_t* base, uint32_t device, StreamConstants& slot) {
  std::vector<PendingConstants>& pending = t_pending_constants;
  size_t keep = 0;
  for (const PendingConstants& p : pending) {
    if (p.device != device) {
      pending[keep++] = p;
      continue;
    }
    const uint8_t* source = Guest(base, p.pointer);
    for (uint32_t i = 0; i < p.words; ++i) {
      slot.Set(p.index - 0x4000 + i, ReadBE(source + 4 * i), kFromGame);
    }
  }
  pending.resize(keep);
}


template <typename Use>
void ForEachLiteral(const uint8_t* base, uint32_t container, uint32_t physical, Use use) {
  if (!container || !physical) {
    return;
  }
  const uint32_t table = ReadBE(Guest(base, container + 20));
  if (!table) {
    return;
  }
  const uint32_t start = container + table + 20;
  const uint32_t bytes = ReadBE(Guest(base, container + table + 16));
  if (bytes > 4096) {
    return;
  }
  for (uint32_t p = start; p + 8 <= start + bytes; p += 8) {
    const uint8_t* e = Guest(base, p);
    const uint32_t reg = (uint32_t(e[0]) << 8) | e[1];
    const uint32_t count = (uint32_t(e[2]) << 8) | e[3];
    if (!count) {
      break;
    }
    use((4 * reg) >> 16, (4 * reg) & 0x7FF, Guest(base, physical + ReadBE(e + 4)), count);
  }
}


template <typename Use>
void ForEachLiteralOf(const uint8_t* base, uint32_t vs, uint32_t ps, Use use) {
  if (vs) {
    ForEachLiteral(base, vs + 0x368, ReadBE(Guest(base, vs + 0x20)), use);
  }
  if (ps) {
    ForEachLiteral(base, ps + 0x28, ReadBE(Guest(base, ps + 0x18)), use);
  }
}


uint64_t TakeShot(const uint8_t* base, uint32_t device, uint32_t vs = 0, uint32_t ps = 0, uint64_t sequence = 0) {
  if (!sequence) {
    sequence = g_next_shot.fetch_add(1, std::memory_order_relaxed);
  }
  Shot& shot = g_shots[sequence % kShots];
  shot.sequence.store(0, std::memory_order_release);
  MirrorSnapshot& d = shot.data;
  const uint8_t* disp = base + device;
  for (uint32_t g = 0; g < kGroupsMirror; ++g) {
    const uint32_t off = g_offset_group[g].load(std::memory_order_relaxed);
    d.mask[g] = off ? g_mask_group[g].load(std::memory_order_relaxed) : 0;
    d.base_register[g] = g < 8 ? 0x2000 + g * 0x80 : 0x4900;
    if (!off) {
      continue;
    }
    for (uint32_t i = 0; i < 64; ++i) {
      if ((d.mask[g] >> (63 - i)) & 1) {
        d.state[g][i] = ReadBE(disp + off + i * 4);
      }
    }
  }
  for (uint32_t i = 0; i < 192; ++i) {
    d.fetch[i] = ReadBE(disp + kDeviceFetch + i * 4);
  }
  for (uint32_t i = 0; i < 1024; ++i) {
    d.constants[i] = ReadBE(disp + kDeviceConstantsVs + i * 4);
    d.constants[1024 + i] = ReadBE(disp + kDeviceConstantsPs + i * 4);
  }
  std::memcpy(d.constants_mirror, d.constants, sizeof(d.constants));
  std::memset(d.constants_from, kFromUnknown, sizeof(d.constants_from));
  ForEachLiteralOf(base, vs, ps, [&](uint32_t type, uint32_t first, const uint8_t* source, uint32_t count) {
    for (uint32_t i = 0; type == 1 && i < count && first + i < 192; ++i) {
      d.fetch[first + i] = ReadBE(source + 4 * i);
    }
  });
  {
    std::lock_guard<std::mutex> lock(g_stream_constants_mutex);
    const auto it = g_stream_constants.find(device);
    if (it != g_stream_constants.end()) {
      const StreamConstants& c = *it->second;
      for (uint32_t i = 0; i < 2048; ++i) {
        if ((c.known[i >> 6] >> (i & 63)) & 1) {
          d.constants[i] = c.value[i];
          d.constants_from[i] = c.from[i];
        }
      }
    }
  }
  shot.sequence.store(sequence, std::memory_order_release);
  return sequence;
}

int GroupOf(uint32_t register_base) {
  if (register_base >= 0x2000 && register_base < 0x2400 && (register_base & 0x7F) == 0) {
    return int((register_base - 0x2000) >> 7);
  }
  return register_base == 0x4900 ? 8 : -1;
}


constexpr uint64_t kChecksDraw = 20000;
constexpr uint64_t kCheckDrawEvery = 1024;
DrawRecord g_draw_in_ongoing;
bool g_draw_pending = false;
uint64_t g_turn_draw = 0;
std::atomic<bool> g_draw_off{false};
std::atomic<bool> g_draw_applying{false};
std::atomic<uint64_t> g_draw_equal{0};
std::atomic<bool> g_draw_different{false};

uint32_t g_draw_case = 0;
DrawRecord g_draw_lookup;
DrawRecord g_draw_marker;
bool g_draw_has_lookup = false;
bool g_draw_has_marker = false;

uint64_t g_i_draws = 0;
uint64_t g_i_in_marker = 0;
uint64_t g_i_checking = 0;
uint64_t g_i_per_queue = 0;
uint64_t g_i_no_flushstate = 0;
int64_t g_i_next_ms = 0;

bool DrawInMarker() {
  static const bool active = REXCVAR_GET(bo2zm_d3d_marker_record);
  return active && !g_draw_off.load(std::memory_order_relaxed);
}


void EnqueueDraw(const DrawRecord& reg_index) {
  const uint32_t write = g_write.load(std::memory_order_relaxed);
  const uint32_t next = (write + 1) & (kSizeQueue - 1);
  if (next == g_read.load(std::memory_order_acquire)) {
    g_lost.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_queue[write] = reg_index;
  g_write.store(next, std::memory_order_release);
}

void TurnOffDraw() {

  if (g_draw_off.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  static constexpr const char* kCaseText[] = {
      "?", "the records give different shaders", "?",
      "the lookup finds no record and the ring's identity gives other shaders"};
  const DrawRecord& b = g_draw_lookup;
  const DrawRecord& m = g_draw_marker;
  REXLOG_ERROR("[d3d_marker] draw record: MISMATCH with the ring's lookup ({}): lookup {} (function {} type {} VS "
               "{:08X} PS {:08X}), marker {} (function {} type {} VS {:08X} PS {:08X}). Draw records OFF for the rest of "
               "the session: the records go back to the queue",
               kCaseText[g_draw_case < 4 ? g_draw_case : 0], g_draw_has_lookup ? "with a record" : "without a record",
               int(b.function), b.args[0], b.vs, b.ps, g_draw_has_marker ? "with a record" : "without a record",
               int(m.function), m.args[0], m.vs, m.ps);
}

void ReportDraw() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  if (now < g_i_next_ms) {
    return;
  }
  const bool first = g_i_next_ms == 0;
  g_i_next_ms = now + 10000;
  if (!first) {
    BO2ZM_REPORT_DEFERRED("[d3d_marker] draw records, last 10 s: {} in the marker ({} checking), {} through "
                "the queue, {} without FlushState | phase {} | checked on the ring {} of {}",
                g_i_in_marker, g_i_checking, g_i_per_queue, g_i_no_flushstate,
                g_draw_off.load(std::memory_order_relaxed)     ? "OFF"
                : g_draw_applying.load(std::memory_order_relaxed) ? "applying"
                                                                     : "observing",
                g_draw_equal.load(std::memory_order_relaxed), kChecksDraw);
  }
  g_i_in_marker = g_i_checking = g_i_per_queue = g_i_no_flushstate = 0;
}

}


namespace hooks_detail {

const ShaderInput* IdentifyCreation(const uint8_t* base, uint32_t address, bool vertices) {
  NativeShaders* shaders = g_shaders.load(std::memory_order_acquire);
  if (!shaders || !address || address > UINT32_MAX - 24) {
    return nullptr;
  }
  const uint8_t* p = base + address;
  if (ReadBE(p) != (vertices ? 0x102A0E01u : 0x102A0E00u)) {
    return nullptr;
  }
  const uint64_t total = uint64_t(ReadBE(p + 4)) + ReadBE(p + 8);
  if (total < 24 || total > 65536 || uint64_t(address) + total > (uint64_t(1) << 32)) {
    return nullptr;
  }
  return shaders->IdentifyContainer(std::span<const uint8_t>(p, size_t(total)));
}

void RememberCreation(uint32_t object, const ShaderInput* input, bool vertices) {
  if (!g_shaders.load(std::memory_order_acquire)) {
    return;
  }
  (vertices ? g_created_vs : g_created_ps).fetch_add(1, std::memory_order_relaxed);
  if (input) {
    (vertices ? g_known_vs : g_known_ps).fetch_add(1, std::memory_order_relaxed);
  } else if (g_warnings.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("[native] C5b: {} shader created at {:08X} that is not in the library",
                vertices ? "vertex" : "pixel", object);
  }
  if (!object) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);

  if (input) {
    g_objects.insert_or_assign(object, input);
  } else {
    g_objects.erase(object);
  }
  g_generation.fetch_add(1, std::memory_order_acq_rel);
}

}


const NativeShaders* LibraryActive() {
  return g_shaders.load(std::memory_order_acquire);
}

void ActivateHooks(NativeShaders* shaders) {
  g_shaders.store(shaders, std::memory_order_release);
  if (!shaders) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_objects.clear();
    g_object_numbers.clear();
    g_generation.fetch_add(1, std::memory_order_acq_rel);
  }
}

void RegisterShaderObject(const uint8_t* base, uint32_t object, uint32_t physical, bool vertices) {
  NativeShaders* shaders = g_shaders.load(std::memory_order_acquire);
  if (!shaders || !object || !physical) {
    return;
  }


  auto host = [base](uint32_t address) {
#if REX_PLATFORM_WIN32
    return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
    return base + address;
#endif
  };
  const uint8_t* c = host(object + (vertices ? 0x368u : 0x28u));
  const uint32_t flags = ReadBE(c);
  const uint32_t size_virtual = ReadBE(c + 4);
  const uint32_t size_physical = ReadBE(c + 8);
  int32_t number = -1;
  if ((flags & 0xFFFFFF00u) == 0x102A1100u && bool(flags & 1) == vertices && size_virtual >= 24 &&
      uint64_t(size_virtual) + size_physical <= 65536) {
    thread_local std::vector<uint8_t> blob;
    blob.assign(c, c + size_virtual);
    const uint8_t* p = host(physical);
    blob.insert(blob.end(), p, p + size_physical);
    number = shaders->NumberOfContainer(blob);
#if defined(__ANDROID__)


    bo2zm::android::capture::Record(blob, shaders->InLibrary(number));
#endif
  }
  (vertices ? g_created_vs : g_created_ps).fetch_add(1, std::memory_order_relaxed);
  if (number >= 0) {
    (vertices ? g_known_vs : g_known_ps).fetch_add(1, std::memory_order_relaxed);
  } else if (g_warnings.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("[native] C5b: {} shader registered at {:08X} (flags {:08X}) that is not in the library",
                vertices ? "vertex" : "pixel", object, flags);
  }
  std::lock_guard<std::mutex> lock(g_mutex);

  g_objects.erase(object);
  if (number >= 0) {
    g_object_numbers.insert_or_assign(object, uint32_t(number));
  } else {
    g_object_numbers.erase(object);
  }
  g_generation.fetch_add(1, std::memory_order_acq_rel);
}

void AnnotateDraw(DrawFunction function, const uint8_t* base, uint32_t vulkan_device, uint32_t r4,
                  uint32_t r5, uint32_t r6, uint32_t r7, uint16_t vegetation) {
  if (!vulkan_device || !g_shaders.load(std::memory_order_relaxed)) {
    return;
  }
  DrawRecord reg_index;
  reg_index.vs = ReadBE(base + vulkan_device + kDeviceVs);
  reg_index.ps = ReadBE(base + vulkan_device + kDevicePs);
  reg_index.args[0] = r4;
  reg_index.args[1] = r5;
  reg_index.args[2] = r6;
  reg_index.args[3] = r7;
  reg_index.function = function;
  reg_index.shadow = 0;
  reg_index.vegetation = vegetation;
  static const bool shadow = REXCVAR_GET(bo2zm_native_d3d_shadow);
  if (shadow && ++g_counter_shots % kShotEvery == 0) {
    reg_index.shadow = TakeShot(base, vulkan_device);
  }


  if (DrawInMarker()) {
    if (g_draw_pending) {
      EnqueueDraw(g_draw_in_ongoing);
      ++g_i_no_flushstate;
    }
    g_draw_in_ongoing = reg_index;
    g_draw_pending = true;
    if ((++g_i_draws & 4095) == 0) {
      ReportDraw();
    }
    return;
  }
  EnqueueDraw(reg_index);
}

bool TakeDrawInOngoing(DrawRecord& reg_index) {
  if (!g_draw_pending) {
    return false;
  }
  g_draw_pending = false;
  reg_index = g_draw_in_ongoing;
  return true;
}

uint32_t DecideModeDraw() {
  if (!DrawInMarker()) {
    return 0;
  }


  if (g_draw_different.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    TurnOffDraw();
    return 0;
  }
  if (!g_draw_applying.load(std::memory_order_relaxed)) {
    const uint64_t equal = g_draw_equal.load(std::memory_order_relaxed);
    if (equal < kChecksDraw) {
      return kDrawCheck;
    }
    g_draw_applying.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[d3d_marker] draw record: {} draws checked against the ring's lookup, 0 disagreements: the marker now "
                "carries the record and the ring does not look it up (1 in {} is still checked)",
                equal, kCheckDrawEvery);
  }
  return (++g_turn_draw & (kCheckDrawEvery - 1)) == 0 ? kDrawCheck : kDrawApply;
}

void DeliverDraw(const DrawRecord& reg_index, uint32_t mode, bool in_marker) {
  if (!in_marker) {
    ++g_i_per_queue;
    EnqueueDraw(reg_index);
    return;
  }
  ++g_i_in_marker;
  if (mode == kDrawCheck) {
    ++g_i_checking;
    EnqueueDraw(reg_index);
  }
}

void FinishDraw() {
  if (!g_draw_pending) {
    return;
  }
  g_draw_pending = false;
  ++g_i_no_flushstate;
  EnqueueDraw(g_draw_in_ongoing);
}

namespace {
std::atomic<uint64_t> g_stream_records{0};
std::atomic<uint64_t> g_stream_checking{0};
}

struct ShotReserved {
  uint64_t sequence;
  uint32_t device, vs, ps;
};
thread_local ShotReserved t_shot = {};

void AfterDraw(const uint8_t* base) {
  if (t_shot.sequence) {
    TakeShot(base, t_shot.device, t_shot.vs, t_shot.ps, t_shot.sequence);
    t_shot.sequence = 0;
  }
}

bool RecordForStream(DrawFunction function, const uint8_t* base, uint32_t device, uint32_t r4, uint32_t r5,
                     uint32_t r6, uint32_t r7, DrawRecord& record, uint32_t& mode) {
  if (!device || !g_shaders.load(std::memory_order_relaxed) || !DrawInMarker()) {
    return false;
  }


  if (g_draw_different.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    TurnOffDraw();
    return false;
  }
  thread_local uint64_t turn = 0;
  if (!g_draw_applying.load(std::memory_order_relaxed)) {
    const uint64_t equal = g_draw_equal.load(std::memory_order_relaxed);
    if (equal < kChecksDraw) {
      mode = kDrawCheck;
    } else {
      if (!g_draw_applying.exchange(true, std::memory_order_relaxed)) {
        REXLOG_INFO("[d3d_marker] draw record in the stream: {} draws checked against the ring's identity, 0 "
                    "disagreements: the ring now takes each draw's shaders from its record (1 in {} still checked)",
                    equal, kCheckDrawEvery);
      }
      mode = (++turn & (kCheckDrawEvery - 1)) == 0 ? kDrawCheck : kDrawApply;
    }
  } else {
    mode = (++turn & (kCheckDrawEvery - 1)) == 0 ? kDrawCheck : kDrawApply;
  }
  record = DrawRecord{};
  record.vs = ReadBE(base + device + kDeviceVs);
  record.ps = ReadBE(base + device + kDevicePs);
  record.args[0] = r4;
  record.args[1] = r5;
  record.args[2] = r6;
  record.args[3] = r7;
  record.function = function;


  static const bool shadow = REXCVAR_GET(bo2zm_native_d3d_shadow);
  thread_local uint32_t shots = 0;
  if (shadow) {
    if (++shots % kShotEvery == 0) {

      record.shadow = g_next_shot.fetch_add(1, std::memory_order_relaxed);
      t_shot = {record.shadow, device, record.vs, record.ps};
    }
  }
  g_stream_records.fetch_add(1, std::memory_order_relaxed);
  if (mode == kDrawCheck) {
    g_stream_checking.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

void NoteGameConstants(const uint8_t* base, uint32_t device, uint32_t pointer, uint32_t vectors) {
  static const bool shadow = REXCVAR_GET(bo2zm_native_d3d_shadow);
  if (!shadow || !device || pointer < 4 || vectors == 0 || vectors > 512) {
    return;
  }


  const uint32_t index = ReadBE(Guest(base, pointer - 4)) & 0xFFFF;
  const uint32_t words = 4 * vectors;
  if (index < 0x4000 || index + words > 0x4800) {
    return;
  }
  std::vector<PendingConstants>& pending = t_pending_constants;
  if (pending.size() >= 256) {
    pending.erase(pending.begin());
  }
  pending.push_back({device, pointer, index, words});
}

bool D3DShadowOn() {
  static const bool shadow = REXCVAR_GET(bo2zm_native_d3d_shadow);
  return shadow;
}

void BeforeFlush(const uint8_t* base, uint32_t device) {
  if (!D3DShadowOn() || !device) {
    return;
  }
  const uint8_t* d = base + device;
  std::lock_guard<std::mutex> lock(g_stream_constants_mutex);
  StreamConstants& c = StreamOf(g_stream_constants, device);


  ApplyGameConstants(base, device, c);
  const uint64_t dirty[2] = {(uint64_t(ReadBE(d)) << 32) | ReadBE(d + 4),
                             (uint64_t(ReadBE(d + 8)) << 32) | ReadBE(d + 12)};
  for (uint32_t kind = 0; kind < 2; ++kind) {
    const uint8_t* mirror = d + (kind ? kDeviceConstantsPs : kDeviceConstantsVs);
    for (uint32_t i = 0; dirty[kind] && i < 64; ++i) {
      if ((dirty[kind] >> (63 - i)) & 1) {
        for (uint32_t w = 16 * i; w < 16 * i + 16; ++w) {
          c.Set(kind * 1024 + w, ReadBE(mirror + 4 * w), kFromMirror);
        }
      }
    }
  }
  ForEachLiteralOf(base, ReadBE(d + kDeviceVs), ReadBE(d + kDevicePs),
                   [&](uint32_t type, uint32_t first, const uint8_t* source, uint32_t count) {
                     for (uint32_t i = 0; type == 0 && i < count && first + i < 2048; ++i) {
                       c.Set(first + i, ReadBE(source + 4 * i), kFromLiteral);
                     }
                   });
}

void BeginBuffer(uint32_t device, uint32_t buffer) {
  if (!D3DShadowOn() || !device) {
    return;
  }


  std::lock_guard<std::mutex> lock(g_stream_constants_mutex);
  g_buffer_of_device[device] = buffer;
  StreamOf(g_stream_constants, device) = StreamConstants{};
  g_stream_begins.fetch_add(1, std::memory_order_relaxed);
}

void EndBuffer(uint32_t device) {
  if (!D3DShadowOn() || !device) {
    return;
  }


  std::lock_guard<std::mutex> lock(g_stream_constants_mutex);
  const auto it = g_buffer_of_device.find(device);
  if (it == g_buffer_of_device.end()) {
    g_stream_ends_unknown.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const StreamConstants& now = StreamOf(g_stream_constants, device);
  StreamOf(g_buffer_constants, it->second) = now;
  g_stream_ends.fetch_add(1, std::memory_order_relaxed);
}

void AfterRun(uint32_t device, uint32_t buffer) {
  if (!D3DShadowOn() || !device) {
    return;
  }


  std::lock_guard<std::mutex> lock(g_stream_constants_mutex);
  const auto it = g_buffer_constants.find(buffer);
  if (it == g_buffer_constants.end()) {
    g_stream_runs_unknown.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_stream_runs.fetch_add(1, std::memory_order_relaxed);
  const StreamConstants& from = *it->second;
  StreamConstants& c = StreamOf(g_stream_constants, device);
  for (uint32_t i = 0; i < 2048; ++i) {
    if ((from.known[i >> 6] >> (i & 63)) & 1) {
      c.Set(i, from.value[i], kFromBuffer);
    }
  }
}

std::string StreamConstantsReport() {
  return fmt::format("command buffers: {} begun, {} ended ({} on a device never begun), {} run ({} never ended)",
                     g_stream_begins.exchange(0, std::memory_order_relaxed),
                     g_stream_ends.exchange(0, std::memory_order_relaxed),
                     g_stream_ends_unknown.exchange(0, std::memory_order_relaxed),
                     g_stream_runs.exchange(0, std::memory_order_relaxed),
                     g_stream_runs_unknown.exchange(0, std::memory_order_relaxed));
}

void StreamRecordCounts(uint64_t& records, uint64_t& checking) {
  records = g_stream_records.load(std::memory_order_relaxed);
  checking = g_stream_checking.load(std::memory_order_relaxed);
}

void AnnotateCheckDraw(bool equal, uint32_t kind, const DrawRecord* lookup,
                              const DrawRecord* marker) {
  if (equal) {
    g_draw_equal.store(g_draw_equal.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return;
  }
  if (g_draw_different.load(std::memory_order_relaxed)) {
    return;
  }
  g_draw_case = kind;
  g_draw_has_lookup = lookup != nullptr;
  if (lookup) {
    g_draw_lookup = *lookup;
  }
  g_draw_has_marker = marker != nullptr;
  if (marker) {
    g_draw_marker = *marker;
  }
  g_draw_different.store(true, std::memory_order_release);
}

void LearnGroupMirror(uint32_t register_base, uint64_t mask, uint32_t displacement) {

  static const bool shadow = REXCVAR_GET(bo2zm_native_d3d_shadow);
  if (!shadow) {
    return;
  }
  const int g = GroupOf(register_base);
  if (g < 0 || !displacement) {
    return;
  }


  uint32_t expected = 0;
  g_offset_group[g].compare_exchange_strong(expected, displacement, std::memory_order_relaxed);
  const uint64_t before = g_mask_group[g].load(std::memory_order_relaxed);
  if ((before | mask) != before) {
    g_mask_group[g].store(before | mask, std::memory_order_relaxed);
  }
}

bool ReadSnapshot(uint64_t sequence, MirrorSnapshot& output) {
  const Shot& shot = g_shots[sequence % kShots];
  if (shot.sequence.load(std::memory_order_acquire) != sequence) {
    return false;
  }
  output = shot.data;
  std::atomic_thread_fence(std::memory_order_acquire);
  return shot.sequence.load(std::memory_order_relaxed) == sequence;
}

bool ExtractDraw(DrawRecord& reg_index) {
  const uint32_t read = g_read.load(std::memory_order_relaxed);
  if (read == g_write.load(std::memory_order_acquire)) {
    return false;
  }
  reg_index = g_queue[read];
  g_read.store((read + 1) & (kSizeQueue - 1), std::memory_order_release);
  return true;
}

const ShaderInput* ObjectShader(uint32_t object) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_objects.find(object);
  if (it != g_objects.end()) {
    return it->second;
  }

  const auto n = g_object_numbers.find(object);
  NativeShaders* shaders = g_shaders.load(std::memory_order_acquire);
  if (n == g_object_numbers.end() || !shaders) {
    return nullptr;
  }
  const ShaderInput* input = shaders->PerNumber(n->second);
  if (input) {
    g_objects.emplace(object, input);
  }
  return input;
}

uint64_t GenerationObjects() {
  return g_generation.load(std::memory_order_acquire);
}


namespace {
namespace vegetation {

constexpr uint64_t kChecks = 200000;
constexpr uint64_t kSampleEvery = 4096;

constexpr uint32_t kModeEdram = 0x2D94;
constexpr uint32_t kMaskColor = 0x2D1C;
constexpr uint32_t kControlColor = 0x2D7C;
constexpr uint32_t kIndexBase = 0x2D14;


constexpr uint32_t kDirty20 = 0x20;
constexpr uint32_t kDirty28 = 0x28;
constexpr uint32_t kDirty30 = 0x30;
constexpr uint32_t kBlocks = 0x28C0;
constexpr uint8_t kBlocksMask = 0x3F;
constexpr uint32_t kTypeOcclusion = 9;
constexpr uint32_t kMaxQueries = 16;

enum DisagreementKind : uint32_t { kDisagreeNone, kDisagreeGameYes, kDisagreeRingYes, kDisagreeOcclusion, kDisagreeNoIdentity, kDisagreeModel, kDisagreeKinds };


int8_t g_active = -1;
bool g_applying = false;
bool g_off = false;
bool g_settings = false;
uint64_t g_turn = 0;
uint64_t g_observed_total = 0;
struct Memo {
  uint32_t object = 0;
  const ShaderInput* input = nullptr;
};
std::array<Memo, 32> g_memo{};
uint64_t g_memo_generation = UINT64_MAX;

int64_t g_i_next_ms = 0;
uint64_t g_i_observed = 0;
uint64_t g_i_yes = 0;
uint64_t g_i_skipped = 0;
uint64_t g_i_samples = 0;
uint64_t g_i_block = 0;
uint64_t g_i_targets = 0;
uint64_t g_i_occlusion = 0;
uint64_t g_i_settings = 0;
uint64_t g_i_count = 0;
std::array<uint32_t, 64> g_i_blocks{};


std::mutex g_queries_lock;
std::array<uint32_t, kMaxQueries> g_queries{};
uint32_t g_n_queries = 0;
std::atomic<uint32_t> g_queries_open{0};


enum Counter : uint32_t {
  kYesYes,
  kNoNo,
  kYesNo,
  kNoYes,
  kNoYesBlock,
  kNoYesTargets,
  kNoYesOcclusion,
  kNoYesSettings,
  kNoYesCount,
  kSettingsDifferent,
  kNoCompare,
  kAgreements,
  kSamplesOk,
  kCounters
};


alignas(64) std::atomic<uint64_t> g_counters[kCounters];
alignas(64) std::array<uint64_t, kCounters> g_previous{};


alignas(64) std::atomic<bool> g_different{false};
uint32_t g_disagree_kind = kDisagreeNone;
uint16_t g_flags = 0;
VegetationDetail g_detail;

uint64_t ReadBE64(const uint8_t* p) {
  return (uint64_t(ReadBE(p)) << 32) | ReadBE(p + 4);
}

void WriteBE(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

const char* YesNo(bool value) {
  return value ? "si" : "no";
}

inline void Count(Counter c) {
  g_counters[c].store(g_counters[c].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}


void Disagreement(DisagreementKind kind, uint16_t flags, const VegetationDetail& ring) {
  if (g_different.load(std::memory_order_relaxed)) {
    return;
  }
  g_disagree_kind = kind;
  g_flags = flags;
  g_detail = ring;
  g_different.store(true, std::memory_order_release);
}


const ShaderInput* Input(uint32_t object) {
  const uint64_t generation = g_generation.load(std::memory_order_relaxed);
  if (generation != g_memo_generation) {
    g_memo.fill(Memo{});
    g_memo_generation = generation;
  }
  Memo& memo = g_memo[size_t((object * UINT32_C(2654435761)) >> 27)];
  if (memo.object != object) {
    memo.object = object;
    memo.input = ObjectShader(object);
  }
  return memo.input;
}


bool Struct(const uint8_t* d, uint32_t& reason) {
  const uint32_t object_ps = ReadBE(d + kDevicePs);
  const uint32_t object_vs = ReadBE(d + kDeviceVs);
  if (!object_ps || !object_vs) {
    reason = kVegReasonNoShaders;
    return false;
  }


  uint32_t mode = ReadBE(d + kModeEdram) & 0x7;
  if (mode == 5 && (ReadBE64(d + kDirty30) & 0x100)) {
    mode = 4;
  }
  if (mode != 4) {
    reason = kVegReasonMode;
    return false;
  }
  const ShaderInput* ps = Input(object_ps);
  const ShaderInput* vs = Input(object_vs);
  if (!ps || !vs) {
    reason = kVegReasonUnknown;
    return false;
  }
  const uint32_t mask = ReadBE(d + kMaskColor);
  for (uint32_t i = 0; i < 4; ++i) {
    if (((mask >> (i * 4)) & 0xF) && ((ps->outputs >> i) & 0x1)) {
      reason = kVegReasonColor;
      return false;
    }
  }
  const uint32_t control = ReadBE(d + kControlColor);
  const bool test_alpha = ((control >> 3) & 0x1) && (control & 0x7) != 7;
  if (!test_alpha && !ps->discards && !(ps->outputs & 0x10)) {
    reason = kVegReasonNoDiscard;
    return false;
  }
  reason = 0;
  return true;
}

void RefreshSettingsVegetation() {
  g_settings = REXCVAR_GET(bo2zm_native_ps_alpha_only) && REXCVAR_GET(bo2zm_native_ps_alpha_only_alternate_s) <= 0 &&
              REXCVAR_GET(bo2zm_shadows_without_vegetation);
}


void TurnOffVegetation() {
  if (g_off) {
    return;
  }
  g_off = true;
  static constexpr const char* kDisagreeText[kDisagreeKinds] = {
      "?",
      "the game sees vegetation and the ring does not",
      "the ring sees vegetation and the game does not",
      "occlusion query open on the ring and not in the game",
      "the game sees vegetation and the ring does not draw with the shaders of its record",
      "the ring's verdict is not Draw's early discard"};
  const VegetationDetail& a = g_detail;
  const uint16_t b = g_flags;
  REXLOG_ERROR("[vegetation] MISMATCH with the ring ({}): game {} (reason {}, settings {}, occlusion {}, block {}, "
               "targets {}, would skip {}, sample {}; phase {}); ring: structural {}, settings {}, occlusion {}, early "
               "discard in Draw {}, RB_MODECONTROL {:08X}, RB_COLOR_MASK {:08X}, RB_COLORCONTROL {:08X}, PS n{} "
               "(outputs {:X}, discards {}), VS n{}. Vegetation filter in the game OFF for the rest of the session: "
               "every Draw* goes back to the ring",
               kDisagreeText[g_disagree_kind < kDisagreeKinds ? g_disagree_kind : 0], YesNo((b & kVegYes) != 0), (b >> kVegReason) & 0xF,
               YesNo((b & kVegSettings) != 0), YesNo((b & kVegOcclusion) != 0), YesNo((b & kVegBlock) != 0),
               YesNo((b & kVegTargets) != 0), YesNo((b & kVegWouldSkip) != 0), YesNo((b & kVegSample) != 0),
               g_applying ? "applying" : "observing", YesNo(a.structural), YesNo(a.settings), YesNo(a.occlusion),
               YesNo(a.early), a.mode, a.mask, a.control, a.ps, a.outputs, YesNo(a.discards), a.vs);
}


void ReportVegetation() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  if (now < g_i_next_ms) {
    return;
  }
  const bool first = g_i_next_ms == 0;
  g_i_next_ms = now + 10000;
  std::array<uint64_t, kCounters> ring{};
  for (uint32_t i = 0; i < kCounters; ++i) {
    ring[i] = g_counters[i].load(std::memory_order_relaxed);
  }
  if (first) {
    REXLOG_INFO("[vegetation] shadow-map vegetation filtered in the game: starts by observing; it skips "
                "once the ring has checked {} draws it would skip without any disagreement",
                kChecks);
  } else {
    std::array<uint64_t, kCounters> d{};
    for (uint32_t i = 0; i < kCounters; ++i) {
      d[i] = ring[i] - g_previous[i];
    }
    std::string blocks;
    for (uint32_t v = 1; v < 64; ++v) {
      if (g_i_blocks[v]) {
        blocks += fmt::format(" {:02X}:{}", v, g_i_blocks[v]);
      }
    }
    if (!blocks.empty()) {
      blocks = " (values" + blocks + ")";
    }
    const bool lost = g_queries_open.load(std::memory_order_relaxed) == UINT32_MAX;
    BO2ZM_REPORT_DEFERRED(
        "[vegetation] Draw*, last 10 s: {} looked at, {} vegetation for the game: {} skipped and {} sent "
        "for checking; not skipped {} in D3D blocks{}, {} with targets to dump, {} with an occlusion query open, {} "
        "with the settings off or alternating, {} because of the count | ring: yes and yes {}, no and no {}, game yes "
        "and ring no {}, game no and ring yes {} ({} because of block, {} because of targets, {} because of occlusion "
        "only in the game, {} because of settings, {} because of the count), different settings {}, not compared {}, "
        "good samples {} | phase {} | checked {} of {}{}",
        g_i_observed, g_i_yes, g_i_skipped, g_i_samples, g_i_block, blocks, g_i_targets, g_i_occlusion, g_i_settings,
        g_i_count, d[kYesYes], d[kNoNo], d[kYesNo], d[kNoYes], d[kNoYesBlock], d[kNoYesTargets], d[kNoYesOcclusion],
        d[kNoYesSettings], d[kNoYesCount], d[kSettingsDifferent], d[kNoCompare], d[kSamplesOk],
        g_applying ? "applying" : "observing", ring[kAgreements], kChecks,
        lost ? " | occlusion queries: THE COUNT WAS LOST, nothing is skipped" : "");
  }
  g_previous = ring;
  g_i_observed = g_i_yes = g_i_skipped = g_i_samples = g_i_block = g_i_targets = g_i_occlusion = g_i_settings =
      g_i_count = 0;
  g_i_blocks.fill(0);
}

}
}

uint16_t DecideVegetation(DrawFunction function, uint8_t* base, uint32_t vulkan_device, uint32_t r5, uint32_t r6,
                           uint32_t r7, bool& skip) {
  using namespace vegetation;
  skip = false;
  if (g_active < 0) {
    g_active = REXCVAR_GET(bo2zm_d3d_game_vegetation) ? 1 : 0;
  }
  if (!g_active || g_off || !vulkan_device || !g_shaders.load(std::memory_order_relaxed) ||
      (function != DrawFunction::kVertices && function != DrawFunction::kIndexed)) {
    return 0;
  }


  if (g_different.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    TurnOffVegetation();
    return 0;
  }
  if ((g_observed_total++ & 1023) == 0) {
    RefreshSettingsVegetation();
    ReportVegetation();
  }
  ++g_i_observed;
  const uint8_t* d = base + vulkan_device;
  uint32_t reason = 0;
  const bool yes = Struct(d, reason);
  const uint32_t open = g_queries_open.load(std::memory_order_relaxed);
  const uint8_t blocks = uint8_t(d[kBlocks] & kBlocksMask);
  const bool targets = (uint32_t(ReadBE64(d + kDirty20)) & 0x3FFFC000u) != 0;
  const uint32_t count = function == DrawFunction::kVertices ? r6 : r7;
  uint16_t flags = uint16_t(kVegHas | (reason << kVegReason));
  if (yes) {
    flags = uint16_t(flags | kVegYes);
  }
  if (open) {
    flags = uint16_t(flags | kVegOcclusion);
  }
  if (g_settings) {
    flags = uint16_t(flags | kVegSettings);
  }
  if (blocks) {
    flags = uint16_t(flags | kVegBlock);
  }
  if (targets) {
    flags = uint16_t(flags | kVegTargets);
  }
  if (!yes) {
    return flags;
  }
  ++g_i_yes;

  if (blocks) {
    ++g_i_block;
    ++g_i_blocks[blocks];
    return flags;
  }
  if (targets) {
    ++g_i_targets;
    return flags;
  }
  if (open) {
    ++g_i_occlusion;
    return flags;
  }
  if (!g_settings) {
    ++g_i_settings;
    return flags;
  }
  if (count == 0 || count > 0xFFFF) {
    ++g_i_count;
    return flags;
  }
  flags = uint16_t(flags | kVegWouldSkip);
  if (!g_applying) {
    const uint64_t checked = g_counters[kAgreements].load(std::memory_order_relaxed);
    if (checked < kChecks) {
      return flags;
    }
    g_applying = true;
    REXLOG_INFO("[vegetation] {} vegetation draws the game would skip checked on the ring, 0 disagreements: they are "
                "skipped in the D3D's Draw* now (1 in {} is still sent to check it)",
                checked, kSampleEvery);
  }
  if ((++g_turn & (kSampleEvery - 1)) == 0) {
    ++g_i_samples;
    return uint16_t(flags | kVegSample);
  }


  uint8_t* mirror = base + vulkan_device;
  const uint32_t index = function == DrawFunction::kVertices ? r5 : 0u;
  if (ReadBE(mirror + kIndexBase) != index) {
    WriteBE(mirror + kIndexBase, index);
    const uint64_t dirty = ReadBE64(mirror + kDirty28) | (uint64_t(1) << 61);
    WriteBE(mirror + kDirty28, uint32_t(dirty >> 32));
    WriteBE(mirror + kDirty28 + 4, uint32_t(dirty));
  }
  ++g_i_skipped;
  skip = true;
  return flags;
}

void AnnotateQueryD3D(const uint8_t* base, uint32_t query, uint32_t flags) {
  using namespace vegetation;
  if (!query || ReadBE(base + query + 4) != kTypeOcclusion) {
    return;
  }
  const bool begins = (flags & 2) != 0;
  if (!begins && !(flags & 1)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_queries_lock);
  uint32_t i = 0;
  while (i < g_n_queries && g_queries[i] != query) {
    ++i;
  }
  if (begins) {
    if (i == g_n_queries) {
      if (g_n_queries == kMaxQueries) {
        g_queries_open.store(UINT32_MAX, std::memory_order_relaxed);
        return;
      }
      g_queries[g_n_queries++] = query;
    }
  } else if (i < g_n_queries) {
    g_queries[i] = g_queries[--g_n_queries];
  }
  if (g_queries_open.load(std::memory_order_relaxed) != UINT32_MAX) {
    g_queries_open.store(g_n_queries, std::memory_order_relaxed);
  }
}

uint16_t IdentityForVegetation(uint16_t flags, bool with_shaders) {
  using namespace vegetation;
  if (!(flags & kVegHas)) {
    return 0;
  }
  if (with_shaders) {
    return flags;
  }
  Count(kNoCompare);
  if (flags & kVegYes) {
    Disagreement(kDisagreeNoIdentity, flags, VegetationDetail{});
  }
  return 0;
}

void AnnotateVegetationRing(uint16_t flags, const VegetationDetail& ring) {
  using namespace vegetation;
  const bool yes = (flags & kVegYes) != 0;
  const bool would_skip = (flags & kVegWouldSkip) != 0;
  const bool full = ring.structural && ring.settings && !ring.occlusion;
  Count(would_skip ? (full ? kYesYes : kYesNo) : (full ? kNoYes : kNoNo));
  if (yes != ring.structural) {
    Disagreement(yes ? kDisagreeGameYes : kDisagreeRingYes, flags, ring);
    return;
  }
  if (would_skip) {
    if (!ring.settings) {
      Count(kSettingsDifferent);
    } else if (ring.occlusion) {
      Disagreement(kDisagreeOcclusion, flags, ring);
    } else {
      Count(kAgreements);
      if (flags & kVegSample) {
        Count(kSamplesOk);
      }
    }
    return;
  }
  if (full) {
    Count((flags & kVegBlock)       ? kNoYesBlock
           : (flags & kVegTargets)   ? kNoYesTargets
           : (flags & kVegOcclusion)   ? kNoYesOcclusion
           : !(flags & kVegSettings)   ? kNoYesSettings
                                         : kNoYesCount);
  }
}

void AnnotateVegetationModel(uint16_t flags, const VegetationDetail& ring) {
  vegetation::Disagreement(vegetation::kDisagreeModel, flags, ring);
}

HookStats HooksStats() {
  HookStats e;
  e.created_vs = g_created_vs.load(std::memory_order_relaxed);
  e.known_vs = g_known_vs.load(std::memory_order_relaxed);
  e.created_ps = g_created_ps.load(std::memory_order_relaxed);
  e.known_ps = g_known_ps.load(std::memory_order_relaxed);
  e.lost = g_lost.load(std::memory_order_relaxed);
  return e;
}

}


#include "bo2zm_microcode_versions.h"


#if defined(BO2ZM_REFERENCE_GAME_HOOKS) && !defined(BO2ZM_NATIVE_SHADER_LIBRARY)


REX_EXTERN(__imp__sub_8259BC90);
REX_HOOK_RAW(sub_8259BC90) {
  const auto* input =
      bo2zm::native::hooks_detail::IdentifyCreation(base, ctx.r3.u32, false);
  __imp__sub_8259BC90(ctx, base);
  bo2zm::native::hooks_detail::RememberCreation(ctx.r3.u32, input, false);
  bo2zm::native::microcode::WarnCreation(base, ctx.r3.u32, false);
}

REX_EXTERN(__imp__sub_8259C038);
REX_HOOK_RAW(sub_8259C038) {
  const auto* input = bo2zm::native::hooks_detail::IdentifyCreation(base, ctx.r3.u32, true);
  __imp__sub_8259C038(ctx, base);
  bo2zm::native::hooks_detail::RememberCreation(ctx.r3.u32, input, true);
  bo2zm::native::microcode::WarnCreation(base, ctx.r3.u32, true);
}
#endif


#if defined(BO2ZM_REFERENCE_GAME_HOOKS)
REX_EXTERN(__imp__sub_825A2FB8);
REX_HOOK_RAW(sub_825A2FB8) {
  namespace mc = bo2zm::native::microcode;
  const uint32_t target = ctx.r5.u32;
  const uint32_t vs = ctx.r4.u32;
  const uint32_t return_address = uint32_t(ctx.lr);
  const bool in_room = vs != 0 && mc::ReadGuest32(base, vs + 40) == target;
  if (!in_room && return_address == 0x825A38D0u) {
    mc::Count(mc::g_patches_in_copy);
    __imp__sub_825A2FB8(ctx, base);
    return;
  }
  const uint32_t slot = mc::SlotOf(mc::Physical(target) & ~uint32_t(3));
  mc::BeginWrite(slot);
  __imp__sub_825A2FB8(ctx, base);


  if (in_room) {
    mc::Count(mc::g_patches_in_room);
  } else {
    mc::g_other_return.store(return_address, std::memory_order_relaxed);
    mc::g_other_target.store(target, std::memory_order_relaxed);
    mc::Count(mc::g_patches_others);
  }
  mc::FinishWrite(slot);
}
#endif
