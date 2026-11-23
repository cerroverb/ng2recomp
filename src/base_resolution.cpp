// ng2 - Ninja Gaiden II static recompilation
//
// base_resolution_720p (#143): the game's 3D drawn at 1280x720,
// the size of its final picture, instead of 1120x584, so that the render
// scales are 1280x720's (2x 2560x1440, 3x 3840x2160: pixel for pixel on those
// screens) and the 3D isn't stretched by 8/7 and 90/73 into the final
// picture. Two places set that size: the scene's surface, made with the
// game's two 1280x720 ones at startup
// (sub_836489C0 calls sub_83882778(width, height, ...) for each; the second
// call is the scene's), and the render manager's scene size (sub_837EB9E0
// stores 1120 and 584 at +64 and +68, then calls sub_837EBBD0 with it in
// r3). Read once, at startup: a change needs a restart.

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "gpu/guest.h"

REXCVAR_DEFINE_BOOL(base_resolution_720p, false, "Video",
                    "The game's 3D drawn at 1280x720 instead of 1120x584 (the render scale "
                    "multiplies it; takes effect after a restart)");

namespace ng2 {

// Fixed for the run: the game makes its surfaces once.
bool Base720p() {
  static const bool on = REXCVAR_GET(base_resolution_720p);
  return on;
}

}  // namespace ng2

namespace {
constexpr uint32_t kSceneSurfaceCall = 0x83648A80;  // return address of the 1120x584 one
constexpr uint32_t kSceneSizeCall = 0x837EBA4C;     // after the size is stored
constexpr uint32_t kWidth = 1280, kHeight = 720;
}  // namespace

REX_EXTERN(__imp__sub_83882778);
REX_HOOK_RAW(sub_83882778) {
  if (ng2::Base720p() && uint32_t(ctx.lr) == kSceneSurfaceCall && ctx.r3.u32 == 1120 &&
      ctx.r4.u32 == 584) {
    ctx.r3.u64 = kWidth;
    ctx.r4.u64 = kHeight;
    REXLOG_INFO("base resolution: the game's 3D at {}x{}", kWidth, kHeight);
  }
  __imp__sub_83882778(ctx, base);
}

REX_EXTERN(__imp__sub_837EBBD0);
REX_HOOK_RAW(sub_837EBBD0) {
  if (ng2::Base720p() && uint32_t(ctx.lr) == kSceneSizeCall) {
    uint32_t manager = ctx.r3.u32;
    if (ng2::guest::Load32(manager + 64) == 1120 && ng2::guest::Load32(manager + 68) == 584) {
      uint32_t size[2] = {rex::byte_swap(kWidth), rex::byte_swap(kHeight)};
      std::memcpy(ng2::guest::Virtual(manager + 64), size, sizeof(size));
    }
  }
  __imp__sub_837EBBD0(ctx, base);
}


// The scene's MSAA: the render manager sets it up (sub_837EB5C8) only if the
// picture's 2x EDRAM size (sub_83804B40: samples x width x height x 4),
// doubled for color and depth, fits the console's 10 MB of EDRAM; 1120x584 does, 1280x720 doesn't, so the game drew it without
// MSAA and the antialiasing setting had nothing to multiply. The host has no
// EDRAM limit: the check passes.
REX_EXTERN(__imp__sub_83804B40);
REX_HOOK_RAW(sub_83804B40) {
  constexpr uint32_t kMsaaCheck = 0x837EB5EC;  // in sub_837EB5C8
  constexpr uint32_t kEdram = 0xA00000;
  // (the return address before the call: the function's own calls change it)
  bool check = ng2::Base720p() && uint32_t(ctx.lr) == kMsaaCheck;
  __imp__sub_83804B40(ctx, base);
  if (check && ctx.r3.u32 * 2ull > kEdram) {
    ctx.r3.u64 = kEdram / 2;
  }
}

// (Left as they are: sub_83640350 builds some effect's offsets from +-0.5/1120
// and +-0.5/584 each frame, 0.57 and 0.62 of a 1280x720 texel instead of 0.5,
// a shift of under a tenth of a texel.)
