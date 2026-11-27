// ng2 - Ninja Gaiden II static recompilation
//
// RECOMP SETTINGS: the title menu's Xbox LIVE Marketplace entry opens the
// game's own Options screen as a screen of the recomp's settings
// (docs/menus.md). While it is open:
//
// - The category list holds our categories (their names written over the
//   game's in the loaded string table, the rest blanked) and is run by us:
//   Confirm opens a category's page, Back leaves Options.
// - Each category is one of the game's pages, shown empty: its rows are
//   drawn by our panel (settings_panel.h) with the game's arrow and cursor
//   pictures copied in, run by us with the game's input and sounds. Cheats
//   asks first (on the title only), Achievements lists the game's.
// - In the in-game menu the same panel is drawn on the list's empty rows.
//
// Everything is restored and the settings are saved when Options closes.
// The title menu also loses LEADERBOARDS and gains EXIT GAME here.

#include <algorithm>
#include <atomic>
#include <functional>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/ui/windowed_app_context.h>

#include <SDL3/SDL.h>

#include "gpu/guest.h"
#include "gpu/menu_label.h"
#include "recomp_settings.h"
#include "overlay.h"
#include "achievements.h"
#include "settings_panel.h"
#include "gpu/prompts.h"
#include "rebind.h"
#include "menu_mouse.h"

#include "aspect.h"

REXCVAR_DEFINE_INT32(windowed_width, 0, "UI/Window",
                     "The window's width when not fullscreen (0: the default size)");
REXCVAR_DEFINE_INT32(windowed_height, 0, "UI/Window",
                     "The window's height when not fullscreen (0: the default size)");
// (kNoPosition: none saved; the window is placed by the system)
REXCVAR_DEFINE_INT32(windowed_x, -1000000, "UI/Window",
                     "The window's left edge when not fullscreen (saved when it's moved)");
REXCVAR_DEFINE_INT32(windowed_y, -1000000, "UI/Window",
                     "The window's top edge when not fullscreen (saved when it's moved)");
REXCVAR_DEFINE_BOOL(cheats_unlocked, false, "Cheats",
                    "The Cheats asked for once in RECOMP SETTINGS (achievements are off while they are)");
REXCVAR_DEFINE_BOOL(exclusive_fullscreen, false, "UI/Window",
                    "Fullscreen in the desktop's display mode (exclusive) rather than as a "
                    "borderless window over the desktop");

namespace ng2 {
bool Base720p();  // base_resolution.cpp
}  // namespace ng2

namespace {
// (defined below, with the hooks)
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*),
                   std::initializer_list<uint32_t> args, double f1 = 0.0);
}  // namespace

namespace ng2::recomp_settings {
namespace {

// The title menu opens screens with sub_8366FAC8(manager, id): 13 is the
// "sign in to see the Marketplace" message, 9 the Options screen.
constexpr uint32_t kMarketplaceMessage = 13;
constexpr uint32_t kOptionsScreen = 9;

// Options state (sub_823E59A0): the page (0 the category list, 1 Game
// Settings, past 11 closes Options), the page before this frame's, the
// depth, and a cursor per page.
constexpr uint32_t kPage = 0x83D017B0;
constexpr uint32_t kLastPage = 0x83D017B4;
constexpr uint32_t kDepth = 0x83D017B8;
constexpr uint32_t kCursors = 0x83D017CC;
constexpr uint32_t kCategoryPage = 0;
constexpr uint32_t kClosed = 0xFF;  // what the category list's Back sets
// The pad state, the player's pad, and the menu model.
constexpr uint32_t kPadState = 0x84F5BBD8;
constexpr uint32_t kPlayerPad = 0x84C3DA3E;
constexpr uint32_t kMenuModel = 0x8532EDB0;
constexpr uint32_t kCursorSound = 0x8206EE68;  // the float the sounds take
constexpr uint32_t kEnter = 6;
constexpr uint32_t kLeave = 7;

// An arrow list (made once: its arrows are the title panel's pictures).
constexpr uint32_t kListVtable = 0x822869B0;
constexpr uint32_t kListSize = 0x270;
constexpr uint32_t kPlayTimeText = 0x859F5574;  // UTF-16, from sub_836D0E90
constexpr uint32_t kPageRows = 0x10;           // a page object's row count
constexpr uint32_t kMaxPageRows = 32;  // a page's settings

// Options strings: table 2 (id 0x20000 + index), strings back to back in
// 16-byte slots.
constexpr uint32_t kTable = 0x20000;
// The categories' names, by category: ours first, the others blanked.
constexpr uint32_t kCategoryNames[] = {0x00, 0x01, 0x97, 0x03, 0x02, 0x04, 0x05, 0x61};
constexpr uint32_t kPlayTimeName = 0x2F;
// Notes under the panel (NoteOf).
constexpr std::u16string_view kAllWeaponsNote = u"Weapons given stay in your saves, even turned off.";
constexpr std::u16string_view kInterpNote = u"Experimental: pictures between the game's frames.";
// The Options table's ids that can give our texts (g_overrides).
constexpr uint32_t kSlotCount = 0x380;

enum class Kind {
  kScale, kFiltering, kLevel, kOnOff, kPresent, kWindowMode, kWindowSize, kSamples, kAspect,
  kSensitivity
};
// Mouse Sensitivity's choices (mnk_sensitivity).
constexpr double kSensitivities[] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0};
// Render Scale's choices (render_scale).
constexpr double kScales[] = {1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
struct Setting {
  std::u16string_view label;
  std::array<std::u16string_view, 10> choices;
  uint32_t count;
  const char* name;
  Kind kind;
};
// Window Size's choices, for each aspect ratio (Aspect Ratio's order).
struct WindowSize {
  int32_t width, height;
  std::u16string_view text;
};
constexpr uint32_t kWindowSizeCount = 5;
constexpr WindowSize kWindowSizes[][kWindowSizeCount] = {
    {{1280, 720, u"1280x720"}, {1600, 900, u"1600x900"}, {1920, 1080, u"1920x1080"},
     {2560, 1440, u"2560x1440"}, {3840, 2160, u"3840x2160"}},
    {{1280, 800, u"1280x800"}, {1440, 900, u"1440x900"}, {1680, 1050, u"1680x1050"},
     {1920, 1200, u"1920x1200"}, {2560, 1600, u"2560x1600"}},
    {{1720, 720, u"1720x720"}, {2560, 1080, u"2560x1080"}, {3440, 1440, u"3440x1440"},
     {3840, 1600, u"3840x1600"}, {5120, 2160, u"5120x2160"}},
    {{2560, 720, u"2560x720"}, {3840, 1080, u"3840x1080"}, {5120, 1440, u"5120x1440"},
     {6880, 1440, u"6880x1440"}, {7680, 2160, u"7680x2160"}},
    {{960, 720, u"960x720"}, {1024, 768, u"1024x768"}, {1440, 1080, u"1440x1080"},
     {1600, 1200, u"1600x1200"}, {2048, 1536, u"2048x1536"}},
};

constexpr Setting kSettings[] = {
    // With what the game's 3D (1120x584) is drawn at, as the NGB recomp's
    // (1280x720 with base_resolution_720p: Choice).
    {u"Render Scale",
     {u"1x 1120x584", u"1.5x 1680x876", u"2x 2240x1168", u"3x 3360x1752", u"4x 4480x2336",
      u"5x 5600x2920", u"6x 6720x3504", u"7x 7840x4088", u"8x 8960x4672"},
     9, "render_scale", Kind::kScale},
    {u"Texture Filtering", {u"Default", u"4x", u"8x", u"16x"}, 4, "anisotropic_filtering",
     Kind::kFiltering},
    {u"Bloom", {u"Off", u"Reduced", u"On"}, 3, "bloom", Kind::kLevel},
    {u"Anti-Aliasing", {u"Off", u"2x MSAA", u"4x MSAA", u"8x MSAA"}, 4, "antialiasing",
     Kind::kSamples},
    {u"Window Mode", {u"Windowed", u"Borderless", u"Fullscreen"}, 3, "fullscreen",
     Kind::kWindowMode},
    // Its choices follow the aspect ratio (kWindowSizes).
    {u"Window Size", {}, kWindowSizeCount, "windowed_width", Kind::kWindowSize},
    {u"V-Sync", {u"Off", u"FIFO", u"Mailbox", u"Adaptive"}, 4, "present_mode", Kind::kPresent},
    {u"Show FPS", {u"On", u"Off"}, 2, "show_fps", Kind::kOnOff},
    {u"Infinite Health", {u"On", u"Off"}, 2, "cheat_infinite_health", Kind::kOnOff},
    {u"Infinite Ninpo", {u"On", u"Off"}, 2, "cheat_infinite_ninpo", Kind::kOnOff},
    {u"Unlock Difficulties", {u"On", u"Off"}, 2, "cheat_unlock_difficulties", Kind::kOnOff},
    {u"Unlock Costumes", {u"On", u"Off"}, 2, "cheat_unlock_costumes", Kind::kOnOff},
    {u"Skip Intro", {u"On", u"Off"}, 2, "skip_intro", Kind::kOnOff},
    {u"Skip Cutscenes", {u"On", u"Off"}, 2, "skip_cutscenes", Kind::kOnOff},
    {u"Aspect Ratio", {u"16:9", u"16:10", u"21:9", u"32:9", u"4:3"}, 5, "aspect_ratio", Kind::kAspect},
    // The prompts follow the device in use (gpu/prompts.cpp).
    {u"Button Prompts", {u"Auto", u"Xbox", u"PlayStation", u"Nintendo", u"Keyboard"}, 5,
     "button_prompts", Kind::kLevel},
    {u"Mouse Sensitivity", {u"0.25", u"0.5", u"0.75", u"1", u"1.5", u"2", u"3", u"4", u"6", u"8"}, 10, "mnk_sensitivity",
     Kind::kSensitivity},
    // Extras (extras.cpp); Texture LOD Bias is the renderer's, on Graphics.
    {u"Skip Death Screen", {u"On", u"Off"}, 2, "skip_death_screen", Kind::kOnOff},
    {u"Auto Chapter Screen", {u"On", u"Off"}, 2, "auto_chapter_screen", Kind::kOnOff},
    {u"HUD", {u"Shown", u"Fade Out", u"Hidden"}, 3, "hud_mode", Kind::kLevel},
    {u"Post-Effects", {u"On", u"Off"}, 2, "post_effects", Kind::kOnOff},
    {u"Texture LOD Bias", {u"Game", u"0.0", u"-1.0"}, 3, "texture_lod_bias", Kind::kLevel},
    {u"Smooth HUD and Menus", {u"On", u"Off"}, 2, "smooth_hud", Kind::kOnOff},
    {u"Hi-Res Font", {u"On", u"Off"}, 2, "hires_font", Kind::kOnOff},
    {u"All Weapons and Ninpo", {u"Off", u"Level 1", u"Level 3"}, 3, "cheat_all_weapons",
     Kind::kLevel},
    {u"All Crystal Skulls", {u"On", u"Off"}, 2, "cheat_all_skulls", Kind::kOnOff},
    {u"Max HP and Ninpo", {u"On", u"Off"}, 2, "cheat_max_health_ninpo", Kind::kOnOff},
    // The renderer's wait for the screen (gpu/video.cpp).
    {u"Input Latency", {u"Standard", u"Low"}, 2, "input_latency", Kind::kLevel},
    // The guest's vblanks on the display's refreshes (gpu/native_gpu.cpp).
    {u"Frame Pacing", {u"Auto", u"Timer"}, 2, "frame_pacing", Kind::kLevel},
    // Pictures made between the game's frames: one each refresh of the
    // display (Full Rate) or each second one (Half Rate; each one at 120 Hz
    // and below, where a second one would leave none between), or one
    // between each two of the game's frames (2x: 120 for its 60);
    // gpu/interp.cpp. Nothing on a 60 Hz display but where the game itself
    // runs at 30 or less.
    // (Short: a longer choice is squeezed narrow to fit.)
    {u"Frame Interpolation", {u"Off", u"Full Rate", u"Half Rate", u"2x"}, 4,
     "frame_interpolation", Kind::kLevel},
    // The 1024x1024 shadow map in fights too (extras.cpp).
    {u"Shadows", {u"Original", u"Hi-Res"}, 2, "shadows", Kind::kLevel},
    // Master Edition's camera (camera.cpp); camera control in OT and UT.
    {u"Modern Camera", {u"On", u"Off"}, 2, "modern_camera", Kind::kOnOff},
    {u"OT/UT Free Camera", {u"On", u"Off"}, 2, "free_camera_ot_ut", Kind::kOnOff},
    // Normal maps filtered between their mips (gpu/video.cpp).
    {u"Normal Map Filtering", {u"Trilinear", u"Game"}, 2, "smooth_normal_maps", Kind::kOnOff},
    {u"One Hit Kill", {u"On", u"Off"}, 2, "cheat_one_hit_kill", Kind::kOnOff},
    {u"Instant Charge", {u"On", u"Off"}, 2, "cheat_instant_charge", Kind::kOnOff},
    // Extras (extras.cpp).
    {u"More Blood", {u"On", u"Off"}, 2, "more_blood", Kind::kOnOff},
    // The size the game's 3D is drawn at before Render Scale (experimental,
    // base_resolution.cpp; after a restart).
    {u"Base Resolution", {u"1280x720", u"1120x584"}, 2, "base_resolution_720p", Kind::kOnOff},
    // An unlock shown in the bottom right corner (achievements.cpp).
    {u"Achievement Notifications", {u"On", u"Off"}, 2, "achievement_notifications", Kind::kOnOff},
};
// The window sizes for the chosen aspect ratio.
const WindowSize* WindowSizes() {
  return kWindowSizes[ng2::aspect::AspectIndex()];
}

// A setting's k-th choice as shown.
std::u16string_view Choice(const Setting& setting, uint32_t k) {
  if (setting.kind == Kind::kWindowSize) {
    return WindowSizes()[std::min(k, kWindowSizeCount - 1)].text;
  }
  // Base Resolution applies at the next start: the choice that isn't the one
  // running says so.
  if (std::string_view(setting.name) == "base_resolution_720p") {
    static constexpr std::u16string_view kNow[] = {u"1280x720", u"1120x584"};
    static constexpr std::u16string_view kLater[] = {u"1280x720 (Restart)", u"1120x584 (Restart)"};
    uint32_t running = ng2::Base720p() ? 0 : 1;
    k = std::min<uint32_t>(k, 1);
    return k == running ? kNow[k] : kLater[k];
  }
  // (base_resolution_720p: the game's 3D at 1280x720)
  if (setting.kind == Kind::kScale && ng2::Base720p()) {
    static constexpr std::u16string_view k720p[] = {
        u"1x 1280x720",  u"1.5x 1920x1080", u"2x 2560x1440", u"3x 3840x2160", u"4x 5120x2880",
        u"5x 6400x3600", u"6x 7680x4320",   u"7x 8960x5040", u"8x 10240x5760"};
    return k720p[std::min<uint32_t>(k, 8)];
  }
  return setting.choices[std::min(k, setting.count - 1)];
}

struct PageRow {
  uint32_t setting;
};
struct Page {
  std::u16string_view name;
  uint32_t page;   // its Options page (category + 1)
  uint32_t size;   // its page object's
  std::array<PageRow, kMaxPageRows> rows;
  uint32_t count;
  // The page the screen shows (its builder's, the menu model's page, its
  // cursor and values), when not the one the Options loop runs (page): 0, the
  // same.
  uint32_t view = 0;
  // Its rows are a table of ours (the Controls page), none of the game's.
  bool custom = false;
  // The achievements' list (achievements.h), none of the game's either.
  bool achievements = false;
};
constexpr uint32_t ViewOf(const Page& page) {
  return page.view ? page.view : page.page;
}
// Every row is an arrow list made in our own memory (g_titleRows, one per
// setting), so a page's own layout doesn't limit what it holds: the game's
// page objects give their builder, their Restore Defaults row and their
// slot in the menu model.
// (and a Restore Defaults row after them, for a page without one)
constexpr PageRow Row(uint32_t setting) {
  return {setting};
}
constexpr Page kPages[] = {
    {u"Graphics", 1, 0x2BB0,
     {{Row(0), Row(37), Row(3), Row(1), Row(21), Row(33), Row(30), Row(2), Row(20)}},
     9},
    {u"Display", 2, 0x1CC0, {{Row(4), Row(5), Row(14), Row(6), Row(28), Row(29)}}, 6},
    {u"Interface", 4, 0x26A0,
     {{Row(19), Row(23), Row(22), Row(7), Row(15), Row(16), Row(38)}},
     7},
    // The Audio Settings page (5, builder sub_836D54F8, which stores its
    // page number at +12; Restore Defaults at +0x1380).
    {u"Extras", 5, 0x1460, {{Row(12), Row(13), Row(17), Row(18), Row(10), Row(36)}}, 6},
    // Shown on the Copyright Information page (6: what the category list
    // shows for its sixth category; builder sub_836D57B8), run as page 11
    // (the Options loop runs page 6 inline, 11 through sub_823E7C40); no
    // Restore Defaults of its own: ours.
    {u"Gameplay", 11, 0x400, {{Row(27), Row(31), Row(32)}}, 3, 6},
    // On the Options screen's page object for Return to Main Menu (8), an
    // empty page it makes and never shows (ShowControlsPage), run as page 7
    // (Restore Default's: its handler sub_823E82E0); its Restore Defaults is
    // ours.
    {u"Controls", 7, 0x1E0, {}, 0, 8, true},
    // On the same empty page as Controls (the title's routes go by category).
    {u"Achievements", 7, 0x1E0, {}, 0, 8, false, true},
    // Last: on the title its question until unlocked, in game only once unlocked.
    {u"Cheats", 3, 0x13E0,
     {{Row(8), Row(9), Row(34), Row(35), Row(11), Row(24), Row(25), Row(26)}},
     8},
};
constexpr uint32_t kPageCount = std::size(kPages);
constexpr std::u16string_view kRestoreText = u"Restore Defaults";

// The page the screen shows for one the Options loop runs.
uint32_t ShownPage(uint32_t page) {
  for (const Page& p : kPages) {
    if (p.page == page) {
      return ViewOf(p);
    }
  }
  return page;
}

const Page* PageOf(uint32_t page) {
  for (const Page& p : kPages) {
    if (p.page == page) {
      return &p;
    }
  }
  return nullptr;
}

// The page the title's Options loop runs: by the category picked, as two
// share Controls' empty page.
uint32_t g_titleCategory = 0;
const Page* TitlePage() {
  uint32_t page = ng2::guest::Load32(kPage);
  if (g_titleCategory < std::size(kPages) && kPages[g_titleCategory].page == page) {
    return &kPages[g_titleCategory];
  }
  return PageOf(page);
}

bool g_open = false;
// The settings changed while open, as set (fullscreen is applied later, on
// the UI thread): saved on closing.
std::map<std::string, std::string, std::less<>> g_written;
bool g_inGameOurs = false;  // our settings are in the in-game menu's list
bool g_unsaved = false;     // in game: a change made this press, saved at its end
std::filesystem::path g_configPath;
rex::ui::WindowedAppContext* g_appContext = nullptr;
// While ours is shown: the Options table's ids that give our texts instead of
// the game's (the text lookup's hook asks, RecompText), by slot; 0: the game's.
std::array<std::atomic<uint32_t>, kSlotCount> g_overrides{};

// The page object being built by the game's builder, whose rows are left out.
uint32_t g_building = 0;
uint32_t g_buildingSize = 0;

void StoreBE32(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24), p[1] = uint8_t(value >> 16), p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}
void Store32(uint32_t address, uint32_t value) {
  StoreBE32(guest::Virtual(address), value);
}
float LoadFloat(uint32_t address) {
  return guest::LoadFloat(address);
}
void StoreFloat(uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Store32(address, bits);
}

std::vector<uint8_t> Utf16BE(std::u16string_view text) {
  std::vector<uint8_t> bytes;
  for (char16_t c : text) {
    bytes.push_back(uint8_t(c >> 8));
    bytes.push_back(uint8_t(c));
  }
  return bytes;
}

// One of our texts in guest memory (UTF-16 BE), made once and kept.
uint32_t GuestText(std::u16string_view text) {
  static std::map<std::u16string, uint32_t, std::less<>> made;
  if (auto it = made.find(text); it != made.end()) {
    return it->second;
  }
  std::vector<uint8_t> bytes = Utf16BE(text);
  bytes.push_back(0), bytes.push_back(0);
  uint32_t address = rex::Runtime::instance()->memory()->SystemHeapAlloc(uint32_t(bytes.size()));
  if (address) {
    std::memcpy(guest::Virtual(address), bytes.data(), bytes.size());
  }
  made.emplace(std::u16string(text), address);
  return address;
}

// Our texts given for an Options table id: the game's menus ask the text
// lookup by id, in the game's language, so this works in every language and
// with no room needed in the table.
void Override(uint32_t slot, std::u16string_view text) {
  if (slot < kSlotCount) {
    g_overrides[slot].store(GuestText(text), std::memory_order_release);  // (its bytes first)
  }
}



void PutBackGame();

void Open() {
  if (g_open) {
    return;
  }
  g_open = true;
  PutBackGame();
  ng2::video::SetRecompHeader(true);
  for (uint32_t i = 0; i < std::size(kCategoryNames); i++) {
    Override(kCategoryNames[i], i < kPageCount ? kPages[i].name : u"");
  }
  Override(kPlayTimeName, u"");
  REXLOG_INFO("recomp settings: open");
}

// A "name = value" line (leading spaces already gone).
bool IsKeyLine(std::string_view line, std::string_view name) {
  if (line.substr(0, name.size()) != name) {
    return false;
  }
  line.remove_prefix(name.size());
  size_t next = line.find_first_not_of(" \t");
  return next != std::string_view::npos && line[next] == '=';
}

// Writes the menu's settings into the config file, keeping the rest of it:
// each replaces its own top-level line, or is added before the first table.
void SaveSettings() {
  std::vector<std::string> lines;
  if (std::ifstream in(g_configPath); in) {
    for (std::string line; std::getline(in, line);) {
      lines.push_back(line);
    }
  }
  for (const auto& [name, value] : g_written) {
    // (a text setting quoted: the keys' lists)
    const auto* info = rex::cvar::GetFlagInfo(name);
    std::string shown = value;
    if (info && info->type == rex::cvar::FlagType::String) {
      shown = "\"";
      for (char c : value) {
        if (c == '"' || c == '\\') {
          shown += '\\';
        }
        shown += c;
      }
      shown += '"';
    }
    std::string entry = name + " = " + shown;
    size_t firstTable = lines.size();
    bool replaced = false;
    for (size_t i = 0; i < lines.size(); i++) {
      std::string_view line = lines[i];
      line.remove_prefix(std::min(line.find_first_not_of(" \t"), line.size()));
      if (!line.empty() && line[0] == '[') {
        firstTable = std::min(firstTable, i);
        break;
      }
      if (IsKeyLine(line, name)) {
        lines[i] = entry;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      lines.insert(lines.begin() + std::ptrdiff_t(firstTable), entry);
    }
  }
  // Written whole beside it, then moved over it: a failed write (a full disk)
  // leaves the old file as it was.
  // (a symlinked ng2.toml: its target is written, the link kept)
  std::error_code linkError;
  std::filesystem::path target = g_configPath;
  if (std::filesystem::is_symlink(g_configPath, linkError)) {
    if (auto resolved = std::filesystem::canonical(g_configPath, linkError); !linkError) {
      target = resolved;
    }
  }
  std::filesystem::path temporary = target;
  temporary += ".new";
  {
    std::ofstream out(temporary, std::ios::trunc);
    for (const std::string& line : lines) {
      out << line << '\n';
    }
    out.close();
    if (!out) {
      std::error_code ec;
      std::filesystem::remove(temporary, ec);
      REXLOG_ERROR("recomp settings: can't write {}", rex::path_to_utf8(temporary));
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temporary, target, ec);  // (replaces it, on Windows too)
  if (ec) {
    std::filesystem::remove(temporary, ec);
    REXLOG_ERROR("recomp settings: can't replace {}", rex::path_to_utf8(g_configPath));
    return;
  }
  REXLOG_INFO("recomp settings: saved to {}", rex::path_to_utf8(g_configPath));
}

// The game's texts and menu values back, once nothing draws ours: on the
// title menu's next build, or before Options is next built or opened.
void PutBackGame() {
  ng2::video::SetRecompHeader(false);
  for (auto& override : g_overrides) {
    override.store(0, std::memory_order_relaxed);
  }
}

void Close() {
  ng2::rebind::CancelKeyCapture();  // (a Controls cell left waiting)
  if (!g_open) {
    return;
  }
  g_open = false;
  if (!g_written.empty() && !g_configPath.empty()) {
    SaveSettings();
  }
  g_written.clear();
  REXLOG_INFO("recomp settings: closed");
}

constexpr const char* kExclusive = "exclusive_fullscreen";

// The settings as the rows' choices, and back.
constexpr int32_t kFiltering[4] = {-1, 3, 4, 5};  // game, 4x, 8x, 16x

// A setting's value, counting one set but not applied yet.
std::string GetSetting(std::string_view name) {
  if (auto it = g_written.find(name); it != g_written.end()) {
    return it->second;
  }
  return rex::cvar::GetFlagByName(name);
}

// The Cheats page (its rows are the cheats).
constexpr uint32_t kCheatsPage = 3;
const Page& CheatsPage() {
  return *PageOf(kCheatsPage);
}
bool AnyCheatOn() {
  const Page& page = CheatsPage();
  for (uint32_t i = 0; i < page.count; i++) {
    const char* name = kSettings[page.rows[i].setting].name;
    const auto* info = rex::cvar::GetFlagInfo(name);
    if (info && GetSetting(name) != info->default_value) {
      return true;
    }
  }
  return false;
}
bool Unlocked() {
  return GetSetting("cheats_unlocked") == "true" || AnyCheatOn();
}
// Unlocked() for other threads (the achievements' filter runs on the one
// unlocking): kept by the menus' thread, each frame and on each change.
std::atomic<bool> g_cheatsUnlocked{false};

int32_t GetInt(std::string_view name) {
  return std::atoi(GetSetting(name).c_str());
}

uint32_t ValueOf(const Setting& setting) {
  switch (setting.kind) {
    case Kind::kScale: {
      double value = std::atof(GetSetting(setting.name).c_str());
      uint32_t nearest = 0;
      for (uint32_t i = 0; i < std::size(kScales); i++) {
        if (std::abs(kScales[i] - value) < std::abs(kScales[nearest] - value)) {
          nearest = i;
        }
      }
      return nearest;
    }
    case Kind::kFiltering: {
      int32_t filtering = GetInt(setting.name);
      return filtering >= 5 ? 3 : filtering == 4 ? 2 : filtering == 3 ? 1 : 0;
    }
    case Kind::kLevel:
      // Button Prompts' Steam Deck (5, removed) is Auto now, as prompts.cpp takes it.
      if (std::string_view(setting.name) == "button_prompts" && GetInt(setting.name) >= int32_t(setting.count)) {
        return 0;
      }
      return uint32_t(std::clamp(GetInt(setting.name), 0, int32_t(setting.count) - 1));
    case Kind::kSensitivity: {
      double value = std::atof(GetSetting(setting.name).c_str());
      uint32_t nearest = 3;
      for (uint32_t i = 0; i < std::size(kSensitivities); i++) {
        if (std::abs(kSensitivities[i] - value) < std::abs(kSensitivities[nearest] - value)) {
          nearest = i;
        }
      }
      return nearest;
    }
    case Kind::kAspect:
      return ng2::aspect::AspectIndex();
    case Kind::kOnOff:
      return GetSetting(setting.name) == "true" ? 0 : 1;
    case Kind::kPresent:
      // vsync = false from older settings means off until present_mode is set.
      if (!g_written.contains(setting.name) &&
          rex::cvar::GetFlagSource(setting.name) == rex::cvar::Source::kDefault &&
          GetSetting("vsync") == "false") {
        return 0;
      }
      return uint32_t(std::clamp(GetInt(setting.name), 0, int32_t(setting.count) - 1));
    case Kind::kWindowMode:
      return GetSetting("fullscreen") != "true" ? 0 : GetSetting(kExclusive) == "true" ? 2 : 1;
    case Kind::kSamples: {
      int32_t samples = GetInt(setting.name);
      return samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0;
    }
    case Kind::kWindowSize:
      for (uint32_t i = 0; i < kWindowSizeCount; i++) {
        if (GetInt("windowed_width") == WindowSizes()[i].width &&
            GetInt("windowed_height") == WindowSizes()[i].height) {
          return i;
        }
      }
      return 0;
  }
  return 0;
}

// The window's settings, applied on the UI thread (the window belongs to
// it): the fullscreen display mode before fullscreen itself, the windowed
// size after leaving it.
bool IsWindowSetting(std::string_view name) {
  return name == "fullscreen" || name == kExclusive || name == "windowed_width" ||
         name == "windowed_height";
}

constexpr int32_t kNoPosition = -1000000;

// A saved window position still on a connected display (one unplugged since
// isn't), else false.
bool UsablePosition(int32_t x, int32_t y) {
  if (x == kNoPosition || y == kNoPosition) {
    return false;
  }
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  bool on = false;
  for (int i = 0; displays && i < count && !on; i++) {
    SDL_Rect bounds;
    // (the title bar's point: the window can be grabbed back)
    on = SDL_GetDisplayBounds(displays[i], &bounds) && x + 32 >= bounds.x &&
         x + 32 < bounds.x + bounds.w && y + 8 >= bounds.y && y + 8 < bounds.y + bounds.h;
  }
  SDL_free(displays);
  return on;
}

void ApplyWindow(bool fullscreen) {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!window) {
    return;
  }
  if (fullscreen) {
    const SDL_DisplayMode* mode =
        rex::cvar::Query<bool>(kExclusive)
            ? SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window))
            : nullptr;
    SDL_SetWindowFullscreenMode(window, mode);
  }
  // Through the cvar: the runtime keeps its own fullscreen state.
  rex::cvar::SetFlagByName("fullscreen", fullscreen ? "true" : "false");
  if (!fullscreen) {
    int32_t width = rex::cvar::Query<int32_t>("windowed_width");
    int32_t height = rex::cvar::Query<int32_t>("windowed_height");
    if (width <= 0 || height <= 0) {
      width = WindowSizes()[0].width, height = WindowSizes()[0].height;
    }
    SDL_SetWindowSize(window, width, height);
    int32_t x = rex::cvar::Query<int32_t>("windowed_x"), y = rex::cvar::Query<int32_t>("windowed_y");
    if (UsablePosition(x, y)) {
      SDL_SetWindowPosition(window, x, y);
    }
  }
}

