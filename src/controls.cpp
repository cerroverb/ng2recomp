// ng2 - Ninja Gaiden II static recompilation
//
// Keyboard and mouse. The SDK's keyboard-and-mouse driver plays as a pad (its
// keybind_* settings: a list of keys per button; a key may be in two lists,
// pressing both buttons, as the game's two-button actions need). This game's
// keys, for the settings the player hasn't set (the game's own actions, from
// its Controller Settings, in brackets):
//
//   W A S D    left stick (move)          mouse       right stick (camera)
//   left mouse, J   X (quick attack)      right mouse, K   Y (strong attack)
//   Space      A (jump)                   E           B (projectile weapon)
//   F          Y + B (ninpo)              C           A + X (water run)
//   Shift      LT (block)                 Q, middle mouse   RT
//   R          LB                         V           RB
//   Z / X      stick clicks               Tab         BACK
//   Escape     START                      arrows      d-pad
//   Return, Backspace: A and B too (menus)
//
// Whichever of the keyboard and a pad was used last is the one in use (the
// SDK's input system); the button prompts follow it (gpu/prompts.cpp), the
// mouse moves the camera only then, and the pointer is hidden while a pad
// is in use. (Shift can be bound: SDK patch 0017.)

#include "controls.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

namespace ng2::controls {

namespace {

rex::ui::WindowedAppContext* g_appContext = nullptr;

}  // namespace

void SetAppContext(rex::ui::WindowedAppContext* context) {
  g_appContext = context;
}

// A Steam Deck: Steam's SteamDeck=1, or the board's name (LCD Jupiter, OLED
// Galileo) when started without Steam.
static bool OnSteamDeck() {
  static const bool deck = [] {
    if (const char* v = std::getenv("SteamDeck"); v && *v) {
      return std::strcmp(v, "1") == 0;
    }
#if defined(__linux__)
    if (FILE* f = std::fopen("/sys/devices/virtual/dmi/id/board_name", "r")) {
      char name[32] = {};
      bool match = std::fgets(name, sizeof(name), f) &&
                   (std::strncmp(name, "Jupiter", 7) == 0 || std::strncmp(name, "Galileo", 7) == 0);
      std::fclose(f);
      return match;
    }
#endif
    return false;
  }();
  return deck;
}

Device ActiveDevice() {
  auto* runtime = rex::Runtime::instance();
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!input) {
    return Device::kUnknown;
  }
  switch (input->ActiveKind(0)) {
    case rex::input::DeviceKind::kXbox: return OnSteamDeck() ? Device::kSteamDeck : Device::kXbox;
    case rex::input::DeviceKind::kPlayStation: return Device::kPlayStation;
    case rex::input::DeviceKind::kNintendo: return Device::kNintendo;
    case rex::input::DeviceKind::kKeyboard: return Device::kKeyboard;
    default: return Device::kUnknown;
  }
}

void ApplyDefaults() {
  static constexpr std::pair<const char*, const char*> kDefaults[] = {
      {"mnk_mode", "true"},
      {"mnk_mouse", "true"},
      {"keybind_a", "Space,C,Return"},
      {"keybind_b", "E,F,Backspace"},
      {"keybind_x", "LMB,J,C"},
      {"keybind_y", "RMB,K,F"},
      {"keybind_left_trigger", "Shift"},
      {"keybind_right_trigger", "Q,MMB"},
      {"keybind_left_shoulder", "R"},
      {"keybind_right_shoulder", "V"},
      {"keybind_lstick_up", "W"},
      {"keybind_lstick_down", "S"},
      {"keybind_lstick_left", "A"},
      {"keybind_lstick_right", "D"},
      {"keybind_lstick_press", "Z"},
      {"keybind_rstick_up", "Numpad8"},
      {"keybind_rstick_down", "Numpad5"},
      {"keybind_rstick_left", "Numpad4"},
      {"keybind_rstick_right", "Numpad6"},
      {"keybind_rstick_press", "X"},
      {"keybind_dpad_up", "Up"},
      {"keybind_dpad_down", "Down"},
      {"keybind_dpad_left", "Left"},
      {"keybind_dpad_right", "Right"},
      {"keybind_back", "Tab"},
      {"keybind_start", "Escape"},
  };
  for (auto [name, value] : kDefaults) {
    const auto* info = rex::cvar::GetFlagInfo(name);
    if (info && info->source == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName(name, value);
    }
  }
}

void Frame() {
  // The mouse moves the camera (and is captured) only while the keyboard and
  // mouse are in use: a pad player's pointer is left alone, and a key or a
  // click hands the camera back to the mouse. The pointer is hidden while a
  // pad is in use. The driver hides it while it captures and, on releasing,
  // puts back what it found, so the pointer is checked again on the UI
  // thread on a change and twice a second: hidden while a pad is in use,
  // auto-hidden while the keyboard is and the driver isn't capturing.
  static std::atomic<bool> queued{false}, keyboardInUse{false}, overlayOpen{false};
  static Device last = Device::kUnknown;
  static bool lastOverlay = false;
  static uint32_t frames = 0, soon = 0;
  // Nothing used yet (at start, or a pad of no known kind): no mouse look,
  // as with a pad. An overlay of the SDK's open (F3, F4): the pointer free
  // and shown for it.
  Device device = ActiveDevice();
  auto* runtime = rex::Runtime::instance();
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  bool overlay = input && input->IsOverlayOpen();
  bool keyboard = device == Device::kKeyboard && !overlay;
  rex::input::mnk::SetMouseLookActive(keyboard);
  keyboardInUse.store(keyboard);
  overlayOpen.store(overlay);
  if (device != last || overlay != lastOverlay) {
    soon = 10;  // the next frames too, after the driver's own release
  }
  last = device;
  lastOverlay = overlay;
  bool now = soon > 0 || ++frames % 30 == 0;
  if (soon > 0) {
    soon--;
  }
  if (!now || !g_appContext || queued.exchange(true)) {
    return;
  }
  g_appContext->CallInUIThreadDeferred([] {
    queued.store(false);
    bool keyboard = keyboardInUse.load();
    auto* runtime = rex::Runtime::instance();
    auto* window = runtime ? runtime->display_window() : nullptr;
    if (!window) {
      return;
    }
    using Visibility = rex::ui::Window::CursorVisibility;
    if (overlayOpen.load()) {
      if (window->GetCursorVisibility() != Visibility::kVisible) {
        window->SetCursorVisibility(Visibility::kVisible);
      }
    } else if (!keyboard) {
      if (window->GetCursorVisibility() != Visibility::kHidden) {
        window->SetCursorVisibility(Visibility::kHidden);
      }
    } else if (!window->IsMouseCaptureRequested() &&
               window->GetCursorVisibility() == Visibility::kHidden) {
      window->SetCursorVisibility(Visibility::kAutoHidden);
    }
  });
}

}  // namespace ng2::controls
