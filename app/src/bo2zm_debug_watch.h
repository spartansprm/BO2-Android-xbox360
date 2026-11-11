#pragma once

#include <cstdint>
#include <vector>

namespace bo2zm::debug {


enum class Watch { kVolume, kVertices };


void WatchPhysicalPages(const std::vector<uint32_t>& pages, Watch what = Watch::kVolume,
                        const std::vector<uint64_t>* bytes = nullptr);


bool WatchVertices();

}
