// ng2 - Ninja Gaiden II static recompilation
//
// Button prompts (prompts.h). Every button the game shows, in the HUD and in
// its menus' texts (the @[pad:...] tags), is drawn from one 512x256 8-bit
// atlas (X, Y, B, A, the triggers and shoulders, the sticks, the d-pad, BACK
// and START) looked up in a 256x1 palette. Both are told apart by their
// content as the renderer loads them (CreateTexture); for the PlayStation,
// Nintendo or keyboard prompts the button cells are painted over with that
// set's pictures (the keyboard's: the keys bound to each button) and the set's
// colours written into the palette entries no other part of the atlas uses.
// The pictures are made from the game's own buttons (its discs, plates and
// ovals, relettered: tools/make_prompts.py, tools/prompt_art.py), so they
// are shipped masked (prompt_sets.inc): XORed with ChaCha20 streams keyed by
// hashes of the game's own atlas and palette, made here as they load. Without
// the game's textures the data is noise. The sticks (LS, RS) stay the game's
// own in the PlayStation and Nintendo sets. The renderer checks
// both every frame with Generation() in their hash, so a change shows at once.
// Muramasa's shop draws its tab icons (LT, RT) from textures of its own,
// copies of the atlas's plates: those get the set's too (ApplyShopIcon, #148).
// NG2_DUMP_PROMPT_ATLAS=<dir> writes the game's atlas and palette there
// (atlas.bin, palette.bin: make_prompts.py's input).

#include "prompts.h"
#include "../aspect.h"
#include "../menu_mouse.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../controls.h"
#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_INT32(button_prompts, 0, "Display",
                     "0: Auto (the device in use), 1: Xbox, 2: PlayStation, 3: Nintendo, 4: Keyboard");

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
    default:
      return {nullptr, 0, nullptr};
  }
}

static_assert(sizeof(kPlayStationColors) / 4 <= sizeof(kFree));
static_assert(sizeof(kNintendoColors) / 4 <= sizeof(kFree));
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

// The bound key's picture (its index in kKeys: its masks' stream).
size_t Key(const std::string& name) {
  constexpr size_t count = sizeof(kKeys) / sizeof(kKeys[0]);
  for (size_t k = 0; k + 1 < count; k++) {
    if (name == kKeys[k].name) {
      return k;
    }
  }
  return count - 1;  // the blank key
}

// The masks (make_prompts.py's mask()): ChaCha20 keystreams (RFC 8439), one
// per stream (its nonce), under a 256-bit key hashed from the game's texture
// (XXH3-128 with seeds kMaskSeed and kMaskSeed + 1). A cipher: the parts of a
// picture known to be clear give away nothing of the key or the rest.
constexpr uint64_t kMaskSeed = 0x4E47325052ull;
struct MaskKey {
  uint32_t w[8] = {};
  bool operator==(const MaskKey&) const = default;
};
MaskKey KeyOf(const uint8_t* data, size_t size) {
  MaskKey key;
  for (int h = 0; h < 2; h++) {
    XXH128_hash_t v = XXH3_128bits_withSeed(data, size, kMaskSeed + h);
    key.w[h * 4 + 0] = uint32_t(v.low64);
    key.w[h * 4 + 1] = uint32_t(v.low64 >> 32);
    key.w[h * 4 + 2] = uint32_t(v.high64);
    key.w[h * 4 + 3] = uint32_t(v.high64 >> 32);
  }
  return key;
}
void ChaChaBlock(const uint32_t in[16], uint8_t out[64]) {
  uint32_t x[16];
  std::memcpy(x, in, sizeof(x));
  auto rotl = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
  auto quarter = [&](int a, int b, int c, int d) {
    x[a] += x[b], x[d] = rotl(x[d] ^ x[a], 16);
    x[c] += x[d], x[b] = rotl(x[b] ^ x[c], 12);
    x[a] += x[b], x[d] = rotl(x[d] ^ x[a], 8);
    x[c] += x[d], x[b] = rotl(x[b] ^ x[c], 7);
  };
  for (int round = 0; round < 10; round++) {
    quarter(0, 4, 8, 12), quarter(1, 5, 9, 13), quarter(2, 6, 10, 14), quarter(3, 7, 11, 15);
    quarter(0, 5, 10, 15), quarter(1, 6, 11, 12), quarter(2, 7, 8, 13), quarter(3, 4, 9, 14);
  }
  for (int i = 0; i < 16; i++) {
    uint32_t v = x[i] + in[i];
    out[i * 4 + 0] = uint8_t(v), out[i * 4 + 1] = uint8_t(v >> 8);
    out[i * 4 + 2] = uint8_t(v >> 16), out[i * 4 + 3] = uint8_t(v >> 24);
  }
}
// Byte i of a stream (blocks made as they are reached).
class Mask {
 public:
  Mask(const MaskKey& key, uint64_t stream) {
    state_[0] = 0x61707865, state_[1] = 0x3320646E, state_[2] = 0x79622D32, state_[3] = 0x6B206574;
    std::memcpy(state_ + 4, key.w, sizeof(key.w));
    state_[12] = 0, state_[13] = 0, state_[14] = uint32_t(stream), state_[15] = uint32_t(stream >> 32);
  }
  uint8_t operator[](size_t i) {
    if (i / 64 != block_) {
      block_ = i / 64;
      state_[12] = uint32_t(block_);
      ChaChaBlock(state_, bytes_);
    }
    return bytes_[i % 64];
  }

