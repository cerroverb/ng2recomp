// ng2 - Ninja Gaiden II static recompilation
//
// The window's icon, made at start from the player's own game files: the N of
// the title logo on its metal plate with the red brush-stroke II over its
// right edge, on a black rounded tile (src/game_icon.cpp). The art is the
// game's, so none of it is in the repo or the packages; when the game files
// can't be read the window keeps the packaged icon.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::ui {
class WindowedAppContext;
}

namespace ng2::game_icon {

// The sizes made, largest first.
inline constexpr int kSizes[] = {512, 256, 128, 64, 48, 32, 16};

struct Picture {
  int size = 0;
  std::vector<uint8_t> rgba;  // size x size, R G B A, not premultiplied
};

// The icon at every one of kSizes from the game folder (the title screen's
// sprite pack in other.ng2). False, with why, if the files can't be read.
bool Make(const std::filesystem::path& game_root, std::vector<Picture>* out, std::string* why);

// Sets the window's icon on a thread of its own: from <cache_dir>/ng2_<size>.png,
// or made with Make and written there for the next start. Nothing changes if
// neither works.
void Start(rex::ui::WindowedAppContext* app_context, const std::filesystem::path& game_root,
           const std::filesystem::path& cache_dir);

// Waits for that thread (at shutdown).
void Stop();

}  // namespace ng2::game_icon
