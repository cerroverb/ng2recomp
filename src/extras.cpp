// ng2 - Ninja Gaiden II static recompilation
//
// Extras (RECOMP SETTINGS' Extras page) and three more cheats: this port's
// own versions of options of alicenotmech's Ninja Gaiden II Master Edition,
// whose patch showed where in the game (Title Update 3) each one acts.
//
// Skip Death Screen: the death sequence (sub_82442E18) waits 12 frames
// where it waits 120 and 1 where it waits 30, the GAME OVER caption
// (sub_836A3508 from sub_83695B20) isn't started, and once the game over
// menu (0x85366170, state +0x524) is up Start is pressed for the player.
//
// Auto Chapter Screen: the chapter card (0x8532D850, state +0x480: 1-3 its
// intro, 4 waiting, 5 pressed, then 6 and 7) skips the wait for its intro and
// is pressed on, as the mission briefing (state 3 of 0x85022DA8 +0x54) is.
// Both press Start in the pad state (sub_822F2E38, just before the four
// pads are merged).
//
// HUD: Hidden skips the HUD's draw functions; Fade Out draws the health and
// ninpo gauges, the ammo counter and the enemy gauge with the HUD's alpha
// (0x853264AC) scaled by one that rises while fighting (hurt, a button held,
// a lock-on, an enemy within 15 units, or the HUD back after a pause) and
// falls three seconds after.
//
// Post-Effects off: the post passes' mask (0x84C55758, read by sub_83648E50)
// loses the blur bits (0x400, 0x10000; 0x1000 unless 0x4000 is also set),
// and the white flash (sub_836FB7A8 from sub_8269CF98) isn't started.
//
// Smooth HUD and Menus: the 2D sprites near 1:1 scale take the game's
// filtered sprite path (sub_83803400) too, as the others do.
//
// Hi-Res Font: the font's glyphs are rasterised at twice their size and
// their metrics halved (read at start: glyphs already made keep their size).
//
// Shadows Hi-Res (Graphics page): the game keeps two shadow maps, 1024x1024
// and 512x512 (both made at start, sub_82332240), and each frame
// sub_823347E8 asks for the 512 one (0x83C110EC = 2; 1 is the 1024 one)
// when none of its 160 entries (sub_82334F18) wants the big one, which is
// how it is in a fight. The main loop (sub_822F0CA8) takes the request once a
// frame. With Hi-Res a request for 512 is turned into one for 1024. (The
// option is Master Edition's "Hi-res Shadows", by alicenotmech.)
//
// Cheats: All Weapons and Ninpo puts every weapon and ninpo (Level 1 or 3)
// and full ammo in the save's inventory (save +0x30: 128 slots of id, count,
// flags) at a new game and at each stage's start; All Crystal Skulls counts
// all 30 as found; Max HP and Ninpo reads the life and ninpo upgrades as
// full when a stage's character is made from the save.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "gpu/guest.h"

REXCVAR_DEFINE_BOOL(skip_death_screen, false, "Extras",
                    "Straight to the game over menu after dying, without the death screen");
REXCVAR_DEFINE_BOOL(auto_chapter_screen, false, "Extras",
                    "Chapter cards and mission briefings go on by themselves");
REXCVAR_DEFINE_INT32(hud_mode, 0, "Extras", "0: shown, 1: fades out when not fighting, 2: hidden");
REXCVAR_DEFINE_BOOL(post_effects, true, "Extras", "The blur and white flashes (off: none)");
REXCVAR_DEFINE_BOOL(smooth_hud, false, "Extras", "The HUD's and menus' pictures always filtered");
REXCVAR_DEFINE_INT32(shadows, 0, "Video", "0: the game's (512x512 in fights), 1: hi-res (1024x1024 always)");
REXCVAR_DEFINE_BOOL(hires_font, false, "Extras",
                    "Text drawn from glyphs twice the size (from the next start)");
REXCVAR_DEFINE_INT32(cheat_all_weapons, 0, "Cheats",
                     "0: off, 1: every weapon and ninpo at Level 1, 2: at Level 3");
