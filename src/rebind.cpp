// ng2 - Ninja Gaiden II static recompilation
//
// Controls: the keys and the controller's buttons for the game's actions
// (rebind.h; RECOMP SETTINGS' Controls page).
//
// The game binds actions, not buttons: the profile ([0x84C559E0]) holds an
// action id for each of 12 slots at +8 (A, B, X, Y, LB, RB, LT, RT, then
// four two-button pairs; 0 none), and play asks for an action's buttons
// (sub_823A29D0: the masks, table 0x83B0A068, of the slots holding it). Its
// own Controller Settings write Type A/B and the shoulder types there
// (tables 0x83B00C70 / 0x83B00CD0) and into a copy at 0x83C3EBE8; the
// player's cached masks (0x84F12720 + 96 * player) are made from them as a
// stage starts (sub_823ABB68). The menus read the buttons themselves, so
// Confirm and Back stay A and B.
//
// With every binding here at its default the game's layout is left alone.
// Otherwise the slots are written from these before each input update, with
// the cached masks: an action on its button's slot, Water Run and Ninpo on a
// button's slot too or, bound to two buttons, on the first two pair slots.
// Those slots' bits are the game's for A+X and B+Y, made by the character's
// input (sub_823B7470(input, pad record, wanted pair): into the buttons
// pressed, +16, when both of a pair are pressed within two frames, or one
// while the other is held for the pair asked for); for the player's pads
// they are made here instead, from the two buttons bound. The game's prompts
// follow the slots; the pair slots' two letters (0x83AB3618: slot, index,
// letter) are written with them.
//
// The keyboard: each action's two keys press the controller button(s) it is
// bound to, through the SDK's keyboard pad (keybind_*: keys for each
// button), whose lists are made from these once any differs from its
// default; movement is the left stick, the item menus the d-pad, and Return
// and Backspace stay on A and B for the menus. The camera keys and the stick
// clicks are left to keybind_*.

#include "rebind.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "gpu/guest.h"

// A controller binding as rebind.h has it: -1 none, 0-7 a button (A, B, X,
// Y, LB, RB, LT, RT), 0x100 | first << 4 | second two together.
REXCVAR_DEFINE_INT32(pad_jump, 0, "Controls", "Jump's button (-1 none, 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT)");
REXCVAR_DEFINE_INT32(pad_quick_attack, 2, "Controls", "Quick Attack's button");
REXCVAR_DEFINE_INT32(pad_strong_attack, 3, "Controls", "Strong Attack's button");
REXCVAR_DEFINE_INT32(pad_projectile, 1, "Controls", "Projectile Weapon's button");
REXCVAR_DEFINE_INT32(pad_ninpo, 0x113, "Controls", "Ninpo's button, or two (0x100 + 16 * first + second)");
REXCVAR_DEFINE_INT32(pad_water_run, 0x102, "Controls", "Water Run's button, or two");
REXCVAR_DEFINE_INT32(pad_block, 6, "Controls", "Block's button");
REXCVAR_DEFINE_INT32(pad_center_camera, 7, "Controls", "Center Camera's button");
REXCVAR_DEFINE_INT32(pad_first_person, 4, "Controls", "1st-Person View's button");
REXCVAR_DEFINE_INT32(pad_interact, 5, "Controls", "Interact's button");

