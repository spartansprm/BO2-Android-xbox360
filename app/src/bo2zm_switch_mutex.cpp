#if defined(__SWITCH__)

#include <pthread.h>
#include <sys/lock.h>

#include <atomic>

#include "bo2zm_benchmark.h"
#include "bo2zm_switch_mutex.h"

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t* mutex);

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {


  for (int attempt = 0; attempt < 64; ++attempt) {
    if (pthread_mutex_trylock(mutex) == 0) {
      return 0;
    }
    for (int pause = 0; pause < 8; ++pause) {
      __asm__ __volatile__("yield");
    }
  }
  return __real_pthread_mutex_lock(mutex);
}


namespace {
_LOCK_RECURSIVE_T g_malloc_lock = __LOCK_INITIALIZER_RECURSIVE;
std::atomic<bool> g_malloc_spin{false};
std::atomic<int> g_malloc_spin_factor{0};

bool MallocSpin() {
  if (const int factor = g_malloc_spin_factor.load(std::memory_order_relaxed)) {
    const int arm = bo2zm::benchmark::AbArmCached();
    if (arm >= 0) {
      return (arm & factor) != 0;
    }
  }
  return g_malloc_spin.load(std::memory_order_relaxed);
}
}

extern "C" void __wrap___malloc_lock(struct _reent*) {
  if (MallocSpin()) {
    for (int attempt = 0; attempt < 64; ++attempt) {
      if (__libc_lock_try_acquire_recursive(&g_malloc_lock) == 0) {
        return;
      }
      for (int pause = 0; pause < 8; ++pause) {
        __asm__ __volatile__("yield");
      }
    }
  }
  __libc_lock_acquire_recursive(&g_malloc_lock);
}

extern "C" void __wrap___malloc_unlock(struct _reent*) {
  __libc_lock_release_recursive(&g_malloc_lock);
}

namespace bo2zm {
void SetMallocSpin(bool on) {
  g_malloc_spin.store(on, std::memory_order_relaxed);
}
void SetMallocSpinFactor(int factor) {
  g_malloc_spin_factor.store(factor, std::memory_order_relaxed);
}
}

#endif
