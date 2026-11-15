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

#include <chrono>
#include <cstdint>
#include <thread>

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