REXCVAR_DEFINE_STRING(key_jump, "Space", "Controls", "Jump's keys (one or two, comma-separated)");
REXCVAR_DEFINE_STRING(key_quick_attack, "LMB,J", "Controls", "Quick Attack's keys");
REXCVAR_DEFINE_STRING(key_strong_attack, "RMB,K", "Controls", "Strong Attack's keys");
REXCVAR_DEFINE_STRING(key_projectile, "E", "Controls", "Projectile Weapon's keys");
REXCVAR_DEFINE_STRING(key_ninpo, "F", "Controls", "Ninpo's keys");
REXCVAR_DEFINE_STRING(key_water_run, "C", "Controls", "Water Run's keys");
REXCVAR_DEFINE_STRING(key_block, "Shift", "Controls", "Block's keys");
REXCVAR_DEFINE_STRING(key_center_camera, "Q,MMB", "Controls", "Center Camera's keys");
REXCVAR_DEFINE_STRING(key_first_person, "R", "Controls", "1st-Person View's keys");
REXCVAR_DEFINE_STRING(key_interact, "V", "Controls", "Interact's keys");
REXCVAR_DEFINE_STRING(key_move_forward, "W", "Controls", "Move Forward's keys");
REXCVAR_DEFINE_STRING(key_move_back, "S", "Controls", "Move Back's keys");
REXCVAR_DEFINE_STRING(key_move_left, "A", "Controls", "Move Left's keys");
REXCVAR_DEFINE_STRING(key_move_right, "D", "Controls", "Move Right's keys");
REXCVAR_DEFINE_STRING(key_items_up, "Up", "Controls", "The d-pad's up (item menus)");
REXCVAR_DEFINE_STRING(key_items_down, "Down", "Controls", "The d-pad's down");
REXCVAR_DEFINE_STRING(key_items_left, "Left", "Controls", "The d-pad's left");
REXCVAR_DEFINE_STRING(key_items_right, "Right", "Controls", "The d-pad's right");
REXCVAR_DEFINE_STRING(key_inventory, "Escape", "Controls", "START's keys (the inventory)");
REXCVAR_DEFINE_STRING(key_menu, "Tab", "Controls", "BACK's keys (the in-game menu)");

