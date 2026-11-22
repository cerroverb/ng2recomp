// ng2 - Ninja Gaiden II static recompilation
//
// The Gameplay page of RECOMP SETTINGS (recomp_settings.cpp).
//
// Skip Cutscenes: the game lets a cutscene be skipped (Start or A, mask 0x110
// in the pad's newly pressed buttons) only once it has been seen. Both of
// its checks read the event's id (u16 0x84C3F0B4) and a seen byte: movies
// in the event-script command sub_8365E760 (0x84C3EC4F + id; skipping sets
// the 30-frame fade-out), real-time cutscenes in the event-script loop
// sub_83655B98 ([0x84C559E0] + 104 + id, below 384). Both run as script
// code that yields each frame, so the bytes are set from the input update
// (sub_822F2E38, once a frame, before the scripts) on a frame a skip button
// is pressed while a cutscene runs (a movie on the host player, or a script
// with a skip argument: u16 0x84A69AD4), and put back the next frame: the
// profile isn't changed. A script that can't be skipped at all (its skip
// argument 0) still can't.
//
// Skip Intro: the boot movie (NinjaVI.wmv, the first movie played) isn't
// opened by the host movie player (src/movie), so it ends at once.

#include "gameplay.h"

#include <cstdint>
#include <iterator>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "gpu/guest.h"
#include "gpu/video.h"
#include "movie/movie.h"

REXCVAR_DEFINE_BOOL(skip_intro, false, "Gameplay", "Skip the opening movie at launch");
REXCVAR_DEFINE_BOOL(skip_cutscenes, false, "Gameplay",
                    "Any cutscene can be skipped, seen or not");

namespace {

using ng2::guest::Load32;
using ng2::guest::Virtual;

constexpr uint32_t kEventId = 0x84C3F0B4;        // u16
constexpr uint32_t kMovieSeen = 0x84C3EC4F;      // + id
constexpr uint32_t kMovieSeenCount = kEventId - kMovieSeen;  // room before the id
constexpr uint32_t kScriptSkip = 0x84A69AD4;     // u16: the running script's skip argument
constexpr uint32_t kProfile = 0x84C559E0;        // -> profile
constexpr uint32_t kCutsceneSeen = 104;          // profile + id
constexpr uint32_t kCutsceneCount = 384;
constexpr uint32_t kPadIndex = 0x84C3DA3E;       // u8: the player's pad
constexpr uint32_t kPadPressed = 0x83C07FF8;     // + pad * 0x44: newly pressed
constexpr uint32_t kSkipButtons = 0x110;

uint16_t EventId() {
  const uint8_t* p = Virtual(kEventId);
  return uint16_t(p[0] << 8 | p[1]);
}

bool SkipPressed() {
  uint32_t pad = *Virtual(kPadIndex);
  return pad < 4 && (Load32(kPadPressed + pad * 0x44) & kSkipButtons) != 0;
}

// Seen bytes set for a skip press, put back the next frame.
struct Forced {
  uint32_t address;
  uint8_t was;
};
Forced g_forced[2];
uint32_t g_forcedCount = 0;

void Force(uint32_t address) {
  uint8_t* byte = Virtual(address);
  if (*byte == 0 && g_forcedCount < std::size(g_forced)) {
    g_forced[g_forcedCount++] = {address, *byte};
    *byte = 1;
  }
}

void PutBack() {
  for (uint32_t i = 0; i < g_forcedCount; i++) {
    uint8_t* byte = Virtual(g_forced[i].address);
    if (*byte == 1) {
      *byte = g_forced[i].was;
    }
  }
  g_forcedCount = 0;
}

uint16_t Load16(uint32_t address) {
  const uint8_t* p = Virtual(address);
  return uint16_t(p[0] << 8 | p[1]);
}

}  // namespace

namespace ng2::gameplay {

bool SkipIntro() {
  return REXCVAR_GET(skip_intro);
}

bool InCutscene() {
  return Load16(kScriptSkip) != 0;
}

}  // namespace ng2::gameplay

namespace ng2::rebind {
void ApplyPad();  // rebind.cpp
}

// The input update, once a frame: the pads read, the scripts still to run.
REX_EXTERN(__imp__sub_822F2E38);
REX_HOOK_RAW(sub_822F2E38) {
  PutBack();
  ng2::rebind::ApplyPad();
  __imp__sub_822F2E38(ctx, base);
  ng2::video::NoteInputRead();
  if (!REXCVAR_GET(skip_cutscenes) || !SkipPressed()) {
    return;
  }
  // Only while a cutscene runs: a movie on the host player, or a script
  // whose cutscene can be skipped at all.
  uint16_t id = EventId();
  if (ng2::movie::AnyPlaying() && id < kMovieSeenCount) {
    Force(kMovieSeen + id);
  }
  uint32_t profile = Load32(kProfile);
  if (Load16(kScriptSkip) != 0 && profile && id < kCutsceneCount) {
    Force(profile + kCutsceneSeen + id);
  }
}