REXCVAR_DEFINE_BOOL(cheat_all_skulls, false, "Cheats", "All 30 crystal skulls count as found");
REXCVAR_DEFINE_BOOL(cheat_max_health_ninpo, false, "Cheats",
                    "Life and ninpo upgrades full from the start");

namespace {

using ng2::guest::Load16;
using ng2::guest::Load32;
using ng2::guest::LoadFloat;

void Store8(uint32_t address, uint8_t value) {
  *ng2::guest::Virtual(address) = value;
}
void Store16(uint32_t address, uint16_t value) {
  value = rex::byte_swap(value);
  std::memcpy(ng2::guest::Virtual(address), &value, 2);
}
void Store32(uint32_t address, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(ng2::guest::Virtual(address), &value, 4);
}
void StoreFloat(uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Store32(address, bits);
}
void ScaleFloats(uint32_t address, uint32_t count, float scale) {
  for (uint32_t i = 0; i < count; i++) {
    StoreFloat(address + 4 * i, LoadFloat(address + 4 * i) * scale);
  }
}

constexpr uint32_t kStart = 0x10;  // the pad's Start bit

// Skip Death Screen and Auto Chapter Screen.
constexpr uint32_t kGameOverState = 0x85366694;  // 2: its menu is up
constexpr uint32_t kChapterCardState = 0x8532DCD0;
constexpr uint32_t kChapterCardIntro = 0x84C3F060;  // 1 while its intro must play
constexpr uint32_t kBriefingState = 0x85022DFC;     // 3: waiting

// HUD.
constexpr uint32_t kHudAlpha = 0x853264AC;
constexpr uint32_t kDevice = 0x859EEBC0;      // + 0x40A0: the HUD's frame count
constexpr uint32_t kPlayerHealth = 0x85660E0E;
constexpr uint32_t kHeldButtons = 0x83C08100;  // all pads'
constexpr uint32_t kLockOn = 0x83B3CB80, kBossGauge = 0x8532E2AC;
constexpr uint32_t kCharacters = 0x8565B4F0, kCharacterSize = 0x5B10;
constexpr uint32_t kCharacterHealth = 0x591E, kCharacterX = 0x270, kCharacterZ = 0x278;
constexpr float kNear = 15.0f;
constexpr int kHoldFrames = 180;

// Post-Effects off.
constexpr uint32_t kPostPasses = 0x84C55758;

// Cheats.
constexpr uint32_t kSave = 0x83BD2488;         // the save's state, while playing
constexpr uint32_t kEquipped = 0x85660E3E;     // the player's item in hand
constexpr uint32_t kInventory = 0x30, kSlots = 128;

// Every weapon and ninpo, and the ammo, as the inventory holds them: id,
// count, flags (the weapons' level, less one; 0x10: ammo, only added; 0x40:
// taken away at a stage's start unless in hand).
struct Item {
  uint16_t id;
  uint8_t count, flags;
};
constexpr Item kLevel1[] = {
    {0x0C, 1, 0}, {0x11, 1, 0}, {0x12, 1, 0}, {0x13, 1, 0},  {0x14, 1, 0},    {0x15, 1, 0},
    {0x18, 1, 0}, {0x01, 0, 0x40}, {0x06, 1, 0}, {0x52, 1, 0}, {0x92, 1, 0},  {0x53, 1, 0},
    {0x00, 1, 0}, {0x23, 30, 0x10}, {0x22, 1, 0}, {0x93, 1, 0}, {0x25, 30, 0x10},
    {0x94, 100, 0x10}, {0xA0, 1, 0}, {0x05, 1, 0},
};
constexpr Item kLevel3[] = {
    {0x07, 1, 2}, {0x0C, 1, 2}, {0x11, 1, 2}, {0x12, 1, 2}, {0xCC, 1, 2},    {0x14, 1, 2},
    {0x15, 1, 2}, {0x18, 1, 2}, {0x01, 1, 0x40}, {0x06, 1, 2}, {0x52, 1, 2},  {0x92, 1, 2},
    {0x53, 1, 2}, {0x00, 1, 0}, {0x23, 30, 0x10}, {0x22, 1, 0}, {0x93, 1, 0}, {0x25, 30, 0x10},
    {0x94, 100, 0x10}, {0xA0, 1, 0}, {0x05, 1, 0},
};

// The item's own slot (even used up: but for item 0, whose id an empty slot
// also has), and the first empty one (count 0, as the game's).
void FindSlots(uint32_t slots, uint16_t id, uint32_t* found, uint32_t* free) {
  *found = *free = 0;
  for (uint32_t s = 0; s < kSlots; s++) {
    uint32_t slot = slots + 4 * s;
    uint8_t count = *ng2::guest::Virtual(slot + 2);
    if (Load16(slot) == id && (count > 0 || id != 0) && !*found) {
      *found = slot;
    } else if (count == 0 && !*free) {
      *free = slot;
    }
  }
}

// Fills the live save's inventory; stageStart: at a stage's start (else a
// new game's save or a save loaded). What it gives stays, as items found:
// switched off, it gives no more.
void GiveAllWeapons(bool stageStart) {
  int32_t level = REXCVAR_GET(cheat_all_weapons);
  uint32_t save = Load32(kSave);
  if (level <= 0 || (save >> 16) != 0x859E) {
    return;
  }
  const Item* items = level == 1 ? kLevel1 : kLevel3;
  size_t count = level == 1 ? std::size(kLevel1) : std::size(kLevel3);
  uint32_t slots = save + kInventory;
  for (size_t i = 0; i < count; i++) {
    const Item& item = items[i];
    if (item.count == 0) {
      continue;
    }
    uint32_t found, free;
    FindSlots(slots, item.id, &found, &free);
    if (item.flags & 0x40) {
      // (Level 3: the plain Dragon Sword goes, for the True one, unless in
      // hand)
      if (stageStart && found && Load16(kEquipped) != item.id) {
        Store16(found, 0), Store8(found + 2, 0), Store8(found + 3, 0);  // (empty, as the game's)
      }
      continue;
    }
    bool ammo = item.flags & 0x10;
    uint8_t flags = item.flags & 0xF;
    if (found) {
      uint8_t* bytes = ng2::guest::Virtual(found);
      bytes[2] = std::max(bytes[2], item.count);
      if (!ammo) {
        bytes[3] = std::max(bytes[3], flags);
      }
    } else if (free) {
      Store16(free, item.id), Store8(free + 2, item.count), Store8(free + 3, flags);
    }
  }
}

// The HUD's fade: its alpha, and the frames left before it falls.
float g_hudFade = 1.0f;
int g_hudHold = kHoldFrames;
uint32_t g_hudLastFrame = 0;
uint16_t g_hudLastHealth = 0;

bool Fighting() {
  uint32_t device = Load32(kDevice);
  uint32_t frame = device ? Load32(device + 0x40A0) : 0;
  uint32_t last = g_hudLastFrame;
  g_hudLastFrame = frame;
  if (frame - last > 30) {
    return true;  // the HUD back after a while (a pause, a cutscene)
  }
  uint16_t health = Load16(kPlayerHealth);
  uint16_t lastHealth = g_hudLastHealth;
  g_hudLastHealth = health;
  if (health < lastHealth || (Load32(kHeldButtons) & 0x3EF) || Load32(kLockOn) ||
      Load32(kBossGauge)) {
    return true;
  }
  float x = LoadFloat(kCharacters + kCharacterX), z = LoadFloat(kCharacters + kCharacterZ);
  for (uint32_t k = 1; k < 16; k++) {
    uint32_t enemy = kCharacters + k * kCharacterSize;
    if (Load16(enemy + kCharacterHealth) == 0 || Load32(enemy + kCharacterX) == 0) {
      continue;
    }
    float dx = LoadFloat(enemy + kCharacterX) - x, dz = LoadFloat(enemy + kCharacterZ) - z;
    if (dx * dx + dz * dz < kNear * kNear) {
      return true;
    }
  }
  return false;
}

void UpdateHudFade() {
  if (Fighting()) {
    g_hudHold = kHoldFrames;
    g_hudFade = std::min(g_hudFade + 1.0f / 15.0f, 1.0f);
  } else if (--g_hudHold < 0) {
    g_hudHold = 0;
    g_hudFade = std::max(g_hudFade - 1.0f / 60.0f, 0.0f);
  }
}

// A HUD part: drawn, hidden, or drawn with the fade's alpha.
std::atomic<int64_t> g_hudCalledMs{0};

void HudPart(PPCContext& ctx, uint8_t* base, void (*draw)(PPCContext&, uint8_t*), bool fades,
             bool update = false) {
  g_hudCalledMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count(),
                      std::memory_order_relaxed);
  int32_t mode = REXCVAR_GET(hud_mode);
  if (mode == 2) {
    return;
  }
  if (mode != 1 || !fades) {
    draw(ctx, base);
    return;
  }
  if (update) {
    UpdateHudFade();
  }
  float alpha = LoadFloat(kHudAlpha);
  StoreFloat(kHudAlpha, alpha * g_hudFade);
  draw(ctx, base);
  StoreFloat(kHudAlpha, alpha);
}