namespace ng2::rebind {

namespace {

using ng2::guest::Load32;

constexpr uint32_t kProfile = 0x84C559E0;
constexpr uint32_t kSlots = 8, kSlotCount = 12;  // in the profile
constexpr uint32_t kSlotsCopy = 0x83C3EBE8;
constexpr uint32_t kMasks = 0x83B0A068;
constexpr uint32_t kCached = 0x84F12720, kCachedSize = 96, kPlayers = 4;
constexpr uint32_t kLayouts = 0x83B00C70, kShoulderLayouts = 0x83B00CD0, kLayoutSize = 48;
constexpr uint32_t kLayoutType = 56, kShoulderType = 57;  // in the profile
// The players' pad records (68 bytes each), then the merged one.
constexpr uint32_t kPadRecords = 0x83C07FF0, kPadRecordSize = 68, kMergedPad = 0x83C08100;
// The prompts' letters for the first two pair slots: 12-byte entries (slot,
// index, a UTF-16 letter), X A for slot 8 and Y B for 9.
constexpr uint32_t kPairLetters = 0x83AB3618 + 12 * 8, kLetter = 8;
constexpr char16_t kButtonLetters[] = {u'A', u'B', u'X', u'Y', u'l', u'r', u'L', u'R'};

// The game's action ids (0: one of ours only, with no slot).
enum Id : uint32_t {
  kNoId = 0, kNinpo = 1, kWaterRun = 2, kJump = 3, kProjectile = 4, kQuick = 5, kStrong = 6,
  kBlock = 7, kCenterCamera = 8, kInteract = 12, kFirstPerson = 13,
};
// The cached masks: the action at each field of a player's.
constexpr std::pair<uint32_t, Id> kCachedFields[] = {
    {8, kQuick}, {12, kProjectile}, {16, kNinpo},  {24, kStrong},
    {28, kJump}, {32, kBlock},      {36, kWaterRun}, {72, kCenterCamera},
};

struct Action {
  const char16_t* name;
  const char* key;       // its keys' setting
  const char* pad;       // its controller binding's (null: fixed)
  Id id;
  const char* button;    // fixed: the keyboard pad's list its keys go to
  const char16_t* fixed;  // and what the page shows for the controller
};
constexpr Action kTable[kActions] = {
    {u"Jump", "key_jump", "pad_jump", kJump},
    {u"Quick Attack", "key_quick_attack", "pad_quick_attack", kQuick},
    {u"Strong Attack", "key_strong_attack", "pad_strong_attack", kStrong},
    {u"Projectile Weapon", "key_projectile", "pad_projectile", kProjectile},
    {u"Ninpo", "key_ninpo", "pad_ninpo", kNinpo},
    {u"Water Run", "key_water_run", "pad_water_run", kWaterRun},
    {u"Block", "key_block", "pad_block", kBlock},
    {u"Center Camera", "key_center_camera", "pad_center_camera", kCenterCamera},
    {u"1st-Person View", "key_first_person", "pad_first_person", kFirstPerson},
    {u"Interact", "key_interact", "pad_interact", kInteract},
    {u"Move Forward", "key_move_forward", nullptr, kNoId, "keybind_lstick_up", u"@[pad:LS]"},
    {u"Move Back", "key_move_back", nullptr, kNoId, "keybind_lstick_down", u"@[pad:LS]"},
    {u"Move Left", "key_move_left", nullptr, kNoId, "keybind_lstick_left", u"@[pad:LS]"},
    {u"Move Right", "key_move_right", nullptr, kNoId, "keybind_lstick_right", u"@[pad:LS]"},
    {u"Item Menu Up", "key_items_up", nullptr, kNoId, "keybind_dpad_up", u"@[pad:Lu]"},
    {u"Item Menu Down", "key_items_down", nullptr, kNoId, "keybind_dpad_down", u"@[pad:Ld]"},
    {u"Item Menu Left", "key_items_left", nullptr, kNoId, "keybind_dpad_left", u"@[pad:Ll]"},
    {u"Item Menu Right", "key_items_right", nullptr, kNoId, "keybind_dpad_right", u"@[pad:Lr]"},
    {u"Inventory", "key_inventory", nullptr, kNoId, "keybind_start", u"@[pad:Start]"},
    {u"In-Game Menu", "key_menu", nullptr, kNoId, "keybind_back", u"@[pad:Select]"},
};

void (*g_store)(const char* name, const std::string& value) = nullptr;

void Store(const char* name, const std::string& value) {
  if (g_store) {
    g_store(name, value);
  } else {
    rex::cvar::SetFlagByName(name, value);
  }
}

std::array<std::string, 2> Keys(int action) {
  std::string list = rex::cvar::GetFlagByName(kTable[action].key);
  std::array<std::string, 2> keys;
  size_t n = 0;
  for (size_t at = 0; at <= list.size() && n < 2;) {
    size_t comma = std::min(list.find(',', at), list.size());
    if (comma > at) {
      keys[n++] = list.substr(at, comma - at);
    }
    at = comma + 1;
  }
  return keys;
}

void StoreKeys(int action, const std::array<std::string, 2>& keys) {
  // (the first slot first)
  const std::string& a = keys[0].empty() ? keys[1] : keys[0];
  const std::string& b = keys[0].empty() ? std::string() : keys[1];
  Store(kTable[action].key, b.empty() ? a : a + "," + b);
}

int PadOf(int action) {
  if (!kTable[action].pad) {
    return kNone;
  }
  int binding = std::atoi(rex::cvar::GetFlagByName(kTable[action].pad).c_str());
  bool pair = IsPair(binding) && PadPairs(action) && First(binding) < 8 && Second(binding) < 8 &&
              First(binding) < Second(binding) && binding < 0x200;
  return pair || (binding >= 0 && binding < 8) ? binding : kNone;
}

bool AllDefault() {
  for (const Action& action : kTable) {
    for (const char* name : {action.key, action.pad}) {
      if (!name) {
        continue;
      }
      const auto* info = rex::cvar::GetFlagInfo(name);
      if (info && rex::cvar::GetFlagByName(name) != info->default_value) {
        return false;
      }
    }
  }
  return true;
}

void Store32(uint32_t address, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(ng2::guest::Virtual(address), &value, 4);
}
void Store16(uint32_t address, uint16_t value) {
  value = rex::byte_swap(value);
  std::memcpy(ng2::guest::Virtual(address), &value, 2);
}

// The pair slots' two buttons while the slots are ours (masks; 0: none):
// slot 8 Water Run's, slot 9 Ninpo's. Read by the character's input.
std::atomic<uint32_t> g_pairMasks[2][2] = {};
std::atomic<bool> g_ours{false};

void AddKeys(std::string& list, const std::string& key) {
  if (!key.empty()) {
    list += (list.empty() ? "" : ",") + key;
  }
}

// The keyboard pad's lists from the actions' keys.
void ApplyKeys() {
  std::string signature;
  for (int a = 0; a < kActions; a++) {
    signature += rex::cvar::GetFlagByName(kTable[a].key) + ';' + std::to_string(PadOf(a)) + ';';
  }
  static std::string applied;
  static bool made = false;
  if (signature == applied) {
    return;
  }
  applied = signature;
  // Left as they are (the game's defaults, controls.cpp, or the player's
  // own keybind_*) until something here differs from its default.
  if (!made && AllDefault()) {
    return;
  }
  made = true;
  static constexpr const char* kButtonBinds[] = {
      "keybind_a", "keybind_b", "keybind_x", "keybind_y", "keybind_left_shoulder",
      "keybind_right_shoulder", "keybind_left_trigger", "keybind_right_trigger"};
  std::string lists[8];
  AddKeys(lists[0], "Return");  // the menus' Confirm and Back
  AddKeys(lists[1], "Backspace");
  for (int a = 0; a < kActions; a++) {
    auto keys = Keys(a);
    if (kTable[a].button) {
      rex::cvar::SetFlagByName(kTable[a].button, keys[1].empty() ? keys[0] : keys[0] + "," + keys[1]);
      continue;
    }
    int binding = PadOf(a);
    if (binding == kNone) {
      continue;
    }
    for (int button : {First(binding), IsPair(binding) ? Second(binding) : -1}) {
      if (button >= 0) {
        AddKeys(lists[button], keys[0]);
        AddKeys(lists[button], keys[1]);
      }
    }
  }
  for (uint32_t i = 0; i < 8; i++) {
    rex::cvar::SetFlagByName(kButtonBinds[i], lists[i]);
  }
}

// Key capture: the next key or mouse button pressed, ahead of the keyboard
// driver.
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::atomic<bool> g_capturing{false};
std::atomic<int64_t> g_lastPollMs{0};  // when the page last asked about the wait
std::mutex g_capturedMutex;
std::string g_captured;  // (guarded)
std::chrono::steady_clock::time_point g_captureStart;  // (guarded)
constexpr auto kCaptureTime = std::chrono::seconds(10);  // then given up
bool g_eaten[256] = {};  // keys whose press was taken (UI thread): their release too

class CaptureListener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    // A taken key's repeats are ours too (its release is): the driver would
    // hold it down otherwise.
    if (g_eaten[uint32_t(e.virtual_key()) & 0xFF]) {
      e.set_handled(true);
      return;
    }
    // A wait its page no longer asks about (the menu closed under it): over.
    if (g_capturing.load() && NowMs() - g_lastPollMs.load() > 300) {
      std::lock_guard lock(g_capturedMutex);
      g_capturing.store(false);
    }
    if (g_capturing.load() && !e.prev_state()) {  // (not the confirming key's repeats)
      std::string name = rex::ui::VirtualKeyToString(e.virtual_key());
      if (name == "Escape") {  // leaves it as it was
        std::lock_guard lock(g_capturedMutex);
        g_capturing.store(false);
      } else {
        Take(std::move(name));
      }
      g_eaten[uint32_t(e.virtual_key()) & 0xFF] = true;
      e.set_handled(true);
    }
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    // Only a taken key's release: another one (the key that confirmed the
    // row, let go after) reaches the keyboard driver.
    if (bool& eaten = g_eaten[uint32_t(e.virtual_key()) & 0xFF]; eaten) {
      eaten = false;
      e.set_handled(true);
    }
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    using Button = rex::ui::MouseEvent::Button;
    if (g_capturing.load() && NowMs() - g_lastPollMs.load() > 300) {
      std::lock_guard lock(g_capturedMutex);
      g_capturing.store(false);
    }
    if (!g_capturing.load()) {
      return;
    }
    switch (e.button()) {
      case Button::kLeft: Take("LMB"); break;
      case Button::kRight: Take("RMB"); break;
      case Button::kMiddle: Take("MMB"); break;
      default: return;
    }
    e.set_handled(true);
  }

