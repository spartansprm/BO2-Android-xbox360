#include "bo2zm_native_shaders.h"
#include "../bo2zm_native_system.h"

#include "bo2zm_native_cache_paths.h"
#include "bo2zm_native_shader_profile.h"
#include "bo2zm_shader_index.h"
#include "bo2zm_shader_library.h"
#if BO2ZM_RUNTIME_SHADERS
#include "bo2zm_shader_translator.h"
#include "bo2zm_shader_translator_hlsl.h"
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>


#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

#if defined(__ANDROID__) || defined(__linux__)
#include <sys/resource.h>
#endif

#if defined(__SWITCH__)
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>
#endif

REXCVAR_DEFINE_BOOL(bo2zm_native_shader_index_check, false, "BO2",
                    "Test: at start the whole shader pack is also read as before its index (bo2zm_shaders.spvidx) "
                    "and every shader made from the index is compared with it (log: \"index check\"). Slow and "
                    "uses much memory: for the PC")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_INT32(bo2zm_native_shader_translate, 1, "BO2",
                     "Shaders the SPIR-V library does not have: 0 = not drawn (as up to 0.48); 1 = translated on the "
                     "device (XenosRecomp + glslang) the first time and kept in cache/runtime-shaders; 2 = test: "
                     "EVERY shader translated on the device and the library ignored (compare the image with mode 1)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_shader_translate_threads, -1, "BO2",
                     "Background threads that translate, while a map loads, the shaders the game registers and the "
                     "cache does not have yet (so fewer are translated when first drawn). -1 = automatic (3 without "
                     "a library, 2 with one), 0 = only when drawn")
    .range(-1, 6)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_shader_library, true, "BO2",
                    "Use the SPIR-V shader library (bo2zm_shaders.spvpack) if there is one. false = ignore it: every "
                    "shader is translated on the device (bo2zm_native_shader_translate)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {


void AnalyzeConstants(bo2zm::native::ShaderInput& e) {
  e.constants_runs.clear();
  e.constants_runs_valid = false;
  e.constants_runs_bytes = 0;
  if (e.microcode.empty()) {
    return;
  }
#if defined(__SWITCH__)
  try {
    rex::graphics::Shader analysis(
        e.vertices ? rex::graphics::xenos::ShaderType::kVertex : rex::graphics::xenos::ShaderType::kPixel, 0,
        e.microcode.data(), e.microcode.size(), std::endian::native);
    rex::string::StringBuffer disassembly;
    analysis.AnalyzeUcode(disassembly);
    const auto& map = analysis.constant_register_map();
    if (map.float_dynamic_addressing) {
      return;
    }
    for (uint32_t reg = 0; reg < 256;) {
      if (!((map.float_bitmap[reg / 64] >> (reg % 64)) & 1)) {
        ++reg;
        continue;
      }
      const uint32_t first = reg;
      while (reg < 256 && ((map.float_bitmap[reg / 64] >> (reg % 64)) & 1)) {
        ++reg;
      }
      e.constants_runs.emplace_back(uint16_t(first), uint16_t(reg - first));
      e.constants_runs_bytes += (reg - first) * 16;
    }

    e.constants_runs_valid = e.constants_runs.size() <= 24;
  } catch (const std::exception&) {
    e.constants_runs.clear();
  }
#endif
}

}

namespace bo2zm::native {
namespace {

constexpr uint32_t kMaxWarnings = 48;

constexpr const char* kUses[] = {"position", "weight",   "indices",    "normal",
                                 "point_size", "texcoord", "tangent",    "binormal",
                                 "tessfactor","position_t", "color",    "fog",
                                 "depth", "sample", "usage14",     "usage15"};


const char* Read(const bo2zm::shaderlib::Shader& shader, ShaderInput& e) {
  return ReadContainer(shader, e, AnalyzeConstants);
}

struct RawKey {
  uint64_t hash;
  uint32_t words;
  bool vertices;
  bool operator==(const RawKey&) const = default;
};

struct RawKeyHash {
  size_t operator()(const RawKey& c) const {
    return size_t(c.hash ^ (uint64_t(c.words) << 1) ^ uint64_t(c.vertices));
  }
};

}

struct NativeShaders::Data {
  bo2zm::shaderlib::ShaderLibrary library;
  std::vector<IndexEntry> index;
  uint64_t identity = 0;
  std::vector<uint16_t> fetches;
  IndexStats index_stats;

  std::map<std::pair<bool, uint32_t>, std::vector<uint32_t>> candidates;

  std::unique_ptr<std::atomic<const ShaderInput*>[]> made;
  std::mutex making;
  std::deque<bo2zm::shaderlib::Shader> made_shaders;
  std::deque<ShaderInput> made_inputs;
  std::vector<uint8_t> unmakeable;
  uint32_t make_warnings = 0;
  std::unordered_map<RawKey, const ShaderInput*, RawKeyHash> cache;
  std::vector<uint32_t> temporal;
  NativeShaderStats stats;
  uint32_t warnings = 0;
  bool loaded = false;


  struct RuntimeSlot {
    std::vector<uint8_t> container;
    uint64_t low = 0, high = 0;
    uint32_t size = 0;
    int state = 0;
    bool translating = false;
    const ShaderInput* input = nullptr;
  };
  int translate_mode = 0;
  uint64_t translator_identity = 0;
  uint64_t catalog_id = 0;
  std::filesystem::path runtime_folder;
  std::mutex runtime_mutex;
  std::condition_variable runtime_changed;
  std::deque<RuntimeSlot> runtime;
  std::unordered_map<uint64_t, std::vector<uint32_t>> runtime_by_hash;
  std::ofstream catalog;


  struct RuntimeMatch {
    uint32_t position = 0;
    uint64_t hash = 0;
    std::vector<uint32_t> normalized;
    std::vector<uint16_t> fetches;
  };
  std::map<std::pair<bool, uint32_t>, std::vector<RuntimeMatch>> runtime_candidates;
  std::atomic<uint64_t> runtime_match_generation{0};
  uint64_t observed_runtime_match_generation = 0;
  uint32_t runtime_index_warnings = 0;
  void IndexRuntime(uint32_t position);
  uint32_t runtime_translated = 0, runtime_cached = 0, runtime_failed = 0;
  double runtime_ms = 0;
  bool no_library = false;


