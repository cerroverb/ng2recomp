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
#include "../aspect.h"
#include "guest.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
  // Half Rate: never fewer pictures than the game's 60 frames (at 60 or 70 Hz
  // each second refresh was 30 or 35 a second): each as many refreshes as
  // fit in the game's frame, two at most (one at 70 or 90 Hz, two at 120 Hz
  // and up).
  int64_t refresh = InterpRefreshNs();
  if (refresh <= 0) {
    return 2;
  }
  return std::clamp<uint32_t>(uint32_t(1e9 / 60.0 / double(refresh) + 0.05), 1u, 2u);
}
}  // namespace

uint32_t InterpDivider() {
  return std::max(InterpSetDivider(), g_interpAutoDivider.load());
}
namespace {

struct InterpState {
  std::vector<InterpDraw> previous, current;
  std::vector<InterpDraw> older;  // the frame before the previous (things the previous didn't draw)
  std::vector<InterpPalette> previousPalettes, palettes;
  std::unordered_map<uint64_t, int32_t> paletteIndex;  // this frame's, by address and size
  uint64_t bonesLerped = 0, bonesBytes = 0, palettesJumped = 0, bonesSnapped = 0;
  int64_t buildNs = 0, matchNs = 0, constantsNs = 0, bonesNs = 0;
  int64_t prepNs = 0, meshNs = 0, worstNs = 0;  // (worst: the slowest frame's whole build)
  uint64_t drawsSeen = 0, drawsWorst = 0;
  uint64_t frames = 0, interpolated = 0, draws = 0, matched = 0, snapped = 0, registers = 0;
  uint64_t fromOlder = 0;  // things not drawn the frame before, taken from the one before it
  uint64_t borrowed = 0, byMesh = 0, cuts = 0, jumped = 0, partial = 0, psDraws = 0, paused = 0;
  int64_t lastBuildNs = 0;  // the last frame's build
  uint32_t keptFor = 0;  // frames the last full frame was kept over (partial ones)
  int64_t lastFullNs = 0;  // the last full frame's present (game thread's clock)
  int64_t lastPresentNs = 0;
  double intervalNs = 1e9 / 60.0;  // between the game's frames, smoothed
  int64_t baseNs = 0;   // the last paced frame's start: its pictures after it
  int64_t windowNs = 0; // the time the last paced frame's pictures covered
  int64_t gridNs = 0;   // the next picture's time on the display's grid
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

// Pictures between two frames where the display's refresh isn't known or
// paced by: 2 (180 Hz for the game's 60).
constexpr uint32_t kInterpSteps = 2;

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

// How far apart two draws' constants are (the largest change, relative to
// each register's size).
float InterpDistance(const InterpDraw& a, const InterpDraw& b, float bound = 1e30f,
                     const std::vector<bool>* skip = nullptr) {
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
static void InterpRunsFrom(const std::vector<uint8_t>& moved, const std::vector<uint16_t>& registers,
                           std::vector<uint8_t>& asNow) {
  asNow.assign(registers.size(), 0);
  for (size_t r = 0; r < registers.size(); r++) {
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
  struct Timed {
    int64_t start;
    ~Timed() {
      int64_t ns = SteadyNs() - start;
      g_interp.buildNs += ns;
      g_interp.worstNs = std::max(g_interp.worstNs, ns);
      g_interp.lastBuildNs = ns;
    }
  } timed{startNs};
  // Paused when the pictures cost the game its 60 (the Steam Deck in a big
  // fight: 7-12 ms of building a frame, the game at 30): the frames shown as
  // they are for a while (1.5 s, longer each time it comes back too soon), then
  // tried again. Whether they cost it is seen in the pause: a game no faster
  // without them (a cutscene at 30 of its own) isn't paused again while it
  // runs at that rate.
  {
    static int64_t pausedUntil = 0, holdNs = 0, resumedNs = 0, lastNs = 0;
    static int64_t halvedNs = 0, halfHoldNs = 0, fullAgainNs = 0;  // (half rate)
    static double buildAvg = 0, gapAvg = 1e9 / 60.0, gapFast = 1e9 / 60.0, gapBefore = 0, ownRate = 0,
                  pausedSum = 0;
    static uint32_t behind = 0, pausedCount = 0, sharpRun = 0;
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
        g.lastFullNs = g.lastPresentNs = g.baseNs = g.gridNs = 0;
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
      if (g_interpAutoDivider.load() > 1 && presentNs - halvedNs > halfHoldNs && gapAvg < 17'000'000) {
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
      const double late = ownRate ? ownRate * 1.08 : 17'500'000.0;
      if (gapValid && gapAvg > late) {
        behind++;
      } else if (gapValid && gapAvg < (ownRate ? ownRate * 1.02 : 17'000'000.0)) {
        behind = behind > 2 ? behind - 2 : 0;
      }
      // A sharp drop (under 45 fps, or well under the game's own rate, for 6
      // frames in a row on a quick average): at once, not after the 45 above
      // (a dip to 72 on the screen came and went before they added up).
      const double sharp = ownRate ? ownRate * 1.3 : 1e9 / 45.0;
      sharpRun = gapValid && gapFast > sharp ? sharpRun + 1 : 0;
      // First half rate (at 120 Hz and up: 60 pictures a second or more
      // still), then the pause: the step from 180 to 60 on the screen was
      // what showed (a heavy room: the game at 45 with the pictures, 53 without).
      const int64_t refresh = InterpRefreshNs();
      const bool canHalve = InterpDivider() < 2 && refresh > 0 && refresh < 8'500'000;
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
  std::unordered_map<uint64_t, std::vector<uint32_t>> groups;
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
  std::unordered_map<uint64_t, uint32_t> valueDraws;  // draws with each value
  std::unordered_map<uint64_t, uint32_t> currentCount;  // draws of each kind this frame
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
  std::vector<std::vector<bool>> skips(g.current.size());
  for (size_t i = 0; i < g.current.size(); i++) {
    auto& d = g.current[i];
    if (!d.submitted) {
      continue;
    }
    auto& skip = skips[i];
    skip.resize(d.registers.size());
    for (size_t r = 0; r < d.registers.size(); r++) {
      auto it = valueDraws.find(valueKey(d.registers[r], &d.constants[r * 4]));
      skip[r] = it != valueDraws.end() && it->second >= sharedDraws;
    }
  }
  // For matching only, also left out: a value many things have now (8 meshes
  // or more), whatever register it's in: a light (where, reach, colour),
  // which the game sorts again into each thing's list as it moves (outdoors,
  // among torches, a chest's list changed every few frames and it lost its
  // match, drawn off its place: a jerk). A thing's own matrix is its own.
  std::vector<std::vector<bool>> matchSkips = skips;
  {
    auto vectorKey = [](const float* y) {
      uint64_t a, b;
      std::memcpy(&a, y, 8);
      std::memcpy(&b, y + 2, 8);
      return (a * 0x9E3779B97F4A7C15ull) ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2));
    };
    // (a thing's own values: its few parts' meshes, 3 for a chest; a light's: 40-200)
    constexpr uint32_t kManyMeshes = 8;
    std::unordered_map<uint64_t, std::vector<uint64_t>> meshesWith;  // value: its meshes, up to 8
    for (auto& d : g.current) {
      if (!d.submitted) {
        continue;
      }
      for (size_t r = 0; r < d.registers.size(); r++) {
        auto& m = meshesWith[vectorKey(&d.constants[r * 4])];
        if (m.size() < kManyMeshes && std::find(m.begin(), m.end(), d.mesh) == m.end()) {
          m.push_back(d.mesh);
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
        auto it = meshesWith.find(vectorKey(&d.constants[r * 4]));
        if (it != meshesWith.end() && it->second.size() >= kManyMeshes) {
          skip[r] = true;
        }
      }
    }
  }
  std::vector<int32_t> match(g.current.size(), -1);
  std::vector<float> matchDistance(g.current.size(), 0.0f);
  std::vector<bool> used(g.previous.size());
  std::unordered_map<uint64_t, uint32_t> occurrence;
  uint32_t submitted = 0, matched = 0;
  // Close enough to take without looking further: the same place among its
  // kind, moved as things move in a frame.
  constexpr float kClose = 0.1f;
  // First by mesh: the same vertex and index data with the same shaders is the
  // same thing, however far the camera turned. One of it before and now: taken
  // unless it moved more than its size (another thing in a reused buffer);
  // several (copies of one mesh): the nearest, as close as alike draws must be.
  {
    auto meshKey = [](const InterpDraw& d) { return d.mesh * 0xC2B2AE3D27D4EB4Full ^ d.group; };
    int64_t meshStartNs = SteadyNs();
    g.prepNs += meshStartNs - startNs;
    struct MeshTimed {
      int64_t start;
      ~MeshTimed() { g_interp.meshNs += SteadyNs() - start; }
    } meshTimed{meshStartNs};
    std::unordered_map<uint64_t, std::vector<uint32_t>> meshes, meshesNow;
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
    // Copies of one skinned mesh (bodies of one kind of enemy) can have the
    // same constants and differ only in their bones: how far the bones of
    // one before are from these now (the median of some rows' moves).
    std::unordered_map<uint64_t, int32_t> previousPalette;
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
    for (auto& [key, now] : meshesNow) {
      auto it = meshes.find(key);
      if (it == meshes.end()) {
        continue;
      }
      auto& before = it->second;
      bool single = now.size() == 1 && before.size() == 1;
      float bound = single ? 1.5f : (before.size() > 8 ? 0.1f : 0.3f);
      // A few alike copies without bones (a door's pieces flying from one
      // place, the same matrix's place, turned differently): paired nearest
      // first over all of them, not each in turn with the nearest still free
      // (the first piece took another's of the frame before, and the one it
      // was went without: part of the way each piece turned into another's
      // pose, a shake, the pieces thrown about).
      if (!single && now.size() <= 16 && before.size() <= 16 && !g.current[now[0]].boneSize) {
        struct Pair {
          float score;
          uint32_t i, j;
        };
        std::vector<Pair> pairs;
        std::unordered_map<uint32_t, std::pair<float, float>> bestOf;  // i: best, second score
        for (uint32_t i : now) {
          auto& d = g.current[i];
          auto& bs = bestOf[i];
          bs = {1e30f, 1e30f};
          for (uint32_t j : before) {
            if (used[j] || g.previous[j].constants.size() != d.constants.size()) {
              continue;
            }
            float distance = InterpDistance(d, g.previous[j], 1.0f, &matchSkips[i]);
            if (!(distance < 1.0f)) {
              continue;
            }
            pairs.push_back({distance, i, j});
            if (distance < bs.first) {
              bs.second = bs.first;
              bs.first = distance;
            } else if (distance < bs.second) {
              bs.second = distance;
            }
          }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
          return a.score < b.score || (a.score == b.score && (a.i < b.i || (a.i == b.i && a.j < b.j)));
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
            matched++;
            g.byMesh++;
          }
        }
        continue;
      }
      for (uint32_t i : now) {
        auto& d = g.current[i];
        int32_t best = -1;
        float bestDistance = 1e30f, bestScore = 1e30f, secondScore = 1e30f;
        // A copy before at the same place in the bone buffer is this one
        // (another place is another body of its kind): only it, if there's one.
        bool havePlace = false;
        if (d.boneSize) {
          for (uint32_t j : before) {
            havePlace |= (d.boneAddress & 0xFFFF) == (g.previous[j].boneAddress & 0xFFFF);
          }
        }
        for (uint32_t j : before) {
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
          float distance = InterpDistance(d, g.previous[j], secondScore, &matchSkips[i]);
          if (!(distance < secondScore)) {
            continue;  // (can't be the best or the next)
          }
          float apart = bonesApart(d, g.previous[j]);
          if (apart > kPaletteJump) {
            continue;  // (another's bones)
          }
          // (the same place in the bone buffer, the next of its copies: most likely it)
          bool samePlace = (d.boneAddress & 0xFFFF) == (g.previous[j].boneAddress & 0xFFFF);
          float score = distance + apart + (d.boneSize && !samePlace ? 0.25f : 0.0f);
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
        bool clear = !d.boneSize && bestDistance <= 0.6f && secondScore >= 2.5f * bestScore;
        if (best >= 0 && (bestDistance <= bound || clear)) {
          used[best] = true;
          match[i] = best;
          matchDistance[i] = bestDistance;
          matched++;
          g.byMesh++;
        }
      }
    }
  }
  for (size_t i = 0; i < g.current.size(); i++) {
    auto& d = g.current[i];
    if (!d.submitted) {
      continue;
    }
    submitted++;
    uint32_t n = occurrence[d.group]++;
    if (match[i] >= 0) {
      continue;  // (by its mesh)
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
      matched++;
    }
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
  uint32_t unmatchedNow = 0;
  std::unordered_map<uint64_t, int32_t> paletteBefore;
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
  if (!partial && !g.previous.empty() && submitted && matched * 2 < submitted) {
    g.cuts++;
    static uint32_t logged = 0;
    if (logged < 40) {
      logged++;
      REXLOG_INFO("video: interp: not between (a cut?): {} of {} draws matched, {} draws before", matched,
                  submitted, g.previous.size());
    }
  }
  if (!partial && !g.previous.empty() && submitted && matched * 2 >= submitted) {
    frames = std::make_shared<InterpFrames>();
    frames->presentNs = presentNs;
    uint32_t steps = kInterpSteps;
    int64_t refresh = InterpRefreshNs();
    if (refresh > 0 && refresh < 16'000'000) {
      // The time this frame stands for: from the last frame to it (a slow
      // screen's: from the last full one), shown from its start B on: B is
      // its present and the time to make its pictures, kept a steady
      // interval from the last one (the game's frames come a little
      // unevenly). On the display's grid (a refresh, or the divider's
      // several, apart; more on a slow screen so as not to pass what the
      // swapchain's images allow), each refresh in (B, B + window] gets a
      // picture that far along; one at the end is the frame's own.
      // (the time since the last full frame: a partial one between, near
      // fire every third, is not shown, so this one's pictures cover both)
      const int64_t window = span > 0 && span < 4 * int64_t(g.intervalNs) ? span : int64_t(g.intervalNs);
      const int64_t cap = std::min<int64_t>(kInterpMaxPictures, g_interpPictureCap.load());
      int64_t per = std::max<int64_t>(InterpDivider(),
                                      int64_t(std::ceil(double(window) / double(refresh) / double(cap) - 0.05)));
      const int64_t step = refresh * per;
      const int64_t lag = std::clamp<int64_t>(g_interpReadyLagNs.load() + 1'500'000, 2'000'000, 20'000'000);
      int64_t base = presentNs + lag;
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
      // 2x: one picture between the game's frames, not one each refresh: of
      // the refreshes in the window, the one nearest halfway and the last
      // (at 180 Hz shown one refresh, then two: each drawn as far along as
      // when it is shown, so the motion keeps its speed).
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
      frames->paced = true;
      frames->ownShown = !frames->fractions.empty() && frames->fractions.back() >= 0.999f;
      if (frames->ownShown) {
        frames->fractions.pop_back();
      }
      steps = uint32_t(frames->fractions.size());
      frames->slow = g.slow;
      frames->spanNs = span;
    } else if (g.slow && span) {
      // As many pictures as the usual rate gives in that time, or with the
      // display's refresh known, each the same whole number of refreshes
      // (two at 180 Hz for 15 a second: pictures held one, then three,
      // looked as choppy as before).
      double pictureNs = 1e9 / 60.0 / double(steps + 1);
      int64_t wanted = std::llround(double(span) / pictureNs);
      int64_t cap = std::min<int64_t>(kInterpMaxPictures, g_interpPictureCap.load());
      if (int64_t refresh = ModeRefreshNs()) {
        double refreshes = double(span) / double(refresh);
        int64_t per = std::max<int64_t>(1, int64_t(std::ceil(refreshes / double(cap) - 0.05)));
        wanted = std::max<int64_t>(1, std::llround(refreshes / double(per)));
        frames->spacingNs = per * refresh;
      }
      // (no more than the cap: the images a frame may hold, its own counted)
      steps = uint32_t(std::clamp<int64_t>(wanted, std::min<int64_t>(steps + 1, cap), cap) - 1);
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
    // A matched draw one of whose own registers (not the camera's) jumped
    // (taken as it is now while the rest went part of the way, its matrix
    // would be half one frame's, half the other's: a double): drawn where it
    // is now, seen from the camera part of the way, as a draw without a match.
    std::vector<int32_t> lerpMatch = match;
    uint64_t jumpedDraws = 0;
    // Only the moved registers take the value now, in every draw that has
    // that value (one light, the same in each pass); the matrices go on part
    // of the way, so a thing lit by a light sorted again stays in one piece.
    std::unordered_set<uint64_t> jumpedValues;
    for (size_t i = 0; i < g.current.size(); i++) {
      if (match[i] < 0 || matchDistance[i] == 0.0f) {
        continue;
      }
      auto& d = g.current[i];
      auto& before = g.previous[match[i]];
      std::vector<uint8_t> moved(d.registers.size(), 0), asNow;
      bool anyMoved = false;
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
      if (!anyMoved) {
        continue;
      }
      // A light is several registers (where, colour, reach): one found moved,
      // its run from it is taken as now, or one light's place would go with
      // another's reach (a flash on a wall, on the floor).
      InterpRunsFrom(moved, d.registers, asNow);
      for (size_t r = 0; r < d.registers.size(); r++) {
        if (!asNow[r]) {
          continue;
        }
        if (std::memcmp(&before.constants[r * 4], &d.constants[r * 4], 16) != 0) {
          jumpedValues.insert(sharedKey(d.registers[r], &d.constants[r * 4]));
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
    for (size_t i = 0; i < g.current.size(); i++) {
      if (match[i] < 0 || lerpMatch[i] >= 0) {
        continue;
      }
      auto& d = g.current[i];
      auto& before = g.previous[match[i]];
      for (size_t r = 0; r < d.registers.size(); r++) {
        if (r < skips[i].size() && skips[i][r]) {
          continue;
        }
        if (std::memcmp(&before.constants[r * 4], &d.constants[r * 4], 16) != 0) {
          jumpedValues.insert(valueAt(d.registers[r], &d.constants[r * 4]));
        }
      }
    }
    std::unordered_map<uint64_t, const float*> shared;
    for (size_t i = 0; i < g.current.size(); i++) {
      if (lerpMatch[i] < 0 || matchDistance[i] == 0.0f) {
        continue;
      }
      auto& d = g.current[i];
      auto& before = g.previous[lerpMatch[i]];
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
    std::unordered_map<uint64_t, std::vector<Common>> common;
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
      if (g.current[i].submitted && lerpMatch[i] < 0) {
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
    std::vector<std::vector<uint8_t>> psLerp(g.current.size());  // 1: part of the way
    uint64_t psDraws = 0;
    for (size_t i = 0; i < g.current.size(); i++) {
      auto& d = g.current[i];
      if (!d.submitted || lerpMatch[i] < 0 || d.psConstants.empty()) {
        continue;
      }
      auto& before = g.previous[lerpMatch[i]];
      if (before.psRegisters != d.psRegisters ||
          std::memcmp(before.psConstants.data(), d.psConstants.data(), d.psConstants.size() * 4) == 0) {
        continue;
      }
      std::vector<uint8_t> lerp(d.psRegisters.size(), 0), psMoved(d.psRegisters.size(), 0);
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
      std::vector<uint8_t> asNow;
      InterpRunsFrom(psMoved, d.psRegisters, asNow);
      for (size_t r = 0; r < d.psRegisters.size(); r++) {
        if (asNow[r]) {
          lerp[r] = 0;
        }
      }
      psLerp[i] = std::move(lerp);
      psDraws++;
    }
    g.psDraws += psDraws;
    std::vector<std::vector<const float*>> borrowed(g.current.size());
    std::vector<bool> notOwn(g.current.size());  // drawn without its own match's values
    // A thing the game didn't draw the frame before (some it leaves out of
    // a frame now and then; one come into view as the camera turned: a
    // chest, a corpse's sword): its own matrix (it times the camera) is new
    // while the camera's registers take the frame before's, and drawn
    // between it stood off its place, a jerk. Drawn the frame before that,
    // the frame before's value is taken as halfway between that one and now.
    std::vector<std::vector<float>> halfway(g.current.size());
    std::vector<bool> olderUsed(g.older.size());
    uint64_t fromOlder = 0;
    uint64_t borrowedDraws = 0;
    for (size_t i = 0; i < g.current.size(); i++) {
      auto& d = g.current[i];
      if (!d.submitted || d.ui) {
        continue;
      }
      const bool poisoned = false;  // (moved values are taken as now one by one, below)
      const InterpDraw* before = (!poisoned && lerpMatch[i] >= 0) ? &g.previous[lerpMatch[i]] : nullptr;
      notOwn[i] = !before;
      bool any = false;
      std::vector<const float*> xs(d.registers.size(), nullptr);
      if (!before && match[i] < 0 && d.mesh && !d.boneSize) {
        int32_t best = -1;
        float bestDistance = 0.6f;  // (two frames' move)
        for (uint32_t j = 0; j < g.older.size(); j++) {
          auto& o = g.older[j];
          if (olderUsed[j] || !o.submitted || o.mesh != d.mesh || o.group != d.group ||
              o.constants.size() != d.constants.size()) {
            continue;
          }
          float distance = InterpDistance(d, o, bestDistance, &matchSkips[i]);
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
        if (!jumpedValues.empty() && jumpedValues.count(valueAt(d.registers[r], y))) {
          continue;  // (moved: as now)
        }
        // Its own match's value first; another draw's (by the value now) only
        // for a draw without one, and not one that jumped there (that draw's
        // jump spread to every draw with the value: a spike across the
        // pictures between).
        auto it = before ? shared.end() : shared.find(valueAt(d.registers[r], y));
        if (it != shared.end()) {
          const float* other = it->second;
          float size = 1.0f, change = 0;
          for (uint32_t c = 0; c < 4; c++) {
            size = std::max({size, std::fabs(other[c]), std::fabs(y[c])});
            change = std::max(change, std::fabs(other[c] - y[c]));
          }
          if (!(change <= kInterpJump * size)) {
            it = shared.end();
          }
        }
        if (before) {
          x = &before->constants[r * 4];
        } else if (it != shared.end()) {
          x = it->second;
        } else if (auto c = common.find(d.group); c != common.end() && r < c->second.size()) {
          auto& one = c->second[r];
          if (one.nx >= 2 && one.ny >= 2 && one.xSame && one.ySame) {
            x = one.x;  // (the kind's own camera)
          }
        }
        if (x && std::memcmp(x, y, 16) != 0) {
          xs[r] = x;
          any = true;
        }
      }
      if (any) {
        borrowed[i] = std::move(xs);
        borrowedDraws += lerpMatch[i] < 0 || poisoned;
      }
    }
    g.fromOlder += fromOlder;
    g.borrowed += borrowedDraws;
    // Each picture on its own thread (they share nothing but the uploads,
    // taken under a lock): a crowd's 3 ms of constants in about one picture's
    // time. The first is this thread's, and counts the registers.
    std::mutex uploadLock;
    auto LockedAllocate = [&](uint64_t size) {
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
          auto& before = g.previous[lerpMatch[i]];
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
    g_interpPool.ForEach(steps, submitted >= 200 ? steps - 1 : 0, [&](size_t k) { picture(uint32_t(k)); });
    int64_t mixedNs = SteadyNs();
    g.constantsNs += mixedNs - matchedNs;
    // Bone palettes: as the draws that read them were matched.
    std::unordered_map<uint64_t, int32_t> previousIndex;
    for (int32_t i = 0; i < int32_t(g.previousPalettes.size()); i++) {
      auto& palette = g.previousPalettes[i];
      previousIndex.emplace((uint64_t(palette.address) << 32) | palette.size, i);
    }
    std::vector<int32_t> pairs(g.palettes.size(), -1);
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
    std::vector<PaletteJob> jobs(g.palettes.size());
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
                "bones {:.2f}; slowest frame {:.2f} ms; draws {:.0f} a frame, {} at most",
                double(g.matchNs) / 600e6, double(g.prepNs) / 600e6, double(g.meshNs) / 600e6,
                double(g.constantsNs) / 600e6, double(g.bonesNs) / 600e6, double(g.worstNs) / 1e6,
                double(g.drawsSeen) / 600.0, g.drawsWorst);
    g.matchNs = g.constantsNs = g.bonesNs = g.prepNs = g.meshNs = g.worstNs = 0;
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
    g.bonesBytes = g.palettesJumped = g.bonesSnapped = 0;
    g.buildNs = 0;
    g.frames = g.interpolated = g.draws = g.matched = g.snapped = g.registers = 0;
    g.bonesLerped = 0;
  }
  if (partial) {
    g.keptFor++;
    if (!frames) {
      frames = std::make_shared<InterpFrames>();
    }
    frames->partial = true;
    frames->slow = g.slow;
    // Paced by the display: not shown either (the last full frame's pictures
    // go on by the clock; shown at once it cut them short: a jump, near fire
    // every third frame, seen as a double).
    frames->paced = InterpRefreshNs() > 0 && InterpRefreshNs() < 16'000'000;
    g.current.clear();
    g.palettes.clear();
    g.paletteIndex.clear();
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
  g_interp.baseNs = g_interp.gridNs = g_interp.lastFullNs = g_interp.lastPresentNs = 0;
  g_interp.slowRun = 0;
  g_interp.slow = false;
  g_interpAutoDivider = 1;
  g_interpCapture.current.clear();
  g_interpCapture.palettes.clear();
  g_interpCapture.paletteIndex.clear();
  g_interpOn = on ? 1 : 0;
}

}  // namespace ng2::video
