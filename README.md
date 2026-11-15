![Ninja Gaiden II Recompiled](docs/images/banner.jpg)

# Ninja Gaiden II Recompiled

**A PC port of Ninja Gaiden II by static recompilation.**

A native Linux and Windows version of *Ninja Gaiden II* (Xbox 360, 2008), made by statically
recompiling the game's own PowerPC code to run directly on a PC, not an emulator. It is built on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and draws with its own Vulkan renderer, which
takes over the game's Direct3D calls instead of emulating the Xbox 360 GPU. It plays at a steady
60 fps on modest hardware (the Steam Deck is the baseline) and adds PC options in the game's own
menus: higher render resolutions, widescreen and ultrawide, anti-aliasing, keyboard and mouse,
PlayStation and Nintendo button prompts, and more.

**You need your own copy of the game.** This project contains none of the game's data (graphics,
sound, movies, text): on first start it asks for your disc image, Games on Demand package or
extracted game, and installs it. It is made for the **World release** (title ID `544307D5`) with
**Title Update 3**, which it asks for too. The downloadable content (Mission Mode, the costume packs)
is supported and optional: it can be added on the first start or later.

## Features

- Native Linux (AppImage) and Windows (x64) builds; Steam Deck friendly
- Its own Vulkan renderer: render resolution up to 8x, anti-aliasing (MSAA up to 8x), anisotropic
  filtering, texture LOD bias, the game's bloom on, reduced or off
- 16:9, 16:10, 21:9, 32:9 and 4:3: the 3D fills the screen, the HUD goes to the screen's corners,
  the menus and movies keep their own frame
- Keyboard and mouse, with the keys' own pictures in the game's button prompts; Xbox,
  PlayStation and Nintendo prompts, following the controller in use
- Options from alicenotmech's Master Edition, in the port's own versions: skip the death screen,
  chapter cards that go on by themselves, a HUD that fades out when not fighting (or hidden),
  no post-effects, smoother HUD and menus, a sharper font
- Skip intro and skip any cutscene; cheats: infinite health and ninpo, unlock difficulties and
  costumes, all weapons and ninpo, all crystal skulls, max health and ninpo
- Low input latency and frame pacing on the display's own refreshes (options)
- Mods: game files replaced by files in a mods folder, nothing of the game's changed
- Render pipelines made in the background and remembered between sessions, so new areas don't
  stutter; the game's movies decoded on the PC (FFmpeg)
- **RECOMP SETTINGS** on the title menu and in the in-game menu; **EXIT GAME** on the title menu

## Performance

The Steam Deck is the baseline for optimization: changes are judged by whether it holds 60 fps in the
game's heaviest scenes.

Measured on a Steam Deck LCD in Game Mode, on battery, with sound, in Chapter 1's first fight
(29 seconds each, the Deck's own performance overlay's frame log), the port at the Deck's default
1x render scale and Xenia Edge at its defaults (November 2026 builds):

| | This port | Xenia Edge |
|---|---|---|
| Average fps | 60.0 | 55.4 |
| 1% low | 57.4 | 20.3 |
| 0.1% low | 55.5 | 17.9 |
| Slowest frame | 18 ms | 56 ms |
| Frames over 20 ms | 0 | 380 |
| CPU load | 26% | 50% |
| GPU load | 52% | 84% |
| Whole Deck's power draw | 10 W | 22 W |

The port holds 60 fps with no slow frame, while the emulator averages 55 fps and drops to 18. The
port draws less than half the power, so a charge lasts about twice as long.

## Shaders

The game's shaders are translated once, when the port is built, not while you play. All 663 of them
are in the game's executable: the build converts each into a Vulkan shader (SPIR-V) and packs them
into the program, so none is compiled at run time.

What a GPU still has to make is pipelines: a shader together with the draw settings it is used
with. This is the step that makes emulators stutter the first time an effect appears. Here:

- At start, the port makes the pipelines of a list recorded by playing through the game (333 of
  them), in the background as the game starts.
- A pipeline that wasn't on that list is made in the background when the game first needs it; the
  game doesn't wait, and the effect it belongs to can be missing for a frame or so
  (`async_pipelines = false` in `ng2.toml` waits instead). It is added to your own list
  (`ng2-pipelines.bin`) and made at every later start.
- The GPU driver keeps the finished pipelines in its own cache, so later starts are quicker than the
  first one after installing.

## Getting started

### What you need

These files, from your own console or discs. The first start asks for them in this order.