// F11 and Alt+Enter: fullscreen on and off; a window moved: its place kept.
// Seen on the UI thread (SDL's event watch), saved on the game thread
// (WindowFrame): the settings file is written there.
std::atomic<int32_t> g_movedX{kNoPosition}, g_movedY{kNoPosition};
std::atomic<int64_t> g_movedAt{0};  // steady_clock ms; 0: nothing to save
std::atomic<int> g_toggledTo{-1};   // 0/1: fullscreen chosen by key, to save
std::atomic<bool> g_applyingWindow{false};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// An event filter, not a watch: it runs before the runtime's own handling
// (an SDL event watch), so Alt+Enter can be kept from also pressing A (Return
// is bound to it). The window is changed on the UI thread, outside SDL's
// dispatch, as the menu changes it.
SDL_EventFilter g_previousFilter = nullptr;
void* g_previousFilterData = nullptr;

bool SDLCALL FilterWindowEvents(void*, SDL_Event* event) {
  bool altEnter = event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_RETURN &&
                  (event->key.mod & SDL_KMOD_ALT);
  if ((altEnter || (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_F11)) &&
      !event->key.repeat && g_appContext) {
    // (from the runtime's own state, which the setting follows)
    bool fullscreen = !rex::cvar::Query<bool>("fullscreen");
    g_movedAt = 0;  // (a move now is the switch's, not the player's)
    g_toggledTo = fullscreen ? 1 : 0;
    g_appContext->CallInUIThread([fullscreen] {
      g_applyingWindow = true;
      ApplyWindow(fullscreen);
      g_applyingWindow = false;
    });
  } else if (event->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN) {
    g_movedAt = 0;
  } else if (event->type == SDL_EVENT_WINDOW_MOVED && !g_applyingWindow) {
    SDL_Window* window = SDL_GetWindowFromID(event->window.windowID);
    if (window && !(SDL_GetWindowFlags(window) &
                    (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
      g_movedX = event->window.data1, g_movedY = event->window.data2;
      g_movedAt = NowMs();
    }
  }
  if (altEnter) {
    return false;  // (the key-up still comes; a lone release presses nothing)
  }
  return g_previousFilter ? g_previousFilter(g_previousFilterData, event) : true;
}

bool g_windowChanged = false;

void ScheduleWindow() {
  g_windowChanged = false;
  if (!g_appContext) {
    return;
  }
  std::vector<std::pair<std::string, std::string>> values;
  for (const auto& [name, value] : g_written) {
    if (IsWindowSetting(name) && name != "fullscreen") {
      values.emplace_back(name, value);
    }
  }
  bool fullscreen = GetSetting("fullscreen") == "true";
  g_appContext->CallInUIThread([values, fullscreen] {
    for (const auto& [name, value] : values) {
      rex::cvar::SetFlagByName(name, value);
    }
    ApplyWindow(fullscreen);
  });
}

void SetSetting(const char* name, const std::string& text) {
  if (GetSetting(name) == text) {
    return;
  }
  g_written[name] = text;
  // In game, each press's changes are saved at its end (the input handler):
  // the menu can close without Back (Start, the window closed).
  if (g_inGameOurs) {
    g_unsaved = true;
  }
  if (IsWindowSetting(name)) {
    g_windowChanged = true;  // applied once the row's settings are all set
    return;
  }
  rex::cvar::SetFlagByName(name, text);
  g_cheatsUnlocked.store(Unlocked(), std::memory_order_relaxed);
}

void SetValue(const Setting& setting, uint32_t value) {
  switch (setting.kind) {
    case Kind::kScale:
      SetSetting(setting.name, fmt::format("{}", kScales[std::min<size_t>(value, std::size(kScales) - 1)]));
      break;
    case Kind::kFiltering:
      SetSetting(setting.name, std::to_string(kFiltering[value]));
      break;
    case Kind::kLevel:
      SetSetting(setting.name, std::to_string(value));
      break;
    case Kind::kSensitivity:
      SetSetting(setting.name, fmt::format("{}", kSensitivities[std::min<size_t>(value, std::size(kSensitivities) - 1)]));
      break;
    case Kind::kAspect: {
      // The window keeps its place in Window Size's list, at the new shape.
      const Setting* sizes = nullptr;
      for (const Setting& other : kSettings) {
        if (other.kind == Kind::kWindowSize) {
          sizes = &other;
        }
      }
      uint32_t size = sizes ? ValueOf(*sizes) : 0;
      SetSetting(setting.name, std::to_string(value));
      if (sizes) {
        SetValue(*sizes, size);
      }
      break;
    }
    case Kind::kOnOff:
      SetSetting(setting.name, value == 0 ? "true" : "false");
      break;
    case Kind::kPresent:
      // The older vsync setting would keep it off otherwise. It is also the
      // SDK's guest vblank switch (off: 1000 Hz, which the game would run
      // at), so it stays on.
      SetSetting("vsync", "true");
      SetSetting(setting.name, std::to_string(value));
      break;
    case Kind::kWindowMode:
      SetSetting(kExclusive, value == 2 ? "true" : "false");
      SetSetting("fullscreen", value != 0 ? "true" : "false");
      break;
    case Kind::kSamples:
      SetSetting(setting.name, std::to_string(value == 0 ? 0 : 1 << value));
      break;
    case Kind::kWindowSize:
      SetSetting("windowed_width", std::to_string(WindowSizes()[value].width));
      SetSetting("windowed_height", std::to_string(WindowSizes()[value].height));
      break;
  }
}

void RestoreDefault(const Setting& setting) {
  // The aspect through its own setter: the window keeps its place in Window
  // Size's list.
  if (setting.kind == Kind::kAspect) {
    if (const auto* info = rex::cvar::GetFlagInfo(setting.name)) {
      SetValue(setting, uint32_t(std::atoi(info->default_value.c_str())));
    }
    return;
  }
  auto reset = [](const char* name) {
    if (const auto* info = rex::cvar::GetFlagInfo(name)) {
      SetSetting(name, info->default_value);
    }
  };
  reset(setting.name);
  if (setting.kind == Kind::kPresent) {
    reset("vsync");
  } else if (setting.kind == Kind::kWindowMode) {
    reset(kExclusive);
  } else if (setting.kind == Kind::kWindowSize) {
    reset("windowed_height");
  }
}

}  // namespace

// rebind.cpp's settings: as ours, and saved at once outside our menus (the
// game's own Controller Settings changed them).
bool g_controlsToSave = false;  // set outside our menus: saved at the frame's end (WindowFrame)
void StoreControl(const char* name, const std::string& value) {
  SetSetting(name, value);
  if (!g_open && !g_inGameOurs) {
    g_controlsToSave = true;
  }
}

void SetConfigPath(const std::filesystem::path& path) {
  g_configPath = path;
  ng2::rebind::SetStore(StoreControl);
}

// Older versions wrote the keys' settings unquoted (key_jump = Space): not
// TOML, so the whole file failed to load and every setting started at its
// default. Before it is loaded: each top-level line of a text setting whose
// value isn't quoted gets its quotes, written back only if something changed.
std::string g_repairNote;  // logged once logging is up (ReportConfigRepair)
void RepairConfig(const std::filesystem::path& given) {
  // (a symlinked config: its target written, as SaveSettings does)
  std::error_code canonicalError;
  std::filesystem::path path = std::filesystem::canonical(given, canonicalError);
  if (canonicalError) {
    path = given;
  }
  std::ifstream in(path);
  if (!in) {
    return;
  }
  std::vector<std::string> lines;
  uint32_t fixed = 0;
  bool inTable = false;
  for (std::string line; std::getline(in, line);) {
    std::string_view view = line;
    size_t start = view.find_first_not_of(" \t");
    if (start != std::string_view::npos && view[start] == '[') {
      inTable = true;
    }
    size_t eq = view.find('=');
    if (!inTable && start != std::string_view::npos && view[start] != '#' && eq != std::string_view::npos) {
      std::string_view name = view.substr(start, eq - start);
      name = name.substr(0, name.find_last_not_of(" \t") + 1);
      std::string_view value = view.substr(eq + 1);
      size_t v = value.find_first_not_of(" \t");
      const auto* info = rex::cvar::GetFlagInfo(std::string(name));
      // (an empty value too: "key =" isn't TOML either; arrays and tables
      // left alone)
      bool alreadyTOML = v != std::string_view::npos && (value[v] == '"' || value[v] == '\'' ||
                                                         value[v] == '[' || value[v] == '{');
      if (info && info->type == rex::cvar::FlagType::String && !alreadyTOML) {
        std::string text(v == std::string_view::npos ? std::string_view() : value.substr(v));
        // (a comment after the value stays one)
        std::string comment;
        if (size_t hash = text.rfind('#', 0) == 0 ? 0 : text.find(" #"); hash != std::string::npos) {
          comment = text.substr(hash);
          text.resize(hash);
        }
        text.erase(text.find_last_not_of(" \t\r") + 1);
        std::string quoted = "\"";
        for (char c : text) {
          if (c == '"' || c == '\\') {
            quoted += '\\';
          }
          quoted += c;
        }
        quoted += '"';
        line = std::string(name) + " = " + quoted + comment;
        fixed++;
      }
    }
    lines.push_back(std::move(line));
  }
  in.close();
  if (!fixed) {
    return;
  }
  std::filesystem::path temp = path;
  temp += ".tmp";
  std::error_code ec;
  {
    std::ofstream out(temp, std::ios::trunc);
    for (const std::string& line : lines) {
      out << line << '\n';
    }
    out.close();  // (a full disk fails here, not at the writes)
    if (!out) {
      ec = std::make_error_code(std::errc::io_error);
    }
  }
  if (!ec) {
    std::filesystem::rename(temp, path, ec);
  }
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
  }
  g_repairNote = ec ? fmt::format("recomp settings: couldn't repair {} ({}); its settings don't load",
                                  rex::path_to_utf8(path), ec.message())
                    : fmt::format("recomp settings: {} unquoted text setting(s) quoted in {} (it "
                                  "didn't load before)",
                                  fixed, rex::path_to_utf8(path));
}

