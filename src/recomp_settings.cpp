// ng2 - Ninja Gaiden II static recompilation
//
// RECOMP SETTINGS: the title menu's Xbox LIVE Marketplace entry opens the
// game's own Options screen as a screen of the recomp's settings
// (docs/menus.md). While it is open:
//
// - The category list holds our categories (their names written over the
//   game's in the loaded string table, the rest blanked) and is run by us:
//   Confirm opens a category's page, Back leaves Options.
// - Each category is one of the game's pages. Its builder runs with its own
//   rows left out, then we append ours: arrow lists for settings with more
//   than two choices (made in place where the page has none), two-choice
//   rows for the others, and Restore Defaults. Our texts go in spare slots
//   of the string table (the Music Test's track names).
// - The pages' handlers are replaced by one that edits our settings, using
//   the game's own input, sound and menu-model functions.
//
// Everything is restored and the settings are saved when Options closes.
// The title menu also loses LEADERBOARDS and gains EXIT GAME here.

#include <algorithm>
#include <functional>
#include <array>
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
REXCVAR_DEFINE_BOOL(exclusive_fullscreen, false, "UI/Window",
                    "Fullscreen in the desktop's display mode (exclusive) rather than as a "
                    "borderless window over the desktop");

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
// sub_836A5AA0(n): the menu's transitions.
constexpr uint32_t kChoiceChanged = 5;
constexpr uint32_t kEnter = 6;
constexpr uint32_t kLeave = 7;

// The pages we use, and where in their page objects our rows go. Graphics
// is the Game Settings page: its language list is our first arrow row, and
// two more list widgets are made in the memory of a choice row it no longer
// uses. Display is the Controller Settings page. Each row's widget is reset,
// placed and appended by us after the game's builder has run with its own
// rows left out.
constexpr uint32_t kListVtable = 0x822869B0;  // an arrow list ("<- value ->")
constexpr uint32_t kListSize = 0x270;
constexpr uint32_t kListTexts = 0x20;        // up to 10 value texts
constexpr uint32_t kListShown = 0x234;       // the value text shown
constexpr uint32_t kLeftArrow = 0x50, kRightArrow = 0xE0;  // the arrows' sprites
constexpr uint32_t kSpriteX = 0x30;          // an arrow's offset across, from the
                                             // value (it grows towards it)
constexpr float kArrowSpread = 32.0f;
constexpr uint32_t kWidgetValue = 0x14;
constexpr uint32_t kWidgetX = 0x18, kWidgetY = 0x1C;
constexpr uint32_t kOnOffRow = 0x600;  // a choice row's flag: 1 shows its first choice
constexpr uint32_t kPlayTimeText = 0x859F5574;  // UTF-16, from sub_836D0E90
constexpr uint32_t kPageRows = 0x10;           // a page object's row count
constexpr uint32_t kModelValues = 0x8532EE30;  // + 0x40 * page + 8 * row
constexpr uint32_t kStringTable = 0x83BD1DC0;  // for sub_823E1238(table, language, id)

// Options strings: table 2 (id 0x20000 + index), strings back to back in
// 16-byte slots.
constexpr uint32_t kTable = 0x20000;
// The categories' names, by category: ours first, the others blanked.
constexpr uint32_t kCategoryNames[] = {0x00, 0x01, 0x97, 0x03, 0x02, 0x04, 0x05, 0x61};
constexpr uint32_t kPlayTimeName = 0x2F;
// A description of the game's we don't show (its Restore Defaults' long
// one, 53 letters): All Weapons and Ninpo's warning, written over it.
constexpr uint32_t kNoteSlot = 0x39;
constexpr std::u16string_view kAllWeaponsNote = u"Weapons given stay in your saves, even turned off.";
// Frame Interpolation's description (placed with the labels).
constexpr std::u16string_view kInterpNote = u"Experimental: pictures between the game's frames.";
// The Music Test's track names: room for our texts.
constexpr uint32_t kSpareFirst = 0x63, kSpareEnd = 0x97;
constexpr uint32_t kSlotCount = 0x98;  // past the last index used

enum class Kind {
  kScale, kFiltering, kLevel, kOnOff, kPresent, kWindowMode, kWindowSize, kSamples, kAspect,
  kSensitivity
};
// Mouse Sensitivity's choices (mnk_sensitivity).
constexpr double kSensitivities[] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
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
    // With what the game's 3D (1120x584) is drawn at, as the NGB recomp's.
    {u"Render Scale",
     {u"1x 1120x584", u"2x 2240x1168", u"3x 3360x1752", u"4x 4480x2336", u"5x 5600x2920",
      u"6x 6720x3504", u"7x 7840x4088", u"8x 8960x4672"},
     8, "render_scale", Kind::kScale},
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
    {u"Mouse Sensitivity", {u"0.25", u"0.5", u"0.75", u"1", u"1.5", u"2"}, 6, "mnk_sensitivity",
     Kind::kSensitivity},
    // Extras (extras.cpp, and the renderer's Texture LOD Bias).
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
    // display (Full Rate) or each second one (Half Rate; each one below
    // 120 Hz, so never fewer than the game's 60), or one between each two
    // of the game's frames (2x: 120 for its 60); gpu/interp.cpp.
    // (Short: a longer choice is squeezed narrow to fit.)
    {u"Frame Interpolation", {u"Off", u"Full Rate", u"Half Rate", u"2x"}, 4,
     "frame_interpolation", Kind::kLevel},
    // The shadow map's area fixed on Ryu (gpu/video.cpp, ShadowFitCorrect).
    {u"Shadows", {u"Original", u"Steady"}, 2, "shadow_fit", Kind::kLevel},
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
  return setting.choices[std::min(k, setting.count - 1)];
}

