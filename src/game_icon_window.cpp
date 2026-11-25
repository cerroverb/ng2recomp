// ng2 - Ninja Gaiden II static recompilation
//
// The game's icon on the window (game_icon.h). It is made once from the game
// files (about 150 ms) and kept in the data folder's icon/ as ng2_<size>.png,
// so later starts only read three or four small PNGs; both happen on a thread
// of their own, and the icon is set through the UI thread once it is there.
// SDL gives it to the desktop: _NET_WM_ICON on X11, xdg-toplevel-icon on
// Wayland (where the compositor has it: KDE Plasma 6.1 on; GNOME doesn't),
// WM_SETICON on Windows. Without it the window keeps the packaged icon (the
// executable's own on Windows; on Linux the desktop's choice for the app).

#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app_context.h>

#include "game_icon.h"

namespace fs = std::filesystem;

namespace ng2::game_icon {
namespace {

// Written next to the cached pictures: changed when the drawing changes, so
// older pictures are made again.
constexpr std::string_view kStamp = "n_plate_over_ii2 1\n";

// The thread, waited for at shutdown (Stop) or else at exit: one left
// joinable would end the program.
struct Worker {
  std::thread thread;
  ~Worker() {
    if (thread.joinable()) thread.join();
  }
} g_worker;

fs::path PicturePath(const fs::path& dir, int size) {
  return dir / ("ng2_" + std::to_string(size) + ".png");
}

bool LoadCache(const fs::path& dir, std::vector<Picture>* out) {
  std::ifstream stamp(dir / "made.txt", std::ios::binary);
  std::stringstream text;
  text << stamp.rdbuf();
  if (!stamp || text.str() != kStamp) return false;
  out->clear();
  for (int size : kSizes) {
    SDL_Surface* loaded = SDL_LoadPNG(rex::path_to_utf8(PicturePath(dir, size)).c_str());
    SDL_Surface* s = loaded ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : nullptr;
    SDL_DestroySurface(loaded);
    bool ok = s && s->w == size && s->h == size;
    if (ok) {
      Picture p{size, std::vector<uint8_t>(size_t(size) * size * 4)};
      for (int y = 0; y < size; ++y) {
        std::memcpy(&p.rgba[size_t(y) * size * 4], static_cast<uint8_t*>(s->pixels) + y * s->pitch,
                    size_t(size) * 4);
      }
      out->push_back(std::move(p));
    }
    SDL_DestroySurface(s);
    if (!ok) return false;
  }
  return true;
}

// Each picture written beside its place and moved in, the stamp last: a
// start cut short leaves no half-written cache.
void SaveCache(const fs::path& dir, std::vector<Picture>& pictures) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::remove(dir / "made.txt", ec);
  for (auto& p : pictures) {
    SDL_Surface* s = SDL_CreateSurfaceFrom(p.size, p.size, SDL_PIXELFORMAT_RGBA32, p.rgba.data(),
                                           p.size * 4);
    fs::path path = PicturePath(dir, p.size), part = path;
    part += ".part";
    bool ok = s && SDL_SavePNG(s, rex::path_to_utf8(part).c_str());
    SDL_DestroySurface(s);
    if (!ok || (fs::rename(part, path, ec), ec)) {
      REXLOG_WARN("window icon: can't write {}: {}", rex::path_to_utf8(path),
                  ok ? ec.message() : SDL_GetError());
      fs::remove(part, ec);
      return;
    }
  }
  std::ofstream(dir / "made.txt", std::ios::binary) << kStamp;
}

const Picture* Find(const std::vector<Picture>& pictures, int size) {
  for (const auto& p : pictures) {
    if (p.size == size) return &p;
  }
  return nullptr;
}

// On the UI thread. Windows takes one picture (for the title bar and the
// taskbar): the one for the taskbar's 32 at the display's scale. On Linux 64,
// with 128 and 256 for Wayland's scaled displays (X11 takes the first alone).
void SetIcon(std::vector<Picture>& pictures, const std::string& how) {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!window) {
    REXLOG_WARN("window icon: no window");
    return;
  }
#ifdef _WIN32
  const float want = 32 * SDL_GetWindowDisplayScale(window);
  int main_size = 256;
  for (int size : {32, 48, 64, 128}) {
    if (size >= want) {
      main_size = size;
      break;
    }
  }
  const int extra[] = {0};
#else
  const int main_size = 64;
  const int extra[] = {128, 256};
#endif
  auto surface = [&](int size) -> SDL_Surface* {
    const Picture* p = Find(pictures, size);
    return p ? SDL_CreateSurfaceFrom(size, size, SDL_PIXELFORMAT_RGBA32,
                                     const_cast<uint8_t*>(p->rgba.data()), size * 4)
             : nullptr;
  };
  SDL_Surface* icon = surface(main_size);
  if (!icon) return;
  for (int size : extra) {
    if (SDL_Surface* s = size ? surface(size) : nullptr) {
      SDL_AddSurfaceAlternateImage(icon, s);
      SDL_DestroySurface(s);  // (the icon holds it)
    }
  }
  if (SDL_SetWindowIcon(window, icon)) {
    REXLOG_INFO("window icon: set from the game files ({} px; {})", main_size, how);
  } else {
    REXLOG_INFO("window icon: not taken ({})", SDL_GetError());
  }
  SDL_DestroySurface(icon);
}

}  // namespace

void Start(rex::ui::WindowedAppContext* app_context, const fs::path& game_root,
           const fs::path& cache_dir) {
  if (!app_context || g_worker.thread.joinable()) return;
  g_worker.thread = std::thread([app_context, game_root, cache_dir] {
    auto pictures = std::make_shared<std::vector<Picture>>();
    auto start = std::chrono::steady_clock::now();
    auto ms = [&] {
      return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
    };
    std::string how;
    try {
      if (LoadCache(cache_dir, pictures.get())) {
        how = "read from " + rex::path_to_utf8(cache_dir) + " in " + ms() + " ms";
      } else {
        // A failure is remembered for these game files (its stamp: the
        // index's size), so other discs aren't searched again each start.
        std::error_code ec;
        std::string failed = std::string(kStamp) +
                             std::to_string(fs::file_size(game_root / "load_img.bin", ec)) + "\n";
        std::ifstream before(cache_dir / "failed.txt", std::ios::binary);
        std::stringstream text;
        text << before.rdbuf();
        if (before && text.str() == failed) {
          REXLOG_INFO("window icon: the packaged one (the game's files gave none before)");
          return;
        }
        std::string why;
        if (!Make(game_root, pictures.get(), &why)) {
          REXLOG_INFO("window icon: the packaged one ({})", why);
          fs::create_directories(cache_dir, ec);
          std::ofstream(cache_dir / "failed.txt", std::ios::binary) << failed;
          return;
        }
        fs::remove(cache_dir / "failed.txt", ec);
        how = "made in " + ms() + " ms";
        SaveCache(cache_dir, *pictures);
      }
    } catch (const std::exception& e) {
      REXLOG_INFO("window icon: the packaged one ({})", e.what());
      return;
    }
    app_context->CallInUIThread([pictures, how] { SetIcon(*pictures, how); });
  });
}

void Stop() {
  if (g_worker.thread.joinable()) g_worker.thread.join();
}

}  // namespace ng2::game_icon
