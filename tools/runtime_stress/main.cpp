#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/chrono/clock.h>

#include "bo2zm_test_waits.h"

int main(int argc, char** argv) {
  const double scale = argc > 1 ? std::atof(argv[1]) : 1.0;

  rex::chrono::Clock::set_guest_tick_frequency(50000000);
  const bool ok = bo2zm::test_waits::Run(scale, [](const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  });
  return ok ? 0 : 1;
}