struct PageRow {
  uint32_t setting;
  uint32_t widget;  // offset in the page object (ours: in g_titleRows)
  bool list;        // an arrow list (else two choices)
  bool make;        // a list widget we make there
  bool ours = false;  // made in our own memory (the page has no room)
};
struct Page {
  std::u16string_view name;
  uint32_t page;   // its Options page (category + 1)
  uint32_t size;   // its page object's
  uint32_t restore;  // the Restore Defaults row's widget
  std::array<PageRow, 8> rows;
  uint32_t count;
  bool ourRestore = false;  // its Restore Defaults row made by us (the page has none)
  // The page the screen shows (its builder's, the menu model's page, its
  // cursor and values), when not the one the Options loop runs (page): 0, the
  // same.
  uint32_t view = 0;
};
constexpr uint32_t ViewOf(const Page& page) {
  return page.view ? page.view : page.page;
}
// Every row is an arrow list made in our own memory (g_titleRows, one per
// setting), so a page's own layout doesn't limit what it holds: the game's
// page objects give their builder, their Restore Defaults row and their
// slot in the menu model.
// (and a Restore Defaults row after them, for a page without one)
constexpr uint32_t kRestoreSize = 0xE0;
constexpr uint32_t kOurRestore = (uint32_t(std::size(kSettings)) * kListSize + 15) & ~15u;
constexpr uint32_t kTitleRowsSize = (kOurRestore + kRestoreSize + 0xFFF) & ~0xFFFu;
constexpr PageRow Row(uint32_t setting) {
  return {setting, setting * kListSize, true, true, true};
}
constexpr Page kPages[] = {
    {u"Graphics", 1, 0x2BB0, 0x2AD0,
     {{Row(0), Row(3), Row(1), Row(30), Row(2), Row(20)}},
     6},
    {u"Display", 2, 0x1CC0, 0x1A20, {{Row(4), Row(5), Row(14), Row(6), Row(28), Row(29)}}, 6},
    // (its name in the game's third category name, which holds 7 letters)
    {u"Cheats", 3, 0x13E0, 0x1300,
     {{Row(8), Row(9), Row(11), Row(24), Row(25), Row(26)}},
     6},
    {u"Interface", 4, 0x26A0, 0xAA0,
     {{Row(19), Row(23), Row(22), Row(7), Row(15), Row(16)}},
     6},
    // The Audio Settings page (5, builder sub_836D54F8, which stores its
    // page number at +12; Restore Defaults at +0x1380).
    {u"Extras", 5, 0x1460, 0x1380, {{Row(12), Row(13), Row(17), Row(18), Row(21)}}, 5},
    // Shown on the Copyright Information page (6: what the category list
    // shows for its sixth category; builder sub_836D57B8), run as page 11
    // (the Options loop runs page 6 inline, 11 through sub_823E7C40); no
    // Restore Defaults of its own: ours.
    {u"Gameplay", 11, 0x400, kOurRestore, {{Row(27), Row(10)}}, 2, true, 6},
};
static_assert(kOurRestore + kRestoreSize <= kTitleRowsSize);
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

bool g_open = false;
// The settings changed while open, as set (fullscreen is applied later, on
// the UI thread): saved on closing.
std::map<std::string, std::string, std::less<>> g_written;
bool g_inGameOurs = false;  // our settings are in the in-game menu's list
bool g_unsaved = false;     // in game: a change made this press, saved at its end
// Guest memory ours replaces: the game's values in the menu model for our
// pages (put back on closing), and the widgets our list widgets are made
// over (put back before the page is next built: the closing screen still
// draws them).
std::vector<std::pair<uint32_t, std::vector<uint8_t>>> g_savedGuest;
std::vector<std::pair<uint32_t, std::vector<uint8_t>>> g_savedWidgets;
// Our list widgets on the title's pages, by setting (0: none).
uint32_t g_listWidgets[std::size(kSettings)] = {};

void PutBack(std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& saved) {
  for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
    std::memcpy(guest::Virtual(it->first), it->second.data(), it->second.size());
  }
  saved.clear();
}
std::filesystem::path g_configPath;
rex::ui::WindowedAppContext* g_appContext = nullptr;
std::vector<std::pair<std::u16string_view, uint32_t>> g_textIds;  // our texts' string ids
// While ours is shown: the Options table's ids that give our texts instead of
// the game's (the text lookup's hook asks, RecompText), by slot; 0: the game's.
std::array<std::atomic<uint32_t>, kSlotCount> g_overrides{};

// The page object being built by the game's builder, whose rows are left out.
uint32_t g_building = 0;
uint32_t g_buildingSize = 0;

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
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

// Each of our texts a spare id of the Options table (its strings unused by
// Options), given our text by the lookup.
void PlaceTexts() {
  std::vector<std::u16string_view> texts{kRestoreText, kInterpNote};
  for (const Setting& setting : kSettings) {
    texts.push_back(setting.label);
    // List values are strings of ours (ListString); only choice rows'
    // choices are the table's.
    bool list = false;
    for (const Page& page : kPages) {
      for (uint32_t i = 0; i < page.count; i++) {
        list |= &kSettings[page.rows[i].setting] == &setting && page.rows[i].list;
      }
    }
    if (!list) {
      texts.insert(texts.end(), setting.choices.begin(), setting.choices.begin() + setting.count);
    }
  }
  std::sort(texts.begin(), texts.end());
  texts.erase(std::unique(texts.begin(), texts.end()), texts.end());
  uint32_t slot = kSpareFirst;
  for (std::u16string_view text : texts) {
    if (slot >= kSpareEnd) {
      REXLOG_WARN("recomp settings: no room for a text");
      break;
    }
    Override(slot, text);
    g_textIds.emplace_back(text, kTable | slot);
    slot++;
  }
}

// The string id of one of our texts (a blank one if it wasn't placed).
uint32_t TextId(std::u16string_view text) {
  for (auto [placed, id] : g_textIds) {
    if (placed == text) {
      return id;
    }
  }
  return kTable | 0x34;  // an empty string
}

void PutBackGame();

void Open() {
  if (g_open) {
    return;
  }
  g_open = true;
  PutBackGame();
  ng2::video::SetRecompHeader(true);
  PlaceTexts();
  for (uint32_t i = 0; i < std::size(kCategoryNames); i++) {
    Override(kCategoryNames[i], i < kPageCount ? kPages[i].name : u"");
  }
  Override(kPlayTimeName, u"");
  Override(kNoteSlot, kAllWeaponsNote);
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
    std::string entry = name + " = " + value;
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
  g_textIds.clear();
  PutBack(g_savedGuest);
}

