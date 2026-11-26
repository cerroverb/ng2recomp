// ng2 - Ninja Gaiden II static recompilation
//
// Keyboard and mouse (controls.cpp): the SDK's keyboard-and-mouse pad with
// keys for this game, the device in use, the pointer.

#pragma once

namespace rex::ui {
class WindowedAppContext;
}

namespace ng2::controls {

enum class Device { kUnknown, kXbox, kPlayStation, kNintendo, kKeyboard };

// The kind of device player one used last (any thread).
Device ActiveDevice();

// At start, after the settings are read: this game's keys for the ones the
// player hasn't set.
void ApplyDefaults();

// Each frame: the pointer hidden while a controller is in use.
void Frame();

void SetAppContext(rex::ui::WindowedAppContext* context);

}  // namespace ng2::controls
