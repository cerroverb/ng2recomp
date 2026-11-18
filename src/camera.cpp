// Modern Camera and OT/UT Free Camera: the camera of alicenotmech's Ninja Gaiden II Master
// Edition (https://github.com/alicenotmech/ng2-master-edition, MIT; see THIRD_PARTY.md),
// ported from the modern versions of the game. Their camera functions are recompiled with the
// game's (mods-builtin/master-edition/camera-codegen.bin, ng2_camera.toml); the bytes they
// read (code caves with constants, their state) are written into the image at start
// (WriteCameraCode, camera.bin). Their glue, which on the console branches into the middle of the
// game's camera functions, is done here as hooks:
//
// - Modern Camera: while the latch (0x859FF83D) is set, the follow camera (sub_823039D0) and its
//   first step (sub_823045B0) run their versions, and the hooks in ng2_camera.toml change four
//   places inside the game's and skip a call. Two more of their hooks only swapped a constant for a
//   copy of the same value (50 degrees, 0x83B28460 and their 0x83A891FC): not done. The latch and the 18 values of the camera presets it goes with are
//   set when a camera is set up (sub_822F75D0), as their mod menu does at a save's load, and also
//   at once when the setting changes in play (their menu waits for the next load): the follow
//   camera's preset (entry 7) is then copied into the live camera as its setup would.
// - OT/UT Free Camera: the action camera of OT, UT and throws (sub_82312F80) still runs, but its
//   result is thrown away and the follow camera keeps the player's control.

#include <cstdint>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ppc/context.h>

#include "gpu/guest.h"

REXCVAR_DEFINE_BOOL(modern_camera, false, "Gameplay",
                    "The camera of Master Edition, from the modern versions of the game");
REXCVAR_DEFINE_BOOL(free_camera_ot_ut, false, "Gameplay",
                    "Camera control kept during OT, UT and throws");

namespace {

using ng2::guest::Load32;
using ng2::guest::Virtual;

void Store32(uint32_t address, uint32_t value) {
  *reinterpret_cast<uint32_t*>(Virtual(address)) = rex::byte_swap(value);
}
uint8_t Load8(uint32_t address) {
  return *Virtual(address);
}
void Store8(uint32_t address, uint8_t value) {
  *Virtual(address) = value;
}

// Their latch: Modern Camera in effect (set at a camera's setup).
constexpr uint32_t kLatch = 0x859FF83D;
// Set while the action camera runs for OT/UT Free Camera: their wrappers inside it (patched
// calls, camera-codegen.bin) then skip its own follow steps; 0x859FA218 (never set) would
// turn that off.
constexpr uint32_t kActionFlag = 0x859FA214;
constexpr uint32_t kActionLastId = 0x859FA218 + 4;  // 0x859FA21C
constexpr uint32_t kActionDistance = 0x859FA220, kActionHeight = 0x859FA224;
constexpr uint32_t kActionSave = 0x859FA300;  // camera +48..+383, then +816..+1023

bool Modern() {
  return Load8(kLatch) == 1;
}

// The camera presets (0x83B274B0, 112-byte entries) as their mod menu sets them: address,
// the game's value, Modern Camera's.
struct Preset {
  uint32_t address, original, modern;
};
constexpr Preset kPresets[] = {
    {0x83B277E0, 0x40066666, 0x409CCCCD}, {0x83B277E4, 0x400CCCCD, 0x40A00000},
    {0x83B277E8, 0x40400000, 0x40E00000}, {0x83B277EC, 0x3F800000, 0x40600000},
    {0x83B277F0, 0x3E19999A, 0x3F000000}, {0x83B277F4, 0x3D4CCCCD, 0x3E99999A},
    {0x83B277F8, 0x3DCCCCCD, 0x3F333333}, {0x83B277FC, 0x3DCCCCCD, 0x3F000000},
    {0x83B27800, 0x3FC00000, 0x40800000}, {0x83B27804, 0x3F333333, 0x40000000},
    {0x83B27808, 0x3F333333, 0x40000000}, {0x83B2780C, 0x3F000000, 0x402CCCCD},
    {0x83B27810, 0x3F000000, 0x3FA66666}, {0x83B27814, 0x40800000, 0x41800000},
    {0x83B27510, 0x00000001, 0x00000000}, {0x83B277C4, 0x00000000, 0x3E4CCCCD},
    {0x83B277D4, 0x3E4CCCCD, 0x3FD9999A}, {0x83B277D8, 0xC00CCCCD, 0xC0A00000},
};

// The preset table and the entry the follow camera copies (sub_822F75D0: entry 7 into the camera
// part at +1632, at +384, its +348 then 1).
constexpr uint32_t kPresetTable = 0x83B274B0, kPresetSize = 112, kFollowPreset = 7;

void ApplyPresets() {
  const bool on = REXCVAR_GET(modern_camera);
  if (!on && !Modern()) {
    return;  // (off and never on: the game's own values left alone)
  }
  for (const Preset& p : kPresets) {
    Store32(p.address, on ? p.modern : p.original);
  }
  if (Modern() != on) {
    REXLOG_INFO("camera: Modern Camera {}", on ? "on" : "off");
  }
  Store8(kLatch, on ? 1 : 0);
}

}  // namespace

