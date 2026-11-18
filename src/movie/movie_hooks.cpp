// ng2 - Ninja Gaiden II static recompilation
//
// The game's movie player is the XDK's XMedia2 XMV player (library at
// 0x8386xxxx). Its WMV decoder runs part of its work on the GPU and reads
// the results back, which neither renderer provides: movies end at once or
// stall the game (the opening's billboard). So by default the player is
// replaced: creating one opens a HostMovie (FFmpeg) and returns a small
// guest object standing for it, and the method wrappers the game calls (it
// never calls the player's vtable itself) are answered on the host.
// NG2_MOVIES=xmedia keeps the original player (no longer whole: Modern Camera's code, camera.cpp, sits over
// the start of its WMV decoder, sub_83A42C90); NG2_MOVIE_TRACE=1 logs calls.
//
// What the game uses (from its calls and a trace of the original):
//   create(device, params, &player): params +4 flags (0x8: loop, the
//     opening's billboard), +24 audio stream (-1 none), +32 path
//     ("game:\x.wmv")
//   +72 frame(player, desc): desc +12/+16/+20 Y/U/V addresses (linear L8
//     textures the game created), +24/+28/+32 sizes, +36/+40/+44 pitches;
//     S_OK, or 0x16660026 once the movie has ended
//   +80 render(player, 0, 0): draw the frame to the screen (full-screen
//     movies)
//   +112 status via 83888190(player, out): out[0] < 6 playing
//   +120 audio(player, &voice): the game sets voice+140 to the volume and
//     calls voice->vtbl[+92]
//   +8 release, +96 stop, +92 and 838880A8 at the start of full-screen
//     movies
//
// The original player also allocates physical memory for its frames when
// it is created, before the game creates the textures the frames go to.
// The game depends on that layout: its title movie setup clears a texture
// that, without those allocations, lands on the archive's table of contents
// (0xEBEB0000), and the title's font and sprites then never load. So the
// stand-in makes the same physical allocations (ReservePhysical) and holds
// them until the game releases the player.

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "../gpu/guest.h"
#include "../gpu/video.h"
#include "host_movie.h"
#include "movie.h"
#include "../gameplay.h"

REX_EXTERN(__imp__sub_83835960);  // XMemAlloc(size, attributes)
REX_EXTERN(__imp__sub_838359F8);  // XMemFree(address, attributes)

namespace {

using ng2::guest::Load32;
using ng2::guest::LoadFloat;
using ng2::guest::Virtual;

constexpr uint32_t kEndOfMovie = 0x16660026;
constexpr uint32_t kFail = 0x80004005;  // E_FAIL
// A guest function that returns 0 (li r3,0; blr): every slot of the
// stand-in audio voice's vtable.
constexpr uint32_t kReturnZero = 0x8242C148;
constexpr uint32_t kVoiceVtableSlots = 64;
constexpr uint32_t kVoiceSize = 256;
constexpr uint32_t kObjectSize = 64;

bool TraceEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("NG2_MOVIE_TRACE");
    return v && v[0] == '1';
  }();
  return enabled;
}

bool HostMovies() {
  static const bool enabled = [] {
    const char* v = std::getenv("NG2_MOVIES");
    return !(v && std::string_view(v) == "xmedia");
  }();
  return enabled;
}

void Store32(uint32_t address, uint32_t value) {
  *reinterpret_cast<uint32_t*>(Virtual(address)) = rex::byte_swap(value);
}

rex::memory::Memory* GuestMemory() {
  return rex::Runtime::instance()->memory();
}

