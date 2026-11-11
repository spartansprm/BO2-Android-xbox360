#pragma once

#include <cstdio>
#include <format>
#include <string>

#define REXSTRESS_LOG(level, ...) \
  std::fprintf(stderr, "[%s] %s\n", level, std::format(__VA_ARGS__).c_str())
#define REXSYS_DEBUG(...) ((void)0)
#define REXSYS_NOISY_DEBUG(...) ((void)0)
#define REXSYS_INFO(...) REXSTRESS_LOG("info", __VA_ARGS__)
#define REXSYS_WARN(...) REXSTRESS_LOG("warn", __VA_ARGS__)
#define REXSYS_ERROR(...) REXSTRESS_LOG("error", __VA_ARGS__)
#define REXLOG_INFO(...) REXSTRESS_LOG("info", __VA_ARGS__)
