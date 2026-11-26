// ng2 - Ninja Gaiden II static recompilation
//
// The mouse in the game's menus (menu_mouse.h). While a menu is up
// (aspect.cpp: something drawn in a menu's 2D bucket lately, or the menus'
// pad helpers run) the pointer is free and shown (controls.cpp), and its
// buttons and wheel are the pad's: the left button A, the right B, the
// wheel the d-pad's up and down, each click or step one press; the Delete
// key is Y (the load screen's Delete). They're taken ahead of the keyboard
// driver (whose buttons are attacks), and pressed into every pad's words
// just before the four pads are merged (extras.cpp's Ng2ExtrasPad), as a
// pad's would be. In play the mouse is the keyboard driver's again.

#include "menu_mouse.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace ng2::menu_mouse {

namespace {

constexpr uint32_t kA = 0x10, kB = 0x20, kX = 0x40, kY = 0x80, kUp = 0x1, kDown = 0x2, kLeft = 0x4, kRight = 0x8;  // the game's pad bits

std::atomic<bool> g_menu{false};
// Clicks not yet pressed: each one press of a frame, with a frame let go
// before the next (holding the button doesn't repeat it).
std::atomic<int> g_leftClicks{0}, g_rightClicks{0}, g_middleClicks{0};  // (middle: the Delete key)
constexpr int kMostQueued = 3;  // clicks or wheel steps waiting at most
std::atomic<int> g_wheel{0};  // steps: + up, - down
std::atomic<int32_t> g_x{-1}, g_y{-1}, g_width{0}, g_height{0};
std::atomic<bool> g_moved{false}, g_movedNow{false};
// The sideways lists' arrows: where each was last drawn, and when.
struct Arrow {
  std::atomic<float> x0{0}, y0{0}, x1{0}, y1{0};
  std::atomic<int64_t> ms{-1000000};
};
Arrow g_arrows[4];  // left, right, up, down

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
std::atomic<bool> g_used{false};     // moved, clicked or wheeled (any time)
std::atomic<bool> g_pressedNow{false};  // PadBits pressed something this frame
std::atomic<bool> g_clickedNow{false};  // PadBits pressed A for a click this frame
// Clicks taken: their releases ours too (cleared as the menu closes, so a
// release lost meanwhile doesn't keep the next one in play from the driver).
std::atomic<bool> g_tookLeft{false}, g_tookRight{false};

class MenuMouse final : public rex::ui::WindowInputListener {
 public:
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    using Button = rex::ui::MouseEvent::Button;
    if (!g_menu.load()) {
      return;
    }
    g_used.store(true);
    Track(e);
    if (e.button() == Button::kLeft) {
      if (g_leftClicks.load() < kMostQueued) {
        g_leftClicks.fetch_add(1);
      }
      g_tookLeft.store(true);
    } else if (e.button() == Button::kRight) {
      if (g_rightClicks.load() < kMostQueued) {
        g_rightClicks.fetch_add(1);
      }
      g_tookRight.store(true);
    } else {
      return;
    }
    e.set_handled(true);
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    using Button = rex::ui::MouseEvent::Button;
    if (e.button() == Button::kLeft && g_tookLeft.exchange(false)) {
      e.set_handled(true);
    } else if (e.button() == Button::kRight && g_tookRight.exchange(false)) {
      e.set_handled(true);
    }
  }
  // Delete: Y in a menu (the load screen's Delete), a key out of the way for
  // it.
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (g_menu.load() && e.virtual_key() == rex::ui::VirtualKey::kDelete && !e.prev_state()) {
      if (g_middleClicks.load() < kMostQueued) {
        g_middleClicks.fetch_add(1);
      }
      e.set_handled(true);
    }
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    Track(e);
    if (g_menu.load()) {
      g_moved.store(true);
      g_used.store(true);
    }
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (!g_menu.load()) {
      return;
    }
    g_used.store(true);
    static int32_t rest = 0;  // (UI thread) a wheel's part steps
    rest += e.scroll_y();
    int steps = rest / int32_t(rex::ui::MouseEvent::kScrollPerDetent);
    rest -= steps * int32_t(rex::ui::MouseEvent::kScrollPerDetent);
    if (steps) {
      int was = g_wheel.load();
      while (!g_wheel.compare_exchange_weak(was, std::clamp(was + steps, -kMostQueued, kMostQueued))) {
      }
    }
    e.set_handled(true);
  }

 private:
  static void Track(rex::ui::MouseEvent& e) {
    g_x.store(e.x());
    g_y.store(e.y());
    if (auto* window = e.target()) {
      g_width.store(int32_t(window->GetActualPhysicalWidth()));
      g_height.store(int32_t(window->GetActualPhysicalHeight()));
    }
  }
};
MenuMouse g_listener;

}  // namespace

void AddListener(rex::ui::Window* window) {
  constexpr size_t kBelowKeyCapture = 99999;  // (rebind.cpp's capture first)
  window->AddInputListener(&g_listener, kBelowKeyCapture);
}

void SetMenu(bool menu) {
  if (!menu && g_menu.exchange(false)) {
    g_leftClicks.store(0), g_rightClicks.store(0), g_middleClicks.store(0), g_wheel.store(0);
    g_tookLeft.store(false), g_tookRight.store(false);
  } else if (menu) {
    g_menu.store(true);
  }
}

bool InMenu() {
  return g_menu.load();
}

