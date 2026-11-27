// ng2 - Ninja Gaiden II static recompilation
//
// RECOMP SETTINGS' pages, drawn by us (settings_panel.h).

#include "settings_panel.h"

#include <algorithm>
#include <cmath>

namespace ng2::panel {
namespace {

constexpr float kLit = 1.0f, kDim = 0.5f, kNote = 0.8f;

}  // namespace

float TextWidth(std::u16string_view text, int size) {
  // (fitted to the game's own centered values: "1x 1120x584" 163 px,
  // "2x MSAA" 115, "Original" 99, "Default" 87 at size 0x1C)
  float em = 0.0f;
  for (char16_t c : text) {
    if (std::u16string_view(u"il.,:;'|!").find(c) != std::u16string_view::npos) {
      em += 0.28f;
    } else if (c == u' ') {
      em += 0.3f;
    } else if (std::u16string_view(u"fjrtI()-").find(c) != std::u16string_view::npos) {
      em += 0.36f;
    } else if (std::u16string_view(u"mw").find(c) != std::u16string_view::npos) {
      em += 0.78f;
    } else if (c == u'M' || c == u'W') {
      em += 0.85f;
    } else if (c >= u'A' && c <= u'Z') {
      em += 0.64f;
    } else if (c >= u'0' && c <= u'9') {
      em += 0.65f;
    } else {
      em += 0.5f;
    }
  }
  return em * float(size);
}

std::vector<std::u16string> Wrap(std::u16string_view text, size_t width) {
  std::vector<std::u16string> lines;
  while (text.size() > width) {
    size_t cut = text.rfind(u' ', width);
    if (cut == std::u16string_view::npos || cut == 0) {
      cut = width;
    }
    lines.emplace_back(text.substr(0, cut));
    text.remove_prefix(std::min(text.size(), text[cut] == u' ' ? cut + 1 : cut));
  }
  lines.emplace_back(text);
  return lines;
}

namespace {

void Clamp(State& state, int count) {
  state.row = std::clamp(state.row, 0, std::max(count - 1, 0));
}

void Follow(State& state, int count, const Layout& layout) {
  if (state.row < state.scroll) {
    state.scroll = state.row;
  } else if (state.row >= state.scroll + layout.shown) {
    state.scroll = state.row + 1 - layout.shown;
  }
  state.scroll = std::clamp(state.scroll, 0, std::max(count - layout.shown, 0));
}

}  // namespace

int PointedRow(const State& state, int count, float x, float y, const Layout& layout) {
  if (x < layout.left - 16.0f || x > layout.right) {
    return -1;
  }
  float line = std::floor((y - layout.top) / layout.pitch + 0.5f);
  int row = state.scroll + int(line);
  return line >= 0.0f && line < float(layout.shown) && row < count ? row : -1;
}

Action Run(State& state, const std::vector<Row>& rows, const Input& input, const Layout& layout) {
  int count = int(rows.size());
  Clamp(state, count);
  Follow(state, count, layout);
  if (input.back) {
    return Action::kLeave;
  }
  if (count == 0) {
    return Action::kNone;
  }
  int row = state.row;
  if (input.up) {
    row = (row + count - 1) % count;
  }
  if (input.down) {
    row = (row + 1) % count;
  }
  int pointed = input.pointer ? PointedRow(state, count, input.x, input.y, layout) : -1;
  if (pointed >= 0) {
    row = pointed;
  }
  if (row != state.row) {
    state.row = row;
    Follow(state, count, layout);
    return Action::kMoved;
  }
  const Row& at = rows[size_t(row)];
  // A click: on the row picked (a click elsewhere does nothing); on a
  // choice's value, its arrow back steps back, the rest on.
  bool previous = input.left, next = input.right, confirm = input.confirm;
  if (input.click) {
    confirm = false;
    if (pointed == row) {
      if (at.kind == RowKind::kButton) {
        confirm = true;
      } else if (layout.valueCenter && input.x >= layout.value - 160.0f) {
        (input.x < layout.value ? previous : next) = true;
      } else if (!layout.valueCenter && input.x >= layout.value - 16.0f) {
        (input.x < layout.value + 20.0f ? previous : next) = true;
      }
    }
  }
  if (at.kind == RowKind::kButton) {
    return confirm ? Action::kConfirm : Action::kNone;
  }
  if (previous != next) {
    return previous ? Action::kPrevious : Action::kNext;
  }
  return input.reset ? Action::kReset : Action::kNone;
}

void Draw(std::vector<overlay::Text>& texts, const State& state, const std::vector<Row>& rows, bool focused,
          const Layout& layout, std::vector<Line>* lines) {
  int count = int(rows.size());
  State shown = state;
  Clamp(shown, count);
  Follow(shown, count, layout);
  auto put = [&](float x, float y, std::u16string text, float shade, int size) {
    texts.push_back({x, y, std::move(text), shade, size});
  };
  for (int i = shown.scroll; i < count && i < shown.scroll + layout.shown; i++) {
    const Row& row = rows[size_t(i)];
    float y = layout.top + layout.pitch * float(i - shown.scroll);
    bool lit = focused && i == shown.row;
    if (lines) {
      lines->push_back({i, y});
    }
    put(layout.left, y, row.label, lit ? kLit : kDim, layout.size);
    if (row.kind == RowKind::kChoice && layout.valueCenter) {
      put(layout.value - 0.5f * TextWidth(row.value, layout.size), y, row.value, lit ? kLit : kDim, layout.size);
    } else if (row.kind == RowKind::kChoice) {
      // The value after "‹ " either way, so it doesn't move as it's lit.
      float arrow = TextWidth(u"‹ ", layout.size);
      put(layout.value + arrow, y, row.value, lit ? kLit : kDim, layout.size);
      if (lit) {
        put(layout.value, y, u"‹", kLit, layout.size);
        put(layout.value + arrow + TextWidth(row.value, layout.size) + 0.25f * float(layout.size), y, u"›", kLit,
            layout.size);
      }
    }
  }
  // More rows above or below.
  float markX = layout.left - 30.0f;
  if (shown.scroll > 0) {
    put(markX, layout.top, u"▲", kDim, layout.noteSize);
  }
  if (shown.scroll + layout.shown < count) {
    put(markX, layout.top + layout.pitch * float(layout.shown - 1), u"▼", kDim, layout.noteSize);
  }
  if (focused && shown.row < count && !rows[size_t(shown.row)].note.empty()) {
    std::vector<std::u16string> noteLines = Wrap(rows[size_t(shown.row)].note);
    float lineHeight = float(layout.noteSize);
    float y = layout.noteY - 0.5f * lineHeight * float(noteLines.size() - 1);
    for (std::u16string& line : noteLines) {
      put(layout.left, y, std::move(line), kNote, layout.noteSize);
      y += lineHeight;
    }
  }
}

}  // namespace ng2::panel
