// ng2 - Ninja Gaiden II static recompilation
//
// The game's achievements in the port (achievements.h).

#include "achievements.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/ui/image_decode.h>

#include "game_icon.h"
#include "gpu/video.h"
#include "lock_icon.h"
#include "overlay.h"
#include "recomp_settings.h"
#include "settings_panel.h"

REXCVAR_DEFINE_BOOL(achievement_notifications, true, "Achievements",
                    "An unlocked achievement shown in the bottom right corner");

namespace {

using rex::system::AchievementInfo;

// XACH flags: 0x8, shown while locked (else a secret one).
constexpr uint32_t kShownLocked = 0x8;
constexpr uint32_t kIconKeyBase = 0x41430000;  // "AC" + the image id

std::u16string Utf16(const std::string& text) {
  std::u16string out;
  for (size_t i = 0; i < text.size();) {
    uint32_t c = uint8_t(text[i]);
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c = extra == 3 ? c & 7 : extra == 2 ? c & 15 : extra == 1 ? c & 31 : c;
    i++;
    for (int k = 0; k < extra && i < text.size(); k++, i++) {
      c = (c << 6) | (uint8_t(text[i]) & 0x3F);
    }
    if (c >= 0x10000) {
      c -= 0x10000;
      out += char16_t(0xD800 + (c >> 10));
      out += char16_t(0xDC00 + (c & 0x3FF));
    } else {
      out += char16_t(c);
    }
  }
  return out;
}

rex::system::KernelState* Kernel() {
  return rex::system::kernel_state();
}

// An achievement's icon as a UI image, decoded from the game's XDBF once.
uint32_t IconOf(const AchievementInfo& info) {
  if (!info.image_id) {
    return 0;
  }
  uint32_t key = kIconKeyBase + (info.image_id & 0xFFFF);
  if (ng2::video::HasUiImage(key)) {
    return key;
  }
  auto* kernel = Kernel();
  if (!kernel) {
    return 0;
  }
  const auto db = kernel->title_xdbf();
  if (!db.is_valid()) {
    return 0;
  }
  const auto block = db.GetEntry(rex::system::util::XdbfSection::kImage, uint64_t(info.image_id));
  if (!block) {
    return 0;
  }
  int width = 0, height = 0;
  std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(block.buffer, block.size, width, height);
  if (rgba.empty() || width <= 0 || height <= 0) {
    ng2::video::SetUiImagePixels(key, 0, 0, {});  // (not tried again: HasUiImage)
    return 0;
  }
  ng2::video::SetUiImagePixels(key, uint32_t(width), uint32_t(height), std::move(rgba));
  return key;
}

// The padlock secret achievements show while locked, made once from the
// game's grey tiles (lock_icon.h); 0 if they couldn't be read.
uint32_t LockIcon() {
  constexpr uint32_t kKey = 0x4C4B0001;  // "LK"
  if (ng2::video::HasUiImage(kKey)) {
    return kKey;
  }
  auto* kernel = Kernel();
  if (!kernel) {
    return 0;
  }
  const auto db = kernel->title_xdbf();
  if (!db.is_valid()) {
    return 0;
  }
  std::vector<std::vector<uint8_t>> tiles;
  for (uint32_t i = 0; i < ng2::lock_icon::kTileCount; i++) {
    const auto block =
        db.GetEntry(rex::system::util::XdbfSection::kImage, uint64_t(ng2::lock_icon::kFirstTile + i));
    if (!block) {
      continue;
    }
    int width = 0, height = 0;
    std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(block.buffer, block.size, width, height);
    if (width == int(ng2::lock_icon::kSize) && height == int(ng2::lock_icon::kSize)) {
      tiles.push_back(std::move(rgba));
    }
  }
  std::vector<uint8_t> lock = ng2::lock_icon::Build(tiles);
  if (lock.empty()) {
    REXLOG_WARN("achievements: no padlock icon ({} of the game's tiles read)", tiles.size());
    ng2::video::SetUiImagePixels(kKey, 0, 0, {});  // (not tried again: HasUiImage)
    return 0;
  }
  ng2::video::SetUiImagePixels(kKey, ng2::lock_icon::kSize, ng2::lock_icon::kSize, std::move(lock));
  return kKey;
}

// The toast's box: the game's own frame (the Missions screens' chrome
// border, 519x344) round its dark panel (126x74), from their sprite packs
// in the archives. The frame is scaled as a whole to the toast's height, so
// its border and ornaments keep their shape; its two ends (with the horns)
// are kept, and the stretch between them is repeated, there and back, as
// often as the toast's width needs. Drawn at twice the UI's size, under the
// toast's texts.
std::filesystem::path g_gameRoot;
constexpr uint32_t kFrameFile = 4634, kPanelFile = 4645;
constexpr int kFrameW = 519, kFrameH = 344, kFrameEnd = 200, kPanelW = 126, kPanelH = 74;
// The frame's opening (source pixels): inside its border, clear of it.
constexpr float kOpenLeft = 26.0f, kOpenRight = 489.0f, kOpenTop = 44.0f, kOpenBottom = 299.0f;
// The frame's border, at its middle: the panel reaches it.
constexpr float kBorderLeft = 19.0f, kBorderRight = 495.0f, kBorderTop = 36.0f, kBorderBottom = 306.0f;
constexpr uint32_t kBoxKeyBase = 0x42580000;  // "BX" + its width

struct Rgba {
  int w = 0, h = 0;
  std::vector<uint8_t> p;
};

// Premultiplied, bilinear, edges clamped.
std::array<float, 4> Sample(const Rgba& im, float x, float y) {
  x = std::clamp(x, 0.0f, float(im.w - 1)), y = std::clamp(y, 0.0f, float(im.h - 1));
  int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, im.w - 1), y1 = std::min(y0 + 1, im.h - 1);
  float fx = x - float(x0), fy = y - float(y0);
  std::array<float, 4> out{};
  auto add = [&](int px, int py, float weight) {
    const uint8_t* c = &im.p[(size_t(py) * im.w + px) * 4];
    float a = c[3] / 255.0f;
    out[0] += weight * c[0] * a, out[1] += weight * c[1] * a, out[2] += weight * c[2] * a;
    out[3] += weight * a;
  };
  add(x0, y0, (1 - fx) * (1 - fy)), add(x1, y0, fx * (1 - fy));
  add(x0, y1, (1 - fx) * fy), add(x1, y1, fx * fy);
  return out;
}