 private:
  static void Take(std::string name) {
    if (name.empty()) {
      return;
    }
    std::lock_guard lock(g_capturedMutex);
    if (g_capturing.load()) {
      g_captured = std::move(name);
      g_capturing.store(false);
    }
  }
};
CaptureListener g_listener;

}  // namespace

const char16_t* Name(int action) {
  return kTable[action].name;
}
std::string Key(int action, int slot) {
  return Keys(action)[slot & 1];
}
void SetKey(int action, int slot, const std::string& key) {
  if (!key.empty()) {
    for (int other = 0; other < kActions; other++) {
      auto keys = Keys(other);
      bool had = false;
      for (int s = 0; s < 2; s++) {
        if (keys[s] == key && !(other == action && s == slot)) {
          keys[s].clear();
          had = true;
        }
      }
      if (had) {
        StoreKeys(other, keys);
      }
    }
  }
  auto keys = Keys(action);
  keys[slot & 1] = key;
  StoreKeys(action, keys);
}
bool PadFixed(int action) {
  return kTable[action].pad == nullptr;
}
bool PadPairs(int action) {
  return kTable[action].id == kNinpo || kTable[action].id == kWaterRun;
}
int Pad(int action) {
  return PadOf(action);
}
const char16_t* PadFixedText(int action) {
  return kTable[action].fixed ? kTable[action].fixed : u"";
}
void SetPad(int action, int binding) {
  if (PadFixed(action) || (IsPair(binding) && !PadPairs(action))) {
    return;
  }
  // A button, or a pair, leaves whatever had it (a button taken alone
  // doesn't break a pair it is part of: the two can be both, as the game's
  // Ninpo on Strong Attack's and Projectile's).
  if (binding != kNone) {
    for (int other = 0; other < kActions; other++) {
      if (other != action && !PadFixed(other) && PadOf(other) == binding) {
        Store(kTable[other].pad, std::to_string(kNone));
      }
    }
  }
  Store(kTable[action].pad, std::to_string(binding));
}
void Default(int action) {
  for (const char* name : {kTable[action].key, kTable[action].pad}) {
    if (const auto* info = name ? rex::cvar::GetFlagInfo(name) : nullptr) {
      Store(name, info->default_value);
    }
  }
}
void Defaults() {
  for (int a = 0; a < kActions; a++) {
    Default(a);
  }
  // The game's own Controller Settings to its defaults too (Type A), so it
  // shows these.
  uint32_t profile = Load32(kProfile);
  if (profile >= 0x40000000u && profile < 0xA0000000u) {
    ng2::guest::Virtual(profile)[kLayoutType] = 0;
    ng2::guest::Virtual(profile)[kShoulderType] = 0;
  }
}
void SetStore(void (*store)(const char* name, const std::string& value)) {
  g_store = store;
}

