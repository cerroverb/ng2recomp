// ng2 - Ninja Gaiden II static recompilation
//
// The game's GPU without GPU emulation. The native renderer draws from the
// game's Direct3D calls (video.cpp); the rest of what Direct3D asks of a GPU
// is done here, at the Direct3D calls that ask for it, so nothing reads the
// command stream Direct3D writes:
//
// - The command stream: Direct3D still writes and kicks it (CP_RB_WPTR); a
//   kick reports it consumed at once (the read pointer write-back Direct3D
//   checks for room). No packet is looked at.
// - What the hardware would do with the stream that the game can see, as
//   steps in call order on a guest thread of our own (interrupts call into
//   the game), each as soon as its call is made: the renderer draws from
//   copies taken at the calls, so the work before a step is always done.
//     InsertFence (8375F810)          the status word to the fence block's
//                                     +4, then the fence counter to +0
//     the callback handshake          scratch 4 / 5 = callback / argument,
//       (8375FD30)                    scratch 0 = the CPU mask, the
//                                     interrupt (the game's handler runs the
//                                     callback and clears scratch 0); then
//                                     waited back to 0, scratch 4 poisoned
//     the flip (8376A518)             scratch 1 (flip pending) = 1; after its
//                                     handshake (the swap callback 8376A380)
//                                     waited back to 0, which the vblank
//                                     handler does once the flip is due: the
//                                     vsync wait. The steps after it wait too.
//   The scratch registers are mirrored at the block device+10900 points to
//   (scratch n at +4n), the fence block at device+10896.
// - Vblanks: the game's interrupt callback at the video mode's rate.
// - What the stream also holds needs nothing here: waits on GPU registers
//   (cache coherency), cache flush events, the gamma ramp (video.cpp reads
//   the device's copy), XE_SWAP (the Swap hook presents), draws and state.
//   A census of every packet type over boot and Chapter 1's first fight found
//   no other memory write and no occlusion query. Not done: the GPU counter
//   timestamps (EVENT_WRITE_SHD initiator bit 31) Swap writes when
//   device+21608 is set and sub_83771778 in device mode 2; neither happened.
// Each step was checked against the packets TU3's functions write.

#include "win_display.h"
#include "native_gpu.h"
#include "video.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

REXCVAR_DECLARE(bool, vsync);
REXCVAR_DECLARE(int32_t, present_mode);
// Frame Pacing: Auto puts the guest's vblanks on the display's own refreshes
// (video.cpp's display clock: one every N of them, just after one) when N refreshes come within 2% of the video mode's rate, so no
// frame is shown twice for the drift between the two clocks; else, and with
// Timer, they come from a timer at the video mode's rate.
REXCVAR_DEFINE_INT32(frame_pacing, 1, "Video", "0: auto (on the display's refreshes), 1: timer (the game's)");
// Where in a refresh a locked vblank falls (% of it after the refresh): just
// after it, the frame has nearly the whole refresh to be drawn and shown at
// the next one (at 50, half: a slower frame misses it, waits a refresh).
REXCVAR_DEFINE_INT32(frame_pacing_phase, 5, "Video", "Auto frame pacing: % of a refresh after it");

namespace ng2::gpu {
namespace {

using rex::X_STATUS;

constexpr uint32_t kCpRbWptr = 0x01C5;

// Device fields (big-endian words).
constexpr uint32_t kDevFenceBlock = 10896;    // pointer: +0 fence counter, +4 status word
constexpr uint32_t kDevScratchBlock = 10900;  // pointer: scratch n at +4n
constexpr uint32_t kDevFenceCounter = 10908;  // the fence InsertFence writes
constexpr uint32_t kDevStatusBits = 14904;    // (& 3) | device+48: the status word
constexpr uint32_t kDevStatusBase = 48;

class NativeGraphicsSystem;
std::atomic<NativeGraphicsSystem*> g_system{nullptr};

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  ~NativeGraphicsSystem() override { Shutdown(); }

  // The renderer owns the window and presents.
  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override {
    return X_STATUS_SUCCESS;
  }

  rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                              rex::system::KernelState* kernel) override {
    dispatcher_ = dispatcher;
    kernel_ = kernel;
    memory_ = dispatcher->memory();
    // The GPU registers: 0x7FC80000-0x7FCFFFFF.
    memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this, &ReadRegisterThunk,
                                   &WriteRegisterThunk);
    running_ = true;
    g_system = this;
    effect_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_, 128 * 1024, 0, [this] { return EffectThread(); }));
    effect_thread_->set_name("GPU Steps");
    effect_thread_->Create();
    vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_, 128 * 1024, 0, [this] { return VsyncThread(); }));
    vsync_thread_->set_name("GPU VSync");
    vsync_thread_->Create();
    REXLOG_INFO("gpu: no GPU emulation (Direct3D's GPU side at its calls)");
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return false; }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    interrupt_callback_ = callback;
    interrupt_data_ = user_data;
  }

  void InitializeRingBuffer(uint32_t, uint32_t) override {}

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t) override {
    rptr_writeback_.store(ptr, std::memory_order_release);
  }

  // Stops both threads and waits for them (the runtime frees this right after).
  void Shutdown() override {
    {
      std::lock_guard lock(steps_mutex_);
      if (!running_.exchange(false)) {
        return;
      }
    }
    g_system = nullptr;
    steps_cv_.notify_all();
    WakeWaits();
    for (auto* thread : {&effect_thread_, &vsync_thread_}) {
      if (*thread) {
        (*thread)->Wait(0, 0, 0, nullptr);
        thread->reset();
      }
    }
  }

  // --- The Direct3D calls ------------------------------------------------------

  void InsertFence(uint32_t device) {
    uint32_t block = Field(device, kDevFenceBlock);
    uint32_t counter = Field(device, kDevFenceCounter);
    uint32_t word = (Field(device, kDevStatusBits) & 3) | Field(device, kDevStatusBase);
    Step([this, block, word, counter] {
      Write32(block + 4, word);
      Write32(block, counter);
    });
  }

  // flags bit 1: the callback's interrupt; bit 2: wait for its handler, then
  // poison scratch 4; neither: both. CPU mask in flags bits 24-29 (none: CPU 2).
  void CallbackHandshake(uint32_t device, uint32_t flags, uint32_t callback, uint32_t argument) {
    if (!(flags & 6)) {
      flags |= 6;
    }
    uint32_t scratch = Field(device, kDevScratchBlock);
    uint32_t cpus = (flags >> 24) & 0x3F;
    if (!cpus) {
      cpus = 4;
    }
    Step([this, flags, scratch, callback, argument, cpus] {
      if (flags & 2) {
        Write32(scratch + 16, callback);
        Write32(scratch + 20, argument);
        Write32(scratch, cpus);
        for (uint32_t cpu = 0; cpu < 6; cpu++) {
          if (cpus & (1u << cpu)) {
            DispatchInterrupt(1, cpu);
          }
        }
      }
      if (flags & 4) {
        WaitWord(scratch, 0);
        Write32(scratch + 16, 0x0BADF00Du);
      }
    });
  }

  void FlipBegin(uint32_t device, uint32_t noWait) {
    if (noWait) {
      return;
    }
    uint32_t scratch = Field(device, kDevScratchBlock);
    Step([this, scratch] { Write32(scratch + 4, 1); });
  }

  void FlipWait(uint32_t device, uint32_t noWait) {
    if (noWait) {
      return;
    }
    uint32_t scratch = Field(device, kDevScratchBlock);
    Step([this, scratch] { WaitWord(scratch + 4, 0); });
  }

 private:
  // --- Registers ---------------------------------------------------------------

  static uint32_t ReadRegisterThunk(void*, void* context, uint32_t addr) {
    return static_cast<NativeGraphicsSystem*>(context)->ReadRegister(addr);
  }
  static void WriteRegisterThunk(void*, void* context, uint32_t addr, uint32_t value) {
    static_cast<NativeGraphicsSystem*>(context)->WriteRegister(addr, value);
  }

  // The answers the SDK's GraphicsSystem gives; the game reads the interrupt status.
  uint32_t ReadRegister(uint32_t addr) {
    uint32_t r = (addr & 0xFFFF) / 4;
    rex::system::X_VIDEO_MODE mode;
    switch (r) {
      case 0x0F00:  // RB_EDRAM_TIMING
        return 0x08100748;
      case 0x0F01:  // RB_BC_CONTROL
        return 0x0000200E;
      case 0x194C:  // R500_D1MODE_V_COUNTER
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), 0x0FFFu);
      case 0x1951:  // interrupt status: vblank
        return 1;
      case 0x1961:  // AVIVO_D1MODE_VIEWPORT_SIZE
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_width), 0x0FFFu) << 16 |
               std::min(uint32_t(mode.display_height), 0x0FFFu);
      default:
        return 0;
    }
  }

  // The kick: consumed as soon as written.
  void WriteRegister(uint32_t addr, uint32_t value) {
    if ((addr & 0xFFFF) / 4 != kCpRbWptr) {
      return;
    }
    if (uint32_t writeback = rptr_writeback_.load(std::memory_order_acquire)) {
      rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), value);
    }
  }

  // --- Steps ---------------------------------------------------------------------

  uint32_t Field(uint32_t device, uint32_t offset) const {
    return rex::memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(device + offset));
  }

  void Write32(uint32_t address, uint32_t value) {
    if (auto* p = memory_->TranslateVirtual<uint8_t*>(address)) {
      rex::memory::store_and_swap<uint32_t>(p, value);
    }
  }

  // A step runs once the draw worker (if on) has drawn the calls before it.
  void Step(std::function<void()> step) {
    uint64_t after = ng2::video::RecordedCalls();
    {
      std::lock_guard lock(steps_mutex_);
      steps_.push_back({after, std::move(step)});
    }
    steps_cv_.notify_one();
  }

  int EffectThread() {
    while (running_) {
      PendingStep step;
      {
        std::unique_lock lock(steps_mutex_);
        steps_cv_.wait(lock, [&] { return !steps_.empty() || !running_; });
        if (!running_) {
          break;
        }
        step = std::move(steps_.front());
        steps_.pop_front();
      }
      ng2::video::WaitForRecordedCalls(step.after);
      step.run();
    }
    return 0;
  }

  // Until a guest word reads `value`; the steps after it wait too. Looked at
  // again at each vblank (its handler is what clears the words waited on) and
  // every 100 us. At most 250 ms: a word nothing sets must not hold the game
  // for ever (logged).
  void WaitWord(uint32_t address, uint32_t value) {
    auto read = [&] {
      return rex::memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(address));
    };
    if (read() == value) {
      return;
    }
    auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(waits_mutex_);
    while (running_ && read() != value) {
      uint64_t seen = wakes_;
      waits_cv_.wait_for(lock, std::chrono::microseconds(100),
                         [&] { return wakes_ != seen || !running_; });
      if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(250)) {
        if (read() != value) {
          REXLOG_WARN("gpu: a wait gave up after 250 ms: {:08X} reads {:08X}, not {:08X}", address,
                      read(), value);
        }
        break;
      }
    }
  }

  void WakeWaits() {
    {
      std::lock_guard lock(waits_mutex_);
      wakes_++;
    }
    waits_cv_.notify_all();
  }

  // The game's graphics interrupt callback on the calling guest thread. One at
  // a time, as on the console (both kinds go to CPU 2): the swap callback fills
  // the flip queue the vblank handler drains.
  void DispatchInterrupt(uint32_t source, uint32_t cpu) {
    if (!interrupt_callback_) {
      return;
    }
    std::lock_guard lock(interrupt_mutex_);
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) {
      return;
    }
    thread->SetActiveCpu(cpu);
    uint64_t args[] = {source, interrupt_data_};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args, 2);
  }

  // --- Vblanks -------------------------------------------------------------------

  // A sleep to a point, its last part spun (yielding): what a sleep may
  // overshoot by (Windows' sleeps are whole milliseconds).
  static void SleepNs(int64_t ns) {
    // Asleep, then the last 0.3 ms awake (Windows: a high-resolution timer,
    // where a plain sleep wakes on the scheduler's tick).
    // (1.5 ms awake where the sleep isn't precise: Windows before 10 1803)
    static thread_local const int64_t kSpin = ng2::video::PreciseSleepAvailable() ? 300'000 : 1'500'000;
    int64_t end = ng2::video::DisplayClockNow() + ns;
    if (ns > kSpin) {
      ng2::video::PreciseSleepNs(ns - kSpin);
    }
    while (ng2::video::DisplayClockNow() < end) {
      std::this_thread::yield();
    }
  }

  // The video mode's rate (1 kHz with vsync off, as the SDK's worker did).
  int VsyncThread() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    double hz = std::max(1.0, double(float(mode.refresh_rate)));
    uint64_t frequency = rex::chrono::Clock::guest_tick_frequency();
    uint64_t vsyncInterval = std::max<uint64_t>(1, uint64_t(double(frequency) / hz));
    uint64_t freeInterval = std::max<uint64_t>(1, frequency / 1000);
    uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
    const int64_t guestPeriodNs = int64_t(1e9 / hz);
    int64_t lastVblankNs = 0;
    int64_t lastTargetNs = 0;  // the locked vblanks' last target (0: unlocked since)
    bool lockedLogged = false;
    int64_t lastDrainNs = 0, drainPausedUntil = 0, drainPauseNs = 30'000'000'000;
    int drainsLogged = 0, drainsQuick = 0;
    bool skipNext = false;
    while (running_) {
      int64_t refreshNs = 0, periodNs = 0;
      // (FIFO, Mailbox and Adaptive: there the presents reach the screen on
      // its refreshes, Adaptive's when on time, as they are once locked; with
      // V-Sync Off they show at once. Adaptive left out, its vblanks ran on the
      // timer and drifted against the screen: choppy until a V-Sync toggle
      // happened to line them up again, #36)
      // NG2_TEST_GAMESCOPE_AUTO=1 (#46): Auto under gamescope too, where it
      // otherwise acts as Timer.
      static const bool gamescopeAuto = [] {
        const char* v = std::getenv("NG2_TEST_GAMESCOPE_AUTO");
        bool on = v && v[0] == '1';
        if (on) {
          REXLOG_INFO("video: test switch NG2_TEST_GAMESCOPE_AUTO=1");
        }
        return on;
      }();
      int32_t presentMode = REXCVAR_GET(present_mode);
      if (REXCVAR_GET(vsync) && REXCVAR_GET(frame_pacing) == 0 &&
          (!ng2::video::UnderGamescope() || gamescopeAuto) &&
          (presentMode == 1 || presentMode == 2 || presentMode == 3) &&
          ng2::video::DisplayRefresh(&refreshNs, &periodNs)) {
        int64_t every = std::clamp<int64_t>((guestPeriodNs + periodNs / 2) / periodNs, 1, 4);
        int64_t step = periodNs * every;
        if (std::abs(step - guestPeriodNs) * 50 < guestPeriodNs) {
          if (!lockedLogged) {
            lockedLogged = true;
            REXLOG_INFO("gpu: vblanks on the display's refreshes ({:.3f} Hz, every {})",
                        1e9 / double(periodNs), every);
          }
          // frame_pacing_phase after a refresh: the first refresh at least a
          // step, less half a refresh, after the last vblank (no two close
          // together when the lock starts). Refresh by refresh, so on a 120 Hz
          // panel (every 2) the vblanks keep to every second refresh whichever
          // refresh the clock last reported.
          int64_t phase = std::clamp<int32_t>(REXCVAR_GET(frame_pacing_phase), 0, 50);
          int64_t target = refreshNs + periodNs * phase / 100;
          // From the last target, not the wake-up after it: an overslept
          // wake-up would push this one a refresh late.
          int64_t after = lastTargetNs ? lastTargetNs : lastVblankNs;
          while (target < after + step - periodNs / 2) {
            target += periodNs;
          }
          lastTargetNs = target;
          int64_t wait = target - ng2::video::DisplayClockNow();
          if (wait > 0) {
            SleepNs(wait);
          }
          lastVblankNs = ng2::video::DisplayClockNow();
          last = rex::chrono::Clock::QueryGuestTickCount();
          // A drain request (gamescope's, below) isn't acted on while locked:
          // taken here so it doesn't skip a vblank when the lock lets go.
          ng2::video::DrainRequested();
          DispatchInterrupt(0, 2);
          WakeWaits();
          continue;
        }
      }
      uint64_t interval = REXCVAR_GET(vsync) ? vsyncInterval : freeInterval;
      // The display's queue run dry (video.cpp's PresentQueueDrained: a hitch
      // emptied it and the display runs a hair faster than this timer, so it
      // never refills): vblanks 1% faster until the acquires block again,
      // that is until a frame is queued and the display paces the game.
      // Bounded: a frame of queue takes 100 vblanks at 1%, so an episode
      // lasts 4 s at most; one that doesn't refill (a queue that can't fill:
      // a slow GPU, a display the rate check let through, or a heavy scene
      // where the game is under 60 anyway) pauses the next for 10 s,
      // doubling up to a minute, and one that refills starts that over.
      // Never off for good: heavy scenes failed it twice and left the queue
      // dry for the rest of the session (#36, Mission 14).
      static bool refilling = false;
      static int64_t refillStartNs = 0, refillPausedUntilNs = 0, refillPauseNs = 10'000'000'000;
      static int refillsLogged = 0;
      bool drained = ng2::video::PresentQueueDrained();
      int64_t nowNs = ng2::video::DisplayClockNow();
      if (refilling && !drained) {
        refilling = false;  // (it filled: the acquires block again)
        refillPauseNs = 10'000'000'000;
      } else if (refilling && nowNs - refillStartNs > 4'000'000'000) {
        refilling = false;
        refillPausedUntilNs = nowNs + refillPauseNs;
        if (refillsLogged <= 10) {
          REXLOG_INFO("gpu: the display's queue didn't refill in 4 s, next try in {} s",
                      refillPauseNs / 1'000'000'000);
        }
        refillPauseNs = std::min<int64_t>(refillPauseNs * 2, 60'000'000'000);
      } else if (!refilling && drained && REXCVAR_GET(vsync) && nowNs >= refillPausedUntilNs) {
        refilling = true;
        refillStartNs = nowNs;
        if (refillsLogged++ < 10) {
          REXLOG_INFO("gpu: the display's queue ran dry, vblanks 1% faster until it refills");
        }
      }
      if (refilling) {
        interval = interval * 99 / 100;
      }
      uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
      // A frame ahead of the display under gamescope (video.cpp's
      // DrainRequested): the next step gives no vblank, once. Up to 3 come
      // within 3 s of each other (a queue two frames ahead takes two); a
      // fourth means skips don't help (a GPU too slow, Steam's frame
      // limiter): none for 30 s, doubling each time that happens again, up
      // to 10 min. A skip with no request for 3 s after it starts that over.
      bool drain = ng2::video::DrainRequested();  // (always taken: none left over)
      if (drain && REXCVAR_GET(vsync) && (presentMode == 1 || presentMode == 2 || presentMode == 3)) {
        int64_t t = ng2::video::DisplayClockNow();
        bool quick = lastDrainNs && t - lastDrainNs < 3'000'000'000;
        if (lastDrainNs && !quick) {
          drainPauseNs = 30'000'000'000;  // (the last skips helped)
        }
        drainsQuick = quick ? drainsQuick + 1 : 0;
        if (drainsQuick >= 3) {
          drainPausedUntil = t + drainPauseNs;
          drainPauseNs = std::min<int64_t>(drainPauseNs * 2, 600'000'000'000);
          drainsQuick = 0;
          lastDrainNs = 0;
        } else if (t >= drainPausedUntil) {
          lastDrainNs = t;
          skipNext = true;
          if (drainsLogged < 20) {
            drainsLogged++;
            REXLOG_INFO("gpu: a vblank late, the game a frame ahead of the display");
          }
        }
      }
      if (now - last > std::max(interval * 4, frequency / 20)) {
        last = now - interval;  // far behind (a stall over 50 ms): no burst
      }
      if (now - last >= interval) {
        last += interval;
        if (skipNext) {  // (the drain: this step gives no vblank)
          skipNext = false;
          continue;
        }
        lastVblankNs = ng2::video::DisplayClockNow();
        lastTargetNs = 0;
        DispatchInterrupt(0, 2);
        WakeWaits();
        continue;
      }
      auto left = std::chrono::nanoseconds(
          int64_t(double(last + interval - now) * 1e9 / double(frequency)));
#if defined(_WIN32)
      // A high-resolution timer to 0.3 ms before, then awake (a plain sleep
      // there wakes on the scheduler's tick: a vblank up to 1 ms late, or a spin).
      if (left > std::chrono::microseconds(400)) {
        ng2::video::PreciseSleepNs(left.count() - 300'000);
      } else {
        std::this_thread::yield();
      }
#else
      std::this_thread::sleep_for(left);
#endif
    }
    return 0;
  }

  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<uint32_t> rptr_writeback_{0};
  uint32_t interrupt_callback_ = 0, interrupt_data_ = 0;
  std::recursive_mutex interrupt_mutex_;
  std::mutex steps_mutex_;
  std::condition_variable steps_cv_;
  struct PendingStep {
    uint64_t after = 0;  // the draw worker's calls recorded before it
    std::function<void()> run;
  };
  std::deque<PendingStep> steps_;
  std::mutex waits_mutex_;
  std::condition_variable waits_cv_;
  uint64_t wakes_ = 0;
  rex::system::object_ref<rex::system::XHostThread> effect_thread_, vsync_thread_;
};

NativeGraphicsSystem* System() {
  return g_system.load(std::memory_order_acquire);
}

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsSystem() {
  return std::make_unique<NativeGraphicsSystem>();
}

void OnInsertFence(uint32_t device) {
  if (auto* s = System()) {
    s->InsertFence(device);
  }
}

void OnCallbackHandshake(uint32_t device, uint32_t flags, uint32_t callback, uint32_t argument) {
  if (auto* s = System()) {
    s->CallbackHandshake(device, flags, callback, argument);
  }
}

void OnFlipBegin(uint32_t device, uint32_t noWait) {
  if (auto* s = System()) {
    s->FlipBegin(device, noWait);
  }
}

void OnFlipWait(uint32_t device, uint32_t noWait) {
  if (auto* s = System()) {
    s->FlipWait(device, noWait);
  }
}

}  // namespace ng2::gpu
