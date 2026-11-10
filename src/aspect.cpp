// ng2 - Ninja Gaiden II static recompilation
//
// Aspect Ratio (Display page of RECOMP SETTINGS): the game draws for 16:9.
//
// 3D: the game's only 16:9 float (0x8228EBFC) is the aspect its projections
// are built with (the camera update sub_822FA8C8 builds them from the field
// of view at camera +176). It is set to the chosen aspect: wider screens see
// more at the sides; narrower ones (16:10, 4:3) keep the sides and get a taller
// field of view (Vert+), set around that update.
//
// The picture is still drawn into the game's 16:9 targets, and the
// presenter stretches it to the chosen aspect (src/gpu/video.cpp), so 2D is
// drawn squeezed by the inverse, around the centre of the game's 1280x720
// space, to look right once stretched: sprites and gauges (their four
// pixel-space vertices, 24 bytes apart, at the quad draw sub_8368EFF8),
// text (the 4x4 transform the text pass hands sub_8370DD70 by value in
// r4..r10, a row-vector matrix: column 0 the high words of r4/r6/r8/r10,
// column 1 their low words) and movies (sub_83660FC8 builds a movie's quad
// in clip space: four vertices 16 bytes apart).

#include "aspect.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/logging.h>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "gpu/guest.h"

REXCVAR_DEFINE_INT32(aspect_ratio, 0, "Display", "0: 16:9, 1: 16:10, 2: 21:9, 3: 32:9, 4: 4:3");

