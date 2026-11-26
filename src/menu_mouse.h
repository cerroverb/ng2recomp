// ng2 - Ninja Gaiden II static recompilation
//
// The mouse in the game's menus (menu_mouse.cpp).

#pragma once

#include <cstdint>

namespace rex::ui {
class Window;
}

namespace ng2::menu_mouse {

// On the UI thread, once the window is up (controls.cpp).
void AddListener(rex::ui::Window* window);
// Each frame: whether a menu is up (controls.cpp, from aspect.cpp's buckets).
void SetMenu(bool menu);
// The game's frames, counted at each frame's start (controls.cpp): the
// menus' signs are kept for a couple of them, not a time (a slow frame
// doesn't end a menu).
void FrameStart();
uint64_t Frame();
bool InMenu();
// A menu read the pad (its helpers: a direction, Confirm, Back), from their
// hooks; MenuInputLately: in the last frames (play reads actions instead).
void NoteMenuInput();
bool MenuInputLately();
// The pad's bits the mouse (and Delete) press this frame (the game's: A
// 0x10, B 0x20, Y 0x80, d-pad up 1, down 2), just before the pads are merged.
uint32_t PadBits();
// A stage is on (its HUD asked for), or the title menu is up: its menus are
// the pause menu's then, whose X and Y prompts are play's (the command list).
void NoteStage(bool on);
bool InStage();
// The Controls table takes input this frame: the Delete key is X there
// (Clear), not Y.
void DeleteClears();
bool DeleteClearsNow();
// Scripted runs (script_input.h's "ptr<x>x<y>" and "click" steps): the
// pointer moved to that point of a 1280x720 window, and a left click.
void TestPointer(int32_t x, int32_t y);
void TestClick();
// Where the pointer is, in the game's 1280x720 UI (false: not known).
bool Pointer(float* x, float* y);
// The mouse was moved, clicked or wheeled in a menu since this was last
// asked.
bool TakeMouseUsed();
// PadBits pressed something this frame (the merged pad's presses are the
// mouse's then, not a controller's).
bool PressedThisFrame();
// A menu's arrow drawn this frame (aspect.cpp), in the 1280x720 UI: a click
// on it presses that way, and the wheel goes sideways while only sideways
// ones are up.
enum class Way { kLeft, kRight, kUp, kDown };
void NoteArrow(Way way, float x0, float y0, float x1, float y1);
// The pointer moved this frame (in a menu): a menu that points follows it
// then, not while it rests (the keys and pad lead then).
bool PointerMoved();
// The left button was clicked this frame (its A press, PadBits): where.
bool ClickedThisFrame();
// Either: the pointer picks the row it's on (a click picks it too, so it
// confirms that row, not the one the keys picked).
inline bool PointerActive() {
  return PointerMoved() || ClickedThisFrame();
}

}  // namespace ng2::menu_mouse