void ReportConfigRepair() {
  if (!g_repairNote.empty()) {
    REXLOG_WARN("{}", g_repairNote);
  }
}

void SetAppContext(rex::ui::WindowedAppContext* context) {
  g_appContext = context;
}

// The text lookup's hook (storage_prompt.cpp): our text for this id while
// ours is shown, as a guest address; 0: the game's.
uint32_t RecompText(uint32_t id) {
  uint32_t slot = id - kTable;
  return slot < kSlotCount ? g_overrides[slot].load(std::memory_order_acquire) : 0;
}

}  // namespace ng2::recomp_settings

using namespace ng2::recomp_settings;

namespace {

// Calls a guest function with r3-r8 below the caller's guest stack (the
// replaced handler has no frame of its own), leaving the caller's registers
// as they were.
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*),
                   std::initializer_list<uint32_t> args, double f1) {
  PPCContext saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  uint64_t* registers[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64,
                           &ctx.r6.u64, &ctx.r7.u64, &ctx.r8.u64};
  size_t i = 0;
  for (uint32_t arg : args) {
    *registers[i++] = arg;
  }
  ctx.f1.f64 = f1;
  function(ctx, base);
  uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}


}  // namespace

REX_EXTERN(__imp__sub_823E3828);  // a pad direction: (pad state, pad, 1, 0, 0 up 1 down 2 right 3 left)
REX_EXTERN(__imp__sub_823E4158);  // Confirm: (&pad state, cursor, enabled[], 0)
REX_EXTERN(__imp__sub_823E42B0);  // Back: (&pad state, cursor, 0xFF, 0)
REX_EXTERN(__imp__sub_823E43B0);  // Y, a row's default: (&pad state)
REX_EXTERN(__imp__sub_8242AFC0);  // a menu sound: (id), f1
REX_EXTERN(__imp__sub_836A5AA0);  // a menu transition: (n, flag)
REX_EXTERN(__imp__sub_836A4B90);  // the page's state into the menu model
REX_EXTERN(__imp__sub_836CF0D0);  // a widget's reset: (widget, model)
REX_EXTERN(__imp__sub_836CF198);  // a list's or label's height: (widget) -> f1
REX_EXTERN(__imp__sub_836CF3B0);  // a choice row's height: (widget) -> f1
REX_EXTERN(__imp__sub_83704630);  // a text sprite's constructor: (sprite)
REX_EXTERN(__imp__sub_8242C048);  // a widget part's constructor: (part)
REX_EXTERN(__imp__sub_823E1238);  // a string: (table, language, id) -> pointer
REX_EXTERN(__imp__sub_8367F948);  // the text language
REX_EXTERN(__imp__sub_836CF1A8);  // a choice onto a choice row: (widget, string id)
REX_EXTERN(__imp__sub_836D03E8);  // a row onto a page: (page, label id, widget)

// The title menu's entries (sub_8366F4D0): 0x140 bytes each, from
// 0x853A3B60; at +0x124 the action, +0x128 the row's place, +0x130 "needs
// Xbox LIVE" (shown greyed while signed out). LEADERBOARDS (action 3) goes,
// the entries below move up into the rows above, and ours isn't greyed.
constexpr uint32_t kTitleEntries = 0x853A3B60;
constexpr uint32_t kTitleEntrySize = 0x140;
constexpr uint32_t kTitleEntryCount = 0x853A4A60;
constexpr uint32_t kEntryAction = 0x124, kEntryPlace = 0x128, kEntryOnline = 0x130;
constexpr uint32_t kLeaderboards = 3, kMarketplace = 6;
// A new last entry quits the game: action 7, which the title menu doesn't
// handle; its picture (table 0x83AB3878, by language and action) is the
// atlas's EXIT DEMO band, redrawn as EXIT GAME (src/gpu/menu_label.cpp).
constexpr uint32_t kExitAction = 7;
constexpr uint32_t kEntryStoragePrompt = 0x12C;
constexpr uint32_t kTitleEntryCapacity = 12;
constexpr uint32_t kTitleCursor = 0x853A4A64;
constexpr uint32_t kPadPressed = 0x83C07FF8;  // + pad * 0x44: newly pressed buttons
constexpr uint32_t kConfirmButtons = 0x150;

REX_EXTERN(__imp__sub_8366F4D0);
void CheatsTitleMenu(PPCContext& ctx, uint8_t* base, void (*build)(PPCContext&, uint8_t*));
REX_HOOK_RAW(sub_8366F4D0) {
  PutBackGame();
  CheatsTitleMenu(ctx, base, __imp__sub_8366F4D0);  // (Unlock Difficulties' paths)
  using ng2::guest::Load32;
  uint32_t count = Load32(kTitleEntryCount);
  if (count > 8) {
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint32_t entry = kTitleEntries + i * kTitleEntrySize;
    if (Load32(entry + kEntryAction) == kMarketplace) {
      Store32(entry + kEntryOnline, 0);
    }
  }
  for (uint32_t i = 0; i < count; i++) {
    uint32_t entry = kTitleEntries + i * kTitleEntrySize;
    if (Load32(entry + kEntryAction) != kLeaderboards) {
      continue;
    }
    for (uint32_t j = i; j + 1 < count; j++) {
      uint32_t to = kTitleEntries + j * kTitleEntrySize;
      uint32_t place = Load32(to + kEntryPlace);
      std::memcpy(ng2::guest::Virtual(to), ng2::guest::Virtual(to + kTitleEntrySize),
                  kTitleEntrySize);
      Store32(to + kEntryPlace, place);
    }
    Store32(kTitleEntryCount, --count);
    break;
  }
  if (count >= 2 && count < kTitleEntryCapacity) {
    uint32_t last = kTitleEntries + (count - 1) * kTitleEntrySize;
    uint32_t exit = last + kTitleEntrySize;
    std::memcpy(ng2::guest::Virtual(exit), ng2::guest::Virtual(last), kTitleEntrySize);
    Store32(exit + kEntryAction, kExitAction);
    Store32(exit + kEntryPlace,
            Load32(last + kEntryPlace) * 2 - Load32(last - kTitleEntrySize + kEntryPlace));
    Store32(exit + kEntryStoragePrompt, 0);
    Store32(exit + kEntryOnline, 0);
    Store32(kTitleEntryCount, count + 1);
  }
}

// The title menu's "signed in" check: ours always takes the signed-out path,
// whose message sub_8366FAC8 turns into our screen.
REX_EXTERN(__imp__sub_8366F2E8);
REX_HOOK_RAW(sub_8366F2E8) {
  uint32_t index = ctx.r3.u32;
  if (index < ng2::guest::Load32(kTitleEntryCount) &&
      ng2::guest::Load32(kTitleEntries + index * kTitleEntrySize + kEntryAction) == kMarketplace) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8366F2E8(ctx, base);
}

// The row of a column of rows the pointer is on (in the 1280x720 UI: the
// column's left and right, the first row's center, the rows' pitch), or -1.
int PointedRow(float left, float right, float top, float pitch, int count) {
  float x, y;
  if (!ng2::menu_mouse::Pointer(&x, &y) || x < left || x > right) {
    return -1;
  }
  float row = std::floor((y - top) / pitch + 0.5f);
  return row >= 0.0f && row < float(count) ? int(row) : -1;
}

// The title menu's entries (the pointer picks one): centers and pitch.
constexpr float kTitleMenuLeft = 500.0f, kTitleMenuRight = 780.0f;
constexpr float kTitleMenuTop = 432.0f, kTitleMenuPitch = 35.2f;

// Mission Mode's list (sub_823E4758, its state at +0, the mission picked at
// +4; state 3 takes input): 24 missions in rows of four, three rows shown,
// scrolled by the view (0x8536CEF0) to keep the picked one in sight, its
// offset at +0x3A00 (-75 a row). The pointer on a mission picks it as the
// d-pad does (sound 51, the view told); a click on none confirms nothing
// (#163).
REX_EXTERN(__imp__sub_836C7738);  // the mission view: (view, mission)
constexpr uint32_t kMissionView = 0x8536CEF0;
constexpr float kMissionLeft = 612.0f, kMissionTop = 197.0f, kMissionWidth = 130.0f, kMissionHeight = 75.0f;
REX_EXTERN(__imp__sub_823E4758);
REX_HOOK_RAW(sub_823E4758) {
  using ng2::guest::Load32;
  uint32_t object = ctx.r3.u32;
  float x, y;
  if (Load32(object) == 3 && ng2::menu_mouse::Pointer(&x, &y)) {
    int column = int(std::floor((x - kMissionLeft) / kMissionWidth));
    int row = int(std::floor((y - kMissionTop) / kMissionHeight));
    int first = int(std::lround(-LoadFloat(kMissionView + 0x3A00) / kMissionHeight));
    int mission = -1;
    if (column >= 0 && column < 4 && row >= 0 && row < 3 && first >= 0 && first <= 3) {
      mission = (first + row) * 4 + column;
    }
    if (ng2::menu_mouse::ClickedThisFrame() && mission < 0) {
      constexpr uint32_t kA = 0x10;
      for (uint32_t pad = 0; pad < 5; pad++) {  // (the four, then the merged one)
        uint32_t at = kPadPressed + pad * 0x44;
        Store32(at, Load32(at) & ~kA);
      }
    }
    if (ng2::menu_mouse::PointerActive() && mission >= 0 && uint32_t(mission) != Load32(object + 4)) {
      Store32(object + 4, uint32_t(mission));
      CallGuest(ctx, base, __imp__sub_8242AFC0, {51}, LoadFloat(kCursorSound));
      CallGuest(ctx, base, __imp__sub_836C7738, {kMissionView, uint32_t(mission)}, 0.0);
    }
  }
  __imp__sub_823E4758(ctx, base);
}

// The title menu's input: Confirm on the exit entry quits. (Each frame the
// menu is up: perf_overlay.cpp shows the version then.)
void TitleMenuFrame();
REX_EXTERN(__imp__sub_8366FBC0);
REX_HOOK_RAW(sub_8366FBC0) {
  ng2::menu_mouse::NoteStage(false);  // (the title's: no stage)
  // The pointer on an entry: the cursor there (before the menu's update,
  // which lights it); a click on none confirms nothing (its A taken out of
  // the pads' presses for this update).
  if (ng2::menu_mouse::ClickedThisFrame()) {
    uint32_t count = std::min<uint32_t>(ng2::guest::Load32(kTitleEntryCount), 8);
    if (PointedRow(kTitleMenuLeft, kTitleMenuRight, kTitleMenuTop, kTitleMenuPitch, int(count)) < 0) {
      constexpr uint32_t kA = 0x10;
      for (uint32_t pad = 0; pad < 5; pad++) {  // (the four, then the merged one)
        uint32_t at = kPadPressed + pad * 0x44;
        Store32(at, ng2::guest::Load32(at) & ~kA);
      }
    }
  }
  if (ng2::menu_mouse::PointerActive()) {
    uint32_t count = std::min<uint32_t>(ng2::guest::Load32(kTitleEntryCount), 8);
    int row = PointedRow(kTitleMenuLeft, kTitleMenuRight, kTitleMenuTop, kTitleMenuPitch, int(count));
    if (row >= 0 && uint32_t(row) != ng2::guest::Load32(kTitleCursor)) {
      // (the sound the menu makes when its cursor moves: 17, #163)
      CallGuest(ctx, base, __imp__sub_8242AFC0, {17}, LoadFloat(kCursorSound));
      Store32(kTitleCursor, uint32_t(row));
    }
  }
  __imp__sub_8366FBC0(ctx, base);
  TitleMenuFrame();
  ng2::menu_mouse::NoteMenuInput();
  using ng2::guest::Load32;
  uint32_t player = *ng2::guest::Virtual(kPlayerPad);
  uint32_t cursor = Load32(kTitleCursor);
  if ((Load32(kPadPressed + player * 0x44) & kConfirmButtons) && cursor < Load32(kTitleEntryCount) &&
      Load32(kTitleEntries + cursor * kTitleEntrySize + kEntryAction) == kExitAction &&
      g_appContext) {
    REXLOG_INFO("title menu: exit");
    // As the window's close button: the runtime ends the title and exits
    // (a plain quit runs a teardown that can wait on the game's threads).
    g_appContext->CallInUIThread([] {
      int count = 0;
      SDL_Window** windows = SDL_GetWindows(&count);
      SDL_Window* window = (windows && count > 0) ? windows[0] : nullptr;
      SDL_free(windows);
      if (window) {
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        event.window.windowID = SDL_GetWindowID(window);
        SDL_PushEvent(&event);
      }
    });
  }
}

// Opening a screen: the Marketplace message becomes the Options screen with
// our settings in it.
REX_EXTERN(__imp__sub_8366FAC8);
REX_HOOK_RAW(sub_8366FAC8) {
  if (ctx.r4.u32 == kMarketplaceMessage) {
    ctx.r4.u64 = kOptionsScreen;
    Open();
  }
  __imp__sub_8366FAC8(ctx, base);
}

// The Options screen's run: its loop (sub_823E59A0) returns when Options
// closes.
REX_EXTERN(__imp__sub_823E57D8);
REX_HOOK_RAW(sub_823E57D8) {
  __imp__sub_823E57D8(ctx, base);
  Close();
}


