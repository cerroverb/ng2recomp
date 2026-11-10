// ng2 - Ninja Gaiden II static recompilation
//
// Frame interpolation (interp.cpp): what the renderer (video.cpp) and it
// share.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

#include <plume_render_interface.h>

namespace ng2::video {

// Frame interpolation (NG2_INTERP=1). Each draw's vertex constants are kept
// (the registers its shader reads); at the present the frame's draws are
// matched with the last frame's (same shaders and target shape, the closest
// constants: the game sorts its draws anew each frame), and for each picture
// between the two every matched draw gets its constants that part of the
// way. A register that moved too far for motion (the game re-sorts its list
// of lights) takes the new value. The frame's commands then run again with
// those (the present's lambda), each picture shown in turn before the frame's.
struct InterpDraw {
  uint64_t group = 0;
  std::vector<uint16_t> registers;
  std::vector<float> constants;  // 4 a register, little-endian values
  std::array<uint64_t, 3> pushConstants{};
  bool submitted = false;
  uint32_t boneAddress = 0, boneSize = 0;  // its bone palette (physical), if skinned
  uint64_t mesh = 0;  // its vertex and index data (0: not known)
  // The pixel shader's registers it reads and their values: the lights, in
  // the camera's space, must turn with the camera part of the way too.
  std::vector<uint16_t> psRegisters;
  std::vector<float> psConstants;
  bool ui = false;  // in the game's 2D pass (HUD, menus): drawn as it is now
};
struct InterpPalette {
  uint32_t address = 0, size = 0;
  std::vector<uint32_t> words;  // as in guest memory (big-endian) at the frame's end
};
struct InterpFrames {
  std::vector<std::vector<std::array<uint64_t, 3>>> phases;  // push constants by draw
  std::vector<std::vector<plume::RenderBufferReference>> copies;    // palette copies by palette
  // A slow screen (the scene drawn 30 or 15 times a second: Game Over): the
  // pictures spread by the clock over spanNs, the time since the last full
  // frame; a partial frame there (the last picture again) not shown.
  bool slow = false, partial = false;
  int64_t spanNs = 0;
  int64_t spacingNs = 0;  // whole refreshes between its pictures (0: span / pictures)
  // Paced by the display: a picture for each
  // refresh (or each second, third: the rate's divider) in the time the frame
  // stands for, each part of the way by when it is shown. times: when each
  // picture shown goes out (the pictures between, then the frame's own if
  // ownShown: a refresh falls at its end); fractions: the pictures between's.
  bool paced = false, ownShown = true;
  std::vector<int64_t> times;
  std::vector<float> fractions;
  int64_t presentNs = 0;  // the frame's present (game thread's clock)
};
// The most pictures a frame (between and its own): what the swapchain's
// images leave (set with it).
constexpr uint32_t kInterpMaxPictures = 8;

// Where the pictures' constants and palettes go (video.cpp: the frame slot's
// upload space for them).
struct InterpUpload {
  plume::RenderBuffer* buffer;
  uint64_t offset;
  uint8_t* data;
  uint64_t address;
};

// Frame interpolation's threads: the build of a frame's pictures, their palettes
// and constants (the render thread records the pictures). A few made once, at the first
// job, instead of a thread for each job each frame. Never destroyed (left
// to run at exit, as the other workers: a job may be waiting for the GPU).
class WorkerPool {
 public:
  // Runs job on one of the threads.
  void Post(std::function<void()> job) {
    {
      std::lock_guard lock(mutex_);
      Start();
      queue_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

  // body(i) for each i below count, on this thread and on as many of the
  // pool's as are free (at most helpers); returns when all are done. This
  // thread takes items too, so it never waits for a job that hasn't started
  // (a job of the pool's may call it).
  template <typename Body>
  void ForEach(size_t count, uint32_t helpers, Body&& body) {
    if (count <= 1 || helpers == 0) {
      for (size_t i = 0; i < count; i++) {
        body(i);
      }
      return;
    }
    struct Shared {
      std::atomic<size_t> next{0}, done{0};
      std::mutex mutex;
      std::condition_variable cv;
    };
    auto shared = std::make_shared<Shared>();
    auto* function = &body;  // (read only for an item taken: the caller is still waiting then)
    auto work = [shared, count, function] {
      for (size_t i; (i = shared->next.fetch_add(1)) < count;) {
        (*function)(i);
        if (shared->done.fetch_add(1) + 1 == count) {
          std::lock_guard lock(shared->mutex);
          shared->cv.notify_all();
        }
      }
    };
    for (size_t h = 0; h < std::min<size_t>(helpers, count - 1); h++) {
      Post(work);
    }
    work();
    std::unique_lock lock(shared->mutex);
    shared->cv.wait(lock, [&] { return shared->done.load() == count; });
  }

 private:
  void Start() {  // (mutex_ held)
    if (!threads_.empty()) {
      return;
    }
    uint32_t count = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 6u);
    for (uint32_t i = 0; i < count; i++) {
      threads_.emplace_back([this] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "Interpolation");
#endif
        while (true) {
          std::function<void()> job;
          {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return !queue_.empty(); });
            job = std::move(queue_.front());
            queue_.pop_front();
          }
          job();
        }
      });
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> threads_;
};
extern WorkerPool& g_interpPool;

// interp.cpp
bool InterpWanted();   // what the setting (or NG2_INTERP) asks for
bool InterpEnabled();  // on for the frame being made (changed between frames only)
uint32_t InterpDivider();  // a picture each n-th refresh
int32_t CaptureInterpDraw(uint64_t group, const uint32_t* guestConstants, const uint64_t (&mask)[4],
                          const uint32_t* psConstants = nullptr, const uint64_t (*psMask)[4] = nullptr);
InterpDraw& InterpCaptured(int32_t index);
int32_t NoteInterpPalette(uint32_t address, uint32_t size);
void NoteInterpDraw(int32_t index, const uint64_t (&pushConstants)[3]);
// The frame's pictures, built on the pool (the last build waited for first).
std::shared_future<std::shared_ptr<InterpFrames>> StartInterpBuild(uint32_t slot, uint64_t frameCount,
                                                                   int64_t presentNs);
void WaitInterpBuild();
void ResetInterp(bool on);  // switched: everything kept from earlier frames dropped
extern std::atomic<uint32_t> g_interpPictureCap;
extern std::shared_future<std::shared_ptr<InterpFrames>> g_interpLastJob;
extern std::atomic<int64_t> g_interpReadyLagNs;

// video.cpp
int64_t InterpRefreshNs();  // the display's refresh, 0 not known
int64_t ModeRefreshNs();    // the display mode's (SDL), 0 not known
InterpUpload AllocateInterpUpload(uint32_t slot, uint64_t size, uint64_t alignment);
bool UsesRenderThread();

}  // namespace ng2::video
