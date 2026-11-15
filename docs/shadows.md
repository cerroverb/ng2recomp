# Shadows (notes)

How the game's character and room shadows work, what was measured for #92 and #31, and
the "Shadows" option (Graphics page, `shadows`). Addresses are Title Update 3.

## The game's shadow map

- One sun-style light per room, orthographic, with a fixed direction (Mission 1's castle hall:
  48 degrees above the horizon, [0.656, -0.746, 0.11]). Because the light is oblique, the
  shadow of a character in the air lands to one side of them: that is the game's own look.
- Each frame the render thread (`sub_822F16D0`) runs the shadow pass `sub_8231C948`. It
  draws the casters into a 1024x1024 or 512x512 depth map (format 33; see below), which two passes blur (pixel shaders
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

## Two shadow maps, and Shadows "Hi-Res"

The game keeps two shadow maps, 1024x1024 and 512x512 (both made at start, `sub_82332240`), and
uses the big one out of fights and the small one in them:
- Each frame `sub_823347E8` goes through 160 entries (24 bytes each from `0x84C3DC60`), asking
  `sub_82334F18` about each one. If any wants the big map it asks for it (`0x83C110EC` = 1), and
  otherwise for the small one (2). It leaves the request alone while a script runs (the running
  script's skip argument, the short at `0x84A69AD4`, is -1, 1 or 2).
- Stage setup (`sub_83655FE0`, `sub_836584F8`, `sub_83672B38`) asks too.
- The main loop (`sub_822F0CA8`) takes the request once a frame: the index at `0x83C11048`
  (0 the big map, 1 the small one) picks the surfaces for that frame.

In Chapter 1's opening the map is 1024x1024 until the first fight, then 512x512. So in a fight
the shadows are blurrier for two reasons: half the texels, over a larger area.

"Shadows" on the Graphics page (`shadows`), Hi-Res: after `sub_823347E8` a request for the small
map is turned into one for the big one, and the big one is asked for whenever the small one
is in use and nothing else is asked (src/extras.cpp), so fights keep 1024x1024. Mission 1 with
it stays on the big map throughout, with the same pictures otherwise. alicenotmech's Master
Edition mod for Xenia Edge has the same option ("Hi-res Shadows").

An earlier option, "Steady" (0.6.1), gave the map a fixed area on Ryu by correcting every caster
and receiver matrix. It kept Ryu's shadow sharp, but enemies away from him lost theirs, and it
only knew the 512x512 map. Hi-Res replaced it. The code is in commit 5031bf6.

## A patch posted in #92

A Xenia patch posted in #92 writes `0x60000000` to `0x8213F640`, `0x8214A8C0` and `0x8214A8C4`.
On Title Update 3 all three are inside shaders:
- `0x8213F640` is the last word of the header of the lit vertex shader for floors and walls (at
  `0x8213F200`). The shader's converted code is the same with it.
- The other two are in the skinned one (at `0x8214A2A0`). They change its fog band choice
  (`FogNearMiddleFar`), not its shadow matrix.

This port converts the game's shaders when it is built, so a patched shader isn't found ("not in
the shader cache") and the patch does nothing here. Measured with it written into memory, the
shadow map's area changed frame by frame exactly as without it.