namespace {

// The page objects the title's pages were built on (the Graphics page's
// cursor mark is our panel's).
uint32_t g_titleObject[std::size(kPages)] = {};
uint32_t PageIndex(const Page& page) {
  return uint32_t(&page - kPages);
}
constexpr uint32_t kPageValues = 8;
// The menu model's values for a page: none (ours draws its own).
uint32_t StoreValues(PPCContext& ctx, const Page*) {
  uint32_t values = ctx.r1.u32 - 0x80;
  for (uint32_t i = 0; i < kPageValues; i++) {
    Store32(values + 4 * i, 0);
  }
  return values;
}

REX_EXTERN(__imp__sub_83835960);  // XMemAlloc(size, attributes)

constexpr uint32_t kListArenaAttributes = 0xB2800000;  // (the arrow list's memory)


// A list widget made in place, as the Game Settings page's constructor
// (sub_836A5200) makes its language list.
void MakeList(PPCContext& ctx, uint8_t* base, uint32_t widget) {
  std::memset(ng2::guest::Virtual(widget), 0, kListSize);
  Store32(widget, kListVtable);
  for (uint32_t sprite : {0x50u, 0xE0u, 0x170u}) {
    Store32(widget + sprite + 0x74, 0);
    CallGuest(ctx, base, __imp__sub_83704630, {widget + sprite});
  }
  CallGuest(ctx, base, __imp__sub_8242C048, {widget + 0x200});
  Store32(widget + 0x260, 1);
}

// After a page's builder (model: its second argument, the menu model +
// 0x10): the page object kept (the Graphics page's cursor mark is the
// panel's), and once an arrow list of ours (its arrows are the panel's).
REX_EXTERN(__imp__sub_836D0A70);  // an arrow list's pictures: (list)
uint32_t g_arrowList = 0;
constexpr uint32_t kArrowPicture = 0x50 + 0x60;  // its right arrow's picture
void BuildPage(PPCContext& ctx, uint8_t* base, const Page& page, uint32_t object, uint32_t model) {
  g_titleObject[PageIndex(page)] = object;
  Store32(object + kPageRows, 0);
  if (!g_arrowList) {
    g_arrowList = CallGuest(ctx, base, __imp__sub_83835960, {kListSize, kListArenaAttributes});
    if (!g_arrowList) {
      return;
    }
    MakeList(ctx, base, g_arrowList);
  }
  CallGuest(ctx, base, __imp__sub_836CF0D0, {g_arrowList, model});
  Store32(g_arrowList + kArrowPicture, 0);  // (its pictures taken again: ArrowPictures)
}
// The arrow list's pictures, once UI package 120 (Options') has them: each
// frame of ours until then (its builders run before it's loaded). Taken
// again each time Options opens (BuildPage): play reuses the package's memory.
void ArrowPictures(PPCContext& ctx, uint8_t* base) {
  if (g_arrowList && !ng2::guest::Load32(g_arrowList + kArrowPicture)) {
    CallGuest(ctx, base, __imp__sub_836D0A70, {g_arrowList});
  }
}

// A page's builder, run once as Options opens: the game's rows left out,
// then ours.
void Build(PPCContext& ctx, uint8_t* base, void (*builder)(PPCContext&, uint8_t*), uint32_t number) {
  uint32_t object = ctx.r3.u32, model = ctx.r4.u32;
  const Page* page = nullptr;
  for (const Page& p : kPages) {
    page = ViewOf(p) == number ? &p : page;
  }
  if (!page) {
    builder(ctx, base);
    return;
  }
  if (!g_open) {
    PutBackGame();
  }
  if (!g_open) {
    builder(ctx, base);
    return;
  }
  g_building = object;
  g_buildingSize = page->size;
  builder(ctx, base);
  g_building = 0;
  if (page->view) {
    // Its page and the one before, as the settings pages' builders store
    // them (+12, +8; the Copyright Information page's doesn't): each row
    // appended takes them, for its cursor and value in the menu model.
    Store32(object + 8, page->view - 1);
    Store32(object + 12, page->view);
  }
  BuildPage(ctx, base, *page, object, model);
}

bool InBuild(uint32_t address) {
  return g_building && address - g_building < g_buildingSize;
}


}  // namespace

namespace {


// The Controls page: a table of ours (the overlay's texts) in the page's
// place, on the title's Options screen and in the in-game list, as the NGB
// recomp's: an action a row, with its two keys and its controller button
// (rebind.h), then Default Controls. Confirm on a cell waits for a key, or a
// button (two together for Ninpo and Wind Run); X (Delete) clears it, Y gives the
// row its defaults.
constexpr uint32_t kMergedPad = 0x83C08100;  // the pads' buttons: +0 held, +8 pressed
constexpr uint32_t kButtonMasks[8] = {0x10, 0x20, 0x40, 0x80, 0x1000, 0x2000, 0x4000, 0x8000};
constexpr uint32_t kPadX = 0x40;
constexpr std::u16string_view kButtonTags[8] = {u"@[pad:Rd]", u"@[pad:Rr]", u"@[pad:Rl]", u"@[pad:Ru]",
                                                u"@[pad:L1]", u"@[pad:R1]", u"@[pad:L2]", u"@[pad:R2]"};
constexpr std::u16string_view kButtonNames[8] = {u"A", u"B", u"X", u"Y", u"LB", u"RB", u"LT", u"RT"};
constexpr int kControlRows = ng2::rebind::kActions + 1;  // (the last: Default Controls)
struct ControlsState {
  int row = 0, column = 0;  // column: 0 and 1 the keys, 2 the controller's
  int capture = 0;          // waiting for 1 a key, 2 a button
  uint32_t ignore = 0;      // buttons held when the wait began, until let go
  int first = -1;           // a pair's first button, held
  bool letGo = false;       // after a button was taken: nothing until all are let go
  std::chrono::steady_clock::time_point since, firstAt;
};
ControlsState g_controls;

void EndControlsCapture() {
  ng2::rebind::CancelKeyCapture();
  g_controls.capture = 0;
}

// The table's place in the 1280x720 UI.
constexpr float kTableNameX = 456.0f, kTableCellX[3] = {740.0f, 880.0f, 1020.0f};
constexpr float kTableTop = 128.0f, kTablePitch = 21.5f;

// The table, shown for this frame (focused: the cursor is in it).
uint32_t g_inGameLayer = 0;  // the in-game list's rows' layer (its draw)
void ShowControls(bool focused, uint32_t layer = 0) {
  namespace rebind = ng2::rebind;
  // (shown before it takes input: Delete is its Clear from its first frame,
  // never Y's Default)
  ng2::menu_mouse::DeleteClears();
  constexpr float kNameX = kTableNameX, kTop = kTableTop, kPitch = kTablePitch, kHelpY = 606.0f;
  constexpr const float (&kCellX)[3] = kTableCellX;
  constexpr int kSize = 0x12;
  constexpr float kDim = 0.5f, kRowLit = 0.8f, kFixed = 0.25f;
  const ControlsState& c = g_controls;
  std::vector<ng2::overlay::Text> texts;
  auto put = [&](float x, float y, std::u16string text, float shade) {
    texts.push_back({x, y, std::move(text), shade, kSize});
  };
  bool keyboardPrompts = ng2::prompts::CurrentSet() == ng2::prompts::Set::kKeyboard;
  for (int row = 0; row < kControlRows; row++) {
    float y = kTop + kPitch * float(row);
    bool lit = focused && row == c.row;
    if (row == rebind::kActions) {
      put(kNameX, y, u"Default Controls", lit ? 1.0f : kDim);
      continue;
    }
    put(kNameX, y, rebind::Name(row), lit ? kRowLit : kDim);
    for (int column = 0; column < 3; column++) {
      bool on = lit && column == c.column && !(column == 2 && rebind::PadFixed(row));
      std::u16string text;
      if (on && c.capture) {
        text = c.capture == 1 ? u"Press a key" : u"Press a button";
      } else if (column < 2) {
        std::string key = rebind::Key(row, column);
        text.assign(key.begin(), key.end());
      } else if (rebind::PadFixed(row)) {
        text = rebind::PadFixedText(row);
      } else if (int binding = rebind::Pad(row); binding != rebind::kNone) {
        // (the buttons' pictures, or their names while the prompts are the
        // keyboard's: its pictures would be keys)
        const auto& names = keyboardPrompts ? kButtonNames : kButtonTags;
        text = names[rebind::First(binding)];
        if (rebind::IsPair(binding)) {
          text += u" + ";
          text += names[rebind::Second(binding)];
        }
      }
      if (text.empty()) {
        text = u"-";
      }
      if (on && !c.capture) {
        text = u"‹ " + text + u" ›";
      }
      // (the controller's own, not to be set here: greyed)
      bool fixed = column == 2 && rebind::PadFixed(row);
      put(kCellX[column], y, std::move(text), fixed ? kFixed : on ? 1.0f : lit ? kRowLit : kDim);
    }
  }
  if (focused) {
    put(kNameX, kHelpY,
        c.capture == 1   ? u"Press a key or mouse button (Esc leaves it)"
        : c.capture == 2 ? (rebind::PadPairs(c.row) ? u"Press a button, or two together"
                                                    : u"Press the controller's button")
        : c.row == rebind::kActions ? u"@[pad:Rd] Every action as it was"
                                    : u"@[pad:Rd] Set   @[pad:Rl] Clear   @[pad:Ru] Default",
        kRowLit);
  }
  ng2::overlay::SetPanel(std::move(texts), layer, layer != 0);
}

// The table's input for a frame; true when Back leaves it.
bool RunControls(PPCContext& ctx, uint8_t* base) {
  namespace rebind = ng2::rebind;
  using ng2::guest::Load32;
  ng2::menu_mouse::DeleteClears();  // (Delete: Clear here, as X)
  using Clock = std::chrono::steady_clock;
  ControlsState& c = g_controls;
  double sound = LoadFloat(kCursorSound);
  auto play = [&](uint32_t which) { CallGuest(ctx, base, __imp__sub_8242AFC0, {which}, sound); };
  bool back = CallGuest(ctx, base, __imp__sub_823E42B0, {kPadState, 0, 0xFF, 0}) != 0;
  uint32_t held = Load32(kMergedPad), pressed = Load32(kMergedPad + 8);
  if (c.capture == 1) {
    if (back) {
      rebind::CancelKeyCapture();
    }
    std::string key;
    if (!rebind::PollKeyCapture(key)) {
      if (!key.empty()) {
        rebind::SetKey(c.row, c.column, key);
      }
      play(key.empty() ? 2 : 3);
      c.capture = 0;
    }
    return false;
  }
  if (c.capture == 2) {
    c.ignore &= held;
    uint32_t now = held & ~c.ignore;
    int found = -1;
    for (int b = 0; b < 8; b++) {
      if ((now & kButtonMasks[b]) && b != c.first && found < 0) {
        found = b;
      }
    }
    int binding = rebind::kNone;
    bool done = false;
    if (!rebind::PadPairs(c.row)) {
      done = found >= 0;
      binding = found;
    } else if (c.first < 0) {
      if (found >= 0) {
        c.first = found, c.firstAt = Clock::now();  // a second may join it
      }
    } else if (found >= 0) {
      done = true, binding = rebind::Pair(c.first, found);
    } else if (!(held & kButtonMasks[c.first]) || Clock::now() - c.firstAt > std::chrono::milliseconds(1500)) {
      done = true, binding = c.first;  // let go, or held a while, alone
    }
    if (done) {
      play(rebind::SetPad(c.row, binding) ? 3 : 2);
      c.capture = 0;
      c.letGo = true;
    } else if (c.first < 0 && Clock::now() - c.since > std::chrono::seconds(5)) {
      play(2);
      c.capture = 0;  // none in time: as it was
    }
    return false;
  }
  if (c.letGo) {
    uint32_t any = 0;
    for (uint32_t mask : kButtonMasks) {
      any |= mask;
    }
    if (held & any) {
      return false;
    }
    c.letGo = false;
  }
  if (back) {
    return true;
  }
  uint32_t pad = Load32(kPadState);
  uint32_t player = *ng2::guest::Virtual(kPlayerPad);
  auto direction = [&](uint32_t which) {
    return CallGuest(ctx, base, __imp__sub_823E3828, {pad, player, 1, 0, which}) != 0;
  };
  int row = c.row, column = c.column;
  if (direction(0)) {
    row = (row + kControlRows - 1) % kControlRows;
  }
  if (direction(1)) {
    row = (row + 1) % kControlRows;
  }
  // (a fixed row's controller cell isn't one to stop on: two columns there)
  int columns = row < rebind::kActions && rebind::PadFixed(row) ? 2 : 3;
  column = std::min(column, columns - 1);
  if (direction(3)) {
    column = (column + columns - 1) % columns;
  }
  if (direction(2)) {
    column = (column + 1) % columns;
  }
  // The pointer on a cell: that one (on a name: the row, its cell kept).
  float px, py;
  if (ng2::menu_mouse::PointerActive() && ng2::menu_mouse::Pointer(&px, &py)) {
    int pointed = PointedRow(kTableNameX - 16.0f, kTableCellX[2] + 140.0f, kTableTop, kTablePitch, kControlRows);
    if (pointed >= 0) {
      row = pointed;
      int pointedColumn = px >= kTableCellX[2] - 10.0f ? 2 : px >= kTableCellX[1] - 10.0f ? 1
                        : px >= kTableCellX[0] - 10.0f ? 0 : -1;
      bool fixed = row < rebind::kActions && rebind::PadFixed(row);
      if (pointedColumn >= 0 && !(pointedColumn == 2 && fixed)) {
        column = pointedColumn;
      } else if (fixed) {
        column = std::min(column, 1);
      }
    }
  }
  if (row != c.row || column != c.column) {
    play(2);
    c.row = row, c.column = column;
  }
  // (a click off the table confirms nothing)
  bool clickOff = ng2::menu_mouse::ClickedThisFrame() &&
                  PointedRow(kTableNameX - 16.0f, kTableCellX[2] + 140.0f, kTableTop, kTablePitch, kControlRows) < 0;
  // (X, and Delete, clear: the game's confirm takes X too in some of its
  // modes, sub_823E4158's +56 word: 0 A/X/Start, 2 A/X, 4 A)
  bool clear = (pressed & kPadX) != 0;
  bool confirm = !clickOff && !clear && CallGuest(ctx, base, __imp__sub_823E4158, {kPadState, 0, 0, 0}) != 0;
  if (c.row == rebind::kActions) {
    if (confirm) {
      rebind::Defaults();
      play(3);
    }
  } else if (confirm) {
    if (c.column < 2) {
      c.capture = 1;
      rebind::StartKeyCapture();
      play(3);
    } else if (!rebind::PadFixed(c.row)) {
      c.capture = 2, c.ignore = held, c.first = -1, c.since = Clock::now();
      play(3);
    }
  } else if (clear) {
    if (c.column < 2) {
      rebind::SetKey(c.row, c.column, "");
    } else {
      if (!rebind::SetPad(c.row, rebind::kNone)) {
        play(2);
        return false;
      }
    }
    play(3);
  } else if (CallGuest(ctx, base, __imp__sub_823E43B0, {kPadState})) {
    play(rebind::Default(c.row) ? 3 : 2);
  }
  return false;
}

// The pages but Controls: our panel (settings_panel.h), with each
// setting's note under it.
std::u16string_view NoteOf(std::string_view name) {
  static constexpr std::pair<std::string_view, std::u16string_view> kNotes[] = {
      {"render_scale", u"The size the game's 3D is drawn at."},
      {"base_resolution_720p", u"The 3D's size before Render Scale. Takes effect after a restart."},
      {"antialiasing", u"Smooths edges. The game's own is 2x."},
      {"anisotropic_filtering", u"Sharper textures seen at an angle."},
      {"smooth_normal_maps", u"Normal maps blended between their sizes, no hard lines."},
      {"shadows", u"Hi-Res: the larger shadow map in fights too."},
      {"bloom", u"The glow around bright lights."},
      {"aspect_ratio", u"The picture's shape. Window Size follows it."},
      {"windowed_width", u"The window's size when windowed."},
      {"present_mode", u"Off can tear. FIFO waits for each refresh."},
      {"input_latency", u"Low: no finished frame waits for the screen, a refresh or two less delay (not with interpolation)."},
      {"frame_pacing", u"Auto times frames to the display, Timer to the game's clock."},
      {"frame_interpolation", kInterpNote},
      {"show_fps", u"The frame rate in a corner of the screen."},
      {"button_prompts", u"Auto follows the controller or keyboard in use."},
      {"mnk_sensitivity", u"How fast the mouse turns the camera."},
      {"cheat_infinite_health", u"Ryu takes no damage."},
      {"cheat_infinite_ninpo", u"Ninpo costs nothing."},
      {"cheat_all_weapons", kAllWeaponsNote},
      {"cheat_one_hit_kill", u"Enemies fall to one hit."},
      {"cheat_instant_charge", u"Charged attacks without the wait."},
      {"skip_intro", u"Straight to the title screen."},
      {"modern_camera", u"The camera of Master Edition."},
      {"free_camera_ot_ut", u"Move the camera during Obliteration and Ultimate Techniques."},
      {"more_blood", u"Twice the blood sprays and stains."},
      {"achievement_notifications", u"An unlocked achievement shown in the bottom right corner."},
      {"fullscreen", u"Borderless and Fullscreen cover the whole screen."},
      {"cheat_unlock_difficulties", u"Every difficulty in New Game and Mission Mode."},
      {"cheat_unlock_costumes", u"Every costume to choose from."},
      {"cheat_all_skulls", u"All 30 crystal skulls count as found."},
      {"cheat_max_health_ninpo", u"Life and ninpo upgrades full from the start."},
      {"skip_cutscenes", u"Any cutscene can be skipped, even unseen ones."},
      {"skip_death_screen", u"Straight to the game over menu after dying."},
      {"auto_chapter_screen", u"Chapter cards and mission briefings go on by themselves."},
      {"hud_mode", u"Fade Out hides the HUD while you aren't fighting."},
      {"post_effects", u"The game's blur and white flashes."},
      {"texture_lod_bias", u"Lower: sharper distant textures, with more shimmer."},
      {"smooth_hud", u"The HUD's and menus' pictures always filtered: softer when scaled."},
      {"hires_font", u"Text drawn from glyphs twice the size. Takes effect after a restart."},
  };
  for (const auto& [key, note] : kNotes) {
    if (key == name) {
      return note;
    }
  }
  return {};
}

ng2::panel::State g_panelStates[std::size(kPages)];
// (on the title: the game's rows' places, its arrows and cursor mark)
constexpr ng2::panel::Layout kTitleLayout = [] {
  ng2::panel::Layout layout;
  layout.top = 145.0f, layout.pitch = 44.0f, layout.shown = 10;
  layout.left = 457.0f, layout.value = 916.0f, layout.valueCenter = true;
  return layout;
}();
// (in the in-game list: on the categories' lines)
constexpr ng2::panel::Layout kInGameLayout = [] {
  ng2::panel::Layout layout;
  layout.top = 143.0f, layout.pitch = 41.0f, layout.shown = 11;
  layout.left = 457.0f, layout.value = 888.0f, layout.valueCenter = true;
  return layout;
}();

// (in game: each value between text arrows, as the game's rows had them;
// the title's arrow pictures can't load in play, docs/menus.md)
std::vector<ng2::panel::Row> PanelRows(const Page& page, bool inGame = false) {
  std::vector<ng2::panel::Row> rows;
  for (uint32_t i = 0; i < page.count; i++) {
    const Setting& setting = kSettings[page.rows[i].setting];
    std::u16string value(Choice(setting, ValueOf(setting)));
    if (inGame) {
      value = u"‹  " + value + u"  ›";
    }
    rows.push_back({ng2::panel::RowKind::kChoice, std::u16string(setting.label), std::move(value),
                    std::u16string(NoteOf(setting.name))});
  }
  rows.push_back({ng2::panel::RowKind::kButton, std::u16string(kRestoreText), {},
                  u"Every setting on this page back to its default."});
  if (page.page == kCheatsPage && !inGame) {  // (locked again on the title only)
    rows.push_back({ng2::panel::RowKind::kButton, u"Lock Cheats", {},
                    u"Every cheat off and locked again: achievements back on."});
  }
  return rows;
}

// Rows for this frame (focused: the cursor is in them), and extra texts;
// on the title with the game's arrows and cursor mark.
void ShowRows(const std::vector<ng2::panel::Row>& rows, const ng2::panel::State& state, bool focused,
              uint32_t layer, const ng2::panel::Layout& layout, std::vector<ng2::overlay::Text> texts = {}) {
  std::vector<ng2::panel::Line> lines;
  ng2::panel::Draw(texts, state, rows, focused, layout, &lines);
  std::vector<ng2::overlay::Sprite> sprites;
  if (!layer && g_arrowList && ng2::guest::Load32(g_arrowList + kArrowPicture) && g_titleObject[0]) {
    // The game's pictures, copied from an arrow list (its arrows, at +-32
    // from their own places, the first row's) and the Graphics page (its
    // cursor mark, at the first row's label): moved down to each line.
    constexpr uint32_t kRightArrow = 0x50, kLeftArrow = 0xE0, kPageMark = 0x20;
    constexpr float kArrowShift = 32.0f, kDimArrow = 0.45f;
    for (const ng2::panel::Line& line : lines) {
      float down = line.y - kTitleLayout.top;
      bool lit = focused && line.row == state.row;
      if (rows[size_t(line.row)].kind == ng2::panel::RowKind::kChoice) {
        sprites.push_back({g_arrowList + kRightArrow, kArrowShift, down, true, lit ? 1.0f : kDimArrow, true});
        sprites.push_back({g_arrowList + kLeftArrow, -kArrowShift, down, true, lit ? 1.0f : kDimArrow, true});
      }
      if (lit) {
        sprites.push_back({g_titleObject[0] + kPageMark, 0.0f, down, true});
      }
    }
  }
  ng2::overlay::SetPanel(std::move(texts), layer, layer != 0, std::move(sprites));
}

// The cheats' question (the title's Cheats page while they're locked): its
// two lines, then Yes and No (No picked first).
ng2::panel::State g_cheatsQuestion{1};  // (No)
constexpr ng2::panel::Layout kQuestionLayout = [] {
  ng2::panel::Layout layout = kTitleLayout;
  layout.top = kTitleLayout.top + 3 * kTitleLayout.pitch;
  return layout;
}();
std::vector<ng2::panel::Row> QuestionRows() {
  return {{ng2::panel::RowKind::kButton, u"Yes", {}, {}}, {ng2::panel::RowKind::kButton, u"No", {}, {}}};
}
void ShowCheatsQuestion(bool focused) {
  std::vector<ng2::overlay::Text> texts;
  float shade = focused ? 1.0f : 0.5f;
  texts.push_back({kTitleLayout.left, kTitleLayout.top, u"Do you want to use cheats?", shade});
  float y = kTitleLayout.top + kTitleLayout.pitch;
  for (std::u16string& line : ng2::panel::Wrap(u"Achievements are turned off while cheats are unlocked.", 40)) {
    texts.push_back({kTitleLayout.left, y, std::move(line), shade * 0.8f});
    y += 0.75f * kTitleLayout.pitch;
  }
  ShowRows(QuestionRows(), g_cheatsQuestion, focused, 0, kQuestionLayout, std::move(texts));
}

// The page's texts for this frame (focused: the cursor is in it).
void ShowPanel(const Page& page, bool focused, uint32_t layer = 0) {
  if (!layer && page.page == kCheatsPage && !Unlocked()) {
    ShowCheatsQuestion(focused);
    return;
  }
  ShowRows(PanelRows(page, layer != 0), g_panelStates[&page - kPages], focused, layer,
           layer ? kInGameLayout : kTitleLayout);
}

// A frame's input for our panel.
ng2::panel::Input PanelInput(PPCContext& ctx, uint8_t* base) {
  using ng2::guest::Load32;
  ng2::panel::Input input;
  input.back = CallGuest(ctx, base, __imp__sub_823E42B0, {kPadState, 0, 0xFF, 0}) != 0;
  uint32_t pad = Load32(kPadState);
  uint32_t player = *ng2::guest::Virtual(kPlayerPad);
  auto direction = [&](uint32_t which) {
    return CallGuest(ctx, base, __imp__sub_823E3828, {pad, player, 1, 0, which}) != 0;
  };
  input.up = direction(0), input.down = direction(1), input.right = direction(2), input.left = direction(3);
  input.confirm = CallGuest(ctx, base, __imp__sub_823E4158, {kPadState, 0, 0, 0}) != 0;
  input.reset = CallGuest(ctx, base, __imp__sub_823E43B0, {kPadState}) != 0;
  input.pointer = ng2::menu_mouse::PointerActive() && ng2::menu_mouse::Pointer(&input.x, &input.y);
  input.click = ng2::menu_mouse::ClickedThisFrame() && ng2::menu_mouse::Pointer(&input.x, &input.y);
  return input;
}

void PlaySound(PPCContext& ctx, uint8_t* base, uint32_t which) {
  CallGuest(ctx, base, __imp__sub_8242AFC0, {which}, LoadFloat(kCursorSound));
}

// The cheats' question's input for a frame; true when it's left (No, Back).
bool RunCheatsQuestion(PPCContext& ctx, uint8_t* base) {
  using ng2::panel::Action;
  Action action = ng2::panel::Run(g_cheatsQuestion, QuestionRows(), PanelInput(ctx, base), kQuestionLayout);
  if (action == Action::kMoved) {
    PlaySound(ctx, base, 2);
  } else if (action == Action::kConfirm && g_cheatsQuestion.row == 0) {
    SetSetting("cheats_unlocked", "true");
    g_cheatsQuestion.row = 1;  // (No first, when asked again)
    PlaySound(ctx, base, 3);
  } else if (action == Action::kConfirm || action == Action::kLeave) {
    g_cheatsQuestion.row = 1;
    return true;
  }
  return false;
}

// The page's input for a frame; true when Back leaves it.
bool RunPanel(PPCContext& ctx, uint8_t* base, const Page& page, const ng2::panel::Layout& layout) {
  if (&layout == &kTitleLayout && page.page == kCheatsPage && !Unlocked()) {
    return RunCheatsQuestion(ctx, base);
  }
  ng2::panel::State& state = g_panelStates[&page - kPages];
  std::vector<ng2::panel::Row> rows = PanelRows(page, &layout == &kInGameLayout);
  auto play = [&](uint32_t which) { PlaySound(ctx, base, which); };
  using ng2::panel::Action;
  Action action = ng2::panel::Run(state, rows, PanelInput(ctx, base), layout);
  uint32_t row = uint32_t(state.row);
  switch (action) {
    case Action::kLeave:
      return true;
    case Action::kMoved:
      play(2);
      break;
    case Action::kPrevious:
    case Action::kNext:
      if (row < page.count) {
        const Setting& setting = kSettings[page.rows[row].setting];
        uint32_t value = ValueOf(setting);
        SetValue(setting, action == Action::kNext ? (value + 1) % setting.count
                                                  : (value + setting.count - 1) % setting.count);
        play(3);
      }
      break;
    case Action::kReset:
      if (row < page.count) {
        RestoreDefault(kSettings[page.rows[row].setting]);
        play(3);
      }
      break;
    case Action::kConfirm:  // Restore Defaults, or Lock Cheats (the next row)
      for (uint32_t i = 0; i < page.count; i++) {
        RestoreDefault(kSettings[page.rows[i].setting]);
      }
      if (row > page.count) {
        SetSetting("cheats_unlocked", "false");
        state.row = 0;
        g_cheatsQuestion.row = 1;
      }
      play(3);
      break;
    case Action::kNone:
      break;
  }
  return false;
}

constexpr float kValueLeft = 770.0f, kValueCenter = 916.0f;

// The Achievements page (achievements.h), after Unleashed Recompiled's
// list: each with its icon (greyed while locked), its name and unlock date,
// its description under them; the unlocked first, the latest first. The
// count under the list, and that cheats keep them off while unlocked.
ng2::panel::State g_achievementsState;
ng2::panel::Layout AchievementsLayout(bool inGame) {
  ng2::panel::Layout layout = inGame ? kInGameLayout : kTitleLayout;
  layout.top = 165.0f, layout.pitch = 70.0f, layout.shown = 6;
  return layout;
}
std::vector<ng2::panel::Row> AchievementRows(const std::vector<ng2::achievements::Entry>& entries) {
  std::vector<ng2::panel::Row> rows;
  for (const auto& entry : entries) {
    rows.push_back({ng2::panel::RowKind::kButton, entry.name, {}, {}});
  }
  return rows;
}
// The line of the row picked (its center's y), for the in-game list's mark.
float g_achievementsMarkY = 0.0f;
void ShowAchievements(bool focused, uint32_t layer) {
  namespace ach = ng2::achievements;
  std::vector<ach::Entry> entries = ach::List();
  ng2::panel::Layout layout = AchievementsLayout(layer != 0);
  ng2::panel::State state = g_achievementsState;
  int count = int(entries.size());
  state.row = std::clamp(state.row, 0, std::max(count - 1, 0));
  state.scroll = std::clamp(state.scroll, 0, std::max(count - layout.shown, 0));
  std::vector<ng2::overlay::Text> texts;
  std::vector<ng2::overlay::Sprite> sprites;
  std::vector<ach::Icon> icons;
  constexpr float kIcon = 56.0f, kTextShift = 72.0f, kDateX = 920.0f;
  for (int i = state.scroll; i < count && i < state.scroll + layout.shown; i++) {
    const ach::Entry& entry = entries[size_t(i)];
    float y = layout.top + layout.pitch * float(i - state.scroll);
    bool lit = focused && i == state.row;
    float dim = entry.unlocked ? 1.0f : 0.6f;
    if (entry.icon) {
      icons.push_back({entry.icon, layout.left, y - 0.5f * kIcon, kIcon, entry.unlocked ? 1.0f : 0.5f,
                       !entry.unlocked});
    }
    float textX = layout.left + kTextShift;
    texts.push_back({textX, y - 17.0f, entry.name, (lit ? 1.0f : 0.6f) * dim});
    if (entry.unlocked) {
      texts.push_back({kDateX, y - 17.0f, ach::DateText(entry.unlockedAt), lit ? 0.8f : 0.5f, 0x16});
    }
    float lineY = y + 9.0f;
    std::vector<std::u16string> lines = ng2::panel::Wrap(entry.description, 52);
    for (size_t k = 0; k < lines.size() && k < 2; k++, lineY += 21.0f) {
      texts.push_back({textX, lineY, lines[k], (lit ? 0.85f : 0.5f) * dim, 0x16});
    }
    if (lit) {
      g_achievementsMarkY = y;
      if (!layer && g_titleObject[0]) {
        sprites.push_back({g_titleObject[0] + 0x20, 0.0f, y - kTitleLayout.top, true});
      }
    }
  }
  // The scroll bar right of the dates (the game's chrome on its dark panel),
  // its thumb as long as the share of the list shown, where it is.
  if (count > layout.shown) {
    constexpr float kBarX = 1126.0f, kBarWidth = 10.0f, kThumbWidth = 14.0f;
    float top = layout.top - 32.0f, height = layout.pitch * float(layout.shown - 1) + 64.0f;
    float thumb = std::max(28.0f, height * float(layout.shown) / float(count));
    float thumbY = top + (height - thumb) * float(state.scroll) / float(count - layout.shown);
    float alpha = focused ? 1.0f : 0.6f;
    if (uint32_t track = ach::ScrollTrack()) {
      ach::Icon icon{track, kBarX, top, kBarWidth, 0.55f * alpha, false};
      icon.height = height;
      icons.push_back(icon);
    }
    if (uint32_t knob = ach::ScrollThumb()) {
      ach::Icon icon{knob, kBarX + 0.5f * (kBarWidth - kThumbWidth), thumbY, kThumbWidth, alpha, false};
      icon.height = thumb;
      icons.push_back(icon);
    }
  }
  if (count == 0) {
    texts.push_back({layout.left, layout.top, u"No achievements found in the game's files.", 0.6f});
  } else {
    std::string total = fmt::format("{} / {} unlocked", ach::UnlockedCount(), ach::Total());
    float shade = focused ? 0.8f : 0.5f;
    texts.push_back({layout.left, layout.noteY - 12.0f, std::u16string(total.begin(), total.end()), shade, 0x18});
    if (Unlocked()) {
      texts.push_back({layout.left, layout.noteY + 12.0f, u"Off while cheats are unlocked.", shade, 0x18});
    }
  }
  ng2::achievements::SetPageIcons(std::move(icons));
  ng2::overlay::SetPanel(std::move(texts), layer, layer != 0, std::move(sprites));
}

// The Achievements page's input for a frame; true when Back leaves it.
bool RunAchievements(PPCContext& ctx, uint8_t* base, bool inGame) {
  using ng2::panel::Action;
  std::vector<ng2::panel::Row> rows = AchievementRows(ng2::achievements::List());
  Action action = ng2::panel::Run(g_achievementsState, rows, PanelInput(ctx, base), AchievementsLayout(inGame));
  if (action == Action::kMoved) {
    PlaySound(ctx, base, 2);
  }
  return action == Action::kLeave;
}

// A page (focused: the cursor is in it), and its input: the Controls table,
// the achievements, or our panel's settings.
void ShowPage(const Page& page, bool focused, uint32_t layer = 0) {
  if (page.custom) {
    ShowControls(focused, layer);
  } else if (page.achievements) {
    ShowAchievements(focused, layer);
  } else {
    ShowPanel(page, focused, layer);
  }
}
bool RunPageInput(PPCContext& ctx, uint8_t* base, const Page& page, bool inGame) {
  return page.custom         ? RunControls(ctx, base)
         : page.achievements ? RunAchievements(ctx, base, inGame)
                             : RunPanel(ctx, base, page, inGame ? kInGameLayout : kTitleLayout);
}

void RunPage(PPCContext& ctx, uint8_t* base, const Page& page) {
  using ng2::guest::Load32;
  ArrowPictures(ctx, base);
  uint32_t number = ViewOf(page);  // (the page shown)
  // The Controls table or our panel, each with its own input.
  bool leave = RunPageInput(ctx, base, page, false);
  ShowPage(page, !leave);
  if (leave) {
    Store32(kPage, kCategoryPage);
    Store32(kDepth, Load32(kDepth) - 1);
    CallGuest(ctx, base, __imp__sub_836A5AA0, {kLeave, 0});
  }
  if (g_windowChanged) {
    ScheduleWindow();
  }
  CallGuest(ctx, base, __imp__sub_836A4B90,
            {kMenuModel, ShownPage(Load32(kLastPage)), number, kCursors, StoreValues(ctx, &page),
             kPageValues});
}

}  // namespace