// The frame's source x for a destination one (both in source pixels, pixel
// centres), the frame destSize wide: the ends as they are, the middle run
// there and back `runs` times (odd, so both ends join).
float Across(float d, float destSize, int runs) {
  constexpr float kEnd = float(kFrameEnd), kMiddle = float(kFrameW - 2 * kFrameEnd);
  if (d < kEnd) {
    return d;
  }
  if (d >= destSize - kEnd) {
    return float(kFrameW) - (destSize - d);
  }
  float t = (d - kEnd) / (destSize - 2 * kEnd) * float(runs);
  int run = std::min(int(t), runs - 1);
  float f = t - float(run);
  return kEnd + (run % 2 ? 1.0f - f : f) * kMiddle;
}

// The frame's and panel's pictures, read once (SetUp: at the start, not at
// the first unlock); false if they aren't in the archives.
Rgba g_framePicture{kFrameW, kFrameH, {}}, g_panelPicture{kPanelW, kPanelH, {}};
bool g_boxPictures = false;
void ReadBoxPictures() {
  g_boxPictures =
      !g_gameRoot.empty() &&
      ng2::game_icon::PackPicture(g_gameRoot, kFrameFile, "t_0x790014", kFrameW, kFrameH, &g_framePicture.p) &&
      ng2::game_icon::PackPicture(g_gameRoot, kPanelFile, "t_0x7c000c", kPanelW, kPanelH, &g_panelPicture.p);
  if (!g_boxPictures) {
    REXLOG_WARN("achievements: the game's frame pictures aren't in its archives: no box behind the toast");
  }
}