void StartKeyCapture() {
  g_lastPollMs.store(NowMs());
  std::lock_guard lock(g_capturedMutex);
  g_captured.clear();
  g_captureStart = std::chrono::steady_clock::now();
  g_capturing.store(true);
}
void CancelKeyCapture() {
  std::lock_guard lock(g_capturedMutex);
  g_capturing.store(false);
  g_captured.clear();
}
bool PollKeyCapture(std::string& key) {
  g_lastPollMs.store(NowMs());
  std::lock_guard lock(g_capturedMutex);
  if (g_capturing.load() && std::chrono::steady_clock::now() - g_captureStart > kCaptureTime) {
    g_capturing.store(false);
  }
  if (g_capturing.load()) {
    return true;
  }
  key = std::exchange(g_captured, {});
  return false;
}
// On the UI thread, once the window is up (controls.cpp).
void AddCaptureListener(rex::ui::Window* window) {
  constexpr size_t kAheadOfEverything = 100000;
  window->AddInputListener(&g_listener, kAheadOfEverything);
}

namespace {

// The game's pair slots' buttons (A+X, B+Y, X+Y, A+B) and their letters.
constexpr std::pair<int, int> kBuiltInPairs[4] = {{0, 2}, {1, 3}, {2, 3}, {0, 1}};
constexpr char16_t kBuiltInLetters[4][2] = {{u'X', u'A'}, {u'Y', u'B'}, {u'X', u'Y'}, {u'A', u'B'}};

int BuiltInPairSlot(int binding) {
  for (int i = 0; i < 4; i++) {
    if (binding == Pair(kBuiltInPairs[i].first, kBuiltInPairs[i].second)) {
      return 8 + i;
    }
  }
  return -1;
}

// The game's layout for its Button Layout and Shoulder Buttons (14 in its
// tables: a slot the other table sets).
void GameLayout(uint8_t type, uint8_t shoulders, uint32_t (&slots)[kSlotCount]) {
  for (uint32_t i = 0; i < kSlotCount; i++) {
    uint32_t face = Load32(kLayouts + kLayoutSize * (type & 1) + 4 * i);
    uint32_t shoulder = Load32(kShoulderLayouts + kLayoutSize * (shoulders & 3) + 4 * i);
    slots[i] = shoulder != 14 ? shoulder : face != 14 ? face : 0;
  }
}

// The controller bindings a game layout gives (kNone for the fixed rows).
std::array<int, kActions> LayoutBindings(uint8_t type, uint8_t shoulders) {
  uint32_t slots[kSlotCount];
  GameLayout(type, shoulders, slots);
  std::array<int, kActions> bindings;
  bindings.fill(kNone);
  for (int a = 0; a < kActions; a++) {
    for (uint32_t i = 0; kTable[a].pad && i < kSlotCount; i++) {
      if (slots[i] == kTable[a].id) {
        bindings[a] = i < 8 ? int(i) : Pair(kBuiltInPairs[i - 8].first, kBuiltInPairs[i - 8].second);
        break;
      }
    }
  }
  return bindings;
}

// These settings are a game layout's, as it gives them.
bool PadIsLayout(int layout) {
  auto bindings = LayoutBindings(uint8_t(layout >> 2), uint8_t(layout & 3));
  for (int a = 0; a < kActions; a++) {
    if (kTable[a].pad && PadOf(a) != bindings[a]) {
      return false;
    }
  }
  return true;
}

// The game's layout as these settings (its own Controller Settings
// changed, or a save with another layout loaded).
void ImportLayout(uint8_t type, uint8_t shoulders) {
  auto bindings = LayoutBindings(type, shoulders);
  for (int a = 0; a < kActions; a++) {
    if (kTable[a].pad) {
      Store(kTable[a].pad, std::to_string(bindings[a]));
    }
  }
}

bool PadAllDefault() {
  for (const Action& action : kTable) {
    if (!action.pad) {
      continue;
    }
    const auto* info = rex::cvar::GetFlagInfo(action.pad);
    if (info && rex::cvar::GetFlagByName(action.pad) != info->default_value) {
      return false;
    }
  }
  return true;
}

uint32_t g_lastProfile = 0;
// When the game's Controller Settings page last ran.
std::atomic<int64_t> g_layoutPageMs{-1000000};

int g_lastLayout = -1;  // the profile's Button Layout and Shoulder Buttons, as last seen

}  // namespace

