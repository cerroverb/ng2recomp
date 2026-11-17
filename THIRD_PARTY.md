# Third-party components

This project's own code is under the GNU General Public License, version 3 or (at your option) any
later version (GPL-3.0-or-later, [LICENSE](LICENSE)). It is built from, and its packages contain, the
following parts, each under its own license. None of them is copied into this repository: they are
checked out into `thirdparty/` at pinned versions, with this port's changes applied from `patches/`.

## Built into the game

| Component | Use | License |
|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (`librexruntime`) | the recompiler and Xbox 360 runtime | BSD-3-Clause (its `LICENSE`; parts derived from Xenia, BSD-3-Clause) |
| [plume](https://github.com/renderbag/plume) | the Vulkan layer of the native renderer | MIT |
| volk, Vulkan Memory Allocator (through plume) | Vulkan loading and memory | MIT |
| Vulkan-Headers (through plume) | | Apache-2.0 / MIT |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) | translates the game's shaders at build time; its `smol-v` is compiled in | MIT |
| [smol-v](https://github.com/aras-p/smol-v) | compressed SPIR-V | MIT |
| SDL 3 (built into the ReXGlue runtime) | window, input, audio output | zlib |
| [xxHash](https://github.com/Cyan4973/xxHash) | hashing | BSD-2-Clause |
| [Xelu's controller and keyboard prompts](https://thoseawesomeguys.com/prompts/) (`assets/prompts`) | the PlayStation, Nintendo and keyboard button prompts, reduced into the game's button atlas (`tools/make_prompts.py`) | CC0 |

The native renderer (`src/gpu/video.cpp`) follows the Vulkan renderer of
[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) (GPL-3.0).

This port's changes to the ReXGlue SDK's recompiler (`patches/rexglue-sdk/0025`, `0026`) include parts
taken from [IzunaRecomp](https://github.com/YaibaKamikaze/IzunaRecomp) (MIT): the detection of the
game's busy-wait loops (`IsSpinLoopHead`), and the plain guest memory accesses with a compiler barrier
at each label. Its notice:

    MIT License

    Copyright (c) 2026 TheSaltTrader (Ninja's Dawn)
    Copyright (c) 2026 The Izuna Project contributors (IzunaRecomp)

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## From the system

These come from the system, except FFmpeg, which the AppImage, the Windows build and the 64-bit ARM Linux build carry:

| Library | License |
|---|---|
| FFmpeg (libavformat, libavcodec, libavutil, libswresample, libswscale) | LGPL-2.1-or-later. The AppImage bundles an LGPL build of FFmpeg 9.0.2 (source: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz, configured as in `packaging/appimage/Dockerfile`), the Windows build ships BtbN's LGPL shared build (`packaging/windows/build.sh`), and the 64-bit ARM Linux build bundles its own LGPL build of FFmpeg 9.0.2 (source: https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n9.0.2.tar.gz, SHA-256 6e374ed621e48faa40639307dff48ba6fe574a509977956d2cce9669b7cc27e9, configured as in `packaging/aarch64/build.sh`); a build from source uses the system's |
| zstd (the embedded shader cache) | BSD-3-Clause |
| The Vulkan loader | Apache-2.0 |
