// ng2 - Ninja Gaiden II static recompilation
//
// Button prompts (prompts.h). Every button the game shows, in the HUD and in
// its menus' texts (the @[pad:...] tags), is drawn from one 512x256 8-bit
// atlas (X, Y, B, A, the triggers and shoulders, the sticks, the d-pad, BACK
// and START) looked up in a 256x1 palette. Both are told apart by their
// content as the renderer loads them (CreateTexture); for the PlayStation,
// Nintendo, Steam Deck or keyboard prompts the button cells are painted over with that
// set's pictures (prompt_sets.inc, from Xelu's CC0 prompts; the keyboard's are
// the keys bound to each button) and the set's colours written into the
// palette entries no other part of the atlas uses. The sticks' and the
// d-pad's pictures stay the game's. Nothing of the game's is shipped: only
// which cells and palette entries these are. The renderer checks both every
// frame with Generation() in their hash, so a change shows at once.

#include "prompts.h"
#include "../menu_mouse.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../controls.h"
#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_INT32(button_prompts, 0, "Display",
                     "0: Auto (the device in use), 1: Xbox, 2: PlayStation, 3: Nintendo, 4: Keyboard, "
                     "5: Steam Deck");

namespace ng2::prompts {

namespace {

struct PromptCell {
  uint16_t x, y, w, h;
};
#include "prompt_sets.inc"

constexpr uint64_t kAtlasHash = 0xEAF07C3B5DAB0BA1ull, kPaletteHash = 0xD1C855C91D4CEB65ull;
constexpr uint32_t kAtlasWidth = 512, kAtlasHeight = 256;
// The cells' surroundings cleared too (the game's shadows).
constexpr int kMargin = 8;
// The palette entries only the cleared cells use (from the game's atlas).
constexpr uint8_t kFree[] = {24,  35,  41,  60,  64,  67,  75,  76,  81,  84,  86,  91,  95,
                             97,  98,  99,  103, 110, 112, 113, 116, 121, 126, 127, 130, 131,
                             135, 136, 141, 144, 146, 149, 151, 158, 160, 162, 164, 167, 170,
                             172, 173, 177, 179, 180, 186, 188, 193, 198, 199, 203, 210, 212,
                             215, 218, 222, 231, 239, 244, 246, 247, 248, 251};
constexpr size_t kCellCount = sizeof(kPromptCells) / sizeof(kPromptCells[0]);

struct SetData {
  const uint8_t (*colors)[4];
  size_t colorCount;
  const uint8_t* const* cells;
};

SetData Data(Set set) {
  switch (set) {
    case Set::kPlayStation:
      return {kPlayStationColors, sizeof(kPlayStationColors) / 4, kPlayStationCells};
    case Set::kNintendo:
      return {kNintendoColors, sizeof(kNintendoColors) / 4, kNintendoCells};
    case Set::kSteamDeck:
      return {kSteamDeckColors, sizeof(kSteamDeckColors) / 4, kSteamDeckCells};
    default:
      return {nullptr, 0, nullptr};
  }
}

static_assert(sizeof(kPlayStationColors) / 4 <= sizeof(kFree));
static_assert(sizeof(kNintendoColors) / 4 <= sizeof(kFree));
static_assert(sizeof(kSteamDeckColors) / 4 <= sizeof(kFree));
static_assert(sizeof(kKeyboardColors) / 4 <= sizeof(kFree));

// The atlas's cells, in kPromptCells' order: the button whose keys a keyboard
// cell shows, and the shape of its key picture.
enum class Shape { kSquare, kTall, kWide };
struct CellButton {
  const char* bind;
  Shape shape;
};
constexpr CellButton kCellButtons[] = {
    {"keybind_x", Shape::kSquare},           {"keybind_y", Shape::kSquare},
    {"keybind_b", Shape::kSquare},           {"keybind_a", Shape::kSquare},
    {"keybind_left_trigger", Shape::kTall},  {"keybind_right_trigger", Shape::kTall},
    {"keybind_left_shoulder", Shape::kWide}, {"keybind_right_shoulder", Shape::kWide},
    {"keybind_lstick_press", Shape::kSquare}, {"keybind_rstick_press", Shape::kSquare},
    {"keybind_back", Shape::kSquare},        {"keybind_start", Shape::kSquare},
};
static_assert(sizeof(kCellButtons) / sizeof(kCellButtons[0]) == kCellCount);

// The first key bound to a button, its modifiers left out ("Shift+Up": Up).
std::string FirstKey(const char* bind) {
  std::string keys = rex::cvar::GetFlagByName(bind);
  std::string first = keys.substr(0, keys.find(','));
  size_t plus = first.rfind('+');
  if (plus != std::string::npos && plus + 1 < first.size()) {
    first = first.substr(plus + 1);
  }
  while (!first.empty() && first.front() == ' ') {
    first.erase(first.begin());
  }
  while (!first.empty() && first.back() == ' ') {
    first.pop_back();
  }
  return first;
}

const KeyPictures& Key(const std::string& name) {
  for (const KeyPictures& key : kKeys) {
    if (name == key.name) {
      return key;
    }
  }
  return kKeys[sizeof(kKeys) / sizeof(kKeys[0]) - 1];  // the blank key
}

Set Chosen() {
  // NG2_PROMPTS=ps / nintendo / steamdeck / keys / xbox overrides the setting (tests).
  if (const char* v = std::getenv("NG2_PROMPTS"); v && *v) {
    switch (v[0]) {
      case 'p': case 'P': return Set::kPlayStation;
      case 'n': case 'N': return Set::kNintendo;
      case 'k': case 'K': return Set::kKeyboard;
      case 's': case 'S': return Set::kSteamDeck;
      default: return Set::kXbox;
    }
  }
  switch (REXCVAR_GET(button_prompts)) {
    case 1: return Set::kXbox;
    case 2: return Set::kPlayStation;
    case 3: return Set::kNintendo;
    case 4: return Set::kKeyboard;
    case 5: return Set::kSteamDeck;
    default:
      switch (controls::ActiveDevice()) {
        case controls::Device::kPlayStation: return Set::kPlayStation;
        case controls::Device::kNintendo: return Set::kNintendo;
        case controls::Device::kKeyboard: return Set::kKeyboard;
        case controls::Device::kSteamDeck: return Set::kSteamDeck;
        default: return Set::kXbox;
      }
  }
}

// The key a cell shows: in a menu, the menus' own for A, B and Y (Return,
// Backspace, Delete: menu_mouse.cpp) and no mouse button (the mouse's are
// the menus' there), else the first one bound.
std::string ShownKey(const char* bind) {
  if (!ng2::menu_mouse::InMenu()) {
    return FirstKey(bind);
  }
  std::string_view name = bind;
  if (name == "keybind_a") {
    return "Return";
  }
  if (name == "keybind_b") {
    return "Backspace";
  }
  if (name == "keybind_y") {
    return "Delete";
  }
  std::string keys = rex::cvar::GetFlagByName(bind);
  for (size_t at = 0; at <= keys.size();) {
    size_t comma = std::min(keys.find(',', at), keys.size());
    std::string key = keys.substr(at, comma - at);
    if (size_t plus = key.rfind('+'); plus != std::string::npos && plus + 1 < key.size()) {
      key = key.substr(plus + 1);
    }
    if (!key.empty() && key != "LMB" && key != "RMB" && key != "MMB") {
      return key;
    }
    at = comma + 1;
  }
  return FirstKey(bind);
}

std::atomic<uint32_t> g_generation{0};
std::mutex g_mutex;
Set g_shown = Set::kXbox;          // (g_mutex)
std::string g_shownKeys;           // (g_mutex) the keyboard set's keys
std::string BoundKeys() {
  std::string keys;
  for (const CellButton& cell : kCellButtons) {
    keys += ShownKey(cell.bind) + ',';
  }
  return keys;
}

}  // namespace

Set CurrentSet() {
  std::lock_guard lock(g_mutex);
  return g_shown;
}

uint32_t Generation() {
  Set set = Chosen();
  std::string keys = set == Set::kKeyboard ? BoundKeys() : std::string();
  std::lock_guard lock(g_mutex);
  if (set != g_shown || keys != g_shownKeys) {
    g_shown = set;
    g_shownKeys = std::move(keys);
    g_generation++;
  }
  return g_generation.load();
}

bool Apply(uint32_t width, uint32_t height, Kind kind, uint8_t* data, uint64_t bytes) {
  bool atlas = kind == Kind::kIndex8 && width == kAtlasWidth && height == kAtlasHeight &&
               bytes >= uint64_t(width) * height &&
               XXH3_64bits(data, size_t(width) * height) == kAtlasHash;
  bool palette = !atlas && kind == Kind::kRGBA8 && width == 256 && height == 1 && bytes >= 1024 &&
                 XXH3_64bits(data, 1024) == kPaletteHash;
  if (!atlas && !palette) {
    return false;
  }
  Set set = CurrentSet();
  if (set == Set::kXbox) {
    return true;  // the game's own
  }
  if (set == Set::kKeyboard && atlas) {
    for (size_t c = 0; c < kCellCount; c++) {
      const PromptCell& cell = kPromptCells[c];
      for (int y = int(cell.y) - kMargin; y < int(cell.y + cell.h) + kMargin; y++) {
        for (int x = int(cell.x) - kMargin; x < int(cell.x + cell.w) + kMargin; x++) {
          if (x >= 0 && y >= 0 && x < int(width) && y < int(height)) {
            data[size_t(y) * width + x] = 0;
          }
        }
      }
      const KeyPictures& key = Key(ShownKey(kCellButtons[c].bind));
      Shape shape = kCellButtons[c].shape;
      const uint8_t* pixels = shape == Shape::kTall ? key.tall : shape == Shape::kWide ? key.wide : key.square;
      uint32_t w = shape == Shape::kTall ? kTallW : shape == Shape::kWide ? kWideW : kSquareW;
      uint32_t h = shape == Shape::kTall ? kTallH : shape == Shape::kWide ? kWideH : kSquareH;
      // Centred on the cell, within its cleared surroundings.
      int x0 = int(cell.x) + (int(cell.w) - int(w)) / 2, y0 = int(cell.y) + (int(cell.h) - int(h)) / 2;
      for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
          int ax = x0 + int(x), ay = y0 + int(y);
          uint8_t v = pixels[y * w + x];
          if (v && ax >= 0 && ay >= 0 && ax < int(width) && ay < int(height)) {
            data[size_t(ay) * width + ax] = kFree[v - 1];
          }
        }
      }
    }
    return true;
  }
  if (atlas) {
    SetData d = Data(set);
    for (size_t c = 0; c < kCellCount; c++) {
      const PromptCell& cell = kPromptCells[c];
      for (int y = int(cell.y) - kMargin; y < int(cell.y + cell.h) + kMargin; y++) {
        for (int x = int(cell.x) - kMargin; x < int(cell.x + cell.w) + kMargin; x++) {
          if (x >= 0 && y >= 0 && x < int(width) && y < int(height)) {
            data[size_t(y) * width + x] = 0;
          }
        }
      }
      const uint8_t* pixels = d.cells[c];
      for (uint32_t y = 0; y < cell.h; y++) {
        for (uint32_t x = 0; x < cell.w; x++) {
          uint8_t v = pixels[y * cell.w + x];
          data[size_t(cell.y + y) * width + cell.x + x] = v ? kFree[v - 1] : 0;
        }
      }
    }
    return true;
  }
  {
    // The game samples the palette swizzled (03012): stored B, G, R, A.
    SetData d = set == Set::kKeyboard ? SetData{kKeyboardColors, sizeof(kKeyboardColors) / 4, nullptr}
                                      : Data(set);
    for (size_t i = 0; i < d.colorCount; i++) {
      uint8_t* entry = data + kFree[i] * 4;
      entry[0] = d.colors[i][2];
      entry[1] = d.colors[i][1];
      entry[2] = d.colors[i][0];
      entry[3] = d.colors[i][3];
    }
    return true;
  }
  return false;
}

}  // namespace ng2::prompts
