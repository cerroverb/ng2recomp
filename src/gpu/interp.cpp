// ng2 - Ninja Gaiden II static recompilation
//
// Frame interpolation (experimental): pictures made between the game's
// frames from its own draws. Each frame's draws are captured with the
// constants their shaders read and their bone palettes (CaptureInterpDraw,
// NoteInterpPalette, from video.cpp's draws); at the present the frame is
// matched with the last one, draw by draw, and for each picture between the
// two every matched draw gets its constants and bones part of the way
// (BuildInterpFrames, on the pool's threads). video.cpp then runs the
// frame's commands again with those, and presents the pictures in time.

#include "interp.h"

#include "../gameplay.h"
#include "../aspect.h"
#include "guest.h"
#include "video.h"

#include <algorithm>
#include <functional>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(int32_t, frame_interpolation);

using namespace plume;

namespace ng2::video {

WorkerPool& g_interpPool = *new WorkerPool;

std::atomic<uint32_t> g_interpPictureCap{4};
// The last frame's build (game thread's): waited for before the next, and
// before interpolation is switched.
std::shared_future<std::shared_ptr<InterpFrames>> g_interpLastJob;
// From a frame's present to its pictures handed to the presenter (the build
// and the recording), the slowest lately: the pictures' first time is after it.
std::atomic<int64_t> g_interpReadyLagNs{4'000'000};
std::atomic<int64_t> g_interpLateNs{0};
namespace {

using Upload = InterpUpload;

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Half rate set by the autopause (2) while the pictures cost the game frames:
// half the pictures' GPU work, before pausing them altogether.
std::atomic<uint32_t> g_interpAutoDivider{1};
uint32_t InterpSetDivider() {
  if (REXCVAR_GET(frame_interpolation) != 2) {
    return 1;
  }
  // Half Rate: each second refresh only where that still leaves a picture
  // between the game's frames (above 125 Hz: 72 a second at 144 Hz, 120 at
  // 240 Hz). At 120 Hz and below each second one is the game's own frames, a
  // frame late and nothing between: each refresh there, as Full Rate.
  int64_t refresh = InterpRefreshNs();
  if (refresh <= 0) {
    return 2;
  }
  return refresh < 8'000'000 ? 2u : 1u;
}
}  // namespace

uint32_t InterpDivider() {
  return std::max(InterpSetDivider(), g_interpAutoDivider.load());
}
namespace {

// The autopause's own (the build's; ResetInterp clears it: left from a
// pause, it kept the pictures off for up to 24 s after the setting was
// turned off and on again).
struct InterpAuto {
  int64_t pausedUntil = 0, holdNs = 0, resumedNs = 0, lastNs = 0;
  int64_t halvedNs = 0, halfHoldNs = 0, fullAgainNs = 0;  // (half rate)
  double buildAvg = 0, gapAvg = 1e9 / 60.0, gapFast = 1e9 / 60.0, gapBefore = 0, ownRate = 0, pausedSum = 0;
  uint32_t behind = 0, pausedCount = 0, sharpRun = 0;
  uint32_t gameInterval = 0;  // the game's limiter word, as last logged
  double gameOwn = 0;         // the frame it asks for, kept ownFrames frames past the word's return to 60
  uint32_t ownFrames = 0;
};

struct InterpState {
  InterpAuto autopause;
  std::vector<InterpDraw> previous, current;
  std::vector<InterpDraw> older;  // the frame before the previous (things the previous didn't draw)
  std::vector<InterpPalette> previousPalettes, palettes;
  std::unordered_map<uint64_t, int32_t> paletteIndex;  // this frame's, by address and size
  uint64_t bonesLerped = 0, bonesBytes = 0, palettesJumped = 0, bonesSnapped = 0;
  int64_t buildNs = 0, matchNs = 0, constantsNs = 0, bonesNs = 0;
  int64_t prepNs = 0, meshNs = 0, worstNs = 0;  // (worst: the slowest frame's whole build)
  // The slowest frame's split: setup, by mesh, the rest of matching, constants, bones.
  std::array<int64_t, 5> worstSplit{};
  uint64_t drawsSeen = 0, drawsWorst = 0;
  uint64_t frames = 0, interpolated = 0, draws = 0, matched = 0, snapped = 0, registers = 0;
  uint64_t fromOlder = 0;  // things not drawn the frame before, taken from the one before it
  uint64_t borrowed = 0, byMesh = 0, cuts = 0, jumped = 0, partial = 0, psDraws = 0, paused = 0;
  int64_t lastBuildNs = 0;  // the last frame's build
  uint32_t keptFor = 0;  // frames the last full frame was kept over (partial ones)
  int64_t lastFullNs = 0;  // the last full frame's present (game thread's clock)
  int64_t lastPresentNs = 0;
  double intervalNs = 1e9 / 60.0;  // between the game's frames, smoothed
  int64_t endNs = 0;      // when the last paced frame's own picture is shown: the next one's start (0: none)
  // (the grid, where the game's frame isn't a whole number of steps)
  int64_t baseNs = 0;     // the last paced frame's start: its pictures after it
  int64_t windowNs = 0;   // the time the last paced frame's pictures covered
  int64_t gridNs = 0;     // the next picture's time on the display's grid
  int64_t latencyNs = 0;  // from a paced frame's pictures being ready to its own shown
  // (paced frames, for the log: the pictures between, frames a refresh more or
  // fewer than usual, timelines started anew)
  uint64_t pacedFrames = 0, pacedBetween = 0, pacedUneven = 0, pacedAnew = 0;
  uint32_t slowRun = 0;    // full frames in a row a long time apart
  bool slow = false;  // draws without a match given the shared registers' values before
};
InterpState g_interp;  // the build's (its thread's while one runs)
// What this frame's draws are, filled as they're drawn (game thread); handed
// to the build at the present.
struct InterpCapture {
  std::vector<InterpDraw> current;
  std::vector<InterpPalette> palettes;
  std::unordered_map<uint64_t, int32_t> paletteIndex;
};
InterpCapture g_interpCapture;

}  // namespace

// What the setting (or NG2_INTERP=1/0, which overrides it) asks for.
bool InterpWanted() {
  const char* v = std::getenv("NG2_INTERP");
  if (v && v[0] ? v[0] != '1' : REXCVAR_GET(frame_interpolation) <= 0) {
    return false;
  }
  if (!UsesRenderThread()) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("video: interpolation needs the render thread (NG2_RENDER_THREAD=0 is set): off");
    }
    return false;
  }
  return true;
}
// On or off for the frame being made: changed between frames only
// (SwitchInterp), as the setting is.
std::atomic<int> g_interpOn{-1};
bool InterpEnabled() {
  int on = g_interpOn.load();
  if (on < 0) {
    on = InterpWanted() ? 1 : 0;
    g_interpOn = on;
  }
  return on != 0;
}

// Pictures between two frames where the display's refresh isn't known: 2
// (180 a second for the game's 60).
constexpr uint32_t kInterpSteps = 2;
// The game's frame limiter: vblanks a frame (frame_wait.cpp).
constexpr uint32_t kGameInterval = 0x84C5342C;

int32_t CaptureInterpDraw(uint64_t group, const uint32_t* guestConstants,
                          const uint64_t (&mask)[4], const uint32_t* psConstants,
                          const uint64_t (*psMask)[4]) {
  InterpDraw draw;
  draw.group = group;
  draw.ui = ng2::aspect::In2DPass();
  if (psConstants && psMask) {
    for (uint32_t word = 0; word < 4; word++) {
      for (uint64_t bits = (*psMask)[word]; bits; bits &= bits - 1) {
        uint32_t r = word * 64 + uint32_t(__builtin_ctzll(bits));
        if (r < 224) {
          draw.psRegisters.push_back(uint16_t(r));
          for (uint32_t c = 0; c < 4; c++) {
            draw.psConstants.push_back(std::bit_cast<float>(rex::byte_swap(psConstants[r * 4 + c])));
          }
        }
      }
    }
  }
  for (uint32_t word = 0; word < 4; word++) {
    for (uint64_t bits = mask[word]; bits; bits &= bits - 1) {
      uint32_t r = word * 64 + uint32_t(__builtin_ctzll(bits));
      if (r < 256) {
        draw.registers.push_back(uint16_t(r));
        for (uint32_t c = 0; c < 4; c++) {
          draw.constants.push_back(std::bit_cast<float>(rex::byte_swap(guestConstants[r * 4 + c])));
        }
      }
    }
  }
  g_interpCapture.current.push_back(std::move(draw));
  return int32_t(g_interpCapture.current.size() - 1);
}

int32_t NoteInterpPalette(uint32_t address, uint32_t size) {
  auto [it, added] =
      g_interpCapture.paletteIndex.emplace((uint64_t(address) << 32) | size, int32_t(g_interpCapture.palettes.size()));
  if (added) {
    g_interpCapture.palettes.push_back({address, size, {}});
  }
  return it->second;
}

InterpDraw& InterpCaptured(int32_t index) {
  return g_interpCapture.current[index];
}

void NoteInterpDraw(int32_t index, const uint64_t (&pushConstants)[3]) {
  if (index >= 0 && size_t(index) < g_interpCapture.current.size()) {
    auto& draw = g_interpCapture.current[index];
    std::copy(std::begin(pushConstants), std::end(pushConstants), draw.pushConstants.begin());
    draw.submitted = true;
  }
}

namespace {

// The build's own containers (its maps by value and kind, its lists for each
// draw) come from one block a frame instead of a malloc and a free each: in a
// crowd (900 draws, 15k registers) those were a third of the build's time,
// the render thread waiting on it (#128). Each pool thread keeps its block,
// grown to what its biggest frame took (a build may overlap one dropped by a
// reset, on another thread).
class BuildArena {
 public:
  BuildArena() : resource_(Block().data.get(), Block().size, &upstream_) {}
  ~BuildArena() {
    resource_.release();
    auto& block = Block();
    if (upstream_.bytes && block.size < kMaxBlock) {
      block.size = std::min<size_t>(kMaxBlock, (block.size + upstream_.bytes) * 5 / 4);
      block.data = std::make_unique<std::byte[]>(block.size);
    }
  }
  std::pmr::memory_resource* get() { return &resource_; }

 private:
  static constexpr size_t kMaxBlock = 64u << 20;
  struct Owned {
    std::unique_ptr<std::byte[]> data = std::make_unique<std::byte[]>(1u << 20);
    size_t size = 1u << 20;
  };
  static Owned& Block() {
    static thread_local Owned block;
    return block;
  }
  // (past the block: from the heap, counted to grow the block for next time)
  struct Counting : std::pmr::memory_resource {
    size_t bytes = 0;
    void* do_allocate(size_t size, size_t alignment) override {
      bytes += size;
      return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* p, size_t size, size_t alignment) override {
      std::pmr::new_delete_resource()->deallocate(p, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
  };
  Counting upstream_;
  std::pmr::monotonic_buffer_resource resource_;
};

template <typename T>
using ArenaVector = std::pmr::vector<T>;
template <typename K, typename V>
using ArenaMap = std::pmr::unordered_map<K, V>;

// The build's tables by register value (a lookup or two for each register of
// each draw: std::unordered_map's node and prime modulo for each were a sixth
// of the build, #128): open addressing in the arena, sized once for the most
// entries it can get and never grown, so a value's reference stays put.
template <typename V>
class ValueMap {
 public:
  ValueMap(size_t most, std::pmr::memory_resource* arena) : slots_(arena) {
    size_t size = 16;
    while (size < most * 2) {
      size <<= 1;
    }
    slots_.resize(size);
    mask_ = size - 1;
  }
  // (the value, made if there's none)
  V& operator[](uint64_t key) {
    Slot& slot = Find(key);
    if (!slot.used) {
      slot.used = true;
      slot.key = key;
      count_++;
    }
    return slot.value;
  }
  V* find(uint64_t key) {
    Slot& slot = Find(key);
    return slot.used ? &slot.value : nullptr;
  }
  bool contains(uint64_t key) { return Find(key).used; }
  void emplace(uint64_t key, const V& value) {  // (kept if there's one)
    Slot& slot = Find(key);
    if (!slot.used) {
      slot.used = true;
      slot.key = key;
      slot.value = value;
      count_++;
    }
  }
  bool empty() const { return count_ == 0; }

