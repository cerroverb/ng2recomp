# Working on this project

Notes for contributors and coding agents: how the project is laid out, how to build and test it, and
the rules that keep a test run from disturbing the machine it runs on.

## Layout

- `src/`: the hand-written part: the app and its hooks (`main.cpp`, `ng2_app.h`), the native
  renderer (`src/gpu/video.cpp`, `d3d_hooks.cpp`, shaders in `src/gpu/shaders/`), the menus
  (`recomp_settings.cpp`: RECOMP SETTINGS on the title menu and in the in-game menu, the title
  menu's entries; `gpu/menu_label.cpp`: the pictures of their labels and the Options header), the
  frame-rate counter (`perf_overlay.cpp`), movies on the host (`src/movie/`), the scripted
  controller for tests (`script_input.h`), mods (`mods.cpp`: game files replaced through the
  game's own loose-file loading; how it works is in its header comment).
- `generated/default/`: the recompiled game code, generated at build time from the player's
  `default.xex`. **Never commit it**, nor the game's data (`game_files/`, `*.iso`, `*.xex`, saves,
  captures of the game's pictures).
- `ng2_manifest.toml`, `ng2_codegen.toml`, `ng2_pointer_funcs.toml`: the ReXGlue project: manifest,
  codegen overrides (function boundaries, fibers, setjmp/longjmp), and functions only reached through
  pointers (`tools/find_pointer_funcs.py` on an image dumped with `NG2_DUMP_IMAGE`).
- `patches/`: changes to the ReXGlue SDK, XenosRecomp and plume (`git am` onto the pinned
  versions in `thirdparty/`, which aren't committed).
- `packaging/`: the AppImage (`appimage/build.sh`: the portable one, built in a container;
  `appimage/build-local.sh`: the native build, for this machine only), the Windows build
  (`windows/build.sh`) and their resources. Both packages carry the game built twice, x86-64-v2
  (`ng2`) and x86-64-v3 (`ng2-avx2`); `src/cpu_handover.cpp` starts the second where the CPU has
  AVX2 (`NG2_NO_AVX2=1` keeps the first). `portable.txt` next to the package keeps everything in
  `userdata/` beside it (`packaging/appimage/AppRun`, `src/windows_setup.cpp`).
- `docs/`: `renderer-plan.md` (the renderer, with what was measured), `menus.md` (the game's menus,
  reverse-engineered), `shaders.md`, the D3D function maps (`d3d-map-*.md`), `xenia-notes.md`.
  The game is Title Update 3 since 2026-11-03 (Title Update 2 from 2026-11-02); older notes use
  TU2 or 1.0 addresses, which `title-update.md` maps (`tools/map_addresses.py` made the maps).

## Building

See the README. After the first build, `cmake --build out/build/linux-amd64-release --target ng2`
rebuilds the app alone.

## Test runs

A test run must not disturb the machine it runs on: no window on the desktop, no sound, no input
devices, and none of the player's settings or saves.

- `tools/run-headless-windows.sh` does the same for the Windows build under Proton (umu-run).
- `tools/run-headless.sh <seconds> <out dir> [game options]` runs the game in a headless gamescope
  with its own HOME, settings (`<out dir>/ng2.toml`), user data and log (`<out dir>/ng2.log`, which
  rotates at 5 MB: read `ng2.1.log` first). `NG2_SHOTS="30 60"` takes screenshots at those seconds.
- `NG2_PAD_SCRIPT=30000:start,34000:a,...` plays a controller (`<ms>:<button>[:<hold ms>]`;
  buttons and sticks in `src/script_input.h`). From boot, `30000:start,34000:a,37000:a,40000:a,
  58000:a,63000:a,80000:a,262000:a,280000:a` reaches Chapter 1's first fight at about 280 s (TU3:
  the chapter card takes the press at 80 s); Back about 300 s opens the in-game menu. With the
  Mission Mode DLC installed, New Game first asks for a costume: add `43000:a,46000:a`.
- `tools/collect_pipelines.sh <out dir>` remakes the packaged pipeline seed
  (`packaging/ng2-pipelines-seed.bin`): headless runs through Chapter 1 and Mission Mode (missions
  1-12, fighting), their pipeline lists merged by `tools/merge_pipelines.py`. The game makes the
  seed's pipelines in the background at start (src/gpu/video.cpp, `MakeListedPipelines`), moved to
  the player's antialiasing. Remake it when shaders or the pipeline key change.
- `NG2_ROOM_JUMP=<from>:<to>` swaps a room in the story script (src/room_jump.cpp lists the rooms
  with cutscenes): with the New Game path above, `3:5` plays the cutscene before Chapter 2 at about
  80-110 s.
- The native renderer draws by default; to compare with Xenia's set `NG2_RENDERER=xenos` and add
  `--gpu_plugin=xenos --headless` (Xenia's draws cutscenes black in this build).

## Environment

- `NG2_RENDERER=xenos`: the SDK's Xenia GPU emulation draws instead of the native renderer (dev
  builds only: the packages leave out the GPU plugin, `librexgpu-xenos`). The native renderer runs
  with no GPU emulation (src/gpu/native_gpu.cpp).
- `NG2_AUDIO=0`: no audio device; the game's audio still runs.
- `NG2_BARYCENTRIC=1` or `0`: cut-off parts' id interpolated in the shader, or by the hardware
  (default: in the shader on NVIDIA, where the GPU supports it).
- `NG2_CONFIG=<file>`: settings file instead of `<exe dir>/ng2.toml`.
- `NG2_PROFILE=1`: time per renderer stage, once a second, and every 5 s (Linux) the busiest threads' CPU
  time ("threads, ms of CPU each second"; "(renderer)" is the game's render thread, "Render
  Commands" ours). `NG2_HITCHES=1` (or `log_hitches`): slow frames with what they were spent on.
  `NG2_FPS_LOG=1`: the game's frame rate once a second.
- `NG2_PIPELINE_STATS=1`: what the driver compiled each shader pair to, logged once a pair (registers,
  scratch, instructions; VK_KHR_pipeline_executable_properties, plume patches 0011 and 0012: the driver feature is only enabled with it). On AMD the vertex
  stage is `ShaderProperties.gs`; `scratchMemUsageInBytes` above 0 there means spilled registers.
- `NG2_PRESENT_GAPS=1`: the time between real presents (on the thread that makes them) each second.
  Test switches for choppy play at a steady 60 (#36, #46): presents are held until 90% of a refresh
  after the last (V-Sync FIFO or Mailbox only; on by default since 0.5.58, `NG2_TEST_EVEN_PRESENT=0` turns it off); `NG2_TEST_GAMESCOPE_AUTO=1` lets
  Frame Pacing Auto work under gamescope, where it otherwise acts as Timer (experimental: if gamescope
  reports a frame shown when it arrives, the lock can follow its own presents). At 60 fps the game's own
  frame limiter (sub_822F0BF8) doesn't run (its interval word is 0): the presents pace the game.
- `NG2_LATENCY_LOG=1`: every 10 s, from the game reading the pad to that frame's present and to its
  reaching the screen (the display clock, VK_KHR_present_wait). `NG2_TEST_REFRESH=59` (run-headless.sh)
  sets the headless display's rate.
- `NG2_RENDER_THREAD=0`: Vulkan commands, submits and presents on the game's render thread again
  instead of the "Render Commands" thread (for comparisons).
- Exclusive full screen (Windows): with Frame Interpolation on and Window Mode Fullscreen (`fullscreen`
  and `exclusive_fullscreen`), the swapchain takes the display (VK_EXT_full_screen_exclusive, plume
  patches 0017, 0020 and 0021), so its presents are flipped by the display instead of composed by DWM, which
  drops pictures at high refresh rates; decided again when either changes. `NG2_EXCLUSIVE=1` takes it
  always, `2` leaves it to the driver, `0` never.
- Frame Interpolation (RECOMP SETTINGS, Display; `frame_interpolation` 0 off, 1 a picture each refresh,
  2 each second, each one below 120 Hz, 3 2x: one picture between the game's frames; experimental,
  off by default; switched live), or `NG2_INTERP=1`/`0` over it: each frame's draws
  drawn again with their constants and bone palettes part of the way from the last frame's, a picture for each refresh of the
  display, as far along as when it is shown (`NG2_INTERP_REFRESH=<Hz>`: paced as for that display, for tests).
  Draws are matched by mesh, then by shaders and constants; stats every 600 frames. When building the pictures
  takes the game below 60 (a big fight on a slow CPU) it drops to half rate, then pauses them for a while.
  Best in Window Mode Fullscreen (exclusive). `NG2_INTERP_DEBUG=1` leaves out the draws without their own match.
- `--log_slow_reads=<ms>`: log game file reads that take at least that long (thread, size, file).
  `--async_file_reads=false`: reads the game asks to be asynchronous block its thread again.
  `--test_read_delay_us_per_mb=12000`: slow every read to about SD card speed, to measure loading
  hitches on a fast disk.
- `NG2_TRACE_FRAME=t<sec>+<count>`: log every draw of those frames.
- F7, or `NG2_TRACE_FILE=<file>` (starts when the file appears): every draw of the next
  `NG2_TRACE_FRAMES` (60) frames and a small picture every 10, in `~/ng2-trace-<n>` (`HOME` decides
  where).
- `NG2_DUMP_TEXTURES=<dir>`: write decoded textures (PGM/PAM/DDS).
- `NG2_TRACE_PICTURES=<n>`: a trace picture every n frames (10; 0: none). `NG2_TRACE_PICTURES_ONLY=1`:
  F7 takes the pictures alone, no draws or memory (a trace's own stall can hide what it's after).
  `NG2_TRACE_STACKS=1`: each traced draw with its guest call chain. Traced draws carry their shader
  constants ("consts"). `NG2_TRACE_ALL_CONSTANTS=1`: every traced frame's vertex and pixel shader
  constants as the guest has them (`vs-constants-<n>.bin`, `ps-constants-<n>.bin`, a record per
  draw); small resolves up to 512x512 (the shadow map) are written as the GPU holds them.
- `NG2_DEBUG_SKIP=<vs hash prefix>[:<count>],...`: those draws aren't made; with
  `NG2_DEBUG_SKIP_BLINK=1` only 3 s of every 6 (to see what a part of the picture is).
- `NG2_TEST_TOGGLE_LOG=1`: logs draws whose shader constants switch between two sets of values from
  frame to frame (once each, with what differs: constants, vertex data, the textures sampled, render
  state). `NG2_TEST_WAIT_GPU=1`: the CPU waits for the GPU at each present.
- `NG2_READBACK_SYNC=0`: read-back resolves applied at the frame slot's next use again instead of at
  the end of the next present (the game's brightness and object animation read them: flicker,
  chest jitter); `=1` also with frame interpolation, where it is off by default (it cost frames).
- `NG2_PLAYER_SLOTS=1`: each controller its own player again; by default every controller is player
  one (only player one is signed in, #89).
- `NG2_TEST_STALE_LOG=1`: once a second, the resolved textures whose guest memory was found changed
  since (read from guest memory instead of the GPU's copy); `NG2_TEST_NO_STALE=1` always uses the GPU's
  copy.
- `NG2_DEBUG_HIGHLIGHT=<vs hash>|<hash>,<hash>...|cycle:<s>|all|rigid`: draw a vertex shader's
  draws in magenta (a list or all: a colour per shader, logged; rigid: the unskinned ones;
  `NG2_DEBUG_EXCLUDE=<hash>,...` leaves some out).
- `tools/xtr_draws.py <trace.xtr>`: draws, constants (`--consts`), registers (`--regs`) and
  buffers (`--dump`) of a Xenia Edge GPU trace, to compare a frame with ours.
- `NG2_DUMP_IMAGE=<file>`: write the loaded XEX image for analysis.
- `NG2_D3D_STATS=1`: D3D draw, clear, resolve and swap counts every second.
- `NG2_DRAW_STACKS=1`: each distinct chain of guest callers behind a draw, logged once (what draws
  a picture). `NG2_BUCKET_LOG=1`: each 2D render bucket's box per second and its draw methods
  (src/aspect.cpp).
- Mission Mode is a quick way to reach bosses headless (Mission 04, Ruler of Lightning). Quick
  menu: d-pad up, then up, left and A equips the bow.
- `NG2_MOVIES=xmedia`: the game's own movie player instead of FFmpeg; `NG2_MOVIE_TRACE=1` logs it.
- `NG2_PAD_LIVE=<file>`: presses each line appended to the file.

## Reverse engineering

Ghidra (headless) on the loaded image works well for this game: import the image raw at base
`0x82000000` as `PowerPC:BE:64:A2ALT-32addr`. The recompiled C++ in `generated/default/` carries each
instruction as a comment, which helps where Ghidra stops. What is known about the menus is in
`docs/menus.md`.
