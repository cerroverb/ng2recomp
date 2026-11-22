// ng2 - Ninja Gaiden II static recompilation
//
// The Cheats page of RECOMP SETTINGS (recomp_settings.cpp).
//
// Infinite Health: damage to a character goes through sub_82412508 (r3 a
// record whose first word is the character), and the damage that lowers its
// most health (the red end of the bar) through sub_82412EA8 and
// sub_824131E0. Each gives none when the byte at 0x83CB5992 is 1 (anyone),
// or 2 and the character is the player (two of the character's own flags,
// and in sub_82412508 the record's +10 being 0). It is set to 2 for those
// calls only, so enemies still take theirs.
//
// Infinite Ninpo: a character's ninpo is a u16 at +0x5942 (200 a charge),
// its most at +0x5940. A cast spends as usual, and the player's ninpo is
// filled back to its most each frame in play (CheatsFrame, from the frame's
// start). (A cost of 0 instead left the HUD
// showing a charge spent: its gauge plays a cast's drain itself and only
// shows the charges again when it sees the ninpo go up.)
//
// Unlock Difficulties: sub_823E1098(clears, d) says difficulty d is open
// (some difficulty at or above d - 1 cleared). The New Game path list
// (sub_8366F4D0) asks it for Mentor and Master Ninja; the answer is yes for
// those two calls. Mission Mode doesn't ask: choosing a mission
// (sub_823E4758) sets how many difficulties it offers (1, 2, 3 or 4) from
// the mission's record in the profile (cleared flags 0x10..0x80 in byte 7
// of [0x84C559E0] + 1016 + 8 * mission), except for missions whose table
// entry (0x83B2E334 + 12 * mission) is 3, which offer one. A hook right
// after the count is set raises it to 4.
//
// Unlock Costumes: the costume list (sub_823E1448) adds a costume per
// difficulty the same check says is cleared; it is yes there too.
//
// One Hit Kill: in sub_82412508 (r3 the hit's record, r4 the attacker's),
// the damage goes through sub_82413B08(damage, record, segment), whose
// return is the damage taken from that health segment (u16s at the
// character's +0x5930, 0xFFFF unused; a damage at least the segment's
// empties it). For a hit by the player on a character no player controls
// (+0x59D3 0xFF), it returns the most and empties the other segments: the
// game then finds the health gone and kills it as usual. A hit that does no
// damage (or one on a character that can't be hurt now, which returns
// before) is left as it is.
//
// Instant Charge: a player's charge (Ultimate Techniques) is in a 108-byte
// block at 0x84C3DA40 + 108 * the character's +0x59D3: +0 its level, +20
// the frames held (0 when not charging), +22 and +24 the frames for levels
// 1 and 2. The player's update (sub_8238B2D8) raises the level when the
// count reaches one of those, else counts on; each frame in play, a count
// that has started is set to the next level's.
//
// Neither writes the unlocks to the profile (a game played on a difficulty
// or in a costume they open is saved as usual): switched off, the menus
// are as before.

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "aspect.h"
#include "gpu/guest.h"

REXCVAR_DEFINE_BOOL(cheat_infinite_health, false, "Cheats", "The player takes no damage");
REXCVAR_DEFINE_BOOL(cheat_infinite_ninpo, false, "Cheats", "Casting ninpo doesn't spend it");
REXCVAR_DEFINE_BOOL(cheat_unlock_difficulties, false, "Cheats",
                    "Every difficulty in New Game and Mission Mode");
REXCVAR_DEFINE_BOOL(cheat_unlock_costumes, false, "Cheats", "Every costume");
REXCVAR_DEFINE_BOOL(cheat_one_hit_kill, false, "Cheats", "Any hit that does damage kills the enemy");
REXCVAR_DEFINE_BOOL(cheat_instant_charge, false, "Cheats", "Ultimate Techniques charge at once");

namespace {

constexpr uint32_t kNoDamage = 0x83CB5992;
constexpr uint8_t kPlayerOnly = 2;

// A damage function with the player's no-damage flag set.
void WithoutPlayerDamage(PPCContext& ctx, uint8_t* base, void (*damage)(PPCContext&, uint8_t*)) {
  uint8_t* flag = ng2::guest::Virtual(kNoDamage);
  uint8_t game = *flag;
  bool ours = REXCVAR_GET(cheat_infinite_health) && game == 0;
  if (ours) {
    *flag = kPlayerOnly;
  }
  damage(ctx, base);
  if (ours && *flag == kPlayerOnly) {
    *flag = game;
  }
}

}  // namespace

constexpr uint32_t kPlayerRecord = 0x84C3DC60;  // the player's record (below)
constexpr uint32_t kRecordSize = 24;
constexpr uint32_t kSegments = 0x5930, kSegmentCount = 8, kPlayerIndex = 0x59D3;
uint32_t g_oneHitTarget = 0;  // the character a hit by the player is on, in sub_82412508

