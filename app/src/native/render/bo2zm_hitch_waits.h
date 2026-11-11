#pragma once

#include <atomic>
#include <cstdint>

namespace bo2zm::waits {

enum Kind : uint32_t {
  kHandoffExecutor,
  kHandoffPreparer,
  kRoomRing,
  kGameMid,
  kRingNoJob,
  kRingRegMem,

  kExecutorNoOrders,
  kPreparerList,
  kPreparerOut,
  kPreparerScenery,
  kNumTypes
};

inline std::atomic<uint64_t> g_ns[kNumTypes];
inline std::atomic<uint64_t> g_times[kNumTypes];
inline std::atomic<uint32_t> g_vtable_mid{0};
inline std::atomic<uint32_t> g_caller_mid{0};


inline std::atomic<uint64_t> g_bytes_hash{0};
inline std::atomic<uint64_t> g_hashes_postponed{0};


inline std::atomic<uint64_t> g_draws{0};
inline std::atomic<uint64_t> g_ns_ring_working{0};
inline std::atomic<uint64_t> g_ns_textures{0};
inline std::atomic<uint64_t> g_textures_uploads{0};
inline std::atomic<uint64_t> g_bytes_uploaded{0};
inline std::atomic<uint64_t> g_textures_created{0};


inline std::atomic<uint64_t> g_ns_hash_raw{0};
inline std::atomic<uint64_t> g_ns_hash_data{0};


inline std::atomic<uint64_t> g_ns_waiting_copies{0};
inline std::atomic<uint64_t> g_waits_copies{0};


inline std::atomic<uint64_t> g_ns_helping_copies{0};
inline std::atomic<uint64_t> g_copies_helped{0};


inline std::atomic<uint64_t> g_ns_create_textures{0};
inline std::atomic<uint64_t> g_ns_waiting_bindings{0};
inline std::atomic<uint64_t> g_textures_bound_thread{0};


inline std::atomic<uint64_t> g_ns_hash_snapshot{0};
inline std::atomic<uint64_t> g_textures_hash_thread{0};
inline std::atomic<uint64_t> g_ns_hash_thread{0};
inline std::atomic<uint64_t> g_textures_hash_ring{0};
inline std::atomic<uint64_t> g_ns_hash_ring{0};
inline std::atomic<uint64_t> g_ns_waiting_hashes{0};

inline void Add(Kind type, uint64_t ns) {
  g_ns[type].fetch_add(ns, std::memory_order_relaxed);
  g_times[type].fetch_add(1, std::memory_order_relaxed);
}

}
