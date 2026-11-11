#include <atomic>
#include <cstdint>
#include <string>

#include "render/bo2zm_native_hooks.h"
#include "render/bo2zm_native_render.h"
#include "render/bo2zm_reflection_demand.h"


std::atomic<uint64_t> g_bo2zm_frames_game{0};

namespace bo2zm::reflection_demand {
void AnnotateRead() {}
void AnnotateCopy() {}
void AnnotateSwap() {}
bool MeasureVisibility() { return false; }
bool VisibilityChecked() { return false; }
void AnnotateVisible(bool) {}
void AnnotateHidden() {}
void AnnotateWitness(bool) {}
}

namespace bo2zm::guard30 {
void Heartbeat(double) {}
void Report() {}
bool NoShadows(bool of_user) { return of_user; }
}

namespace bo2zm::render_targets {


uint32_t SamplesOriginalModeCurrent(const uint8_t*) { return 1; }
}

namespace bo2zm::scenery_lod {
std::string Summary() { return {}; }
}

namespace bo2zm::native {


}