REX_EXTERN(__imp__sub_82412508);
REX_HOOK_RAW(sub_82412508) {
  uint32_t player = ng2::guest::Load32(kPlayerRecord);
  uint32_t target = ctx.r3.u32 ? ng2::guest::Load32(ctx.r3.u32) : 0;
  uint32_t attacker = ctx.r4.u32 ? ng2::guest::Load32(ctx.r4.u32) : 0;
  // The player's hits: its own record's (kicks, throws) and the next one's,
  // its weapon's (whose character is the weapon's, not the player's).
  bool byPlayer = attacker == player || ctx.r4.u32 == kPlayerRecord + kRecordSize;
  bool oneHit = REXCVAR_GET(cheat_one_hit_kill) && player && byPlayer && target &&
                target != player && ng2::guest::Virtual(target)[kPlayerIndex] == 0xFF;
  g_oneHitTarget = oneHit ? target : 0;
  WithoutPlayerDamage(ctx, base, __imp__sub_82412508);
  g_oneHitTarget = 0;
}
REX_EXTERN(__imp__sub_82413B08);
REX_HOOK_RAW(sub_82413B08) {
  uint32_t damage = ctx.r3.u32 & 0xFFFF;
  uint32_t record = ctx.r4.u32, segment = ctx.r5.u32 & 0xFF;
  __imp__sub_82413B08(ctx, base);
  uint32_t target = g_oneHitTarget;
  if (!target || damage == 0 || !record || ng2::guest::Load32(record) != target) {
    return;
  }
  for (uint32_t i = 0; i < kSegmentCount; i++) {
    uint32_t at = target + kSegments + 2 * i;
    if (i != segment && ng2::guest::Load16(at) != 0xFFFF) {
      std::memset(ng2::guest::Virtual(at), 0, 2);
    }
  }
  ctx.r3.u64 = 0xFFFF;
}
REX_EXTERN(__imp__sub_82412EA8);
REX_HOOK_RAW(sub_82412EA8) {
  WithoutPlayerDamage(ctx, base, __imp__sub_82412EA8);
}
REX_EXTERN(__imp__sub_824131E0);
REX_HOOK_RAW(sub_824131E0) {
  WithoutPlayerDamage(ctx, base, __imp__sub_824131E0);
}

// The player's record is the first of the characters' 24-byte records; the
// game reads its character straight from 0x84C3DC60. The game's own test that
// a record's character is the player (sub_82412508): the record's +10 is 0,
// the character's +0x59D3 isn't 0xFF and its +0x59BF is 0.
constexpr uint32_t kNinpo = 0x5942, kNinpoMost = 0x5940;

void FillPlayerNinpo();
void ChargePlayer();
bool HudInPlay();  // extras.cpp

// The New Game path list is made with the title menu (sub_8366F4D0, at
// 0x853A3470, after the menu's entries: Acolyte and Warrior, then Mentor
// and Master Ninja if open, their count at +8208). It is always made with
// all four, and the count follows the setting (CheatsFrame): the paths the
// profile has opened, or all of them, at once either way.
constexpr uint32_t kPathCount = 0x853A3470 + 8208;
constexpr uint32_t kProfile = 0x84C559E0;
bool g_allPathsAsked = false;  // while the list is made
uint32_t g_allPaths = 0, g_openPaths = 0;

// The unlock check, by caller (its return address).
REX_EXTERN(__imp__sub_823E1098);
REX_HOOK_RAW(sub_823E1098) {
  uint32_t from = uint32_t(ctx.lr);
  bool paths = from == 0x8366F874 || from == 0x8366F8A8;
  bool costumes = from == 0x823E1714 || from == 0x823E174C || from == 0x823E1784 ||
                  from == 0x823E17BC;
  if ((paths && g_allPathsAsked) ||
      (costumes && REXCVAR_GET(cheat_unlock_costumes))) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_823E1098(ctx, base);
}

// Around the title menu's builder (recomp_settings.cpp's hook): all four
// paths asked for, then the ones open without the cheat counted.
void CheatsTitleMenu(PPCContext& ctx, uint8_t* base, void (*build)(PPCContext&, uint8_t*)) {
  g_allPathsAsked = true;
  build(ctx, base);
  g_allPathsAsked = false;
  g_allPaths = ng2::guest::Load32(kPathCount);
  // The paths open without the cheat (Master Ninja only after Mentor, as
  // the list has them).
  uint32_t profile = ng2::guest::Load32(kProfile);
  uint32_t open = 2;
  if (profile && g_allPaths == 4) {
    PPCRegister r3 = ctx.r3, r4 = ctx.r4;
    for (uint32_t difficulty : {2u, 3u}) {
      ctx.r3.u64 = profile + 720, ctx.r4.u64 = difficulty;
      __imp__sub_823E1098(ctx, base);
      if (ctx.r3.u32 == 0) {
        break;
      }
      open++;
    }
    ctx.r3 = r3, ctx.r4 = r4;
  }
  g_openPaths = std::min(open, g_allPaths);
}