void GameControllerPage() {
  g_layoutPageMs.store(NowMs());
}

void ApplyPad() {
  uint32_t profile = Load32(kProfile);
  if (profile >= 0x40000000u && profile < 0xA0000000u) {
    // The game's own Controller Settings: its layout taken as these
    // settings when it changes there, or when a save with another layout
    // loads while these are the defaults.
    uint8_t type = ng2::guest::Virtual(profile)[kLayoutType] & 1;
    uint8_t shoulders = ng2::guest::Virtual(profile)[kShoulderType] & 3;
    int layout = type << 2 | shoulders;
    // (a change made on that page; another, a save loading, only while
    // these are still the layout the game had, or the defaults: custom ones
    // stay)
    if (layout != g_lastLayout || profile != g_lastProfile) {
      bool onPage = layout != g_lastLayout && profile == g_lastProfile && NowMs() - g_layoutPageMs.load() < 500;
      bool asTheGameHad = g_lastLayout >= 0 && PadIsLayout(g_lastLayout);
      if (onPage || ((asTheGameHad || PadAllDefault()) && !PadIsLayout(layout))) {
        ImportLayout(type, shoulders);
      }
    }
    g_lastProfile = profile, g_lastLayout = layout;
  }
  ApplyKeys();
  if (profile < 0x40000000u || profile >= 0xA0000000u) {
    return;
  }
  // The slots from these settings. A pair the game has goes on its slot
  // (its own input makes its bit); another on slot 8 (Water Run) or 9
  // (Ninpo), whichever is free, made here (sub_823B7470's hook) with its
  // prompt letters.
  uint32_t slots[kSlotCount] = {};
  uint32_t pairs[2][2] = {};
  int custom[2] = {-1, -1};  // the actions on slots 8/9 made here
  for (int a = 0; a < kActions; a++) {
    int binding = PadOf(a);
    if (binding == kNone) {
      continue;
    }
    if (!IsPair(binding)) {
      slots[binding] = kTable[a].id;
    } else if (int slot = BuiltInPairSlot(binding); slot >= 0) {
      slots[slot] = kTable[a].id;
    }
  }
  for (int a = 0; a < kActions; a++) {
    int binding = PadOf(a);
    if (!IsPair(binding) || BuiltInPairSlot(binding) >= 0) {
      continue;
    }
    int slot = kTable[a].id == kWaterRun ? 8 : 9;
    if (slots[slot]) {
      slot = slot == 8 ? 9 : 8;
    }
    if (slots[slot]) {
      continue;  // (both taken by the game's pairs)
    }
    slots[slot] = kTable[a].id;
    custom[slot - 8] = a;
    pairs[slot - 8][0] = Load32(kMasks + 4 * First(binding));
    pairs[slot - 8][1] = Load32(kMasks + 4 * Second(binding));
  }
  for (uint32_t pair = 0; pair < 2; pair++) {
    g_pairMasks[pair][0].store(pairs[pair][0]);
    g_pairMasks[pair][1].store(pairs[pair][1]);
    // The prompts' two letters for slot 8 + pair: ours, or the game's.
    int binding = custom[pair] >= 0 ? PadOf(custom[pair]) : kNone;
    Store16(kPairLetters + 24 * pair + kLetter,
            binding != kNone ? kButtonLetters[First(binding)] : kBuiltInLetters[pair][0]);
    Store16(kPairLetters + 24 * pair + 12 + kLetter,
            binding != kNone ? kButtonLetters[Second(binding)] : kBuiltInLetters[pair][1]);
  }
  g_ours.store(custom[0] >= 0 || custom[1] >= 0);
  bool same = true;
  for (uint32_t i = 0; i < kSlotCount; i++) {
    same &= Load32(profile + kSlots + 4 * i) == slots[i];
  }
  if (same) {
    return;
  }
  for (uint32_t i = 0; i < kSlotCount; i++) {
    Store32(profile + kSlots + 4 * i, slots[i]);
    Store32(kSlotsCopy + 4 * i, slots[i]);
  }
  // The players' cached masks, as sub_823ABB68 makes them.
  for (uint32_t p = 0; p < kPlayers; p++) {
    for (auto [field, action] : kCachedFields) {
      uint32_t mask = 0;
      for (uint32_t i = 0; i < kSlotCount; i++) {
        mask |= slots[i] == action ? Load32(kMasks + 4 * i) : 0;
      }
      Store32(kCached + kCachedSize * p + field, mask);
    }
  }
}

}  // namespace ng2::rebind

