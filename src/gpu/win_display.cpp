// ng2 - Ninja Gaiden II static recompilation
//
// Windows timing helpers (win_display.h).
//
// The display clock: Frame Pacing Auto puts the guest's vblanks on the
// display's refreshes, which it learns from VK_KHR_present_wait (video.cpp's
// DisplayClockThread). AMD's Windows driver doesn't offer that, so there the
// guest ran on its own 60 Hz timer, which drifts from the display and needs
// the queue refill (#36). DXGI tells any GPU's Windows program when the
// monitor refreshes: a thread blocks in IDXGIOutput::WaitForVBlank for the
// monitor the window is on (found again every 2 s: the window can move) and
// reports each vblank.
//
// The period is trusted once 120 refreshes in a row agree with it within 3%,
// and kept until a quarter of a 5 s window disagrees; 8 in a row that don't
// (a bad first measure, or the monitor's mode changed, faster or slower)
// start the measure again. A variable-refresh screen (G-Sync, FreeSync)
// refreshes when a frame arrives, so locking to it would chase the game's own
// frames: 30 refreshes in a 5 s window that disagree before the period is
// trusted make it count as variable for the session, and the game keeps its
// own 60 Hz clock, which such a screen shows evenly. What's reported is the
// refresh period; the vblank lock takes every few (2 on a 120 Hz screen).

#include "gpu/win_display.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <rex/logging.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#endif

namespace ng2::video {

#if defined(_WIN32)
namespace {
// One timer a thread, kept for the thread's life (the timing threads live as
// long as the game). Windows 10 1803 and later; else null.
HANDLE ThreadTimer() {
  thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  return timer;
}
}  // namespace
#endif

bool PreciseSleepAvailable() {
#if defined(_WIN32)
  return ThreadTimer() != nullptr;
#else
  return true;
#endif
}

void PreciseSleepNs(int64_t ns) {
  if (ns <= 0) {
    return;
  }
#if defined(_WIN32)
  if (HANDLE timer = ThreadTimer()) {
    LARGE_INTEGER due;
    due.QuadPart = -std::max<int64_t>(1, ns / 100);  // (relative, 100 ns units)
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
      WaitForSingleObject(timer, DWORD(ns / 1'000'000 + 100));
      return;
    }
  }
#endif
  std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

#if defined(_WIN32)
namespace {

// The DXGI output showing the monitor, on whichever adapter drives it (a
// laptop's panel may hang off the integrated GPU while the discrete one draws).
IDXGIOutput* OutputFor(HMONITOR monitor) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return nullptr;
  }
  IDXGIOutput* found = nullptr;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT a = 0; !found && SUCCEEDED(factory->EnumAdapters1(a, &adapter)); a++) {
    IDXGIOutput* output = nullptr;
    for (UINT o = 0; !found && SUCCEEDED(adapter->EnumOutputs(o, &output)); o++) {
      DXGI_OUTPUT_DESC desc;
      if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
        found = output;
      } else {
        output->Release();
      }
    }
    adapter->Release();
  }
  factory->Release();
  return found;
}

void Run(HWND hwnd, const std::atomic<bool>* run, int64_t (*now)(), DisplayVblankReport report) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  HMONITOR monitor = nullptr;
  IDXGIOutput* output = nullptr;
  int64_t last = 0, checkedAt = 0, measured = 0;
  int agreeing = 0, offInRow = 0, failures = 0;
  uint32_t windowUnits = 0, windowOff = 0;
  bool trusted = false, variable = false, warned = false;
  auto restart = [&] {
    last = 0, measured = 0, agreeing = 0, offInRow = 0;
    windowUnits = 0, windowOff = 0;
    trusted = false;
    report(0, 0, false);
  };
  while (run->load()) {
    int64_t t = now();
    if (!output || t - checkedAt > 2'000'000'000) {
      checkedAt = t;
      HMONITOR m = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
      if (m != monitor || !output) {
        if (output) {
          output->Release();
        }
        if (m != monitor) {
          variable = false, warned = false;  // (another monitor: judged afresh)
        }
        output = OutputFor(m);
        monitor = m;
        failures = 0;
        restart();
        if (!output) {
          if (!warned) {
            warned = true;
            REXLOG_WARN("video: no DXGI output for the window's monitor: the guest's vblanks keep "
                        "their own timer");
          }
          std::this_thread::sleep_for(std::chrono::seconds(2));
          continue;
        }
      }
    }
    if (FAILED(output->WaitForVBlank())) {
      if (++failures >= 20) {  // (a mode change or an adapter reset: the output again)
        output->Release();
        output = nullptr;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    failures = 0;
    t = now();
    int64_t interval = last ? t - last : 0;
    last = t;
    if (interval < 4'000'000 || interval > 70'000'000) {
      continue;  // (the first, or a long stall: no measure)
    }
    // A late wake-up that missed a refresh or two isn't a change of rate.
    int64_t one = interval;
    bool skipped = false;
    if (measured) {
      int64_t n = std::max<int64_t>(1, (interval + measured / 2) / measured);
      if (n > 1 && n <= 3 && std::llabs(interval - n * measured) * 25 < measured) {
        one = interval / n;
        skipped = true;
      }
    }
    bool agrees = measured && one <= 20'000'000 && std::llabs(one - measured) * 33 < measured;
    // (8 in a row off, or skipping: a bad first measure, or the mode changed)
    offInRow = agrees && !skipped ? 0 : offInRow + 1;
    if (offInRow >= 8 && interval <= 20'000'000) {
      measured = interval, agreeing = 0, offInRow = 0;
      if (trusted) {
        trusted = false;
        REXLOG_INFO("video: display clock (DXGI) no longer steady (the refresh changed)");
      }
    } else if (one <= 20'000'000) {
      measured = measured ? measured + (one - measured) / 16 : one;
    }
    agreeing = agrees ? agreeing + 1 : 0;
    windowUnits++, windowOff += agrees ? 0 : 1;
    bool was = trusted;
    if (!trusted && !variable && windowOff >= 30) {
      variable = true;
      REXLOG_INFO("video: the monitor's refresh keeps changing (G-Sync / FreeSync): the guest's "
                  "vblanks keep their own 60 Hz timer");
    }
    if (!trusted) {
      trusted = !variable && agreeing >= 120;
    } else if (windowUnits >= 300) {
      trusted = windowOff * 4 <= windowUnits;
    }
    if (windowUnits >= 300) {
      windowUnits = 0, windowOff = 0;
    }
    if (trusted != was) {
      REXLOG_INFO("video: display clock (DXGI) {} at {:.3f} Hz", trusted ? "steady" : "no longer steady",
                  1e9 / double(measured));
      if (!trusted) {
        agreeing = 0;
      }
    }
    report(t, measured, trusted);
  }
  if (output) {
    output->Release();
  }
}

}  // namespace

bool StartWindowsDisplayClock(void* hwnd, const std::atomic<bool>* run, int64_t (*now)(),
                              DisplayVblankReport report) {
  if (!hwnd) {
    return false;
  }
  std::thread([=] { Run(static_cast<HWND>(hwnd), run, now, report); }).detach();
  return true;
}
#else
bool StartWindowsDisplayClock(void*, const std::atomic<bool>*, int64_t (*)(), DisplayVblankReport) {
  return false;
}
#endif

}  // namespace ng2::video