// Mission Mode: r11 the difficulties offered, r31 the mission screen
// (+4: the mission). ng2_codegen.toml places it at 0x823E4A80.
void Ng2MissionDifficulties(PPCRegister& r11, PPCRegister& r31) {
  constexpr uint32_t kMissionTable = 0x83B2E334;  // 12 bytes a mission
  constexpr uint32_t kFixedDifficulty = 3;
  constexpr uint32_t kAll = 4;
  if (!REXCVAR_GET(cheat_unlock_difficulties)) {
    return;
  }
  uint32_t mission = ng2::guest::Load32(r31.u32 + 4);
  if (mission < 100 && ng2::guest::Load32(kMissionTable + 12 * mission) != kFixedDifficulty &&
      r11.u32 < kAll) {
    // The screen keeps the count at +48 (the choice, its cursor's wrap);
    // r11 places the cursor once.
    r11.u64 = kAll;
    uint8_t* count = ng2::guest::Virtual(r31.u32 + 48);
    count[0] = 0, count[1] = 0, count[2] = 0, count[3] = uint8_t(kAll);
  }
}

// Each frame (perf_overlay.cpp's frame start): Infinite Ninpo keeps the
// gauge full.
void CheatsFrame() {
  // Unlock Difficulties: the New Game paths shown (on the title menu: in
  // play the list is gone, and it is made again with the next title menu).
  // Stopped once play starts (the HUD drawn, or its functions run with the HUD
  // hidden).
  if (ng2::aspect::HudShown() || HudInPlay()) {
    g_allPaths = 0;
  }
  if (g_allPaths >= 2 && g_allPaths <= 4) {
    uint32_t count = REXCVAR_GET(cheat_unlock_difficulties) ? g_allPaths : g_openPaths;
    if (ng2::guest::Load32(kPathCount) != count) {
      uint32_t value = rex::byte_swap(count);
      std::memcpy(ng2::guest::Virtual(kPathCount), &value, 4);
    }
  }
  // In play only: before it the record holds something else.
  if (REXCVAR_GET(cheat_infinite_ninpo) && (ng2::aspect::HudShown() || HudInPlay())) {
    FillPlayerNinpo();
  }
  if (REXCVAR_GET(cheat_instant_charge) && (ng2::aspect::HudShown() || HudInPlay())) {
    ChargePlayer();
  }
}

// A charge the player has started, to the next level's count (the game
// raises the level on its next update).
void ChargePlayer() {
  constexpr uint32_t kCharges = 0x84C3DA40, kChargeSize = 108;
  uint32_t player = ng2::guest::Load32(kPlayerRecord);
  if (player < 0x40000000u || player >= 0xA0000000u ||
      ng2::guest::Virtual(kPlayerRecord)[10] != 0 || ng2::guest::Virtual(player)[0x59BF] != 0) {
    return;
  }
  uint8_t index = ng2::guest::Virtual(player)[0x59D3];
  if (index >= 4) {
    return;
  }
  uint32_t block = kCharges + kChargeSize * index;
  uint16_t level = ng2::guest::Load16(block), count = ng2::guest::Load16(block + 20);
  uint16_t first = ng2::guest::Load16(block + 22), second = ng2::guest::Load16(block + 24);
  if (count == 0 || first == 0 || second <= first || level >= 2) {
    return;
  }
  uint16_t to = level == 0 ? first : second;
  if (count < to) {
    uint16_t value = rex::byte_swap(to);
    std::memcpy(ng2::guest::Virtual(block + 20), &value, 2);
  }
}

// The player's ninpo to its most (a cast's spend, a frame in play).
void FillPlayerNinpo() {
  uint32_t player = ng2::guest::Load32(kPlayerRecord);
  if (player < 0x40000000u || player >= 0xA0000000u ||
      ng2::guest::Virtual(kPlayerRecord)[10] != 0 ||
      ng2::guest::Virtual(player)[0x59D3] == 0xFF || ng2::guest::Virtual(player)[0x59BF] != 0) {
    return;
  }
  uint16_t most = ng2::guest::Load16(player + kNinpoMost);
  if (most && most <= 0x2000 && ng2::guest::Load16(player + kNinpo) < most) {
    uint16_t value = rex::byte_swap(most);
    std::memcpy(ng2::guest::Virtual(player + kNinpo), &value, 2);
  }
}
