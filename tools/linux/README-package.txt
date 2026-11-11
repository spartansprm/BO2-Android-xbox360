bo2-zm-nx for Linux ARM64 (AArch64)
====================================

This folder was built from your own copy of the game and contains code recompiled from it:
keep it for your own machines and do not share it.

Put these next to bo2zm.sh (the same files the Windows build uses):

  game/                    the disc's files, plus default_mp.xexp from title update 18
  update/                  the rest of the title update 18 package
  bo2zm_shaders.spvpack    the shader library built from your game files
  bo2zm_shaders.spvidx

Then run ./bo2zm.sh. Settings are read from bo2zm.toml in this folder. Saves, caches and the DLC packs
go to a user/ folder here if there is one (the DLC in user/0000000000000000/, as on the Switch),
otherwise to $XDG_DATA_HOME/bo2zm (usually ~/.local/share/bo2zm).

You need a 64-bit ARM Linux with glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39 or later)
and 4 KB memory pages, an X11 or Wayland desktop, the Vulkan loader with a driver for your GPU, and
PipeWire, PulseAudio or ALSA. A Vulkan 1.1 driver is required.