1. **The game**, World release (`544307D5`), in any one of these forms:
   - a disc image: `*.iso`
   - the Games on Demand package: the file in `544307D5/00007000` (named like
     `C1200E9B558D04454FA6`), with its `.data` folder next to it
   - an extracted copy: the folder with `default.xex` and the `.ng2` archives
2. **Title Update 3** (required): `TU_1A461UL_000000G000000.00000000000O4`, or the `default.xexp`
   taken from it. On a console it is in `Content/0000000000000000/544307D5/000B0000`. The port is
   built for this update and checks it; the Japanese release's Title Update 3 is refused.
3. **The DLC** (optional): the console's `544307D5/00000002` folder, with Mission Mode and the
   costume packs. It can also be added later, in the game folder's `dlc` folder.

And a 64-bit PC with SSE4.2 (any from about 2009 on) and a GPU with Vulkan 1.3 drivers.

The first start after installing takes longer to load: the render pipelines are made then and
kept, and later starts are quicker.

### Linux and Steam Deck

1. Download `NinjaGaiden2-x86_64.AppImage` from the releases page and make it executable
   (`chmod +x`, or Properties → Allow executing).
2. Run it. On the first start it asks whether to keep everything next to the AppImage (portable
   mode, below), then for your copy of the game, Title Update 3 and, if you have it, the DLC folder. A disc image or package is copied once into the data folder (below).
   More DLC can go in the game folder's `dlc` folder later; it is installed on the next start.
3. On a Steam Deck, add the AppImage to Steam (Add a Non-Steam Game) to play it in Game Mode, and
   choose 16:10 in RECOMP SETTINGS → Display.

Needs a 64-bit Linux with glibc 2.35 or newer (SteamOS, Ubuntu 22.04+, Debian 12, Fedora 36+),
and Python 3. The first-start windows use `kdialog` (SteamOS and KDE desktops), or `qarma` or
`zenity` where kdialog isn't installed (GNOME, Cinnamon, Xfce...). With none of them, start the
AppImage from a terminal and it asks there. An extracted copy of the game next to the AppImage
(`default.xex` beside it, or in a `game_files` or `game` folder there) is used without asking, as
is `NG2_GAME_DIR`; Title Update 3's `default.xexp` goes next to `default.xex` and DLC in its `dlc`
folder.

### Windows

1. Download `NinjaGaiden2-windows-x64.zip` from the releases page and extract it anywhere.
2. Run `ng2.exe`. The first time it asks about portable mode (below), then for the game, Title Update 3 and the DLC as on Linux; a
   disc image or package is copied into `%LOCALAPPDATA%\NinjaGaiden2\game`.

Needs Windows 10 or 11 (64-bit). The Microsoft Visual C++ runtime DLLs are included.

Both packages carry the game twice, for any PC with SSE4.2 and for one with AVX2 (most PCs since
2013, the Steam Deck): the faster one is used where the CPU has it. `NG2_NO_AVX2=1` keeps the other.

### Where things are kept

| | Linux | Windows |
|---|---|---|
| Copied game (from a disc image or package), saves, log (`ng2.log`) | `~/.local/share/ng2-recomp` | `%LOCALAPPDATA%\NinjaGaiden2` |
| Settings (`ng2.toml`), the chosen game folder, the pipeline list (`ng2-pipelines.bin`) | `~/.config/ng2-recomp` | `%LOCALAPPDATA%\NinjaGaiden2` |

**Portable mode:** with a file named `portable.txt` next to the AppImage (or `ng2.exe`), all of
these go into a `userdata` folder next to it instead. The first start offers it. The folder must
be writable (not inside Program Files). Delete `portable.txt` to go back.

`NG2_GAME_DIR=<folder>` starts with that game folder instead of the remembered one (Linux; on
Windows pass `--game_data_root=<folder>`).

## Settings

Everything is in the game's own menus: **RECOMP SETTINGS** on the title menu, and **Recomp
Settings** in the in-game menu (Back, under Save Method). Changes apply at once and are saved;
each defaults to the game's own behaviour.

- **Graphics**: Render Scale (1x–8x, with the resolution it draws at, as "2x 2240x1168");
  Anti-Aliasing (off, or 2x MSAA as on the console, 4x, 8x); Texture Filtering (the game's, or
  4x–16x anisotropic); Shadows (the game's, or Steady: a fixed area on Ryu, so shadows stay
  sharp with enemies around); Bloom (on, reduced, off); Post-Effects (off: no blur, no white
  flashes)
