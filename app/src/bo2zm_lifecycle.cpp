#include "bo2zm_lifecycle.h"

#include <string_view>

#include <rex/ppc.h>

#include "bo2zm_relaunch.h"
#include "bo2zm_updates.h"

#if defined(__SWITCH__)
#include <switch.h>

#include "bo2zm_benchmark.h"
#endif

namespace bo2zm::lifecycle {

bool Prepare() { return true; }

void OnBoot() {
#if defined(__SWITCH__)

  if (bo2zm::benchmark::IsActive()) {
    appletSetAutoSleepDisabled(true);
  }
#endif
}

void OnBenchmarkDone() {}

void Stop() {}

}

namespace bo2zm::updates {
void Command(std::string_view) {}
void EveryFrame(PPCContext&, uint8_t*) {}
void Stop() {}
}

#if defined(__SWITCH__)
namespace {
__attribute__((constructor(101))) void HandOverEarlyAtStart() { bo2zm::HandOverEarly(); }
}
#endif
