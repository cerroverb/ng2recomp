# What to take from the Ninja Gaiden Black port

Survey of ~/ngb-recomp (2026-10-28) for reuse here. NGB is GPL-3.0-or-later
(its xboxrecomp runtime is MIT with LGPL/GPL xemu parts); NG2 can take its
code once NG2 is GPL-3.0-or-later as well.

Worth taking, best first:

1. Windows cross build: `packaging/windows/build.sh`, `deps.txt`,
   `dlldeps.py`, `cmake/toolchains/windows-x64-llvm-mingw.cmake`, `ngb.rc`,
   manifest, icon. llvm-mingw + MSYS2 CLANG64 sysroot, DLL discovery, zip with
   licences. Adapt (names, SDL3). Check ReXGlue builds with mingw first.
2. AppImage: `packaging/appimage/build.sh` and `Dockerfile` (Ubuntu 22.04,
   static libstdc++, "never bundle" list, patchelf, zsync updates). Adapt:
   SDL3, no glib/epoxy; keep the Qt dialog layer (no GTK: the user prefers Qt).
3. Disc install: `src/install.c` (already handles the XGD2 offset, .7z/.zip,
   `.partial` install dirs, zenity/kdialog/GetOpenFileName pickers). Adapt the
   XBE check to an XEX/title ID check (544307D5); replaces
   `tools/xdvdfs_extract.py` for players.
4. Config/save/log dirs and portable mode (`portable.txt`): `src/port.h`,
   `src/main.c`. Copy with renames, after checking ReXGlue's own paths.
5. Test rig: headless gamescope and Proton (umu-run) scripts, the timed/live
   pad-file input parser (`xboxrecomp/src/usb/usb_gamepad.c`, ~200 lines of
   plain C) to wire into ReXGlue's input layer, save loaders. NG2 has no
   input injection yet; this is the piece needed to test past the title.
6. Rebindable controls (`src/bindings.c`, `keyboard.c`, `controls.ini`) and
   NGB's Xelu button prompt art (CC0); NG2 makes its prompts from the game's own buttons.
7. Settings list and ini format (Graphics/Display/Sound/Gameplay/Cheats/
   Controls pages). NGB's menus patch the game's own menu engine; NG2 would
   build them in ImGui like Unleashed, so only the list and the ini carry over.
8. Ideas only: widescreen HUD anchoring heuristics (docs/TECHNICAL.md), the
   F7 draw trace, cheats/memdump hooks (`src/cheats.c`), issue templates.

Not applicable: the x86 lifter and xboxrecomp runtime, the NV2A/D3D8 renderer
(`src/d3d`), Morton swizzle, DirectSound/APU audio.

Things to look for in NG2 because NGB had them: a fixed render-node pool
(NGB's HUD vanished in busy fights), play-time cap.
