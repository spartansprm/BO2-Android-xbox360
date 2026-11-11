#pragma once
#include <stdexcept>
#undef assert
#define assert(expr) ((expr) ? (void)0 : throw std::runtime_error("assert failed: " #expr))