// The loop starts with the first category and rows picked, and leaves the
// game's as they were.
REX_EXTERN(__imp__sub_823E59A0);
REX_HOOK_RAW(sub_823E59A0) {
  ng2::menu_mouse::NoteMenuInput();  // (a menu up: the mouse's, menu_mouse.cpp)
  constexpr uint32_t kSaved = 12;  // (pages up to 11)
  uint32_t saved[kSaved];
  for (uint32_t i = 0; i < kSaved; i++) {
    saved[i] = ng2::guest::Load32(kCursors + 4 * i);
  }
  bool ours = g_open;
  if (ours) {
    for (uint32_t i = 0; i < kSaved; i++) {
      Store32(kCursors + 4 * i, 0);
    }
  }
  __imp__sub_823E59A0(ctx, base);
  if (ours) {
    for (uint32_t i = 0; i < kSaved; i++) {
      Store32(kCursors + 4 * i, saved[i]);
    }
  }
}

void GamePagePointer(PPCContext& ctx, uint8_t* base, uint32_t page);

// The category list, replaced while ours is open: our categories only;
// Confirm opens one's page, Back leaves Options.
REX_EXTERN(__imp__sub_823E5D88);
REX_HOOK_RAW(sub_823E5D88) {
  ng2::menu_mouse::NoteMenuInput();  // (a menu up: the mouse's, menu_mouse.cpp)
  if (!g_open) {
    GamePagePointer(ctx, base, 0);
    __imp__sub_823E5D88(ctx, base);
    return;
  }
  ArrowPictures(ctx, base);
  using ng2::guest::Load32;
  uint32_t page = Load32(kPage);
  uint32_t cursor = std::min(Load32(kCursors), kPageCount - 1);
  uint32_t enabled = ctx.r1.u32 - 0xC0;
  for (uint32_t i = 0; i < 8; i++) {
    Store32(enabled + 4 * i, i < kPageCount);
  }
  // The pointer on a category: that one (before Confirm, which a click is);
  // a click on none confirms nothing.
  constexpr float kCategoryLeft = 90.0f, kCategoryRight = 340.0f, kCategoryTop = 145.0f, kCategoryPitch = 60.0f;
  int pointedCategory = PointedRow(kCategoryLeft, kCategoryRight, kCategoryTop, kCategoryPitch, int(kPageCount));
  bool clickOff = ng2::menu_mouse::ClickedThisFrame() && pointedCategory < 0;
  if (ng2::menu_mouse::PointerActive() && pointedCategory >= 0 && uint32_t(pointedCategory) != cursor) {
    CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, LoadFloat(kCursorSound));
    cursor = uint32_t(pointedCategory);
    Store32(kCursors, cursor);
  }
  if (!clickOff && CallGuest(ctx, base, __imp__sub_823E4158, {kPadState, cursor, enabled, 0})) {
    Store32(kPage, kPages[cursor].page);
    g_titleCategory = cursor;
    Store32(kDepth, Load32(kDepth) + 1);
    CallGuest(ctx, base, __imp__sub_836A5AA0, {kEnter, 0});
  } else if (CallGuest(ctx, base, __imp__sub_823E42B0, {kPadState, cursor, 7, 0})) {
    Store32(kPage, kClosed);
    CallGuest(ctx, base, __imp__sub_836A5AA0, {kLeave, 0});
  } else {
    uint32_t pad = Load32(kPadState);
    uint32_t player = *ng2::guest::Virtual(kPlayerPad);
    auto pressed = [&](uint32_t direction) {
      return CallGuest(ctx, base, __imp__sub_823E3828, {pad, player, 1, 0, direction}) != 0;
    };
    uint32_t moved = cursor;
    if (pressed(0)) {
      moved = (moved + kPageCount - 1) % kPageCount;
    }
    if (pressed(1)) {
      moved = (moved + 1) % kPageCount;
    }
    if (moved != cursor) {
      CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, LoadFloat(kCursorSound));
    }
    Store32(kCursors, moved);
    ShowPage(kPages[moved], false);
  }
  CallGuest(ctx, base, __imp__sub_836A4B90,
            {kMenuModel, ShownPage(Load32(kLastPage)), ShownPage(page), kCursors, StoreValues(ctx, nullptr), 8});
}