 private:
  struct Slot {
    uint64_t key = 0;
    V value{};
    bool used = false;
  };
  Slot& Find(uint64_t key) {
    uint64_t h = key ^ (key >> 31);  // (the keys are mixed already: their high bits down)
    h *= 0x9E3779B97F4A7C15ull;
    for (size_t i = size_t(h >> 32) & mask_;; i = (i + 1) & mask_) {
      Slot& slot = slots_[i];
      if (!slot.used || slot.key == key) {
        return slot;
      }
    }
  }
  ArenaVector<Slot> slots_;
  size_t mask_ = 0, count_ = 0;
};

// How far apart two draws' constants are (the largest change, relative to
// each register's size).
float InterpDistance(const InterpDraw& a, const InterpDraw& b, float bound = 1e30f,
                     const ArenaVector<bool>* skip = nullptr) {
  float distance = 0;
  for (size_t r = 0; r * 4 < a.constants.size() && distance < bound; r++) {
    if (skip && r < skip->size() && (*skip)[r]) {
      continue;
    }
    const float* x = &a.constants[r * 4];
    const float* y = &b.constants[r * 4];
    float size = 1.0f, change = 0;
    for (uint32_t c = 0; c < 4; c++) {
      size = std::max({size, std::fabs(x[c]), std::fabs(y[c])});
      change = std::max(change, std::fabs(x[c] - y[c]));
    }
    float d = change / size;
    distance = std::max(distance, std::isfinite(d) ? d : 1e9f);
  }
  return distance;
}

// At the present (game thread): the push constants of each picture between
// the last frame and this one, or none (no last frame, too little matched).
// A value that changed more than this part of its size in a frame jumped (a
// light list sorted again, a new pose, a move to elsewhere): taken as it is
// now. Not less: a fast turn of the camera changes the matrices that have it
// by about half (at 30 degrees a frame), and those must go part of the way.
// Register r now holds what another register, itself changed, held before: a
// list sorted again (lights by distance), however little the values differ.
// (Not a value in many places: zeros, ones.)
static bool InterpMovedFromAnother(const float* before, const float* now, size_t count, size_t r) {
  const float* x = before + r * 4;
  const float* y = now + r * 4;
  if (std::memcmp(x, y, 16) == 0 || (y[0] == y[1] && y[1] == y[2] && y[2] == y[3])) {
    return false;
  }
  // As near its own value before as to another's: not moved, the same value
  // with a little float noise (a near-identity rotation row, which a chest's
  // two matrices share: outdoors, far from the origin, its last digits change
  // each frame, and the chest and its place were taken as now, a jerk with
  // the camera).
  bool nearOwn = true;
  for (uint32_t c = 0; c < 4 && nearOwn; c++) {
    nearOwn = std::fabs(x[c] - y[c]) <= 1e-4f * std::max(1.0f, std::fabs(y[c]));
  }
  if (nearOwn) {
    return false;
  }
  for (size_t r2 = 0; r2 < count; r2++) {
    const float* x2 = before + r2 * 4;
    if (r2 == r || std::memcmp(x2, now + r2 * 4, 16) == 0) {
      continue;
    }
    bool same = true;
    for (uint32_t c = 0; c < 4 && same; c++) {
      same = std::fabs(x2[c] - y[c]) <= 1e-4f * std::max(1.0f, std::fabs(y[c]));
    }
    if (same) {
      return true;
    }
  }
  return false;
}

// Each register from a moved one on, in a run (a light is several: where,
// colour, reach), taken as now.
static void InterpRunsFrom(const uint8_t* moved, const std::vector<uint16_t>& registers, uint8_t* asNow) {
  for (size_t r = 0; r < registers.size(); r++) {
    asNow[r] = 0;
    if (moved[r] || (r > 0 && asNow[r - 1] && registers[r] == registers[r - 1] + 1)) {
      asNow[r] = 1;
    }
  }
}

constexpr float kInterpJump = 0.6f;

// How far a bone may move in a frame and be mixed:
// more, it is as now (made just now, or given elsewhere for a frame).
constexpr float kBoneTeleport = 8.0f;

// How far most of a palette's bones may move in a frame and still be its
// own: more, another thing's bones.
constexpr float kPaletteJump = 0.75f;

std::shared_ptr<InterpFrames> BuildInterpFrames(uint32_t slot, uint64_t frameCount, int64_t presentNs) {
  // (its own uploads: the game thread's go on with the next frame)
  auto Allocate = [slot](uint64_t size, uint64_t alignment = 256) {
    return AllocateInterpUpload(slot, size, alignment);
  };
  auto& g = g_interp;
  int64_t startNs = SteadyNs();
  BuildArena arenaHolder;
  std::pmr::memory_resource* arena = arenaHolder.get();
  struct Timed {
    int64_t start;
    std::array<int64_t, 5> was;
    static std::array<int64_t, 5> Sums() {
      auto& g = g_interp;
      return {g.prepNs, g.meshNs, g.matchNs, g.constantsNs, g.bonesNs};
    }
    ~Timed() {
      int64_t ns = SteadyNs() - start;
      g_interp.buildNs += ns;
      auto now = Sums();
      // (not the frame whose stats line set the sums to 0: its split is lost)
      if (ns > g_interp.worstNs && now[2] >= was[2]) {
        g_interp.worstNs = ns;
        for (size_t k = 0; k < now.size(); k++) {
          g_interp.worstSplit[k] = now[k] - was[k];
        }
        g_interp.worstSplit[2] -= g_interp.worstSplit[0] + g_interp.worstSplit[1];
      }
      g_interp.lastBuildNs = ns;
    }
  } timed{startNs, Timed::Sums()};
  // Paused when the pictures cost the game its 60 (the Steam Deck in a big
  // fight: 7-12 ms of building a frame, the game at 30): the frames shown as
  // they are for a while (1.5 s, longer each time it comes back too soon), then
  // tried again. Whether they cost it is seen in the pause: a game no faster
  // without them (a cutscene at 30 of its own) isn't paused again while it
  // runs at that rate.
  {
    auto& a = g.autopause;
    int64_t &pausedUntil = a.pausedUntil, &holdNs = a.holdNs, &resumedNs = a.resumedNs, &lastNs = a.lastNs;
    int64_t &halvedNs = a.halvedNs, &halfHoldNs = a.halfHoldNs, &fullAgainNs = a.fullAgainNs;
    double &buildAvg = a.buildAvg, &gapAvg = a.gapAvg, &gapFast = a.gapFast, &gapBefore = a.gapBefore,
           &ownRate = a.ownRate, &pausedSum = a.pausedSum;
    uint32_t &behind = a.behind, &pausedCount = a.pausedCount, &sharpRun = a.sharpRun;
    // The rate the game itself asks for: its frame limiter's word, vblanks a
    // frame (0 at 60; 2 on the Game Over screen and wherever it runs at 30).
    // A frame that long isn't one the pictures cost: every such section went
    // to half rate after 6 frames and paused 6 later, to find in the pause
    // that the game was no faster without them, each time anew.
    const uint32_t gameInterval = guest::Load32(kGameInterval);
    if (gameInterval != a.gameInterval) {
      static uint32_t logged = 0;
      if (logged < 20) {
        logged++;
        REXLOG_INFO("video: interp: the game's frame limiter at {} vblanks a frame (was {})", gameInterval,
                    a.gameInterval);
      }
      a.gameInterval = gameInterval;
    }
    // (kept for 8 frames after the word goes back: the frames under way are
    // still that long, and counted as a sharp drop they paused the pictures)
    if (gameInterval >= 2 && gameInterval <= 6) {
      a.gameOwn = double(gameInterval) * 1e9 / 60.0;
      a.ownFrames = 8;
    } else if (a.ownFrames && --a.ownFrames == 0) {
      a.gameOwn = 0;
    }
    const double gameOwn = a.gameOwn;
    int64_t gap = lastNs ? presentNs - lastNs : 0;
    lastNs = presentNs;
    const bool gapValid = gap > 0 && gap < 150'000'000;
    if (gapValid) {
      gapAvg += (double(gap) - gapAvg) * 0.1;
      gapFast += (double(gap) - gapFast) * 0.3;
    }
    if (pausedUntil) {
      if (gapValid) {
        pausedSum += double(gap);
        pausedCount++;
      }
      if (presentNs < pausedUntil) {
        g.previous.clear();
        g.older.clear();
        g.current.clear();
        g.previousPalettes.clear();
        g.palettes.clear();
        g.paletteIndex.clear();
        g.keptFor = 0;
        g.lastFullNs = g.lastPresentNs = g.endNs = g.latencyNs = g.baseNs = g.gridNs = 0;
        g.slowRun = 0;
        g.slow = false;
        g.lastBuildNs = 0;
        return nullptr;
      }
      pausedUntil = 0;
      resumedNs = presentNs;
      behind = 0;
      buildAvg = 0;
      double without = pausedCount ? pausedSum / pausedCount : 0;
      // (a tenth faster without them is not worth the 60 on the screen: a
      // heavy room at 53 alone, 50 with the pictures at half rate)
      if (pausedCount >= 10 && without > gapBefore * 0.9) {
        ownRate = gapBefore;  // (no faster without them: its own rate)
        holdNs = 0;
        REXLOG_INFO("video: interp: on again: the game is at {:.0f} fps without the pictures too", 1e9 / without);
      } else {
        REXLOG_INFO("video: interp: on again ({:.0f} fps without the pictures, {:.0f} with)",
                    without > 0 ? 1e9 / without : 0.0, 1e9 / gapBefore);
      }
    } else {
      buildAvg += (double(g.lastBuildNs) - buildAvg) * 0.1;
      if (ownRate && gapAvg < 17'000'000) {
        ownRate = 0;  // (back at 60)
      }
      // Half rate back to full once the game is at 60 again for the hold
      // (longer each time full rate costs it frames again soon after).
      // (the rate it asks for itself, or was found to have in a pause)
      const double own = std::max(ownRate, gameOwn);
      if (g_interpAutoDivider.load() > 1 && presentNs - halvedNs > halfHoldNs &&
          gapAvg < (own ? own * 1.02 : 17'000'000.0)) {
        g_interpAutoDivider = 1;
        fullAgainNs = presentNs;
        behind = 0;
        REXLOG_INFO("video: interp: full rate again");
      }
      // On the average, not each frame's: at 55 a slow frame of 33 ms comes
      // once in ten among ones of 16.7, which each frame's count never added up.
      // Whatever the build takes: the pictures' cost can be the GPU's (a heavy
      // pass at 4K, 10 ms a frame and four times that with the pictures: the
      // game at 47 for 40 s, never paused while the build took under 3 ms).
      // A game no faster without them is found in the pause (ownRate).
      const double late = own ? own * 1.08 : 17'500'000.0;
      if (gapValid && gapAvg > late) {
        behind++;
      } else if (gapValid && gapAvg < (own ? own * 1.02 : 17'000'000.0)) {
        behind = behind > 2 ? behind - 2 : 0;
      }
      // A sharp drop (under 45 fps, or well under the game's own rate, for 6
      // frames in a row on a quick average): at once, not after the 45 above
      // (a dip to 72 on the screen came and went before they added up).
      const double sharp = own ? own * 1.3 : 1e9 / 45.0;
      sharpRun = gapValid && gapFast > sharp ? sharpRun + 1 : 0;
      // First half rate, then the pause: the step from 180 to 60 on the screen
      // was what showed (a heavy room: the game at 45 with the pictures, 53
      // without). Only where half the refreshes still leave one between the
      // game's frames (above 125 Hz): at 120 Hz half rate is the game's frames
      // alone, shown a frame late, and the pause is that without the wait.
      const int64_t refresh = InterpRefreshNs();
      const bool canHalve = InterpDivider() < 2 && refresh > 0 && refresh < 8'000'000;
      if ((behind >= 45 || sharpRun >= 6) && canHalve) {
        sharpRun = 0;
        behind = 0;
        halfHoldNs = fullAgainNs && presentNs - fullAgainNs < 10'000'000'000
                         ? std::min<int64_t>(halfHoldNs * 2, 60'000'000'000)
                         : 2'000'000'000;
        g_interpAutoDivider = 2;
        halvedNs = presentNs;
        REXLOG_INFO("video: interp: half rate for {:.0f} s at least (building a frame {:.1f} ms, the game at {:.0f} fps)",
                    double(halfHoldNs) / 1e9, buildAvg / 1e6, 1e9 / gapAvg);
      } else if (behind >= 45 || sharpRun >= 6) {
        sharpRun = 0;
        holdNs = resumedNs && holdNs && presentNs - resumedNs < 10'000'000'000
                     ? std::min<int64_t>(holdNs * 2, 24'000'000'000)
                     : 1'500'000'000;
        pausedUntil = presentNs + holdNs;
        gapBefore = gapAvg;
        pausedSum = 0;
        pausedCount = 0;
        g.paused++;
        REXLOG_INFO("video: interp: paused for {:.0f} s (building a frame {:.1f} ms, the game at {:.0f} fps)",
                    double(holdNs) / 1e9, buildAvg / 1e6, 1e9 / gapAvg);
      }
    }
  }
  g.drawsSeen += g.current.size();
  g.drawsWorst = std::max<uint64_t>(g.drawsWorst, g.current.size());
  std::shared_ptr<InterpFrames> frames;
  ArenaMap<uint64_t, ArenaVector<uint32_t>> groups(arena);
  for (uint32_t i = 0; i < g.previous.size(); i++) {
    if (g.previous[i].submitted) {
      groups[g.previous[i].group].push_back(i);
    }
  }
  // The registers several kinds of draws share this frame (the camera, the
  // view, the light): left out of how far apart two draws are, so a turn of
  // the camera doesn't part every draw from its last.
  auto valueKey = [](uint16_t r, const float* y) {
    uint64_t a, b;
    std::memcpy(&a, y, 8);
    std::memcpy(&b, y + 2, 8);
    return (a * 0x9E3779B97F4A7C15ull) ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2)) ^
           (uint64_t(r) << 52);
  };
  // Only a value most draws have (the camera, the view): a light's, shared by
  // the floor's and the walls' passes for it, still tells one light's pass
  // from another's.
  size_t registersNow = 0;  // (the most values the tables below can get)
  for (auto& d : g.current) {
    registersNow += d.registers.size();
  }
  ValueMap<uint32_t> valueDraws(registersNow, arena);  // draws with each value
  ArenaMap<uint64_t, uint32_t> currentCount(arena);  // draws of each kind this frame
  uint32_t submittedNow = 0;
  for (auto& d : g.current) {
    if (!d.submitted) {
      continue;
    }
    submittedNow++;
    currentCount[d.group]++;
    for (size_t r = 0; r < d.registers.size(); r++) {
      valueDraws[valueKey(d.registers[r], &d.constants[r * 4])]++;
    }
  }
  const uint32_t sharedDraws = std::max<uint32_t>(16, submittedNow / 4);
  ArenaVector<ArenaVector<bool>> skips(g.current.size(), arena);
  for (size_t i = 0; i < g.current.size(); i++) {
    auto& d = g.current[i];
    if (!d.submitted) {
      continue;
    }
    auto& skip = skips[i];
    skip.resize(d.registers.size());
    for (size_t r = 0; r < d.registers.size(); r++) {
      const uint32_t* draws = valueDraws.find(valueKey(d.registers[r], &d.constants[r * 4]));
      skip[r] = draws && *draws >= sharedDraws;
    }
  }
  // For matching only, also left out: a value many things have now (8 meshes
  // or more), whatever register it's in: a light (where, reach, colour),
  // which the game sorts again into each thing's list as it moves (outdoors,
  // among torches, a chest's list changed every few frames and it lost its
  // match, drawn off its place: a jerk). A thing's own matrix is its own.
  ArenaVector<ArenaVector<bool>> matchSkips(skips, arena);
  {
    auto vectorKey = [](const float* y) {
      uint64_t a, b;
      std::memcpy(&a, y, 8);
      std::memcpy(&b, y + 2, 8);
      return (a * 0x9E3779B97F4A7C15ull) ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2));
    };
    // (a thing's own values: its few parts' meshes, 3 for a chest; a light's: 40-200)
    constexpr uint32_t kManyMeshes = 8;
    struct Meshes {
      uint64_t mesh[kManyMeshes];
      uint32_t count = 0;
    };
    ValueMap<Meshes> meshesWith(registersNow, arena);  // value: its meshes, up to 8
    for (auto& d : g.current) {
      if (!d.submitted) {
        continue;
      }
      for (size_t r = 0; r < d.registers.size(); r++) {
        auto& m = meshesWith[vectorKey(&d.constants[r * 4])];
        if (m.count < kManyMeshes && std::find(m.mesh, m.mesh + m.count, d.mesh) == m.mesh + m.count) {
          m.mesh[m.count++] = d.mesh;
        }
      }
    }
    for (size_t i = 0; i < g.current.size(); i++) {
      auto& d = g.current[i];
      if (!d.submitted) {
        continue;
      }
      auto& skip = matchSkips[i];
      skip.resize(d.registers.size());
      for (size_t r = 0; r < d.registers.size(); r++) {
        const Meshes* m = meshesWith.find(vectorKey(&d.constants[r * 4]));
        if (m && m->count >= kManyMeshes) {
          skip[r] = true;
        }
      }
    }
  }
  // Between copies of one mesh, which one before a copy now was. A thing of 8
  // meshes or more has its own matrices in all of them (Chapter 7's pods, 24
  // each), so the rule above leaves them out and every pod before was as near
  // as any: the first free one was taken, in draw order, which their glass
  // and lamps change as they are sorted back to front, and part of the way
  // they went toward the next pod's place, ghosts between the pods (#131).
  // What a camera turn can't change tells them apart: a copy that stands
  // still has registers exactly as they were (where it is in its room: c3),
  // and how many of its own (not the lights, c96-c107, nor what most draws
  // share) are the same to the bit ranks the copies before first (as many for
  // each, what all the copies have alike, for a copy that moves); then the
  // score, and how far apart those registers are. Only to rank copies that
  // are near enough: whether one is near enough stays as it was.
  // The camera's change this frame, as a matrix (set in the mesh pass, from
  // the things that are one of a kind and stand still): a thing's matrix
  // (c21-c24: its place times the camera's view and projection) before,
  // times this, is its matrix now if it stood still. Which copy of a mesh a
  // draw was can't be told by the nearest when the camera moves further in a
  // frame than half the way between two copies (a ceiling's racks, each
  // taken for the next as the camera turned or Ryu walked, a slot over in
  // the pictures between, #131): the copy before that this carries onto the
  // draw now is it, however far the camera went.
  struct Mat4 {
    double m[4][4];
  };
  static std::atomic<uint32_t> statStill{0}, statNew{0}, statBack{0};  // (for the log below)
  bool cameraKnown = false;
  Mat4 cameraChange{}, cameraBack{};  // (back: from a matrix now to the one before)
  // Kinds of draws (shaders and target) whose one-of-a-kind things didn't go
  // with it: another view's (a shadow map's light, a reflection), where the
  // camera's change says nothing.
  ArenaMap<uint64_t, uint8_t> cameraKinds(arena);  // 1: one agreed, 2: one didn't
  auto cameraFor = [&](uint64_t group) {
    if (!cameraKnown) {
      return false;
    }
    auto it = cameraKinds.find(group);
    return it == cameraKinds.end() || (it->second & 1);
  };
  auto wvpOf = [](const InterpDraw& d, const std::vector<float>& constants, Mat4& out) {
    uint32_t have = 0;
    for (size_t r = 0; r < d.registers.size(); r++) {
      if (d.registers[r] >= 21 && d.registers[r] <= 24) {
        for (uint32_t c = 0; c < 4; c++) {
          out.m[d.registers[r] - 21][c] = constants[r * 4 + c];
        }
        have |= 1u << (d.registers[r] - 21);
      } else if (d.registers[r] > 24) {
        break;
      }
    }
    return have == 15;
  };
  auto carried = [](const Mat4& before, const Mat4& change) {
    Mat4 out{};
    for (uint32_t i = 0; i < 4; i++) {
      for (uint32_t j = 0; j < 4; j++) {
        for (uint32_t k = 0; k < 4; k++) {
          out.m[i][j] += before.m[i][k] * change.m[k][j];
        }
      }
    }
    return out;
  };
  // (how far a matrix is from another, by its largest value)
  auto matOff = [](const Mat4& a, const Mat4& b) {
    double size = 1.0, off = 0;
    for (uint32_t i = 0; i < 4; i++) {
      for (uint32_t j = 0; j < 4; j++) {
        size = std::max(size, std::fabs(a.m[i][j]));
        double d = std::fabs(a.m[i][j] - b.m[i][j]);
        off = std::max(off, std::isfinite(d) ? d : 1e30);
      }
    }
    return float(std::min(off / size, 1e9));
  };
  auto inverse = [](const Mat4& in, Mat4& out) {
    double a[4][8];
    for (uint32_t i = 0; i < 4; i++) {
      for (uint32_t j = 0; j < 4; j++) {
        a[i][j] = in.m[i][j];
        a[i][j + 4] = i == j ? 1.0 : 0.0;
      }
    }
    for (uint32_t c = 0; c < 4; c++) {
      uint32_t pivot = c;
      for (uint32_t r = c + 1; r < 4; r++) {
        if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) {
          pivot = r;
        }
      }
      if (!(std::fabs(a[pivot][c]) > 1e-9)) {
        return false;
      }
      for (uint32_t j = 0; j < 8; j++) {
        std::swap(a[c][j], a[pivot][j]);
      }
      double scale = 1.0 / a[c][c];
      for (uint32_t j = 0; j < 8; j++) {
        a[c][j] *= scale;
      }
      for (uint32_t r = 0; r < 4; r++) {
        if (r != c) {
          double f = a[r][c];
          for (uint32_t j = 0; j < 8; j++) {
            a[r][j] -= f * a[c][j];
          }
        }
      }
    }
    for (uint32_t i = 0; i < 4; i++) {
      for (uint32_t j = 0; j < 4; j++) {
        out.m[i][j] = a[i][j + 4];
      }
    }
    return true;
  };
  // How far apart two draws' own matrices are (below c64, not what most
  // draws share): which copy of a mesh a draw is, whatever its lights did
  // (-1: it has none).
  auto matrixApart = [&](size_t i, const InterpDraw& b) {
    const auto& d = g.current[i];
    float most = -1.0f;
    for (size_t r = 0; r * 4 < d.constants.size(); r++) {
      if (d.registers[r] >= 64 || (r < skips[i].size() && skips[i][r])) {
        continue;
      }
      const float* x = &d.constants[r * 4];
      const float* y = &b.constants[r * 4];
      float size = 1.0f, change = 0;
      for (uint32_t c = 0; c < 4; c++) {
        size = std::max({size, std::fabs(x[c]), std::fabs(y[c])});
        change = std::max(change, std::fabs(x[c] - y[c]));
      }
      most = std::max(most, std::isfinite(change) ? change / size : 1e9f);
    }
    return most;
  };
  struct CopyRank {
    int32_t anchored = 0;  // registers as its thing's found mesh had them before, less those unlike
    uint32_t same = 0;
    float rest = 1e30f;
    bool operator<(const CopyRank& o) const {
      return anchored != o.anchored ? anchored > o.anchored : same != o.same ? same > o.same : rest < o.rest;
    }
  };
  // By a value now, the value the draw before of the same thing had (below).
  ValueMap<const float*> anchorBefore(registersNow, arena);
  ArenaVector<uint8_t> anchoredDraw(g.current.size(), 0, arena);  // carries such a value
  // Only whether a copy before is another thing's, by the found mesh's
  // values (the anchors): -1 at the first that says so.
  auto anchoredOf = [&](size_t i, const InterpDraw& b) {
    if (!anchoredDraw[i]) {
      return 0;
    }
    const auto& d = g.current[i];
    int32_t anchored = 0;
    for (size_t r = 0; r * 4 < d.constants.size(); r++) {
      if ((r < skips[i].size() && skips[i][r]) || d.registers[r] >= 64) {
        continue;
      }
      const float* const* was = anchorBefore.find(valueKey(d.registers[r], &d.constants[r * 4]));
      if (was && *was) {
        if (std::memcmp(*was, &b.constants[r * 4], 16) != 0) {
          return -1;
        }
        anchored++;
      }
    }
    return anchored;
  };
  auto copyRank = [&](uint32_t i, const InterpDraw& b, float score) {
    const auto& d = g.current[i];
    const auto& skip = skips[i];
    float distance = 0;
    uint32_t same = 0;
    int32_t anchored = 0;
    for (size_t r = 0; r * 4 < d.constants.size(); r++) {
      if ((r < skip.size() && skip[r]) || d.registers[r] >= 64) {
        continue;
      }
      const float* x = &d.constants[r * 4];
      const float* y = &b.constants[r * 4];
      if (anchoredDraw[i]) {
        const float* const* was = anchorBefore.find(valueKey(d.registers[r], x));
        if (was && *was) {
          anchored += std::memcmp(*was, y, 16) == 0 ? 1 : -1;
        }
      }
      if (std::memcmp(x, y, 16) == 0) {
        same++;
        continue;
      }
      float size = 1.0f, change = 0;
      for (uint32_t c = 0; c < 4; c++) {
        size = std::max({size, std::fabs(x[c]), std::fabs(y[c])});
        change = std::max(change, std::fabs(x[c] - y[c]));
      }
      // (summed, each to its whole size at most: by the largest alone, one
      // register that changes much in every copy, a light's pulse, hid which
      // copy's matrices were nearest, and three ceiling parts turned about
      // a column were each taken for the next, #131)
      float v = change / size;
      distance += std::isfinite(v) ? std::min(v, 1.0f) : 1.0f;
    }
    // (by the camera's change where it is known: the rest says less)
    if (cameraFor(d.group)) {
      Mat4 now, was;
      if (wvpOf(d, d.constants, now) && wvpOf(d, b.constants, was)) {
        return CopyRank{anchored, 0, matOff(now, carried(was, cameraChange))};
      }
    }
    return CopyRank{anchored, same, score + 0.002f * std::min(distance, 16.0f)};
  };
  ArenaVector<int32_t> match(g.current.size(), -1, arena);
  // A copy of a mesh that wasn't drawn the frame before, among copies that
  // were (a pod come into view as the camera turned, #131): the register
  // that says so (below), or -1. It takes no other copy's draw before.
  ArenaVector<int32_t> strangerReg(g.current.size(), -1, arena);
  ArenaVector<uint8_t> meshBefore(g.current.size(), 0, arena);  // its mesh was drawn the frame before
  ArenaVector<uint8_t> oneOfMany(g.current.size(), 0, arena);   // matched among copies of its mesh
  ArenaVector<float> matchDistance(g.current.size(), 0.0f, arena);
  ArenaVector<uint8_t> matchHow(g.current.size(), 0, arena);  // 1 pairs, 2 each in turn, 3 by kind, 4 stood still
  ArenaVector<bool> used(g.previous.size(), false, arena);
  ArenaMap<uint64_t, uint32_t> occurrence(arena);
  uint32_t submitted = 0, matched = 0;
  // Close enough to take without looking further: the same place among its
  // kind, moved as things move in a frame.
  constexpr float kClose = 0.1f;
  // First by mesh: the same vertex and index data with the same shaders is the
  // same thing, however far the camera turned. One of it before and now: taken
  // unless it moved more than its size (another thing in a reused buffer);
  // several (copies of one mesh): the nearest, as close as alike draws must be.
  std::function<void(bool)> rejectFar;  // (set in the mesh pass, used again after the pass by kind)
  {
    auto meshKey = [](const InterpDraw& d) { return d.mesh * 0xC2B2AE3D27D4EB4Full ^ d.group; };
    int64_t meshStartNs = SteadyNs();
    g.prepNs += meshStartNs - startNs;
    struct MeshTimed {
      int64_t start;
      ~MeshTimed() { g_interp.meshNs += SteadyNs() - start; }
    } meshTimed{meshStartNs};
    ArenaMap<uint64_t, ArenaVector<uint32_t>> meshes(arena), meshesNow(arena);
    for (uint32_t j = 0; j < g.previous.size(); j++) {
      if (g.previous[j].submitted && g.previous[j].mesh) {
        meshes[meshKey(g.previous[j])].push_back(j);
      }
    }
    for (uint32_t i = 0; i < g.current.size(); i++) {
      if (g.current[i].submitted && g.current[i].mesh) {
        meshesNow[meshKey(g.current[i])].push_back(i);
      }
    }
    // The camera's change: from one thing of a kind (its mesh drawn once
    // before and once now), the one that most others agree with.
    {
      struct One {
        Mat4 was, now;
        uint64_t group;
      };
      ArenaVector<One> ones(arena);
      for (auto& [key, now] : meshesNow) {
        auto it = meshes.find(key);
        if (now.size() != 1 || it == meshes.end() || it->second.size() != 1 || ones.size() >= 96) {
          continue;
        }
        const auto& d = g.current[now[0]];
        const auto& b = g.previous[it->second[0]];
        One one;
        one.group = d.group;
        if (!d.boneSize && !d.ui && b.constants.size() == d.constants.size() && wvpOf(d, d.constants, one.now) &&
            wvpOf(d, b.constants, one.was)) {
          ones.push_back(one);
        }
      }
      uint32_t most = 0;
      float moved = 0;
      const size_t tries = std::min<size_t>(ones.size(), 12);
      for (size_t t = 0; t < tries && ones.size() >= 8; t++) {
        const One& ref = ones[t * ones.size() / tries];
        Mat4 inv;
        if (!inverse(ref.was, inv)) {
          continue;
        }
        Mat4 change = carried(inv, ref.now);
        uint32_t agree = 0;
        for (const One& o : ones) {
          agree += matOff(o.now, carried(o.was, change)) < 0.01f;
        }
        if (agree > most) {
          most = agree;
          cameraChange = change;
          moved = matOff(ref.now, ref.was);
        }
      }
      // (not across a cut to another camera in the same room: the room's
      // things agree with any change, and would be carried across it)
      cameraKnown = most >= 6 && most * 3 >= ones.size() && moved <= 1.0f && inverse(cameraChange, cameraBack);
      if (cameraKnown) {
        for (const One& o : ones) {
          cameraKinds[o.group] |= matOff(o.now, carried(o.was, cameraChange)) < 0.01f ? 1 : 2;
        }
      }
      // (how often the camera's change is known, for the log)
      static std::atomic<uint32_t> frames{0}, known{0}, agreed{0}, of{0};
      known += cameraKnown;
      agreed += most;
      of += uint32_t(ones.size());
      if (++frames % 600 == 0) {
        REXLOG_INFO("video: interp: camera change known in {} of 600 frames; {} of {} one-of-a-kind things agree; "
                    "{} draws stood still, {} new among copies, {} given the matrix they had before",
                    known.exchange(0), agreed.exchange(0), of.exchange(0), statStill.exchange(0), statNew.exchange(0),
                    statBack.exchange(0));
      }
    }
    // First, for sure: a draw whose matrix now is a copy before's carried by
    // the camera's change stood still, and is that copy (whatever its lights
    // did, however many copies, in whatever order they were drawn). Then, in
    // a mesh some of whose copies stood still, a copy with no copy before
    // near where it would have been is new in view: it takes none (and is
    // given the matrix it would have had before, below: it comes into view
    // with the rest, not a picture late).
    if (cameraKnown) {
      ArenaVector<Mat4> was(arena);
      for (auto& [key, now] : meshesNow) {
        auto it = meshes.find(key);
        if (it == meshes.end()) {
          continue;
        }
        auto& before = it->second;
        const auto& first = g.current[now[0]];
        if (first.boneSize || first.ui || now.size() * before.size() > 8192 || !cameraFor(first.group)) {
          continue;
        }
        was.clear();
        bool rows = true;
        for (uint32_t j : before) {
          Mat4 m;
          rows = rows && g.previous[j].constants.size() == first.constants.size() &&
                 wvpOf(first, g.previous[j].constants, m);
          if (!rows) {
            break;
          }
          was.push_back(carried(m, cameraChange));
        }
        if (!rows) {
          continue;
        }
        uint32_t still = 0;
        for (uint32_t i : now) {
          Mat4 m;
          if (g.current[i].constants.size() != first.constants.size() || !wvpOf(g.current[i], g.current[i].constants, m)) {
            continue;
          }
          int32_t best = -1;
          float off = 0.01f, second = 1e30f;
          for (size_t k = 0; k < before.size(); k++) {
            if (used[before[k]]) {
              continue;
            }
            float o = matOff(m, was[k]);
            if (o < off) {
              second = off;
              off = o;
              best = int32_t(k);
            } else {
              second = std::min(second, o);
            }
          }
          // (two as near: passes of one thing with one matrix, told apart
          // by their other registers, below)
          if (best >= 0 && second >= off + 1e-4f) {
            used[before[best]] = true;
            match[i] = int32_t(before[best]);
            matchHow[i] = 4;
            matched++;
            g.byMesh++;
            still++;
            statStill++;
          }
        }
        if (!still || now.size() < 2) {
          continue;
        }
        // (near: within half the least way between two copies, so a copy
        // that moves a little keeps its own, and one new in view beside a
        // copy gone from it doesn't take that one's)
        float nearBy = 0.05f;
        if (before.size() <= 64) {
          float spacing = 0.6f;
          for (size_t a = 0; a < was.size(); a++) {
            for (size_t b = a + 1; b < was.size(); b++) {
              float o = matOff(was[a], was[b]);
              if (o > 1e-4f) {
                spacing = std::min(spacing, o);
              }
            }
          }
          nearBy = 0.5f * spacing;
        }
        for (uint32_t i : now) {
          Mat4 m;
          if (match[i] >= 0 || !wvpOf(g.current[i], g.current[i].constants, m)) {
            continue;
          }
          float nearest = 1e30f;
          for (size_t k = 0; k < before.size(); k++) {
            if (!used[before[k]]) {
              nearest = std::min(nearest, matOff(m, was[k]));
            }
          }
          if (nearest >= nearBy) {
            for (size_t r = 0; r < g.current[i].registers.size(); r++) {
              if (g.current[i].registers[r] == 24) {
                strangerReg[i] = int32_t(r);
                statNew++;
              }
            }
          }
        }
      }
    }
    // Copies of one skinned mesh (bodies of one kind of enemy) can have the
    // same constants and differ only in their bones: how far the bones of
    // one before are from these now (the median of some rows' moves).
    ArenaMap<uint64_t, int32_t> previousPalette(arena);
    for (int32_t p = 0; p < int32_t(g.previousPalettes.size()); p++) {
      previousPalette.emplace((uint64_t(g.previousPalettes[p].address) << 32) | g.previousPalettes[p].size, p);
    }
    auto bonesApart = [&](const InterpDraw& d, const InterpDraw& b) {
      auto it = previousPalette.find((uint64_t(b.boneAddress) << 32) | b.boneSize);
      if (!d.boneSize || it == previousPalette.end() || b.boneSize != d.boneSize) {
        return 0.0f;
      }
      auto at = g.paletteIndex.find((uint64_t(d.boneAddress) << 32) | d.boneSize);
      if (at == g.paletteIndex.end()) {
        return 0.0f;
      }
      const auto& then = g.previousPalettes[it->second].words;
      const auto* now = g.palettes[at->second].words.data();  // (copied at the present)
      size_t rows = std::min(then.size(), g.palettes[at->second].words.size()) / 4;
      double sum = 0;
      size_t n = 0, step = std::max<size_t>(1, rows / 48);
      for (size_t r = 0; r < rows && n < 48; r += step, n++) {
        float move = 0;
        for (size_t c = 0; c < 4; c++) {
          float fx = std::bit_cast<float>(rex::byte_swap(then[r * 4 + c]));
          float fy = std::bit_cast<float>(rex::byte_swap(now[r * 4 + c]));
          move = std::max(move, std::isfinite(fx - fy) ? std::fabs(fx - fy) : 1e9f);
        }
        sum += move;
      }
      return n ? float(sum / double(n)) : 0.0f;
    };
    // A mesh written anew each frame in two buffers by turns: its copies
    // before, drawn from the other buffer, found by their index data
    // (only ones whose own mesh isn't drawn now: another mesh's copies
    // that share the index buffer keep theirs).
    auto altKey = [](const InterpDraw& d) { return d.meshAlt * 0xC2B2AE3D27D4EB4Full ^ d.group; };
    ArenaMap<uint64_t, ArenaVector<uint32_t>> byAlt(arena);
    for (auto& [key, before] : meshes) {
      const auto& b = g.previous[before[0]];
      if (b.meshAlt && !meshesNow.count(key)) {
        auto& list = byAlt[altKey(b)];
        list.insert(list.end(), before.begin(), before.end());
      }
    }
    // Before any is matched: which copies are which by a register of their
    // own, and through them the things they are part of.
    // Copies that stand still each have a register of their own, the same
    // to the bit as the frame before (where a pod is in its room, c3): one
    // whose value there no copy before had is new, come into view, not one
    // of them moved. Left to the nearest still free, it took the draw of a
    // pod gone from view at the other end of the row (their distance leaves
    // out what a thing of 8 meshes shares, so every pod is as near), and
    // part of the way its glass and lamps stood between two pods, a flicker
    // while the camera turned (#131). Such a register: each copy now has
    // its own value in it, and at least half of them (two or more) have
    // theirs from a copy before. Not bodies: their place in the bone buffer
    // says which.
    // The other meshes of such a thing (a pod's body, 20 meshes by another
    // shader) have no such register, only matrices with the camera's in them,
    // which they share with the mesh that has: the value now tells which
    // thing a draw is part of, and the value its found copy had before, which
    // draw before is its own (anchorBefore; null: of a thing new in view, no
    // draw before is). Without it the bodies went by draw order, each to the
    // next pod's before while the camera turned and pods came into view, and
    // part of the way they stood a third of the way to the next pod, their
    // glass, rightly placed, a ghost beside them.
    // (only registers where every copy of the mesh has a value of its own: a
    // fade one copy has and the rest share says nothing of which thing)
    struct NewCopy {
      uint32_t draw;
      int32_t reg;
    };
    ArenaVector<NewCopy> newCopies(arena);
    ArenaVector<uint64_t> newValues(arena);
    auto apart = [](const float* x, const float* y) {
      float d = 0;
      for (uint32_t c = 0; c < 4; c++) {
        float v = std::fabs(x[c] - y[c]);
        d = std::max(d, std::isfinite(v) ? v : 1e30f);
      }
      return d;
    };
    for (auto& [key, now] : meshesNow) {
      auto it = meshes.find(key);
      if (it == meshes.end()) {
        continue;
      }
      auto& before = it->second;
      if (now.size() < 2 || now.size() > 64 || before.size() > 64 || g.current[now[0]].boneSize) {
        continue;
      }
      const auto& first = g.current[now[0]];
      const size_t registers = first.constants.size() / 4;
      ArenaVector<bool> distinct(registers, false, arena);
      bool any = false;
      for (size_t r = 0; r < registers; r++) {
        if (first.registers[r] >= 64) {
          continue;
        }
        bool own = true;
        for (size_t a = 0; a < now.size() && own; a++) {
          const auto& x = g.current[now[a]].constants;
          own = x.size() == first.constants.size();
          for (size_t b = 0; b < a && own; b++) {
            own = std::memcmp(&x[r * 4], &g.current[now[b]].constants[r * 4], 16) != 0;
          }
        }
        distinct[r] = own;
        any |= own;
      }
      if (!any) {
        continue;
      }
      ArenaVector<bool> has(now.size(), false, arena);
      ArenaVector<int32_t> partner(now.size(), -1, arena);   // the copy before, by any such register
      ArenaVector<uint32_t> tested(now.size(), 0, arena), unlike(now.size(), 0, arena);
      ArenaVector<int32_t> unlikeReg(now.size(), -1, arena);
      // (each register's values before and now in tables: a scan of the
      // copies before for each copy now, and of the copies now for each of
      // those, was a register's copies cubed, milliseconds a frame where
      // an explosion drew dozens of one mesh, #134)
      ArenaMap<uint64_t, uint32_t> beforeAt(arena), nowAt(arena);  // value -> its first draw
      auto sameAt = [&](const ArenaMap<uint64_t, uint32_t>& table, const float* x, size_t r,
                        bool previous) -> int32_t {
        auto found = table.find(valueKey(0, x));
        if (found == table.end()) {
          return -1;
        }
        const auto& y = (previous ? g.previous[found->second] : g.current[found->second]).constants;
        return std::memcmp(x, &y[r * 4], 16) == 0 ? int32_t(found->second) : -1;
      };
      for (size_t r = 0; r < registers; r++) {
        if (!distinct[r]) {
          continue;
        }
        beforeAt.clear();
        nowAt.clear();
        for (uint32_t j : before) {
          const auto& y = g.previous[j].constants;
          if (y.size() == first.constants.size()) {
            beforeAt.emplace(valueKey(0, &y[r * 4]), j);
          }
        }
        for (uint32_t i : now) {
          nowAt.emplace(valueKey(0, &g.current[i].constants[r * 4]), i);
        }
        uint32_t kept = 0;
        has.assign(now.size(), false);
        for (size_t a = 0; a < now.size(); a++) {
          if (sameAt(beforeAt, &g.current[now[a]].constants[r * 4], r, true) >= 0) {
            has[a] = true;
            kept++;
          }
        }
        if (kept < 2 || kept * 2 < now.size()) {
          continue;
        }
        for (size_t a = 0; a < now.size(); a++) {
          const float* x = &g.current[now[a]].constants[r * 4];
          if (has[a]) {
            if (partner[a] < 0) {
              partner[a] = sameAt(beforeAt, x, r, true);
            }
            continue;
          }
          // (one that moved keeps its own: a draw before that no copy now
          // has the value of, nearer in this register than half the way to
          // the next copy; a pod gone from view is a pod's width away or more)
          float spacing = 1e30f, nearest = 1e30f;
          for (size_t b = 0; b < now.size(); b++) {
            if (b != a) {
              spacing = std::min(spacing, apart(x, &g.current[now[b]].constants[r * 4]));
            }
          }
          for (uint32_t j : before) {
            const auto& y = g.previous[j].constants;
            if (y.size() != first.constants.size()) {
              continue;
            }
            if (sameAt(nowAt, &y[r * 4], r, false) < 0) {  // (not another copy's now)
              nearest = std::min(nearest, apart(x, &y[r * 4]));
            }
          }
          tested[a]++;
          if (!(nearest < 0.5f * spacing)) {
            unlike[a]++;
            if (unlikeReg[a] < 0) {
              unlikeReg[a] = int32_t(r);
            }
          }
        }
      }
      // What a found copy has of its own (its matrices with the camera's in
      // them: c24, c11, c16), by the value now, with the value its draw
      // before had: the thing's other meshes carry them too. A copy new by
      // every such register (and found by none): its values noted once all
      // the found ones are, so a thing one of whose meshes was drawn before
      // isn't taken for new by another that wasn't.
      for (size_t a = 0; a < now.size(); a++) {
        const uint32_t i = now[a];
        const auto& d = g.current[i];
        const auto& skip = skips[i];
        if (match[i] >= 0) {
          partner[a] = match[i];  // (it stood still: that one, for sure)
        }
        const bool isNew = partner[a] < 0 && tested[a] > 0 && unlike[a] == tested[a] && now.size() >= 3;
        if (partner[a] < 0 && !isNew) {
          continue;
        }
        if (isNew) {
          newCopies.push_back({i, unlikeReg[a]});
        }
        for (size_t r = 0; r < registers; r++) {
          if (!distinct[r] || (r < skip.size() && skip[r])) {
            continue;
          }
          uint64_t valueAt = valueKey(d.registers[r], &d.constants[r * 4]);
          if (isNew) {
            newValues.push_back(valueAt);
          } else {
            anchorBefore.emplace(valueAt, &g.previous[partner[a]].constants[r * 4]);
          }
        }
      }
    }
    for (const NewCopy& n : newCopies) {
      strangerReg[n.draw] = n.reg;
    }
    for (uint64_t valueAt : newValues) {
      anchorBefore.emplace(valueAt, nullptr);  // (a found copy's stays)
    }
    // The other meshes of a thing new in view: new too. And which draws carry
    // a found thing's value (only those are looked up when ranked).
    if (!anchorBefore.empty()) {
      for (size_t i = 0; i < g.current.size(); i++) {
        auto& d = g.current[i];
        if (!d.submitted || !d.mesh || d.boneSize) {
          continue;
        }
        for (size_t r = 0; r * 4 < d.constants.size(); r++) {
          if ((r < skips[i].size() && skips[i][r]) || d.registers[r] >= 64) {
            continue;
          }
          const float* const* x = anchorBefore.find(valueKey(d.registers[r], &d.constants[r * 4]));
          if (!x) {
            continue;
          }
          if (*x) {
            anchoredDraw[i] = 1;
          } else if (strangerReg[i] < 0) {
            strangerReg[i] = int32_t(r);
          }
        }
      }
    }
    for (auto& [key, now] : meshesNow) {
      if (meshes.count(key)) {
        for (uint32_t i : now) {
          meshBefore[i] = 1;
        }
      }
      const ArenaVector<uint32_t>* found = nullptr;
      if (auto it = meshes.find(key); it != meshes.end()) {
        found = &it->second;
      } else if (const auto& d = g.current[now[0]]; d.meshAlt && !d.boneSize && now.size() >= 2) {
        // (as many copies as before, a few: not sprites from a ring of
        // vertex data over one quad's indices, unrelated by their count)
        if (auto alt = byAlt.find(altKey(d)); alt != byAlt.end() && alt->second.size() == now.size() &&
                                              now.size() <= 16) {
          found = &alt->second;
        }
      } else if (d.meshAlt && d.boneSize && now.size() <= 16) {
        // (as many before as now or not: two like bodies' draws before share
        // the key; each takes the one at its own place)
        // A body's meshes whose vertex data the game moved (Sonia in Chapter
        // 7's cockpit, every few frames, #131): by their index data, each with
        // a draw before at its own place in the bone buffer, its palette's
        // size the same. Left without, they were drawn with the pose now and
        // the camera before while the rest of her went part of the way: two
        // of her, a ghost.
        if (auto alt = byAlt.find(altKey(d)); alt != byAlt.end() && alt->second.size() <= 16) {
          bool placed = true;
          for (uint32_t i : now) {
            const auto& c = g.current[i];
            bool own = false;
            for (uint32_t j : alt->second) {
              const auto& b = g.previous[j];
              own |= b.boneSize == c.boneSize && (b.boneAddress & 0xFFFF) == (c.boneAddress & 0xFFFF);
            }
            placed &= own;
          }
          if (placed) {
            found = &alt->second;
          }
        }
      }
      if (!found) {
        continue;
      }
      auto& before = *found;
      bool single = now.size() == 1 && before.size() == 1;
      float bound = single ? 1.5f : (before.size() > 8 ? 0.1f : 0.3f);
      // A few alike copies without bones (a door's pieces flying from one
      // place, the same matrix's place, turned differently): paired nearest
      // first over all of them, not each in turn with the nearest still free
      // (the first piece took another's of the frame before, and the one it
      // was went without: part of the way each piece turned into another's
      // pose, a shake, the pieces thrown about).
      if (!single && now.size() <= 16 && before.size() <= 16 && !g.current[now[0]].boneSize) {
        // Each copy's own few lights (c96-c107, the nearest of the room's,
        // picked again as Ryu and the camera move: Chapter 6's banners,
        // #121; places c96-c99, colours c100-c103, reach c104-c107, all
        // changed with them) say nothing of where it is: left out of the score. With
        // them a copy was farther from itself before than from its
        // neighbour, or from all, and drawn as now between, ahead of the
        // camera, a flicker as it moved.
        auto copyDistance = [&](uint32_t i, uint32_t j) {
          const auto& a = g.current[i].constants;
          const auto& b = g.previous[j].constants;
          const auto& regs = g.current[i].registers;
          const auto& skip = matchSkips[i];
          float distance = 0;
          for (size_t r = 0; r * 4 < a.size() && distance < 1.0f; r++) {
            if ((r < skip.size() && skip[r]) || (regs[r] >= 96 && regs[r] < 108)) {
              continue;
            }
            const float* x = &a[r * 4];
            const float* y = &b[r * 4];
            float size = 1.0f, change = 0;
            for (uint32_t c = 0; c < 4; c++) {
              size = std::max({size, std::fabs(x[c]), std::fabs(y[c])});
              change = std::max(change, std::fabs(x[c] - y[c]));
            }
            float d = change / size;
            distance = std::max(distance, std::isfinite(d) ? d : 1e9f);
          }
          return distance;
        };
        struct Pair {
          float score;
          CopyRank rank;  // (alike copies told apart)
          uint32_t i, j;
        };
        ArenaVector<Pair> pairs(arena);
        ArenaMap<uint32_t, std::pair<float, float>> bestOf(arena);  // i: best, second score
        for (uint32_t i : now) {
          auto& d = g.current[i];
          auto& bs = bestOf[i];
          bs = {1e30f, 1e30f};
          if (strangerReg[i] >= 0 || match[i] >= 0) {
            continue;
          }
          for (uint32_t j : before) {
            if (used[j] || g.previous[j].constants.size() != d.constants.size()) {
              continue;
            }
            float distance = copyDistance(i, j);
            if (!(distance < 1.0f)) {
              continue;
            }
            CopyRank rank = copyRank(i, g.previous[j], distance);
            if (rank.anchored < 0) {
              continue;  // (another thing's draw before)
            }
            pairs.push_back({distance, rank, i, j});
            if (distance < bs.first) {
              bs.second = bs.first;
              bs.first = distance;
            } else if (distance < bs.second) {
              bs.second = distance;
            }
          }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
          return a.rank < b.rank || (!(b.rank < a.rank) && (a.i < b.i || (a.i == b.i && a.j < b.j)));
        });
        for (const Pair& p : pairs) {
          if (match[p.i] >= 0 || used[p.j]) {
            continue;
          }
          auto& bs = bestOf[p.i];
          bool clear = p.score == bs.first && p.score <= 0.6f && bs.second >= 2.5f * p.score;
          if (p.score <= bound || clear) {
            used[p.j] = true;
            match[p.i] = int32_t(p.j);
            matchDistance[p.i] = p.score;
            matchHow[p.i] = 1;
            oneOfMany[p.i] = 1;
            matched++;
            g.byMesh++;
          }
        }
        continue;
      }
      for (size_t position = 0; position < now.size(); position++) {
        const uint32_t i = now[position];
        auto& d = g.current[i];
        if (strangerReg[i] >= 0 || match[i] >= 0) {
          continue;
        }
        // Hundreds of copies without bones (sparks, debris of an explosion):
        // only those before near its own place in the draw order, which such
        // copies keep from frame to frame. Each against every one before was
        // the most of a frame's matching where rockets burst (#134).
        constexpr size_t kWindow = 32;
        size_t from = 0, to = before.size();
        const bool windowed = !d.boneSize && before.size() > 2 * kWindow;
        if (windowed) {
          size_t at = position * before.size() / now.size();
          from = at > kWindow ? at - kWindow : 0;
          to = std::min(before.size(), at + kWindow + 1);
        }
        int32_t best = -1;
        float bestDistance = 1e30f, bestScore = 1e30f, secondScore = 1e30f;
        // Among those near enough (the bound), the best by copyRank: taken
        // over the best by score, which stays for the rest (#131).
        int32_t ranked = -1;
        float rankedDistance = 0;
        CopyRank rankedRank;
        // (a few copies: among hundreds, sparks of one mesh, it is the nearest as before)
        const bool rankCopies = !d.boneSize && now.size() <= 64 && before.size() <= 64;
        // A copy before at the same place in the bone buffer is this one
        // (another place is another body of its kind): only it, if there's one.
        bool havePlace = false;
        if (d.boneSize) {
          for (uint32_t j : before) {
            havePlace |= (d.boneAddress & 0xFFFF) == (g.previous[j].boneAddress & 0xFFFF);
          }
        }
        for (size_t at = from; at < to; at++) {
          const uint32_t j = before[at];
          if (used[j] || g.previous[j].constants.size() != d.constants.size()) {
            continue;
          }
          if (d.boneSize && (d.boneAddress & 0xFFFF) != (g.previous[j].boneAddress & 0xFFFF)) {
            if (havePlace) {
              continue;
            }
            // (another place: only one no copy now is at, or it's that one's)
            bool taken = false;
            for (uint32_t k : now) {
              taken |= (g.current[k].boneAddress & 0xFFFF) == (g.previous[j].boneAddress & 0xFFFF);
            }
            if (taken) {
              continue;
            }
          }
          if (rankCopies && anchoredDraw[i] && anchoredOf(i, g.previous[j]) < 0) {
            continue;  // (another thing's draw before)
          }
          float distance =
              InterpDistance(d, g.previous[j], rankCopies ? std::max(secondScore, bound) : secondScore, &matchSkips[i]);
          if (!(distance < secondScore) && !(rankCopies && distance <= bound)) {
            continue;  // (can't be the best, the next or near enough)
          }
          float apart = bonesApart(d, g.previous[j]);
          if (apart > kPaletteJump) {
            continue;  // (another's bones)
          }
          // (the same place in the bone buffer, the next of its copies: most likely it)
          bool samePlace = (d.boneAddress & 0xFFFF) == (g.previous[j].boneAddress & 0xFFFF);
          float score = distance + apart + (d.boneSize && !samePlace ? 0.25f : 0.0f);
          // (its bones and their place counted: one near enough with them)
          if (rankCopies && score <= bound) {
            CopyRank rank = copyRank(i, g.previous[j], score);
            if (rank < rankedRank) {
              ranked = int32_t(j);
              rankedDistance = distance;
              rankedRank = rank;
            }
          }
          if (!(distance < secondScore)) {
            continue;
          }
          if (score < bestScore) {
            secondScore = bestScore;
            best = int32_t(j);
            bestDistance = distance;
            bestScore = score;
          } else if (score < secondScore) {
            secondScore = score;
          }
        }
        // Past the bound but far nearer than any other copy: still it (a
        // camera turn moves the matrices of nine alike things 0.11 a frame,
        // past their 0.1; a light of its own sorted again).
        // (not with a window: a copy outside it could be as near)
        bool clear = !d.boneSize && !windowed && bestDistance <= 0.6f && secondScore >= 2.5f * bestScore;
        if (ranked >= 0) {
          best = ranked;
          bestDistance = rankedDistance;
        }
        // None near enough: a light's flash on it (an enemy's shot: values
        // of its own above c64, past the bound in a frame) is no move. The
        // copy before whose matrices are nearest, if they hardly differ:
        // left without, a rack of 18 behind the camera was taken from two
        // frames ago, any copy's, and between two copies' matrices it stood
        // before the camera, on a wall, for a picture (#131).
        // (a few copies, as for copyRank: among hundreds, sparks of one mesh,
        // each against every one before was milliseconds a frame, #134)
        if (rankCopies && !single && !(best >= 0 && (bestDistance <= bound || clear))) {
          int32_t nearest = -1;
          float apart = 0.3f;
          Mat4 wvpNow;
          const bool byCamera = cameraFor(d.group) && wvpOf(d, d.constants, wvpNow);
          for (uint32_t j : before) {
            if (used[j] || g.previous[j].constants.size() != d.constants.size() ||
                (anchoredDraw[i] && anchoredOf(i, g.previous[j]) < 0)) {
              continue;
            }
            // (by the camera's change where it is known: the nearest as the
            // matrices stand is the next copy's while the camera turns)
            float a = matrixApart(i, g.previous[j]);
            if (!(a >= 0 && a < 0.3f)) {
              continue;
            }
            if (byCamera) {
              Mat4 was;
              if (wvpOf(d, g.previous[j].constants, was)) {
                a = std::min(matOff(wvpNow, carried(was, cameraChange)), 0.299f);
              }
            }
            if (a < apart) {
              apart = a;
              nearest = int32_t(j);
            }
          }
          if (nearest >= 0) {
            best = nearest;
            bestDistance = bound;  // (taken: the check after the pass sees to the rest)
          }
        }
        if (best >= 0 && (bestDistance <= bound || clear)) {
          used[best] = true;
          match[i] = best;
          matchDistance[i] = bestDistance;
          matchHow[i] = 2;
          oneOfMany[i] = !single;
          matched++;
          g.byMesh++;
        }
      }
    }
    // A copy matched among copies went by a distance that can't see a big
    // thing's own matrix (8 meshes share it: left out), so one new in view
    // took whichever copy before was free: a ceiling's ducts, come into view
    // as the camera tilted up, each drawn between its place and another's
    // in the pictures between, there and gone from picture to picture
    // (#131). Its matrices (below c64: not its lights), all of them, must not
    // have changed much more than those of the things that are one of a kind
    // did, which is the camera's move this frame: or it is new, drawn as it
    // is now.
    {
      auto ownChange = [&](size_t i, int32_t* worst) {
        const auto& d = g.current[i];
        const auto& b = g.previous[match[i]];
        float most = 0;
        for (size_t r = 0; r * 4 < d.constants.size(); r++) {
          if (d.registers[r] >= 64 || (r < skips[i].size() && skips[i][r])) {
            continue;
          }
          const float* x = &d.constants[r * 4];
          const float* y = &b.constants[r * 4];
          float size = 1.0f, change = 0;
          for (uint32_t c = 0; c < 4; c++) {
            size = std::max({size, std::fabs(x[c]), std::fabs(y[c])});
            change = std::max(change, std::fabs(x[c] - y[c]));
          }
          float v = std::isfinite(change) ? change / size : 1e9f;
          if (v > most) {
            most = v;
            if (worst) {
              *worst = int32_t(r);
            }
          }
        }
        return most;
      };
      rejectFar = [&, ownChange](bool byKind) {
        ArenaVector<float> alone(arena);
        for (size_t i = 0; i < g.current.size(); i++) {
          if (match[i] >= 0 && !oneOfMany[i] && matchHow[i] != 3 && !g.current[i].boneSize) {
            alone.push_back(ownChange(i, nullptr));
          }
        }
        if (alone.size() < 8) {
          return;
        }
        std::nth_element(alone.begin(), alone.begin() + alone.size() / 2, alone.end());
        const float most = std::max(0.3f, 2.5f * alone[alone.size() / 2] + 0.1f);
        for (size_t i = 0; i < g.current.size(); i++) {
          // (by its kind: a mesh taken for another draw's, as blindly)
          const bool check = byKind ? matchHow[i] == 3 && g.current[i].mesh != 0 : oneOfMany[i] != 0;
          if (match[i] < 0 || !check || g.current[i].boneSize) {
            continue;
          }
          int32_t worst = 0;
          if (ownChange(i, &worst) > most) {
            used[match[i]] = false;
            match[i] = -1;
            matchDistance[i] = 0;
            matchHow[i] = 0;
            strangerReg[i] = worst;  // (nor by its kind, below)
            matched--;
            if (!byKind) {
              g.byMesh--;
            }
          }
        }
      };
      rejectFar(false);
    }
  }
  for (size_t i = 0; i < g.current.size(); i++) {
    auto& d = g.current[i];
    if (!d.submitted) {
      continue;
    }
    submitted++;
    uint32_t n = occurrence[d.group]++;
    if (match[i] >= 0 || strangerReg[i] >= 0) {
      continue;  // (by its mesh; or new among its copies)
    }
    auto it = groups.find(d.group);
    if (it == groups.end()) {
      continue;
    }
    auto& candidates = it->second;
    int32_t best = -1;
    float bestDistance = 1e30f;
    auto consider = [&](uint32_t j) {
      if (used[j] || g.previous[j].constants.size() != d.constants.size()) {
        return;
      }
      if (d.boneSize && g.previous[j].boneSize &&
          (d.mesh != g.previous[j].mesh ||
           (d.boneAddress & 0xFFFF) != (g.previous[j].boneAddress & 0xFFFF))) {
        return;  // (skinned: another body; its own was looked for by mesh)
      }
      if (anchoredDraw[i] && anchoredOf(i, g.previous[j]) < 0) {
        return;  // (another thing's draw before, by its found mesh)
      }
      float distance = InterpDistance(d, g.previous[j], bestDistance, &matchSkips[i]);
      if (distance < bestDistance) {
        best = int32_t(j);
        bestDistance = distance;
      }
    };
    // The same place among its kind first (the order mostly holds), then the others near it.
    if (n < candidates.size()) {
      consider(candidates[n]);
    }
    for (uint32_t step = 1; bestDistance > kClose && step <= 16; step++) {
      if (n + step < candidates.size()) {
        consider(candidates[n + step]);
      }
      if (step <= n && n - step < candidates.size()) {
        consider(candidates[n - step]);
      }
    }
    // Too far apart to be the same thing a frame on (another enemy of the
    // same kind): drawn as it is.
    bool crowded = candidates.size() > 8;
    if (best >= 0 && bestDistance <= (crowded ? 0.05f : 0.3f)) {
      used[best] = true;
      match[i] = best;
      matchDistance[i] = bestDistance;
      matchHow[i] = 3;
      matched++;
    }
  }
  if (rejectFar) {
    rejectFar(true);
  }
  // (whether a matched draw changed at all: with the shared registers)
  for (size_t i = 0; i < g.current.size(); i++) {
    if (match[i] >= 0) {
      matchDistance[i] = InterpDistance(g.current[i], g.previous[match[i]]);
    }
  }
  g.frames++;
  g.draws += submitted;
  g.matched += matched;
  // The HUD and menus as they are now: their pieces (icons, a list that
  // scrolls) are many alike, and mixed with another's they'd leave a trail.
  for (size_t i = 0; i < g.current.size(); i++) {
    if (g.current[i].ui) {
      match[i] = -1;
    }
  }
  int64_t matchedNs = SteadyNs();
  g.matchNs += matchedNs - startNs;
  // A partial frame (a few passes over the last picture, the scene not drawn
  // again: some places do it every third frame): not drawn again between,
  // as its passes add to what is there, and the next frame goes on from the
  // last full one (kept, for two such at most).
  uint32_t submittedBefore = 0;
  for (auto& d : g.previous) {
    submittedBefore += d.submitted;
  }
  // Or fewer draws than before, all the same as they were (Game Over draws
  // its scene 15 times a second and between only its passes again over it):
  // the next frame, which draws the scene, matched to this one would be most
  // draws new, a cut, and not one picture between.
  bool repeat = submitted && submitted < submittedBefore && g.keptFor < 2;
  // (And the poses the same: a still camera over one standing in place, a
  // spark gone, isn't one, its bones moved.)
  // (The HUD and menus' draws, unmatched above, count as new: left out, a
  // menu's frames, a draw fewer than the last now and then over a still
  // scene, were repeats and not shown, the menu at a third of its rate.)
  uint32_t unmatchedNow = 0;
  ArenaMap<uint64_t, int32_t> paletteBefore(arena);
  for (size_t i = 0; repeat && i < g.current.size(); i++) {
    auto& d = g.current[i];
    if (!d.submitted) {
      continue;
    }
    if (match[i] < 0) {
      repeat = ++unmatchedNow <= 2;
    } else if (matchDistance[i] != 0.0f) {
      repeat = false;
    } else if (d.boneSize) {
      if (paletteBefore.empty()) {
        for (int32_t p = 0; p < int32_t(g.previousPalettes.size()); p++) {
          paletteBefore.emplace((uint64_t(g.previousPalettes[p].address) << 32) | g.previousPalettes[p].size, p);
        }
      }
      auto& b = g.previous[match[i]];
      auto now = g.paletteIndex.find((uint64_t(d.boneAddress) << 32) | d.boneSize);
      auto then = paletteBefore.find((uint64_t(b.boneAddress) << 32) | b.boneSize);
      repeat = now != g.paletteIndex.end() && then != paletteBefore.end() &&
               g.palettes[now->second].words == g.previousPalettes[then->second].words;
    }
  }
  const bool partial = submitted && (submitted * 4 < submittedBefore || repeat) && g.keptFor < 2;
  if (partial) {
    g.partial++;
  }
  if (g.lastPresentNs) {
    double gap = double(presentNs - g.lastPresentNs);
    if (gap > 5e6 && gap < 70e6) {
      g.intervalNs += (gap - g.intervalNs) * 0.1;
    }
  }
  g.lastPresentNs = presentNs;
  // Full frames 28 ms or more apart three times in a row: a slow screen
  // (a hitch now and then isn't one); back under 22 ms, not.
  int64_t span = 0;
  if (!partial && submitted) {
    span = g.lastFullNs ? presentNs - g.lastFullNs : 0;
    g.lastFullNs = presentNs;
    if (span >= 28'000'000 && span < 150'000'000) {
      g.slowRun = std::min(g.slowRun + 1, 3u);
    } else if (span < 22'000'000 || span >= 150'000'000) {
      g.slowRun = 0;
    }
    if (g.slow != (g.slowRun >= 3)) {
      REXLOG_INFO("video: interp: {} slow screen (full frames {:.1f} ms apart)", g.slow ? "out of a" : "a",
                  double(span) / 1e6);
    }
    g.slow = g.slowRun >= 3;
  }
  // A cut (another camera, a loaded room): most draws are new.
  bool between = !partial && !g.previous.empty() && submitted;
  if (between && matched * 2 < submitted) {
    between = false;
    g.cuts++;
    static uint32_t logged = 0;
    if (logged < 40) {
      logged++;
      REXLOG_INFO("video: interp: not between (a cut?): {} of {} draws matched, {} draws before", matched,
                  submitted, g.previous.size());
    }
  }
  // A cutscene (a script with a skip argument running: the word the skip
  // reads, gameplay.cpp): no pictures between. The game plays them at 30,
  // cut by cut, and between two shots' frames, or of a body set aside for
  // the next shot, the pictures showed what neither frame had (Chapter 7's
  // cockpit: another shot of Sonia across the canopy, #131).
  // NG2_INTERP_CUTSCENES=1 gives them pictures again.
  static const bool cutscenePictures = [] {
    const char* v = std::getenv("NG2_INTERP_CUTSCENES");
    return v && v[0] == '1';
  }();
  if (between && !cutscenePictures && ng2::gameplay::InCutscene()) {
    between = false;
  }
  // The pictures a frame gets. Paced by the display where it is faster than
  // the game (a refresh under 16 ms). A display at the game's own 60 has no
  // refresh to show a picture in between its frames: none (two were made and
  // presented each frame, three scenes drawn for the one shown), but for a
  // slow screen's, a picture each refresh the game leaves out.
  const int64_t refresh = InterpRefreshNs();
  const bool paced = refresh > 0 && refresh < 16'000'000;
  const bool slowNow = !paced && g.slow && span;
  uint32_t steps = kInterpSteps;
  int64_t slowSpacing = 0;
  if (between && slowNow) {
    // As many pictures as the usual rate gives in that time, or with the
    // display's refresh known, each the same whole number of refreshes
    // (two at 180 Hz for 15 a second: pictures held one, then three,
    // looked as choppy as before).
    double pictureNs = 1e9 / 60.0 / double(steps + 1);
    int64_t wanted = std::llround(double(span) / pictureNs);
    int64_t cap = std::min<int64_t>(kInterpMaxPictures, g_interpPictureCap.load());
    // (no more than the cap: the images a frame may hold, its own counted)
    int64_t least = std::min<int64_t>(steps + 1, cap);
    if (int64_t known = refresh > 0 ? refresh : ModeRefreshNs()) {
      double refreshes = double(span) / double(known);
      int64_t per = std::max<int64_t>(1, int64_t(std::ceil(refreshes / double(cap) - 0.05)));
      wanted = std::max<int64_t>(1, std::llround(refreshes / double(per)));
      slowSpacing = per * known;
      least = 1;  // (no more pictures than refreshes to show them)
    }
    steps = uint32_t(std::clamp<int64_t>(wanted, least, cap) - 1);
  } else if (between && !paced && refresh > 0) {
    steps = 0;
  }
  if (between && !paced && steps == 0) {
    between = false;  // (the frame alone, shown at once)
  }
  if (!between && !partial) {
    g.endNs = 0;  // (shown at once: a paced frame after it starts anew)
  }
  if (between) {
    frames = std::make_shared<InterpFrames>();
    frames->presentNs = presentNs;
    if (paced) {
      // The time this frame stands for (window): from the last frame to it
      // (a slow screen's: from the last full one; a partial frame between,
      // near fire every third, is not shown, so this one's pictures cover
      // both). Where that is a whole number of steps, its pictures go from
      // where the last frame's ended (start: that frame's own picture's time)
      // to its own picture's time (end), so the frame's own picture is always
      // one of those shown and every scene drawn is: the pictures between at
      // each refresh (or the divider's several) back from the end, each as
      // far along as when it is shown. (On the grid of its own, the frame's
      // own picture was shown only when a refresh fell at its end: at 120 Hz
      // most frames drew one scene more than was shown.)
      const int64_t window = span > 0 && span < 4 * int64_t(g.intervalNs) ? span : int64_t(g.intervalNs);
      const int64_t cap = std::min<int64_t>(kInterpMaxPictures, g_interpPictureCap.load());
      int64_t per = std::max<int64_t>(InterpDivider(),
                                      int64_t(std::ceil(double(window) / double(refresh) / double(cap) - 0.05)));
      const int64_t lag = std::clamp<int64_t>(g_interpReadyLagNs.load() + 1'500'000, 2'000'000, 20'000'000);
      const int64_t step = refresh * per;
      // The game's frame a whole number of steps (120, 180, 240 Hz; half rate
      // at 240): each frame's own picture shown, its pictures pinned between
      // the last one's and its own. Otherwise on the display's grid.
      // (by the display's rate against the game's 60, not the game's frame
      // as measured: a game dipping to 55 crossed the line and back, a jump
      // from one timing to the other each time)
      // Not under gamescope (the Steam Deck), where the last frame's pictures
      // are hurried out at each frame's first acquire: the frame's own, timed
      // a frame after its pictures are made, went out with the next frame's,
      // pictures in twos 0-4 ms apart.
      const double perFrame = 1e9 / 60.0 / double(step);
      // (NG2_TEST_PINNED=1: under gamescope too, for the headless tests)
      static const bool pinnedTest = [] {
        const char* v = std::getenv("NG2_TEST_PINNED");
        return v && v[0] == '1';
      }();
      const bool whole = (!UnderGamescope() || pinnedTest) && perFrame >= 0.9 &&
                         std::fabs(perFrame - std::round(perFrame)) < 0.1;
      if (whole) {
        g.baseNs = g.gridNs = 0;
        const int64_t ready = presentNs + lag;  // (its pictures made: none goes out sooner)
        // From its pictures being ready to its own shown: the game's usual
        // frame, whatever time this one stands for. (A late frame's, or one
        // after a partial frame, stands for more: its pictures go from when
        // they are made to the usual time after, the first part of its motion
        // left out with the wait that was. Shown that much later instead, the
        // frames after it carried the time: three frames' pictures waiting in
        // the presenter, the render thread waiting 16 ms a frame for room
        // there, the game at 53 and its menus slow to answer.)
        g.latencyNs = std::min<int64_t>(int64_t(g.intervalNs), window);
        const int64_t target = ready + g.latencyNs;
        // The times follow the display, not its nominal rate: where the
        // presenter shows the pictures after their time (a display at
        // 119.98 Hz paced as the mode's 120.00: 0.2 ms later each second,
        // 20 ms of latency after a minute and a half), the start goes an
        // eighth of that later each frame, until they are shown on time. The
        // frames then fall behind their target by a step now and then, and
        // one gets no picture between.
        // (each one taken once: a late one after a hitch would go on moving
        // the times; and only with a picture between to give up: a frame of
        // one step can't make up what its start is put back)
        const int64_t late = std::clamp<int64_t>(g_interpLateNs.exchange(0), 0, 50'000'000);
        int64_t start = g.endNs ? g.endNs + (std::llround(perFrame) >= 2 ? late / 8 : 0) : 0;
        if (!start || std::abs(target - window - start) > 2 * window) {
          start = target - window;  // (the first, or far off after a hitch: anew)
          g.pacedAnew++;
        }
        // The steps from the start to the end: those to the target, the
        // nearest; where the frame is a whole number of them (2 at 120 Hz)
        // that number until the target is six tenths of one off (the
        // game's 60 and the display's rate drift apart slowly: at half a
        // refresh off, the frames' jitter gave one frame a refresh more and the
        // next one less, back and forth).
        // (in steps: a refresh, or the divider's several, so that at half
        // rate the pictures stay each second refresh from frame to frame)
        const double exact = double(target - start) / double(step);
        // (the game's usual frame, smoothed: each one's own time jitters a millisecond or two)
        const bool regular = std::abs(window - int64_t(g.intervalNs)) < int64_t(g.intervalNs) / 4;
        const double usual = double(regular ? int64_t(g.intervalNs) : window) / double(step);
        int64_t count = std::llround(exact);
        if (std::fabs(usual - std::round(usual)) < 0.1 && std::fabs(exact - std::round(usual)) < 0.6) {
          count = std::llround(usual);
        }
        count = std::max<int64_t>(count, 1);
        g.pacedFrames++;
        g.pacedUneven += std::fabs(double(count) - double(window) / double(step)) > 0.75;
        const int64_t end = start + count * step;
        g.endNs = end;
        // Back from the end a step apart, down to the start (the last frame's
        // own is there), none before they are made (it would go out late, on
        // top of the next), no more than the images allow.
        std::vector<int64_t> times;
        for (int64_t t = end - step; t > start + step / 2 && t >= ready - refresh / 2 &&
                                     int64_t(times.size()) + 1 < cap;
             t -= step) {
          times.push_back(t);
        }
        // 2x: one picture between the game's frames, not one each refresh: the
        // refresh nearest halfway (at 180 Hz shown one refresh, then two: each
        // drawn as far along as when it is shown, so the motion keeps its speed).
        if (REXCVAR_GET(frame_interpolation) == 3 && times.size() > 1) {
          const int64_t half = start + std::max<int64_t>(1, count / 2) * step;
          int64_t best = times[0];
          for (int64_t t : times) {
            if (std::abs(t - half) < std::abs(best - half)) {
              best = t;
            }
          }
          times = {best};
        }
        std::reverse(times.begin(), times.end());
        for (int64_t t : times) {
          frames->times.push_back(t);
          frames->fractions.push_back(float(double(t - start) / double(end - start)));
        }
        g.pacedBetween += frames->fractions.size();
        frames->times.push_back(end);  // (the frame's own)
        frames->ownShown = true;
      } else {
        // Where the game's frame isn't a whole number of steps (75, 90, 100,
        // 144, 165 Hz; half rate at 144): the pictures on the display's grid,
        // each as far along as when it is shown, the frame's own only when a
        // refresh falls at its end. (Pinned to whole refreshes there, the
        // frames took 2 and 3 refreshes by turns at 144 Hz, and the motion a
        // refresh with them: 0.5, 0.5, 0.33, 0.33, 0.33 of a frame.)
        // (from the pinned timing: on from its last frame's own picture)
        int64_t base = presentNs + lag;
        if (g.endNs && !g.baseNs) {
          base = std::max(base, g.endNs);
          g.gridNs = g.endNs + step;  // (the refresh after its own, not that one again)
        }
        g.endNs = 0;
        g.latencyNs = 0;
        g.pacedFrames++;
        if (g.baseNs) {
          // Where the last frame's pictures ended (its own shown): this one's
          // start, so one goes on from the other. (From the last start and this
          // frame's own time, a frame 3 ms later than the last left 3 ms with
          // no picture: a refresh shown twice, a jerk, about once a second.)
          int64_t predicted = g.baseNs + (g.windowNs ? g.windowNs : window);
          if (std::abs(base - predicted) < window / 3) {
            // (drawn to the present slowly; never before the pictures are made)
            base = std::max(predicted + (base - predicted) / 16, presentNs + lag - 1'500'000);
          }
        }
        g.baseNs = base;
        g.windowNs = window;
        if (g.gridNs) {  // (the grid by the display as it shows them, as the pinned times)
          g.gridNs += std::clamp<int64_t>(g_interpLateNs.exchange(0), 0, 50'000'000) / 8;
        }
        if (!g.gridNs || std::abs(g.gridNs - base) > 4 * window) {
          g.gridNs = base + 1;
        }
        while (g.gridNs <= base) {
          g.gridNs += step;
        }
        while (g.gridNs <= base + window + step / 8 && int64_t(frames->times.size()) < cap) {
          float f = std::min(1.0f, float(double(g.gridNs - base) / double(window)));
          frames->times.push_back(g.gridNs);
          frames->fractions.push_back(f);
          g.gridNs += step;
        }
        // 2x: of the refreshes in the window, the one nearest halfway and the last.
        if (REXCVAR_GET(frame_interpolation) == 3 && frames->times.size() > 2) {
          size_t half = 0;
          for (size_t k = 1; k + 1 < frames->times.size(); k++) {
            if (std::fabs(frames->fractions[k] - 0.5f) < std::fabs(frames->fractions[half] - 0.5f)) {
              half = k;
            }
          }
          frames->times = {frames->times[half], frames->times.back()};
          frames->fractions = {frames->fractions[half], frames->fractions.back()};
        }
        frames->ownShown = !frames->fractions.empty() && frames->fractions.back() >= 0.999f;
        if (frames->ownShown) {
          frames->fractions.pop_back();
        }
        g.pacedBetween += frames->fractions.size();
      }
      frames->paced = true;
      steps = uint32_t(frames->fractions.size());
      frames->slow = g.slow;
      frames->spanNs = span;
    } else if (slowNow) {
      frames->spacingNs = slowSpacing;
      frames->slow = true;
      frames->spanNs = span;
      static uint32_t logged = 0;
      if (logged < 20) {
        logged++;
        REXLOG_INFO("video: interp: slow screen frame {}: {} pictures over {:.1f} ms", frameCount, steps + 1,
                    double(span) / 1e6);
      }
    }
    frames->phases.resize(steps);
    // What the matched draws' registers were the frame before, by register and
    // value now: the camera, the view, the light (the same for every draw that
    // reads them). A draw without a match (one of many alike: particles, a new
    // one) takes those of its registers from them, the rest as it is: drawn
    // where it is now, seen from the camera part of the way, as the others.
    auto sharedKey = [](uint16_t r, const float* y) {
      uint64_t a, b;
      std::memcpy(&a, y, 8);
      std::memcpy(&b, y + 2, 8);
      return (a * 0x9E3779B97F4A7C15ull) ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2)) ^ (uint64_t(r) << 52);
    };
    uint64_t jumpedDraws = 0;
    // Only the moved registers take the value now, in every draw that has
    // that value (one light, the same in each pass); the matrices go on part
    // of the way, so a thing lit by a light sorted again stays in one piece.
    ValueMap<uint8_t> jumpedValues(registersNow, arena);
    // Which registers moved: each draw on its own (a crowd's 15k registers,
    // each looked for among its draw's others), spread over a few threads in
    // runs of draws, into one array (the arena isn't for threads); the values
    // that jumped then noted here.
    ArenaVector<uint32_t> firstRegister(g.current.size() + 1, 0, arena);
    for (size_t i = 0; i < g.current.size(); i++) {
      firstRegister[i + 1] = firstRegister[i] + uint32_t(g.current[i].registers.size());
    }
    ArenaVector<uint8_t> movedAll(registersNow, 0, arena), asNowAll(registersNow, 0, arena);
    ArenaVector<uint8_t> anyMovedIn(g.current.size(), 0, arena);
    auto findMoved = [&](size_t i) {
      if (match[i] < 0 || matchDistance[i] == 0.0f) {
        return;
      }
      auto& d = g.current[i];
      auto& before = g.previous[match[i]];
      uint8_t* moved = movedAll.data() + firstRegister[i];
      uint8_t* asNow = asNowAll.data() + firstRegister[i];
      bool anyMoved = false;
      // A thing that spins fast (a helicopter's rotor, a turbine's fan: a
      // quarter turn or more in a frame): its matrix's rows turn that far,
      // and part of the way between two such the rows are shorter (by the
      // cosine of half the angle: nothing left at a half turn), the blades
      // shrunk, gone and back from picture to picture (Chapter 7's cutscenes,
      // #131). Rows of its own (two at least: one vector passing by zero
      // isn't a turn) that lose a fifth of their length halfway, about 75
      // degrees: taken as now, the rotor stepping at the game's rate as it
      // does without the pictures between.
      auto spun = [&](size_t r) {
        if ((r < skips[i].size() && skips[i][r]) || d.registers[r] >= 64) {
          return false;
        }
        const float* x = &before.constants[r * 4];
        const float* y = &d.constants[r * 4];
        float lx = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        float ly = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
        float mx = 0.5f * (x[0] + y[0]), my = 0.5f * (x[1] + y[1]), mz = 0.5f * (x[2] + y[2]);
        float half = std::sqrt(mx * mx + my * my + mz * mz);
        float mean = 0.5f * (lx + ly);
        // (rows of like length: a turn, not a thing shown or hidden by its scale)
        return std::isfinite(half) && mean > 1e-4f && std::fabs(lx - ly) < 0.1f * mean && half < 0.8f * mean;
      };
      uint32_t spunRows = 0;
      for (size_t r = 0; r < d.registers.size() && spunRows < 2; r++) {
        spunRows += spun(r);
      }
      for (size_t r = 0; r < d.registers.size(); r++) {
        const bool shared = r < skips[i].size() && skips[i][r];
        const float* x = &before.constants[r * 4];
        const float* y = &d.constants[r * 4];
        float change = 0, size = 1.0f;
        for (uint32_t c = 0; c < 4; c++) {
          change = std::max(change, std::fabs(x[c] - y[c]));
          size = std::max({size, std::fabs(x[c]), std::fabs(y[c])});
        }
        // Changed to a value another draw had the frame before: a list's
        // entry taken from there (a light gone from this draw's list and
        // another in its place: 35,-1,-1 reach 10 then 107,11,-109 reach 6;
        // part of the way, the light crossed the room and the walls' shading
        // with it, seen as the walls moving). A moving thing's or camera's
        // values are new each frame. Also for the registers many draws share
        // (a light is).
        // (By a third of itself at least: a colour fading a little isn't.)
        // Moved to another register (a list sorted again: the lights): not a
        // turn, however fast, which goes part of the way.
        // Or changed by about its whole size: another value, not a move (a
        // thing's list of lights given others as it went from room to room:
        // 17,26,-425 then -5.7,-7.3,0; part of the way, its light swept
        // across the room). Only a light's (c64 on): a camera's matrix row is
        // about 1 each, and a turn changes it by about that much; taken as
        // replaced, the floor's camera went as now while Ryu's (the same
        // matrix, in other registers) went part of the way, and the floor
        // slid off under him.
        const bool replaced = !shared && change > 0.9f * size && d.registers[r] >= 64;
        // A light's place (c96-c99: where, its reach in w) gone further than
        // half its reach, or its reach changed by a quarter: another light in
        // that place of the list, not this one moving. The game gives a thing
        // one set of lights, then another, frame by frame (a door's pieces:
        // 33,29,-406 reach 0.5 and 17,26,-412 reach 6 by turns); in the
        // world's units the change was small beside the place (4%), and part
        // of the way the light swept across the piece, over and over.
        bool lightSwapped = false;
        if (d.registers[r] >= 96 && d.registers[r] < 100 && std::isfinite(change)) {
          const float dx = x[0] - y[0], dy = x[1] - y[1], dz = x[2] - y[2];
          const float reach = std::max(std::fabs(x[3]), std::fabs(y[3]));
          lightSwapped = std::sqrt(dx * dx + dy * dy + dz * dz) > 0.5f * reach ||
                         std::fabs(x[3] - y[3]) > 0.25f * reach;
        }
        moved[r] = !std::isfinite(change) || replaced || lightSwapped ||
                   (!shared && d.registers.size() == before.registers.size() &&
                    InterpMovedFromAnother(before.constants.data(), d.constants.data(), d.registers.size(), r));
        anyMoved |= moved[r] != 0;
      }
      // (the turned rows alone: not the rows after them, where the thing is,
      // or a rotor would stand still while its helicopter moved on)
      bool anySpun = false;
      for (size_t r = 0; r < d.registers.size() && spunRows >= 2; r++) {
        anySpun |= spun(r);
      }
      if (!anyMoved && !anySpun) {
        return;
      }
      // A light is several registers (where, colour, reach): one found moved,
      // its run from it is taken as now, or one light's place would go with
      // another's reach (a flash on a wall, on the floor).
      InterpRunsFrom(moved, d.registers, asNow);
      for (size_t r = 0; r < d.registers.size() && anySpun; r++) {
        asNow[r] |= spun(r) ? 1 : 0;
      }
      anyMovedIn[i] = 1;
    };
    {
      constexpr size_t kRun = 32;
      static const uint32_t helpers = std::min(3u, std::max(1u, std::thread::hardware_concurrency() / 4) - 1);
      const size_t runs = (g.current.size() + kRun - 1) / kRun;
      g_interpPool.ForEach(runs, g.current.size() >= 128 ? helpers : 0, [&](size_t run) {
        for (size_t i = run * kRun; i < std::min(g.current.size(), (run + 1) * kRun); i++) {
          findMoved(i);
        }
      });
    }
    for (size_t i = 0; i < g.current.size(); i++) {
      if (!anyMovedIn[i]) {
        continue;
      }
      auto& d = g.current[i];
      auto& before = g.previous[match[i]];
      const uint8_t* asNow = asNowAll.data() + firstRegister[i];
      for (size_t r = 0; r < d.registers.size(); r++) {
        if (!asNow[r]) {
          continue;
        }
        if (std::memcmp(&before.constants[r * 4], &d.constants[r * 4], 16) != 0) {
          jumpedValues[sharedKey(d.registers[r], &d.constants[r * 4])] = 1;
          jumpedDraws++;
        }
      }
    }
    g.jumped += jumpedDraws;
    // By value, not by draw: the passes of one thing (its colour, its light,
    // its outline, by other shaders) carry the same matrix now, and each must
    // go the same way, or the picture between shows it twice (a ghost). A
    // value now that a matched draw had otherwise before takes that; a value
    // that jumped in some draw takes it as now in every draw that has it.
    // (with the register: a common value, a white, means nothing across registers)
    auto valueAt = [&](uint16_t r, const float* y) { return sharedKey(r, y); };
    ValueMap<const float*> shared(registersNow, arena);
    for (size_t i = 0; i < g.current.size(); i++) {
      if (match[i] < 0 || matchDistance[i] == 0.0f) {
        continue;
      }
      auto& d = g.current[i];
      auto& before = g.previous[match[i]];
      for (size_t r = 0; r < d.registers.size(); r++) {
        const float* x = &before.constants[r * 4];
        const float* y = &d.constants[r * 4];
        if (std::memcmp(x, y, 16) != 0) {
          shared.emplace(valueAt(d.registers[r], y), x);
        }
      }
    }
    // Per draw: each register's value the frame before (null: as now), found
    // once for all the pictures; none when nothing moves.
    // Per kind of draw (its shaders and target), a register with one value in
    // all its draws, before and now (that shader's own camera: a billboard's
    // view, which no other kind has): what a draw of it without a match (a
    // particle, in data written anew each frame) takes the value before from.
    struct Common {
      const float* x = nullptr;
      const float* y = nullptr;
      uint32_t nx = 0, ny = 0;
      bool xSame = true, ySame = true;
    };
    ArenaMap<uint64_t, ArenaVector<Common>> common(arena);
    auto noteCommon = [&](const InterpDraw& d, bool now) {
      auto& c = common[d.group];
      if (c.empty()) {
        c.resize(d.registers.size());
      }
      if (c.size() != d.registers.size()) {
        return;
      }
      for (size_t r = 0; r < c.size(); r++) {
        const float* v = &d.constants[r * 4];
        const float*& at = now ? c[r].y : c[r].x;
        bool& same = now ? c[r].ySame : c[r].xSame;
        if (!at) {
          at = v;
        } else if (same && std::memcmp(at, v, 16) != 0) {
          same = false;
        }
        (now ? c[r].ny : c[r].nx)++;
      }
    };
    for (size_t i = 0; i < g.current.size(); i++) {
      if (g.current[i].submitted && match[i] < 0) {
        common[g.current[i].group];  // (only the kinds with a draw without a match)
      }
    }
    for (auto& d : g.previous) {
      if (d.submitted && common.count(d.group)) {
        noteCommon(d, false);
      }
    }
    for (auto& d : g.current) {
      if (d.submitted && common.count(d.group)) {
        noteCommon(d, true);
      }
    }
    // The pixel shader's registers, by the draw's own match: part of the way,
    // but a light list sorted again taken as now from the first moved one
    // on (its run of registers), as the vertex shader's.
    ArenaVector<ArenaVector<uint8_t>> psLerp(g.current.size(), arena);  // 1: part of the way
    uint64_t psDraws = 0;
    for (size_t i = 0; i < g.current.size(); i++) {
      auto& d = g.current[i];
      if (!d.submitted || match[i] < 0 || d.psConstants.empty()) {
        continue;
      }
      auto& before = g.previous[match[i]];
      if (before.psRegisters != d.psRegisters ||
          std::memcmp(before.psConstants.data(), d.psConstants.data(), d.psConstants.size() * 4) == 0) {
        continue;
      }
      ArenaVector<uint8_t> lerp(d.psRegisters.size(), 0, arena), psMoved(d.psRegisters.size(), 0, arena);
      for (size_t r = 0; r < d.psRegisters.size(); r++) {
        const float* x = &before.psConstants[r * 4];
        const float* y = &d.psConstants[r * 4];
        float size = 1.0f, change = 0;
        for (uint32_t c = 0; c < 4; c++) {
          size = std::max({size, std::fabs(x[c]), std::fabs(y[c])});
          change = std::max(change, std::fabs(x[c] - y[c]));
        }
        if (change == 0.0f) {
          continue;
        }
        if (!std::isfinite(change)) {
          continue;  // (as now)
        }
        // A register of part numbers (each n.1, 65535.1 for none) is numbers,
        // not amounts: as now. The bodies' pixel shaders (SupDel, c30) drop
        // the pixels of a cut-off part, its number equal to one of these to
        // 0.0001; mixed, the number between named no part, and the cut part
        // showed, stretched (corpse spikes).
        auto numbers = [](const float* v) {
          for (uint32_t c = 0; c < 4; c++) {
            if (!(v[c] >= 0.0f && std::fabs(v[c] - std::floor(v[c]) - 0.1f) < 0.01f)) {
              return false;
            }
          }
          return true;
        };
        if (numbers(x) && numbers(y)) {
          continue;
        }
        lerp[r] = 1;
        psMoved[r] = InterpMovedFromAnother(before.psConstants.data(), d.psConstants.data(), d.psRegisters.size(), r);
      }
      ArenaVector<uint8_t> asNow(d.psRegisters.size(), 0, arena);
      InterpRunsFrom(psMoved.data(), d.psRegisters, asNow.data());
      for (size_t r = 0; r < d.psRegisters.size(); r++) {
        if (asNow[r]) {
          lerp[r] = 0;
        }
      }
      psLerp[i] = std::move(lerp);
      psDraws++;
    }
    g.psDraws += psDraws;
    ArenaVector<ArenaVector<const float*>> borrowed(g.current.size(), arena);
    ArenaVector<bool> notOwn(g.current.size(), false, arena);  // drawn without its own match's values
    // A thing the game didn't draw the frame before (some it leaves out of
    // a frame now and then; one come into view as the camera turned: a
    // chest, a corpse's sword): its own matrix (it times the camera) is new
    // while the camera's registers take the frame before's, and drawn
    // between it stood off its place, a jerk. Drawn the frame before that,
    // the frame before's value is taken as halfway between that one and now.
    ArenaVector<ArenaVector<float>> halfway(g.current.size(), arena);
    ArenaVector<bool> olderUsed(g.older.size(), false, arena);
    uint64_t fromOlder = 0;
    uint64_t borrowedDraws = 0;
    for (size_t i = 0; i < g.current.size(); i++) {
      auto& d = g.current[i];
      if (!d.submitted || d.ui) {
        continue;
      }
      // (a moved value is taken as now one by one, below)
      const InterpDraw* before = match[i] >= 0 ? &g.previous[match[i]] : nullptr;
      notOwn[i] = !before;
      bool any = false;
      ArenaVector<const float*> xs(d.registers.size(), nullptr, arena);
      ArenaVector<bool> found(before ? 0 : d.registers.size(), false, arena);  // (a value before, if any)
      // (not one a matched draw says where it was: a part of a thing whose
      // other meshes were drawn, with the thing's own matrix in it. The one
      // two frames ago was any copy's, a pod's inner part taken from the next
      // pod's: between, it stood out of its pod, halfway to the next, #131)
      bool placedByThing = false;
      if (!before && match[i] < 0 && d.mesh && !d.boneSize) {
        for (size_t r = 0; r < d.registers.size() && !placedByThing; r++) {
          if ((r < skips[i].size() && skips[i][r]) || d.registers[r] >= 64) {
            continue;
          }
          placedByThing = shared.find(valueAt(d.registers[r], &d.constants[r * 4])) != nullptr;
        }
      }
      // A draw with no draw before, and the camera's change known: its
      // matrix before as if it stood still (the matrix now carried back). A
      // thing come into view moves in with the rest of the picture, not at
      // its place now a picture early; nothing is taken from another copy.
      bool carriedBack = false;
      if (!before && match[i] < 0 && cameraFor(d.group) && !d.boneSize && !d.ui && !placedByThing) {
        Mat4 now;
        if (wvpOf(d, d.constants, now)) {
          Mat4 was = carried(now, cameraBack);
          auto& h = halfway[i];
          h.assign(d.constants.begin(), d.constants.end());
          for (size_t r = 0; r < d.registers.size(); r++) {
            if (d.registers[r] >= 21 && d.registers[r] <= 24) {
              for (uint32_t c = 0; c < 4; c++) {
                h[r * 4 + c] = float(was.m[d.registers[r] - 21][c]);
              }
            }
          }
          carriedBack = true;
          statBack++;
        }
      }
      if (!before && match[i] < 0 && d.mesh && !d.boneSize && !placedByThing && !carriedBack) {
        int32_t best = -1;
        float bestDistance = 0.6f;  // (two frames' move)
        for (uint32_t j = 0; j < g.older.size(); j++) {
          auto& o = g.older[j];
          if (olderUsed[j] || !o.submitted || o.mesh != d.mesh || o.group != d.group ||
              o.constants.size() != d.constants.size()) {
            continue;
          }
          // (new among its copies: itself two frames ago only)
          if (strangerReg[i] >= 0 && std::memcmp(&o.constants[size_t(strangerReg[i]) * 4],
                                                 &d.constants[size_t(strangerReg[i]) * 4], 16) != 0) {
            continue;
          }
          float distance = InterpDistance(d, o, 0.6f, &matchSkips[i]);
          if (!(distance < 0.6f)) {
            continue;
          }
          // (by its matrices where it has its own: which copy it was; one
          // whose matrices are far is another copy, a blind pick)
          float apart = matrixApart(i, o);
          if (apart >= 0) {
            if (apart >= 0.45f) {
              continue;
            }
            distance = apart;
          }
          if (distance < bestDistance) {
            best = int32_t(j);
            bestDistance = distance;
          }
        }
        if (best >= 0) {
          olderUsed[best] = true;
          auto& o = g.older[best].constants;
          auto& h = halfway[i];
          h.resize(d.constants.size());
          for (size_t v = 0; v < h.size(); v++) {
            h[v] = std::isfinite(o[v]) ? 0.5f * (o[v] + d.constants[v]) : d.constants[v];
          }
          for (size_t r = 0; r < d.registers.size(); r++) {
            if (std::memcmp(&h[r * 4], &d.constants[r * 4], 16) != 0) {
              xs[r] = &h[r * 4];
              any = true;
            }
          }
          borrowed[i] = std::move(xs);
          fromOlder++;
          continue;
        }
      }
      for (size_t r = 0; r < d.registers.size(); r++) {
        const float* y = &d.constants[r * 4];
        const float* x = nullptr;
        if (!jumpedValues.empty() && jumpedValues.contains(valueAt(d.registers[r], y))) {
          continue;  // (moved: as now)
        }
        // Its own match's value first; another draw's (by the value now) only
        // for a draw without one, and not one that jumped there (that draw's
        // jump spread to every draw with the value: a spike across the
        // pictures between).
        const float* const* sharedBefore = before ? nullptr : shared.find(valueAt(d.registers[r], y));
        if (sharedBefore) {
          const float* other = *sharedBefore;
          float size = 1.0f, change = 0;
          for (uint32_t c = 0; c < 4; c++) {
            size = std::max({size, std::fabs(other[c]), std::fabs(y[c])});
            change = std::max(change, std::fabs(other[c] - y[c]));
          }
          if (!(change <= kInterpJump * size)) {
            sharedBefore = nullptr;
          }
        }
        if (before) {
          x = &before->constants[r * 4];
        } else if (sharedBefore) {
          x = *sharedBefore;
        } else if (auto c = common.find(d.group); c != common.end() && r < c->second.size()) {
          auto& one = c->second[r];
          if (one.nx >= 2 && one.ny >= 2 && one.xSame && one.ySame) {
            x = one.x;  // (the kind's own camera)
          }
        }
        if (!before && x) {
          found[r] = true;
        }
        if (x && std::memcmp(x, y, 16) != 0) {
          xs[r] = x;
          any = true;
        }
      }
      // A draw without its own match: a run of registers (a matrix: c21-c24)
      // some of whose rows have a value before and some not (rows the kind's
      // draws all share, its camera's turn, and a row of its own, where it
      // is) was the camera before turning a place now: a thing near the
      // camera swung far, into view (Chapter 7's cockpit while the camera
      // shook: a Sonia set aside for the next shot, a ghost across the
      // pictures between, #131). Such a run as it is now, all of it.
      // (only a mesh drawn before, its change too big to be taken: not a
      // particle or a sprite, whose own place is new each frame while it
      // goes with the camera)
      if (!before && any && meshBefore[i]) {
        any = false;
        for (size_t r = 0; r < d.registers.size();) {
          size_t end = r + 1;
          while (end < d.registers.size() && d.registers[end] == d.registers[end - 1] + 1) {
            end++;
          }
          bool some = false, all = true;
          for (size_t k = r; k < end; k++) {
            some |= xs[k] != nullptr;
            all &= found[k];
          }
          if (some && !all) {
            for (size_t k = r; k < end; k++) {
              xs[k] = nullptr;
            }
          }
          for (size_t k = r; k < end; k++) {
            any |= xs[k] != nullptr;
          }
          r = end;
        }
      }
      if (carriedBack) {
        for (size_t r = 0; r < d.registers.size(); r++) {
          if (d.registers[r] >= 21 && d.registers[r] <= 24) {
            xs[r] = std::memcmp(&halfway[i][r * 4], &d.constants[r * 4], 16) != 0 ? &halfway[i][r * 4] : nullptr;
            any |= xs[r] != nullptr;
          }
        }
      }
      if (any) {
        borrowed[i] = std::move(xs);
        borrowedDraws += match[i] < 0;
      }
    }
    g.fromOlder += fromOlder;
    g.borrowed += borrowedDraws;
    // Each picture on its own thread (they share nothing but the uploads,
    // taken under a lock): a crowd's 3 ms of constants in about one picture's
    // time. The first is this thread's, and counts the registers.
    // (one picture, or a few draws: all on this thread, no lock)
    const uint32_t pictureHelpers = submitted >= 200 && steps > 1 ? steps - 1 : 0;
    std::mutex uploadLock;
    auto LockedAllocate = [&](uint64_t size) {
      if (!pictureHelpers) {
        return Allocate(size);
      }
      std::lock_guard lock(uploadLock);
      return Allocate(size);
    };
    auto picture = [&](uint32_t k) {
      auto Allocate = LockedAllocate;
      float t = frames->paced ? frames->fractions[k] : float(k + 1) / float(steps + 1);
      auto& phase = frames->phases[k];
      phase.resize(g.current.size());
      for (size_t i = 0; i < g.current.size(); i++) {
        auto& d = g.current[i];
        phase[i] = d.pushConstants;
        if (!psLerp[i].empty()) {
          auto& before = g.previous[match[i]];
          Upload upload = Allocate((d.psRegisters.back() + 1u) * 16u);
          auto* out = reinterpret_cast<float*>(upload.data);
          for (size_t r = 0; r < d.psRegisters.size(); r++) {
            const float* x = &before.psConstants[r * 4];
            const float* y = &d.psConstants[r * 4];
            float v[4];
            for (uint32_t c = 0; c < 4; c++) {
              v[c] = psLerp[i][r] ? x[c] + (y[c] - x[c]) * t : y[c];
            }
            std::memcpy(out + d.psRegisters[r] * 4, v, sizeof(v));  // (write-combined: written once)
          }
          phase[i][1] = upload.address;
        }
        // NG2_INTERP_DEBUG=1: the draws without their own match left out of
        // the pictures between (their constants all zero), to see which they are.
        static const bool hideNotOwn = [] {
          const char* v = std::getenv("NG2_INTERP_DEBUG");
          return v && v[0] == '1';
        }();
        if (hideNotOwn && notOwn[i] && d.submitted) {
          Upload zeros = Allocate(d.registers.empty() ? 16 : (d.registers.back() + 1u) * 16u);
          std::memset(zeros.data, 0, d.registers.empty() ? 16 : (d.registers.back() + 1u) * 16u);
          phase[i][0] = zeros.address;
          continue;
        }
        if (borrowed[i].empty()) {
          continue;  // (as now)
        }
        // Up to the last register the shader reads (the registers ascend).
        Upload upload = Allocate(d.registers.empty() ? 16 : (d.registers.back() + 1u) * 16u);
        auto* out = reinterpret_cast<float*>(upload.data);
        for (size_t r = 0; r < d.registers.size(); r++) {
          const float* y = &d.constants[r * 4];
          const float* x = borrowed[i][r] ? borrowed[i][r] : y;
          float size = 1.0f, change = 0;
          for (uint32_t c = 0; c < 4; c++) {
            size = std::max({size, std::fabs(x[c]), std::fabs(y[c])});
            change = std::max(change, std::fabs(x[c] - y[c]));
          }
          bool snap = !std::isfinite(change);  // (a turn, however fast, goes part of the way)
          if (k == 0) {
            g.registers++;
            g.snapped += snap;
          }
          float v[4];
          for (uint32_t c = 0; c < 4; c++) {
            v[c] = snap ? y[c] : x[c] + (y[c] - x[c]) * t;
          }
          std::memcpy(out + d.registers[r] * 4, v, sizeof(v));  // (write-combined: written once)
        }
        phase[i][0] = upload.address;
      }
    };
    g_interpPool.ForEach(steps, pictureHelpers, [&](size_t k) { picture(uint32_t(k)); });
    int64_t mixedNs = SteadyNs();
    g.constantsNs += mixedNs - matchedNs;
    // Bone palettes: as the draws that read them were matched.
    ArenaMap<uint64_t, int32_t> previousIndex(arena);
    for (int32_t i = 0; i < int32_t(g.previousPalettes.size()); i++) {
      auto& palette = g.previousPalettes[i];
      previousIndex.emplace((uint64_t(palette.address) << 32) | palette.size, i);
    }
    ArenaVector<int32_t> pairs(g.palettes.size(), -1, arena);
    for (size_t i = 0; i < g.current.size(); i++) {
      auto& d = g.current[i];
      if (match[i] < 0 || !d.boneSize) {
        continue;
      }
      auto& before = g.previous[match[i]];
      auto now = g.paletteIndex.find((uint64_t(d.boneAddress) << 32) | d.boneSize);
      auto then = previousIndex.find((uint64_t(before.boneAddress) << 32) | before.boneSize);
      if (now != g.paletteIndex.end() && then != previousIndex.end() && pairs[now->second] < 0 &&
          g.previousPalettes[then->second].words.size() == g.palettes[now->second].words.size()) {
        pairs[now->second] = then->second;
      }
    }
    // Each palette once for all the pictures: a row (four floats, big-endian
    // as the fetch reads them) unchanged copied, a row that jumped taken as it
    // is now, the others part of the way, written straight to each picture's
    // upload.
    float ts[kInterpMaxPictures] = {};
    for (uint32_t k = 0; k < steps; k++) {
      ts[k] = frames->paced ? frames->fractions[k] : float(k + 1) / float(steps + 1);
    }
    frames->copies.assign(steps, std::vector<RenderBufferReference>(g.palettes.size()));
    // Each palette on its own (a crowd's took 3 ms a frame of the build, the
    // render thread waiting): first whether it jumped, then mixed, both
    // spread over a few threads; what's shared (uploads, the log) between.
    struct PaletteJob {
      bool skip = true;  // (no pair, or still)
      bool jumped = false, finite = true;
      double moved = 0;
      size_t rows = 0, worstRow = 0, farRows = 0;
      size_t changed = 0;  // (rows that moved: the mean over them)
      float worst = 0;
      uint32_t* outs[kInterpMaxPictures] = {};
      uint64_t snapped = 0;
    };
    ArenaVector<PaletteJob> jobs(g.palettes.size(), arena);
    static const uint32_t workers = std::min(4u, std::max(1u, std::thread::hardware_concurrency() / 4));
    auto spread = [&](auto&& body) { g_interpPool.ForEach(jobs.size(), workers - 1, body); };
    spread([&](size_t i) {
      auto& job = jobs[i];
      if (pairs[i] < 0) {
        return;
      }
      auto& now = g.palettes[i];
      auto& then = g.previousPalettes[pairs[i]];
      if (now.words == then.words) {
        return;  // (still: the frame's own copy)
      }
      job.skip = false;
      // Another thing's bones (one of several alike: bodies of one kind of
      // enemy, each with its bones elsewhere in the buffer, matched with
      // another's): most bones far from where they were, which mixed would
      // stretch it towards the other (a spike). A pose of its own moves most
      // bones little in a frame, however fast one (the sword) goes: by the
      // mean of each row's largest change (where its last value is, the row
      // order of the matrices unknown).
      for (size_t w = 0; w + 4 <= now.words.size(); w += 4, job.rows++) {
        float move = 0;
        if (!(then.words[w] | then.words[w + 1] | then.words[w + 2] | then.words[w + 3]) ||
            !(now.words[w] | now.words[w + 1] | now.words[w + 2] | now.words[w + 3])) {
          continue;  // (a bone left zero: taken as now, below)
        }
        for (size_t c = 0; c < 4; c++) {
          float fx = std::bit_cast<float>(rex::byte_swap(then.words[w + c]));
          float fy = std::bit_cast<float>(rex::byte_swap(now.words[w + c]));
          job.finite &= std::isfinite(fx) && std::isfinite(fy);
          move = std::max(move, std::fabs(fx - fy));
        }
        job.moved += move;
        job.changed += move > 0;
        job.farRows += move > 2.0f;
        if (move > job.worst) {
          job.worst = move, job.worstRow = job.rows;
        }
      }
      // (Over the rows that moved, not all: a buffer of a group of bodies
      // is read whole, its few hundred bones among thousands of rows unused.
      // When the camera turns the game lays the bones of the bodies in view
      // out anew, many moved to another bone's rows at once: a spike on each
      // body of the group, unseen in the mean over the whole buffer.)
      job.jumped = !job.finite || (job.changed && !(job.moved / double(job.changed) <= kPaletteJump));
    });
    for (size_t i = 0; i < jobs.size(); i++) {
      auto& job = jobs[i];
      if (job.skip) {
        continue;
      }
      auto& now = g.palettes[i];
      if (job.jumped) {
        g.palettesJumped++;
        job.skip = true;
        continue;
      }
      for (uint32_t k = 0; k < steps; k++) {
        Upload upload = Allocate(now.size, 16);
        job.outs[k] = reinterpret_cast<uint32_t*>(upload.data);
        frames->copies[k][i] = RenderBufferReference(upload.buffer, upload.offset);
      }
      g.bonesBytes += now.size;
      g.bonesLerped++;
    }
    spread([&](size_t i) {
      auto& job = jobs[i];
      if (job.skip) {
        return;
      }
      auto& now = g.palettes[i];
      auto& then = g.previousPalettes[pairs[i]];
      uint32_t* const* outs = job.outs;
      size_t words = now.size / 4;
      const uint32_t* x = then.words.data();
      const uint32_t* y = now.words.data();
      // A bone left all zero in one of the frames (the game writes some only
      // every other frame: a cut-off part's), its matrix (three rows of four)
      // as it is now: half of one and none of the other would draw its
      // vertices halfway to the world's middle (a spike).
      auto zeroRow = [&](const uint32_t* p, size_t row) {
        return row * 4 + 4 <= words && !(p[row * 4] | p[row * 4 + 1] | p[row * 4 + 2] | p[row * 4 + 3]);
      };
      // Likewise a bone whose place (each row's last value) moved further
      // than any moves in a frame: one just made (all 1 0 0 0 at the world's
      // middle the frame before), or given for one frame where it is on its
      // own (the place near 0) and back: mixed, its vertices would go halfway.
      auto teleported = [&](size_t r) {
        if (r * 4 + 4 > words) {
          return false;
        }
        float px = std::bit_cast<float>(rex::byte_swap(x[r * 4 + 3]));
        float py = std::bit_cast<float>(rex::byte_swap(y[r * 4 + 3]));
        return !(std::fabs(px - py) <= kBoneTeleport);
      };
      auto boneZero = [&](size_t row) {
        size_t first = row / 3 * 3;
        for (size_t r = first; r < first + 3; r++) {
          if (zeroRow(x, r) || zeroRow(y, r) || teleported(r)) {
            return true;
          }
        }
        return false;
      };
      size_t boneOf = SIZE_MAX;  // (the bone the rows are in: tested once for its three)
      bool zero = false;
      for (size_t w = 0; w < words; w += 4) {
        size_t n = std::min<size_t>(4, words - w);
        size_t row = w / 4;
        if (row / 3 != boneOf) {
          boneOf = row / 3;
          zero = boneZero(row);
        }
        bool sameRow = std::memcmp(x + w, y + w, n * 4) == 0;
        if (!sameRow && row % 3 == 0 && zero) {
          job.snapped++;
        }
        if (sameRow || zero) {
          for (uint32_t k = 0; k < steps; k++) {
            std::memcpy(outs[k] + w, y + w, n * 4);
          }
          continue;
        }
        float fx[4], fy[4];
        bool finite = true;
        for (size_t c = 0; c < n; c++) {
          fx[c] = std::bit_cast<float>(rex::byte_swap(x[w + c]));
          fy[c] = std::bit_cast<float>(rex::byte_swap(y[w + c]));
          finite &= std::isfinite(fx[c]) && std::isfinite(fy[c]);
        }
        if (!finite) {
          for (uint32_t k = 0; k < steps; k++) {
            std::memcpy(outs[k] + w, y + w, n * 4);
          }
          continue;
        }
        for (uint32_t k = 0; k < steps; k++) {
          uint32_t v[4];
          for (size_t c = 0; c < n; c++) {
            v[c] = rex::byte_swap(std::bit_cast<uint32_t>(fx[c] + (fy[c] - fx[c]) * ts[k]));
          }
          std::memcpy(outs[k] + w, v, n * 4);
        }
      }
    });
    for (auto& job : jobs) {
      g.bonesSnapped += job.snapped;
    }
    g.bonesNs += SteadyNs() - mixedNs;
    g.interpolated++;
  }
  if (g.frames == 600) {
    REXLOG_INFO("video: interp: ms a frame: matching {:.2f} (setup {:.2f}, by mesh {:.2f}), constants {:.2f}, "
                "bones {:.2f}; slowest frame {:.2f} ms ({:.2f} {:.2f} {:.2f} {:.2f} {:.2f}); draws {:.0f} a frame, "
                "{} at most",
                double(g.matchNs) / 600e6, double(g.prepNs) / 600e6, double(g.meshNs) / 600e6,
                double(g.constantsNs) / 600e6, double(g.bonesNs) / 600e6, double(g.worstNs) / 1e6,
                g.worstSplit[0] / 1e6, g.worstSplit[1] / 1e6, g.worstSplit[2] / 1e6, g.worstSplit[3] / 1e6,
                g.worstSplit[4] / 1e6,
                double(g.drawsSeen) / 600.0, g.drawsWorst);
    g.matchNs = g.constantsNs = g.bonesNs = g.prepNs = g.meshNs = g.worstNs = 0;
    g.worstSplit = {};
    g.drawsSeen = g.drawsWorst = 0;
    REXLOG_INFO("video: interp: {}/{} frames interpolated, {:.1f}% of draws matched, {:.1f}% "
                "given the camera only, {:.2f}% of registers snapped",
                g.interpolated, g.frames, 100.0 * double(g.matched) / double(std::max<uint64_t>(g.draws, 1)),
                100.0 * double(g.borrowed) / double(std::max<uint64_t>(g.draws, 1)),
                100.0 * double(g.snapped) / double(std::max<uint64_t>(g.registers, 1)));
    REXLOG_INFO("video: interp: {} frames taken for cuts, {:.1f}% of draws matched by their mesh, {} "
                "registers taken as now (sorted again), {} things not drawn the frame before taken from the one before it",
                g.cuts, 100.0 * double(g.byMesh) / double(std::max<uint64_t>(g.draws, 1)), g.jumped, g.fromOlder);
    g.fromOlder = 0;
    REXLOG_INFO("video: interp: {} partial frames (the last full one kept), {:.1f}% of draws with pixel "
                "shader constants part of the way", g.partial,
                100.0 * double(g.psDraws) / double(std::max<uint64_t>(g.draws, 1)));
    g.borrowed = g.cuts = g.byMesh = g.jumped = g.partial = g.psDraws = 0;
    REXLOG_INFO("video: interp: {:.1f} bone palettes a frame part of the way ({:.0f} KB), {:.2f} ms "
                "a frame matching and mixing",
                double(g.bonesLerped) / 600.0, double(g.bonesBytes) / 600.0 / 1024.0,
                double(g.buildNs) / 600.0 / 1e6);
    REXLOG_INFO("video: interp: {:.2f} bone palettes a frame taken as now (a row jumped), {} bones taken as "
                "now (left zero or moved far)",
                double(g.palettesJumped) / 600.0, g.bonesSnapped);
    if (g.pacedFrames && g.latencyNs) {
      REXLOG_INFO("video: interp: paced frames: {:.2f} pictures between each, {} of {} a refresh more or fewer "
                  "than their time, {} started anew; the frame's own shown {:.1f} ms after its pictures are made",
                  double(g.pacedBetween) / double(g.pacedFrames), g.pacedUneven, g.pacedFrames, g.pacedAnew,
                  double(g.latencyNs) / 1e6);
    }
    g.pacedFrames = g.pacedBetween = g.pacedUneven = g.pacedAnew = 0;
    g.bonesBytes = g.palettesJumped = g.bonesSnapped = 0;
    g.buildNs = 0;
    g.frames = g.interpolated = g.draws = g.matched = g.snapped = g.registers = 0;
    g.bonesLerped = 0;
  }
  if (partial) {
    g.keptFor++;
    g.current.clear();
    g.palettes.clear();
    g.paletteIndex.clear();
    // On a display at the game's rate, outside a slow screen: shown at once
    // as the full frames there are (handed to the presenter it came a frame
    // late, on top of the next one).
    if (refresh >= 16'000'000 && !g.slow) {
      return nullptr;
    }
    if (!frames) {
      frames = std::make_shared<InterpFrames>();
    }
    frames->partial = true;
    frames->slow = g.slow;
    // Paced by the display: not shown either (the last full frame's pictures
    // go on by the clock; shown at once it cut them short: a jump, near fire
    // every third frame, seen as a double).
    frames->paced = paced;
    return frames;
  }
  g.keptFor = 0;
  g.older.swap(g.previous);
  g.previous.swap(g.current);
  g.current.clear();
  g.previousPalettes.swap(g.palettes);
  g.palettes.clear();
  g.paletteIndex.clear();
  return frames;
}

}  // namespace

