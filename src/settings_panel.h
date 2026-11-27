// ng2 - Ninja Gaiden II static recompilation
//
// RECOMP SETTINGS' pages, drawn by us: a page is a column of rows (a label,
// and for a choice its value, changed with left and right) with a note
// under them for the row picked, scrolled when it holds more rows than fit.
// The same on the title's Options screen and in the in-game menu, to the
// right of the game's own category column (recomp_settings.cpp hosts it,
// reads the pad and plays the sounds). Text only, in the game's menu font
// (overlay.h): the row picked is lit, the others dim.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "overlay.h"

namespace ng2::panel {

enum class RowKind : uint8_t {
  kChoice,  // "Label      ‹ Value ›": left and right change it, Y its default
  kButton,  // "Label": Confirm does it
};

struct Row {
  RowKind kind = RowKind::kChoice;
  std::u16string label;
  std::u16string value;  // a choice's, as shown
  std::u16string note;   // under the page while it's picked (one line)
};

// One frame's input, already read from the pad and the mouse.
struct Input {
  bool up = false, down = false, left = false, right = false;
  bool confirm = false, back = false, reset = false;  // reset: Y, a row's default
  bool pointer = false;  // the menu pointer is in use (x, y in the 1280x720 UI)
  bool click = false;    // its left button, this frame
  float x = 0.0f, y = 0.0f;
};

// What a frame's input did, for the host to act on.
enum class Action : uint8_t {
  kNone,
  kMoved,     // another row picked (the cursor's sound)
  kPrevious,  // the row's value back one
  kNext,      // on one
  kConfirm,   // a button pressed
  kReset,     // the row's default
  kLeave,     // Back
};

struct State {
  int row = 0;     // the row picked
  int scroll = 0;  // the first row shown
};

// Where the page is drawn, in the 1280x720 UI.
struct Layout {
  float left = 456.0f;    // the labels' left edge
  float value = 780.0f;   // the values' (text arrows), or their center (valueCenter)
  bool valueCenter = false;  // values centered on value, no text arrows (the host draws the game's)
  float right = 1110.0f;  // the rows' right end (the pointer)
  float top = 145.0f;     // the first row's line
  float pitch = 40.0f;
  int shown = 10;         // rows that fit
  float noteY = 606.0f;
  int size = 0x1C;        // the font's size (the game's Options rows')
  int noteSize = 0x18;
};

// About how wide a text is in the menu font at a size, in the 1280x720 UI
// (its letters' widths guessed by kind; the font has no measure here).
float TextWidth(std::u16string_view text, int size);

// A text cut at spaces into lines of up to width letters (the note keeps
// clear of the button hints beside it).
std::vector<std::u16string> Wrap(std::u16string_view text, size_t width = 34);

// A frame's input on the page (its state kept in range of the rows).
Action Run(State& state, const std::vector<Row>& rows, const Input& input, const Layout& layout = {});

// The row of the page under the pointer (rows shown from state.scroll), or -1.
int PointedRow(const State& state, int count, float x, float y, const Layout& layout = {});

// A row shown, with its line's y.
struct Line {
  int row;
  float y;
};
// The page's texts for a frame (focused: the cursor is in it, else all dim
// and no note), appended to texts; lines: the rows shown.
void Draw(std::vector<overlay::Text>& texts, const State& state, const std::vector<Row>& rows, bool focused,
          const Layout& layout = {}, std::vector<Line>* lines = nullptr);

}  // namespace ng2::panel