// The pages' builders.
REX_EXTERN(__imp__sub_836D14C0);
REX_HOOK_RAW(sub_836D14C0) {
  Build(ctx, base, __imp__sub_836D14C0, 1);  // Game Settings
}
REX_EXTERN(__imp__sub_836D2078);
REX_HOOK_RAW(sub_836D2078) {
  Build(ctx, base, __imp__sub_836D2078, 2);  // Controller Settings
}
REX_EXTERN(__imp__sub_836D2B10);
REX_HOOK_RAW(sub_836D2B10) {
  Build(ctx, base, __imp__sub_836D2B10, 3);  // Camera Settings
}
REX_EXTERN(__imp__sub_836D43C8);
REX_HOOK_RAW(sub_836D43C8) {
  Build(ctx, base, __imp__sub_836D43C8, 4);  // page 4's
}
REX_EXTERN(__imp__sub_836D57B8);
REX_HOOK_RAW(sub_836D57B8) {
  // (also its vtable's slot 12, which the page's teardown calls: only the
  // build, from sub_836CFF00, makes our rows)
  constexpr uint32_t kFromBuild = 0x836CFF54;
  if (uint32_t(ctx.lr) != kFromBuild) {
    __imp__sub_836D57B8(ctx, base);
    return;
  }
  Build(ctx, base, __imp__sub_836D57B8, 6);  // Copyright Information
}
// The Copyright Information page's own methods (its vtable 0x82286EC0,
// slots 13-15: its text, drawn and scrolled): while it shows ours, those of
// a plain page (0x82286F78's: 1, nothing, 0).
REX_EXTERN(__imp__sub_836D57C0);
REX_HOOK_RAW(sub_836D57C0) {
  if (g_open) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_836D57C0(ctx, base);
}
REX_EXTERN(__imp__sub_836D5898);
REX_HOOK_RAW(sub_836D5898) {
  if (!g_open) {
    __imp__sub_836D5898(ctx, base);
  }
}
REX_EXTERN(__imp__sub_836D5970);
REX_HOOK_RAW(sub_836D5970) {
  if (g_open) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_836D5970(ctx, base);
}
REX_EXTERN(__imp__sub_836D54F8);
REX_HOOK_RAW(sub_836D54F8) {
  Build(ctx, base, __imp__sub_836D54F8, 5);  // Audio Settings
}

// A choice or a row from a builder whose rows are left out.
REX_HOOK_RAW(sub_836CF1A8) {
  if (!InBuild(ctx.r3.u32)) {
    __imp__sub_836CF1A8(ctx, base);
  }
}
REX_HOOK_RAW(sub_836D03E8) {
  if (!InBuild(ctx.r3.u32)) {
    __imp__sub_836D03E8(ctx, base);
  }
}

// The game's own Options (ours not open): the pointer on a row puts the
// page's cursor there, with the menu's cursor sound, before its handler
// runs (which lights the row); a click on no row confirms nothing, and a
// click on the row's value picks what's under it (#163). A row's value is
// in the menu model, as (current, last) words from kOptionsValues + 0x40 *
// page + 8 * row.
constexpr uint32_t kOptionsValues = 0x8532EE30;
enum class Pick : uint8_t {
  kNone,    // no value (Restore Defaults, a row that opens a page)
  kHalves,  // a list or a slider: the value's left half left, its right half right
  kTwo,     // two choices, the left one 0
  kOnOff,   // two choices, the left one 1 (the game's On/Off rows, and a few)
  kFour,    // four choices in two lines of two, 0-3 left to right, top to bottom
};
struct OptionsRow {
  float top, bottom;  // in the 1280x720 UI
  Pick pick;
  float split = 0;  // kFour: between its two lines
};
struct OptionsRows {
  uint32_t page;
  float left, right;
  std::initializer_list<OptionsRow> rows;  // in cursor order ("???" rows: a zero span)
};
constexpr float R(int i) { return 123.0f + 44.0f * float(i); }  // the pages' usual rows
const OptionsRows kOptionsRows[] = {
    {0, 90.0f, 390.0f,  // the category list
     {{115, 175, Pick::kNone}, {175, 235, Pick::kNone}, {235, 295, Pick::kNone}, {295, 355, Pick::kNone},
      {355, 415, Pick::kNone}, {415, 475, Pick::kNone}, {475, 535, Pick::kNone}, {535, 595, Pick::kNone}}},
    {1, 440.0f, 1110.0f,  // Game Settings
     {{R(0), R(1), Pick::kHalves}, {R(1), R(2), Pick::kTwo}, {R(2), R(3), Pick::kOnOff},
      {R(3), R(4), Pick::kOnOff}, {R(4), R(5), Pick::kOnOff}, {R(5), R(6), Pick::kOnOff},
      {R(6), R(7), Pick::kOnOff}, {R(7), R(8), Pick::kNone}}},
    {2, 440.0f, 1110.0f,  // Controller Settings
     {{R(0), R(1), Pick::kTwo}, {R(1), R(3), Pick::kFour, 210}, {R(3), R(4), Pick::kOnOff},
      {R(4), R(5), Pick::kTwo}, {R(5), R(6), Pick::kNone}}},
    {3, 440.0f, 1110.0f,  // Camera Settings
     {{120, 213, Pick::kFour, 158}, {213, 315, Pick::kFour, 268}, {340, 385, Pick::kHalves},
      {385, 430, Pick::kHalves}, {430, 475, Pick::kNone}}},
    {4, 440.0f, 1110.0f,  // Screen Settings
     {{R(0), R(1), Pick::kOnOff}, {R(1), R(2), Pick::kNone}, {0, 0, Pick::kNone}, {R(3), R(4), Pick::kNone}}},
    {5, 440.0f, 1110.0f,  // Audio Settings
     {{R(0), R(1), Pick::kHalves}, {R(1), R(2), Pick::kHalves}, {R(2), R(3), Pick::kHalves},
      {R(3), R(4), Pick::kTwo}, {0, 0, Pick::kNone}, {R(5), R(6), Pick::kNone}}},
};

// Presses still to make for the last click (a four-choice row's steps, one a
// frame), on which page, and the menu frame of the last (dropped when the page
// isn't run for a few frames: Options closed, ours opened).
uint32_t g_optionsSteps = 0, g_optionsStepsPage = 0;
uint64_t g_optionsStepsFrame = 0;

void GamePagePointer(PPCContext& ctx, uint8_t* base, uint32_t page) {
  using ng2::guest::Load32;
  const OptionsRows* rows = nullptr;
  for (const OptionsRows& r : kOptionsRows) {
    if (r.page == page) {
      rows = &r;
    }
  }
  constexpr uint32_t kA = 0x10, kLeft = 0x4, kRight = 0x8;
  auto press = [](uint32_t remove, uint32_t add) {
    for (uint32_t pad = 0; pad < 5; pad++) {  // (the four, then the merged one)
      uint32_t at = kPadPressed + pad * 0x44;
      Store32(at, (Load32(at) & ~remove) | add);
    }
  };
  // Restore Defaults' question (page 7): Yes (783-815) or No (882-908) at
  // y 648-662, its cursor (by the depth, sub_823E82E0) 1 Yes, 0 No; picked
  // with the question's sound (3).
  if (page == 7) {
    const uint32_t cursorAt = kCursors + 4 * std::min(Load32(kDepth), 11u);
    float x, y;
    if (ng2::menu_mouse::Pointer(&x, &y)) {
      int word = y >= 632.0f && y < 678.0f ? (x >= 760.0f && x < 850.0f ? 1 : x >= 850.0f && x < 940.0f ? 0 : -1) : -1;
      if (ng2::menu_mouse::ClickedThisFrame() && word < 0) {
        press(kA, 0);
      }
      if (ng2::menu_mouse::PointerActive() && word >= 0 && uint32_t(word) != Load32(cursorAt)) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {3}, LoadFloat(kCursorSound));
        Store32(cursorAt, uint32_t(word));
      }
    }
    return;
  }
  if (g_optionsSteps && g_optionsStepsPage == page &&
      ng2::menu_mouse::Frame() - g_optionsStepsFrame <= 3) {
    g_optionsSteps--;
    g_optionsStepsFrame = ng2::menu_mouse::Frame();
    press(kA, kRight);  // (a click meanwhile confirms nothing)
    return;
  }
  g_optionsSteps = 0;
  float x = 0, y = 0;
  if (!rows || !ng2::menu_mouse::Pointer(&x, &y)) {
    return;
  }
  int row = -1;
  const OptionsRow* at = rows->rows.begin();
  if (x >= rows->left && x <= rows->right) {
    for (int i = 0; i < int(rows->rows.size()); i++) {
      if (y >= at[i].top && y < at[i].bottom) {
        row = i;
      }
    }
  }
  uint32_t cursorAddress = kCursors + 4 * page;
  if (ng2::menu_mouse::ClickedThisFrame()) {
    if (row < 0) {
      press(kA, 0);
    } else if (page != 0 && x >= kValueLeft && at[row].pick != Pick::kNone) {
      // (the click is the value's, not Confirm; on another row than the
      // cursor's, the cursor goes there below, before the page's handler)
      const OptionsRow& r = at[row];
      uint32_t value = Load32(kOptionsValues + 0x40 * page + 8 * uint32_t(row));
      bool right = x >= kValueCenter;
      uint32_t add = 0;
      if (r.pick == Pick::kHalves) {
        add = right ? kRight : kLeft;
      } else if (r.pick == Pick::kTwo || r.pick == Pick::kOnOff) {
        bool leftShown = value == (r.pick == Pick::kOnOff ? 1u : 0u);
        add = leftShown == right ? kRight : 0;  // (either way toggles)
      } else if (r.pick == Pick::kFour) {
        uint32_t target = (y >= r.split ? 2 : 0) + (right ? 1 : 0);
        uint32_t steps = (target + 4 - (value & 3)) % 4;
        if (steps) {
          add = kRight;
          g_optionsSteps = steps - 1, g_optionsStepsPage = page;
          g_optionsStepsFrame = ng2::menu_mouse::Frame();
        }
      }
      press(kA, add);
    }
  }
  if (ng2::menu_mouse::PointerActive() && row >= 0 && at[row].bottom > at[row].top &&
      uint32_t(row) != Load32(cursorAddress)) {
    CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, LoadFloat(kCursorSound));
    Store32(cursorAddress, uint32_t(row));
  }
}

// The pages' handlers, replaced while ours is open.
void Handle(PPCContext& ctx, uint8_t* base, void (*handler)(PPCContext&, uint8_t*)) {
  ng2::menu_mouse::NoteMenuInput();  // (a menu up: the mouse's, menu_mouse.cpp)
  const Page* page = TitlePage();
  if (g_open && page) {
    RunPage(ctx, base, *page);
  } else {
    if (!g_open) {
      GamePagePointer(ctx, base, ng2::guest::Load32(kPage));
    }
    handler(ctx, base);
  }
}
REX_EXTERN(__imp__sub_823E66B8);
REX_HOOK_RAW(sub_823E66B8) {
  Handle(ctx, base, __imp__sub_823E66B8);
}
REX_EXTERN(__imp__sub_823E6D00);
REX_HOOK_RAW(sub_823E6D00) {
  // (the game's Controller Settings, when it's the game's page: its layout
  // changed there is taken as the Controls page's, rebind.cpp)
  if (!g_open) {
    ng2::rebind::GameControllerPage();
  }
  Handle(ctx, base, __imp__sub_823E6D00);
}
REX_EXTERN(__imp__sub_823E7268);
REX_HOOK_RAW(sub_823E7268) {
  Handle(ctx, base, __imp__sub_823E7268);
}
REX_EXTERN(__imp__sub_823E5FE0);
REX_HOOK_RAW(sub_823E5FE0) {
  Handle(ctx, base, __imp__sub_823E5FE0);
}
REX_EXTERN(__imp__sub_823E7710);
REX_HOOK_RAW(sub_823E7710) {
  Handle(ctx, base, __imp__sub_823E7710);
}
REX_EXTERN(__imp__sub_823E7C40);
REX_HOOK_RAW(sub_823E7C40) {
  Handle(ctx, base, __imp__sub_823E7C40);  // Screen Settings (page 11)
}
REX_EXTERN(__imp__sub_823E82E0);
REX_HOOK_RAW(sub_823E82E0) {
  Handle(ctx, base, __imp__sub_823E82E0);  // Restore Default's question (page 7)
}

// The Options screen, as it opens (sub_836D5D08(screen, model)): its pages
// are made (their builders run, ours among them), and a table of the page
// shown for each category is filled (+8 + 4 * category). While ours is open
// the seventh category's (Restore Default's question, not a page) is the
// page object for Return to Main Menu (+0x81A0, empty and never shown),
// with Controls' rows; the game fills the table again each time it opens.
REX_EXTERN(__imp__sub_836D5D08);
REX_HOOK_RAW(sub_836D5D08) {
  constexpr uint32_t kSpareObject = 0x81A0, kShownTable = 8;
  uint32_t screen = ctx.r3.u32, model = ctx.r4.u32;
  __imp__sub_836D5D08(ctx, base);
  const Page* controls = PageOf(7);
  if (!g_open || !screen || !controls) {
    return;
  }
  uint32_t object = screen + kSpareObject;
  Store32(object + 8, controls->view - 1);
  Store32(object + 12, controls->view);
  BuildPage(ctx, base, *controls, object, model);
  // Every category shows it: our panel draws the pages.
  for (uint32_t category = 0; category < kPageCount; category++) {
    Store32(screen + kShownTable + 4 * category, object);
  }
}

// The button hints under a page (sub_836D6898(&flags): 0x10 Change Storage
// Device, 8 Test Volume, 4 Restore Defaults, 2 Confirm, 1 Back), by the
// game's page and row: on ours, Restore Defaults and Back on every row.
REX_EXTERN(__imp__sub_836D6898);
bool InGameOursShown();
bool InGameConfirmShown();
REX_HOOK_RAW(sub_836D6898) {
  uint32_t flags = ctx.r3.u32;
  bool title = g_open && TitlePage();
  // In the in-game menu the game's hints follow its own rows' slots (Save
  // Method's: Change Storage Device): there, with ours shown, Back, and
  // Confirm where it does something (a category, Restore Defaults).
  bool inGame = !title && InGameOursShown() && !ng2::aspect::HudShown();
  if ((!title && !inGame) || !flags) {
    // (the PC has one storage device: no Change Storage Device hint)
    uint32_t game = flags ? ng2::guest::Load32(flags) : 0;
    if (game & 0x10) {
      Store32(flags, game & ~0x10u);
    }
    __imp__sub_836D6898(ctx, base);
    if (game & 0x10) {
      Store32(flags, game);
    }
    return;
  }
  uint32_t game = ng2::guest::Load32(flags);
  // (the cheats' question: Confirm and Back)
  bool question = title && ng2::guest::Load32(kPage) == kCheatsPage && !Unlocked();
  // (the achievements: Back only)
  bool list = title && TitlePage()->achievements;
  uint32_t ours = question ? 0x3 : list ? 0x1 : title ? 0x5 : InGameConfirmShown() ? 0x3 : 0x1;
  Store32(flags, ours);
  __imp__sub_836D6898(ctx, base);
  Store32(flags, game);
}

// The hints' and description's selector (sub_836D6AC8(view, model)): the
// description is the game's for its page and row (a table of text ids by
// page * 8 + row), and the Game Settings page's rows 5 and 6 get Change
// Storage Device before the hints above. On ours: an empty description
// (our panel shows each row's note), through the text lookup's hook
// (storage_prompt.cpp).
void SetDescriptionText(uint32_t id);  // storage_prompt.cpp; 0: the game's
REX_EXTERN(__imp__sub_836D6AC8);
REX_HOOK_RAW(sub_836D6AC8) {
  using ng2::guest::Load32;
  uint32_t model = ctx.r4.u32;
  const Page* page = g_open && model ? TitlePage() : nullptr;
  if (!page) {
    __imp__sub_836D6AC8(ctx, base);
    return;
  }
  // (none of the game's: our panel shows its notes)
  constexpr uint32_t kEmpty = kTable | 0x34;
  SetDescriptionText(kEmpty);
  uint32_t shown = Load32(model);
  if (shown == 1) {
    Store32(model, 2);
  }
  __imp__sub_836D6AC8(ctx, base);
  Store32(model, shown);
  SetDescriptionText(0);
}

// The Game Settings page's play time, formatted each frame into a shared
// string: blank on ours.
REX_EXTERN(__imp__sub_836D0E90);
REX_HOOK_RAW(sub_836D0E90) {
  __imp__sub_836D0E90(ctx, base);
  if (g_open) {
    Store32(kPlayTimeText, 0);
  }
}

bool ng2::recomp_settings::CheatsUnlocked() {
  return g_cheatsUnlocked.load(std::memory_order_relaxed);
}

// Each frame (perf_overlay.cpp): a key's fullscreen choice, and a window
// position once it's been still for a second, written to the settings.
void ng2::recomp_settings::WindowFrame() {
  g_cheatsUnlocked.store(Unlocked(), std::memory_order_relaxed);
  // The Controls' settings changed outside our menus (the game's own
  // Controller Settings): saved, once for all of them.
  if (g_controlsToSave && !g_open && !g_inGameOurs && !g_configPath.empty()) {
    SaveSettings();
    g_written.clear();
    g_controlsToSave = false;
  }
  bool save = false;
  if (int to = g_toggledTo.exchange(-1); to >= 0) {
    g_written["fullscreen"] = to ? "true" : "false";
    save = true;
  }
  int64_t at = g_movedAt.load();
  if (at && NowMs() - at > 1000 && g_movedAt.compare_exchange_strong(at, 0) &&
      !rex::cvar::Query<bool>("fullscreen")) {
    std::string x = std::to_string(g_movedX.load()), y = std::to_string(g_movedY.load());
    if (GetSetting("windowed_x") != x || GetSetting("windowed_y") != y) {
      rex::cvar::SetFlagByName("windowed_x", x);
      rex::cvar::SetFlagByName("windowed_y", y);
      g_written["windowed_x"] = x;
      g_written["windowed_y"] = y;
      save = true;
    }
  }
  if (save && !g_configPath.empty()) {
    SaveSettings();
  }
}

void ng2::recomp_settings::ApplyWindowAtStart() {
  SDL_GetEventFilter(&g_previousFilter, &g_previousFilterData);
  SDL_SetEventFilter(FilterWindowEvents, nullptr);
  // vsync = false from older builds' V-Sync Off: it is also the SDK's guest
  // vblank switch (off: 1000 Hz), so it becomes present_mode 0 and vsync on.
  if (GetSetting("vsync") == "false") {
    if (rex::cvar::GetFlagSource("present_mode") == rex::cvar::Source::kDefault) {
      SetSetting("present_mode", "0");
    }
    SetSetting("vsync", "true");
    if (!g_configPath.empty()) {
      SaveSettings();
    }
    g_written.clear();
  }
  // The runtime opens the window fullscreen as a borderless window, or at its
  // default size: the exclusive mode or our windowed size is set over it.
  bool fullscreen = rex::cvar::Query<bool>("fullscreen");
  if (g_appContext && (fullscreen ? rex::cvar::Query<bool>(kExclusive)
                                  : rex::cvar::Query<int32_t>("windowed_width") > 0 ||
                                        rex::cvar::Query<int32_t>("windowed_x") != kNoPosition)) {
    g_appContext->CallInUIThread([fullscreen] { ApplyWindow(fullscreen); });
  }
}

