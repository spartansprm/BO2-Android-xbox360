bo2-zm-nx game files
====================

Puts your own Black Ops II game files where bo2-zm-nx reads them, in the same switch\bo2zm layout as the SD card,
so you can merge the result onto an SD card that already has bo2zm.nro and the shader library. It does not build
anything: it copies the disc's files, unpacks Title Update 18 and lays out the DLC.

You need Python 3.8 or later (python.org; on Windows tick "Add python.exe to PATH"). Nothing else.

What it needs from you
----------------------

Your own extracted Xbox 360 dump of Call of Duty: Black Ops II, with Title Update 18 and, optionally, your DLC:

    <your disc folder>\
      default.xex, default_mp.xex, *.ff, *.ipak, *.webm, *.bik, ...    the disc's files as extracted
      DLC\415608C3\000B0000\tu00000002_00000000                        Title Update 18
      DLC\415608C3\00000002\<your DLC packages>                        optional

The title update and the DLC can also live anywhere else: point to them with --title-update and --dlc.

The easy way
------------

Put your disc folder next to prepare_game_files.py (any folder name), so it looks like this:

    bo2-zm-nx game files\
      prepare_game_files.py
      README.txt
      lib\
      <your disc folder>\        the one with default_mp.xex
        DLC\415608C3\...

Then open a command prompt in this folder and run:

    python prepare_game_files.py

It finds the folder with default_mp.xex by itself, plus the title update and DLC inside it.

Pointing to your folders
------------------------

    python prepare_game_files.py "<your disc folder>"
    python prepare_game_files.py "<your disc folder>" --out "<output folder>"
    python prepare_game_files.py "<your disc folder>" --title-update "<path to tu00000002_00000000>" --dlc "<DLC folder>"

Options:
    --out <folder>         where to write (default: output\ next to the script)
    --title-update <file>  Title Update 18, if it is not inside the disc folder
    --dlc <folder>         your DLC packages, if they are not inside the disc folder
    --no-dlc               leave the DLC out
    --all-dlc-files        keep every DLC file (by default other languages and multiplayer-only files are skipped)

It checks that the title update is TU18 (the only one bo2-zm-nx runs) and stops with a clear message otherwise. If it
stops for any reason, run the same command again: files already copied are kept.

What it writes
--------------

    <output folder>\
      switch\
        bo2zm\
          game\         the disc's files, plus default.xexp and default_mp.xexp from TU18
          update\       the rest of Title Update 18
          user\
            0000000000000000\
              415608C3\  your DLC (only when you have some)

Putting it on the SD card
-------------------------

Copy the switch folder from the output onto the root of your SD card and let it merge with the switch folder already
there. The SD card then looks like this:

    SD card root\
      switch\
        bo2zm\
          bo2zm.nro                 (already there)
          bo2zm.toml                (already there)
          bo2zm_shaders.spvpack     (already there)
          game\
          update\
          user\0000000000000000\415608C3\

The files this tool writes are your own game data: keep them for your own consoles.
