// ng2 - Ninja Gaiden II static recompilation
//
// The game's achievements in the port (achievements.cpp), after Unleashed
// Recompiled's: the SDK keeps the list (the game's XDBF: names, texts,
// icons) and the unlocks; here they're kept from unlocking while cheats are
// unlocked (recomp_settings.h), an unlock is shown in the bottom right corner
// (achievement_notifications), and RECOMP SETTINGS' Achievements page lists
// them.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ng2::achievements {

struct Entry {
  uint32_t id = 0;
  std::u16string name, description;  // (a secret one's, until unlocked: the 360's words)
  bool unlocked = false;
  uint64_t unlockedAt = 0;  // FILETIME
  uint32_t icon = 0;        // its UI image (video.h), 0: none
};

// Unlocked first, the latest first, then the locked ones in the game's
// order; the locked secret ones last, as one row ("3 Hidden Achievements",
// id 0, the padlock: Steam's).
std::vector<Entry> List();
uint32_t Total();
uint32_t UnlockedCount();

// "2026/11/27 14:05", local time.
std::u16string DateText(uint64_t filetime);

// The UI images to draw this frame besides the toast's (the Achievements
// page's icons): shown with the toast's at the frame's start.
struct Icon {
  uint32_t key;
  float x, y, size, alpha;
  bool grey;
  float height = 0.0f;  // (0: size, square)
};
void SetPageIcons(std::vector<Icon> icons);
// The page's scroll bar pictures (UI images), 0 until the game's are read.
uint32_t ScrollTrack();
uint32_t ScrollThumb();

// The game folder (the toast's box is made from pictures in its archives).
void SetGameRoot(const std::filesystem::path& root);

// Each frame (perf_overlay.cpp's frame start): set up once the SDK has the
// list, the toast shown, the UI images handed over. True when a toast
// starts (its sound is the caller's to play).
bool Frame();
// NG2_TEST_TOAST=1: a toast for the next achievement in the game's order (F6, any
// thread), nothing unlocked: to see it in play and menus.
void PreviewToast();
// NG2_TEST_UNLOCK_KEY=1: the next locked achievement (the hidden ones
// first, then the game's order) unlocked for real (F5, any thread), as the game unlocks one.
void TestUnlockNext();
// The menu sound a toast plays (sub_8242AFC0's number).
constexpr uint32_t kToastSound = 28;

}  // namespace ng2::achievements
