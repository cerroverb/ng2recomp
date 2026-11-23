# Shaders

NG2's shaders are translated ahead of time, as Unleashed Recompiled does it:
XenosRecomp turns each Xbox 360 shader container into HLSL and DXC compiles
that to SPIR-V. The result is linked into the executable as a zstd-compressed
cache (`ng2_shaders`, from `src/gpu/shader_cache.h`), and the renderer looks
shaders up by the XXH3 hash of the guest container. No DXIL: Vulkan only.

## Where the shaders are

All 663 containers (426 vertex, 237 pixel; 625 distinct) are in the XEX's
read-only data, 0x8200AC88-0x82240698. The `.ng2` archives contain none that a
byte scan can see, but they mix raw and compressed blocks; the renderer's
CreateVertexShader/CreatePixelShader hooks must report any hash missing from
the cache, so a shader that only appears at runtime is noticed.

## Build

`CMakeLists.txt` does it on every build where the inputs changed:

1. `ng2_dump_image` loads `game_files/default.xex` with ReXGlue in tool mode
   and writes the decrypted, decompressed image (identical to the one the game
   runs with).
2. XenosRecomp (built as an ExternalProject from `thirdparty/XenosRecomp`,
   a fork of upstream 990d03b with this port's changes on its `ng2` branch) scans it and writes
   `shaders/ng2_shader_cache.cpp` in the build directory. Any shader that fails
   to compile fails the build.

`NG2_GAME_DIR` points at another extraction of the game.

Debugging single shaders: `tools/extract_shaders.py <image> <dir>` writes each
container to its own file; `XenosRecomp <file> <out.hlsl> shader_common.h`
translates one.

## What NG2 needed beyond Unleashed

The first change on the fork's `ng2` branch:

- Memory export. 64 vertex shaders are GPU passes drawn one vertex per output
  (skinning and the like): they read their inputs by index and write results
  with `eA`/`eM#` exports, which later draws use as vertex buffers. Ported
  from Xenia's SPIR-V translator (format packing, endian swap, bounds).
- Raw vertex fetches: 330 vertex shaders fetch without a declared vertex
  element; those fetches read guest memory through the vertex fetch
  constants. Full fetches latch the index for the mini fetches after them.
- The vertex index in r0.x, as the hardware provides it (Unleashed only
  needed it for instancing).
- Bool-conditional exec blocks (`cexec`) were run unconditionally upstream;
  NG2 has about 4000 of them.
- Bool constants as the hardware addresses them: 256 bits (VS b0-127, PS
  b128-255). Unleashed packed 16 per stage; NG2 uses at least b33 and
  unnamed ones.
- 32 texture fetch slots (vertex textures use s16/s17) and 1D textures,
  sampled as Nx1 2D textures. Vertex shaders sample mip 0.
- No vertex input attributes: every vertex fetch reads guest memory. Fetches
  of declared elements are placeholders in the microcode (D3D patches them
  from the vertex declaration on the console), so the renderer supplies each
  element slot's fetch constant, offset, stride and format at draw time.
- An NDC transform in every vertex shader (identity with the hardware
  viewport transform, pixels to NDC without it).
- Interpolator order TEXCOORD0-7, COLOR0-1, TEXCOORD8-15, so the rectangle
  list geometry shader can pass what pixel shaders read (TEXCOORD0-7,
  COLOR0-1) within 64 input components.

## Contract with the renderer

Push constants are three GPU addresses (vertex constants, pixel constants,
shared constants), as in Unleashed. Shared constants, in bytes:

| Offset | Contents |
| --- | --- |
| 0 | texture descriptor indices, `[2D, 3D, Cube, 1D][32 slots]` (uint) |
| 512 | sampler descriptor indices `[32]` |
| 640 | bool constants, 8 uints, bit N = hardware bool address N |
| 672 | swapped texcoords mask |
| 676 | alpha threshold |
| 680 | half pixel offset (float2) |
| 688 | guest memory base (uint64): GPU address of guest physical address 0 |
| 696 | the 96 vertex fetch constants, 2 dwords each |
| 1464 | 16 declared vertex elements (uint4): fetch constant, offset and stride in dwords, format flags |
| 1720 | NDC transform (float4) |
| 1736 | alpha test function (RB_COLORCONTROL alpha_func) |

The D3D12/DXIL side of the patch is not maintained (Vulkan only).

Consequence for the renderer: memexport targets and raw-fetched vertex data
are addressed by guest physical address, so vertex buffers live in one GPU
buffer laid out like guest physical memory (`g_GuestMemoryBase`), filled from
CPU writes on unlock, written by skinning shaders, and bound with offsets for
draws. Shaders need `bufferDeviceAddress` and `vertexPipelineStoresAndAtomics`.

## Multiplying by zero

Xenos multiplies as Shader Model 3: +-0 or a denormal times anything, an
infinity or NaN too, is 0 (Xenia's ZeroIfAnyOperandIsZero). Ninja Gaiden II
relies on it: Sonia's skin blends a garbage position stream (NaN) with a
weight of 0, and plain multiplies turned those vertices into NaN (a hole in
her shoulder). mul, mad, dp2add/dp3/dp4, dst's y, muls, mulsprev and mulsc go
through `mulz`/`dotz` in shader_common.h (patch 0009); they are `precise`,
since DXC doesn't carry `precise` into called functions.

## Rounding

Xenos vertex ALUs round sums and products toward zero and never fuse a
multiply and an add (Xenia Edge 3390fc219, 043a7cb40): games split vertex and
bone indices with a multiply then trunc, and round to nearest can push one
into the next. Vertex shaders therefore ask for SPIR-V's RoundingModeRTZ
(32-bit) and declare their registers and outputs `precise` (NoContraction).
DXC only declares the capability for an instruction that asks for it, so each
vertex shader has one `ps + 0` through such a declared instruction. On a GPU
without shaderRoundingModeRTZFloat32 the renderer removes the mode, its
capability and extension from the SPIR-V (video.cpp DropRoundTowardZero); the
log says which at startup ("vertex shaders round toward zero").

## Predicate counters

Xenos shaders nest conditions with a counter in a register: `setp_*_push`
starts a level, `setp_pop` ends one, `setp_inv` flips the innermost. The
translator had `setp_inv` turn a count of 1 into 1 (it must give 0), so the
instructions after it saw the wrong nesting. NG2's point and spot light code
(the vertex shader of rigid pieces such as the archers' bows) runs only with
point lights near, so the bows were lit in bands of colour under the fire
ninpo. Patch 0010 fixes it, and takes the pushes' test from `.w` and result
from `.x` of their sources as Xenos does (NG2's shaders replicate one
component, so their output didn't change).

## Control flow

Xenos jumps are translated as structured `if` blocks wherever they nest: a
forward conditional jump opens a block that ends at its target, and an
unconditional jump that is the last instruction of such a block and lands
past its end, within the enclosing block, is that block's `else`; one that
lands on the next instruction (an empty else) does nothing (patch 0018).
Only shaders with anything else (a jump back, overlapping blocks) are
emitted as Unleashed did, a `while (true) { switch (pc) { ... } }` state
machine. Before 0018 every shader with an unconditional jump took the loop;
NG2's jumps all nest, so none does now (301 of its 663 shaders have an else,
the skinning and lighting vertex shaders among them).

The loop is expensive on AMD's Windows driver (#2): it compiled the vertex
shaders among those to 256 VGPRs and spilled the rest to scratch, up to
23 KB a vertex (vs 8282819E0F311055), so a few skinned draws took most of a
frame whatever the resolution (RX 6800: 55-125 ms of GPU time a frame in
Chapter 1's first fight). Structured, the same shaders use 50-121 VGPRs and
no scratch, and the frame takes 2-4 ms. `NG2_PIPELINE_STATS=1` logs these
numbers from the driver.
