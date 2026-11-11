// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <atomic>
#include <type_traits>
namespace rex {
#if (defined(__ANDROID__) && !defined(__cpp_lib_atomic_ref)) || defined(REX_TEST_ATOMIC_REF_COMPAT)
// NDK r27 libc++ lacks atomic_ref. Only integral load/store are required here;
// use compiler atomics on the existing aligned guest word, without changing ABI.
template <class T> class atomic_ref {
  static_assert(std::is_integral_v<T> || std::is_pointer_v<T>);
  T* value_;
  static constexpr int order(std::memory_order value) {
    switch (value) {
      case std::memory_order_relaxed: return __ATOMIC_RELAXED;
      case std::memory_order_consume: return __ATOMIC_CONSUME;
      case std::memory_order_acquire: return __ATOMIC_ACQUIRE;
      case std::memory_order_release: return __ATOMIC_RELEASE;
      case std::memory_order_acq_rel: return __ATOMIC_ACQ_REL;
      default: return __ATOMIC_SEQ_CST;
    }
  }
 public:
  static constexpr bool is_always_lock_free = __atomic_always_lock_free(sizeof(T), nullptr);
  static constexpr size_t required_alignment = alignof(T);
  explicit atomic_ref(T& value) noexcept : value_(&value) {}
  T load(std::memory_order memory_order = std::memory_order_seq_cst) const noexcept {
    return __atomic_load_n(value_, order(memory_order));
  }
  void store(T value, std::memory_order memory_order = std::memory_order_seq_cst) const noexcept {
    __atomic_store_n(value_, value, order(memory_order));
  }
};
#else
template <class T> using atomic_ref = std::atomic_ref<T>;
#endif
}