// The in-game list's places in the 1280x720 UI (the pointer picks a row):
// the categories' column (ours) or the game's rows, the page's column, a
// value's place.
constexpr float kInGameLeft = 90.0f, kInGameCategoryRight = 380.0f;
constexpr float kInGamePageLeft = 440.0f;
constexpr float kInGameTop = 143.0f, kInGamePitch = 41.0f;

// RECOMP SETTINGS in gameplay: the Back button's "IN-GAME MANUAL AND OPTIONS"
// list (docs/menus.md) gets a RECOMP SETTINGS row after its twelve. Choosing
// it lays the list out as the title's RECOMP SETTINGS screen until Back: the
// categories on the left (Cheats only once unlocked), the chosen one's page
// on the right drawn by our panel ("‹ value ›" in the game's font where the
// title has arrow pictures), and the header redrawn as RECOMP SETTINGS. The
// list's twelve rows then hold the categories and empty rows. The list's own methods walk its twelve rows, so the extra row is
// added to them by the hooks below.
namespace {

constexpr uint32_t kInGameState = 0x83D0176C;    // 0: the list
constexpr uint32_t kInGameCursors = 0x83D0177C;  // + 4 * state
constexpr uint32_t kInGameModel = 0x853265E0;    // cursor, last cursor
constexpr uint32_t kInGameViews[] = {0x85326650, 0x853268A0, 0x8532B800};  // told of moves
constexpr uint32_t kGameRows = 12;
constexpr uint32_t kEntryIndex = 12;   // our row's index, as the game's handler sees it
constexpr uint32_t kEntryShownAt = 10;   // where it's shown: after Save Method

// Our row is shown after Save Method, but the game's handler and the list
// keep their own indices (10 Quit Game, 11 Return to Game; ours is 12):
// only moving up and down goes by the order shown.
uint32_t ToShown(uint32_t index) {
  return index < kEntryShownAt ? index : index == kEntryIndex ? kEntryShownAt : index + 1;
}
uint32_t ToGameIndex(uint32_t shown) {
  return shown < kEntryShownAt ? shown : shown == kEntryShownAt ? kEntryIndex : shown - 1;
}
constexpr uint32_t kRowsAt = 8;                 // the list's row pointers
constexpr uint32_t kLabelRowSize = 0x130;
constexpr uint32_t kLabelRowVtable = 0x82286618;  // Return to Game's class
constexpr uint32_t kQuitRowVtable = 0x82286660;   // Quit Game's
constexpr uint32_t kRowLabel = 0xE4;              // a row's text
constexpr uint32_t kRowHeight = 0x18;
constexpr uint32_t kLineHeight = 0x8218243C;      // the float the builder gives rows
constexpr uint32_t kGuestAlloc = 0xB2800000;

constexpr std::u16string_view kEntryText = u"Recomp Settings";
// Our layout in the list's rows: the categories, then empty rows for the
// page's column (our panel, or the Controls table, is drawn there), the
// 13th slot our entry's.
constexpr uint32_t kFirstPageRow = kPageCount;
constexpr uint32_t kPageSlots = kGameRows - kFirstPageRow;
constexpr uint32_t kRestoreSlot = kGameRows;
constexpr uint32_t kEmptyRows = kPageSlots + 2;  // (and one for a hidden category's slot)
constexpr float kPageColumn = 344.0f;  // the title's pages are this far right of its categories
constexpr uint32_t kCursorMark = 0x4ED0, kCursorMarkSlot = 0x4EC0;  // in the list
constexpr uint32_t kMarkX = 0x30, kMarkY = 0x34;  // the mark's place, from the list's
constexpr uint32_t kMarkOrigin = 0x8228A1C8;     // the float the list places it from
uint32_t g_list = 0;            // the list object, once built
uint32_t g_arena = 0, g_arenaUsed = 0, g_arenaSize = 0;
uint32_t g_entry = 0;           // our row in the list
uint32_t g_categoryRows[kPageCount] = {}, g_emptyRows[kEmptyRows] = {};
uint32_t g_category = 0;       // the category shown
bool g_inPage = false;         // the cursor is in its settings (else on the categories)
uint32_t g_gameRows[kGameRows] = {};
uint32_t g_suppress = 0;        // directions (bit per sub_823E3828 direction) and Confirm (bit 4)

// The categories in the in-game list: Cheats only once unlocked (NGB's
// way), the others in their order; a category's slot is its place there.
bool CategoryShown(uint32_t category) {
  return kPages[category].page != kCheatsPage || Unlocked();
}
uint32_t ShownCategories() {
  uint32_t count = 0;
  for (uint32_t c = 0; c < kPageCount; c++) {
    count += CategoryShown(c);
  }
  return count;
}
uint32_t SlotOfCategory(uint32_t category) {
  uint32_t slot = 0;
  for (uint32_t c = 0; c < category; c++) {
    slot += CategoryShown(c);
  }
  return slot;
}
uint32_t CategoryAtSlot(uint32_t slot) {
  for (uint32_t c = 0; c < kPageCount; c++) {
    if (CategoryShown(c) && slot-- == 0) {
      return c;
    }
  }
  return kPageCount;
}

uint32_t ArenaAlloc(uint32_t size) {
  size = (size + 15) & ~15u;
  if (g_arenaUsed + size > g_arenaSize) {
    return 0;
  }
  uint32_t at = g_arena + g_arenaUsed;
  g_arenaUsed += size;
  return at;
}

// A UTF-16 string in guest memory, with room for chars more.
uint32_t GuestText(std::u16string_view text, uint32_t room = 0) {
  uint32_t chars = uint32_t(std::max<size_t>(text.size(), room)) + 1;
  uint32_t at = ArenaAlloc(chars * 2);
  if (at) {
    std::vector<uint8_t> bytes = Utf16BE(text);
    std::memset(ng2::guest::Virtual(at), 0, chars * 2);
    std::memcpy(ng2::guest::Virtual(at), bytes.data(), bytes.size());
  }
  return at;
}


}  // namespace

REX_EXTERN(__imp__sub_83835960);  // XMemAlloc(size, attributes)
REX_EXTERN(__imp__sub_83691298);  // a choice row's constructor
REX_EXTERN(__imp__sub_83692EC0);  // a choice row's setup: (row, label id, ids, count, reversed)
REX_EXTERN(__imp__sub_83691458);  // a row's value: (row, value)
REX_EXTERN(__imp__sub_83692378);  // an event to a view: (view, event, model)