std::atomic<uint64_t> g_frame{0};
std::atomic<uint64_t> g_menuInputFrame{0};
void FrameStart() {
  g_frame.fetch_add(1, std::memory_order_relaxed);
}
uint64_t Frame() {
  return g_frame.load(std::memory_order_relaxed);
}
void NoteMenuInput() {
  g_menuInputFrame.store(Frame() + 1, std::memory_order_relaxed);  // (+1: 0 is never)
}
bool MenuInputLately() {
  uint64_t at = g_menuInputFrame.load(std::memory_order_relaxed);
  return at && Frame() + 1 - at <= 2;
}

std::atomic<bool> g_stage{false};
void NoteStage(bool on) {
  g_stage.store(on, std::memory_order_relaxed);
}
bool InStage() {
  return g_stage.load(std::memory_order_relaxed);
}

std::atomic<uint64_t> g_deleteClearsFrame{0};
void DeleteClears() {
  g_deleteClearsFrame.store(Frame() + 1, std::memory_order_relaxed);
}
bool DeleteClearsNow() {
  uint64_t at = g_deleteClearsFrame.load(std::memory_order_relaxed);
  return at && Frame() + 1 - at <= 2;
}

bool Pointer(float* x, float* y);

// An arrow drawn lately.
bool ArrowUp(int i) {
  return NowMs() - g_arrows[i].ms.load() < 200;
}
// Only sideways ones: the wheel cycles that list.
bool ArrowsUp() {
  return (ArrowUp(0) || ArrowUp(1)) && !ArrowUp(2) && !ArrowUp(3);
}
// The arrow the pointer is on (0 left, 1 right, 2 up, 3 down), or -1.
int ArrowAt() {
  float x, y;
  if (!Pointer(&x, &y)) {
    return -1;
  }
  for (int i = 0; i < 4; i++) {
    const Arrow& a = g_arrows[i];
    constexpr float kSlack = 12.0f;  // (they bob up and down)
    if (NowMs() - a.ms.load() < 200 && x >= a.x0.load() - kSlack && x <= a.x1.load() + kSlack &&
        y >= a.y0.load() - kSlack - 20.0f && y <= a.y1.load() + kSlack + 20.0f) {
      return i;
    }
  }
  return -1;
}

uint32_t PadBits() {
  if (!g_menu.load()) {
    g_movedNow.store(false), g_clickedNow.store(false);
    return 0;
  }
  // A click or a wheel step: pressed one frame, let go the next.
  static bool released = true;
  g_clickedNow.store(false);
  g_pressedNow.store(false);
  g_movedNow.store(g_moved.exchange(false));
  if (!released) {
    released = true;
    return 0;
  }
  uint32_t bits = 0;
  if (g_leftClicks.load() > 0) {
    g_leftClicks.fetch_sub(1);
    // On a sideways list's arrow: that way; else A.
    int arrow = ArrowAt();
    constexpr uint32_t kWays[4] = {kLeft, kRight, kUp, kDown};
    bits |= arrow >= 0 ? kWays[arrow] : kA;
    g_clickedNow.store(arrow < 0);
  }
  if (g_rightClicks.load() > 0) {
    bits |= kB;
    g_rightClicks.fetch_sub(1);
  }
  if (g_middleClicks.load() > 0) {
    bits |= DeleteClearsNow() ? kX : kY;  // (the load screen's Delete; the Controls table's Clear)
    g_middleClicks.fetch_sub(1);
  }
  if (int wheel = g_wheel.load(); wheel) {
    // (sideways while a sideways list's arrows are up: the wheel cycles it)
    bool sideways = ArrowsUp();
    bits |= wheel > 0 ? (sideways ? kLeft : kUp) : (sideways ? kRight : kDown);
    g_wheel.fetch_add(wheel > 0 ? -1 : 1);
  }
  released = bits == 0;
  g_pressedNow.store(bits != 0);
  return bits;
}

void NoteArrow(Way way, float x0, float y0, float x1, float y1) {
  Arrow& a = g_arrows[int(way)];
  a.x0.store(x0), a.y0.store(y0), a.x1.store(x1), a.y1.store(y1), a.ms.store(NowMs());
}

bool TakeMouseUsed() {
  return g_used.exchange(false);
}
bool PressedThisFrame() {
  return g_pressedNow.load();
}

bool PointerMoved() {
  return g_movedNow.load();
}
bool ClickedThisFrame() {
  return g_clickedNow.load();
}

void TestPointer(int32_t x, int32_t y) {
  g_width.store(1280), g_height.store(720);
  g_x.store(x), g_y.store(y);
  g_used.store(true);
  g_moved.store(true);
}
void TestClick() {
  if (!g_menu.load()) {
    return;
  }
  g_used.store(true);
  if (g_leftClicks.load() < kMostQueued) {
    g_leftClicks.fetch_add(1);
  }
}

bool Pointer(float* x, float* y) {
  int32_t px = g_x.load(), py = g_y.load(), w = g_width.load(), h = g_height.load();
  if (px < 0 || w <= 0 || h <= 0) {
    return false;
  }
  // The game's 16:9 picture, as large as fits the window, in its 1280x720 UI.
  float scale = std::min(float(w) / 1280.0f, float(h) / 720.0f);
  *x = (float(px) - (float(w) - 1280.0f * scale) / 2.0f) / scale;
  *y = (float(py) - (float(h) - 720.0f * scale) / 2.0f) / scale;
  return true;
}

}  // namespace ng2::menu_mouse