bool HiResFont() {
  static const bool on = REXCVAR_GET(hires_font);
  return on;
}

}  // namespace

// The game asked for its HUD in the last frames (drawn or not: the HUD option
// hidden), so it is in play.
bool HudInPlay() {
  int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
  return now - g_hudCalledMs.load(std::memory_order_relaxed) < 100;
}

// The pad state, before the four pads are merged: r5 the first pad's words.
void Ng2ExtrasPad(PPCRegister& r5) {
  bool press = false;
  if (REXCVAR_GET(skip_death_screen) && Load32(kGameOverState) == 2) {
    press = true;
  }
  if (REXCVAR_GET(auto_chapter_screen)) {
    uint32_t card = Load32(kChapterCardState);
    if (card == 3) {
      if (Load32(kChapterCardIntro) != 1) {
        Store32(kChapterCardState, 4);
      }
    } else if (card == 5 || Load32(kBriefingState) == 3) {
      press = true;
    }
  }
  // Into every pad's words (0x44 apart): the game over menu reads the
  // player's own pad, not the merged one.
  if (press) {
    for (uint32_t pad = 0; pad < 4; pad++) {
      for (uint32_t w = 0; w < 5; w++) {
        uint32_t word = r5.u32 + 0x44 * pad + 4 * w;
        Store32(word, Load32(word) | kStart);
      }
    }
  }
}

