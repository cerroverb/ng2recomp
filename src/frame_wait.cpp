// ng2 - Ninja Gaiden II static recompilation
//
// Three of the game's busy-waits, which on the 360 had hardware threads to
// themselves and here hold whole cores: on a 4-core PC they take CPU time
// from the threads they wait for.
//
// The main loop (sub_822F34C0) hands each frame to the render thread, then
// spins until the render thread has taken it (lwz r11 of 0x84C53430 until
// it reads 0): nearly all of the main thread's time on a fast PC.
//
// The render thread (sub_822F16D0) spins the other way, until the main loop
// has handed it the next frame (0x84C53430, then 0x84C53434, set): nearly a
// whole core whenever the main loop is the slower one, the usual case on a
// busy 4-core PC, where it starved the audio threads (#39).
//
// The render thread's frame limiter (sub_822F0BF8) spins until the vblank
// count (0x84C55B50, from the vblank interrupt) is the word at 0x84C5342C
// past where it started, with the PowerPC low-priority hint in the loop.
//
// When the game asks for fewer frames than vblanks (the word at 0x84C5342C:
// 2 on the game over screen), the limiter reads the count (sub_822F02D8)
// once to start, then until it has moved on by that many. On the console the
// frame's work before it fits in a vblank, so it starts in the vblank the
// last wait ended in and every frame takes exactly 2. Here Present can wait
// on the host display's vsync, which isn't in step with the guest's vblanks,
// and push the start a vblank or two late: that frame took 3 or 4 (27 fps,
// not 30, uneven). A start up to the interval past the last wait's end
// counts from that end, so a frame takes the interval unless its own work
// took longer than that (where the console would also run late).
//
// All three back off: a few pause instructions, then yields, then short sleeps (yields on
// Windows, whose sleeps are whole milliseconds).
//
// Also here, as it is the same handoff:
//
// The poses: the render thread starts each frame by working out where the
// stage's animated parts are (sub_8247EA10: sliding doors and the like), and
// with no scripted scene running it reads their poses (sub_83751980) from the
// array the game's update tasks work in, not from a copy kept for the frame.
// Each tick one task puts those poses back to rest and a later one animates
// them again. On the console the render thread reads them as soon as the
// main loop starts it on the frame (sub_822F0CA8), with the tick's tasks
// done. Here it is often still waiting on the display with the last frame
// when the next tick's tasks run, and then read a pose put back to rest and
// not yet animated: the left leaf of Muramasa's door in Chapter 1 drawn
// 11 cm off, on and off from frame to frame (#21). The poses the render
// thread read in its last frame are now copied as the main loop starts it on
// the next, and its reads of them that frame (any made on the render thread
// with no scene running) get the copies. A part read for the first time still
// reads the array for a frame or two. NG2_POSE_SNAPSHOT=0 has it read the
// array again.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include <rex/hook.h>

#include "gpu/guest.h"
#include "gpu/video.h"

namespace {

// One step of a spin; a new wait (a gap since the last step) starts over.
void Backoff() {
  using clock = std::chrono::steady_clock;
  thread_local uint32_t spins = 0;
  thread_local clock::time_point last;
  auto now = clock::now();
  if (now - last > std::chrono::microseconds(500)) {
    spins = 0;
  }
  spins++;
  if (spins < 64) {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
  } else if (spins < 256) {
    std::this_thread::yield();
  } else {
#if defined(_WIN32)
    // MSVC's sleep_for rounds up to whole milliseconds (Sleep(1), 1-2 ms): too coarse here.
    std::this_thread::yield();
#else
    std::this_thread::sleep_for(std::chrono::microseconds(50));
#endif
  }
  last = clock::now();
}

}  // namespace

// Before the compare: r11 is the handoff word, 0 once the frame is taken.
void Ng2FrameHandoffWait(PPCRegister& r11) {
  if (r11.u32 != 0) {
    Backoff();
  } else {
    ng2::video::NoteFrameTaken();  // (NG2_LATENCY_LOG)
  }
}

// At each loop's quit-flag check, reached only while the word is still 0.
void Ng2FrameWait() {
  Backoff();
}

