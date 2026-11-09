// ng2 - Ninja Gaiden II static recompilation
//
// Hooks on the game's D3D library (XDK 2.0.6683; addresses and signatures in
// docs/d3d-map-*.md). With the native renderer the original still runs, so
// the D3D library's bookkeeping stays intact; the renderer reads the device
// state at these points, and native_gpu.cpp does the GPU side the game can see
// (fences, callbacks, the flip). Otherwise they only call the original.

#include <algorithm>
#include <climits>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include <mutex>
#include <string>
#include <unordered_set>
#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/logging.h>

#include "guest.h"
#include "native_gpu.h"
#include "video.h"

namespace {

struct Counters {
  std::atomic<uint32_t> draws{0};
  std::atomic<uint32_t> clears{0};
  std::atomic<uint32_t> resolves{0};
  std::atomic<uint32_t> swaps{0};
};
Counters g_counters;
std::chrono::steady_clock::time_point g_lastReport = std::chrono::steady_clock::now();

// NG2_D3D_STATS=1: log D3D call counts once a second.
bool StatsEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("NG2_D3D_STATS");
    return v && v[0] == '1';
  }();
  return enabled;
}

void ReportStats() {
  auto now = std::chrono::steady_clock::now();
  if (now - g_lastReport < std::chrono::seconds(1)) {
    return;
  }
  g_lastReport = now;
  REXLOG_INFO("d3d: {} swaps, {} draws, {} clears, {} resolves in the last second",
              g_counters.swaps.exchange(0), g_counters.draws.exchange(0),
              g_counters.clears.exchange(0), g_counters.resolves.exchange(0));
}


// NG2_DRAW_STACKS=1: each distinct chain of guest callers that leads to a
// draw, logged once (return addresses: the caller's, then up the stack's
// back chain, where each function saves its LR 8 bytes below its caller's
// frame). Finds what draws a picture the 2D pass doesn't.
void LogDrawStack(PPCContext& ctx, const char* kind) {
  static const bool on = std::getenv("NG2_DRAW_STACKS") != nullptr;
  // NG2_TRACE_STACKS=1: the chain goes on each draw's trace line too.
  static const bool traced = std::getenv("NG2_TRACE_STACKS") != nullptr;
  if (!on && !traced) {
    return;
  }
  static std::mutex mutex;
  static std::unordered_set<std::string> seen;
  std::string key = fmt::format("{:08X}", uint32_t(ctx.lr));
  auto* memory = rex::Runtime::instance()->memory();
  auto readable = [&](uint32_t address) {
    uint32_t protect = 0;
    auto* heap = memory->LookupHeap(address);
    return heap && heap->QueryProtect(address, &protect) &&
           (protect & rex::memory::kMemoryProtectRead);
  };
  uint32_t sp = ctx.r1.u32;
  for (int depth = 0; depth < 10 && sp && readable(sp); depth++) {
    uint32_t next = ng2::guest::Load32(sp);
    if (!next || next <= sp || next - sp > 0x100000 || !readable(next - 8)) {
      break;
    }
    key += fmt::format(" {:08X}", ng2::guest::Load32(next - 8));
    sp = next;
  }
  if (traced) {
    ng2::video::NoteDrawStack(key);
  }
  if (!on) {
    return;
  }
  std::lock_guard lock(mutex);
  if (seen.insert(key).second) {
    REXLOG_INFO("draw stack {}: {}", kind, key);
  }
}

}  // namespace

// DrawVertices(dev, prim type, start vertex, vertex count)
REX_EXTERN(__imp__sub_83761EA8);
REX_HOOK_RAW(sub_83761EA8) {
  g_counters.draws++;
  LogDrawStack(ctx, "vertices");
  uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  __imp__sub_83761EA8(ctx, base);
  if (ng2::video::IsNativeRenderer()) {
    ng2::video::OnDraw(device, primitive, start, count, false, 0);
  }
}

// DrawIndexedVertices(dev, prim type, base vertex, start index, index count)
REX_EXTERN(__imp__sub_83762290);
REX_HOOK_RAW(sub_83762290) {
  g_counters.draws++;
  LogDrawStack(ctx, "indexed");
  uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32, baseVertex = ctx.r5.u32;
  uint32_t startIndex = ctx.r6.u32, count = ctx.r7.u32;
  __imp__sub_83762290(ctx, base);
  if (ng2::video::IsNativeRenderer()) {
    ng2::video::OnDraw(device, primitive, baseVertex, count, true, startIndex);
  }
}