// Calls a guest function with r3/r4 on the caller's guest stack, leaving
// the caller's registers as they were.
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*),
                   uint32_t r3, uint32_t r4) {
  PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  function(ctx, base);
  uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

struct Player {
  std::unique_ptr<ng2::movie::HostMovie> movie;  // null if the file didn't open
  uint32_t voice = 0;                             // guest stand-in audio voice
  std::vector<std::pair<uint32_t, uint32_t>> reserved;  // (address, attributes)
  bool reportedEnd = false;
  bool blanked = false;  // its planes cleared to black
  std::vector<uint8_t> fullScreen[3];  // a full-screen movie's Y, U, V
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Player> g_players;  // by guest object address
std::filesystem::path g_gameDataRoot;

// "game:\dir\name.wmv" -> <game data root>/dir/name.wmv, each component
// matched case-insensitively like the SDK's host path device (the disc has
// both .wmv and .WMV). Empty if the path leaves the root or isn't found.
std::filesystem::path HostPath(const char* guestPath) {
  std::string_view path(guestPath);
  if (auto colon = path.find(":\\"); colon != std::string_view::npos) {
    path.remove_prefix(colon + 2);
  }
  auto lower = [](std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return text;
  };
  std::filesystem::path result = g_gameDataRoot;
  while (!path.empty()) {
    size_t slash = path.find('\\');
    std::string part(path.substr(0, slash));
    path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
    if (part.empty() || part == ".") {
      continue;
    }
    if (part == "..") {
      return {};
    }
    std::error_code ec;
    std::filesystem::path match;
    for (const auto& entry : std::filesystem::directory_iterator(result, ec)) {
      if (lower(entry.path().filename().string()) == lower(part)) {
        match = entry.path();
        break;
      }
    }
    if (match.empty()) {
      return {};
    }
    result = match;
  }
  return result;
}

// The physical allocations the original player makes when created (traced
// for the 320x580 title movies): control blocks, then three frames of Y, U
// and V with 256-byte aligned pitches. Made even when the movie can't be
// played, since the game's memory layout depends on them.
constexpr int kDefaultWidth = 320, kDefaultHeight = 580;
void ReservePhysical(PPCContext& ctx, uint8_t* base, Player& player, int width, int height) {
  auto align = [](uint32_t value) { return (value + 255) & ~255u; };
  std::vector<std::pair<uint32_t, uint32_t>> blocks = {
      {0x0C, 0xB2800000}, {0x78, 0xB2800000}, {0x60, 0xB5800000},
      {0xB8, 0xB5800000}, {0xB8, 0xB5800000},
  };
  uint32_t lumaSize = align(uint32_t(width)) * uint32_t(height);
  uint32_t chromaSize = align(uint32_t(width + 1) / 2) * uint32_t((height + 1) / 2);
  for (int frame = 0; frame < 3; frame++) {
    blocks.push_back({lumaSize, 0xBC800000});
    blocks.push_back({chromaSize, 0xBC800000});
    blocks.push_back({chromaSize, 0xBC800000});
  }
  for (auto [size, attributes] : blocks) {
    uint32_t address = CallGuest(ctx, base, __imp__sub_83835960, size, attributes);
    if (address) {
      player.reserved.push_back({address, attributes});
    }
  }
}

void FreeReserved(PPCContext& ctx, uint8_t* base, Player& player) {
  for (auto it = player.reserved.rbegin(); it != player.reserved.rend(); ++it) {
    CallGuest(ctx, base, __imp__sub_838359F8, it->first, it->second);
  }
  player.reserved.clear();
}

uint32_t CreateVoice() {
  uint32_t vtable = GuestMemory()->SystemHeapAlloc(kVoiceVtableSlots * 4);
  uint32_t voice = GuestMemory()->SystemHeapAlloc(kVoiceSize);
  if (!vtable || !voice) {
    if (vtable) {
      GuestMemory()->SystemHeapFree(vtable);
    }
    if (voice) {
      GuestMemory()->SystemHeapFree(voice);
    }
    return 0;
  }
  for (uint32_t i = 0; i < kVoiceVtableSlots; i++) {
    Store32(vtable + i * 4, kReturnZero);
  }
  std::memset(Virtual(voice), 0, kVoiceSize);
  Store32(voice, vtable);
  return voice;
}

void FreeVoice(uint32_t voice) {
  if (voice) {
    GuestMemory()->SystemHeapFree(Load32(voice));
    GuestMemory()->SystemHeapFree(voice);
  }
}

// The host player for a guest object, or null if it isn't one.
Player* Find(uint32_t object) {
  auto it = g_players.find(object);
  return it != g_players.end() ? &it->second : nullptr;
}

}  // namespace

namespace ng2::movie {

void SetGameDataRoot(const std::filesystem::path& root) {
  g_gameDataRoot = root;
}

bool AnyPlaying() {
  std::lock_guard lock(g_mutex);
  for (const auto& [object, player] : g_players) {
    if (player.movie && !player.reportedEnd) {
      return true;
    }
  }
  return false;
}

}  // namespace ng2::movie

// XMedia2CreateXmvPlayer(device, parameters, &player)
REX_EXTERN(__imp__sub_838886D8);
REX_HOOK_RAW(sub_838886D8) {
  uint32_t params = ctx.r4.u32, out = ctx.r5.u32;
  uint32_t pathAddress = Load32(params) == 1 ? Load32(params + 32) : 0;
  const char* path = pathAddress ? reinterpret_cast<const char*>(Virtual(pathAddress)) : "";
  // Only movies from files are replaced.
  if (!HostMovies() || !pathAddress) {
    __imp__sub_838886D8(ctx, base);
    if (TraceEnabled()) {
      REXLOG_INFO("movie: create '{}' -> {:#x} player {:#x}", path, ctx.r3.u32, Load32(out));
    }
    return;
  }
  uint32_t object = GuestMemory()->SystemHeapAlloc(kObjectSize);
  if (!object) {
    ctx.r3.u64 = kFail;
    return;
  }
  std::memset(Virtual(object), 0, kObjectSize);
  Player player;
  std::filesystem::path hostPath = HostPath(path);
  // Skip Intro: the boot movie's three streams (NinjaVI, _Left, _Right),
  // the first time each opens, aren't opened: they end at their first frame.
  static std::vector<std::string> introOpened;
  std::string name = hostPath.filename().string();
  bool intro = name.starts_with("NinjaVI") &&
               std::find(introOpened.begin(), introOpened.end(), name) == introOpened.end();
  if (intro) {
    introOpened.push_back(name);
  }
  if (intro && ng2::gameplay::SkipIntro()) {
    REXLOG_INFO("movie: '{}' skipped (Skip Intro)", path);
  } else if (hostPath.empty()) {
    REXLOG_ERROR("movie: '{}' not found", path);
  } else {
    player.movie = ng2::movie::HostMovie::Open(hostPath, Load32(params + 24) != 0xFFFFFFFF,
                                               (Load32(params + 4) & 0x8) != 0);
  }
  ReservePhysical(ctx, base, player, player.movie ? player.movie->Width() : kDefaultWidth,
                  player.movie ? player.movie->Height() : kDefaultHeight);
  player.voice = CreateVoice();
  std::lock_guard lock(g_mutex);
  if (TraceEnabled()) {
    REXLOG_INFO("movie: create '{}' -> host player {:#x} ({} reserved blocks)", path, object,
                player.reserved.size());
  }
  g_players[object] = std::move(player);
  Store32(out, object);
  ctx.r3.u64 = 0;
}

// The planes frames were written to lately (physical addresses), for
// IsFrameTexture: a few movies' worth, each with when a frame last went to it
// (a playing movie writes one every frame; a freed plane's address may be a
// later texture's, so an entry counts only while its movie plays).
constexpr size_t kFrameTextures = 12;
constexpr int64_t kFrameTextureMs = 1000;
std::atomic<uint32_t> g_frameTextures[kFrameTextures];
std::atomic<int64_t> g_frameTextureMs[kFrameTextures];
std::atomic<uint32_t> g_frameTextureNext{0};
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void NoteFrameTexture(uint32_t address) {
  if (!address) {
    return;
  }
  uint32_t physical = (address & 0x1FFFFFFF) + (address >= 0xE0000000u ? 0x1000u : 0u);
  int64_t now = NowMs();
  for (size_t i = 0; i < kFrameTextures; i++) {
    if (g_frameTextures[i].load(std::memory_order_relaxed) == physical) {
      g_frameTextureMs[i].store(now, std::memory_order_relaxed);
      return;
    }
  }
  size_t i = g_frameTextureNext++ % kFrameTextures;
  g_frameTextureMs[i].store(now, std::memory_order_relaxed);
  g_frameTextures[i].store(physical, std::memory_order_relaxed);
}

namespace ng2::movie {
bool IsFrameTexture(uint32_t physical) {
  int64_t now = NowMs();
  for (size_t i = 0; i < kFrameTextures; i++) {
    if (g_frameTextures[i].load(std::memory_order_relaxed) == physical &&
        now - g_frameTextureMs[i].load(std::memory_order_relaxed) < kFrameTextureMs) {
      return true;
    }
  }
  return false;
}
}  // namespace ng2::movie

// +72: the frame due now into the caller's Y/U/V planes
REX_EXTERN(__imp__sub_836A41A8);
REX_HOOK_RAW(sub_836A41A8) {
  uint32_t object = ctx.r3.u32, desc = ctx.r4.u32;
  {
    std::lock_guard lock(g_mutex);
    if (Player* player = Find(object)) {
      bool playing = false;
      // Black until the first frame (the game's textures start zeroed,
      // which is green in YUV): a movie skipped at once, or not opened.
      if (!player->blanked) {
        player->blanked = true;
        for (int p = 0; p < 3; p++) {
          uint32_t address = Load32(desc + 12 + p * 4);
          if (address) {
            std::memset(Virtual(address), p == 0 ? 16 : 128, Load32(desc + 24 + p * 4));
          }
        }
      }
      for (int p = 0; p < 3; p++) {
        NoteFrameTexture(Load32(desc + 12 + p * 4));
      }
      if (player->movie) {
        ng2::movie::HostMovie::Planes planes;
        for (int p = 0; p < 3; p++) {
          uint32_t address = Load32(desc + 12 + p * 4);
          planes.data[p] = address ? Virtual(address) : nullptr;
          planes.size[p] = Load32(desc + 24 + p * 4);
          planes.pitch[p] = Load32(desc + 36 + p * 4);
        }
        playing = player->movie->CopyFrame(planes);
        if (player->voice) {
          player->movie->SetVolume(LoadFloat(player->voice + 140));
        }
      }
      if (!playing && !player->reportedEnd) {
        player->reportedEnd = true;
        if (TraceEnabled()) {
          REXLOG_INFO("movie: {:#x} ended", object);
        }
      }
      ctx.r3.u64 = playing ? 0 : kEndOfMovie;
      return;
    }
  }
  __imp__sub_836A41A8(ctx, base);
}

// +80: draw the frame to the screen (full-screen movies, such as the attract
// trailer): the renderer shows it instead of the game's picture.
REX_EXTERN(__imp__sub_836A41B8);
REX_HOOK_RAW(sub_836A41B8) {
  {
    std::lock_guard lock(g_mutex);
    if (Player* player = Find(ctx.r3.u32)) {
      bool playing = false;
      if (player->movie) {
        // Decoded into host planes of ours, and shown by the renderer in
        // place of the game's picture.
        uint32_t width = uint32_t(player->movie->Width()), height = uint32_t(player->movie->Height());
        uint32_t pitches[3] = {width, (width + 1) / 2, (width + 1) / 2};
        uint32_t rows[3] = {height, (height + 1) / 2, (height + 1) / 2};
        ng2::movie::HostMovie::Planes planes;
        for (int p = 0; p < 3; p++) {
          player->fullScreen[p].resize(size_t(pitches[p]) * rows[p]);
          planes.data[p] = player->fullScreen[p].data();
          planes.pitch[p] = pitches[p];
          planes.size[p] = uint32_t(player->fullScreen[p].size());
        }
        double before = player->movie->ShownTime();
        playing = player->movie->CopyFrame(planes);
        if (playing && player->movie->ShownTime() != before) {
          const uint8_t* data[3] = {planes.data[0], planes.data[1], planes.data[2]};
          ng2::video::ShowMovieFrame(data, pitches, width, height);
        } else if (playing && player->movie->ShownTime() >= 0.0) {
          ng2::video::KeepMovieShown();
        }
        if (player->voice) {
          player->movie->SetVolume(LoadFloat(player->voice + 140));
        }
      }
      ctx.r3.u64 = playing ? 0 : kEndOfMovie;
      return;
    }
  }
  __imp__sub_836A41B8(ctx, base);
}

// status(player, out): out[0] = state (6 and up: finished), out[1]
REX_EXTERN(__imp__sub_83888190);
REX_HOOK_RAW(sub_83888190) {
  uint32_t object = ctx.r3.u32, out = ctx.r4.u32;
  {
    std::lock_guard lock(g_mutex);
    if (Player* player = Find(object)) {
      bool finished = !player->movie || player->movie->Finished();
      Store32(out, finished ? 6 : 1);
      Store32(out + 4, 0);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_83888190(ctx, base);
}

// +120: the movie's audio voice
REX_EXTERN(__imp__sub_838881F8);
REX_HOOK_RAW(sub_838881F8) {
  uint32_t object = ctx.r3.u32, out = ctx.r4.u32;
  {
    std::lock_guard lock(g_mutex);
    if (Player* player = Find(object)) {
      Store32(out, player->voice);
      ctx.r3.u64 = player->voice ? 0 : kFail;
      return;
    }
  }
  __imp__sub_838881F8(ctx, base);
}

// +8: release
REX_EXTERN(__imp__sub_83888098);
REX_HOOK_RAW(sub_83888098) {
  uint32_t object = ctx.r3.u32;
  {
    std::optional<Player> player;
    {
      std::lock_guard lock(g_mutex);
      if (auto it = g_players.find(object); it != g_players.end()) {
        player = std::move(it->second);
        g_players.erase(it);
      }
    }
    if (player) {
      if (TraceEnabled()) {
        REXLOG_INFO("movie: release {:#x}", object);
      }
      player->movie.reset();  // joins the decoder
      FreeReserved(ctx, base, *player);
      FreeVoice(player->voice);
      GuestMemory()->SystemHeapFree(object);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_83888098(ctx, base);
}

// Methods with nothing to do on the host: +96 stop, +92 and 838880A8 (at
// the start of full-screen movies).
#define NG2_PLAYER_NO_OP(addr)                     \
  REX_EXTERN(__imp__sub_##addr);                   \
  REX_HOOK_RAW(sub_##addr) {                       \
    {                                              \
      std::lock_guard lock(g_mutex);               \
      if (Find(ctx.r3.u32)) {                      \
        ctx.r3.u64 = 0;                            \
        return;                                    \
      }                                            \
    }                                              \
    __imp__sub_##addr(ctx, base);                  \
  }

NG2_PLAYER_NO_OP(83888180)
NG2_PLAYER_NO_OP(83888FD8)
NG2_PLAYER_NO_OP(838880A8)