- **Display**: Window Mode (windowed, borderless, fullscreen) and Window Size; Aspect Ratio (16:9,
  16:10, 21:9, 32:9, 4:3); V-Sync (Off, FIFO, Mailbox, Adaptive; a mode the driver lacks falls back
  to FIFO); Frame Pacing (Timer, the game's own 60 Hz clock, or Auto: the game's frames on the
  display's own refreshes, measured from the frames reaching the screen, so no frame is shown
  twice for the drift on a screen a little off 60 Hz, as 59.94; it needs VK_KHR_present_wait, or on
  Windows, where the driver lacks it (AMD), it follows the monitor's own refreshes; else it runs as
  Timer). Timer is the default: use it on a variable-refresh display, G-Sync or FreeSync.
  In the Steam Deck's Game Mode (and any game run through gamescope) Auto acts as Timer: there it
  gave me uneven frame pacing
- **Cheats**: Infinite Health; Infinite Ninpo (each cast's ninpo filled back); Unlock Costumes;
  All Weapons and Ninpo (Level 1 or 3: every melee and projectile weapon, ninpo and ammo, given at a
  new game, a load and a stage's start; what it gives stays in the save when it's turned off, as
  items found); All Crystal Skulls; Max HP and Ninpo (the life and ninpo upgrades full while it's
  on; the save keeps your own)
- **Interface**: HUD (shown, fading out when not fighting, hidden); Hi-Res Font (text drawn from
  glyphs twice the size, sharper above 720p; from the next start); Smooth HUD and Menus (the 2D
  always filtered); Show FPS; Button Prompts (Auto follows the device in use; Xbox, PlayStation,
  Nintendo, Keyboard); Mouse Sensitivity
- **Extras**: Skip Intro; Skip Cutscenes (any, watched or not); Skip Death Screen (straight to the
  game over menu); Auto Chapter Screen (chapter cards and mission briefings go on by themselves);
  Texture LOD Bias (the game's, 0 or -1: less shimmer at higher render scales)
- **Gameplay**: Input Latency (Low: no finished frame waits for the screen; needs
  VK_KHR_present_wait; in the Steam Deck's Game Mode, and any game run through gamescope, it acts as
  Standard, as Low made frames come in pairs there); Unlock Difficulties (every path in New Game and Mission Mode, at once
  either way)

In the in-game menu, a page with six settings has no Restore Defaults row (the list has no room
for it); the title menu's has.

The game's own Options (camera, controller, sound, screen) are where they always were.

Some settings have no menu entry; they go in `ng2.toml`:

- `async_pipelines = false`: make render pipelines as they're needed instead of in the background
  (a new effect can stutter, but never appears a frame late)
- `log_hitches = true`: log every frame that takes over 25 ms with what the renderer spent it on
- `half_pixel_offset = false`: drop the console's half-pixel shift
- `frame_pacing_phase = 5`: where in a refresh Frame Pacing Auto starts a frame (% of it)

## Mods

Mods replace the game's files: models, motions, effects, stages, text, sounds. They go in a `mods`
folder in the data folder (`~/.local/share/ng2-recomp/mods`, `%LOCALAPPDATA%\NinjaGaiden2\mods`,
or `userdata/mods` in portable mode), one folder per mod:

    mods/
      Ryu's New Look/
        mod.toml
        files/
          char/ryu_b/ryu_b.gmd

A file in `files/` replaces the game's file with the same path (`files/char/ryu_b/ryu_b.gmd` is
Ryu's in-game model, `char\ryu_b\ryu_b.gmd`). The game loads it in place of the one in its
archives, so a mod never changes the game folder: delete the mod's folder and the game is as it
was. `mod.toml` is optional:

    name = "Ryu's New Look"
    version = "1.0"
    enabled = true          # false turns the mod off

    # One of the game's own files loaded in place of another (here his costume C model):
    [replace]
    'char/ryu_b/ryu_b.gmd' = 'char/ryu_c/ryu_c.gmd'

Mods are read at start, in their folders' name order: when two replace the same file, the first
one's is used. The log (`ng2.log`) lists each mod and the files it brings. To find which file to
replace, look in `game-files.txt` in the mods folder (every file's path, written by the game at its
first start), or set `log_file_loads = true` in `ng2.toml`: the log then names every file the game
loads.

## Controls

A controller works as on the Xbox 360; a PlayStation or Nintendo controller too, with its own
button prompts. The keyboard and mouse can be used at any time: the game follows whichever was
touched last, and its prompts change to match (or stay on one set: Button Prompts, in Interface).
The mouse moves the camera only while the keyboard and mouse are in use, and the pointer hides while
a controller is.

| Key | Action | Key | Action |
|---|---|---|---|
| W A S D | move | mouse | camera |
| left mouse, J | quick attack (X) | right mouse, K | strong attack (Y) |
| Space | jump (A) | E | projectile (B) |
| F | ninpo (Y + B) | C | water run (A + X) |
| Shift | block (LT) | Q, middle mouse | RT |
| R | LB | V | RB |
| Z / X | stick clicks | Tab / Escape | Back / Start |
| arrows | d-pad | Return / Backspace | A / B |
| Numpad 8 4 5 6 | camera (keys) | | |

The keys can be changed in `ng2.toml` (`keybind_a = "Space,C,Return"` and so on: a list of keys
for each button; a key in two lists presses both, as the game's two-button moves need).

## Known issues

- Some cutscenes may have slight audio delays.
- Overlays and frame limiters that hook the game's presents (MSI Afterburner with RivaTuner, and the
  like) can upset frame pacing: if the game looks choppy at a steady 60 fps, close them completely
  (not only minimized) and try again.
- Frame Interpolation (experimental):
  - Render targets the game keeps from frame to frame without clearing them, and what shaders write
    to memory (memexport), show the previous frame's state in the pictures made between frames.
  - Cherry petals in the opening scene: a second clump appears next to the real one in the pictures
    between.
  - Ryu's shadow sometimes blinks.
  - Moments the game only shows at 60 or less (cuts, partial frames, the death screen) feel worse
    next to the smoother pictures around them than they would at a steady 60.
  - Overlays drawn over the game in exclusive full screen (the AMD overlay, Windows notifications)
    cause some judder while they're up.
  - Windowed and Borderless have been tested much less than Window Mode Fullscreen.
- Tested on a Linux desktop, the Steam Deck, under Proton and in a Windows 11 virtual machine; reports
  from other systems, and from real Windows PCs especially, are welcome.

When reporting a graphics glitch, pressing **F7** while it is on screen records the next frames'
drawing to `~/ng2-trace-<n>` (Linux) or `C:\Users\<you>\ng2-trace-<n>` (Windows): attach that
folder, zipped. For a crash or a freeze, attach the log (`ng2.log`, above).

## To do

- Mouse support in the menus
- A native audio engine
- More gameplay settings
- More visual settings
- Rebindable controls in RECOMP SETTINGS (for now the keys are set in `ng2.toml`)
- An Android port

## Building from source

The build needs the game's code, so it starts from your own disc image. Requirements: clang 20+,
CMake 3.25+, Ninja, Python 3, the Vulkan headers and loader, FFmpeg (libavformat, libavcodec,
libswresample, libswscale).

1. The ReXGlue SDK, pinned at v0.10.0, with this port's patches:

       git clone --recursive https://github.com/rexglue/rexglue-sdk thirdparty/rexglue-sdk
       cd thirdparty/rexglue-sdk && git checkout v0.10.0 && git submodule update --init --recursive
       git am ../../patches/rexglue-sdk/*.patch
       cmake --preset linux-amd64
       cmake --build out/build/linux-amd64 --config Release --target install
       cd ../..

2. XenosRecomp (the shader translator) and plume (the Vulkan layer of Unleashed Recompiled), with
   this port's patches:

       git clone --recursive https://github.com/hedge-dev/XenosRecomp thirdparty/XenosRecomp
       cd thirdparty/XenosRecomp && git checkout 990d03b && git submodule update --init --recursive
       git am ../../patches/xenosrecomp/*.patch
       cd ../..

       git clone https://github.com/renderbag/plume thirdparty/plume
       cd thirdparty/plume && git checkout 1192686 && git submodule update --init contrib/volk contrib/VulkanMemoryAllocator contrib/Vulkan-Headers
       git am ../../patches/plume/*.patch
       cd ../..

   (`git am` makes commits: it needs a git name and email, `git config --global user.name` and
   `user.email`.)

3. Extract the disc (or the Games on Demand package) into `game_files/` and put Title Update 3's
   `default.xexp` next to `default.xex` (see [docs/title-update.md](docs/title-update.md)):

       python3 tools/xdvdfs_extract.py "Ninja Gaiden II (World).iso" game_files
       python3 tools/stfs_extract.py TU_1A461UL_000000G000000.00000000000O4 default.xexp game_files/default.xexp

4. Build. This generates about 560 MB of C++ from the game's code and translates its shaders
   (see [docs/shaders.md](docs/shaders.md)):

       cmake --preset linux-amd64-release
       cmake --build out/build/linux-amd64-release

5. Run it:

       out/build/linux-amd64-release/ng2 --game_data_root=game_files

The AppImage, `packaging/appimage/build.sh`, builds the SDK and the game in an Ubuntu 22.04
container (Docker; `packaging/appimage/Dockerfile`; it also needs rsync,
zsync (`zsyncmake`, for the `.zsync` updaters use) and
[appimagetool](https://github.com/AppImage/appimagetool) on the PATH, or `APPIMAGETOOL=<path>`), so it needs only glibc 2.35 and runs on
SteamOS and other current distributions. It bundles a small FFmpeg with the movies' codecs and
writes `out/appimage/NinjaGaiden2-x86_64.AppImage`. `packaging/appimage/build-local.sh` packs
the native build instead, quicker but only for the machine that built it.

The Windows build is cross-compiled from Linux after the native one: `packaging/windows/build.sh`
(clang and lld against Microsoft's C runtime and Windows SDK from
[xwin](https://github.com/Jake-Shadle/xwin), `xwin --accept-license splat --output ~/.xwin`; plume
also needs its `contrib/D3D12MemoryAllocator` submodule there). It builds the SDK for Windows, fetches
FFmpeg's Windows build, and packs `out/windows/NinjaGaiden2-windows-x64.zip`.

The 64-bit ARM Linux build is cross-compiled the same way, after the native one:
`packaging/aarch64/build.sh` fetches FFmpeg 9.0.2's source and an Ubuntu 24.04 arm64 sysroot,
builds the SDK, a small FFmpeg and the game, and puts them in `out/aarch64-dist`. Besides clang,
lld and the LLVM tools it needs curl, bsdtar, Python 3.11 or newer, gpgv and the Ubuntu archive
keyring in `/usr/share/keyrings` (`ubuntu-keyring` on Arch, `ubu-keyring` on Fedora). The device
needs glibc 2.39 or newer (Ubuntu 24.04 and later; Armada on the AYN Odin 3 Ultra runs it).

That build has no installer: copy `out/aarch64-dist` to the device, put the extracted game in
`game_files/` inside it with Title Update 3's `default.xexp` next to `default.xex` (as in the
build steps above) and DLC in `game_files/dlc/`, and run `./ng2`. Settings, saves and the log
(`ng2.log`) go where the AppImage keeps them (the Linux column above; no portable mode).

How it works: [docs/renderer-plan.md](docs/renderer-plan.md) (the renderer),
[docs/menus.md](docs/menus.md) (the menus), [docs/shaders.md](docs/shaders.md). How to contribute:
[CONTRIBUTING.md](CONTRIBUTING.md); the layout, tools and test rules, also for coding agents:
[AGENTS.md](AGENTS.md).

## Support

If you'd like to support the project: [GitHub Sponsors](https://github.com/sponsors/gamboozino) or [ko-fi.com/gamboozino](https://ko-fi.com/gamboozino)

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay: the recompiler and Xbox 360
  runtime this is built on, itself derived from [Xenia](https://xenia.jp) (BSD-3-Clause)
- [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) by hedge-dev: the native
  renderer follows its Vulkan renderer (GPL-3.0)
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev: the shader translator (MIT)
- [plume](https://github.com/renderbag/plume) by renderbag: the Vulkan layer (MIT)
- [Xelu's controller and keyboard prompts](https://thoseawesomeguys.com/prompts/) by Nicolae "Xelu"
  Berbece: the PlayStation, Nintendo and keyboard button prompts (CC0)
- [Ninja Gaiden II Master Edition](https://github.com/alicenotmech/ng2-master-edition) by
  alicenotmech: a reference for this port's own versions of some of its options
- [Xenia Edge](https://github.com/has207/xenia-edge) by has207: a reference for how the game
  should look and behave, and its GPU traces for comparing frames with the native renderer
- Banner lettering: Chakra Petch by Cadson Demak (SIL Open Font License)

*Ninja Gaiden* is a trademark of Koei Tecmo. This project is not affiliated with or endorsed by Koei
Tecmo or Team Ninja.

## License

This project's code is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version (`GPL-3.0-or-later`; the text is in
[LICENSE](LICENSE)). It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY.
The parts it is built from keep their own licenses: see [THIRD_PARTY.md](THIRD_PARTY.md).