 private:
  uint32_t state_[16];
  size_t block_ = ~size_t(0);
  uint8_t bytes_[64];
};

Set Chosen() {
  // NG2_PROMPTS=ps / nintendo / keys / xbox overrides the setting (tests).
  if (const char* v = std::getenv("NG2_PROMPTS"); v && *v) {
    switch (v[0]) {
      case 'p': case 'P': return Set::kPlayStation;
      case 'n': case 'N': return Set::kNintendo;
      case 'k': case 'K': return Set::kKeyboard;
      default: return Set::kXbox;
    }
  }
  switch (REXCVAR_GET(button_prompts)) {
    case 1: return Set::kXbox;
    case 2: return Set::kPlayStation;
    case 3: return Set::kNintendo;
    case 4: return Set::kKeyboard;
    default:
      switch (controls::ActiveDevice()) {
        case controls::Device::kPlayStation: return Set::kPlayStation;
        case controls::Device::kNintendo: return Set::kNintendo;
        case controls::Device::kKeyboard: return Set::kKeyboard;
        default: return Set::kXbox;
      }
  }
}

// The key a cell shows: in a menu, the menus' own for A, B and Y (Return,
// Backspace, Delete: menu_mouse.cpp; Delete X in the Controls table) and no
// mouse button (the mouse's are the menus' there), but play's X and Y in the
// in-game manual; else the first one bound.
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
  // The in-game manual's X and Y: play's keys, mouse buttons too (its pages
  // show the attacks' buttons; nothing there takes X or Y). Elsewhere Delete
  // is Y (the load screen's Delete), but X in the Controls table (its Clear).
  bool deleteClears = ng2::menu_mouse::DeleteClearsNow();
  bool manual = !deleteClears && ng2::menu_mouse::InStage() && ng2::aspect::ManualShown();
  if (manual && (name == "keybind_x" || name == "keybind_y")) {
    return FirstKey(bind);
  }
  if (name == (deleteClears ? "keybind_x" : "keybind_y")) {
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
// The masks' keys, from the game's atlas and palette as they load (0: not
// loaded yet). The shop's trigger icons need both.
// (g_mutex; g_haveKeys: bit 0 the atlas's, bit 1 the palette's)
MaskKey g_atlasKey, g_paletteKey;
std::atomic<uint32_t> g_haveKeys{0};

// Muramasa's shop's tab icons (#148): the atlas's LT and RT plates with their
// shadows, copied 1:1 into 64x64 RGBA textures of their own (the plate's cell
// at (16 + 6, 4 + 6)). Drawn with the set's LT / RT pictures.
constexpr uint64_t kShopIconHash[2] = {0x7A92BB61BC5298FBull, 0x37077F2C4524A232ull};  // LT, RT
constexpr int kShopIconSize = 64, kShopIconX = 16, kShopIconY = 4;
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

// NG2_DUMP_PROMPT_ATLAS=<dir>: the game's atlas and palette, once each, as
// loaded (make_prompts.py's input).
static void DumpOnce(bool atlas, const uint8_t* data) {
  static std::atomic<bool> done[2];
  const char* dir = std::getenv("NG2_DUMP_PROMPT_ATLAS");
  if (!dir || !*dir || done[atlas].exchange(true)) {
    return;
  }
  std::string path = std::string(dir) + (atlas ? "/atlas.bin" : "/palette.bin");
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(data, 1, atlas ? size_t(kAtlasWidth) * kAtlasHeight : 1024, f);
    std::fclose(f);
    REXLOG_INFO("prompts: wrote {}", path);
  }
}