// BeginVertices(dev, prim type, vertex count, stride) -> vertex data
REX_EXTERN(__imp__sub_837614D8);
REX_HOOK_RAW(sub_837614D8) {
  g_counters.draws++;
  LogDrawStack(ctx, "begin");
  uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  __imp__sub_837614D8(ctx, base);
  if (ng2::video::IsNativeRenderer()) {
    ng2::video::OnBeginVertices(device, primitive, count, stride, ctx.r3.u32);
  }
}

// BeginIndexedVertices(dev, prim type, base vertex, vertex count, index
// count, index format, stride, &indices, [stack] &vertices)
REX_EXTERN(__imp__sub_83761978);
REX_HOOK_RAW(sub_83761978) {
  g_counters.draws++;
  LogDrawStack(ctx, "begin-indexed");
  uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32, baseVertex = ctx.r5.u32;
  uint32_t vertexCount = ctx.r6.u32, indexCount = ctx.r7.u32, indexFormat = ctx.r8.u32;
  uint32_t stride = ctx.r9.u32, indexDataOut = ctx.r10.u32;
  uint32_t vertexDataOut = ng2::guest::Load32(ctx.r1.u32 + 84);  // 9th argument
  __imp__sub_83761978(ctx, base);
  if (ng2::video::IsNativeRenderer() && ctx.r3.s32 >= 0 && indexDataOut && vertexDataOut) {
    ng2::video::OnBeginIndexedVertices(device, primitive, baseVertex, vertexCount, indexCount,
                                       (indexFormat & 4) != 0, stride,
                                       ng2::guest::Load32(indexDataOut),
                                       ng2::guest::Load32(vertexDataOut));
  }
}

// CreateVertexShader(const DWORD* function) -> D3DVertexShader*
REX_EXTERN(__imp__sub_83760F18);
REX_HOOK_RAW(sub_83760F18) {
  uint32_t container = ctx.r3.u32;
  __imp__sub_83760F18(ctx, base);
  if (ng2::video::IsNativeRenderer()) {
    ng2::video::OnCreateShader(ctx.r3.u32, container, false);
  }
}

// CreatePixelShader(const DWORD* function) -> D3DPixelShader*
REX_EXTERN(__imp__sub_83760B50);
REX_HOOK_RAW(sub_83760B50) {
  uint32_t container = ctx.r3.u32;
  __imp__sub_83760B50(ctx, base);
  if (ng2::video::IsNativeRenderer()) {
    ng2::video::OnCreateShader(ctx.r3.u32, container, true);
  }
}

// Clear(dev, count, rects, flags, color, f1 z, stencil)
REX_EXTERN(__imp__sub_8375E2E0);
REX_HOOK_RAW(sub_8375E2E0) {
  g_counters.clears++;
  // The float takes r8's slot, so stencil comes in r9.
  uint32_t device = ctx.r3.u32, count = ctx.r4.u32, rects = ctx.r5.u32, flags = ctx.r6.u32;
  uint32_t color = ctx.r7.u32, stencil = ctx.r9.u32;
  float z = float(ctx.f1.f64);
  // The game builds the rects below its stack pointer, where the call itself
  // writes: copy them first.
  int32_t rectData[ng2::video::kMaxClearRects][4];
  if (!rects) {
    count = 0;
  } else if (count > ng2::video::kMaxClearRects) {
    // More than we keep: their bounding box (more cleared, nothing left stale).
    int32_t box[4] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (uint32_t i = 0; i < count; i++) {
      box[0] = std::min(box[0], ng2::guest::LoadS32(rects + i * 16));
      box[1] = std::min(box[1], ng2::guest::LoadS32(rects + i * 16 + 4));
      box[2] = std::max(box[2], ng2::guest::LoadS32(rects + i * 16 + 8));
      box[3] = std::max(box[3], ng2::guest::LoadS32(rects + i * 16 + 12));
    }
    std::copy(box, box + 4, rectData[0]);
    count = 1;
  } else {
    for (uint32_t i = 0; i < count; i++) {
      for (uint32_t j = 0; j < 4; j++) {
        rectData[i][j] = ng2::guest::LoadS32(rects + i * 16 + j * 4);
      }
    }
  }
  __imp__sub_8375E2E0(ctx, base);
  if (ng2::video::IsNativeRenderer()) {
    ng2::video::OnClear(device, count, rectData, flags, color, z, stencil);
  }
}

// Resolve(dev, flags, src rect, dest texture, dest point, level, slice,
// clear color, f1 clear z, [stack] stencil, params)
REX_EXTERN(__imp__sub_83762CF8);
REX_HOOK_RAW(sub_83762CF8) {
  g_counters.resolves++;
  uint32_t device = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, texture = ctx.r6.u32;
  uint32_t point = ctx.r7.u32, clearColor = ctx.r10.u32;
  float clearZ = float(ctx.f1.f64);
  // 10th argument on the caller's stack (the float takes the 9th slot).
  uint32_t clearStencil = ng2::guest::Load32(ctx.r1.u32 + 92);
  __imp__sub_83762CF8(ctx, base);
  if (ng2::video::IsNativeRenderer()) {
    ng2::video::OnResolve(device, flags, rect, texture, point, clearColor, clearZ, clearStencil);
  }
}

