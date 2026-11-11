#pragma once
#include <cstddef>
#include <cstdint>
namespace bo2zm::gsc {
void Configure();
struct MenuCodeRange { uint32_t begin = 0, end = 0; };


size_t MenuCodeRanges(MenuCodeRange* out, size_t capacity);


bool MenuAncestry(const uint32_t* positions, size_t count);
}