// A shop tab icon (0: LT, 1: RT) drawn with the set's picture of that trigger,
// its colours from the set's.
// Without the atlas's and palette's keys yet, the game's own stays.
static bool ApplyShopIcon(int which, uint8_t* data) {
  Set set = CurrentSet();
  if (set == Set::kXbox || g_haveKeys.load() != 3) {
    return true;
  }
  MaskKey akey, pkey;
  {
    std::lock_guard lock(g_mutex);
    akey = g_atlasKey, pkey = g_paletteKey;
  }
  size_t c = 4 + size_t(which);  // kPromptCells' LT, RT
  const PromptCell& cell = kPromptCells[c];
  int w = int(cell.w) + 2 * kPictureMargin, h = int(cell.h) + 2 * kPictureMargin;
  const uint8_t* masked;
  uint64_t stream, colorStream;
  const uint8_t(*colors)[4];
  size_t colorCount;
  if (set == Set::kKeyboard) {
    size_t k = Key(ShownKey(kCellButtons[c].bind));
    masked = kKeys[k].tall;
    stream = 0x1000 + k * 4 + 1;
    colors = kKeyboardColors;
    colorCount = sizeof(kKeyboardColors) / 4;
    colorStream = kKeyStream;
    w = int(kTallW) + 2 * kPictureMargin;
    h = int(kTallH) + 2 * kPictureMargin;
  } else {
    SetData d = Data(set);
    masked = d.cells[c];
    stream = kSetStream[size_t(set)] * 16 + c;
    colors = d.colors;
    colorCount = d.colorCount;
    colorStream = kSetStream[size_t(set)];
  }
  Mask pictureMask(akey, stream), colorMask(pkey, colorStream);
  std::memset(data, 0, size_t(kShopIconSize) * kShopIconSize * 4);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int ax = kShopIconX + x, ay = kShopIconY + y;
      if (ax >= kShopIconSize || ay >= kShopIconSize) {
        continue;
      }
      size_t i = size_t(y) * w + x;
      uint8_t v = masked[i] ^ pictureMask[i];
      if (!v || v > colorCount) {
        continue;
      }
      // As stored, as the palette: the game samples both swizzled alike (03012).
      uint8_t* out = data + (size_t(ay) * kShopIconSize + ax) * 4;
      for (size_t j = 0; j < 4; j++) {
        out[j] = colors[v - 1][j] ^ colorMask[(v - 1) * 4 + j];
      }
    }
  }
  return true;
}