// The Achievements page's scroll bar, from the same pictures: its track a
// piece of the dark panel, its thumb a piece of the frame's left border (a
// plain stretch of chrome).
uint32_t Crop(uint32_t key, const Rgba& from, int x0, int y0, int w, int h) {
  if (!g_boxPictures) {
    return 0;
  }
  if (!ng2::video::HasUiImage(key)) {
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
      std::memcpy(&rgba[size_t(y) * w * 4], &from.p[(size_t(y0 + y) * from.w + x0) * 4], size_t(w) * 4);
    }
    ng2::video::SetUiImagePixels(key, uint32_t(w), uint32_t(h), std::move(rgba));
  }
  return key;
}

// The box as a UI image width x height (UI pixels); 0 if the game's
// pictures aren't found.
uint32_t BoxImage(int width, int height) {
  uint32_t key = kBoxKeyBase + uint32_t(width);
  if (!g_boxPictures) {
    return 0;
  }
  if (ng2::video::HasUiImage(key)) {
    return key;
  }
  auto start = std::chrono::steady_clock::now();
  const Rgba& frame = g_framePicture;
  const Rgba& panel = g_panelPicture;
  const int w = width * 2, h = height * 2;
  constexpr float kPanelAlpha = 0.8f;
  // source pixels per pixel of the picture: the whole frame's height is the box's
  const float scale = float(kFrameH) / float(h), across = float(w) * scale;
  // as many runs of the middle as leave it nearest its own size
  const float middles = (across - 2 * kFrameEnd) / float(kFrameW - 2 * kFrameEnd);
  const int runs = std::max(1, int(std::lround((middles - 1.0f) / 2.0f)) * 2 + 1);
  const float panelLeft = kBorderLeft / scale, panelRight = float(w) - (float(kFrameW) - kBorderRight) / scale;
  const float panelTop = kBorderTop / scale, panelBottom = kBorderBottom / scale;
  std::vector<uint8_t> rgba(size_t(w) * h * 4);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
      // the panel's body (without its drop shadow, right and below), stretched inside the border
      std::array<float, 4> under{};
      if (cx >= panelLeft && cx < panelRight && cy >= panelTop && cy < panelBottom) {
        under = Sample(panel, 2.0f + (cx - panelLeft) / (panelRight - panelLeft) * float(kPanelW - 12),
                       2.0f + (cy - panelTop) / (panelBottom - panelTop) * float(kPanelH - 12));
        for (float& c : under) {
          c *= kPanelAlpha;
        }
      }
      // the frame over it
      std::array<float, 4> over =
          Sample(frame, Across(cx * scale, across, runs) - 0.5f, cy * scale - 0.5f);
      uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
      float a = over[3] + under[3] * (1 - over[3]);
      for (int k = 0; k < 3; k++) {
        float c = over[size_t(k)] + under[size_t(k)] * (1 - over[3]);
        o[k] = uint8_t(std::lround(std::clamp(a > 0 ? c / a : 0.0f, 0.0f, 255.0f)));
      }
      o[3] = uint8_t(std::lround(std::clamp(a * 255.0f, 0.0f, 255.0f)));
    }
  }
  ng2::video::SetUiImagePixels(key, uint32_t(w), uint32_t(h), std::move(rgba));
  REXLOG_INFO("achievements: the toast's box made ({}x{}, {:.0f} ms)", width, height,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  return key;
}

// The toast: unlocks waiting, and the one shown.
struct Toast {
  std::u16string name;
  uint32_t icon = 0;
  AchievementInfo info;  // (its icon, when icon is 0)
};
std::mutex g_mutex;
std::deque<Toast> g_waiting;  // (g_mutex)
std::vector<ng2::achievements::Icon> g_pageIcons;  // (g_mutex)
uint64_t g_pageIconsSet = 0;  // the frame they were set in (g_mutex)
std::atomic<uint64_t> g_frame{0};  // frames counted by Frame (read by the menus)
Toast g_shown;
std::chrono::steady_clock::time_point g_shownSince;
bool g_showing = false;

constexpr double kFadeIn = 0.2, kHold = 3.0, kFadeOut = 0.3;


// The widest name in the toast's font: every toast's box as wide (one size).
float g_widestName = 0.0f;
constexpr int kToastTextSize = 0x14;

