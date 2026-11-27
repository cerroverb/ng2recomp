// ng2 - Ninja Gaiden II static recompilation
//
// The icon a secret achievement shows until it's unlocked: a chrome padlock
// on the weathered grey tile of the game's own achievement icons, made at
// run time from those icons (nothing of the game's art is stored). The C++
// of tools/lock_art.py, which has the mock-up and the reasoning.

#pragma once

#include <cstdint>
#include <vector>

namespace ng2::lock_icon {

constexpr uint32_t kSize = 64;
// The XDBF images it's made from: the nine grey "nine stars" tiles (one
// stone ground under different chrome characters).
constexpr uint32_t kFirstTile = 0x21, kTileCount = 9;

// The padlock (kSize x kSize RGBA) from the tiles (each kSize x kSize RGBA);
// empty if fewer than five were given.
std::vector<uint8_t> Build(const std::vector<std::vector<uint8_t>>& tiles);

}  // namespace ng2::lock_icon
