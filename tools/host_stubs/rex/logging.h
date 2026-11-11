#pragma once
#include <format>
#include <cstdio>
#define REXLOG_INFO(...) std::puts(std::format(__VA_ARGS__).c_str())
#define REXLOG_WARN(...) std::puts(std::format(__VA_ARGS__).c_str())