void Close() {
  std::fill(std::begin(g_listWidgets), std::end(g_listWidgets), 0u);
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

int32_t GetInt(std::string_view name) {
  return std::atoi(GetSetting(name).c_str());
}

uint32_t ValueOf(const Setting& setting) {
  switch (setting.kind) {
    case Kind::kScale:
      return uint32_t(std::clamp(GetInt(setting.name), 1, int32_t(setting.count)) - 1);
    case Kind::kFiltering: {
      int32_t filtering = GetInt(setting.name);
      return filtering >= 5 ? 3 : filtering == 4 ? 2 : filtering == 3 ? 1 : 0;
    }
    case Kind::kLevel:
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
}

void SetValue(const Setting& setting, uint32_t value) {
  switch (setting.kind) {
    case Kind::kScale:
      SetSetting(setting.name, std::to_string(value + 1));
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

void SetConfigPath(const std::filesystem::path& path) {
  g_configPath = path;
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

// The same, for a function returning a float.
double CallGuestFloat(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*),
                      std::initializer_list<uint32_t> args) {
  PPCContext saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  uint64_t* registers[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
  size_t i = 0;
  for (uint32_t arg : args) {
    *registers[i++] = arg;
  }
  function(ctx, base);
  double result = ctx.f1.f64;
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

// The title menu's input: Confirm on the exit entry quits. (Each frame the
// menu is up: perf_overlay.cpp shows the version then.)
void TitleMenuFrame();
REX_EXTERN(__imp__sub_8366FBC0);
REX_HOOK_RAW(sub_8366FBC0) {
  __imp__sub_8366FBC0(ctx, base);
  TitleMenuFrame();
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

// The values of a page's rows (ours, then Restore Defaults' 0), below the
// guest stack.
uint32_t StoreValues(PPCContext& ctx, const Page* page) {
  uint32_t values = ctx.r1.u32 - 0x80;
  for (uint32_t i = 0; i < 8; i++) {
    Store32(values + 4 * i, page && i < page->count ? ValueOf(kSettings[page->rows[i].setting]) : 0);
  }
  return values;
}

REX_EXTERN(__imp__sub_83835960);  // XMemAlloc(size, attributes)

// List values' strings (UTF-16BE, in guest memory of ours): a list shows
// a string pointer per value, so they need no slot in the game's string
// table (Window Size alone has 20). Made once, kept.
std::vector<std::pair<std::u16string, uint32_t>> g_listStrings;
uint32_t g_listArena = 0, g_listArenaUsed = 0;
constexpr uint32_t kListArenaSize = 0x2000;
constexpr uint32_t kListArenaAttributes = 0xB2800000;

uint32_t ListString(PPCContext& ctx, uint8_t* base, std::u16string_view text) {
  for (const auto& [placed, address] : g_listStrings) {
    if (placed == text) {
      return address;
    }
  }
  if (!g_listArena) {
    g_listArena = CallGuest(ctx, base, __imp__sub_83835960, {kListArenaSize, kListArenaAttributes});
    if (!g_listArena) {
      return 0;
    }
  }
  uint32_t size = uint32_t(text.size() + 1) * 2;
  if (g_listArenaUsed + size > kListArenaSize) {
    REXLOG_WARN("recomp settings: no room for a list value");
    return 0;
  }
  uint32_t address = g_listArena + g_listArenaUsed;
  g_listArenaUsed += (size + 3) & ~3u;
  std::vector<uint8_t> bytes = Utf16BE(text);
  bytes.push_back(0), bytes.push_back(0);
  std::memcpy(ng2::guest::Virtual(address), bytes.data(), bytes.size());
  g_listStrings.emplace_back(text, address);
  return address;
}

// A list widget made in place, as the Game Settings page's constructor
// (sub_836A5200) makes its language list.
void MakeList(PPCContext& ctx, uint8_t* base, uint32_t widget, bool ours) {
  uint8_t* bytes = ng2::guest::Virtual(widget);
  if (!ours) {
    g_savedWidgets.emplace_back(widget, std::vector<uint8_t>(bytes, bytes + kListSize));
  }
  std::memset(ng2::guest::Virtual(widget), 0, kListSize);
  Store32(widget, kListVtable);
  for (uint32_t sprite : {0x50u, 0xE0u, 0x170u}) {
    Store32(widget + sprite + 0x74, 0);
    CallGuest(ctx, base, __imp__sub_83704630, {widget + sprite});
  }
  CallGuest(ctx, base, __imp__sub_8242C048, {widget + 0x200});
  Store32(widget + 0x260, 1);
}

// Our own rows for the title's pages (kOurs*), made once.
uint32_t g_titleRows = 0;
bool MakeTitleRowsOnce(PPCContext& ctx, uint8_t* base) {
  if (!g_titleRows) {
    g_titleRows = CallGuest(ctx, base, __imp__sub_83835960, {kTitleRowsSize, 0xB2800000});
    if (g_titleRows) {
      std::memset(ng2::guest::Virtual(g_titleRows), 0, kTitleRowsSize);
    }
  }
  return g_titleRows != 0;
}

// A Restore Defaults row in our memory, as the pages' constructors make
// theirs (sub_836A5558): its class, and a sprite at +0x40.
constexpr uint32_t kRestoreVtable = 0x822868F8;
void MakeRestore(PPCContext& ctx, uint8_t* base, uint32_t widget) {
  std::memset(ng2::guest::Virtual(widget), 0, kRestoreSize);
  Store32(widget, kRestoreVtable);
  Store32(widget + 0x40 + 0x74, 0);
  CallGuest(ctx, base, __imp__sub_83704630, {widget + 0x40});
}

bool HasOurRows(const Page& page) {
  bool ours = page.ourRestore;
  for (uint32_t i = 0; i < page.count; i++) {
    ours |= page.rows[i].ours;
  }
  return ours;
}

// Our rows onto a page object, after its builder (model: the builder's
// second argument, the menu model + 0x10).
void BuildPage(PPCContext& ctx, uint8_t* base, const Page& page, uint32_t object, uint32_t model) {
  using ng2::guest::Load32;
  Store32(object + kPageRows, 0);
  float y = 0.0f;
  auto place = [&](uint32_t widget) {
    CallGuest(ctx, base, __imp__sub_836CF0D0, {widget, model});
    StoreFloat(widget + kWidgetX, 0.0f);
    StoreFloat(widget + kWidgetY, y);
  };
  for (uint32_t i = 0; i < page.count; i++) {
    const PageRow& row = page.rows[i];
    const Setting& setting = kSettings[row.setting];
    uint32_t widget = (row.ours ? g_titleRows : object) + row.widget;
    uint32_t value = ValueOf(setting);
    if (row.make) {
      MakeList(ctx, base, widget, row.ours);
    }
    place(widget);
    if (row.list) {
      for (uint32_t k = 0; k < 10; k++) {
        Store32(widget + kListTexts + 4 * k, ListString(ctx, base, Choice(setting, k)));
      }
      g_listWidgets[row.setting] = widget;
      Store32(widget + kListShown, Load32(widget + kListTexts + 4 * value));
      // The arrows further apart than the game's, for the longer values
      // (Render Scale's resolutions).
      StoreFloat(widget + kLeftArrow + kSpriteX, LoadFloat(widget + kLeftArrow + kSpriteX) + kArrowSpread);
      StoreFloat(widget + kRightArrow + kSpriteX, LoadFloat(widget + kRightArrow + kSpriteX) - kArrowSpread);
      // The list's parts are placed for the page's first row only.
      for (uint32_t part : {0x50u + 0x34u, 0xE0u + 0x34u, 0x244u}) {
        StoreFloat(widget + part, LoadFloat(widget + part) + y);
      }
    } else {
      Store32(widget + kOnOffRow, 0);
      for (uint32_t k = 0; k < setting.count; k++) {
        CallGuest(ctx, base, __imp__sub_836CF1A8, {widget, TextId(setting.choices[k])});
      }
    }
    Store32(widget + kWidgetValue, value);
    CallGuest(ctx, base, __imp__sub_836D03E8, {object, TextId(setting.label), widget});
    y += float(CallGuestFloat(ctx, base, row.list ? __imp__sub_836CF198 : __imp__sub_836CF3B0,
                              {widget}));
  }
  uint32_t restore = (page.ourRestore ? g_titleRows : object) + page.restore;
  if (page.ourRestore) {
    MakeRestore(ctx, base, restore);
  }
  place(restore);
  CallGuest(ctx, base, __imp__sub_836D03E8, {object, TextId(kRestoreText), restore});
  // The rows' values in the model, as the handler will keep them; the
  // game's are put back on closing.
  uint32_t slots = kModelValues + 0x40 * ViewOf(page);
  uint8_t* bytes = ng2::guest::Virtual(slots);
  g_savedGuest.emplace_back(slots, std::vector<uint8_t>(bytes, bytes + 0x40));
  for (uint32_t i = 0; i < 8; i++) {
    uint32_t value = i < page.count ? ValueOf(kSettings[page.rows[i].setting]) : 0;
    Store32(slots + 8 * i, value);
    Store32(slots + 8 * i + 4, value);
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
  std::erase_if(g_savedWidgets, [&](auto& saved) {
    if (saved.first - object >= page->size) {
      return false;
    }
    std::memcpy(ng2::guest::Virtual(saved.first), saved.second.data(), saved.second.size());
    return true;
  });
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
  if (HasOurRows(*page) && !MakeTitleRowsOnce(ctx, base)) {
    REXLOG_WARN("recomp settings: no memory for the {} page's rows", ng2::guest::Load32(kPage));
    return;
  }
  BuildPage(ctx, base, *page, object, model);
}

bool InBuild(uint32_t address) {
  return g_building && address - g_building < g_buildingSize;
}

// Window Size's list after the aspect ratio changed: the new sizes.
void RefreshWindowSizes(PPCContext& ctx, uint8_t* base) {
  using ng2::guest::Load32;
  for (uint32_t i = 0; i < std::size(kSettings); i++) {
    uint32_t widget = g_listWidgets[i];
    if (kSettings[i].kind != Kind::kWindowSize || !widget) {
      continue;
    }
    for (uint32_t k = 0; k < 10; k++) {
      Store32(widget + kListTexts + 4 * k, ListString(ctx, base, Choice(kSettings[i], k)));
    }
    Store32(widget + kListShown, Load32(widget + kListTexts + 4 * ValueOf(kSettings[i])));
  }
}

// A page's input, while ours is open: up and down pick a row, left and right
// change it, Y sets its default, Confirm on Restore Defaults sets them all,
// Back returns to the category list.
void RunPage(PPCContext& ctx, uint8_t* base, const Page& page) {
  using ng2::guest::Load32;
  uint32_t number = ViewOf(page);  // (the page shown)
  uint32_t rowCount = page.count + 1;
  uint32_t cursorAddress = kCursors + 4 * number;
  uint32_t cursor = std::min(Load32(cursorAddress), rowCount - 1);
  double sound = LoadFloat(kCursorSound);
  if (CallGuest(ctx, base, __imp__sub_823E42B0, {kPadState, cursor, 0xFF, 0})) {
    Store32(kPage, kCategoryPage);
    Store32(kDepth, Load32(kDepth) - 1);
    CallGuest(ctx, base, __imp__sub_836A5AA0, {kLeave, 0});
  } else {
    uint32_t pad = Load32(kPadState);
    uint32_t player = *ng2::guest::Virtual(kPlayerPad);
    auto pressed = [&](uint32_t direction) {
      return CallGuest(ctx, base, __imp__sub_823E3828, {pad, player, 1, 0, direction}) != 0;
    };
    uint32_t moved = cursor;
    if (pressed(0)) {
      moved = (moved + rowCount - 1) % rowCount;
    }
    if (pressed(1)) {
      moved = (moved + 1) % rowCount;
    }
    if (moved != cursor) {
      CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
      cursor = moved;
      Store32(cursorAddress, cursor);
    }
    if (cursor == page.count) {
      // Restore Defaults; the enabled[] list is only read for the list page.
      if (CallGuest(ctx, base, __imp__sub_823E4158, {kPadState, cursor, 0, 0})) {
        for (uint32_t i = 0; i < page.count; i++) {
          RestoreDefault(kSettings[page.rows[i].setting]);
        }
        RefreshWindowSizes(ctx, base);
        CallGuest(ctx, base, __imp__sub_836A5AA0, {kEnter, 0});
      }
    } else {
      const Setting& setting = kSettings[page.rows[cursor].setting];
      uint32_t value = ValueOf(setting);
      uint32_t changed = value;
      bool left = pressed(3);
      if (left) {
        changed = (changed + setting.count - 1) % setting.count;
      }
      if (pressed(2)) {
        changed = (changed + 1) % setting.count;
      }
      if (changed != value) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {3}, sound);
        SetValue(setting, changed);
        if (setting.kind == Kind::kAspect) {
          RefreshWindowSizes(ctx, base);
        }
      } else if (CallGuest(ctx, base, __imp__sub_823E43B0, {kPadState})) {
        RestoreDefault(setting);
        RefreshWindowSizes(ctx, base);
      }
      CallGuest(ctx, base, __imp__sub_836A5AA0, {kChoiceChanged, left ? 0u : 1u});
    }
  }
  if (g_windowChanged) {
    ScheduleWindow();
  }
  CallGuest(ctx, base, __imp__sub_836A4B90,
            {kMenuModel, ShownPage(Load32(kLastPage)), number, kCursors, StoreValues(ctx, &page), 8});
}

}  // namespace

// The loop starts with the first category and rows picked, and leaves the
// game's as they were.
REX_EXTERN(__imp__sub_823E59A0);
REX_HOOK_RAW(sub_823E59A0) {
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

// The category list, replaced while ours is open: our categories only;
// Confirm opens one's page, Back leaves Options.
REX_EXTERN(__imp__sub_823E5D88);
REX_HOOK_RAW(sub_823E5D88) {
  if (!g_open) {
    __imp__sub_823E5D88(ctx, base);
    return;
  }
  using ng2::guest::Load32;
  uint32_t page = Load32(kPage);
  uint32_t cursor = std::min(Load32(kCursors), kPageCount - 1);
  uint32_t enabled = ctx.r1.u32 - 0xC0;
  for (uint32_t i = 0; i < 8; i++) {
    Store32(enabled + 4 * i, i < kPageCount);
  }
  if (CallGuest(ctx, base, __imp__sub_823E4158, {kPadState, cursor, enabled, 0})) {
    Store32(kPage, kPages[cursor].page);
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

// The pages' handlers, replaced while ours is open.
void Handle(PPCContext& ctx, uint8_t* base, void (*handler)(PPCContext&, uint8_t*)) {
  const Page* page = PageOf(ng2::guest::Load32(kPage));
  if (g_open && page) {
    RunPage(ctx, base, *page);
  } else {
    handler(ctx, base);
  }
}
REX_EXTERN(__imp__sub_823E66B8);
REX_HOOK_RAW(sub_823E66B8) {
  Handle(ctx, base, __imp__sub_823E66B8);
}
REX_EXTERN(__imp__sub_823E6D00);
REX_HOOK_RAW(sub_823E6D00) {
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

// The button hints under a page (sub_836D6898(&flags): 0x10 Change Storage
// Device, 8 Test Volume, 4 Restore Defaults, 2 Confirm, 1 Back), by the
// game's page and row: on ours, Restore Defaults and Back on every row.
REX_EXTERN(__imp__sub_836D6898);
bool InGameOursShown();
REX_HOOK_RAW(sub_836D6898) {
  uint32_t flags = ctx.r3.u32;
  bool title = g_open && PageOf(ng2::guest::Load32(kPage));
  // In the in-game menu the game's hints follow its own rows' slots (Save
  // Method's: Change Storage Device): there, with ours shown, Confirm and
  // Back as the game gives them, nothing of its own rows'.
  bool inGame = !title && InGameOursShown() && !ng2::aspect::HudShown();
  if ((!title && !inGame) || !flags) {
    __imp__sub_836D6898(ctx, base);
    return;
  }
  uint32_t game = ng2::guest::Load32(flags);
  uint32_t ours = title ? 0x5 : (game & 0x10) || !(game & 0x3) ? 0x1 : (game & 0x3);
  Store32(flags, ours);
  __imp__sub_836D6898(ctx, base);
  Store32(flags, game);
}

// The hints' and description's selector (sub_836D6AC8(view, model)): the
// description is the game's for its page and row (a table of text ids by
// page * 8 + row), and the Game Settings page's rows 5 and 6 get Change
// Storage Device before the hints above. On ours: no description but on
// Restore Defaults ("Restore settings to their defaults."), All Weapons and
// Ninpo and Frame Interpolation, through the text lookup's hook
// (storage_prompt.cpp), and that page seen as another.
void SetDescriptionText(uint32_t id);  // storage_prompt.cpp; 0: the game's
REX_EXTERN(__imp__sub_836D6AC8);
REX_HOOK_RAW(sub_836D6AC8) {
  using ng2::guest::Load32;
  uint32_t model = ctx.r4.u32;
  const Page* page = g_open && model ? PageOf(Load32(kPage)) : nullptr;
  if (!page) {
    __imp__sub_836D6AC8(ctx, base);
    return;
  }
  constexpr uint32_t kRestoreDescription = kTable | 0x9A, kEmpty = kTable | 0x34;
  uint32_t row = Load32(model + 8 * (Load32(model + 8) + 2));
  std::string_view name = row < page->count ? kSettings[page->rows[row].setting].name : "";
  SetDescriptionText(row == page->count                 ? kRestoreDescription
                     : name == "cheat_all_weapons"      ? (kTable | kNoteSlot)
                     : name == "frame_interpolation"    ? TextId(kInterpNote)
                                                        : kEmpty);
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

// Each frame (perf_overlay.cpp): a key's fullscreen choice, and a window
// position once it's been still for a second, written to the settings.
void ng2::recomp_settings::WindowFrame() {
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

// RECOMP SETTINGS in gameplay: the Back button's "IN-GAME MANUAL AND OPTIONS"
// list (docs/menus.md) gets a RECOMP SETTINGS row after its twelve. Choosing
// it lays the list out as the title's RECOMP SETTINGS screen until Back: the
// categories on the left, the chosen one's settings on the right ("< value >"
// in the game's font where the title has arrow lists), Restore Defaults, and
// the header redrawn as RECOMP SETTINGS. The list's twelve rows then hold the
// categories, the chosen one's settings, Restore Defaults and empty rows of
// no height. The list's own methods walk its twelve rows, so the extra row is
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
constexpr uint32_t kChoiceRowSize = 0x780, kLabelRowSize = 0x130;
constexpr uint32_t kLabelRowVtable = 0x82286618;  // Return to Game's class
constexpr uint32_t kQuitRowVtable = 0x82286660;   // Quit Game's
constexpr uint32_t kRowLabel = 0xE4;              // a row's text
constexpr uint32_t kChoiceText = 0x434;           // + 0x100 per choice
constexpr uint32_t kRowHeight = 0x18;
constexpr uint32_t kLineHeight = 0x8218243C;      // the float the builder gives rows
constexpr uint32_t kAnyText = 0x40018;            // a string id, for the setup call
constexpr uint32_t kGuestAlloc = 0xB2800000;
constexpr uint32_t kChoicesShown = 4;             // a choice row's room

constexpr std::u16string_view kEntryText = u"Recomp Settings";
// Our layout in the list's rows: the categories, then a page's settings and
// Restore Defaults, drawn to the right of the categories (where the title
// screen has its pages), then empty rows.
constexpr uint32_t kFirstPageRow = kPageCount;
constexpr uint32_t kEmptyRows = kGameRows - kFirstPageRow - 1;  // for the shortest page
constexpr float kPageColumn = 344.0f;  // the title's pages are this far right of its categories
constexpr uint32_t kCursorMark = 0x4ED0, kCursorMarkSlot = 0x4EC0;  // in the list
constexpr uint32_t kMarkX = 0x30, kMarkY = 0x34;  // the mark's place, from the list's
constexpr uint32_t kMarkOrigin = 0x8228A1C8;     // the float the list places it from
constexpr uint32_t kValueRoom = 24;
// A row is drawn where its slot's frame in the list's layout is; each of its
// sprites (label, choice mark, choices, the strokes between them) is placed
// from there by +0x30. The label's and the choices' are 0; the strokes' are
// set by the row's setup, the choice mark's (under the chosen one) by its
// update when the value changes.
constexpr uint32_t kSpriteOffset = 0x30;
constexpr uint32_t kRowSprite = 0x20, kChoiceMark = 0x120;
constexpr uint32_t kChoiceItems = 0x370, kChoiceItemSize = 0x100;
constexpr uint32_t kStrokes = 0x1C0, kStrokeSize = 0x90;
constexpr uint32_t kChoiceCount = 0x770;

uint32_t g_list = 0;            // the list object, once built
uint32_t g_arena = 0, g_arenaUsed = 0, g_arenaSize = 0;
uint32_t g_entry = 0;           // our row in the list
uint32_t g_settingRows[std::size(kSettings)] = {};
uint32_t g_valueText[std::size(kSettings)] = {};  // rows with too many choices show one
uint32_t g_categoryRows[kPageCount] = {}, g_restoreRow = 0, g_emptyRows[kEmptyRows] = {};
uint32_t g_category = 0;       // the category shown
bool g_inPage = false;         // the cursor is in its settings (else on the categories)
uint32_t g_gameRows[kGameRows] = {};
uint32_t g_suppress = 0;        // directions (bit per sub_823E3828 direction) and Confirm (bit 4)

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

void SetGuestText(uint32_t at, std::u16string_view text) {
  std::vector<uint8_t> bytes = Utf16BE(text);
  bytes.push_back(0), bytes.push_back(0);
  std::memcpy(ng2::guest::Virtual(at), bytes.data(), bytes.size());
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

std::u16string ValueText(const Setting& setting) {
  return std::u16string(Choice(setting, ValueOf(setting)));
}

// Shown as "< value >", as the title shows it in an arrow list.
bool IsListSetting(uint32_t setting) {
  for (const Page& page : kPages) {
    for (uint32_t i = 0; i < page.count; i++) {
      if (page.rows[i].setting == setting) {
        return page.rows[i].list;
      }
    }
  }
  return false;
}

std::u16string ArrowText(const Setting& setting) {
  return u"\u2039  " + ValueText(setting) + u"  \u203A";
}

// Our rows and their texts, made once.
uint32_t g_ids = 0;  // string ids for the choice rows' setup call
uint32_t g_entryText = 0, g_restoreText = 0, g_emptyText = 0;
uint32_t g_categoryText[kPageCount] = {};
uint32_t g_labelText[std::size(kSettings)] = {};
uint32_t g_choiceTexts[std::size(kSettings)][kChoicesShown] = {};

bool MakeInGameRowsOnce(PPCContext& ctx, uint8_t* base) {
  if (g_arena) {
    return true;
  }
  // The rows, then their texts (0x2000 is plenty for the labels and values).
  constexpr uint32_t kRowsSize = (2 + kPageCount + kEmptyRows) * kLabelRowSize +
                                 uint32_t(std::size(kSettings)) * kChoiceRowSize;
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
  g_restoreRow = ArenaAlloc(kLabelRowSize);
  for (uint32_t& row : g_emptyRows) {
    row = ArenaAlloc(kLabelRowSize);
  }
  for (uint32_t& row : g_settingRows) {
    row = ArenaAlloc(kChoiceRowSize);
  }
  g_ids = ArenaAlloc(4 * kChoicesShown);
  g_entryText = GuestText(kEntryText);
  g_restoreText = GuestText(kRestoreText);
  g_emptyText = GuestText(u"");
  for (uint32_t i = 0; i < kPageCount; i++) {
    g_categoryText[i] = GuestText(kPages[i].name);
  }
  for (uint32_t i = 0; i < std::size(kSettings); i++) {
    const Setting& setting = kSettings[i];
    g_labelText[i] = GuestText(setting.label);
    if (IsListSetting(i)) {
      g_valueText[i] = GuestText(u"", kValueRoom);  // "< value >"
    } else {
      for (uint32_t k = 0; k < setting.count; k++) {
        g_choiceTexts[i][k] = GuestText(setting.choices[k]);
      }
    }
  }
  if (!g_valueText[5] || !g_labelText[std::size(kSettings) - 1] ||
      (IsListSetting(std::size(kSettings) - 1) && !g_valueText[std::size(kSettings) - 1])) {
    REXLOG_WARN("recomp settings: no room for the in-game rows");
    return false;
  }
  for (uint32_t k = 0; k < kChoicesShown; k++) {
    Store32(g_ids + 4 * k, kAnyText);
  }
  MakeLabelRow(ctx, base, g_entry);
  MakeLabelRow(ctx, base, g_restoreRow);
  for (uint32_t row : g_categoryRows) {
    MakeLabelRow(ctx, base, row);
  }
  for (uint32_t row : g_emptyRows) {
    MakeLabelRow(ctx, base, row);
  }
  for (uint32_t row : g_settingRows) {
    CallGuest(ctx, base, __imp__sub_83691298, {row});
  }
  return true;
}

// A setting's row set up in a row slot of ours.
void SetUpSettingRow(PPCContext& ctx, uint8_t* base, uint32_t setting, uint32_t slot) {
  const Setting& that = kSettings[setting];
  uint32_t row = g_settingRows[setting];
  InitRow(ctx, base, row, slot);
  bool one = g_valueText[setting] != 0;
  CallGuest(ctx, base, __imp__sub_83692EC0, {row, kAnyText, g_ids, one ? 1u : that.count, 0});
  Store32(row + kRowLabel, g_labelText[setting]);
  if (one) {
    SetGuestText(g_valueText[setting], ArrowText(that));
    Store32(row + kChoiceText, g_valueText[setting]);
  } else {
    for (uint32_t k = 0; k < that.count; k++) {
      Store32(row + kChoiceText + 0x100 * k, g_choiceTexts[setting][k]);
    }
  }
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

// The in-game list has a slot for a page's Restore Defaults after its rows
// unless they fill the column (the title's pages still have it).
bool InGameRestore(const Page& page) {
  return kFirstPageRow + page.count < kGameRows;
}
static_assert(std::all_of(std::begin(kPages), std::end(kPages),
                          [](const Page& p) { return kFirstPageRow + p.count <= kGameRows; }));

// Restore Defaults after a page's rows, and the empty rows after it.
void SetUpPageEnd(PPCContext& ctx, uint8_t* base, uint32_t restoreSlot) {
  if (restoreSlot < kGameRows) {
    InitRow(ctx, base, g_restoreRow, restoreSlot);
    Store32(g_restoreRow + kRowLabel, g_restoreText);
  }
  for (uint32_t i = 0; i < std::size(g_emptyRows); i++) {
    InitRow(ctx, base, g_emptyRows[i], restoreSlot + 1 + i, true);
    Store32(g_emptyRows[i] + kRowLabel, g_emptyText);
  }
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

// The shown page's values into its rows (by their slots).
void PushValues(PPCContext& ctx, uint8_t* base) {
  const Page& page = kPages[g_category];
  for (uint32_t j = 0; j < page.count; j++) {
    uint32_t i = page.rows[j].setting;
    if (g_valueText[i]) {
      SetGuestText(g_valueText[i], ArrowText(kSettings[i]));
    }
    CallGuest(ctx, base, __imp__sub_83691458,
              {kFirstPageRow + j, g_valueText[i] ? 0 : ValueOf(kSettings[i])});
  }
}

void SetListRows(const uint32_t* rows) {
  for (uint32_t i = 0; i < kGameRows; i++) {
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

// A page row (in slot kFirstPageRow + line) moved to the right column, at
// that line from the top.
float PageRowShiftY() {
  return -float(kFirstPageRow) * LoadFloat(kLineHeight);
}

void ShiftSprite(uint32_t sprite, bool fromZero) {
  float x = fromZero ? 0.0f : LoadFloat(sprite + kSpriteOffset);
  float y = fromZero ? 0.0f : LoadFloat(sprite + kSpriteOffset + 4);
  StoreFloat(sprite + kSpriteOffset, x + kPageColumn);
  StoreFloat(sprite + kSpriteOffset + 4, y + PageRowShiftY());
}

// Right after the row's setup (for a choice row).
void PlacePageRow(uint32_t row, bool choices) {
  ShiftSprite(row + kRowSprite, true);
  if (choices) {
    uint32_t count = std::min<uint32_t>(ng2::guest::Load32(row + kChoiceCount), kChoicesShown);
    for (uint32_t k = 0; k < count; k++) {
      ShiftSprite(row + kChoiceItems + kChoiceItemSize * k, true);
      if (k + 1 < count) {
        ShiftSprite(row + kStrokes + kStrokeSize * k, false);
      }
    }
  }
}

// Our layout in the list, with a category's page (its rows set up again
// for the slots they're shown in); or the game's rows back. Moving between
// categories leaves the category rows as they are, as the title's list.
void ShowInGameRows(PPCContext& ctx, uint8_t* base, bool ours, uint32_t category = 0,
                    bool categoriesShown = false) {
  if (!ours) {
    ArrangeGameRows();
    g_inGameOurs = false;
    ng2::video::SetInGameRecompHeader(false);
    return;
  }
  g_category = category;
  const Page& page = kPages[category];
  uint32_t rows[kGameRows];
  for (uint32_t i = 0; i < kPageCount; i++) {
    rows[i] = g_categoryRows[i];
  }
  for (uint32_t j = 0; j < page.count; j++) {
    SetUpSettingRow(ctx, base, page.rows[j].setting, kFirstPageRow + j);
    rows[kFirstPageRow + j] = g_settingRows[page.rows[j].setting];
    PlacePageRow(rows[kFirstPageRow + j], true);
  }
  uint32_t restoreSlot = kFirstPageRow + page.count;
  SetUpPageEnd(ctx, base, restoreSlot);
  if (InGameRestore(page)) {
    rows[restoreSlot] = g_restoreRow;
    PlacePageRow(g_restoreRow, false);
  }
  for (uint32_t i = restoreSlot + 1; i < kGameRows; i++) {
    rows[i] = g_emptyRows[i - restoreSlot - 1];
  }
  SetListRows(rows);
  // The 13th stays: the move off our row still names index 12.
  g_inGameOurs = true;
  ng2::video::SetInGameRecompHeader(true);
  // Event 1 (as the list's rows get when it opens), to ours only: the
  // list's own sends it to its view too, which closes the menu.
  for (uint32_t i = categoriesShown ? kFirstPageRow : 0; i < kGameRows; i++) {
    CallGuest(ctx, base, __imp__sub_83692378, {rows[i], 1, kInGameModel});
  }
  // A row shows a value once it changes: the slots may hold another
  // page's, so each is changed twice.
  for (uint32_t j = 0; j < page.count; j++) {
    CallGuest(ctx, base, __imp__sub_83691458, {kFirstPageRow + j, 0xFF});
  }
  PushValues(ctx, base);
}

void LeaveInGameRows(PPCContext& ctx, uint8_t* base) {
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

// A choice row's update: the choice mark of ours, placed again whenever the
// game places it, goes with the row; "< value >" rows have none, as the
// title's arrow lists.
REX_EXTERN(__imp__sub_836927A8);
REX_HOOK_RAW(sub_836927A8) {
  uint32_t row = ctx.r3.u32;
  uint32_t state = ng2::guest::Load32(row + 4);
  __imp__sub_836927A8(ctx, base);
  if (!g_inGameOurs || (state != 1 && state != 3)) {
    return;
  }
  for (uint32_t i = 0; i < std::size(kSettings); i++) {
    if (g_settingRows[i] == row) {
      if (g_valueText[i]) {
        StoreFloat(row + kChoiceMark + kSpriteOffset + 4, -10000.0f);
      } else {
        ShiftSprite(row + kChoiceMark, false);
      }
    }
  }
}

// The list's methods over its rows: ours too.
REX_EXTERN(__imp__sub_83695610);
REX_HOOK_RAW(sub_83695610) {  // reset
  uint32_t list = ctx.r3.u32;
  __imp__sub_83695610(ctx, base);
  if (list == g_list && !g_inGameOurs) {
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
  if (!g_inGameOurs) {
    uint32_t row = ng2::guest::Load32(g_list + kRowsAt + 4 * kGameRows);
    CallAt(ctx, base, Method(row, 2), {row});
    return;
  }
  // The cursor mark at our row: a category, or a page row in the right
  // column (the list placed it by the heights of the rows above its slot).
  float origin = LoadFloat(kMarkOrigin), height = LoadFloat(kLineHeight);
  uint32_t line = g_inPage ? ng2::guest::Load32(kInGameCursors) - kFirstPageRow : g_category;
  StoreFloat(list + kCursorMark + kMarkX, origin + (g_inPage ? kPageColumn : 0.0f));
  StoreFloat(list + kCursorMark + kMarkY, std::floor(origin + float(line) * height));
}
REX_HOOK_RAW(sub_83695800) {  // event 1 to every row
  uint32_t list = ctx.r3.u32, model = ctx.r4.u32;
  __imp__sub_83695800(ctx, base);
  if (list == g_list && !g_inGameOurs) {
    uint32_t row = ng2::guest::Load32(g_list + kRowsAt + 4 * kGameRows);
    CallGuest(ctx, base, __imp__sub_83692378, {row, 1, model});
  }
}
REX_EXTERN(__imp__sub_83695748);
REX_HOOK_RAW(sub_83695748) {  // draw: the rows one under another
  uint32_t list = ctx.r3.u32;
  uint64_t layer = ctx.r4.u64, position = ctx.r5.u64;
  __imp__sub_83695748(ctx, base);
  if (list == g_list && !g_inGameOurs) {
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
  if (g_suppress & (1u << ctx.r7.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823E3828(ctx, base);
}
REX_HOOK_RAW(sub_823E4158) {
  if (g_suppress & 0x10) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823E4158(ctx, base);
}

// The list's handler: moves onto and off our row, and our settings while
// they're shown.
REX_EXTERN(__imp__sub_823E8720);
REX_HOOK_RAW(sub_823E8720) {
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
    if (!g_inPage) {
      // The categories: up and down show one's page, Confirm or right go
      // into it, Back leaves.
      if (back) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
        LeaveInGameRows(ctx, base);
        return;
      }
      uint32_t category = g_category;
      if (pressed(0)) {
        category = (category + kPageCount - 1) % kPageCount;
      }
      if (pressed(1)) {
        category = (category + 1) % kPageCount;
      }
      if (category != g_category) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
        ShowInGameRows(ctx, base, true, category, true);
      }
      cursor = g_category;
      if (pressed(2) || CallGuest(ctx, base, __imp__sub_823E4158, {kPadState, cursor, 0, 0})) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
        g_inPage = true;
        cursor = kFirstPageRow;
      }
    } else if (back) {
      CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
      g_inPage = false;
      cursor = g_category;
    } else {
      // A page: up and down pick a row, left and right change it, Y sets its
      // default, Confirm on Restore Defaults sets the page's.
      const Page& page = kPages[g_category];
      uint32_t rowCount = page.count + (InGameRestore(page) ? 1 : 0);
      uint32_t row = std::min(cursor - std::min(cursor, kFirstPageRow), rowCount - 1);
      uint32_t moved = row;
      if (pressed(0)) {
        moved = (moved + rowCount - 1) % rowCount;
      }
      if (pressed(1)) {
        moved = (moved + 1) % rowCount;
      }
      if (moved != row) {
        CallGuest(ctx, base, __imp__sub_8242AFC0, {2}, sound);
        row = moved;
      }
      cursor = kFirstPageRow + row;
      if (row == page.count) {
        if (CallGuest(ctx, base, __imp__sub_823E4158, {kPadState, cursor, 0, 0})) {
          CallGuest(ctx, base, __imp__sub_8242AFC0, {3}, sound);
          for (uint32_t i = 0; i < page.count; i++) {
            RestoreDefault(kSettings[page.rows[i].setting]);
          }
        }
      } else {
        const Setting& setting = kSettings[page.rows[row].setting];
        uint32_t value = ValueOf(setting), changed = value;
        if (pressed(3)) {
          changed = (changed + setting.count - 1) % setting.count;
        }
        if (pressed(2)) {
          changed = (changed + 1) % setting.count;
        }
        if (changed != value) {
          CallGuest(ctx, base, __imp__sub_8242AFC0, {3}, sound);
          SetValue(setting, changed);
        } else if (CallGuest(ctx, base, __imp__sub_823E43B0, {kPadState})) {
          RestoreDefault(setting);
        }
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
    NotifyCursor(ctx, base, cursor);
    PushValues(ctx, base);
    return;
  }

  // The game's list, our row shown after Save Method: the moves onto and
  // off it are made here, in the order shown, the rest by the game's
  // handler in its own indices.
  bool up = pressed(0), down = pressed(1);
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
    if (CallGuest(ctx, base, __imp__sub_823E4158, {kPadState, cursor, 0, 0})) {
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
