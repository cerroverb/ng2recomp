// ng2 - Ninja Gaiden II static recompilation
//
// Audio backend with no output device. The game's XAudio engine needs a
// render client that consumes frames at the hardware rate; the SDK's nop
// backend refuses to create one (and its SDL build has no dummy driver), so
// silent runs (NG2_AUDIO=0) use this: frames are dropped and the client
// semaphore is released every 256 samples at 48 kHz, like a sound card would.

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/logging.h>
#include <rex/thread.h>

namespace ng2 {

using rex::X_STATUS;

class NullAudioDriver : public rex::audio::AudioDriver {
 public:
  NullAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore) {
    thread_ = std::thread([this] { Run(); });
  }

  ~NullAudioDriver() override {
    running_ = false;
    thread_.join();
  }

  void SubmitFrame(uint32_t) override {
    // Bounded like the SDL driver's queue; extra frames are dropped.
    if (queued_.load() < kMaxQueued) {
      queued_.fetch_add(1);
    }
  }

 private:
  static constexpr int kMaxQueued = 64;
  // 256 samples per channel at 48 kHz.
  static constexpr auto kFramePeriod = std::chrono::microseconds(5333);

  void Run() {
    auto next = std::chrono::steady_clock::now();
    while (running_) {
      next += kFramePeriod;
      std::this_thread::sleep_until(next);
      int queued = queued_.load();
      while (queued > 0 && !queued_.compare_exchange_weak(queued, queued - 1)) {
      }
      if (queued > 0) {
        semaphore_->Release(1, nullptr);
      }
    }
  }

  rex::thread::Semaphore* semaphore_;
  std::atomic<int> queued_{0};
  std::atomic<bool> running_{true};
  std::thread thread_;
};

class NullAudioSystem : public rex::audio::AudioSystem {
 public:
  explicit NullAudioSystem(rex::runtime::FunctionDispatcher* function_dispatcher)
      : AudioSystem(function_dispatcher) {}

  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* function_dispatcher) {
    return std::make_unique<NullAudioSystem>(function_dispatcher);
  }

 protected:
  X_STATUS CreateDriver(size_t, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override {
    *out_driver = new NullAudioDriver(memory(), semaphore);
    return X_STATUS_SUCCESS;
  }

  void DestroyDriver(rex::audio::AudioDriver* driver) override { delete driver; }
};

// The default backend: SDL, and where no output device opens (#84: WASAPI
// "can't initialize audio client" on every start), the driver above, so the
// game runs without sound instead of calling the callback it never got.
class SdlOrSilentAudioSystem : public rex::audio::sdl::SDLAudioSystem {
 public:
  using SDLAudioSystem::SDLAudioSystem;

  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* function_dispatcher) {
    return std::make_unique<SdlOrSilentAudioSystem>(function_dispatcher);
  }

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override {
    X_STATUS status = SDLAudioSystem::CreateDriver(index, semaphore, out_driver);
    if (XSUCCEEDED(status)) {
      return status;
    }
    REXLOG_WARN("audio: no output device could be opened; the game runs without sound");
    *out_driver = new NullAudioDriver(memory(), semaphore);
    return X_STATUS_SUCCESS;
  }

  void DestroyDriver(rex::audio::AudioDriver* driver) override {
    if (dynamic_cast<NullAudioDriver*>(driver)) {
      delete driver;
      return;
    }
    SDLAudioSystem::DestroyDriver(driver);
  }
};

}  // namespace ng2