// InsertFence(dev, command buffer) -> command buffer
REX_EXTERN(__imp__sub_8375F810);
REX_HOOK_RAW(sub_8375F810) {
  if (ng2::video::IsNativeRenderer()) {
    ng2::gpu::OnInsertFence(ctx.r3.u32);
  }
  __imp__sub_8375F810(ctx, base);
}

// The callback handshake(dev, command buffer, flags, callback, argument) ->
// command buffer: a CPU callback when the GPU gets there.
REX_EXTERN(__imp__sub_8375FD30);
REX_HOOK_RAW(sub_8375FD30) {
  if (ng2::video::IsNativeRenderer()) {
    ng2::gpu::OnCallbackHandshake(ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  }
  __imp__sub_8375FD30(ctx, base);
}

// The flip(dev, no wait): its handshake (the swap callback) comes from inside.
REX_EXTERN(__imp__sub_8376A518);
REX_HOOK_RAW(sub_8376A518) {
  uint32_t device = ctx.r3.u32, noWait = ctx.r4.u32;
  bool native = ng2::video::IsNativeRenderer();
  if (native) {
    ng2::gpu::OnFlipBegin(device, noWait);
  }
  __imp__sub_8376A518(ctx, base);
  if (native) {
    ng2::gpu::OnFlipWait(device, noWait);
  }
}

// D3D's writers of dirty constants into its command stream (r3 device; the
// registers by mask from r6): runs of them copied as packets, the stream's
// write pointer (device +48) moved on. With no GPU emulation nothing reads the
// stream and the renderer reads the device's own copies: skipped.
REX_EXTERN(__imp__sub_83767670);
REX_HOOK_RAW(sub_83767670) {
  if (!ng2::video::IsNativeRenderer()) {
    __imp__sub_83767670(ctx, base);
  }
}
REX_EXTERN(__imp__sub_83767A10);
REX_HOOK_RAW(sub_83767A10) {
  if (!ng2::video::IsNativeRenderer()) {
    __imp__sub_83767A10(ctx, base);
  }
}

// Swap: the frame is done.
//
// sub_8376E028, a D3D loop the game runs on another thread while it waits
// (the Game Over screen: there the game makes a frame every 2 vblanks), swaps
// the last front buffer again each time round (returning to 8376E0C4). On the
// console that shows the same picture; here each is a present of its own, on
// a clock of its own between the game's frames: frame times of 3 ms, then 30,
// and with V-Sync On refreshes taken from the game's frames (#13). Those are
// left to the guest while the game is presenting its own frames, and only
// reach the screen after a wait of over 100 ms without one.
REX_EXTERN(__imp__sub_8376AA48);
REX_HOOK_RAW(sub_8376AA48) {
  constexpr uint32_t kKeepAlive = 0x8376E0C4;
  static std::atomic<int64_t> lastGameSwapNs{0};
  g_counters.swaps++;
  uint32_t device = ctx.r3.u32, frontBuffer = ctx.r4.u32;
  int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
  bool repeat = uint32_t(ctx.lr) == kKeepAlive &&
                now - lastGameSwapNs.load(std::memory_order_relaxed) < 100'000'000;
  if (uint32_t(ctx.lr) != kKeepAlive) {
    lastGameSwapNs.store(now, std::memory_order_relaxed);
  }
  __imp__sub_8376AA48(ctx, base);
  if (ng2::video::IsNativeRenderer() && !repeat) {
    ng2::video::OnSwap(device, frontBuffer);
  }
  if (StatsEnabled()) {
    ReportStats();
  }
}

// The D3D library's waits for the GPU: each fills a wait block (with the
// time base at +20 when it starts) and calls sub_8375E588 until the GPU's
// read pointer passes a fence (1: still waiting). The console spins here; on
// a PC that's a whole core busy until the next vblank, so once a wait has
// spun a while each step sleeps a little. The count starts again with each
// wait, told apart by that time stamp (they end on the read pointer, not on
// a step's 0).
REX_EXTERN(__imp__sub_8375E588);
REX_HOOK_RAW(sub_8375E588) {
  thread_local uint32_t spins = 0, lastStamp = 0;
  uint32_t stamp = ng2::guest::Load32(ctx.r3.u32 + 20);
  if (stamp != lastStamp) {
    lastStamp = stamp;
    spins = 0;
  }
  __imp__sub_8375E588(ctx, base);
  if (ctx.r3.u32 != 0 && ++spins > 64) {
#if defined(_WIN32)
    std::this_thread::yield();  // a sleep there is a millisecond or more
#else
    std::this_thread::sleep_for(std::chrono::microseconds(100));
#endif
  }
}

// A wait at the barrier over 2 ms, logged when it happens (at most one line a
// second; the ones in between are counted into the next line): a thread kept
// waiting that long is a pass the sound can't afford (one is ~5 ms of audio).
static void NoteBarrierWait(std::chrono::steady_clock::duration waited) {
  using namespace std::chrono;
  static std::atomic<uint32_t> skipped{0};
  static std::atomic<int64_t> last_report_ns{0};
  const double ms = duration<double, std::milli>(waited).count();
  if (ms < 2.0) {
    return;
  }
  const int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  int64_t last = last_report_ns.load();
  if (now - last >= 1'000'000'000 && last_report_ns.compare_exchange_strong(last, now)) {
    REXLOG_WARN("audio barrier: waited {:.1f} ms ({} more over 2 ms since the last line)", ms,
                skipped.exchange(0));
  } else {
    skipped++;
  }
}

// The audio engine's barrier (XAudio2's voice processing, sub_83774800): each
// thread taking part sets its processor's byte in an 8-byte mask and waits
// until the mask holds all of them; the first to see that cleared it, and a
// waiting thread only left on seeing the mask whole or cleared. If the OS
// paused it right then, the other thread could clear the mask and set its
// byte again for the next pass first, leaving both waiting for good (the
// sound stops; on the console the hardware threads aren't paused). Here only
// the lowest-numbered thread taking part clears the mask, a byte at a time and
// its own last, and the others leave once their own byte is cleared: a byte is
// only cleared by a completed barrier and only set again by its own thread
// after that. Byte accesses only: the mask needn't be 8-byte aligned, and an
// 8-byte lock across a cache line would be a split lock.
REX_EXTERN(__imp__sub_83774800);
REX_HOOK_RAW(sub_83774800) {
  // NG2_GAME_AUDIO_BARRIER=1: the game's own barrier (for comparisons).
  static const bool original = [] {
    const char* v = std::getenv("NG2_GAME_AUDIO_BARRIER");
    return v && v[0] == '1';
  }();
  if (original) {
    __imp__sub_83774800(ctx, base);
    return;
  }
  uint32_t engine = ctx.r3.u32;
  if (ng2::guest::Load32(engine + 0x130) == 0) {
    return;
  }
  uint8_t cpu = *ng2::guest::Virtual(ctx.r13.u32 + 0x10C);
  if (cpu >= 6) {
    __imp__sub_83774800(ctx, base);  // not a processor that takes part
    return;
  }
  uint8_t* mask = ng2::guest::Virtual(ctx.r4.u32);
  auto load = [&](uint32_t i) { return __atomic_load_n(mask + i, __ATOMIC_ACQUIRE); };
  auto store = [&](uint32_t i, uint8_t v) { __atomic_store_n(mask + i, v, __ATOMIC_SEQ_CST); };
  // Who takes part, read once as the game does: one byte per processor.
  bool expected[6];
  int clearer = -1;
  for (uint32_t i = 0; i < 6; i++) {
    expected[i] = ng2::guest::Load32(engine + 0x134 + 4 * i) != 0;
    if (expected[i] && clearer < 0) {
      clearer = int(i);
    }
  }
  store(cpu, 1);
  // Timed from the first yield on (a few hundred microseconds in): a normal
  // pass never gets there.
  std::chrono::steady_clock::time_point slow_since{};
  for (uint32_t spins = 0;; spins++) {
    if (load(cpu) == 0) {
      if (spins > 4096) {
        NoteBarrierWait(std::chrono::steady_clock::now() - slow_since);
      }
      return;  // completed
    }
    if (int(cpu) == clearer) {
      bool whole = true;
      for (uint32_t i = 0; i < 6; i++) {
        whole &= (load(i) != 0) == expected[i];
      }
      if (whole) {
        for (uint32_t i = 0; i < 8; i++) {
          if (i != cpu) {
            store(i, 0);
          }
        }
        store(cpu, 0);
        if (spins > 4096) {
          NoteBarrierWait(std::chrono::steady_clock::now() - slow_since);
        }
        return;
      }
    }
    if (spins == 4096) {
      slow_since = std::chrono::steady_clock::now();
    }
    if (spins >= 4096) {
      std::this_thread::yield();
    } else {
#if defined(__x86_64__) || defined(_M_X64)
      __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
      __builtin_arm_yield();
#endif
    }
  }
}