  std::deque<uint32_t> pending;
  std::condition_variable pending_changed;
  std::vector<std::thread> workers;
  bool stopping = false;
  uint32_t background_translated = 0;
  double background_ms = 0;

  ~Data();
  void Work();


  void WriteRuntimeFile(const std::filesystem::path& file, uint64_t low, uint64_t high,
                        const std::vector<uint8_t>& container, const std::vector<uint32_t>& spirv) const;


  void OpenRuntime();


  int32_t RuntimeNumber(std::span<const uint8_t> container);


  const ShaderInput* MakeRuntime(uint32_t position);
  std::filesystem::path RuntimeFile(uint64_t low, uint64_t high, uint32_t size) const;


  const ShaderInput* Make(uint32_t number, std::span<const uint8_t> given = {});
  bool ReadIndex(const std::filesystem::path& file, const bo2zm::shaderlib::PackInfo& pack);

  void MakeIndex(const std::filesystem::path& pack_file, const bo2zm::shaderlib::PackInfo& pack,
                 const std::filesystem::path& file, const IndexProgress& progress);
  void Take(ShaderIndex&& from);
};

NativeShaders::NativeShaders() : data_(std::make_unique<Data>()) {}

NativeShaders::Data::~Data() {
  {
    std::lock_guard<std::mutex> lock(runtime_mutex);
    stopping = true;
  }
  pending_changed.notify_all();
  for (std::thread& worker : workers) {
    worker.join();
  }
}
NativeShaders::~NativeShaders() = default;

bool NativeShaders::loaded() const {
  return data_->loaded;
}

uint64_t NativeShaders::Identity() const {
  const Data& d = *data_;
  if (!d.loaded) {
    return 0;
  }
  if (!d.translate_mode) {
    return d.identity;
  }


  const uint64_t parts[4] = {d.identity, d.catalog_id, d.translator_identity, uint64_t(d.translate_mode)};
  return XXH3_64bits(parts, sizeof(parts));
}


namespace {
constexpr uint32_t kCatalogMagic = 0x4352'5A42;
constexpr uint32_t kRuntimeMagic = 0x5352'5A42;
constexpr uint32_t kRuntimeVersion = 1;
constexpr size_t kRuntimeHeader = 48;
constexpr uint32_t kRuntimeLogs = 24;


struct RuntimeCounters {
  std::atomic<int> mode{-1};
  std::atomic<bool> no_library{false};
  std::atomic<uint32_t> registered{0}, translated_when_drawn{0}, translated_background{0}, from_cache{0}, failed{0},
      waiting{0}, draw_waits{0};


  std::atomic<uint32_t> library_used{0}, library_size{0};