namespace {
constexpr uint32_t kBufferIndex = 0x84C3DA38;  // 0 or 1: the frame's side of what is kept twice
constexpr uint32_t kPoseOffsets = 0x83AA56C8 + 704;  // + buffer * 4: the pose array's place in an object
constexpr uint32_t kStaticBegin = 0x82000000, kStaticEnd = 0x85A90000;  // the game's image (always there)

struct PoseCopy {
  uint32_t address;
  uint8_t data[64];
};

bool PoseSnapshotOn() {
  static const bool on = [] {
    const char* v = std::getenv("NG2_POSE_SNAPSHOT");
    return !(v && v[0] == '0');
  }();
  return on;
}

// Between the two threads (held for a copy of a few kilobytes at most).
std::mutex g_poseMutex;
std::vector<uint32_t> g_poseWanted;     // the poses the render thread read last frame
std::vector<PoseCopy> g_posePending;    // as they were when the main loop last started a frame
bool g_posePendingNew = false;
// The render thread's own.
thread_local bool t_renderThread = false;
thread_local std::vector<PoseCopy> t_poses;  // by address
thread_local std::vector<uint32_t> t_poseRead;
}  // namespace

// The main loop, about to start the render thread on a frame.
REX_EXTERN(__imp__sub_822F0CA8);
REX_HOOK_RAW(sub_822F0CA8) {
  if (PoseSnapshotOn()) {
    std::lock_guard lock(g_poseMutex);
    g_posePending.resize(g_poseWanted.size());
    for (size_t i = 0; i < g_poseWanted.size(); i++) {
      g_posePending[i].address = g_poseWanted[i];
      std::memcpy(g_posePending[i].data, ng2::guest::Virtual(g_poseWanted[i]), 64);
    }
    g_posePendingNew = true;
  }
  __imp__sub_822F0CA8(ctx, base);
}

// The render thread, about to place the stage's parts: the copies made for
// this frame (or a later one, if the main loop is already there), and the
// list of what it read last frame for the next copies.
REX_EXTERN(__imp__sub_8247EA10);
REX_HOOK_RAW(sub_8247EA10) {
  if (PoseSnapshotOn()) {
    t_renderThread = true;
    std::sort(t_poseRead.begin(), t_poseRead.end());
    t_poseRead.erase(std::unique(t_poseRead.begin(), t_poseRead.end()), t_poseRead.end());
    {
      std::lock_guard lock(g_poseMutex);
      if (g_posePendingNew) {
        t_poses.swap(g_posePending);  // (sorted: made from a sorted list)
        g_posePendingNew = false;
      }
      g_poseWanted = t_poseRead;
    }
    t_poseRead.clear();
  }
  __imp__sub_8247EA10(ctx, base);
}

// A part's pose (r3: 64 bytes out, r4: its object's entry, r5: the part, r6:
// which of the entry's buffer bytes). The render thread asks with r6 = 0.
REX_EXTERN(__imp__sub_83751980);
REX_HOOK_RAW(sub_83751980) {
  if (t_renderThread && ctx.r6.u32 == 0) {
    uint32_t entry = ctx.r4.u32;
    uint8_t buffer = *ng2::guest::Virtual(entry + 17 + ng2::guest::Load32(kBufferIndex));
    if (buffer < 8) {  // (255: no pose, the original gives the identity)
      uint32_t address = ng2::guest::Load32(kPoseOffsets + buffer * 4) + ctx.r5.u32 * 64 +
                         ng2::guest::Load32(entry);
      if (address >= kStaticBegin && address < kStaticEnd && kStaticEnd - address >= 64) {
        t_poseRead.push_back(address);
        auto found = std::lower_bound(t_poses.begin(), t_poses.end(), address,
                                      [](const PoseCopy& pose, uint32_t a) { return pose.address < a; });
        if (found != t_poses.end() && found->address == address) {
          std::memcpy(ng2::guest::Virtual(ctx.r3.u32), found->data, 64);
          return;
        }
      }
    }
  }
  __imp__sub_83751980(ctx, base);
}

void Ng2VblankWait() {
  Backoff();
}

// The limiter's reads of the vblank count: its start (returning to 822F0C40)
// and each check in its loop (to 822F0C4C).
REX_EXTERN(__imp__sub_822F02D8);
REX_HOOK_RAW(sub_822F02D8) {
  constexpr uint32_t kStart = 0x822F0C40, kCheck = 0x822F0C4C, kInterval = 0x84C5342C;
  static uint32_t lastEnd = 0;
  static bool waited = false;
  uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_822F02D8(ctx, base);
  if (lr == kStart) {
    // Up to the interval since the last wait ended: blocked, not slow.
    uint32_t interval = ng2::guest::Load32(kInterval);
    if (waited && ctx.r3.u32 - lastEnd <= interval) {
      ctx.r3.u32 = lastEnd;
    }
  } else if (lr == kCheck) {
    lastEnd = ctx.r3.u32;  // the last check is the one the wait ended on
    waited = true;
  }
}
