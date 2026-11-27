// ng2 - Ninja Gaiden II static recompilation
//
// Texts of ours over the game's 2D, in its menu font (perf_overlay.cpp): a
// panel is a list of them, shown for as long as it is set again each frame.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

namespace ng2::overlay {

struct Text {
  float x, y;           // in the 1280x720 UI: the left edge, the line's center
  std::u16string text;  // (the game's tags work: @[pad:Rd] a button's picture)
  float shade = 1.0f;   // 1: a focused row's colour; less: dimmer
  int size = 0x1C;      // the font's size (0x1C: the Options rows')
  uint32_t color = 0;   // 0xRRGGBB, or 0: a focused row's (both times shade)
  float alpha = 1.0f;
};

// A copy of one of the game's sprites (its 0x90 bytes, taken each frame),
// placed by its offset (+0x30, +0x34): its pictures as the game draws them.
struct Sprite {
  uint32_t source = 0;
  float x = 0.0f, y = 0.0f;
  bool image = false;  // an image part (drawn as a list widget's arrows), else an item
  float shade = -1.0f;  // its colour (grey, at +0x40) when 0 or more, else as copied
  bool update = false;  // the source updated first (one nothing else updates)
};

// layer: the 2D layer they're queued in (0: the frame-rate counter's).
// queued: by QueuePanel, from a draw of the caller's (a menu the frame's
// start doesn't run under), not at the frame's start.
void SetPanel(std::vector<Text> texts, uint32_t layer = 0, bool queued = false, std::vector<Sprite> sprites = {});


// The achievements' toast, for this frame (drawn at the frame's start, over
// play or menus, with the panel).
void SetToast(std::vector<Text> texts);
// A string the 2D pass draws at (x, y) (its transform's place, the 1280x720
// UI) is the toast's first (aspect.cpp: its box is drawn under it).
bool IsToastText(float x, float y);

// The panel's texts into this frame's 2D lists, now (a guest thread).
void QueuePanel(PPCContext& ctx, uint8_t* base);

}  // namespace ng2::overlay