  std::atomic<uint32_t> runtime_used{0};
  std::atomic<uint64_t> us_when_drawn{0}, us_background{0}, us_draw_waits{0}, us_longest_wait{0};
} g_counters;
}

std::string RuntimeShadersJson() {
  const auto ms = [](const std::atomic<uint64_t>& us) { return double(us.load(std::memory_order_relaxed)) / 1000.0; };
  return fmt::format(
      "{{\"mode\":{},\"no_library\":{},\"registered\":{},\"translated_when_drawn\":{},"
      "\"translated_background\":{},\"from_cache\":{},\"failed\":{},\"waiting_background\":{},"
      "\"ms_translating_when_drawn\":{:.1f},\"ms_translating_background\":{:.1f},\"first_uses_waited\":{},"
      "\"ms_first_uses_waited\":{:.1f},\"ms_longest_wait\":{:.1f},\"library_size\":{},\"library_used\":{},"
      "\"runtime_used\":{},\"library_hit_percent\":{:.1f}}}",
      g_counters.mode.load(), g_counters.no_library.load() ? "true" : "false", g_counters.registered.load(),
      g_counters.translated_when_drawn.load(), g_counters.translated_background.load(), g_counters.from_cache.load(),
      g_counters.failed.load(), g_counters.waiting.load(), ms(g_counters.us_when_drawn), ms(g_counters.us_background),
      g_counters.draw_waits.load(), ms(g_counters.us_draw_waits), ms(g_counters.us_longest_wait),
      g_counters.library_size.load(), g_counters.library_used.load(), g_counters.runtime_used.load(),
      (g_counters.library_used.load() + g_counters.runtime_used.load())
          ? 100.0 * g_counters.library_used.load() / (g_counters.library_used.load() + g_counters.runtime_used.load())
          : 0.0);
}

std::filesystem::path NativeShaders::Data::RuntimeFile(uint64_t low, uint64_t high, uint32_t size) const {
  return runtime_folder / fmt::format("{:016X}{:016X}-{}-{:016X}.bzrs", high, low, size, translator_identity);
}

void NativeShaders::Data::OpenRuntime() {
  translate_mode = REXCVAR_GET(bo2zm_native_shader_translate);
#if !BO2ZM_RUNTIME_SHADERS
  if (translate_mode) {
    REXLOG_INFO("[native] runtime shaders: not in this build (BO2ZM_RUNTIME_SHADERS): missing shaders are not drawn");
  }
  translate_mode = 0;
  return;
#else
  if (!translate_mode) {
    REXLOG_INFO("[native] runtime shaders: off (bo2zm_native_shader_translate=0)");
    return;
  }
  translator_identity = translate::Identity();
  runtime_folder = cache::Folder() / "runtime-shaders";
  std::error_code ec;
  std::filesystem::create_directories(runtime_folder, ec);
  const std::filesystem::path file = runtime_folder / "catalog.bin";
  std::lock_guard<std::mutex> lock(runtime_mutex);
  runtime.clear();
  runtime_by_hash.clear();
  runtime_candidates.clear();
  runtime_match_generation.store(0, std::memory_order_release);
  observed_runtime_match_generation = 0;
  catalog_id = 0;
  {
    std::ifstream in(file, std::ios::binary);
    uint32_t head[2] = {};
    uint64_t id = 0;
    if (in.read(reinterpret_cast<char*>(head), 8) && in.read(reinterpret_cast<char*>(&id), 8) &&
        head[0] == kCatalogMagic && head[1] == 1 && id) {
      catalog_id = id;
      struct Record {
        uint64_t low, high;
        uint32_t size, reserved;
      } r;
      while (in.read(reinterpret_cast<char*>(&r), sizeof(r))) {
        RuntimeSlot slot;
        slot.low = r.low;
        slot.high = r.high;
        slot.size = r.size;
        runtime_by_hash[r.low].push_back(uint32_t(runtime.size()));
        runtime.push_back(std::move(slot));
      }
    }
  }
  if (!catalog_id) {

    std::random_device random;
    catalog_id = (uint64_t(random()) << 32 | random()) ^
                 uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    catalog_id |= 1;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    const uint32_t head[2] = {kCatalogMagic, 1};
    out.write(reinterpret_cast<const char*>(head), 8);
    out.write(reinterpret_cast<const char*>(&catalog_id), 8);
  }
  catalog.open(file, std::ios::binary | std::ios::app);
  g_counters.mode.store(translate_mode);
  g_counters.no_library.store(no_library);
  int threads = REXCVAR_GET(bo2zm_native_shader_translate_threads);
  if (threads < 0) {
    threads = no_library ? 3 : 2;
  }
  for (int k = 0; k < threads && workers.size() < size_t(threads); ++k) {
    workers.emplace_back([this] { Work(); });
  }
  REXLOG_INFO("[native] runtime shaders: mode {} ({}), translator {:016X}, catalog {:016X} with {} shaders, {} "
              "background threads, in {}",
              translate_mode,
              translate_mode == 2 ? "every shader translated on the device: test"
              : no_library        ? "NO LIBRARY: every shader translated on the device"
                                  : "missing shaders",
              translator_identity, catalog_id, runtime.size(), threads, runtime_folder.string());
#endif
}

void NativeShaders::Data::WriteRuntimeFile(const std::filesystem::path& file, uint64_t low, uint64_t high,
                                           const std::vector<uint8_t>& container,
                                           const std::vector<uint32_t>& spirv) const {

  std::filesystem::path temporary = file;
  temporary += fmt::format(".{:x}.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
  std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
  const uint32_t head32[4] = {kRuntimeMagic, kRuntimeVersion, uint32_t(container.size()), uint32_t(spirv.size())};
  const uint64_t head64[4] = {translator_identity, low, high, XXH3_64bits(spirv.data(), spirv.size() * 4)};
  out.write(reinterpret_cast<const char*>(head32), 16);
  out.write(reinterpret_cast<const char*>(head64), 32);
  out.write(reinterpret_cast<const char*>(container.data()), std::streamsize(container.size()));
  out.write(reinterpret_cast<const char*>(spirv.data()), std::streamsize(spirv.size() * 4));
  out.close();
  std::error_code ec;
  if (out) {
    std::filesystem::rename(temporary, file, ec);
  } else {
    std::filesystem::remove(temporary, ec);
  }
}

void NativeShaders::Data::Work() {
#if defined(__ANDROID__) || defined(__linux__)
  setpriority(PRIO_PROCESS, 0, 10);
#endif
#if BO2ZM_RUNTIME_SHADERS
  while (true) {
    uint32_t position = 0;
    std::vector<uint8_t> container;
    uint64_t low = 0, high = 0;
    {
      std::unique_lock<std::mutex> lock(runtime_mutex);
      pending_changed.wait(lock, [this] { return stopping || !pending.empty(); });
      if (stopping) {
        return;
      }
      position = pending.front();
      pending.pop_front();
      RuntimeSlot& slot = runtime[position];
      if (slot.state != 0 || slot.translating || slot.container.empty()) {
        continue;
      }
      slot.translating = true;
      container = slot.container;
      low = slot.low;
      high = slot.high;
    }
    const std::filesystem::path file = RuntimeFile(low, high, uint32_t(container.size()));
    std::error_code ec;
    bool failed = false;
    std::string error;
    double ms = 0;
    if (!std::filesystem::exists(file, ec)) {
      translate::Result r = translate::Translate(container);
      ms = r.ms_hlsl + r.ms_compile;
      if (r.spirv.empty()) {
        failed = true;
        error = r.error;
      } else {
        WriteRuntimeFile(file, low, high, container, r.spirv);
      }
    }
    std::lock_guard<std::mutex> lock(runtime_mutex);
    RuntimeSlot& slot = runtime[position];
    slot.translating = false;
    g_counters.waiting.store(uint32_t(pending.size()), std::memory_order_relaxed);
    if (failed) {
      slot.state = 3;
      g_counters.failed.fetch_add(1, std::memory_order_relaxed);
      if (runtime_failed++ < kRuntimeLogs) {
        REXLOG_WARN("[native] runtime shaders: n{} could not be translated (background): {}", index.size() + position,
                    error);
      }
    } else if (ms > 0) {
      background_ms += ms;
      g_counters.translated_background.fetch_add(1, std::memory_order_relaxed);
      g_counters.us_background.fetch_add(uint64_t(ms * 1000), std::memory_order_relaxed);
      if (++background_translated % 250 == 0 || background_translated <= 4) {
        REXLOG_INFO("[native] runtime shaders: {} translated in the background ({:.0f} ms of work), {} waiting",
                    background_translated, background_ms, pending.size());
      }
    }
    runtime_changed.notify_all();
  }
#endif
}


void NativeShaders::Data::IndexRuntime(uint32_t position) {
  const RuntimeSlot& slot = runtime[position];
  if (slot.container.size() < 36) return;
  bo2zm::shaderlib::Shader shader;
  shader.original = slot.container;
  shader.vertices = (slot.container[3] & 1u) != 0;
  ShaderInput parsed;
  if (const char* reason = ReadContainer(shader, parsed)) {
    if (runtime_index_warnings++ < 8) {
      REXLOG_WARN("[native] runtime shaders: could not index registered shader n{}: {}",
                  uint32_t(index.size() + position), reason);
    }
    return;
  }
  RuntimeMatch candidate;
  candidate.position = position;
  candidate.hash = parsed.fetch_full ? parsed.hash_no_fetch : parsed.hash;
  candidate.normalized = parsed.fetch_full ? std::move(parsed.microcode_no_fetch)
                                           : std::move(parsed.microcode);
  if (parsed.fetch_full) {
    candidate.fetches.reserve(parsed.elements.size());
    for (const auto& element : parsed.elements) candidate.fetches.push_back(element.instruction);
  }
  const auto words = uint32_t(candidate.normalized.size());
  runtime_candidates[{shader.vertices, words}].push_back(std::move(candidate));
  runtime_match_generation.fetch_add(1, std::memory_order_release);
}

int32_t NativeShaders::Data::RuntimeNumber(std::span<const uint8_t> container) {

  if (!translate_mode || container.size() < 36 || container.size() > 65536 || container[0] != 0x10 ||
      container[1] != 0x2A || container[2] != 0x11) {
    return -1;
  }
  const XXH128_hash_t wide = XXH3_128bits(container.data(), container.size());
  std::lock_guard<std::mutex> lock(runtime_mutex);
  auto& positions = runtime_by_hash[wide.low64];
  for (uint32_t position : positions) {
    RuntimeSlot& slot = runtime[position];
    if (slot.high == wide.high64 && slot.size == container.size()) {
      if (slot.container.empty()) {
        slot.container.assign(container.begin(), container.end());
        g_counters.runtime_used.fetch_add(1, std::memory_order_relaxed);
        IndexRuntime(position);
        if (!workers.empty() && slot.state == 0) {
          pending.push_back(position);
          pending_changed.notify_one();
        }
      }
      return int32_t(index.size() + position);
    }
  }
  const uint32_t position = uint32_t(runtime.size());
  RuntimeSlot slot;
  slot.container.assign(container.begin(), container.end());
  slot.low = wide.low64;
  slot.high = wide.high64;
  slot.size = uint32_t(container.size());
  runtime.push_back(std::move(slot));
  positions.push_back(position);
  IndexRuntime(position);
  g_counters.registered.fetch_add(1, std::memory_order_relaxed);
  g_counters.runtime_used.fetch_add(1, std::memory_order_relaxed);
  if (!workers.empty()) {
    pending.push_back(position);
    pending_changed.notify_one();
  }
  if (catalog.is_open()) {
    const struct {
      uint64_t low, high;
      uint32_t size, reserved;
    } r{wide.low64, wide.high64, uint32_t(container.size()), 0};
    catalog.write(reinterpret_cast<const char*>(&r), sizeof(r));
    catalog.flush();
  }
  return int32_t(index.size() + position);
}

const ShaderInput* NativeShaders::Data::MakeRuntime(uint32_t position) {
#if !BO2ZM_RUNTIME_SHADERS
  (void)position;
  return nullptr;
#else
  std::vector<uint8_t> container;
  uint64_t low = 0, high = 0;
  uint32_t size = 0;
  const auto entered = std::chrono::steady_clock::now();

  const auto note_wait = [&entered] {
    const uint64_t us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - entered).count());
    g_counters.draw_waits.fetch_add(1, std::memory_order_relaxed);
    g_counters.us_draw_waits.fetch_add(us, std::memory_order_relaxed);
    uint64_t longest = g_counters.us_longest_wait.load(std::memory_order_relaxed);
    while (us > longest && !g_counters.us_longest_wait.compare_exchange_weak(longest, us)) {
    }
  };
  {
    std::unique_lock<std::mutex> lock(runtime_mutex);
    if (position >= runtime.size()) {
      return nullptr;
    }
    RuntimeSlot& slot = runtime[position];

    bool waited = false;
    while (slot.state == 1 || slot.translating) {
      waited = true;
      runtime_changed.wait(lock);
    }
    if (slot.state == 2) {
      if (waited) {
        note_wait();
      }
      return slot.input;
    }
    if (slot.state == 3) {
      return nullptr;
    }
    slot.state = 1;
    container = slot.container;
    low = slot.low;
    high = slot.high;
    size = slot.size;
  }
  const auto start = std::chrono::steady_clock::now();
  const std::filesystem::path file = RuntimeFile(low, high, size);
  std::vector<uint32_t> spirv;
  bool from_cache = false;
  std::string error;


