# Shadows (notes)

How the game's character and room shadows work, what was measured for #92 and #31, and
the "Shadows" option (Graphics page, `shadow_fit`). Addresses are Title Update 3.

## The game's shadow map

- One sun-style light per room, orthographic, with a fixed direction (Mission 1's castle hall:
  48 degrees above the horizon, [0.656, -0.746, 0.11]). Because the light is oblique, the
  shadow of a character in the air lands to one side of them: that is the game's own look.
- Each frame the render thread (`sub_822F16D0`) runs the shadow pass `sub_8231C948`. It
  draws the casters into a 512x512 depth map (format 33), which two passes blur (pixel shaders
  1A99B35E, 1B603BC7) into variance moments (R16G16_SNORM). Receivers sample it at sampler 14
  (VSM: var = M2 - M1^2 + ShadowData.x, p = var / (var + d^2)).
- The area the map covers is fitted each frame to the casters the game draws. It eases
  0.1 to 0.8% a frame, but jumps 4 to 57% in one frame when a caster comes into view or
  leaves it. Those casters leave the main pass in the same frames: the game's own CPU
  visibility (the GPU stream has no occlusion queries).
- With enemies around, the map covers more, so every shadow is smaller and blurrier.

Same on Xenia Edge and the console:
- A Xenia Edge stream of Mission 1 (2607 frames) has 41 one-frame jumps over 2%, up to
  +57%/-37%. Our port had 8 in a 300-frame trace of the same fight.
- Xbox Series X footage of the same hall shows the same small, faint shadows.

## Where the matrices are

The light's matrix only exists as a finished value in the D3D constants:
- device shadow at `0x859F0350`;
- constant ring at `0x40002850`.

It gets there through the engine's command executor:
`sub_837E39F8` -> `sub_837DF938` -> `sub_837DFEC8` -> `sub_837F22B8`.

The CPU code that builds it wasn't found:
- The shadow object at `0x84C6B6C0` keeps its box fields (+24..+44) constant during play. Its
  entries from +64 are up to 4 characters' positions (`sub_82343D78` -> `sub_8231C7F0`).
- `sub_837F22B8` is SetTransform (block `0x859F1360`; types 0 view, 1 projection, 2 world,
  3 per object). The shadow pass's own call (return address `0x8231CA7C`) only sets the world
  to identity.

The shaders name their constants (D3DXSHADER_CONSTANTTABLE in each container's header), with
the same registers in every shader:

| Name | Registers | What |
|---|---|---|
| ViewProj | c29-c32 | the light's view-projection in the shadow pass (set before its first draw) |
| WorldViewProj | c21-c24 | the same with an object's world (character casters) |
| ShadowMat_LPB / ShadowMat_WLPB | c8-c11 | receivers: X * light * bias |

The bias takes clip space to the map:
- u = 0.5x + 0.5 + 0.5/512
- v = -0.5y + 0.5 + 0.5/512

Matrices are row vectors, one row per register, translation in the fourth.

Shaders: the room casters' vertex shader is 14EA8C03; the character casters' are C3A47167,
B69ABEF0 and FF6D3E3A. The first shadow draw of a frame is a character's.

## Shadows "Steady"

`ShadowFitCorrect` in src/gpu/video.cpp: Graphics page, `shadow_fit` 1.

Each frame it reads the game's light matrix from c29 at the first shadow-pass draw. In its
place it uses a fixed area on Ryu:
- 2 units half-width by default (`NG2_TEST_SHADOW_SIZE` tries another). The blur is a few
  texels whatever the area, so a smaller area gives sharper, larger-looking shadows; the game's
  is about 6 to 10 in Mission 1.
- centred a unit above his feet (his position is the global `0x83CD8A80`; on his body, since
  the light is oblique), on whole texels so still shadows don't shimmer.

The depth range stays the game's. Every caster and receiver matrix gets the same change in x
and y:

    x' = x * scale + w * offset

Characters further than about 2 units from Ryu across the light lose their shadows.

Steady isn't used, and the game's area is followed instead, when:
- a script cutscene runs (the running script's skip argument at `0x84A69AD4` is non-zero;
  unskippable ones have 0, so Steady still applies in them);
- Ryu isn't inside the game's own area;
- the game's area is already as tight as Steady's.

Moving in and out of Steady:
- Into Steady: the centre goes to Ryu at once (where the map sits barely shows), and the size
  eases to Steady's (20% of the gap a frame growing, 12% shrinking). Once within 1%, it is
  Steady's exactly.
- The game's area: its edges are followed, outward at 20% of the gap a frame and inward at 12%.
- Taken at once: a new light (another room), a gap over 30 frames, or an area wanted more than
  two half-sizes away.

Edge effects: casters crossing the box edge are cut at it. The receivers' sampler clamps to a
border, so the ground beyond reads the border, not the edge texels.

Correctness: casters and receivers keep matching. On a real frame the corrected receiver
equals the corrected caster times the bias to 1.5e-6.

Which draws get corrected:
- Casters: shadow-pass draws (no colour target, 512x512 depth) whose c29-c32 hold the matrix
  the update read.
- Receivers: draws whose shader declares ShadowMat_*. They keep the last correction until the
  next shadow pass.

An earlier version only eased the game's area. In Mission 1 it cut one-frame jumps over 5%
from 52 to 1, but the shadows still shrank with enemies around.