std::shared_future<std::shared_ptr<InterpFrames>> StartInterpBuild(uint32_t slot, uint64_t frameCount,
                                                                   int64_t presentNs) {
  WaitInterpBuild();  // (the build's state is its own till it's done)
  // This frame's bones, as the game will write the next ones there.
  auto& capture = g_interpCapture;
  for (auto& palette : capture.palettes) {
    palette.words.resize(palette.size / 4);
    std::memcpy(palette.words.data(), guest::Physical(palette.address), palette.size & ~3u);
  }
  g_interp.current.swap(capture.current);
  g_interp.palettes.swap(capture.palettes);
  g_interp.paletteIndex.swap(capture.paletteIndex);
  capture.current.clear();
  capture.palettes.clear();
  capture.paletteIndex.clear();
  auto build = std::make_shared<std::packaged_task<std::shared_ptr<InterpFrames>()>>(
      [slot, frameCount, presentNs] { return BuildInterpFrames(slot, frameCount, presentNs); });
  g_interpLastJob = build->get_future().share();
  g_interpPool.Post([build] { (*build)(); });
  return g_interpLastJob;
}

void WaitInterpBuild() {
  if (g_interpLastJob.valid()) {
    g_interpLastJob.wait();
  }
}

void ResetInterp(bool on) {
  g_interpLastJob = {};
  g_interp.previous.clear();
  g_interp.older.clear();
  g_interp.current.clear();
  g_interp.previousPalettes.clear();
  g_interp.palettes.clear();
  g_interp.paletteIndex.clear();
  g_interp.keptFor = 0;
  g_interp.autopause = {};
  g_interp.endNs = g_interp.latencyNs = g_interp.baseNs = g_interp.gridNs = g_interp.lastFullNs =
      g_interp.lastPresentNs = 0;
  g_interp.slowRun = 0;
  g_interp.slow = false;
  g_interpAutoDivider = 1;
  g_interpLateNs = 0;
  g_interpReadyLagNs = 4'000'000;  // (as at the start: what it had grown to stayed through off and on)
  g_interpCapture.current.clear();
  g_interpCapture.palettes.clear();
  g_interpCapture.paletteIndex.clear();
  g_interpOn = on ? 1 : 0;
}

}  // namespace ng2::video