namespace {

// A guest function by address (a vtable's), as generated code calls them.
void CallAt(PPCContext& ctx, uint8_t* base, uint32_t address, std::initializer_list<uint64_t> args) {
  PPCContext saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  uint64_t* registers[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
  size_t i = 0;
  for (uint64_t arg : args) {
    *registers[i++] = arg;
  }
  rex::runtime::ResolveIndirectFunction(address)(ctx, base);
  ctx = saved;
}

uint32_t Method(uint32_t object, uint32_t slot) {
  return ng2::guest::Load32(ng2::guest::Load32(object) + 4 * slot);
}

// A row as the list's builder sets one up: index, init, line height.
void InitRow(PPCContext& ctx, uint8_t* base, uint32_t row, uint32_t index, bool empty = false) {
  Store32(row + 8, index);
  Store32(row + 12, 0);
  Store32(row + 4, 0);
  CallAt(ctx, base, Method(row, 0), {row});
  Store32(row + 0x14, 0x1C);
  StoreFloat(row + kRowHeight, empty ? 0.0f : LoadFloat(kLineHeight));
}

// A label row, as the list's constructor makes Return to Game.
void MakeLabelRow(PPCContext& ctx, uint8_t* base, uint32_t row) {
  std::memset(ng2::guest::Virtual(row), 0, kLabelRowSize);
  Store32(row, kLabelRowVtable);
  CallGuest(ctx, base, __imp__sub_83704630, {row + 0x20});
  CallGuest(ctx, base, __imp__sub_8242C048, {row + 0xB0});
  Store32(row + 0x110, 1);
}



// Our rows and their texts, made once.
uint32_t g_entryText = 0, g_emptyText = 0;
uint32_t g_categoryText[kPageCount] = {};

bool MakeInGameRowsOnce(PPCContext& ctx, uint8_t* base) {
  if (g_arena) {
    return true;
  }
  // The rows, then their texts (0x2000 is plenty for the labels and values).
  constexpr uint32_t kRowsSize = (1 + kPageCount + kEmptyRows) * kLabelRowSize;
  constexpr uint32_t kSize = (kRowsSize + 0x2000 + 0xFFF) & ~0xFFFu;
  uint32_t arena = CallGuest(ctx, base, __imp__sub_83835960, {kSize, kGuestAlloc});
  if (!arena) {
    return false;
  }
  std::memset(ng2::guest::Virtual(arena), 0, kSize);
  g_arena = arena, g_arenaSize = kSize, g_arenaUsed = 0;
  g_entry = ArenaAlloc(kLabelRowSize);
  for (uint32_t& row : g_categoryRows) {
    row = ArenaAlloc(kLabelRowSize);
  }
  for (uint32_t& row : g_emptyRows) {
    row = ArenaAlloc(kLabelRowSize);
  }
  g_entryText = GuestText(kEntryText);
  g_emptyText = GuestText(u"");
  for (uint32_t i = 0; i < kPageCount; i++) {
    g_categoryText[i] = GuestText(kPages[i].name);
  }
  if (!g_emptyText) {
    REXLOG_WARN("recomp settings: no room for the in-game rows");
    return false;
  }
  MakeLabelRow(ctx, base, g_entry);
  for (uint32_t row : g_categoryRows) {
    MakeLabelRow(ctx, base, row);
  }
  for (uint32_t row : g_emptyRows) {
    MakeLabelRow(ctx, base, row);
  }
  return true;
}


// Our rows set up again, as the game's builder does its own each time.
bool SetUpInGameRows(PPCContext& ctx, uint8_t* base) {
  if (!MakeInGameRowsOnce(ctx, base)) {
    return false;
  }
  InitRow(ctx, base, g_entry, kEntryIndex);
  Store32(g_entry + kRowLabel, g_entryText);
  for (uint32_t i = 0; i < kPageCount; i++) {
    InitRow(ctx, base, g_categoryRows[i], i);
    Store32(g_categoryRows[i] + kRowLabel, g_categoryText[i]);
  }
  return true;
}





bool g_inGameShownOrder = false;  // the game's rows placed with ours among them

void SendCursorEvent(PPCContext& ctx, uint8_t* base, uint32_t view) {
  CallGuest(ctx, base, __imp__sub_83692378, {view, 2, kInGameModel});
}

// Tells the list's views the cursor moved (as the game's handler does).
void NotifyCursor(PPCContext& ctx, uint8_t* base, uint32_t cursor) {
  uint32_t last = ng2::guest::Load32(kInGameModel);
  if (last == cursor) {
    return;
  }
  Store32(kInGameModel + 4, last);
  Store32(kInGameModel, cursor);
  for (uint32_t view : kInGameViews) {
    SendCursorEvent(ctx, base, view);
  }
}


void SetListRows(const uint32_t* rows, uint32_t count = kGameRows) {
  for (uint32_t i = 0; i < count; i++) {
    Store32(g_list + kRowsAt + 4 * i, rows[i]);
  }
}


// The game's rows in their own slots (other code finds Quit Game's by
// index), ours in the 13th.
void ArrangeGameRows() {
  SetListRows(g_gameRows);
  Store32(g_list + kRowsAt + 4 * kGameRows, g_entry);
}

// A row is drawn at the place its index (set when it's set up) gives it:
// ours and the two shown after it get those places.
void PlaceGameRows(PPCContext& ctx, uint8_t* base) {
  for (uint32_t shown = kEntryShownAt; shown <= kGameRows; shown++) {
    uint32_t index = ToGameIndex(shown);
    uint32_t row = index == kEntryIndex ? g_entry : g_gameRows[index];
    // Its texts, which the builder set after the setup: Quit Game's class
    // has its label at +0xF4 and its question's texts at +0x20..+0x28.
    bool quit = ng2::guest::Load32(row) == kQuitRowVtable;
    std::vector<uint32_t> keep = quit ? std::vector<uint32_t>{0x20, 0x24, 0x28, 0xF4}
                                      : std::vector<uint32_t>{kRowLabel};
    std::vector<uint32_t> values;
    for (uint32_t offset : keep) {
      values.push_back(ng2::guest::Load32(row + offset));
    }
    InitRow(ctx, base, row, shown);
    for (size_t i = 0; i < keep.size(); i++) {
      Store32(row + keep[i], values[i]);
    }
  }
}

}  // namespace

REX_EXTERN(__imp__sub_83695800);  // the list's event 1, to all its rows: (list, model)

namespace {




// Our layout in the list, with a category's page from its line scroll
// (its rows set up again for the slots they're shown in); or the game's
// rows back. Moving between categories or scrolling leaves the category
// rows as they are, as the title's list.
void ShowInGameRows(PPCContext& ctx, uint8_t* base, bool ours, uint32_t category = 0,
                    bool categoriesShown = false) {
  if (!ours) {
    ArrangeGameRows();
    g_inGameOurs = false;
    ng2::video::SetInGameRecompHeader(false);
    return;
  }
  g_category = CategoryShown(category) ? category : CategoryAtSlot(0);
  uint32_t rows[kRestoreSlot + 1];
  for (uint32_t i = 0; i < kPageCount; i++) {
    uint32_t shown = CategoryAtSlot(i);
    rows[i] = shown < kPageCount ? g_categoryRows[shown] : g_emptyRows[kPageSlots + 1];
    if (!categoriesShown) {
      // (each at its slot: a hidden one moves those after it up)
      InitRow(ctx, base, rows[i], i, shown == kPageCount);
      Store32(rows[i] + kRowLabel, shown < kPageCount ? g_categoryText[shown] : g_emptyText);
    }
  }
  // The page's own slots empty: our panel draws it (or the Controls table).
  for (uint32_t s = 0; s <= kPageSlots; s++) {
    uint32_t slot = kFirstPageRow + s, row = g_emptyRows[s];
    InitRow(ctx, base, row, slot, true);
    Store32(row + kRowLabel, g_emptyText);
    rows[slot] = row;
  }
  SetListRows(rows, kRestoreSlot + 1);
  g_inGameOurs = true;
  ng2::video::SetInGameRecompHeader(true);
  // Event 1 (as the list's rows get when it opens), to ours only: the
  // list's own sends it to its view too, which closes the menu.
  for (uint32_t i = categoriesShown ? kFirstPageRow : 0; i <= kRestoreSlot; i++) {
    CallGuest(ctx, base, __imp__sub_83692378, {rows[i], 1, kInGameModel});
  }
}

void LeaveInGameRows(PPCContext& ctx, uint8_t* base) {
  EndControlsCapture();
  ShowInGameRows(ctx, base, false);
  Store32(kInGameCursors, kEntryIndex);
  NotifyCursor(ctx, base, kEntryIndex);
  if (g_windowChanged) {
    ScheduleWindow();
  }
  if (!g_written.empty() && !g_configPath.empty()) {
    SaveSettings();
  }
  g_written.clear();
}

}  // namespace

// Quit Game's question ("If you quit the game...", Yes, No) sits at fixed
// places (x, y pairs read as it's drawn, sub_83693D68): y 655, the line below
// the list's last row. Our row moves Return to Game down onto it, so while
// ours is in the list the question goes 28 lower.
constexpr uint32_t kQuitQuestionPlaces[] = {0x83BC03CC, 0x83BC03D4, 0x83BC03DC};
constexpr float kQuitQuestionY = 655.0f, kQuitQuestionLowered = 683.0f;

void PlaceQuitQuestion(bool lowered) {
  for (uint32_t place : kQuitQuestionPlaces) {
    StoreFloat(place + 4, lowered ? kQuitQuestionLowered : kQuitQuestionY);
  }
}

// The list's builder, run as it first opens: our row after its twelve.
REX_EXTERN(__imp__sub_836952B8);
REX_HOOK_RAW(sub_836952B8) {
  uint32_t list = ctx.r3.u32;
  __imp__sub_836952B8(ctx, base);
  EndControlsCapture();
  if (g_inGameOurs && !g_written.empty() && !g_configPath.empty()) {
    SaveSettings();  // the list was made again while ours was shown
    g_written.clear();
  }
  g_inGameOurs = false;
  ng2::video::SetInGameRecompHeader(false);
  g_inGameShownOrder = false;
  PlaceQuitQuestion(false);
  if (SetUpInGameRows(ctx, base)) {
    g_list = list;
    for (uint32_t i = 0; i < kGameRows; i++) {
      g_gameRows[i] = ng2::guest::Load32(g_list + kRowsAt + 4 * i);
    }
    ArrangeGameRows();
    PlaceGameRows(ctx, base);
    g_inGameShownOrder = true;
    PlaceQuitQuestion(true);
  }
}


// The list's methods over its rows: ours too.
REX_EXTERN(__imp__sub_83695610);
REX_HOOK_RAW(sub_83695610) {  // reset
  uint32_t list = ctx.r3.u32;
  __imp__sub_83695610(ctx, base);
  if (list == g_list) {
    uint32_t row = ng2::guest::Load32(g_list + kRowsAt + 4 * kGameRows);
    CallAt(ctx, base, Method(row, 1), {row});
  }
}
REX_EXTERN(__imp__sub_83695668);
REX_HOOK_RAW(sub_83695668) {  // update
  uint32_t list = ctx.r3.u32;
  __imp__sub_83695668(ctx, base);
  if (list != g_list) {
    return;
  }
  uint32_t row = ng2::guest::Load32(g_list + kRowsAt + 4 * kGameRows);
  CallAt(ctx, base, Method(row, 2), {row});
  if (!g_inGameOurs) {
    return;
  }
  // The cursor mark at our row: a category, or a page row in the right
  // column (the list placed it by the heights of the rows above its slot).
  float origin = LoadFloat(kMarkOrigin), height = LoadFloat(kLineHeight);
  const ng2::panel::State& state = g_panelStates[g_category];
  uint32_t line = g_inPage ? uint32_t(state.row - state.scroll) : SlotOfCategory(g_category);
  StoreFloat(list + kCursorMark + kMarkX, origin + (g_inPage ? kPageColumn : 0.0f));
  StoreFloat(list + kCursorMark + kMarkY, std::floor(origin + float(line) * height));
  if (g_inPage && kPages[g_category].achievements) {
    // (the achievements' rows are taller than the list's lines; none: no mark)
    StoreFloat(list + kCursorMark + kMarkY, ng2::achievements::Total()
                                                ? std::floor(origin + g_achievementsMarkY - kInGameTop)
                                                : -10000.0f);
  }
  if (g_inPage && kPages[g_category].custom) {
    StoreFloat(list + kCursorMark + kMarkY, -10000.0f);  // the Controls table lights its own cell
  }
}
REX_HOOK_RAW(sub_83695800) {  // event 1 to every row
  uint32_t list = ctx.r3.u32, model = ctx.r4.u32;
  __imp__sub_83695800(ctx, base);
  if (list == g_list) {
    uint32_t row = ng2::guest::Load32(g_list + kRowsAt + 4 * kGameRows);
    CallGuest(ctx, base, __imp__sub_83692378, {row, 1, model});
  }
}
REX_EXTERN(__imp__sub_83695748);
REX_HOOK_RAW(sub_83695748) {  // draw: the rows one under another
  uint32_t list = ctx.r3.u32;
  uint64_t layer = ctx.r4.u64, position = ctx.r5.u64;
  if (list == g_list) {
    g_inGameLayer = uint32_t(layer);
  }
  __imp__sub_83695748(ctx, base);
  if (list == g_list && g_inGameOurs) {
    ng2::overlay::QueuePanel(ctx, base);  // the Controls table or a note, with the list
  }
  if (list == g_list) {
    uint32_t bits = uint32_t(position);
    float y;
    std::memcpy(&y, &bits, sizeof(y));
    for (uint32_t i = 0; i < kGameRows; i++) {
      y += LoadFloat(ng2::guest::Load32(g_list + kRowsAt + 4 * i) + kRowHeight);
    }
    std::memcpy(&bits, &y, sizeof(bits));
    // x in the high word, y in the low one, as each row gets them.
    uint32_t row = ng2::guest::Load32(g_list + kRowsAt + 4 * kGameRows);
    CallAt(ctx, base, Method(row, 3), {row, layer, (position & 0xFFFFFFFF00000000ull) | bits});
  }
}

// Pad queries held back from the game's handler while ours acts.
REX_HOOK_RAW(sub_823E3828) {
  ng2::menu_mouse::NoteMenuInput();
  if (g_suppress & (1u << ctx.r7.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823E3828(ctx, base);
}
REX_HOOK_RAW(sub_823E42B0) {
  ng2::menu_mouse::NoteMenuInput();
  __imp__sub_823E42B0(ctx, base);
}
REX_HOOK_RAW(sub_823E4158) {
  ng2::menu_mouse::NoteMenuInput();
  if (g_suppress & 0x10) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823E4158(ctx, base);
}

// The mouse on the game's own rows of the in-game list and on Quit Game's
// question (#163). Values: the list's model, (current, last) words from
// kInGameModel + 8 * (row + 1), the game's row index. Places measured in the
// 1280x720 UI: the choices of a row in one line, about 182 apart from 545;
// the sliders from 480 to 820. Quit Game's question (state 1): its cursor at
// kInGameCursors + 4, 0 No, 1 Yes, the two words at (800, 683) and (895, 683).
enum class PausePick : uint8_t { kNone, kHalves, kTwo, kOnOff, kFour };
constexpr PausePick kPausePicks[12] = {
    PausePick::kNone,   // the manual
    PausePick::kFour,   // 1st-Person Camera Control: Default, Vertical, Horizontal, All Inverted
    PausePick::kFour,   // 3rd-Person
    PausePick::kHalves, // 1st-Person Cam. Speed
    PausePick::kHalves, // 3rd-Person
    PausePick::kTwo,    // Swimming Control: Default, Vertical Inverted
    PausePick::kOnOff,  // Karma Display
    PausePick::kOnOff,  // Controller Vibration
    PausePick::kOnOff,  // NINJA CINEMA
    PausePick::kOnOff,  // Save Method: Auto, Manual
    PausePick::kNone,   // Quit Game
    PausePick::kNone,   // Return to Game
};
constexpr float kPauseValueLeft = 480.0f, kPauseValueRight = 1200.0f;
constexpr float kPauseChoiceLeft = 545.0f, kPauseChoicePitch = 182.0f, kPauseSliderMiddle = 650.0f;
uint32_t g_pauseSteps = 0, g_pauseStepsRow = 0;  // a four-choice row's presses still to make, one a frame
uint64_t g_pauseStepsFrame = 0;
void PausePress(uint32_t remove, uint32_t add) {
  for (uint32_t pad = 0; pad < 5; pad++) {  // (the four, then the merged one)
    uint32_t at = kPadPressed + pad * 0x44;
    Store32(at, (ng2::guest::Load32(at) & ~remove) | add);
  }
}

// Quit Game's question (sub_823E8D40, state 1): the pointer on Yes or No
// picks it, with the question's own sound (3); a click on it confirms it, on
// neither nothing. The handler shows the cursor it reads each frame.
REX_EXTERN(__imp__sub_823E8D40);
REX_HOOK_RAW(sub_823E8D40) {
  ng2::menu_mouse::NoteMenuInput();
  float x, y;
  if (ng2::menu_mouse::Pointer(&x, &y)) {
    int word = y >= 662.0f && y < 706.0f ? (x >= 760.0f && x < 850.0f ? 1 : x >= 850.0f && x < 950.0f ? 0 : -1) : -1;
    if (ng2::menu_mouse::ClickedThisFrame() && word < 0) {
      PausePress(0x10, 0);
    }
    if (ng2::menu_mouse::PointerActive() && word >= 0 && uint32_t(word) != ng2::guest::Load32(kInGameCursors + 4)) {
      CallGuest(ctx, base, __imp__sub_8242AFC0, {3}, LoadFloat(kCursorSound));
      Store32(kInGameCursors + 4, uint32_t(word));
    }
  }
  __imp__sub_823E8D40(ctx, base);
}

// The list's handler: moves onto and off our row, and our settings while
// they're shown.
REX_EXTERN(__imp__sub_823E8720);
REX_HOOK_RAW(sub_823E8720) {
  ng2::menu_mouse::NoteMenuInput();  // (a menu up: the mouse's, menu_mouse.cpp)
  using ng2::guest::Load32;
  uint32_t state = Load32(kInGameState);
  if (!g_list || state != 0) {
    __imp__sub_823E8720(ctx, base);
    return;
  }
  uint32_t cursorAddress = kInGameCursors;
  uint32_t cursor = Load32(cursorAddress);
  uint32_t pad = Load32(kPadState);
  uint32_t player = *ng2::guest::Virtual(kPlayerPad);
  auto pressed = [&](uint32_t direction) {
    return CallGuest(ctx, base, __imp__sub_823E3828, {pad, player, 1, 0, direction}) != 0;
  };
  double sound = LoadFloat(kCursorSound);

  if (g_inGameOurs) {
    bool back = CallGuest(ctx, base, __imp__sub_823E42B0, {kPadState, cursor, 0xFF, 0}) != 0;
    // The page's column under the pointer (the Controls table, or our panel's rows).
    auto pagePointed = [&] {
      const Page& page = kPages[g_category];
      float x, y;
      if (!ng2::menu_mouse::Pointer(&x, &y)) {
        return false;
      }
      if (page.custom) {
        return PointedRow(kInGamePageLeft, kTableCellX[2] + 140.0f, kTableTop, kTablePitch,
                          ng2::rebind::kActions + 1) >= 0;
      }
      if (page.achievements) {
        return ng2::panel::PointedRow(g_achievementsState, int(ng2::achievements::List().size()), x, y,
                                      AchievementsLayout(true)) >= 0;
      }
      return ng2::panel::PointedRow(g_panelStates[g_category], int(PanelRows(page, true).size()), x, y, kInGameLayout) >= 0;
    };
    // (a click on no row of ours confirms nothing)
    bool clickOff = ng2::menu_mouse::ClickedThisFrame() &&
                    PointedRow(kInGameLeft, kInGameCategoryRight, kInGameTop, kInGamePitch, int(ShownCategories())) < 0 &&
                    !pagePointed();
    // The pointer on a category shows its page (leaving the page for the
    // categories); on the page's column, the cursor goes into it.
    if (ng2::menu_mouse::PointerActive() && !g_controls.capture) {
      int slot = PointedRow(kInGameLeft, kInGameCategoryRight, kInGameTop, kInGamePitch, int(ShownCategories()));
      int category = slot >= 0 ? int(CategoryAtSlot(uint32_t(slot))) : -1;
      if (category >= 0) {
        if (g_inPage || uint32_t(category) != g_category) {
          CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
          if (uint32_t(category) != g_category) {
            ShowInGameRows(ctx, base, true, uint32_t(category), true);
          }
          g_inPage = false;
          cursor = SlotOfCategory(g_category);
        }
      } else if (!g_inPage && pagePointed()) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
        g_inPage = true;
        cursor = kFirstPageRow;
      }
    }
    if (g_inPage) {
      // The page: the Controls table or our panel, each with its own input.
      const Page& page = kPages[g_category];
      bool leave = RunPageInput(ctx, base, page, true);
      if (leave) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
        g_inPage = false;
        cursor = SlotOfCategory(g_category);
      } else if (!CategoryShown(g_category)) {
        // Cheats gone (its last cheat turned off, or Restore Defaults, with
        // cheats_unlocked off: a cheat on from an older ng2.toml): the
        // cursor to the first category.
        g_inPage = false;
        ShowInGameRows(ctx, base, true, CategoryAtSlot(0));
        cursor = 0;
      }
    } else {
      // The categories: up and down show one's page, Confirm or right go
      // into it, Back leaves.
      if (back) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
        LeaveInGameRows(ctx, base);
        return;
      }
      uint32_t count = ShownCategories(), slot = SlotOfCategory(g_category);
      if (pressed(0)) {
        slot = (slot + count - 1) % count;
      }
      if (pressed(1)) {
        slot = (slot + 1) % count;
      }
      if (CategoryAtSlot(slot) != g_category) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
        ShowInGameRows(ctx, base, true, CategoryAtSlot(slot), true);
      }
      cursor = SlotOfCategory(g_category);
      if (pressed(2) || (!clickOff && CallGuest(ctx, base, __imp__sub_823E4158, {kPadState, cursor, 0, 0}))) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
        g_inPage = true;
        cursor = kFirstPageRow;
      }
    }
    Store32(cursorAddress, cursor);
    if (g_windowChanged) {
      ScheduleWindow();
    }
    if (g_unsaved && !g_configPath.empty()) {
      SaveSettings();  // (once a press; g_written keeps them for Back's save too)
    }
    g_unsaved = false;
    ShowPage(kPages[g_category], g_inPage, g_inGameLayer);
    NotifyCursor(ctx, base, cursor);
    return;
  }

  // The game's list, our row shown after Save Method: the moves onto and
  // off it are made here, in the order shown, the rest by the game's
  // handler in its own indices.
  bool up = pressed(0), down = pressed(1);
  // The pointer on a row: the cursor there (the rows in the order shown:
  // the manual's, taller, then the others 41 apart).
  float px, py;
  if (ng2::menu_mouse::ClickedThisFrame() && ng2::menu_mouse::Pointer(&px, &py) &&
      (px < kInGameLeft || px > kPauseValueRight || py < 110.0f || py > 185.0f + 41.0f * float(kGameRows))) {
    g_suppress |= 0x10;  // (a click off the rows confirms nothing)
  }
  // (while a four-choice row's steps run, the pointer leaves the cursor on
  // it, so they all land; a new click still counts)
  bool stepping = g_pauseSteps && !ng2::menu_mouse::ClickedThisFrame() &&
                  ng2::menu_mouse::Frame() - g_pauseStepsFrame <= 3;
  if (!stepping && ng2::menu_mouse::PointerActive() && ng2::menu_mouse::Pointer(&px, &py) &&
      px >= kInGameLeft && px <= kPauseValueRight) {
    int shown = py >= 110.0f && py < 164.0f ? 0 : PointedRow(kInGameLeft, kPauseValueRight, 185.0f - 41.0f, 41.0f, int(kGameRows) + 1);
    if (shown >= 1 || (shown == 0 && py < 164.0f)) {
      uint32_t to = ToGameIndex(uint32_t(shown));
      if (to != cursor) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
        Store32(cursorAddress, to);
        cursor = to;
        g_suppress |= 0x3;  // the game's handler doesn't move it again
        up = down = false;
      }
      // A click on a game row's value: the choice under it (left half of a
      // slider less, right half more); not Confirm.
      if (ng2::menu_mouse::ClickedThisFrame() && to < 12 && px >= kPauseValueLeft &&
          kPausePicks[to] != PausePick::kNone) {
        g_pauseSteps = 0;  // (a new click: the last one's steps no longer)
        uint32_t value = Load32(kInGameModel + 8 * (to + 1));
        int choice = std::clamp(int(std::floor((px - kPauseChoiceLeft) / kPauseChoicePitch + 0.5f)), 0, 3);
        uint32_t add = 0;
        switch (kPausePicks[to]) {
          case PausePick::kHalves:
            add = px < kPauseSliderMiddle ? 0x4 : 0x8;
            break;
          case PausePick::kTwo:
          case PausePick::kOnOff: {
            bool leftShown = value == (kPausePicks[to] == PausePick::kOnOff ? 1u : 0u);
            add = (choice == 0) != leftShown ? 0x8 : 0;  // (either way toggles)
            break;
          }
          case PausePick::kFour:
            if (uint32_t steps = (uint32_t(choice) + 4 - (value & 3)) % 4) {
              add = 0x8;
              g_pauseSteps = steps - 1, g_pauseStepsRow = to;
              g_pauseStepsFrame = ng2::menu_mouse::Frame();
            }
            break;
          default:
            break;
        }
        PausePress(0x10 | 0x4 | 0x8, add);
        g_suppress |= 0x10;
      }
    }
  }
  // A four-choice row's further steps (one a frame), while it's still the
  // cursor's row (after the pointer above may have moved it).
  if (g_pauseSteps && g_pauseStepsRow == cursor && !ng2::menu_mouse::ClickedThisFrame() &&
      ng2::menu_mouse::Frame() - g_pauseStepsFrame <= 3 && g_pauseStepsFrame != ng2::menu_mouse::Frame()) {
    g_pauseSteps--;
    g_pauseStepsFrame = ng2::menu_mouse::Frame();
    PausePress(0x10, 0x8);
    g_suppress |= 0x10;
  } else if (g_pauseStepsRow != cursor || ng2::menu_mouse::Frame() - g_pauseStepsFrame > 3) {
    g_pauseSteps = 0;
  }
  uint32_t shown = ToShown(cursor), toShown = shown;
  if (shown == kEntryShownAt) {
    toShown = up ? kEntryShownAt - 1 : down ? kEntryShownAt + 1 : shown;
  } else if ((shown == kEntryShownAt - 1 && down) || (shown == kEntryShownAt + 1 && up)) {
    toShown = kEntryShownAt;
  }
  uint32_t to = ToGameIndex(toShown);
  if (to != cursor) {
    CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
    Store32(cursorAddress, to);
    cursor = to;
    g_suppress |= 0x3;  // the game's handler doesn't move it again
  }
  if (cursor == kEntryIndex) {
    g_suppress |= 0xC;  // no left and right either
    if (!(g_suppress & 0x10) && CallGuest(ctx, base, __imp__sub_823E4158, {kPadState, cursor, 0, 0})) {
      CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
      g_inPage = false;
      ShowInGameRows(ctx, base, true, 0);
      Store32(cursorAddress, 0);
      NotifyCursor(ctx, base, 0);
      g_suppress = 0;
      return;
    }
    g_suppress |= 0x10;  // the game's own Confirm reads past its rows
  }
  __imp__sub_823E8720(ctx, base);
  g_suppress = 0;
}


// The list's cursor event, as the game's (sub_836958F0) but for rows
// placed in the order shown: the rows to light and unlight are found by
// the handler's indices, and they compare the cursor with their own index
// (their place), so they get the cursors in the order shown; the cursor
// mark's slot (+0x4EC0) is the shown place too.
REX_EXTERN(__imp__sub_836958F0);
REX_HOOK_RAW(sub_836958F0) {
  uint32_t list = ctx.r3.u32, model = ctx.r4.u32;
  // A dialog over the list (Quit Game's Yes / No) moves its own cursor
  // through the same event: the game's.
  if (list != g_list || !g_inGameShownOrder || g_inGameOurs ||
      ng2::guest::Load32(kInGameState) != 0) {
    __imp__sub_836958F0(ctx, base);
    return;
  }
  using ng2::guest::Load32;
  uint32_t cursor = Load32(model), last = Load32(model + 4);
  Store32(model, ToShown(cursor));
  Store32(model + 4, int32_t(last) >= 0 ? ToShown(last) : last);
  if (int32_t(last) >= 0) {
    CallGuest(ctx, base, __imp__sub_83692378,
              {Load32(list + kRowsAt + 4 * last), 2, model});
  }
  CallGuest(ctx, base, __imp__sub_83692378, {Load32(list + kRowsAt + 4 * cursor), 2, model});
  Store32(model, cursor);
  Store32(model + 4, last);
  Store32(list + kCursorMarkSlot, ToShown(cursor));
  ctx.r3.u64 = 1;
}

// Quit Game's value (its Yes / No): the row reads it from the model at
// 8 * (its index + 1), and its index is its shown place, one past the slot
// the game writes it to; it's read from the game's slot instead.
REX_EXTERN(__imp__sub_836941B0);
REX_HOOK_RAW(sub_836941B0) {
  constexpr uint32_t kQuitGameIndex = 10;
  uint32_t row = ctx.r3.u32, model = ctx.r4.u32;
  uint32_t index = ng2::guest::Load32(row + 8);
  bool moved = g_inGameShownOrder && !g_inGameOurs && model == kInGameModel &&
               index == ToShown(kQuitGameIndex);
  if (moved) {
    Store32(row + 8, kQuitGameIndex);
  }
  __imp__sub_836941B0(ctx, base);
  if (moved) {
    Store32(row + 8, index);
  }
}

bool InGameOursShown() {
  return g_inGameOurs;
}

bool InGameConfirmShown() {
  if (!g_inPage) {
    return true;
  }
  // (Confirm does something on the Controls table and Restore Defaults)
  const Page& page = kPages[g_category];
  return !page.achievements && (page.custom || uint32_t(g_panelStates[g_category].row) >= page.count);
}
