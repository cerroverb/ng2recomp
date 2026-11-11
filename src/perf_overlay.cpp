// ng2 - Ninja Gaiden II static recompilation
//
// show_fps: the frame rate and frame time in a corner, drawn as the game's
// own menu text; and the port's version in the title menu's bottom right
// corner, the same way. A text item like the ones on the Options pages (a
// sprite, then a text part: docs/menus.md) lives in guest memory and is
// queued into each frame's 2D lists right after the game starts the frame
// (sub_822F0EF8, which empties them).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "controls.h"
#include "aspect.h"
#include "gpu/guest.h"
#include "gpu/video.h"
#include "recomp_settings.h"

REXCVAR_DEFINE_BOOL(show_fps, false, "Video",
                    "Show the frame rate and frame time in the top left corner");

namespace {

constexpr uint32_t kItemSize = 0x100;   // sprite (0x90), then the text part
constexpr uint32_t kTextPart = 0x90;
constexpr uint32_t kText = 0xC4;        // the part's UTF-16 string
constexpr uint32_t kTextScale = 0xC8;   // x, y
constexpr uint32_t kTextPosition = 0xD0;  // x, y
constexpr uint32_t kTextChars = 32;
constexpr uint32_t kMenuFontSize = 0x1C;  // as the Options rows ask sub_836CEDE8
// From the frame's anchor (about 953, 650 in the 1280x720 UI) to the top
// left corner (left edge, line center).
constexpr float kX = 32.0f - 953.0f, kY = 16.0f - 650.0f;
constexpr uint32_t kLayer = 0x15;  // aspect.cpp keeps this layer's text in the top left corner
constexpr uint32_t kAllocAttributes = 0xB2800000;
// A text frame like the one the Options pages' arrow lists show their value
// in (UI package 0x78's resource 0x2A), from package 0, which is always
// loaded: resource 0xA.
constexpr uint32_t kPackages = 0x85324808;  // 8 bytes per id, +4 the loaded package
constexpr uint32_t kTextPackage = 0, kTextFrame = 0xA;
constexpr uint32_t kTextColor = 0x85371760;  // a focused row's (16 bytes)

uint32_t g_item = 0;  // 0 until made
uint32_t g_resource = 0;  // the item sprite's
// The version in the title menu's bottom right corner, the same kind of item.
uint32_t g_versionItem = 0;
uint32_t g_versionResource = 0;
// (atomic: the title menu's update may run on another thread than the frame start)
std::atomic<uint64_t> g_frameNumber{0};
std::atomic<uint64_t> g_titleMenuFrame{~0ull};  // the last frame the title menu updated in
constexpr float kVersionX = 1150.0f - 953.0f, kVersionY = 684.0f - 650.0f;
std::chrono::steady_clock::time_point g_last;
double g_seconds = 0.0;  // frame times since the text last changed
uint32_t g_frames = 0;
std::u16string g_shown = u"";

void Store32(uint32_t address, uint32_t value) {
  *reinterpret_cast<uint32_t*>(ng2::guest::Virtual(address)) = rex::byte_swap(value);
}
void StoreFloat(uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Store32(address, bits);
}

// Calls a guest function with r3-r6 below the caller's guest stack, leaving
// the caller's registers as they were; returns r3, or f1 through *f1.
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*),
                   std::initializer_list<uint32_t> args, double* f1 = nullptr) {
  PPCContext saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  uint64_t* registers[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
  size_t i = 0;
  for (uint32_t arg : args) {
    *registers[i++] = arg;
  }
  function(ctx, base);
  uint32_t result = ctx.r3.u32;
  if (f1) {
    *f1 = ctx.f1.f64;
  }
  ctx = saved;
  return result;
}

}  // namespace

REX_EXTERN(__imp__sub_83835960);  // XMemAlloc(size, attributes)
REX_EXTERN(__imp__sub_83704630);  // a sprite's constructor
REX_EXTERN(__imp__sub_8242C048);  // a text part's constructor
REX_EXTERN(__imp__sub_836CEDE8);  // the menu font's scale for a size -> f1
REX_EXTERN(__imp__sub_83691AD8);  // a text item into this frame's 2D lists: (item, layer)
REX_EXTERN(__imp__sub_837047E0);  // a sprite's resource: (sprite, resource)
REX_EXTERN(__imp__sub_83704978);  // a sprite's update

