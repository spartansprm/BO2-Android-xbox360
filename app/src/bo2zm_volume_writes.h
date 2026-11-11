#pragma once

#include <cstdint>
#include <vector>

namespace bo2zm::volume_writes {


bool UseMarks();


void TakeMarks(uint32_t physical_first, uint32_t count, std::vector<uint32_t>& pages);


void Missed(uint32_t physical_page);

}
