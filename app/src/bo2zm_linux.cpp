#include <rex/platform.h>

#if REX_PLATFORM_GNU_LINUX

#include <SDL3/SDL_messagebox.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace {

struct PageSizeCheck {
  PageSizeCheck() {
    const long page = sysconf(_SC_PAGESIZE);
    if (page == 4096) {
      return;
    }
    char text[512];
    std::snprintf(text, sizeof(text),
                  "This kernel uses %ld KB memory pages; bo2zm needs a kernel with 4 KB pages.\n\n"
                  "Raspberry Pi 5: add kernel=kernel8.img to /boot/firmware/config.txt and reboot.\n"
                  "Other systems: boot a 4 KB page kernel.",
                  page / 1024);
    std::fprintf(stderr, "bo2zm: %s\n", text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "bo2zm", text, nullptr);
    std::_Exit(1);
  }
} g_page_size_check;

}

namespace bo2zm::platform {


void RestartProcessLinux() {
  std::vector<std::string> args;
  if (FILE* f = std::fopen("/proc/self/cmdline", "rb")) {
    std::string arg;
    for (int c; (c = std::fgetc(f)) != EOF;) {
      if (c == '\0') {
        args.push_back(arg);
        arg.clear();
      } else {
        arg.push_back(static_cast<char>(c));
      }
    }
    std::fclose(f);
  }
  if (args.empty()) {
    REXLOG_ERROR("[relaunch] cannot read /proc/self/cmdline");
    return;
  }
  std::vector<char*> argv;
  for (auto& a : args) {
    argv.push_back(a.data());
  }
  argv.push_back(nullptr);
  rex::FlushLogging();
  close_range(3, ~0u, 0);
  execv("/proc/self/exe", argv.data());

  std::perror("bo2zm: relaunch failed");
}

}

#endif