bool g_setUp = false;
void SetUp() {
  auto* kernel = Kernel();
  if (!kernel || kernel->achievements().ListAchievements().empty()) {
    return;
  }
  g_setUp = true;
  ReadBoxPictures();
  for (const AchievementInfo& info : kernel->achievements().ListAchievements()) {
    g_widestName = std::max(g_widestName, ng2::panel::TextWidth(Utf16(info.label), kToastTextSize));
  }
  // The icons and the padlock made in the background now, so neither the
  // first toast nor the Achievements page waits for them (~40 ms).
  std::thread([] {
    if (auto* kernel = Kernel()) {
      for (const AchievementInfo& info : kernel->achievements().ListAchievements()) {
        IconOf(info);
      }
      LockIcon();
    }
  }).detach();
  auto& manager = kernel->achievements();
  // None while cheats are unlocked (the cheats' question says so).
  manager.SetUnlockFilter([](uint32_t) { return !ng2::recomp_settings::CheatsUnlocked(); });
  manager.RegisterNotificationCallback([](const rex::system::AchievementEvent& event) {
    if (!REXCVAR_GET(achievement_notifications)) {
      return;
    }
    std::lock_guard lock(g_mutex);
    g_waiting.push_back({Utf16(event.achievement.label), 0, event.achievement});
    REXLOG_INFO("achievements: {:08X} unlocked, shown in the corner", event.achievement.id);
  });
  REXLOG_INFO("achievements: {} in the game's list", manager.ListAchievements().size());
}

}  // namespace

namespace ng2::achievements {

std::vector<Entry> List() {
  // (once a frame: the page asks several times)
  static std::mutex cacheMutex;
  static std::vector<Entry> cache;
  static uint64_t cacheFrame = ~0ull;
  std::lock_guard cacheLock(cacheMutex);
  if (cacheFrame == g_frame) {
    return cache;
  }
  std::vector<Entry> entries;
  auto* kernel = Kernel();
  if (!kernel) {
    return entries;
  }
  auto& manager = kernel->achievements();
  uint32_t hidden = 0;
  for (const AchievementInfo& info : manager.ListAchievements()) {
    Entry entry;
    entry.id = info.id;
    entry.unlocked = manager.IsUnlocked(info.id);
    entry.unlockedAt = entry.unlocked ? manager.GetUnlockTime(info.id) : 0;
    if (!(info.flags & kShownLocked) && !entry.unlocked) {
      hidden++;  // (a locked secret one: counted in the last row, as Steam's)
      continue;
    }
    entry.name = Utf16(info.label);
    entry.description = entry.unlocked || info.unachieved_description.empty() ? Utf16(info.description)
                                                                              : Utf16(info.unachieved_description);
    entry.icon = IconOf(info);
    entries.push_back(std::move(entry));
  }
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.unlocked != b.unlocked) {
      return a.unlocked;
    }
    return a.unlocked && a.unlockedAt > b.unlockedAt;
  });
  if (hidden) {
    Entry entry;
    std::string name = fmt::format("{} Hidden Achievement{}", hidden, hidden == 1 ? "" : "s");
    entry.name.assign(name.begin(), name.end());
    entry.description = hidden == 1 ? u"Keep playing to unlock it." : u"Keep playing to unlock them.";
    entry.icon = LockIcon();
    entries.push_back(std::move(entry));
  }
  cache = entries;
  cacheFrame = g_frame;
  return entries;
}

uint32_t Total() {
  auto* kernel = Kernel();
  return kernel ? uint32_t(kernel->achievements().ListAchievements().size()) : 0;
}

uint32_t UnlockedCount() {
  uint32_t count = 0;
  for (const Entry& entry : List()) {
    count += entry.unlocked;
  }
  return count;
}

std::u16string DateText(uint64_t filetime) {
  if (!filetime) {
    return {};
  }
  // FILETIME: 100 ns since 1601.
  constexpr uint64_t kUnixEpoch = 116444736000000000ull;
  std::time_t seconds = std::time_t((filetime - std::min(filetime, kUnixEpoch)) / 10000000ull);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &seconds);
#else
  localtime_r(&seconds, &local);
#endif
  std::string text = fmt::format("{:04}/{:02}/{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1,
                                 local.tm_mday, local.tm_hour, local.tm_min);
  return std::u16string(text.begin(), text.end());
}