// The character's input from a pad record: the pair slots' bits (in the
// buttons pressed, +16) made from the two buttons bound, for a player's pad
// while the slots are ours.
REX_EXTERN(__imp__sub_823B7470);
REX_HOOK_RAW(sub_823B7470) {
  using namespace ng2::rebind;
  uint32_t input = ctx.r3.u32, record = ctx.r4.u32, wanted = ctx.r5.u32;
  bool pad = record == kMergedPad ||
             (record >= kPadRecords && record < kPadRecords + kPadRecordSize * kPlayers &&
              (record - kPadRecords) % kPadRecordSize == 0);
  if (!g_ours.load() || !pad || !input) {
    __imp__sub_823B7470(ctx, base);
    return;
  }
  // Only our slots' bits are made here (the game's own pairs, on the other
  // slots, are its own).
  constexpr uint32_t kSlotBit[2] = {0x40000000u, 0x80000000u};
  uint32_t ours = (g_pairMasks[0][0].load() ? kSlotBit[0] : 0) | (g_pairMasks[1][0].load() ? kSlotBit[1] : 0);
  uint32_t before = Load32(input + 16) & ~ours;  // pressed the frame before
  __imp__sub_823B7470(ctx, base);
  uint32_t held = Load32(input + 8);
  uint32_t pressed = Load32(input + 16) & ~ours;
  uint32_t lately = pressed | before;
  uint32_t made = pressed;
  for (uint32_t pair = 0; pair < 2; pair++) {
    uint32_t a = g_pairMasks[pair][0].load(), b = g_pairMasks[pair][1].load();
    if (!a || !b) {
      continue;
    }
    bool together = (lately & a) && (lately & b);
    bool joined = wanted == kSlotBit[pair] &&
                  (((held & a) && (pressed & b)) || ((held & b) && (pressed & a)));
    if (together || joined) {
      made |= kSlotBit[pair];
    }
  }
  Store32(input + 16, made);
}