// The records of camera.bin (embedded, see CMakeLists.txt): written into the image once it is
// loaded, their pages made writable with the presets'.
extern const uint8_t kCameraRecords[];
extern const uint32_t kCameraRecordsSize;

namespace ng2::camera {

void WriteCameraCode(rex::memory::Memory* memory) {
  auto be32 = [](const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  auto writable = [&](uint32_t address, uint32_t size) {
    auto* heap = memory->LookupHeap(address);
    return heap && heap->Protect(address, size,
                                 rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  };
  const uint8_t* data = kCameraRecords;
  if (kCameraRecordsSize < 0x20 || std::memcmp(data, "MECR", 4) != 0) {
    REXLOG_ERROR("camera: the camera records are missing");
    return;
  }
  uint32_t count = be32(data + 8), at = 0x20, bytes = 0;
  for (uint32_t i = 0; i < count && at + 8 <= kCameraRecordsSize; i++) {
    uint32_t address = be32(data + at), size = be32(data + at + 4);
    if (at + 8 + size > kCameraRecordsSize || !writable(address, size)) {
      REXLOG_ERROR("camera: record at 0x{:08X} can't be written", address);
      return;
    }
    std::memcpy(Virtual(address), data + at + 8, size);
    at += 8 + size, bytes += size;
  }
  if (!writable(0x83B27500, 0x320)) {
    REXLOG_ERROR("camera: the camera presets can't be made writable");
  }
  REXLOG_INFO("camera: Master Edition camera code written ({} records, {} bytes)", count, bytes);
}

}  // namespace ng2::camera

// Their functions (recompiled from camera-codegen.bin): the follow camera's and its step A's.
REX_EXTERN(sub_83A8FCB8);
REX_EXTERN(sub_83A8F7B4);

// A camera's setup copies a preset into it: the presets and the latch follow the setting first.
REX_EXTERN(__imp__sub_822F75D0);
REX_HOOK_RAW(sub_822F75D0) {
  ApplyPresets();
  __imp__sub_822F75D0(ctx, base);
}

REX_EXTERN(__imp__sub_823039D0);
REX_HOOK_RAW(sub_823039D0) {
  // A change of the setting in play: applied now, the live follow preset with it.
  if (REXCVAR_GET(modern_camera) != Modern()) {
    ApplyPresets();
    const uint32_t manager = Load32(0x83B28400);  // (what sub_822F75D0 is called with)
    const uint32_t part = manager ? Load32(manager + 1632) : 0;
    if (part && Load32(part + 348) == 1) {
      std::memcpy(Virtual(part + 384), Virtual(kPresetTable + kFollowPreset * kPresetSize), kPresetSize);
    }
  }
  if (Modern()) {
    sub_83A8FCB8(ctx, base);
    return;
  }
  __imp__sub_823039D0(ctx, base);
}

REX_EXTERN(__imp__sub_823045B0);
REX_HOOK_RAW(sub_823045B0) {
  if (Modern()) {
    sub_83A8F7B4(ctx, base);
    return;
  }
  __imp__sub_823045B0(ctx, base);
}

// Step B: the distance stored at 80(r1) (f13), eased while above twice f27 by the mod's state
// value at 0x859F390C (f12) against f31.
void Ng2CameraStepBDistance(PPCRegister& f10, PPCRegister& f12, PPCRegister& f13,
                            PPCRegister& f27, PPCRegister& f31) {
  if (!Modern()) {
    return;
  }
  uint32_t bits = Load32(0x859F390C);
  float s;
  std::memcpy(&s, &bits, 4);
  f12.f64 = double(s);
  if (!(f12.f64 > f31.f64)) {
    return;
  }
  f10.f64 = double(float(f27.f64 + f27.f64));
  if (!(f13.f64 > f10.f64)) {
    return;
  }
  uint32_t kbits = Load32(0x821C9BB8);
  float k;
  std::memcpy(&k, &kbits, 4);
  double v = double(float(f12.f64 * f12.f64));
  v = double(float(v * double(k)));
  v = double(float(f27.f64 - v));
  f12.f64 = v;
  f13.f64 = double(float(v * f13.f64));
  if (!(f13.f64 > f10.f64)) {
    f13.f64 = f10.f64;
  }
}

// Step B: the flag (r11, low byte) left set only in states 79, 86 and 87 of the object (r30).
void Ng2CameraStepBFlag(PPCRegister& r11, PPCRegister& r30) {
  if (!Modern() || (r11.u32 & 0xFF) != 1) {
    return;
  }
  uint32_t state = Load32(r30.u32 + 336);
  if (state != 79 && state != 86 && state != 87) {
    r11.u64 = 0;
  }
}

// Step D's block and sub_823038A8's call, skipped.
bool Ng2CameraModern() {
  return Modern();
}

// A turn limit (f12): 5 degrees instead of 3.
void Ng2CameraTurnLimit(PPCRegister& f12) {
  if (Modern()) {
    uint32_t bits = Load32(0x8228EB8C);
    float v;
    std::memcpy(&v, &bits, 4);
    f12.f64 = double(v);
  }
}

// OT/UT Free Camera: the camera-mode dispatcher's call (return 0x822FF098) in action camera
// kinds 11, 111 and 114 (byte 0x84EF25F5): the camera (r3) saved, the action camera run with its
// own follow steps skipped (their wrappers, kActionFlag), the camera restored and the follow
// camera run instead.
REX_EXTERN(__imp__sub_82312F80);
REX_HOOK_RAW(sub_82312F80) {
  const uint8_t kind = Load8(0x84EF25F5);
  if (uint32_t(ctx.lr) != 0x822FF098 || !REXCVAR_GET(free_camera_ot_ut) ||
      (kind != 11 && kind != 111 && kind != 114) || Load32(0x859FA218) != 0) {
    __imp__sub_82312F80(ctx, base);
    return;
  }
  const uint32_t camera = ctx.r3.u32;
  uint8_t head[48], body[336], tail[208];
  std::memcpy(head, Virtual(camera), sizeof(head));
  Store32(kActionFlag, 1);
  // The horizontal distance and height from the target to the eye, measured once per action
  // (camera +4688 counts them), kept in +944 and +948.
  uint32_t id = Load32(camera + 4688);
  uint32_t last = Load32(kActionLastId);
  Store32(kActionLastId, id);
  if (last + 1 != id) {
    auto f = [&](uint32_t off) {
      uint32_t b = Load32(camera + off);
      float v;
      std::memcpy(&v, &b, 4);
      return v;
    };
    auto put = [&](uint32_t address, float v) {
      uint32_t b;
      std::memcpy(&b, &v, 4);
      Store32(address, b);
    };
    float dx = f(0) - f(16), dz = f(8) - f(24);
    put(kActionDistance, std::sqrt(dz * dz + dx * dx));
    put(kActionHeight, f(4) - f(20));
  }
  Store32(camera + 944, Load32(kActionDistance));
  Store32(camera + 948, Load32(kActionHeight));
  std::memcpy(body, Virtual(camera + 48), sizeof(body));
  std::memcpy(tail, Virtual(camera + 816), sizeof(tail));
  std::memcpy(Virtual(kActionSave), body, sizeof(body));
  std::memcpy(Virtual(kActionSave + sizeof(body)), tail, sizeof(tail));

  __imp__sub_82312F80(ctx, base);

  std::memcpy(Virtual(camera + 48), Virtual(kActionSave), sizeof(body));
  std::memcpy(Virtual(camera + 816), Virtual(kActionSave + sizeof(body)), sizeof(tail));
  std::memcpy(Virtual(camera), head, sizeof(head));
  ctx.r3.u64 = camera;
  sub_823039D0(ctx, base);
  Store32(kActionFlag, 0);
}