std::atomic<uint32_t> g_previews{0};  // F6 presses waiting (PreviewToast)

uint32_t ScrollTrack() {
  return Crop(0x53430001, g_panelPicture, 24, 12, 16, 48);  // "SC"
}
uint32_t ScrollThumb() {
  return Crop(0x53430002, g_framePicture, 9, 110, 24, 120);
}

std::atomic<uint32_t> g_testUnlocks{0};  // F5 presses waiting (TestUnlockNext)

void TestUnlockNext() {
  static const bool on = [] {
    const char* v = std::getenv("NG2_TEST_UNLOCK_KEY");
    return v && *v == '1';
  }();
  if (on) {
    g_testUnlocks.fetch_add(1, std::memory_order_relaxed);
  }
}

void PreviewToast() {
  static const bool on = [] {
    const char* v = std::getenv("NG2_TEST_TOAST");
    return v && *v == '1';
  }();
  if (on) {
    g_previews.fetch_add(1, std::memory_order_relaxed);
  }
}

void SetGameRoot(const std::filesystem::path& root) {
  g_gameRoot = root;
}

void SetPageIcons(std::vector<Icon> icons) {
  std::lock_guard lock(g_mutex);
  g_pageIcons = std::move(icons);
  g_pageIconsSet = g_frame;
}