  {
    std::ifstream in(file, std::ios::binary);
    uint32_t head32[4] = {};
    uint64_t head64[4] = {};
    if (in.read(reinterpret_cast<char*>(head32), 16) && in.read(reinterpret_cast<char*>(head64), 32) &&
        head32[0] == kRuntimeMagic && head32[1] == kRuntimeVersion && head32[2] == size && head32[3] &&
        head64[0] == translator_identity && head64[1] == low && head64[2] == high &&
        !(low == translate::kSceneResampleHashLow && high == translate::kSceneResampleHashHigh)) {
      std::vector<uint8_t> stored(size);
      std::vector<uint32_t> words(head32[3]);
      if (in.read(reinterpret_cast<char*>(stored.data()), std::streamsize(size)) &&
          in.read(reinterpret_cast<char*>(words.data()), std::streamsize(words.size() * 4)) &&
          XXH3_64bits(words.data(), words.size() * 4) == head64[3] && UboOnlySpirv(words)) {
        const XXH128_hash_t check = XXH3_128bits(stored.data(), stored.size());
        if (check.low64 == low && check.high64 == high &&
            (container.empty() || container == stored)) {
          container = std::move(stored);
          spirv = std::move(words);
          from_cache = true;
        }
      }
    }
  }
  translate::Result translated;
  if (spirv.empty() && !container.empty()) {
    translated = translate::Translate(container);
    spirv = std::move(translated.spirv);
    error = translated.error;
    if (!spirv.empty()) {
      WriteRuntimeFile(file, low, high, container, spirv);
    }
  } else if (spirv.empty()) {


    std::lock_guard<std::mutex> lock(runtime_mutex);
    runtime[position].state = 0;
    runtime_changed.notify_all();
    return nullptr;
  }
  const ShaderInput* made_input = nullptr;
  if (!spirv.empty()) {
    bo2zm::shaderlib::Shader s;
    s.original = std::move(container);
    s.hash = XXH3_64bits(s.original.data(), s.original.size());
    s.vertices = s.original.size() >= 4 && (s.original[3] & 1) != 0;
    s.spirv_file = file.string();
    s.spirv_offset = kRuntimeHeader + size;
    s.spirv_words = uint32_t(spirv.size());
    s.spirv_hash = XXH3_64bits(spirv.data(), spirv.size() * 4);
    const uint32_t kills = std::min<uint32_t>(CountKills(spirv), 255);
    const bool shadow_min = HasMarkShadowMin(spirv);
    std::lock_guard<std::mutex> lock(making);
    made_shaders.push_back(std::move(s));
    ShaderInput e;
    e.shader = &made_shaders.back();
    e.number = uint32_t(index.size() + position);
    const char* reason = Read(made_shaders.back(), e);
    if (reason) {
      error = std::string("its container: ") + reason;
    } else {
      if (!e.vertices) {
        e.kills = kills;
        e.discards = e.kills != 1;
        e.shadow_min = shadow_min;
      }
      made_inputs.push_back(std::move(e));
      made_input = &made_inputs.back();
    }
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  note_wait();
  if (!made_input) {
    g_counters.failed.fetch_add(1, std::memory_order_relaxed);
  } else if (from_cache) {
    g_counters.from_cache.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_counters.translated_when_drawn.fetch_add(1, std::memory_order_relaxed);
    g_counters.us_when_drawn.fetch_add(uint64_t(ms * 1000), std::memory_order_relaxed);
  }
  std::lock_guard<std::mutex> lock(runtime_mutex);
  RuntimeSlot& slot = runtime[position];
  slot.input = made_input;
  slot.state = made_input ? 2 : 3;
  runtime_ms += ms;
  const uint32_t number = uint32_t(index.size() + position);
  if (!made_input) {
    if (runtime_failed++ < kRuntimeLogs) {
      REXLOG_WARN("[native] runtime shaders: n{} ({} bytes) could not be translated: {}", number, size, error);
    }
  } else if (from_cache) {
    if (runtime_cached++ < kRuntimeLogs) {
      REXLOG_INFO("[native] runtime shaders: n{} {} from the cache ({:.1f} ms)", number,
                  made_input->vertices ? "VS" : "PS", ms);
    }
  } else if (runtime_translated++ < kRuntimeLogs || runtime_translated % 100 == 0) {
    REXLOG_INFO("[native] runtime shaders: n{} {} translated in {:.1f} ms (HLSL {:.1f}, compile {:.1f}); {} translated, "
                "{} from the cache, {} failed, {:.0f} ms in all",
                number, made_input->vertices ? "VS" : "PS", ms, translated.ms_hlsl, translated.ms_compile,
                runtime_translated, runtime_cached, runtime_failed, runtime_ms);
  }
  runtime_changed.notify_all();
  return made_input;
#endif
}

bool FetchCoherent(const ShaderInput& vs, std::span<const uint32_t> patched) {
  if (!vs.vertices || patched.size() != vs.microcode.size()) {
    return false;
  }
  for (const VertexElement& element : vs.elements) {
    const uint32_t reg_index = (vs.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F;
    bool found = false;
    for (const VertexElement& other : vs.elements) {
      const uint32_t d0 = patched[size_t(other.instruction) * 3];
      if (((d0 >> 12) & 0x3F) == reg_index && (d0 & 0x1F) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

const ShaderInput* NativeShaders::Data::Make(uint32_t number, std::span<const uint8_t> given) {
  if (number >= index.size()) {
    return nullptr;
  }
  if (const ShaderInput* e = made[number].load(std::memory_order_acquire)) {
    return e;
  }
  const IndexEntry& x = index[number];
  if (!(x.flags & kIndexReadable)) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(making);
  if (const ShaderInput* e = made[number].load(std::memory_order_relaxed)) {
    return e;
  }
  if (unmakeable[number]) {
    return nullptr;
  }
  bo2zm::shaderlib::Shader s;
  if (given.empty()) {
    s.original = library.Container(x.container_offset, x.container_size, x.container_hash);
  } else {
    s.original.assign(given.begin(), given.end());
  }
  ShaderInput e;
  const char* reason = s.original.empty() ? "its container could not be read from the pack" : nullptr;
  if (!reason) {
    s.hash = x.container_hash;
    s.vertices = (x.flags & kIndexVertices) != 0;
    s.container_offset = x.container_offset;
    s.spirv_offset = x.container_offset + x.container_size;
    s.spirv_words = x.spirv_words;
    s.spirv_hash = x.spirv_hash;
    s.library = &library;
    made_shaders.push_back(std::move(s));
    e.shader = &made_shaders.back();
    e.number = number;
    reason = Read(made_shaders.back(), e);
    if (!reason && (e.microcode.size() != x.microcode_words ||
                    (e.fetch_full ? e.hash_no_fetch : e.hash) != x.microcode_hash)) {
      reason = "its microcode does not match the index";
    }
  }
  if (reason) {
    unmakeable[number] = 1;
    if (make_warnings++ < 16) {
      REXLOG_WARN("[native] C5a: library shader n{} not usable: {}", number, reason);
    }
    return nullptr;
  }
  if (!e.vertices) {
    e.kills = x.kills;
    e.discards = e.kills != 1;
    e.shadow_min = (x.flags & kIndexShadowMin) != 0;
  }
  made_inputs.push_back(std::move(e));
  made[number].store(&made_inputs.back(), std::memory_order_release);
  g_counters.library_used.fetch_add(1, std::memory_order_relaxed);
  return &made_inputs.back();
}

bool NativeShaders::Data::ReadIndex(const std::filesystem::path& file, const bo2zm::shaderlib::PackInfo& pack) {
  std::ifstream f(file, std::ios::binary | std::ios::ate);
  if (!f) {
    REXLOG_INFO("[native] C5a: no library index yet ({}): making it", file.string());
    return false;
  }
  const auto size = f.tellg();
  std::vector<uint8_t> bytes(size > 0 ? size_t(size) : 0);
  f.seekg(0);
  ShaderIndex read;
  const char* stale = !f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))
                          ? "too short"
                          : DecodeShaderIndex(bytes, pack, read);
  if (stale) {
    REXLOG_INFO("[native] C5a: library index {} ({}): making it again", file.string(), stale);
    return false;
  }
  Take(std::move(read));
  return true;
}

void NativeShaders::Data::MakeIndex(const std::filesystem::path& pack_file, const bo2zm::shaderlib::PackInfo& pack,
                                    const std::filesystem::path& file, const IndexProgress& progress) {
  ShaderIndex made;
  IndexEvents events;
  events.ignored = [](uint32_t number, const char* reason) {
    REXLOG_WARN("[native] C5a: library container {} ignored: {}", number, reason);
  };
  events.progress = progress;
  BuildShaderIndex(library, pack_file, pack, made, events);
  const std::vector<uint8_t> bytes = EncodeShaderIndex(made, pack);
  Take(std::move(made));
  std::string error;
  if (!SaveShaderIndex(file, bytes, error)) {
    REXLOG_WARN("[native] C5a: could not save the library index ({}): the next start makes it again", error);
  }
}

void NativeShaders::Data::Take(ShaderIndex&& from) {
  index = std::move(from.entries);
  fetches = std::move(from.fetches);
  index_stats = from.stats;
}


void NativeShaders::CheckIndex(const std::filesystem::path& file) const {
  Data& d = *data_;
  const auto start = std::chrono::steady_clock::now();
  bo2zm::shaderlib::ShaderLibrary full;
  std::vector<std::pair<uint32_t, bool>> scans;
  try {
    full.Load(file, [&](uint32_t number, const bo2zm::shaderlib::Shader& s) {
      scans.resize(std::max<size_t>(scans.size(), size_t(number) + 1));
      scans[number] = {CountKills(s.spirv), HasMarkShadowMin(s.spirv)};
    });
  } catch (const std::exception& e) {
    REXLOG_WARN("[native] C5a: index check: the pack could not be read: {}", e.what());
    return;
  }
  uint32_t checked = 0, different = 0;
  const auto& shaders = full.shaders();
  for (uint32_t i = 0; i < shaders.size(); ++i) {
    ShaderInput a;
    const char* reason = Read(shaders[i], a);
    const ShaderInput* b = PerNumber(i);
    const char* what = nullptr;
    if (reason || !b) {
      what = (reason != nullptr) == (b == nullptr) ? nullptr : "usable in only one of them";
    } else if (a.vertices != b->vertices || a.microcode != b->microcode || a.hash != b->hash ||
               a.fetch_full != b->fetch_full || a.microcode_no_fetch != b->microcode_no_fetch ||
               a.hash_no_fetch != b->hash_no_fetch || a.outputs != b->outputs ||
               a.constants_bytes != b->constants_bytes || a.constants_runs != b->constants_runs ||
               a.constants_runs_valid != b->constants_runs_valid || b->shader->original != shaders[i].original ||
               bo2zm::shaderlib::Spirv(*b->shader) != bo2zm::shaderlib::Spirv(shaders[i])) {
      what = "a field differs";
    } else if (a.elements.size() != b->elements.size() || a.samplers.size() != b->samplers.size()) {
      what = "elements or samplers differ";
    } else if (!a.vertices && (b->kills != scans[i].first || b->shadow_min != scans[i].second ||
                               b->discards != (scans[i].first != 1))) {
      what = "kills or the shadow minimum differ";
    } else if (IdentifyContainer(shaders[i].original) != b) {
      what = "IdentifyContainer finds another entry";
    }
    for (size_t k = 0; !what && b && k < a.elements.size(); ++k) {
      const VertexElement &x = a.elements[k], &y = b->elements[k];
      if (x.instruction != y.instruction || x.use != y.use || x.index_use != y.index_use) {
        what = "an element differs";
      }
    }
    for (size_t k = 0; !what && b && k < a.samplers.size(); ++k) {
      const SamplerShader &x = a.samplers[k], &y = b->samplers[k];
      if (x.reg_index != y.reg_index || x.type != y.type || x.map_shadows != y.map_shadows) {
        what = "a sampler differs";
      }
    }
    ++checked;
    if (what && ++different <= 12) {
      REXLOG_WARN("[native] C5a: index check: shader n{}: {}", i, what);
    }
  }
  REXLOG_INFO("[native] C5a: index check: {} of {} shaders compared with the whole pack's, {} different ({:.1f} s)",
              checked, d.index.size(), different,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}

bool NativeShaders::Load(const std::filesystem::path& file, const IndexProgress& progress) {
  Data& d = *data_;
  const auto start = std::chrono::steady_clock::now();
  const std::filesystem::path index_file = ShaderIndexPath(file);
  bool made_now = false;
  try {
    if (!REXCVAR_GET(bo2zm_native_shader_library)) {
      throw std::runtime_error("turned off (bo2zm_native_shader_library=false)");
    }
    const bo2zm::shaderlib::PackInfo pack = d.library.Open(file);
    const uint64_t identity[3] = {pack.size, pack.amount, pack.body_hash};
    d.identity = XXH3_64bits(identity, sizeof(identity));
    if (!d.ReadIndex(index_file, pack)) {
      d.MakeIndex(file, pack, index_file, progress);
      made_now = true;
    }
  } catch (const std::exception& e) {
    d.index.clear();
    d.fetches.clear();
    d.index_stats = {};
    d.identity = 0;
#if BO2ZM_RUNTIME_SHADERS

    if (REXCVAR_GET(bo2zm_native_shader_translate) != 0) {
      REXLOG_WARN("[native] C5a: no shader library ({}): {}. Every shader will be translated on the device the "
                  "first time (slower first loads; then from cache/runtime-shaders)",
                  file.string(), e.what());
      d.no_library = true;
    } else
#endif
    {
      REXLOG_WARN("[native] C5a: shader library not available ({}): {}", file.string(), e.what());
      return false;
    }
  }
  const size_t count = d.index.size();
  g_counters.library_size.store(uint32_t(count), std::memory_order_relaxed);
  d.made = std::make_unique<std::atomic<const ShaderInput*>[]>(count);
  for (size_t i = 0; i < count; ++i) {
    d.made[i].store(nullptr, std::memory_order_relaxed);
  }
  d.unmakeable.assign(count, 0);
  d.candidates.clear();
  uint64_t spirv_words = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const IndexEntry& x = d.index[i];
    spirv_words += x.spirv_words;
    if (x.flags & kIndexReadable) {
      d.candidates[{(x.flags & kIndexVertices) != 0, x.microcode_words}].push_back(i);
    }
  }
  d.cache.clear();
  const IndexStats& s = d.index_stats;
  d.loaded = s.vertex + s.pixel > 0 || d.no_library;
  if (d.loaded) {
    d.OpenRuntime();
  }
  if (d.no_library && !d.translate_mode) {
    d.loaded = false;
  }
  if (d.loaded && !d.no_library && REXCVAR_GET(bo2zm_native_shader_index_check)) {
    CheckIndex(file);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("[native] C5a: library index {} {} in {:.0f} ms ({} shaders; each one is read from the pack the first "
              "time it is used)",
              index_file.string(), made_now ? "made from the whole pack and saved" : "read", ms, count);
  REXLOG_INFO("[native] C5a: library with {} shaders ({} vertex, {} pixel); {} groups with the same microcode, {} of "
              "them with different SPIR-V; {:.1f} MB of SPIR-V left in the pack (read when used)",
              s.vertex + s.pixel, s.vertex, s.pixel, s.repeated, s.repeated_different,
              double(spirv_words * 4) / 1048576.0);
  REXLOG_INFO("[native] C5a: pixel shaders that can discard pixels {} of {} (the rest only carry the alpha test's kill: "
              "with no color to write, their stage can be removed)",
              s.with_kill, s.with_kill + s.no_kill);
  REXLOG_INFO("[native] C5a: shadow by minimum: {} pixel shaders sample the shadow map (SHADOWMAP_SAMPLER) "
              "and {} carry tfetch2DShadowMin{}",
              s.with_map_shadows, s.with_min,
              s.with_min && s.with_min == s.with_map_shadows
                  ? " (library with the minimum)"
                  : " (without the minimum: the shadow map is still copied as always)");
  return d.loaded;
}

const ShaderInput* NativeShaders::Identify(bool vertices,
                                                 std::span<const uint32_t> microcode) {
  Data& d = *data_;
  ++d.stats.loads;
  const RawKey key{XXH3_64bits(microcode.data(), microcode.size_bytes()),
                         uint32_t(microcode.size()), vertices};


  if (d.translate_mode) {
    const auto generation = d.runtime_match_generation.load(std::memory_order_acquire);
    if (generation != d.observed_runtime_match_generation) {
      for (auto it = d.cache.begin(); it != d.cache.end();) {
        if (!it->second) it = d.cache.erase(it);
        else ++it;
      }
      d.observed_runtime_match_generation = generation;
    }
  }
  if (auto it = d.cache.find(key); it != d.cache.end()) {
    return it->second;
  }
  ++d.stats.different;

  const ShaderInput* chosen = nullptr;
  uint32_t matches = 0;
  if (d.translate_mode != 2) {
  if (auto c = d.candidates.find({vertices, uint32_t(microcode.size())});
      c != d.candidates.end()) {
    for (uint32_t number : c->second) {

      const IndexEntry& x = d.index[number];
      uint64_t hash = key.hash;
      if (x.fetches_count) {
        d.temporal.assign(microcode.begin(), microcode.end());

        const uint32_t* mask = (x.flags & kIndexFetchFull) ? kFetchNothing : kFetchKeeps;
        for (uint32_t k = 0; k < x.fetches_count; ++k) {
          const size_t instruction = d.fetches[size_t(x.fetches_start) + k];
          for (size_t j = 0; j < 3; ++j) {
            d.temporal[instruction * 3 + j] &= mask[j];
          }
        }
        hash = XXH3_64bits(d.temporal.data(), d.temporal.size() * sizeof(uint32_t));
      }
      if (hash != x.microcode_hash) {
        continue;
      }
      if (!x.fetches_count) {
        d.temporal.assign(microcode.begin(), microcode.end());
      }
      const ShaderInput* e = d.Make(number);
      if (!e || d.temporal != (e->fetch_full ? e->microcode_no_fetch : e->microcode)) {
        continue;
      }
      if (!chosen) {
        chosen = e;
      }
      ++matches;
    }
  }

  }


  if (!chosen && d.translate_mode) {
    std::vector<uint32_t> matching;
    {
      std::lock_guard<std::mutex> lock(d.runtime_mutex);
      const auto it = d.runtime_candidates.find({vertices, uint32_t(microcode.size())});
      if (it != d.runtime_candidates.end()) {
        for (const auto& candidate : it->second) {
          d.temporal.assign(microcode.begin(), microcode.end());
          for (uint16_t instruction : candidate.fetches) {
            const size_t word = size_t(instruction) * 3;
            d.temporal[word] = d.temporal[word + 1] = d.temporal[word + 2] = 0;
          }
          if (XXH3_64bits(d.temporal.data(), d.temporal.size() * sizeof(uint32_t)) == candidate.hash &&
              d.temporal == candidate.normalized) {
            matching.push_back(candidate.position);
          }
        }
      }
    }
    matches = uint32_t(matching.size());
    for (uint32_t position : matching) {
      if (const ShaderInput* e = d.MakeRuntime(position)) {
        chosen = e;
        break;
      }
    }
  }


  const bool warn = d.warnings < kMaxWarnings && (!vertices || bo2zm::native::LogVertexShaderMisses());
  if (chosen) {
    ++d.stats.identified;
    if (matches > 1) {
      ++d.stats.ambiguous;
    }
    if (warn && vertices) {

      ++d.warnings;
      std::string detail;
      for (const VertexElement& element : chosen->elements) {
        const size_t p = size_t(element.instruction) * 3;
        const uint32_t d0 = microcode[p], d1 = microcode[p + 1], d2 = microcode[p + 2];
        const uint32_t fetch = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
        const int32_t offset = int32_t(d2 << 1) >> 9;
        detail += fmt::format(" {}{}:f{}/fmt{}/z{}/o{}{}", kUses[element.use & 0xF],
                               element.index_use, fetch, (d1 >> 16) & 0x3F, d2 & 0xFF, offset,
                               ((d1 >> 30) & 0x1) ? "/mini" : "");
      }
      REXLOG_INFO("[native] C5a: VS n{} ({} words, {} matches):{}", chosen->number,
                  microcode.size(), matches, detail);
    }
  } else {
    ++d.stats.no_identify;
    if (warn) {
      ++d.warnings;
      const auto c = d.candidates.find({vertices, uint32_t(microcode.size())});
      REXLOG_WARN("[native] C5a: unidentified {} shader: {} words, hash {:016X} ({} containers of that type and "
                  "length)",
                  vertices ? "vertex" : "pixel", microcode.size(), key.hash,
                  c != d.candidates.end() ? c->second.size() : 0);


      for (size_t k = 0; c != d.candidates.end() && k < c->second.size() && k < 2; ++k) {
        const ShaderInput* made = d.Make(c->second[k]);
        if (!made) {
          continue;
        }
        const ShaderInput& e = *made;
        const auto& o = e.shader->original;
        const size_t virtual_size = size_t(o[4]) << 24 | size_t(o[5]) << 16 | size_t(o[6]) << 8 | o[7];
        std::string mismatches;
        uint32_t how_many = 0;
        for (size_t i = 0; i < microcode.size(); ++i) {
          uint32_t incoming = microcode[i];
          for (const VertexElement& element : e.elements) {
            const size_t p = size_t(element.instruction) * 3;
            if (i >= p && i < p + 3) {
              incoming &= kFetchKeeps[i - p];
            }
          }
          if (incoming == e.microcode[i]) {
            continue;
          }
          if (++how_many <= 24) {
            const size_t b = virtual_size + i * 4;
            const uint32_t original = uint32_t(o[b]) << 24 | uint32_t(o[b + 1]) << 16 |
                                      uint32_t(o[b + 2]) << 8 | o[b + 3];
            mismatches += fmt::format(" {}:{:08X}/{:08X}", i, microcode[i], original);
          }
        }
        std::string fetch;
        for (const VertexElement& element : e.elements) {
          fetch += fmt::format(" {}", element.instruction);
        }
        REXLOG_WARN("[native] C5a:   versus n{}: {} different words; fetch in the instructions{}:{}",
                    e.number, how_many, fetch, mismatches);
      }
    }
  }
  d.cache.emplace(key, chosen);
  return chosen;
}

const ShaderInput* NativeShaders::IdentifyContainer(
    std::span<const uint8_t> container) const {
  Data& d = *data_;
  if (!d.loaded || container.size() < 24 || container.size() > 65536) {
    return nullptr;
  }

  const auto runtime = [&]() -> const ShaderInput* {
    const int32_t number = d.RuntimeNumber(container);
    return number >= 0 ? d.MakeRuntime(uint32_t(number) - uint32_t(d.index.size())) : nullptr;
  };
  if (d.translate_mode == 2) {
    return runtime();
  }
  const uint64_t hash = XXH3_64bits(container.data(), container.size());
#if BO2ZM_RUNTIME_SHADERS

  if (d.translate_mode && hash == translate::kSceneResampleHash) {
    return runtime();
  }
#endif
  auto it = std::lower_bound(d.index.begin(), d.index.end(), hash,
                             [](const IndexEntry& x, uint64_t h) { return x.container_hash < h; });
  std::optional<XXH128_hash_t> wide;
  for (; it != d.index.end() && it->container_hash == hash; ++it) {
    if (it->container_size != container.size()) {
      continue;
    }
    const uint32_t number = uint32_t(it - d.index.begin());


    if (const ShaderInput* e = d.made[number].load(std::memory_order_acquire)) {
      if (std::equal(container.begin(), container.end(), e->shader->original.begin(), e->shader->original.end())) {
        return e;
      }
      continue;
    }
    if (!wide) {
      wide = XXH3_128bits(container.data(), container.size());
    }
    if (wide->low64 == it->container_hash_low && wide->high64 == it->container_hash_high) {
      return d.Make(number, container);
    }
  }
  return runtime();
}

int32_t NativeShaders::NumberOfContainer(std::span<const uint8_t> container) const {
  Data& d = *data_;
  if (!d.loaded || container.size() < 24 || container.size() > 65536) {
    return -1;
  }


  if (d.translate_mode == 2) {
    return d.RuntimeNumber(container);
  }
  const uint64_t hash = XXH3_64bits(container.data(), container.size());
#if BO2ZM_RUNTIME_SHADERS
  if (d.translate_mode && hash == translate::kSceneResampleHash) {
    return d.RuntimeNumber(container);
  }
#endif
  auto it = std::lower_bound(d.index.begin(), d.index.end(), hash,
                             [](const IndexEntry& x, uint64_t h) { return x.container_hash < h; });
  std::optional<XXH128_hash_t> wide;
  for (; it != d.index.end() && it->container_hash == hash; ++it) {
    if (it->container_size != container.size()) {
      continue;
    }
    const uint32_t number = uint32_t(it - d.index.begin());

    if (const ShaderInput* e = d.made[number].load(std::memory_order_acquire)) {
      if (std::equal(container.begin(), container.end(), e->shader->original.begin(), e->shader->original.end())) {
        return int32_t(number);
      }
      continue;
    }
    if (!wide) {
      wide = XXH3_128bits(container.data(), container.size());
    }
    if (wide->low64 == it->container_hash_low && wide->high64 == it->container_hash_high) {
      return int32_t(number);
    }
  }
  return d.RuntimeNumber(container);
}


const ShaderInput* NativeShaders::PerNumber(uint32_t number) const {
  Data& d = *data_;
  if (!d.loaded) {
    return nullptr;
  }

  return number < d.index.size() ? d.Make(number) : d.MakeRuntime(number - uint32_t(d.index.size()));
}

const char* NameUse(uint8_t use) {
  return kUses[use & 0xF];
}

bool NativeShaders::InLibrary(int32_t number) const {
  return number >= 0 && size_t(number) < data_->index.size();
}

NativeShaderStats NativeShaders::Stats() const {
  return data_->stats;
}

}