namespace {

// A text item at (x, y) from the frame's anchor, or 0.
uint32_t NewTextItem(PPCContext& ctx, uint8_t* base, float x, float y) {
  uint32_t item = CallGuest(ctx, base, __imp__sub_83835960,
                            {kItemSize + kTextChars * 2, kAllocAttributes});
  if (!item) {
    return 0;
  }
  std::memset(ng2::guest::Virtual(item), 0, kItemSize + kTextChars * 2);
  CallGuest(ctx, base, __imp__sub_83704630, {item});
  CallGuest(ctx, base, __imp__sub_8242C048, {item + kTextPart});
  Store32(item + kText, item + kItemSize);
  // Left aligned and centered on its line; +0xF0: placed by its measured size
  // (as the lists' value text is).
  Store32(item + kTextPart + 8, (ng2::guest::Load32(item + kTextPart + 8) & 0xFFFC3FFF) | 0x4000);
  Store32(item + 0xF0, 1);
  StoreFloat(item + kTextPosition, x);
  StoreFloat(item + kTextPosition + 4, y);
  return item;
}

bool MakeItem(PPCContext& ctx, uint8_t* base) {
  g_item = NewTextItem(ctx, base, kX, kY);
  return g_item != 0;
}

// The item's text (ASCII, up to kTextChars - 1).
void SetText(uint32_t item, const std::string& text) {
  uint8_t* chars = ng2::guest::Virtual(item + kItemSize);
  std::memset(chars, 0, kTextChars * 2);
  for (size_t i = 0; i < text.size() && i + 1 < kTextChars; i++) {
    chars[2 * i + 1] = uint8_t(text[i]);
  }
}

// The text frame, while its package is loaded.
uint32_t FindTextFrame() {
  using ng2::guest::Load32;
  uint32_t loaded = Load32(kPackages + 8 * kTextPackage + 4);
  if (!loaded || Load32(loaded + 0x14) < 3) {
    return 0;
  }
  uint32_t list = Load32(Load32(loaded + 0x20) + 8);
  if (!list || Load32(list + 0x14) <= kTextFrame) {
    return 0;
  }
  return Load32(Load32(list + 0x20) + 4 * kTextFrame);
}

// The item queued for this frame in the menu font: its sprite on the text
// frame (while that package is loaded), scaled, coloured as a focused row.
// *resource is the frame the sprite was last set to.
void QueueText(PPCContext& ctx, uint8_t* base, uint32_t item, uint32_t* resource) {
  uint32_t frame = FindTextFrame();
  if (!frame) {
    return;
  }
  if (frame != *resource) {
    *resource = frame;
    CallGuest(ctx, base, __imp__sub_837047E0, {item, frame});
  }
  // The menu font's scale, once its font is loaded.
  double scale = 0.0;
  CallGuest(ctx, base, __imp__sub_836CEDE8, {kMenuFontSize}, &scale);
  if (!std::isfinite(scale) || scale <= 0.0) {
    return;
  }
  StoreFloat(item + kTextScale, float(scale));
  StoreFloat(item + kTextScale + 4, float(scale));
  std::memcpy(ng2::guest::Virtual(item + 0x40), ng2::guest::Virtual(kTextColor), 16);
  CallGuest(ctx, base, __imp__sub_83704978, {item});
  CallGuest(ctx, base, __imp__sub_83691AD8, {item, kLayer});
}

// The version, while the title menu is up.
void VersionFrame(PPCContext& ctx, uint8_t* base) {
  uint64_t menu = g_titleMenuFrame.load(std::memory_order_relaxed);
  if (menu == ~0ull || menu + 1 < g_frameNumber.load(std::memory_order_relaxed)) {
    return;
  }
  if (!g_versionItem) {
    g_versionItem = NewTextItem(ctx, base, kVersionX, kVersionY);
    if (!g_versionItem) {
      return;
    }
    SetText(g_versionItem, "v" NG2_VERSION);
  }
  QueueText(ctx, base, g_versionItem, &g_versionResource);
}

}  // namespace

// From the title menu's per-frame update (recomp_settings.cpp's hook of
// sub_8366FBC0): the version is shown while it runs.
void TitleMenuFrame() {
  g_titleMenuFrame.store(g_frameNumber.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void CheatsFrame();  // cheats.cpp

// The game starting a frame: time it, and queue the text for it.
REX_EXTERN(__imp__sub_822F0EF8);
REX_HOOK_RAW(sub_822F0EF8) {
  __imp__sub_822F0EF8(ctx, base);
  ng2::video::MarkFrameStart();
  CheatsFrame();
  ng2::controls::Frame();
  ng2::recomp_settings::WindowFrame();
  auto now = std::chrono::steady_clock::now();
  static const bool logFps = [] {
    const char* v = std::getenv("NG2_FPS_LOG");
    return v && v[0] == '1';
  }();
  static double logSeconds = 0.0;
  static uint32_t logFrames = 0;
  if (g_last != std::chrono::steady_clock::time_point()) {
    double frame = std::chrono::duration<double>(now - g_last).count();
    g_seconds += frame;
    g_frames++;
    // NG2_FPS_LOG=1: the game's frame rate in the log once a second.
    if (logFps) {
      logSeconds += frame;
      logFrames++;
      if (logSeconds >= 1.0) {
        REXLOG_INFO("fps {:.1f}", logFrames / logSeconds);
        logSeconds = 0.0;
        logFrames = 0;
      }
    }
  }
  g_last = now;
  g_frameNumber.fetch_add(1, std::memory_order_relaxed);
  VersionFrame(ctx, base);
  // In gameplay only (the HUD up), not on loading screens and menus.
  if (!REXCVAR_GET(show_fps) || !ng2::aspect::HudShown() || (!g_item && !MakeItem(ctx, base))) {
    return;
  }
  // Averaged over half a second, so it can be read.
  if (g_seconds >= 0.5 || g_shown.empty()) {
    double frameTime = g_frames ? g_seconds / g_frames : 0.0;
    // With interpolation, the pictures shown (its and the game's frames).
    static uint64_t lastPictures = 0;
    uint64_t pictures = ng2::video::PicturesPresented();
    if (ng2::video::InterpolationOn() && lastPictures && pictures > lastPictures && g_seconds > 0.0) {
      frameTime = g_seconds / double(pictures - lastPictures);
    }
    lastPictures = pictures;
    std::string text = frameTime > 0.0 ? fmt::format("{:.0f} FPS  {:.1f} ms", 1.0 / frameTime,
                                                     frameTime * 1000.0)
                                       : std::string("-- FPS");
    g_shown.assign(text.begin(), text.end());
    g_shown.resize(std::min<size_t>(g_shown.size(), kTextChars - 1));
    SetText(g_item, text);
    g_seconds = 0.0;
    g_frames = 0;
  }
  QueueText(ctx, base, g_item, &g_resource);
}
