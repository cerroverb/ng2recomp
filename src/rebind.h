// ng2 - Ninja Gaiden II static recompilation
//
// Controls: the keys and the controller's buttons for the game's actions
// (rebind.cpp), as RECOMP SETTINGS' Controls page shows and sets them.

#pragma once

#include <string>

namespace rex::ui {
class Window;
}

namespace ng2::rebind {

constexpr int kActions = 20;

// A controller binding: kNone, a button (0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB,
// 6 LT, 7 RT), or two held together (Pair(a, b), a < b).
constexpr int kNone = -1;
constexpr int Pair(int a, int b) {
  return a < b ? 0x100 | a << 4 | b : 0x100 | b << 4 | a;
}
constexpr bool IsPair(int binding) {
  return binding >= 0x100;
}
constexpr int First(int binding) {
  return IsPair(binding) ? (binding >> 4) & 0xF : binding;
}
constexpr int Second(int binding) {
  return binding & 0xF;
}

const char16_t* Name(int action);
// Its keys (slot 0 or 1; "": none), as the SDK names them ("Space", "LMB").
std::string Key(int action, int slot);
// A key for it (""; clears the slot); the key leaves whatever had it.
void SetKey(int action, int slot, const std::string& key);
// The controller's: whether it can be set here (the sticks, the d-pad,
// START and BACK are the game's), whether it takes a pair, what it has.
bool PadFixed(int action);
bool PadPairs(int action);
int Pad(int action);
bool SetPad(int action, int binding);  // false: refused (Interact keeps a button)
// A fixed one's text (the game's button tags).
const char16_t* PadFixedText(int action);
// One action, or all, as they come.
bool Default(int action);  // false: its button refused (as SetPad)
void Defaults();
// How the settings are stored (RECOMP SETTINGS': written to the config).
void SetStore(void (*store)(const char* name, const std::string& value));
// The game's own Controller Settings page ran (a layout changed then is
// the player's: taken as these settings).
void GameControllerPage();

// The next key or mouse button pressed, taken ahead of the keyboard driver.
void StartKeyCapture();
void CancelKeyCapture();
// True while it waits; once over, the key ("": cancelled, or none in time).
bool PollKeyCapture(std::string& key);
void AddCaptureListener(rex::ui::Window* window);

// Before each input update: the game's bindings as set here.
void ApplyPad();

}  // namespace ng2::rebind