// Skip Death Screen: the death sequence's waits (just loaded).
void Ng2DeathWait(PPCRegister& r29) {
  if (REXCVAR_GET(skip_death_screen)) {
    r29.u64 = 12;
  }
}
void Ng2DeathFade(PPCRegister& r10) {
  if (REXCVAR_GET(skip_death_screen)) {
    r10.u64 = 1;
  }
}
// True: the GAME OVER caption isn't started.
bool Ng2DeathCaption() {
  return REXCVAR_GET(skip_death_screen);
}

// HUD.
#define NG2_HUD_PART(name, fades, update)          \
  REX_EXTERN(__imp__##name);                       \
  REX_HOOK_RAW(name) {                             \
    HudPart(ctx, base, __imp__##name, fades, update); \
  }
NG2_HUD_PART(sub_836A3000, false, false)
NG2_HUD_PART(sub_836BBB00, false, false)
NG2_HUD_PART(sub_8369AF68, false, false)
NG2_HUD_PART(sub_836A1620, false, false)
NG2_HUD_PART(sub_836E8850, true, true)
NG2_HUD_PART(sub_836A65F0, true, false)
NG2_HUD_PART(sub_836C2318, true, false)
#undef NG2_HUD_PART

// Post-Effects off: the blur passes asked for, by two functions and inline
// (true: skipped).
REX_EXTERN(__imp__sub_8364B758);
REX_HOOK_RAW(sub_8364B758) {
  if (!!REXCVAR_GET(post_effects)) {
    __imp__sub_8364B758(ctx, base);
  }
}
REX_EXTERN(__imp__sub_8363CD80);
REX_HOOK_RAW(sub_8363CD80) {
  if (!!REXCVAR_GET(post_effects)) {
    __imp__sub_8363CD80(ctx, base);
  }
}
// Shadows Hi-Res: the frame's request for the small shadow map (2) becomes
// one for the big one (1), and with the small one in use and nothing asked
// (stage setup's request taken while a script runs, which skips the game's
// own), the big one is asked for.
constexpr uint32_t kShadowMapRequest = 0x83C110EC;
constexpr uint32_t kShadowMapIndex = 0x83C11048;  // 0: the big map, 1: the small one
REX_EXTERN(__imp__sub_823347E8);
REX_HOOK_RAW(sub_823347E8) {
  __imp__sub_823347E8(ctx, base);
  if (REXCVAR_GET(shadows) == 1) {
    uint32_t request = ng2::guest::Load32(kShadowMapRequest);
    if (request == 2 || (request == 0 && ng2::guest::Load32(kShadowMapIndex) == 1)) {
      Store32(kShadowMapRequest, 1);
    }
  }
}
bool Ng2PostBlur() {
  return !REXCVAR_GET(post_effects);
}
// The mask as the post passes read it.
void Ng2PostPasses(PPCRegister& r26) {
  if (!REXCVAR_GET(post_effects) && !(r26.u32 & 0x4000)) {
    r26.u64 = r26.u32 & ~0x1000u;
  }
}
bool Ng2WhiteFlash() {
  return !REXCVAR_GET(post_effects);
}

// Smooth HUD and Menus (true: the branch around the filtered path isn't
// taken).
bool Ng2SmoothSprite() {
  return REXCVAR_GET(smooth_hud);
}

// Hi-Res Font.
void Ng2FontSize(PPCRegister& f31) {
  if (HiResFont()) {
    f31.f64 = double(float(f31.f64) * 2.0f);
  }
}
void Ng2FontMetrics(PPCRegister& r31) {
  if (HiResFont()) {
    for (uint32_t offset : {0x20u, 0x34u, 0xE4u, 0xE8u}) {
      StoreFloat(r31.u32 + offset, LoadFloat(r31.u32 + offset) * 0.5f);
    }
  }
}
void Ng2FontBox(PPCRegister& r1) {
  if (HiResFont()) {
    ScaleFloats(r1.u32 + 0x80, 5, 0.5f);
  }
}
void Ng2FontGlyphSize(PPCRegister& f1) {
  if (HiResFont()) {
    f1.f64 = double(float(f1.f64) * 2.0f);
  }
}
void Ng2FontGlyphCell(PPCRegister& r31) {
  if (HiResFont()) {
    for (uint32_t offset : {0x32u, 0x34u}) {
      Store16(r31.u32 + offset, uint16_t((Load16(r31.u32 + offset) + 1) >> 1));
    }
  }
}
void Ng2FontLine(PPCRegister& r31) {
  if (HiResFont()) {
    ScaleFloats(r31.u32 + 4, 5, 0.5f);
  }
}
void Ng2FontExtent(PPCRegister& r29, PPCRegister& r31) {
  if (HiResFont() && int32_t(r29.u32) >= 0) {
    ScaleFloats(r31.u32, 5, 0.5f);
  }
}
// True: the glyph's own size is used (the branch past it isn't taken).
bool Ng2FontOwnSize() {
  return HiResFont();
}

// All Weapons and Ninpo: a stage's start, a save loaded, a new game's save.
REX_EXTERN(__imp__sub_836713C8);
REX_HOOK_RAW(sub_836713C8) {
  GiveAllWeapons(true);
  __imp__sub_836713C8(ctx, base);
}
REX_EXTERN(__imp__sub_8246D7F8);
REX_HOOK_RAW(sub_8246D7F8) {
  GiveAllWeapons(false);
  __imp__sub_8246D7F8(ctx, base);
}
void Ng2NewGameSave() {
  GiveAllWeapons(false);
}

// All Crystal Skulls: skull i found, and the count found.
REX_EXTERN(__imp__sub_82434B78);
REX_HOOK_RAW(sub_82434B78) {
  uint32_t skull = Load16(ctx.r4.u32);
  __imp__sub_82434B78(ctx, base);
  if (REXCVAR_GET(cheat_all_skulls) && Load32(0x84C559E0) != 0 && skull < 30) {
    ctx.r3.u64 = 1;
  }
}
void Ng2SkullCount(PPCRegister& r) {
  if (REXCVAR_GET(cheat_all_skulls)) {
    r.u64 = 30;
  }
}
void Ng2SkullCountR3(PPCRegister& r3) {
  Ng2SkullCount(r3);
}
void Ng2SkullCountR7(PPCRegister& r7) {
  Ng2SkullCount(r7);
}
void Ng2SkullCountR10(PPCRegister& r10) {
  Ng2SkullCount(r10);
}
void Ng2SkullCountR11(PPCRegister& r11) {
  Ng2SkullCount(r11);
}

// Max HP and Ninpo leaves the save alone: the game copies the character
// back into the save (sub_83755D20(save): health at +0xC04 current and
// +0xC06 most, then eight segments of current and most from +0xC08 (only the
// first used: a Life of the Gods raises its most, which a full bar refuses,
// so the item is kept); ninpo +0xC28 current, +0xC2A most). With the cheat
// on, the save's own most values are put back after it, and the currents
// kept within them (a save point's refill stays full).
// The save is kept as long as the character was made with the cheat on, even
// if it was turned off since (else its full bars would go into the save).
constexpr uint32_t kSaveHealth = 0xC04, kSaveSegment = 0xC08, kSaveNinpo = 0xC28;
std::atomic<bool> g_maxMade{false};
REX_EXTERN(__imp__sub_83755D20);
REX_HOOK_RAW(sub_83755D20) {
  uint32_t save = ctx.r3.u32;
  if (!g_maxMade.load() || (save >> 16) != 0x859E) {  // (the character's own maxima otherwise)
    __imp__sub_83755D20(ctx, base);
    return;
  }
  constexpr uint32_t kFrom = kSaveHealth, kTo = 0xC2C;  // health and ninpo
  uint8_t before[kTo - kFrom];
  std::memcpy(before, ng2::guest::Virtual(save + kFrom), sizeof(before));
  __imp__sub_83755D20(ctx, base);
  uint16_t health = Load16(save + kSaveHealth), segment = Load16(save + kSaveSegment);
  uint16_t ninpo = Load16(save + kSaveNinpo);
  std::memcpy(ng2::guest::Virtual(save + kFrom), before, sizeof(before));
  Store16(save + kSaveHealth, std::min(health, Load16(save + kSaveHealth + 2)));
  Store16(save + kSaveSegment, std::min(segment, Load16(save + kSaveSegment + 2)));
  Store16(save + kSaveNinpo, std::min(ninpo, Load16(save + kSaveNinpo + 2)));
}

// Max HP and Ninpo: the upgrades as a stage's character is made from the
// save (just loaded).
void Ng2MaxHealth(PPCRegister& r10) {
  g_maxMade.store(REXCVAR_GET(cheat_max_health_ninpo));
  if (REXCVAR_GET(cheat_max_health_ninpo)) {
    r10.u64 = 3600;
  }
}
// The health bar's other segments (1-7, current and most; 0xFFFF: unused):
// unused, so the first one's 3600 is the whole bar.
void Ng2MaxHealthSegment(PPCRegister& r10) {
  if (REXCVAR_GET(cheat_max_health_ninpo)) {
    r10.u64 = 0xFFFF;
  }
}
// (the ninpo the character starts with, and its most)
void Ng2MaxNinpo(PPCRegister& r10) {
  if (REXCVAR_GET(cheat_max_health_ninpo)) {
    r10.u64 = 2000;
  }
}
