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
};

// layer: the 2D layer they're queued in (0: the frame-rate counter's).
// queued: by QueuePanel, from a draw of the caller's (a menu the frame's
// start doesn't run under), not at the frame's start.
void SetPanel(std::vector<Text> texts, uint32_t layer = 0, bool queued = false);


// The panel's texts into this frame's 2D lists, now (a guest thread).
void QueuePanel(PPCContext& ctx, uint8_t* base);

}  // namespace ng2::overlay