bool Apply(uint32_t width, uint32_t height, Kind kind, uint8_t* data, uint64_t bytes) {
  bool atlas = kind == Kind::kIndex8 && width == kAtlasWidth && height == kAtlasHeight &&
               bytes >= uint64_t(width) * height &&
               XXH3_64bits(data, size_t(width) * height) == kAtlasHash;
  bool palette = !atlas && kind == Kind::kRGBA8 && width == 256 && height == 1 && bytes >= 1024 &&
                 XXH3_64bits(data, 1024) == kPaletteHash;
  int shopIcon = -1;
  if (!atlas && !palette && kind == Kind::kRGBA8 && width == kShopIconSize && height == kShopIconSize &&
      bytes >= uint64_t(width) * height * 4) {
    uint64_t hash = XXH3_64bits(data, size_t(width) * height * 4);
    shopIcon = hash == kShopIconHash[0] ? 0 : hash == kShopIconHash[1] ? 1 : -1;
  }
  if (!atlas && !palette && shopIcon < 0) {
    return false;
  }
  if (shopIcon >= 0) {
    return ApplyShopIcon(shopIcon, data);
  }
  DumpOnce(atlas, data);
  // The keys, once: the first load of either lets the shop's icons be drawn (a new generation).
  MaskKey key = KeyOf(data, atlas ? size_t(width) * height : 1024);
  {
    std::lock_guard lock(g_mutex);
    MaskKey& stored = atlas ? g_atlasKey : g_paletteKey;
    uint32_t bit = atlas ? 1 : 2;
    if (!(g_haveKeys.load() & bit) || !(stored == key)) {
      stored = key;
      g_haveKeys |= bit;
      g_generation++;
    }
  }
  Set set = CurrentSet();
  if (set == Set::kXbox) {
    return true;  // the game's own
  }
  if (palette) {
    // The colours as stored (B, G, R, A: the game samples the palette swizzled, 03012).
    SetData d = set == Set::kKeyboard ? SetData{kKeyboardColors, sizeof(kKeyboardColors) / 4, nullptr}
                                      : Data(set);
    uint64_t stream = set == Set::kKeyboard ? kKeyStream : kSetStream[size_t(set)];
    Mask mask(key, stream);
    for (size_t i = 0; i < d.colorCount; i++) {
      uint8_t* entry = data + kFree[i] * 4;
      for (size_t j = 0; j < 4; j++) {
        entry[j] = d.colors[i][j] ^ mask[i * 4 + j];
      }
    }
    return true;
  }
  // The PlayStation and Nintendo sets keep the game's own sticks (LS, RS: no
  // picture of theirs); they use none of kFree's entries.
  const uint8_t* const* setCells = set == Set::kKeyboard ? nullptr : Data(set).cells;
  for (size_t c = 0; c < kCellCount; c++) {
    if (setCells && !setCells[c]) {
      continue;
    }
    const PromptCell& cell = kPromptCells[c];
    for (int y = int(cell.y) - kMargin; y < int(cell.y + cell.h) + kMargin; y++) {
      for (int x = int(cell.x) - kMargin; x < int(cell.x + cell.w) + kMargin; x++) {
        if (x >= 0 && y >= 0 && x < int(width) && y < int(height)) {
          data[size_t(y) * width + x] = 0;
        }
      }
    }
  }
  size_t colorCount = set == Set::kKeyboard ? sizeof(kKeyboardColors) / 4 : Data(set).colorCount;
  // A picture of w x h (its cell and the picture margin around it) at (x0, y0).
  auto paint = [&](const uint8_t* masked, uint64_t stream, int w, int h, int x0, int y0) {
    Mask mask(key, stream);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        int ax = x0 + x, ay = y0 + y;
        if (ax < 0 || ay < 0 || ax >= int(width) || ay >= int(height)) {
          continue;
        }
        size_t i = size_t(y) * w + x;
        uint8_t v = masked[i] ^ mask[i];
        if (v && v <= colorCount) {
          data[size_t(ay) * width + ax] = kFree[v - 1];
        }
      }
    }
  };
  if (set == Set::kKeyboard) {
    for (size_t c = 0; c < kCellCount; c++) {
      const PromptCell& cell = kPromptCells[c];
      size_t k = Key(ShownKey(kCellButtons[c].bind));
      Shape shape = kCellButtons[c].shape;
      const uint8_t* masked = shape == Shape::kTall   ? kKeys[k].tall
                              : shape == Shape::kWide ? kKeys[k].wide
                                                      : kKeys[k].square;
      uint64_t stream = 0x1000 + k * 4 + (shape == Shape::kTall ? 1 : shape == Shape::kWide ? 2 : 0);
      int w = int(shape == Shape::kTall ? kTallW : shape == Shape::kWide ? kWideW : kSquareW) + 2 * kPictureMargin;
      int h = int(shape == Shape::kTall ? kTallH : shape == Shape::kWide ? kWideH : kSquareH) + 2 * kPictureMargin;
      // Centred on the cell, within its cleared surroundings.
      int x0 = int(cell.x) + (int(cell.w) + 2 * kPictureMargin - w) / 2 - kPictureMargin;
      int y0 = int(cell.y) + (int(cell.h) + 2 * kPictureMargin - h) / 2 - kPictureMargin;
      paint(masked, stream, w, h, x0, y0);
    }
    return true;
  }
  SetData d = Data(set);
  for (size_t c = 0; c < kCellCount; c++) {
    if (!d.cells[c]) {
      continue;
    }
    const PromptCell& cell = kPromptCells[c];
    int x0 = int(cell.x) - kPictureMargin, y0 = int(cell.y) - kPictureMargin;
    paint(d.cells[c], kSetStream[size_t(set)] * 16 + c, int(cell.w) + 2 * kPictureMargin,
          int(cell.h) + 2 * kPictureMargin, x0, y0);
  }
  return true;
}

}  // namespace ng2::prompts