bool Frame() {
  bool started = false;
  g_frame++;
  if (!g_setUp) {
    SetUp();
  }
  // NG2_TEST_UNLOCK=<id>[,<id>...]: those unlocked (as the game does, with
  // its notification) about 10 s after the list is there (or
  // NG2_TEST_UNLOCK_DELAY seconds), one a second.
  static const char* testUnlock = std::getenv("NG2_TEST_UNLOCK");
  static uint64_t testFrom = 0;
  if (testUnlock && g_setUp) {
    if (!testFrom) {
      // (NG2_TEST_UNLOCK_DELAY=<seconds>: later than 10 s)
      const char* delay = std::getenv("NG2_TEST_UNLOCK_DELAY");
      testFrom = g_frame + (delay ? uint64_t(std::strtoul(delay, nullptr, 10)) * 60 : 600);
    }
    if (g_frame >= testFrom && (g_frame - testFrom) % 60 == 0) {
      std::string_view ids(testUnlock);
      size_t index = size_t((g_frame - testFrom) / 60);
      for (size_t k = 0; k < index && !ids.empty(); k++) {
        size_t comma = ids.find(',');
        ids = comma == std::string_view::npos ? std::string_view() : ids.substr(comma + 1);
      }
      if (!ids.empty()) {
        uint32_t id = uint32_t(std::strtoul(std::string(ids.substr(0, ids.find(','))).c_str(), nullptr, 10));
        auto result = Kernel()->achievements().UnlockAchievement(
            id, rex::system::AchievementNotification::kShow);
        REXLOG_INFO("achievements: test unlock {}: {}", id, int(result));
      }
    }
  }
  if (g_setUp && g_testUnlocks.load(std::memory_order_relaxed)) {
    g_testUnlocks.store(0, std::memory_order_relaxed);
    // (the hidden ones first: they're last in the game's order)
    auto& manager = Kernel()->achievements();
    std::vector<AchievementInfo> all = manager.ListAchievements();
    std::stable_partition(all.begin(), all.end(),
                          [](const AchievementInfo& info) { return !(info.flags & kShownLocked); });
    for (const AchievementInfo& info : all) {
      if (!manager.IsUnlocked(info.id)) {
        auto result = manager.UnlockAchievement(info.id, rex::system::AchievementNotification::kShow);
        REXLOG_INFO("achievements: F5 unlock {}: {}", info.id, int(result));
        break;
      }
    }
  }
  if (g_setUp && g_previews.load(std::memory_order_relaxed)) {
    // (each achievement in the game's order, as its unlock shows it: a
    // secret one's real name and icon too)
    g_previews.store(0, std::memory_order_relaxed);
    static size_t next = 0;
    std::vector<AchievementInfo> all = Kernel()->achievements().ListAchievements();
    if (!all.empty()) {
      const AchievementInfo& info = all[next++ % all.size()];
      std::lock_guard lock(g_mutex);
      g_waiting.push_back({Utf16(info.label), 0, info});
      REXLOG_INFO("achievements: toast preview: {:08X}", info.id);
    }
  }
  std::vector<ng2::video::UiImage> images;
  {
    std::lock_guard lock(g_mutex);
    if (g_pageIconsSet + 2 >= g_frame) {
      for (const Icon& icon : g_pageIcons) {
        images.push_back({icon.key, icon.x, icon.y, icon.size, icon.height > 0.0f ? icon.height : icon.size,
                          icon.alpha, icon.grey});
      }
    }
    if (!g_showing && !g_waiting.empty()) {
      g_shown = std::move(g_waiting.front());
      g_waiting.pop_front();
      g_showing = true;
      g_shownSince = std::chrono::steady_clock::now();
      started = true;
    }
  }
  if (g_showing) {
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_shownSince).count();
    if (t >= kFadeIn + kHold + kFadeOut) {
      g_showing = false;
      ng2::overlay::SetToast({});  // (its texts go with its box, even after a long frame)
    } else {
      float alpha = float(t < kFadeIn ? t / kFadeIn : t > kFadeIn + kHold ? 1.0 - (t - kFadeIn - kHold) / kFadeOut
                                                                            : 1.0);
      if (!g_shown.icon && g_shown.info.image_id) {
        g_shown.icon = IconOf(g_shown.info);  // (its own only: decoding the list's took ~40 ms)
      }
      // In the bottom right corner (Steam's), in the game's frame: the icon,
      // then "Achievement Unlocked" in yellow over the name (Unleashed
      // Recompiled's). The box the same for all: as wide as the widest name
      // needs; the icon and the texts in the frame's opening with the same
      // margin all round.
      constexpr float kEdge = 24.0f, kBoxHeight = 80.0f, kIconSize = 40.0f, kGap = 8.0f;
      constexpr int kSize = kToastTextSize;
      constexpr float kBoxTop = 720.0f - kEdge - kBoxHeight;
      constexpr float kFrameScale = float(kFrameH) / kBoxHeight;  // source pixels per UI pixel
      constexpr float kOpenL = kOpenLeft / kFrameScale, kOpenR = (float(kFrameW) - kOpenRight) / kFrameScale;
      constexpr float kOpenT = kOpenTop / kFrameScale, kOpenB = (float(kFrameH) - kOpenBottom) / kFrameScale;
      constexpr float kMargin = (kBoxHeight - kOpenT - kOpenB - kIconSize) / 2;
      float textWidth = std::max(ng2::panel::TextWidth(u"Achievement Unlocked", kSize), g_widestName);
      float content = kOpenL + kMargin + kIconSize + kGap + textWidth + kMargin + kOpenR;
      int boxWidth = (int(std::ceil(content)) + 15) & ~15;
      float left = 1280.0f - kEdge - float(boxWidth), top = kBoxTop + kOpenT + kMargin;
      float iconX = left + kOpenL + kMargin + (float(boxWidth) - content) / 2, textX = iconX + kIconSize + kGap;
      if (uint32_t box = BoxImage(boxWidth, int(kBoxHeight))) {
        ng2::video::UiImage image{box, left, kBoxTop, float(boxWidth), kBoxHeight, alpha, false};
        image.under = true;
        images.push_back(image);
      }
      if (g_shown.icon) {
        images.push_back({g_shown.icon, iconX, top, kIconSize, kIconSize, alpha, false});
      }
      // (the lines' centres a quarter and three quarters down the icon)
      std::vector<ng2::overlay::Text> texts;
      texts.push_back({textX, top + kIconSize * 0.25f, u"Achievement Unlocked", 1.0f, kSize, 0xFCF305, alpha});
      texts.push_back({textX, top + kIconSize * 0.75f, g_shown.name, 1.0f, kSize, 0, alpha});
      ng2::overlay::SetToast(std::move(texts));
    }
  }
  ng2::video::SetUiImages(std::move(images));
  return started;
}

}  // namespace ng2::achievements