namespace {

using ng2::guest::Load32;
using ng2::guest::Virtual;

constexpr float kNative = 16.0f / 9.0f;
constexpr uint32_t kAspectConstant = ng2::aspect::kConstantAddress;
constexpr uint32_t kCameraFov = 176;

float LoadFloat(uint32_t address) {
  uint32_t bits = Load32(address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

void StoreFloat(uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  uint8_t* p = Virtual(address);
  p[0] = uint8_t(bits >> 24), p[1] = uint8_t(bits >> 16), p[2] = uint8_t(bits >> 8),
  p[3] = uint8_t(bits);
}

// How much 2D is squeezed along each axis.
struct Squeeze {
  float x = 1.0f, y = 1.0f;
  bool active() const { return x != 1.0f || y != 1.0f; }
};

Squeeze CurrentSqueeze() {
  float aspect = ng2::aspect::Aspect();
  Squeeze s;
  if (aspect > kNative) {
    s.x = kNative / aspect;
  } else if (aspect < kNative) {
    s.y = aspect / kNative;
  }
  return s;
}


// The 2D pass (sub_8368E2E8) draws 91 buckets in order; bucket i's items
// are a list (item +4: next) from [kBuckets + parity * 728 + 4 * i],
// parity = [kBuckets + 1456] ^ 1, each drawn by its vtable +4 method. Every
// bucket also holds a placeholder (draw method 0x83705CE8). The bucket of
// the item being drawn says what it is (the HUD's pieces have their own).
constexpr uint32_t kBuckets = 0x85325ED8;
constexpr int kBucketCount = 91;
constexpr uint32_t kPlaceholderDraw = 0x83705CE8;

std::unordered_map<uint32_t, int> g_itemBucket;  // render pass thread
int g_drawBucket = -1;
bool g_in2DPass = false;  // render thread: in the 2D pass (HUD, menus)
uint32_t g_drawItem = 0;

// Where a HUD item stays when the screen is wider (or taller): -1 at the
// left (top) edge, 1 at the right (bottom), 0 the centre. Decided by the
// first piece it draws.
struct Anchor {
  int x = 0, y = 0;
  // Placed by the game's projection (already where the widened 3D picture
  // has it): kept at its own centre (cx, cy), its shape squeezed about that.
  bool projected = false;
  float cx = 0.0f, cy = 0.0f;
};
std::unordered_map<uint32_t, Anchor> g_itemAnchor;
bool g_hud = false;  // the gameplay HUD is up this frame (health bar buckets 57-59)
std::atomic<bool> g_hudShown{false};  // the same, for other threads

// Diagnostic (NG2_BUCKET_LOG=1): each bucket's 2D box, once a second.
struct Box {
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  int draws = 0;
};
Box g_boxes[kBucketCount];


std::chrono::steady_clock::time_point g_boxesLogged;
bool BucketLog() {
  static const bool on = std::getenv("NG2_BUCKET_LOG") != nullptr;
  return on;
}

void ScanBuckets() {
  g_itemBucket.clear();
  uint32_t parity = Load32(kBuckets + 1456) ^ 1;
  uint32_t heads = kBuckets + parity * 728;
  bool hud = false;
  for (int i = 0; i < kBucketCount; i++) {
    int guard = 0;
    for (uint32_t item = Load32(heads + 4 * i); item && guard < 4096; item = Load32(item + 4), guard++) {
      g_itemBucket[item] = i;
      if (BucketLog()) {
        // Each bucket's draw methods, once (what draws a HUD piece).
        static std::unordered_map<uint64_t, bool> seen;
        uint32_t method = Load32(Load32(item) + 4);
        if (seen.try_emplace(uint64_t(i) << 32 | method, true).second) {
          REXLOG_INFO("2D bucket {} draw method {:08X}", i, method);
        }
      }
      if (i >= 57 && i <= 59 && Load32(Load32(item) + 4) != kPlaceholderDraw) {
        hud = true;
      }
    }
  }
  g_hud = hud;
  g_hudShown.store(hud, std::memory_order_relaxed);
  // Decided again each frame: the items are render copies from a pool, so
  // an address can be another piece the next frame.
  g_itemAnchor.clear();
}



void NotePoint(float x, float y) {
  if (g_drawBucket < 0) {
    return;
  }
  if (!BucketLog()) {
    return;
  }
  Box& b = g_boxes[g_drawBucket];
  b.x0 = std::min(b.x0, x), b.x1 = std::max(b.x1, x);
  b.y0 = std::min(b.y0, y), b.y1 = std::max(b.y1, y);
}

void NoteQuad(uint32_t vertices, uint32_t stride) {
  if (g_drawBucket < 0) {
    return;
  }
  if (BucketLog()) {
    g_boxes[g_drawBucket].draws++;
  }
  for (uint32_t i = 0; i < 4; i++) {
    NotePoint(LoadFloat(vertices + i * stride), LoadFloat(vertices + i * stride + 4));
  }
}

void LogBoxes() {
  auto now = std::chrono::steady_clock::now();
  if (!BucketLog() || now - g_boxesLogged < std::chrono::seconds(1)) {
    return;
  }
  g_boxesLogged = now;
  std::string line;
  for (int i = 0; i < kBucketCount; i++) {
    Box& b = g_boxes[i];
    if (b.draws) {
      line += fmt::format(" {}:{}[{:.0f},{:.0f}-{:.0f},{:.0f}]", i, b.draws, b.x0, b.y0, b.x1, b.y1);
    }
    b = Box{};
  }
  REXLOG_INFO("2D buckets{}:{}", g_hud ? " (HUD)" : "", line);
}

// Draws an item with its bucket known.
void DrawTagged(PPCContext& ctx, uint8_t* base, void (*draw)(PPCContext&, uint8_t*)) {
  auto it = g_itemBucket.find(ctx.r3.u32);
  int saved = g_drawBucket;
  uint32_t savedItem = g_drawItem;
  g_drawBucket = it == g_itemBucket.end() ? -1 : it->second;
  g_drawItem = ctx.r3.u32;
  draw(ctx, base);
  g_drawBucket = saved;
  g_drawItem = savedItem;
}

// Buckets of the HUD's left column (karma panel 39-41, health bar and ki
// 57-61), and menus' (never moved: pause and inventory 49-50, in-game
// options 70-72, loading and title screens 20, 80-86).
constexpr int kKarmaBucket = 40;
bool LeftHud(int bucket) {
  return (bucket >= 39 && bucket <= 41) || (bucket >= 57 && bucket <= 61);
}
// The frame-rate counter's layer (src/perf_overlay.cpp): the top left corner.
constexpr uint32_t kCounterLayer = 0x15;
bool MenuBucket(int bucket) {
  return bucket == 20 || bucket == 49 || bucket == 50 || (bucket >= 70 && bucket <= 72) ||
         (bucket >= 80 && bucket <= 86);
}

// The combo counter ("12 HITS", its number at 210,240 and its word at 340,240,
// and their animated copies) is drawn with the karma panel too: it stays with
// the left column, not where the bow's reticle would be.
bool ComboCounter(float x, float y) {
  return x < 1280.0f / 3.0f && y > 190.0f && y < 290.0f;
}

// The anchor of the item being drawn, from a point of its first piece.
Anchor ItemAnchor(float x, float y) {
  // The counter's words (copies of its text item, which keep its layer at
  // +8; the game files them in another bucket) are at the top.
  if (g_drawItem && Load32(g_drawItem + 8) == kCounterLayer && y < 48.0f) {
    return {-1, -1};
  }
  if (!g_hud || g_drawBucket < 0 || MenuBucket(g_drawBucket)) {
    return {};
  }
  auto [it, added] = g_itemAnchor.try_emplace(g_drawItem);
  if (added) {
    Anchor& a = it->second;
    if (g_drawBucket == kKarmaBucket && y >= 480.0f) {
      a = {0, 1};  // the boss's health bar, drawn with the karma panel: bottom centre
    } else if (g_drawBucket == kKarmaBucket && x > 2560.0f / 3.0f) {
      a = {1, -1};  // the projectile count, top right
    } else if (g_drawBucket == kKarmaBucket && y > 175.0f && !ComboCounter(x, y)) {
      a = {0, 0, true, x, y};  // the bow's aiming reticle, where the aim points
    } else if (LeftHud(g_drawBucket)) {
      a = {-1, -1};
    } else {
      a.x = x < 1280.0f / 3.0f ? -1 : x > 2560.0f / 3.0f ? 1 : 0;
      a.y = y < 720.0f / 3.0f ? -1 : y > 1440.0f / 3.0f ? 1 : 0;
    }
  }
  return it->second;
}

// v squeezed by k (the presenter stretches it back), kept at its edge.
float Map(float v, float k, int anchor, float size) {
  if (anchor < 0) {
    return v * k;
  }
  if (anchor > 0) {
    return size - (size - v) * k;
  }
  return size * 0.5f + (v - size * 0.5f) * k;
}
// The same as an affine map v * k + offset.
float MapOffset(float k, int anchor, float size) {
  return anchor < 0 ? 0.0f : anchor > 0 ? size * (1.0f - k) : size * 0.5f * (1.0f - k);
}

// Squeezes a quad (4 vertices, x and y first) in place. A quad covering the
// whole screen (a background, a fade) is instead zoomed evenly, about the
// centre, until it covers the wider (or taller) screen: cropped, not
// stretched out of shape.
void SqueezeQuad(uint32_t vertices, uint32_t stride, Squeeze s) {
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (uint32_t i = 0; i < 4; i++) {
    float x = LoadFloat(vertices + i * stride), y = LoadFloat(vertices + i * stride + 4);
    x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
  }
  float kx = s.x, ky = s.y;
  if (x0 <= 1.0f && x1 >= 1279.0f && y0 <= 1.0f && y1 >= 719.0f) {
    float cover = 1.0f / std::min(s.x, s.y);
    kx = cover * s.x, ky = cover * s.y;
    for (uint32_t i = 0; i < 4; i++) {
      uint32_t at = vertices + i * stride;
      StoreFloat(at, Map(LoadFloat(at), kx, 0, 1280.0f));
      StoreFloat(at + 4, Map(LoadFloat(at + 4), ky, 0, 720.0f));
    }
    return;
  }
  Anchor a = ItemAnchor((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
  for (uint32_t i = 0; i < 4; i++) {
    uint32_t at = vertices + i * stride;
    float x = LoadFloat(at), y = LoadFloat(at + 4);
    if (a.projected) {
      StoreFloat(at, a.cx + (x - a.cx) * kx);
      StoreFloat(at + 4, a.cy + (y - a.cy) * ky);
    } else {
      StoreFloat(at, Map(x, kx, a.x, 1280.0f));
      StoreFloat(at + 4, Map(y, ky, a.y, 720.0f));
    }
  }
}

// High and low 32-bit halves of a 64-bit register as floats.
float High(uint64_t value) {
  uint32_t bits = uint32_t(value >> 32);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
float Low(uint64_t value) {
  uint32_t bits = uint32_t(value);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
uint64_t Pair(float high, float low) {
  uint32_t h, l;
  std::memcpy(&h, &high, 4);
  std::memcpy(&l, &low, 4);
  return uint64_t(h) << 32 | l;
}

}  // namespace

namespace ng2::aspect {

bool In2DPass() {
  return g_in2DPass;
}

namespace {
bool g_constantWritable = false;
}  // namespace

bool HudShown() {
  return g_hudShown.load(std::memory_order_relaxed);
}

float SqueezeX() {
  return CurrentSqueeze().x;
}
float SqueezeY() {
  return CurrentSqueeze().y;
}

uint32_t AspectIndex() {
  int32_t index = REXCVAR_GET(aspect_ratio);
  return index >= 0 && index < 5 ? uint32_t(index) : 0;
}

void SetConstantWritable(bool writable) {
  g_constantWritable = writable;
}

// Width / height of the window (0 until the first present).
std::atomic<float> g_windowShape{0.0f};

void SetWindowShape(uint32_t width, uint32_t height) {
  if (height) {
    g_windowShape.store(float(width) / float(height), std::memory_order_relaxed);
  }
}

float Aspect() {
  if (!g_constantWritable) {
    return kNative;  // the 3D would stay 16:9
  }
  switch (AspectIndex()) {
    case 1:
      return 16.0f / 10.0f;
    case 2: {
      // "21:9" monitors are a little wider than 21:9 (2560x1080 2.37, 3440x1440
      // 2.39, 3840x1600 2.4): in a window of about that shape, its own shape,
      // so the picture fills it; else (a 21:9 picture in a 16:9 window) 21:9.
      float window = g_windowShape.load(std::memory_order_relaxed);
      return window >= 2.2f && window <= 2.6f ? window : 21.0f / 9.0f;
    }
    case 3:
      return 32.0f / 9.0f;
    case 4:
      return 4.0f / 3.0f;
    default:
      return kNative;
  }
}

}  // namespace ng2::aspect

// The camera's update (sub_822FA8C8, r3 the camera): its projection
// (sub_822FB378) and right after it its other frustums (camera +688, +752)
// are built from the field of view at +176, so a narrower aspect's taller
// one is set around the whole update; the aspect constant is set first.
REX_EXTERN(__imp__sub_822FA8C8);
REX_HOOK_RAW(sub_822FA8C8) {
  float aspect = ng2::aspect::Aspect();
  if (LoadFloat(kAspectConstant) != aspect && ng2::aspect::g_constantWritable) {
    StoreFloat(kAspectConstant, aspect);
  }
  uint32_t fovAddress = ctx.r3.u32 + kCameraFov;
  if (aspect >= kNative || !ctx.r3.u32) {
    __imp__sub_822FA8C8(ctx, base);
    return;
  }
  float fov = LoadFloat(fovAddress);
  float taller = 2.0f * std::atan(std::tan(fov * 0.5f) * kNative / aspect);
  StoreFloat(fovAddress, taller);
  __imp__sub_822FA8C8(ctx, base);
  // The update may add an effect's change to it (sub_822FDE08): kept.
  StoreFloat(fovAddress, fov + (LoadFloat(fovAddress) - taller));
}

// A sprite's or gauge's quad, in 1280x720 pixels.
REX_EXTERN(__imp__sub_8368EFF8);
REX_HOOK_RAW(sub_8368EFF8) {
  Squeeze s = CurrentSqueeze();
  uint32_t vertices = ctx.r4.u32;
  if (!s.active() || !vertices) {
    if (vertices) {
      NoteQuad(vertices, 24);
    }
    __imp__sub_8368EFF8(ctx, base);
    return;
  }
  constexpr uint32_t kStride = 24, kCount = 4;
  NoteQuad(vertices, kStride);
  uint8_t saved[kStride * kCount];
  std::memcpy(saved, Virtual(vertices), sizeof(saved));
  SqueezeQuad(vertices, kStride, s);
  __imp__sub_8368EFF8(ctx, base);
  std::memcpy(Virtual(vertices), saved, sizeof(saved));
}

// A string's transform.
REX_EXTERN(__imp__sub_8370DD70);
REX_HOOK_RAW(sub_8370DD70) {
  Squeeze s = CurrentSqueeze();
  NotePoint(High(ctx.r10.u64), Low(ctx.r10.u64));
  if (s.active()) {
    Anchor a = ItemAnchor(High(ctx.r10.u64), Low(ctx.r10.u64));
    for (PPCRegister* row : {&ctx.r4, &ctx.r6, &ctx.r8}) {
      row->u64 = Pair(High(row->u64) * s.x, Low(row->u64) * s.y);
    }
    // The translation row.
    float offsetX = a.projected ? a.cx * (1.0f - s.x) : MapOffset(s.x, a.x, 1280.0f);
    float offsetY = a.projected ? a.cy * (1.0f - s.y) : MapOffset(s.y, a.y, 720.0f);
    ctx.r10.u64 = Pair(High(ctx.r10.u64) * s.x + offsetX, Low(ctx.r10.u64) * s.y + offsetY);
  }
  __imp__sub_8370DD70(ctx, base);
}

// A movie's quad, in clip space.
REX_EXTERN(__imp__sub_83660FC8);
REX_HOOK_RAW(sub_83660FC8) {
  uint32_t out = ctx.r3.u32;
  __imp__sub_83660FC8(ctx, base);
  Squeeze s = CurrentSqueeze();
  if (!s.active() || !out) {
    return;
  }
  constexpr uint32_t kStride = 16, kCount = 4;
  for (uint32_t i = 0; i < kCount; i++) {
    uint32_t at = out + i * kStride;
    StoreFloat(at, LoadFloat(at) * s.x);
    StoreFloat(at + 4, LoadFloat(at + 4) * s.y);
  }
}

// The 2D pass: its items' buckets first.
REX_EXTERN(__imp__sub_8368E2E8);
REX_HOOK_RAW(sub_8368E2E8) {
  ScanBuckets();
  g_in2DPass = true;
  __imp__sub_8368E2E8(ctx, base);
  g_in2DPass = false;
  g_drawBucket = -1;
  LogBoxes();
}

// The items' draw methods: sprite, gauge, text, rectangle, and the other.
REX_EXTERN(__imp__sub_8368FDB0);
REX_HOOK_RAW(sub_8368FDB0) {
  DrawTagged(ctx, base, __imp__sub_8368FDB0);
}
REX_EXTERN(__imp__sub_8368F578);
REX_HOOK_RAW(sub_8368F578) {
  DrawTagged(ctx, base, __imp__sub_8368F578);
}
REX_EXTERN(__imp__sub_8370D5C0);
REX_HOOK_RAW(sub_8370D5C0) {
  DrawTagged(ctx, base, __imp__sub_8370D5C0);
}
REX_EXTERN(__imp__sub_8368E888);
REX_HOOK_RAW(sub_8368E888) {
  DrawTagged(ctx, base, __imp__sub_8368E888);
}
REX_EXTERN(__imp__sub_8368EA58);
REX_HOOK_RAW(sub_8368EA58) {
  DrawTagged(ctx, base, __imp__sub_8368EA58);
}

// An untextured rectangle (sub_8368E888 builds it: 4 vertices 16 bytes
// apart, x, y, z, colour), e.g. the health bar's red damage piece.
REX_EXTERN(__imp__sub_8368E500);
REX_HOOK_RAW(sub_8368E500) {
  Squeeze s = CurrentSqueeze();
  uint32_t vertices = ctx.r4.u32;
  constexpr uint32_t kStride = 16, kCount = 4;
  if (vertices) {
    NoteQuad(vertices, kStride);
  }
  if (!s.active() || !vertices) {
    __imp__sub_8368E500(ctx, base);
    return;
  }
  uint8_t saved[kStride * kCount];
  std::memcpy(saved, Virtual(vertices), sizeof(saved));
  SqueezeQuad(vertices, kStride, s);
  __imp__sub_8368E500(ctx, base);
  std::memcpy(Virtual(vertices), saved, sizeof(saved));
}

// A clip item (draw method sub_8369FB08: it saves the device's scissor and
// sets one from the item's x, y, width and height at +16..+28; its partner
// sub_8369FBE8 puts the saved one back). The boss's health bar fill is
// drawn whole and clipped to what is left this way. Its draw is tagged as
// the others, and the rect it sets (D3D's SetScissorRect(device, RECT*),
// in the game's 1280x720 pixels) is mapped as the item's pieces are.
REX_EXTERN(__imp__sub_8369FB08);
bool g_inClipItem = false;
REX_HOOK_RAW(sub_8369FB08) {
  g_inClipItem = true;
  DrawTagged(ctx, base, __imp__sub_8369FB08);
  g_inClipItem = false;
}

REX_EXTERN(__imp__sub_8375C550);
REX_HOOK_RAW(sub_8375C550) {
  Squeeze s = CurrentSqueeze();
  uint32_t rect = ctx.r4.u32;
  if (!s.active() || !g_inClipItem || !g_drawItem || !rect) {
    __imp__sub_8375C550(ctx, base);
    return;
  }
  int32_t r[4];
  for (int i = 0; i < 4; i++) {
    r[i] = int32_t(Load32(rect + 4 * i));
  }
  if (r[0] <= 0 && r[1] <= 0 && r[2] >= 1280 && r[3] >= 720) {
    __imp__sub_8375C550(ctx, base);  // the whole screen: left as it is
    return;
  }
  Anchor a = ItemAnchor((r[0] + r[2]) * 0.5f, (r[1] + r[3]) * 0.5f);
  auto at = [](float v) { return std::max(0, int32_t(std::lround(v))); };
  auto mapX = [&](float v) { return a.projected ? a.cx + (v - a.cx) * s.x : Map(v, s.x, a.x, 1280.0f); };
  auto mapY = [&](float v) { return a.projected ? a.cy + (v - a.cy) * s.y : Map(v, s.y, a.y, 720.0f); };
  int32_t mapped[4] = {at(mapX(float(r[0]))), at(mapY(float(r[1]))), at(mapX(float(r[2]))),
                       at(mapY(float(r[3])))};
  auto store = [](uint32_t address, int32_t value) {
    uint8_t* p = Virtual(address);
    uint32_t v = uint32_t(value);
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
  };
  for (int i = 0; i < 4; i++) {
    store(rect + 4 * i, mapped[i]);
  }
  __imp__sub_8375C550(ctx, base);
  for (int i = 0; i < 4; i++) {
    store(rect + 4 * i, r[i]);
  }
}
