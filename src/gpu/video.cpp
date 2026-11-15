// ng2 - Ninja Gaiden II static recompilation
//
// Native renderer: Vulkan through plume (Unleashed Recompiled's RHI), driven
// by hooks on the game's D3D calls (d3d_hooks.cpp). The game's D3D library
// keeps running (its GPU side done at its calls by native_gpu.cpp, no GPU
// emulation); at each hooked call this file reads the device and does the
// host-side work.
//
// Commands are recorded on the game's D3D thread as the calls happen and
// submitted at Swap. Render targets are host textures keyed by their EDRAM
// placement; resolves copy them into host textures keyed by the destination
// guest address, which is also how Swap finds the front buffer.

#include "video.h"
#include "../aspect.h"
#include "../movie/movie.h"
#include "prompts.h"
#include "win_display.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <filesystem>
#include <future>
#include <thread>
#if defined(__linux__)
#include <pthread.h>
#endif
#include <cstdlib>
#include <cstring>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <array>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include <optional>
#include <unordered_set>
#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <rex/ui/windowed_app_context.h>
#if defined(__linux__)
#include <unistd.h>
#endif

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <zstd.h>

#include <plume_render_interface.h>
#include <plume_vulkan.h>  // (the textures' layouts plume keeps: frame interpolation's pictures)
#include <smolv.h>

#include "guest.h"
#include "interp.h"
#include "menu_label.h"
#include "shader_cache.h"


extern const uint8_t g_fullscreen_vs[];
extern const size_t g_fullscreen_vs_size;
extern const uint8_t g_present_ps[];
extern const size_t g_present_ps_size;
extern const uint8_t g_rect_list_gs[];
extern const size_t g_rect_list_gs_size;
extern const uint8_t g_copy_ps[];
extern const size_t g_copy_ps_size;
extern const uint8_t g_copy_7e3_ps[];
extern const size_t g_copy_7e3_ps_size;
extern const uint8_t g_copy_7e3_ms_ps[];
extern const size_t g_copy_7e3_ms_ps_size;
extern const uint8_t g_scale_ps[];
extern const size_t g_scale_ps_size;
extern const uint8_t g_depth_copy_ps[];
extern const size_t g_depth_copy_ps_size;
extern const uint8_t g_depth_copy_ms_ps[];
extern const size_t g_depth_copy_ms_ps_size;
extern const uint8_t g_debug_ps[];
extern const size_t g_debug_ps_size;

REXCVAR_DEFINE_INT32(render_scale, 1, "Video",
                     "Internal resolution: render targets at this many times the game's size "
                     "each way (1-8)")
    .range(1, 8);
REXCVAR_DEFINE_BOOL(vsync, true, "Video",
                    "Wait for the display's refresh to present (older setting: present_mode "
                    "takes over once set)");
REXCVAR_DEFINE_INT32(present_mode, 1, "Video",
                     "How frames are shown: 0 off (no V-Sync, may tear), 1 FIFO (V-Sync), "
                     "2 mailbox (V-Sync, newest frame), 3 adaptive (V-Sync unless late)")
    .range(0, 3);
REXCVAR_DEFINE_BOOL(async_pipelines, true, "Video",
                    "Make render pipelines on background threads: a new object or effect can "
                    "appear a frame or two late instead of the game stalling");
REXCVAR_DEFINE_BOOL(log_hitches, false, "Video",
                    "Log each frame that takes over 25 ms, with what the renderer spent it on");
REXCVAR_DEFINE_INT32(antialiasing, 2, "Video",
                     "MSAA samples for the render targets the game multisamples (its main "
                     "picture): 0 off, 2 as the game, 4 or 8")
    .range(0, 8);
REXCVAR_DEFINE_INT32(bloom, 2, "Video", "The game's bloom: 0 off, 1 reduced (half), 2 on")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(half_pixel_offset, true, "Video",
                    "Shift drawing by half a pixel where the game uses D3D's pixel centers, "
                    "as the console does");
// Extras' Texture LOD Bias: the game's own mip bias (-1.5 on most textures),
// or 0 or -1 for every texture (less shimmer at higher render scales).
REXCVAR_DEFINE_INT32(texture_lod_bias, 0, "Extras", "0: the game's, 1: 0, 2: -1");
// Input Latency: Low waits, before drawing a frame, until the last one is
// on screen (VK_KHR_present_wait), so no finished frame queues for the
// display (FIFO's three images let two): about one or two refreshes less
// between a press and its picture, with less slack for a slow frame.
REXCVAR_DEFINE_INT32(input_latency, 0, "Video", "0: standard, 1: low (needs VK_KHR_present_wait)");
// Frame Interpolation: pictures made between the game's frames (its draws
// again, part of the way from the last frame's), one for each refresh of the
// display, or each second one; switched live. NG2_INTERP=1/0 overrides.
REXCVAR_DEFINE_INT32(frame_interpolation, 0, "Video",
                     "0: off, 1: a picture each refresh, 2: each second refresh (each one below 120 Hz), "
                     "3: 2x (one picture between the game's frames)");
REXCVAR_DEFINE_INT32(anisotropic_filtering, -1, "Video",
                     "Anisotropic filtering for mipmapped textures: -1 as the game asks, "
                     "0 off, 1-5 for 1x, 2x, 4x, 8x, 16x")
    .range(-1, 5);

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

using namespace plume;
namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;

namespace ng2::video {

void RequestDisplayRate();  // (the display clock, below)
// The game's vblank count (0x84C55B50, its time) at each present: frames that
// saw none or two and more, for the display log line (frames shown evenly can
// still move unevenly when the game's ticks per frame vary).
std::atomic<uint32_t> g_framesNoTick{0}, g_framesManyTicks{0};
// Pictures presented, the game's frames and interpolation's between them
// (the FPS counter's rate with interpolation on).
std::atomic<uint64_t> g_picturesPresented{0};
uint64_t PicturesPresented() {
  return g_picturesPresented.load(std::memory_order_relaxed);
}
bool InterpolationOn() {
  return InterpEnabled();
}

namespace {

// Frame slots: the game is at most this many frames ahead of the GPU. Two;
// three with NG2_INTERP (FrameSlots): the pictures between are made after the
// frame and drawn before it, so with two the game waited for the build and
// three draws of the scene (in a crowd, 25 ms frames); a third lets each go
// on at once (the game, the build, the GPU), each within a frame.
constexpr size_t NUM_FRAMES = 3;
uint32_t FrameSlots() {
  return InterpEnabled() ? 3u : 2u;
}
constexpr RenderFormat BACKBUFFER_FORMAT = RenderFormat::B8G8R8A8_UNORM;
// Bindless heaps, laid out as the translated shaders expect (docs/shaders.md).
constexpr uint32_t TEXTURE_DESCRIPTOR_COUNT = 65536;
constexpr uint32_t SAMPLER_DESCRIPTOR_COUNT = 1024;
constexpr uint32_t PUSH_CONSTANT_SIZE = 24;  // three buffer addresses

// Descriptor slots 0-2 hold the blank 2D, 3D and cube textures.
enum : uint32_t {
  DESCRIPTOR_NULL_2D,
  DESCRIPTOR_NULL_3D,
  DESCRIPTOR_NULL_CUBE,
  DESCRIPTOR_FIRST_FREE,
};
enum : uint32_t { SAMPLER_LINEAR_CLAMP, SAMPLER_FIRST_FREE };

// A host texture with the layout it was last transitioned to.
// The title menu's RECOMP SETTINGS label (menu_label.cpp) and the menu
// atlas's palette this frame, which it's drawn with.
uint32_t g_menuLabelBase = 0;
uint32_t g_menuAtlasPalette = 0;
uint64_t g_menuAtlasPaletteFrame = ~0ull;

struct HostTexture {
  std::unique_ptr<RenderTexture> texture;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
  RenderSampleCounts samples = RenderSampleCount::COUNT_1;
  std::unique_ptr<RenderTextureView> view;  // for the texture heap
  uint32_t descriptor = 0;                    // in the texture heap, 0 = none
  bool float7e3 = false;  // a color target last bound as 2_10_10_10_FLOAT
  uint32_t scale = 1;     // host pixels per guest pixel (render targets, resolves)
  bool prompts = false;   // the button atlas or its palette (prompts.cpp)
};

struct DescriptorAllocator {
  uint32_t next = DESCRIPTOR_FIRST_FREE;
  std::vector<uint32_t> freed;
  uint32_t Allocate() {
    if (!freed.empty()) {
      uint32_t index = freed.back();
      freed.pop_back();
      return index;
    }
    return next < TEXTURE_DESCRIPTOR_COUNT ? next++ : 0;
  }
  void Free(uint32_t index) {
    if (index >= DESCRIPTOR_FIRST_FREE) {
      freed.push_back(index);
    }
  }
};

std::recursive_mutex g_mutex;

std::unique_ptr<RenderInterface> g_interface;
std::unique_ptr<RenderDevice> g_device;
std::unique_ptr<RenderCommandQueue> g_queue;
std::unique_ptr<RenderCommandList> g_commandLists[NUM_FRAMES];
// The frame's prologue: a command list of its own, submitted just before the
// frame's (on its own: PrologueMode), holding the guest memory copies that
// can run first (see RecordGuestMemoryCopy). Recorded by the thread that records the frame's
// draws, with its own pool; open from its first copy to the frame's submit.
std::unique_ptr<RenderCommandList> g_prologueLists[NUM_FRAMES];
bool g_prologueOpen[NUM_FRAMES] = {};
std::unique_ptr<RenderCommandFence> g_commandFences[NUM_FRAMES];
std::unique_ptr<RenderCommandSemaphore> g_acquireSemaphores[NUM_FRAMES];
// One per swapchain image: a present can still hold its semaphore when the
// next frame in flight starts.
std::vector<std::unique_ptr<RenderCommandSemaphore>> g_renderSemaphores;
bool g_commandListPending[NUM_FRAMES];
std::unique_ptr<RenderSwapChain> g_swapChain;

// NG2_LATENCY_LOG=1, from the pad to the screen: the time each frame read
// the pad (the main thread's input update), carried to the frame's present
// (the frame the render thread took, at the main thread's handoff) and to
// the refresh it reached the screen at (the display clock), each 10 s.
std::atomic<int64_t> g_inputNs{0}, g_handedInputNs{0};
struct TaggedPresent {
  uint64_t id = 0;
  int64_t inputNs = 0, presentNs = 0;
};
std::mutex g_taggedMutex;
TaggedPresent g_tagged[64];

int64_t SteadyNs();
// (NG2_LATENCY_LOG) which: 0 the frame's first picture, 1 its own (the last)
void NotePadToPresent(int64_t ns, int which) {
  static std::mutex mutex;
  static double sum[2] = {}, worst[2] = {};
  static uint64_t count[2] = {};
  static int64_t logAt = 0;
  std::lock_guard lock(mutex);
  double ms = double(ns) / 1e6;
  sum[which] += ms, worst[which] = std::max(worst[which], ms), count[which]++;
  int64_t now = SteadyNs();
  if (!logAt) {
    logAt = now;
  }
  if (now - logAt > 10'000'000'000) {
    REXLOG_INFO("latency pad to present: first picture {:.1f} ms (worst {:.1f}), the frame's own "
                "{:.1f} ms (worst {:.1f}); {} frames",
                count[0] ? sum[0] / double(count[0]) : 0.0, worst[0],
                count[1] ? sum[1] / double(count[1]) : 0.0, worst[1], count[1]);
    sum[0] = sum[1] = worst[0] = worst[1] = 0, count[0] = count[1] = 0, logAt = now;
  }
}

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::vector<std::unique_ptr<RenderFramebuffer>> g_swapChainFramebuffers;
bool g_swapChainValid = false;
uint32_t g_frame = 0;
// Frames that stayed an extra refresh on the display in the last whole second
// (the display clock's thread; read by the Input Latency Low wait).
std::atomic<int> g_repeatsLastSecond{0};  // frames that stayed an extra refresh, the last whole second
std::atomic<int> g_lowRelaxations{0};     // times Input Latency Low queued a frame (its log line)
// Swapchain acquires in a row that didn't block (under 1.5 ms): with FIFO
// they block once the display's queue is full, so a long run means the
// queue has run dry and every frame reaches the screen as soon as it's
// ready, with the game thread's jitter (#36: choppy after a hitch until a
// V-Sync change). PresentQueueDrained reads it.
std::atomic<int> g_acquireFreeRun{0};
int64_t g_blockedNs = 0;  // (render thread) the frame's acquire wait, until its present adds its own
bool g_recording = false;

std::unique_ptr<RenderDescriptorSet> g_textureDescriptorSet;
std::unique_ptr<RenderDescriptorSet> g_samplerDescriptorSet;
std::unique_ptr<RenderPipelineLayout> g_pipelineLayout;
std::unique_ptr<RenderSampler> g_linearClampSampler;
HostTexture g_nullTextures[DESCRIPTOR_FIRST_FREE];
bool g_nullTexturesReady = false;  // cleared and in SHADER_READ (the first frame's list)
std::unique_ptr<RenderTextureView> g_nullTextureViews[DESCRIPTOR_FIRST_FREE];
DescriptorAllocator g_descriptors;

std::unique_ptr<RenderShader> g_fullscreenShader;
std::unique_ptr<RenderShader> g_presentShader;
std::unique_ptr<RenderPipeline> g_presentPipeline;
std::unique_ptr<RenderShader> g_rectListShader;
std::unique_ptr<RenderShader> g_copyShader;
std::unique_ptr<RenderShader> g_copy7e3Shader;
std::unique_ptr<RenderShader> g_copy7e3MsShader;
std::unique_ptr<RenderShader> g_scaleShader;
std::unique_ptr<RenderShader> g_debugShader;
std::unique_ptr<RenderShader> g_depthCopyShader;
std::unique_ptr<RenderShader> g_depthCopyMsShader;
// Copy pipelines by pixel shader and destination format.
std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> g_copyPipelines;
// Single-sample copies of MSAA render targets, for converting resolves.
std::unordered_map<const void*, std::unique_ptr<HostTexture>> g_resolveTemps;

// NG2_PROFILE=1: time spent per renderer stage, logged once a second.
// NG2_PROFILE=2 also times each draw on the GPU, logged by shader pair.
// wait: for the GPU (a frame's fence, the swapchain image), mostly inside the other stages.
enum class Stage { kDraw, kSync, kTexture, kPipeline, kResolve, kPresent, kWait, kCount };
constexpr const char* kStageNames[] = {"draw", "sync", "texture", "pipeline", "resolve",
                                       "present", "wait"};
struct Profile {
  bool enabled = [] {
    const char* v = std::getenv("NG2_PROFILE");
    return v && (v[0] == '1' || v[0] == '2');
  }();
  bool perDraw = [] {
    const char* v = std::getenv("NG2_PROFILE");
    return v && v[0] == '2';
  }();
  uint64_t nanoseconds[size_t(Stage::kCount)] = {};
  uint64_t comparedBytes = 0, uploadedBytes = 0, frames = 0;
  // (the end of frame's second look at the frame's ranges; bytes compared by
  // ranges unchanged for over a second but checked every frame)
  uint64_t finishNs = 0, finishBytes = 0, quietBytes = 0;
  uint64_t textureLookups = 0, hashedBytes = 0, texturesMade = 0, textureMadeBytes = 0;
  // GPU time (timestamps): whole frames, their resolves and their present pass.
  uint64_t gpuNs = 0, gpuResolveNs = 0, gpuPresentNs = 0, gpuFrames = 0;
  uint64_t barriers = 0, passes = 0;  // recorded (the CPU's count), and passes started
  uint64_t prologueCopies = 0, inOrderCopies = 0;  // guest memory copies, by where they go
  // GPU time by render pass target ("1120x584 fmt 3 x2, 2 colors, depth"): from a
  // pass's start mark to the next mark.
  std::map<std::string, uint64_t> gpuPassNs;
  // With NG2_PROFILE=2, GPU time by shader pair (vertex, pixel): from a draw's
  // mark to the next mark, and the number of draws.
  std::map<std::pair<uint64_t, uint64_t>, std::pair<uint64_t, uint64_t>> gpuDrawNs;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
} g_profile;

// With NG2_PROFILE=1: GPU timestamps in each frame's command list, read once
// its slot comes round again (its fence passed). Marks: the frame's start,
// each resolve's start and end, the present pass's start, the frame's end.
enum class GpuMark : uint8_t {
  kFrameStart, kResolveStart, kResolveEnd, kPresentStart, kFrameEnd, kPass, kDraw, kDrawEnd
};
uint32_t GpuMarkCapacity() {
  return g_profile.perDraw ? 8192 : 512;
}
struct GpuTiming {
  std::unique_ptr<RenderQueryPool> pool;
  uint32_t count = 0;  // marks written this frame
  std::vector<GpuMark> kinds;
  std::vector<std::string> passLabels;  // per kPass mark, in order
  std::vector<std::pair<uint64_t, uint64_t>> drawShaders;  // per kDraw mark, in order
};
GpuTiming g_gpuTiming[NUM_FRAMES];

// log_hitches (or NG2_HITCHES=1): a frame that took over 25 ms, and well
// over the one before, is logged with what the renderer spent its time on
// and what it had to make.
struct FrameStats {
  bool enabled = false;
  uint64_t nanoseconds[size_t(Stage::kCount)] = {};
  uint32_t pipelines = 0, shaders = 0, textures = 0;
  uint64_t pipelineNs = 0, shaderNs = 0, textureNs = 0, textureBytes = 0;
  std::chrono::steady_clock::time_point lastSwap;
  double lastMs = 0.0;
} g_frameStats;

bool Timing() {
  return g_profile.enabled || g_frameStats.enabled;
}

// The timers' clock: the CPU's time stamp counter where there is one (a
// clock_gettime is tens of ns, and the stages are timed thousands of times a
// frame), converted at a rate measured once against steady_clock.
uint64_t TimerTicks() {
#if defined(__x86_64__) || defined(_M_X64)
  return __rdtsc();
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
#endif
}
uint64_t TicksToNs(uint64_t ticks) {
#if defined(__x86_64__) || defined(_M_X64)
  static const double nsPerTick = [] {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t c0 = __rdtsc();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2)) {
    }
    auto t1 = std::chrono::steady_clock::now();
    uint64_t c1 = __rdtsc();
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
           double(c1 - c0);
  }();
  return uint64_t(double(ticks) * nsPerTick);
#else
  return ticks;
#endif
}

// Times one thing made during the frame (a pipeline, shader or texture).
class MakeTimer {
 public:
  MakeTimer(uint32_t& count, uint64_t& ns) : count_(count), ns_(ns) {
    if (g_frameStats.enabled) {
      start_ = TimerTicks();
    }
  }
  ~MakeTimer() {
    if (g_frameStats.enabled) {
      count_++;
      ns_ += TicksToNs(TimerTicks() - start_);
    }
  }

 private:
  uint32_t& count_;
  uint64_t& ns_;
  uint64_t start_ = 0;
};

class StageTimer {
 public:
  explicit StageTimer(Stage stage) : stage_(stage) {
    if (Timing()) {
      start_ = TimerTicks();
    }
  }
  ~StageTimer() {
    if (Timing()) {
      uint64_t ns = TicksToNs(TimerTicks() - start_);
      if (g_profile.enabled) {
        g_profile.nanoseconds[size_t(stage_)] += ns;
      }
      g_frameStats.nanoseconds[size_t(stage_)] += ns;
    }
  }

 private:
  Stage stage_;
  uint64_t start_ = 0;
};

// NG2_PRESENT_GAPS=1: the time between presents, each second: the median,
// the shortest and longest, and how many were over 20 ms or under 13 ms (at
// 60 fps an even game presents every 16.7 ms; frame pacing shows up here).
void ReportPresentGaps(std::chrono::steady_clock::time_point now) {
  static const bool enabled = [] {
    const char* v = std::getenv("NG2_PRESENT_GAPS");
    return v && v[0] == '1';
  }();
  if (!enabled) {
    return;
  }
  static std::chrono::steady_clock::time_point last, start;
  static std::vector<double> gaps;
  if (last != std::chrono::steady_clock::time_point()) {
    gaps.push_back(std::chrono::duration<double, std::milli>(now - last).count());
  } else {
    start = now;
  }
  last = now;
  if (now - start < std::chrono::seconds(1) || gaps.empty()) {
    return;
  }
  std::sort(gaps.begin(), gaps.end());
  size_t over = std::count_if(gaps.begin(), gaps.end(), [](double g) { return g > 20.0; });
  size_t under = std::count_if(gaps.begin(), gaps.end(), [](double g) { return g < 13.0; });
  REXLOG_INFO("video: present gaps ms: {} median {:.2f} min {:.2f} max {:.2f}, {} over 20, {} under 13",
              gaps.size(), gaps[gaps.size() / 2], gaps.front(), gaps.back(), over, under);
  gaps.clear();
  start = now;
}

void ReportHitch() {
  g_frameStats.enabled = REXCVAR_GET(log_hitches) || [] {
    const char* v = std::getenv("NG2_HITCHES");
    return v && v[0] == '1';
  }();
  auto now = std::chrono::steady_clock::now();
  auto& f = g_frameStats;
  if (f.enabled && f.lastSwap != std::chrono::steady_clock::time_point()) {
    double ms = std::chrono::duration<double, std::milli>(now - f.lastSwap).count();
    // Over 25 ms and well over the frame before (cutscenes run at 30 fps).
    if (ms > 25.0 && ms > 1.6 * f.lastMs) {
      // Sync, texture and pipeline time is spent inside draws.
      double renderer = (f.nanoseconds[size_t(Stage::kDraw)] +
                         f.nanoseconds[size_t(Stage::kResolve)] +
                         f.nanoseconds[size_t(Stage::kPresent)]) / 1e6;
      std::string stages;
      for (size_t i = 0; i < size_t(Stage::kCount); i++) {
        stages += fmt::format(" {} {:.1f}", kStageNames[i], f.nanoseconds[i] / 1e6);
      }
      REXLOG_INFO("video: hitch {:.1f} ms (renderer {:.1f}:{}); made {} pipelines {:.1f} ms, "
                  "{} shaders {:.1f} ms, {} textures {:.1f} ms {:.1f} MB",
                  ms, renderer, stages, f.pipelines, f.pipelineNs / 1e6, f.shaders,
                  f.shaderNs / 1e6, f.textures, f.textureNs / 1e6, f.textureBytes / 1048576.0);
    }
  }
  bool enabled = f.enabled;
  double lastMs = f.lastSwap != std::chrono::steady_clock::time_point()
                      ? std::chrono::duration<double, std::milli>(now - f.lastSwap).count()
                      : 0.0;
  f = FrameStats{};
  f.enabled = enabled;
  f.lastSwap = now;
  f.lastMs = lastMs;
}

// NG2_LATENCY_LOG=1: from the game starting a frame (it reads the pad then)
// to that frame's present call, averaged and worst over each second.
std::atomic<int64_t> g_frameStartNs{0};
void ReportLatency() {
  static const bool enabled = [] {
    const char* v = std::getenv("NG2_LATENCY_LOG");
    return v && v[0] == '1';
  }();
  int64_t start = g_frameStartNs.exchange(0);
  if (!enabled || !start) {
    return;
  }
  static double sum = 0.0, worst = 0.0;
  static uint32_t count = 0;
  static auto last = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  double ms = (now.time_since_epoch().count() - start) / 1e6;
  sum += ms, worst = std::max(worst, ms), count++;
  if (now - last >= std::chrono::seconds(1)) {
    REXLOG_INFO("latency frame start to present: avg {:.2f} ms, worst {:.2f} ms ({} frames)",
                sum / count, worst, count);
    sum = worst = 0.0, count = 0, last = now;
  }
}

// With NG2_PROFILE, every 5 s: the busiest threads' CPU time (ms of each
// second, from /proc; Linux only), so a slow machine's log tells which thread
// limits the frame rate. "(renderer)" is the game's render thread, which runs
// the renderer's hooks (the Vulkan side is on "Render Commands").
void ReportThreads() {
#if defined(__linux__)
  static std::unordered_map<int, uint64_t> last;
  static auto lastTime = std::chrono::steady_clock::now();
  static const int self = int(gettid());
  auto now = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(now - lastTime).count();
  if (seconds < 5.0) {
    return;
  }
  lastTime = now;
  static const double msPerTick = 1000.0 / double(sysconf(_SC_CLK_TCK));
  std::vector<std::pair<double, std::string>> busy;
  std::unordered_map<int, uint64_t> current;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", error)) {
    int tid = std::atoi(entry.path().filename().c_str());
    FILE* f = std::fopen((entry.path() / "stat").c_str(), "r");
    if (!f) {
      continue;
    }
    char text[512] = {};
    size_t n = std::fread(text, 1, sizeof(text) - 1, f);
    std::fclose(f);
    text[n] = 0;
    const char* open = std::strchr(text, '(');
    const char* close = std::strrchr(text, ')');
    if (!open || !close) {
      continue;
    }
    // After the name: state (field 3) ... utime (14), stime (15).
    unsigned long long utime = 0, stime = 0;
    if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime,
                    &stime) != 2) {
      continue;
    }
    uint64_t ticks = utime + stime;
    current[tid] = ticks;
    auto it = last.find(tid);
    if (it == last.end() || ticks < it->second) {  // new, or a reused thread id
      continue;
    }
    double ms = double(ticks - it->second) * msPerTick / seconds;
    if (ms >= 10.0) {
      std::string name(open + 1, close);
      busy.emplace_back(ms, tid == self ? name + " (renderer)" : name);
    }
  }
  last = std::move(current);
  std::sort(busy.begin(), busy.end(), std::greater<>());
  std::string line;
  for (size_t i = 0; i < busy.size() && i < 8; i++) {
    line += fmt::format("{}{} {:.0f}", i ? ", " : " ", busy[i].second, busy[i].first);
  }
  if (!line.empty()) {
    REXLOG_INFO("threads, ms of CPU each second:{}", line);
  }
#endif
}

void ReportProfile() {
  ReportHitch();
  if (g_profile.enabled) {
    ReportThreads();
  }
  g_profile.frames++;
  auto now = std::chrono::steady_clock::now();
  if (!g_profile.enabled || now - g_profile.last < std::chrono::seconds(1)) {
    return;
  }
  std::string line;
  for (size_t i = 0; i < size_t(Stage::kCount); i++) {
    line += fmt::format(" {} {:.1f}", kStageNames[i], g_profile.nanoseconds[i] / 1e6);
  }
  REXLOG_INFO("video: {} frames, ms:{}; compared {:.1f} MB ({:.1f} MB quiet for 1 s), "
              "end of frame {:.1f} MB in {:.1f} ms, uploaded {:.1f} MB; textures {} "
              "looked up, {:.1f} MB hashed, {} made {:.1f} MB",
              g_profile.frames, line, g_profile.comparedBytes / 1048576.0,
              g_profile.quietBytes / 1048576.0, g_profile.finishBytes / 1048576.0,
              g_profile.finishNs / 1e6,
              g_profile.uploadedBytes / 1048576.0, g_profile.textureLookups,
              g_profile.hashedBytes / 1048576.0, g_profile.texturesMade,
              g_profile.textureMadeBytes / 1048576.0);
  if (g_profile.gpuFrames) {
    double frames = double(g_profile.gpuFrames);
    REXLOG_INFO("video: GPU {:.2f} ms a frame: resolves {:.2f}, present {:.2f} ({} frames timed); "
                "{:.0f} passes, {:.0f} barriers a frame",
                g_profile.gpuNs / 1e6 / frames, g_profile.gpuResolveNs / 1e6 / frames,
                g_profile.gpuPresentNs / 1e6 / frames, g_profile.gpuFrames,
                g_profile.passes / double(std::max<uint64_t>(g_profile.frames, 1)),
                g_profile.barriers / double(std::max<uint64_t>(g_profile.frames, 1)));
    REXLOG_INFO("video: guest memory copies a frame: {:.1f} in the prologue, {:.1f} in order",
                g_profile.prologueCopies / double(std::max<uint64_t>(g_profile.frames, 1)),
                g_profile.inOrderCopies / double(std::max<uint64_t>(g_profile.frames, 1)));
    std::vector<std::pair<uint64_t, std::string>> passes;
    for (auto& [label, ns] : g_profile.gpuPassNs) {
      passes.emplace_back(ns, label);
    }
    std::sort(passes.rbegin(), passes.rend());
    std::string top;
    for (size_t i = 0; i < passes.size() && i < 6; i++) {
      top += fmt::format("{}{} {:.2f}", i ? "; " : "", passes[i].second, passes[i].first / 1e6 / frames);
    }
    REXLOG_INFO("video: GPU ms a frame by pass target: {}", top);
    if (!g_profile.gpuDrawNs.empty()) {
      std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> pairs;
      for (auto& [shaders, timed] : g_profile.gpuDrawNs) {
        pairs.emplace_back(timed.first, shaders);
      }
      std::sort(pairs.rbegin(), pairs.rend());
      std::string draws;
      // (NG2_PROFILE_TOP=<n>: that many pairs instead of 8)
      static const size_t top = [] {
        const char* v = std::getenv("NG2_PROFILE_TOP");
        return v ? size_t(std::max(1, std::atoi(v))) : size_t(8);
      }();
      for (size_t i = 0; i < pairs.size() && i < top; i++) {
        auto [vs, ps] = pairs[i].second;
        draws += fmt::format("{}vs {:016X} ps {:016X} {:.2f} ({:.0f} draws)", i ? "; " : "", vs, ps,
                             pairs[i].first / 1e6 / frames,
                             g_profile.gpuDrawNs[pairs[i].second].second / frames);
      }
      REXLOG_INFO("video: GPU ms a frame by shaders: {}", draws);
    }
  }
  g_profile = Profile{};
  g_profile.last = now;
}

// Guest physical memory as the GPU sees it (512 MB): vertex data read by the
// shaders and memory export targets, addressed like the console.
constexpr uint64_t GUEST_MEMORY_SIZE = 512ull << 20;
std::unique_ptr<RenderBuffer> g_guestMemory;
uint64_t g_guestMemoryAddress = 0;
// What g_guestMemory holds, byte for byte, as of the uploads recorded so far
// (calloc: untouched pages cost nothing). Ranges compare the guest's memory
// with it rather than with their own last contents: draws with different
// ranges over the same memory (a ring buffer reused with other sizes)
// overwrite each other's uploads.
uint8_t* g_guestMemoryShadow = nullptr;

// Ranges synced: the frame they were last checked in and the frame they last
// changed in. A range that hasn't changed for a frame is checked once a
// frame; one that has (a dynamic buffer the game fills in batches, which no
// GPU ever stalls here) is checked on every draw, and so is one caught
// written between its draws (dynamic) until it stays unchanged for 8 frames.
//
// Some buffers are written after the draw that reads them: the game's jobs
// fill bone matrices while the render thread goes on, since the console's
// GPU reads them only after the frame is kicked off. A range found changed
// at the end of a frame it was drawn in becomes late: its copy is still
// recorded at its first draw in a frame, but filled from guest memory just
// before the frame is submitted.
//
// Bone palettes (fetch slot 94) are late from their first draw: they rotate
// through three buffers, and a buffer the jobs fill only after the frame's
// draws looked unchanged at its draws, was never caught being written after
// them, and its draws used the bones of three frames before (Ryu's eyelids
// in cutscenes; how far the jobs get depends on CPU timing).
struct GuestMemoryRange {
  uint64_t checkedFrame = ~0ull;
  uint64_t changedFrame = 0;
  uint64_t lateFrame = ~0ull;  // frame its late copy was recorded in
  bool late = false;
  // Found written between its draws while checked once a frame: checked at every draw until
  // it stays unchanged for a while.
  bool dynamic = false;
  uint8_t finishTurn = 0;  // (FinishGuestMemory's 1 in 8)
};
std::unordered_map<uint64_t, GuestMemoryRange> g_guestMemoryRanges;
constexpr uint64_t kRangeStableFrames = 600;
constexpr uint64_t kRangeRecheckFrames = 4;
struct FrameRange {
  uint32_t address, size;
  GuestMemoryRange* range;
  uint8_t* lateCopy;  // upload memory to fill, if late
};
// Ranges drawn with this frame, each once.
std::vector<FrameRange> g_frameRanges;

// Per-frame upload memory for constants and copies.
constexpr uint64_t UPLOAD_CHUNK_SIZE = 32ull << 20;
struct UploadChunk {
  std::unique_ptr<RenderBuffer> buffer;
  uint64_t size = 0;
  uint8_t* data = nullptr;
  uint64_t address = 0;
};
struct UploadAllocator {
  std::vector<UploadChunk> chunks;
  size_t chunk = 0;
  uint64_t offset = 0;
  void Reset() {
    chunk = 0;
    offset = 0;
  }
};
UploadAllocator g_uploads[NUM_FRAMES];
// The pictures between's constants and bones, made on their own thread while
// the game goes on (NG2_INTERP): reset with the frame's.
UploadAllocator g_interpUploads[NUM_FRAMES];
struct Upload {
  RenderBuffer* buffer;
  uint64_t offset;
  uint8_t* data;
  uint64_t address;
};

// Shaders created by the game, by guest object.
struct GuestShader {
  uint32_t container = 0;
  const ShaderCacheEntry* entry = nullptr;
  // Vertex shaders: usage and usage index of each declared element slot.
  std::vector<std::pair<uint8_t, uint8_t>> elements;
  // Pixel shaders: color outputs written (bit n = oCn).
  uint32_t colorOutputs = 0;
};
std::unordered_map<uint32_t, std::unique_ptr<GuestShader>> g_guestShaders;
std::unique_ptr<uint8_t[]> g_spirvCache;
// Host shaders by cache entry: they live as long as the device, so pipelines
// can refer to them.
std::unordered_map<const ShaderCacheEntry*, std::unique_ptr<RenderShader>> g_hostShaders;

std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> g_pipelines;

// Guest textures by fetch constant, with the hash of their memory when last
// uploaded: checked once a frame, or every kTextureRecheckFrames frames it is
// drawn with once unchanged for kTextureStableFrames (hashing every bound
// texture each frame was ~40% of the draw stage). A texture rewritten in place
// after that shows up to that many frames late.
constexpr uint64_t kTextureStableFrames = 120;
constexpr uint64_t kTextureRecheckFrames = 8;
struct CachedTexture {
  std::unique_ptr<HostTexture> host;
  uint64_t contentHash = 0;
  uint64_t checkedFrame = ~0ull;
  uint64_t changedFrame = 0;
};
std::unordered_map<uint64_t, CachedTexture> g_textures;
// Host textures replaced while a frame may still use them: freed once that
// frame's fence has passed.
std::vector<std::unique_ptr<HostTexture>> g_garbage[NUM_FRAMES];
uint64_t g_frameCount = 0;
// Render scale: render targets and the textures resolved from them have this
// many host pixels per guest pixel each way (render_scale, applied between
// frames by ApplyRenderScale).
uint32_t g_resolutionScale = 1;
// MSAA samples for the targets the game multisamples (antialiasing, applied
// with the render scale).
uint32_t g_antialiasing = 2;
// The setting, stepped down to what the device multisamples for the color
// and depth formats the game's multisampled targets use.
uint32_t AntialiasingSetting() {
  int32_t setting = REXCVAR_GET(antialiasing);
  uint32_t samples = setting >= 8 ? 8 : setting >= 4 ? 4 : setting >= 2 ? 2 : 0;
  if (!g_device) {
    return samples;
  }
  for (; samples > 2; samples /= 2) {
    bool supported = true;
    for (RenderFormat format :
         {RenderFormat::R8G8B8A8_UNORM, RenderFormat::R16G16B16A16_FLOAT,
          RenderFormat::D32_FLOAT_S8_UINT}) {
      supported &= (g_device->getSampleCountsSupported(format) & samples) != 0;
    }
    if (supported) {
      break;
    }
  }
  return samples;
}
// The Options header picture's guest address, and how much wider ours is
// than the game's (1: the game's is shown).
struct MenuHeader {
  uint32_t base = ~0u;
  float widthScale = 1.0f;
} g_menuHeader;
// The in-game list's header picture's guest address (ours is drawn at the
// game's width).
uint32_t g_inGameHeaderBase = ~0u;
// The display gamma table (UpdateGammaTable).
std::unique_ptr<HostTexture> g_gammaTable;
uint64_t g_gammaTableHash = 0;

// NG2_DEBUG_HIGHLIGHT=<vs hash>: that vertex shader's draws in magenta.
// =cycle:<seconds>: from that long after the first draw, each vertex shader
// seen in turn for 2 s (logged), to find which draws are which.
// =all: every skinned vertex shader in its own color (DebugColor, logged).
// =rigid: the unskinned ones, small draws only. =<hash>,<hash>...: those, a
// color each.
bool Highlighted(uint64_t vertexShader) {
  static const char* spec = std::getenv("NG2_DEBUG_HIGHLIGHT");
  if (!spec) {
    return false;
  }
  if (std::strcmp(spec, "all") == 0 || std::strcmp(spec, "rigid") == 0) {
    return true;
  }
  static const bool cycle = std::strncmp(spec, "cycle:", 6) == 0;
  if (!cycle && std::strchr(spec, ',')) {  // <hash>,<hash>...: any of them
    return std::strstr(spec, fmt::format("{:016X}", vertexShader).c_str()) != nullptr;
  }
  if (!cycle) {
    static const uint64_t hash = std::strtoull(spec, nullptr, 16);
    return vertexShader == hash;
  }
  static const auto start = std::chrono::steady_clock::now();
  static const double begin = std::strtod(spec + 6, nullptr);
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (elapsed < begin) {
    return false;
  }
  static std::vector<uint64_t> seen;
  if (std::find(seen.begin(), seen.end(), vertexShader) == seen.end()) {
    seen.push_back(vertexShader);
  }
  size_t index = size_t((elapsed - begin) / 2.0) % seen.size();
  static size_t logged = ~size_t(0);
  if (index != logged) {
    logged = index;
    REXLOG_INFO("video: highlighting vs {:016X} ({} of {})", seen[index], index, seen.size());
  }
  return vertexShader == seen[index];
}

// The highlight color for a vertex shader: magenta, or with
// NG2_DEBUG_HIGHLIGHT=all, rigid or a list one from its hash, logged the first time.
void DebugColor(uint64_t vertexShader, float color[4]) {
  static const char* spec = std::getenv("NG2_DEBUG_HIGHLIGHT");
  if (!spec || (std::strcmp(spec, "all") != 0 && std::strcmp(spec, "rigid") != 0 &&
                !std::strchr(spec, ','))) {
    color[0] = 1.0f, color[1] = 0.0f, color[2] = 1.0f, color[3] = 1.0f;
    return;
  }
  uint64_t mixed = XXH3_64bits(&vertexShader, sizeof(vertexShader));
  uint32_t rgb[3] = {uint32_t(mixed & 0xFF), uint32_t((mixed >> 8) & 0xFF),
                     uint32_t((mixed >> 16) & 0xFF)};
  static std::unordered_set<uint64_t> logged;
  if (logged.insert(vertexShader).second) {
    REXLOG_INFO("video: vs {:016X} color {} {} {}", vertexShader, rgb[0], rgb[1], rgb[2]);
  }
  for (int i = 0; i < 3; i++) {
    color[i] = float(rgb[i]) / 255.0f;
  }
  color[3] = 1.0f;
}

// F7: the next frames' draws (NG2_TRACE_FRAMES, 60 by default) into
// ~/ng2-trace-<n>/draws.txt, with a small picture every 10 frames, for bug
// reports. The key is seen on SDL's event thread; the trace starts at the
// next present.
std::atomic<bool> g_traceRequest{false};
FILE* g_traceFile = nullptr;
std::string g_traceDir;
int64_t g_traceFrames = 0, g_traceLeft = 0;
// F7's first frame: each draw's vertex shader constants (256 x 16 bytes, as
// the guest has them: big-endian), in draw order, beside its draw lines.
FILE* g_traceConstants = nullptr;
// NG2_TRACE_ALL_CONSTANTS=1: every traced frame's, and the pixel shader
// constants (224 x 16 bytes) into ps-constants-<n>.bin too.
FILE* g_tracePsConstants = nullptr;
bool TraceAllConstants() {
  static const bool all = [] {
    const char* v = std::getenv("NG2_TRACE_ALL_CONSTANTS");
    return v && *v == '1';
  }();
  return all;
}
thread_local std::string g_drawStack;  // NG2_TRACE_STACKS: the last draw's guest call chain

bool SDLCALL WatchTraceKey(void*, SDL_Event* event) {
  if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_F7 && !event->key.repeat) {
    g_traceRequest = true;
  }
  return true;
}

// A line of a trace: into F7's file while it records, else the log.
template <typename... Args>
void TraceLog(fmt::format_string<Args...> format, Args&&... args) {
  if (g_traceFile) {
    fmt::print(g_traceFile, format, std::forward<Args>(args)...);
    std::fputc('\n', g_traceFile);
  } else {
    REXLOG_INFO("{}", fmt::format(format, std::forward<Args>(args)...));
  }
}

// NG2_TRACE_FRAME=<n>: log what frame n draws; t<seconds>: the first frame
// that starts that long after the first draw. A +<count> suffix traces that
// many frames from there.
// NG2_TRACE_PICTURES_ONLY=1: F7 takes the pictures alone, no draws or memory (a
// trace's own slowness can hide what it's after).
bool TracePicturesOnly() {
  static const bool only = [] {
    const char* v = std::getenv("NG2_TRACE_PICTURES_ONLY");
    return v && *v && *v != '0';
  }();
  return only;
}

bool Tracing() {
  if (g_traceFile) {
    return !TracePicturesOnly();
  }
  static const char* spec = std::getenv("NG2_TRACE_FRAME");
  static const bool byTime = spec && spec[0] == 't';
  static int64_t frame = spec && !byTime ? std::strtoll(spec, nullptr, 10) : -1ll;
  static const int64_t count = [] {
    const char* plus = spec ? std::strchr(spec, '+') : nullptr;
    return plus ? std::max<int64_t>(std::strtoll(plus + 1, nullptr, 10), 1) : 1;
  }();
  if (byTime && frame < 0) {
    static const auto start = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - start >=
        std::chrono::duration<double>(std::strtod(spec + 1, nullptr))) {
      frame = int64_t(g_frameCount) + 1;
    }
  }
  return frame >= 0 && int64_t(g_frameCount) >= frame && int64_t(g_frameCount) < frame + count;
}

std::unordered_map<uint64_t, std::pair<uint32_t, std::unique_ptr<RenderSampler>>> g_samplers;
uint32_t g_nextSampler = SAMPLER_FIRST_FREE;

// Render targets by EDRAM placement and shape.
struct SurfaceKey {
  uint32_t edramBase;
  uint32_t format;  // host RenderFormat, or 0x100 for depth
  uint32_t width;
  uint32_t height;
  uint32_t msaa;
  bool operator==(const SurfaceKey&) const = default;
};
struct SurfaceKeyHash {
  size_t operator()(const SurfaceKey& k) const {
    return (size_t(k.edramBase) * 0x9E3779B1u) ^ (size_t(k.format) << 20) ^
           (size_t(k.width) << 40) ^ (size_t(k.height) << 8) ^ k.msaa;
  }
};
std::unordered_map<SurfaceKey, std::unique_ptr<HostTexture>, SurfaceKeyHash> g_surfaces;

// Textures written by resolves, by guest physical base address and shape
// (ResolvedKey): different shapes can share an address, e.g. two shadow map
// sizes resolved to the same memory in one frame.
std::unordered_map<uint64_t, std::unique_ptr<HostTexture>> g_resolvedTextures;
// The latest resolve to each address, for a texture that views it with
// another shape or format.
std::unordered_map<uint32_t, HostTexture*> g_lastResolve;
// Resolves don't write guest memory, so the memory a resolve targets keeps
// its old bytes. When the CPU later writes there (a texture loaded into memory
// that was once a render target), the resolved textures for that address are
// stale and the guest texture is read instead.
struct ResolvedMemory {
  uint32_t size = 0;
  uint64_t hash = 0;
  uint64_t checkedFrame = ~0ull;
  bool stale = false;
};
std::unordered_map<uint32_t, ResolvedMemory> g_resolvedMemory;

// Framebuffers by attachment.
struct FramebufferKey {
  const HostTexture* colors[4] = {};
  const HostTexture* depth = nullptr;
  bool operator==(const FramebufferKey&) const = default;
};
struct FramebufferKeyHash {
  size_t operator()(const FramebufferKey& k) const {
    return XXH3_64bits(&k, sizeof(k));
  }
};
std::unordered_map<FramebufferKey, std::unique_ptr<RenderFramebuffer>, FramebufferKeyHash>
    g_framebuffers;

// The framebuffer last set on the command list (plume resumes it after
// barriers and copies end the render pass).
const RenderFramebuffer* g_boundFramebuffer = nullptr;

// Drops cached framebuffers that use a texture about to be freed.
void ForgetTexture(const HostTexture* texture) {
  // (the last resolve to an address, if it's this one: it's being freed)
  for (auto it = g_lastResolve.begin(); it != g_lastResolve.end();) {
    it = it->second == texture ? g_lastResolve.erase(it) : std::next(it);
  }
  for (auto it = g_framebuffers.begin(); it != g_framebuffers.end();) {
    const auto& key = it->first;
    if (std::find(std::begin(key.colors), std::end(key.colors), texture) != std::end(key.colors) ||
        key.depth == texture) {
      if (g_boundFramebuffer == it->second.get()) {
        g_boundFramebuffer = nullptr;
      }
      it = g_framebuffers.erase(it);
    } else {
      ++it;
    }
  }
}


// Formats ---------------------------------------------------------------------

RenderFormat ColorTargetFormat(uint32_t format) {
  switch (xenos::ColorRenderTargetFormat(format)) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return RenderFormat::R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return RenderFormat::R16G16B16A16_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return RenderFormat::R16G16B16A16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return RenderFormat::R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return RenderFormat::R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return RenderFormat::R16G16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return RenderFormat::R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return RenderFormat::R32G32_FLOAT;
  }
  return RenderFormat::R8G8B8A8_UNORM;
}

// Test switches for #2 (AMD's Windows driver: the draws that sample the
// shadow map, a 16_16 resolve, cost 2-4 ms each there).
// NG2_TEST_SHADOW_FORMAT=float: 16_16 resolves kept as R16G16_FLOAT (the same
// range, less precision) instead of R16G16_SNORM. NG2_TEST_NO_BORDER=1: clamp
// to border samples as clamp to edge.
bool TestSwitch(const char* name, const char* value) {
  const char* v = std::getenv(name);
  bool on = v && std::strcmp(v, value) == 0;
  if (on) {
    REXLOG_INFO("video: test switch {}={}", name, value);
  }
  return on;
}
bool TestShadowFloat() {
  static const bool on = TestSwitch("NG2_TEST_SHADOW_FORMAT", "float");
  return on;
}
bool TestNoBorder() {
  static const bool on = TestSwitch("NG2_TEST_NO_BORDER", "1");
  return on;
}

// Even presents (#36, #46; on by default since 0.5.58, NG2_TEST_EVEN_PRESENT=0
// turns them off): a present that would follow the last by less than 90% of a
// refresh waits until then. With a frame's work near a
// refresh the game hands frames over in pairs (3 then 33 ms apart); this
// spreads them out so the display's queue doesn't run dry between them, at up
// to a frame of latency while it happens. The last part of the wait spins
// (Windows' sleeps are whole milliseconds).
void EvenOutPresent() {
  static const bool on = !TestSwitch("NG2_TEST_EVEN_PRESENT", "0");
  // (with V-Sync on: FIFO, Mailbox or Adaptive; off, the game runs uncapped
  // and this would cap it near the refresh rate)
  int32_t mode = REXCVAR_GET(present_mode);
  if (!on || !REXCVAR_GET(vsync) || (mode != 1 && mode != 2 && mode != 3)) {
    return;
  }
  static int64_t last = 0;
  int64_t refresh = 0, period = 16'666'667;
  int64_t measured = 0;
  if (DisplayRefresh(&refresh, &measured) && measured > 4'000'000 && measured < 50'000'000) {
    // (a frame's: the whole refreshes nearest 60 Hz, 2 on a 120 Hz panel)
    period = measured * std::clamp<int64_t>((16'666'667 + measured / 2) / measured, 1, 4);
  }
  int64_t now = SteadyNs();
  int64_t until = last + period * 9 / 10;
  if (last && now < until) {
#if defined(_WIN32)
    constexpr int64_t kSpin = 1'500'000;
#else
    constexpr int64_t kSpin = 300'000;
#endif
    if (until - now > kSpin) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(until - now - kSpin));
    }
    while (SteadyNs() < until) {
      std::this_thread::yield();
    }
  }
  last = SteadyNs();
}

// Host format for a resolve destination (a guest texture written by the GPU).
RenderFormat ResolveTextureFormat(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
      return RenderFormat::R8G8B8A8_UNORM;
    case xenos::TextureFormat::k_2_10_10_10:
    // (the lens flare's sun test resolves its 1x1 counts into these: without
    // a host texture they were dropped and the flare read stale memory, #95)
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
    case xenos::TextureFormat::k_16_16_16_16:
      return RenderFormat::R16G16B16A16_UNORM;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16_EXPAND:
      return RenderFormat::R16G16B16A16_FLOAT;
    case xenos::TextureFormat::k_16_16:
      return TestShadowFloat() ? RenderFormat::R16G16_FLOAT : RenderFormat::R16G16_SNORM;
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_EXPAND:
      return RenderFormat::R16G16_FLOAT;
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
      return RenderFormat::R32_FLOAT;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return RenderFormat::R32G32_FLOAT;
    case xenos::TextureFormat::k_5_6_5:
      // (the game over screen keeps the last picture of play in one, #88:
      // without a host texture its resolves were lost and it read black)
      return RenderFormat::B5G6R5_UNORM;
    default:
      return RenderFormat::UNKNOWN;
  }
}

// A guest target's sample count on the host: the antialiasing setting's for
// targets the game multisamples (at least the game's), none for the others.
RenderSampleCounts SampleCount(uint32_t msaa) {
  if ((msaa != 1 && msaa != 2) || g_antialiasing < 2) {
    return RenderSampleCount::COUNT_1;
  }
  uint32_t samples = std::max(g_antialiasing, msaa == 1 ? 2u : 4u);
  return samples >= 8   ? RenderSampleCount::COUNT_8
         : samples >= 4 ? RenderSampleCount::COUNT_4
                        : RenderSampleCount::COUNT_2;
}

// Render thread -----------------------------------------------------------------
//
// The game's render thread calls the hooks, which read guest memory (device
// state, constants, vertex and index data, textures) into upload memory
// there: the guest changes it right after. What goes to Vulkan (commands,
// descriptor writes, submits and presents) is queued instead, in order, and
// a thread of ours does it (NG2_RENDER_THREAD=0: at once, as before). The
// game thread stays at most a frame ahead: before it reuses a frame's upload
// memory it waits for that frame's submit and fence; only it waits on fences
// (plume's wait resets them). Anything that needs the GPU idle drains the
// queue first (Flush).

bool RenderThreadEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("NG2_RENDER_THREAD");
    return !(v && v[0] == '0');
  }();
  return enabled;
}

// The render thread's current command list (the frame's, from its begin).
RenderCommandList* g_threadList = nullptr;

// Queued work: each a callable stored in a chunked arena (no allocation per
// item), run with the frame's real command list.
class OpArena {
 public:
  using Run = void (*)(void* op, RenderCommandList* list);
  using Destroy = void (*)(void* op);
  struct Header {
    Run run;
    Destroy destroy;
    uint32_t size;  // header and op, aligned
  };

  template <typename F>
  void Push(F&& f) {
    using T = std::decay_t<F>;
    constexpr uint32_t kAlign = 16;
    constexpr uint32_t size = (kHeaderSize + uint32_t(sizeof(T)) + kAlign - 1) & ~(kAlign - 1);
    static_assert(alignof(T) <= kAlign);
    uint8_t* at = Reserve(size);
    auto* header = reinterpret_cast<Header*>(at);
    header->run = [](void* op, RenderCommandList* list) { (*static_cast<T*>(op))(list); };
    header->destroy = [](void* op) { static_cast<T*>(op)->~T(); };
    header->size = size;
    new (at + kHeaderSize) T(std::forward<F>(f));
    count_++;
  }

  // Runs every op in order, then forgets them (the chunks are kept).
  void RunAll() {
    for (size_t c = 0; c < chunks_.size(); c++) {
      Chunk& chunk = chunks_[c];
      for (size_t at = 0; at < chunk.used;) {
        auto* header = reinterpret_cast<Header*>(chunk.data.get() + at);
        void* op = chunk.data.get() + at + kHeaderSize;
        header->run(op, g_threadList);
        header->destroy(op);
        at += header->size;
      }
      chunk.used = 0;
    }
    current_ = 0;
    count_ = 0;
  }

  // Runs every op in order and keeps them (a frame's log, run again).
  void RunKeep(RenderCommandList* list) {
    for (size_t c = 0; c < chunks_.size(); c++) {
      Chunk& chunk = chunks_[c];
      for (size_t at = 0; at < chunk.used;) {
        auto* header = reinterpret_cast<Header*>(chunk.data.get() + at);
        header->run(chunk.data.get() + at + kHeaderSize, list);
        at += header->size;
      }
    }
  }

  // Forgets every op without running it.
  void Clear() {
    for (size_t c = 0; c < chunks_.size(); c++) {
      Chunk& chunk = chunks_[c];
      for (size_t at = 0; at < chunk.used;) {
        auto* header = reinterpret_cast<Header*>(chunk.data.get() + at);
        header->destroy(chunk.data.get() + at + kHeaderSize);
        at += header->size;
      }
      chunk.used = 0;
    }
    current_ = 0;
    count_ = 0;
  }

  size_t count() const { return count_; }

 private:
  static constexpr uint32_t kHeaderSize = (sizeof(Header) + 15) & ~15u;
  static constexpr size_t kChunkSize = 256 << 10;
  struct Chunk {
    std::unique_ptr<uint8_t[]> data;
    size_t used = 0;
  };

  uint8_t* Reserve(uint32_t size) {
    while (current_ < chunks_.size() && chunks_[current_].used + size > kChunkSize) {
      current_++;
    }
    if (current_ == chunks_.size()) {
      Chunk chunk;
      chunk.data.reset(new uint8_t[kChunkSize]);  // (new is 16-byte aligned on x64)
      chunks_.push_back(std::move(chunk));
    }
    Chunk& chunk = chunks_[current_];
    uint8_t* at = chunk.data.get() + chunk.used;
    chunk.used += size;
    return at;
  }

  std::vector<Chunk> chunks_;
  size_t current_ = 0;
  size_t count_ = 0;
};

// Frame interpolation (NG2_INTERP=1, with the render thread): each frame's
// commands are kept (g_frameLogs, per frame slot) and run again at its
// present with each draw's vertex constants part of the way from the last
// frame's, for pictures between the two (interp.cpp).
OpArena g_frameLogs[NUM_FRAMES];
bool g_logOps = false;     // game thread: commands are being kept
int32_t g_drawTag = -1;    // game thread: the draw whose push constants come next
// Render thread: while a log runs again, each tagged draw's push constants.
thread_local const std::vector<std::array<uint64_t, 3>>* g_replayConstants = nullptr;
// Bone palettes (fetch slot 94, in guest memory): the copy that fills one is
// tagged with its index in the frame, and filled from elsewhere when the log
// runs again (g_replayCopies, by palette; ref null = as it was).
int32_t g_copyTag = -1;
thread_local const std::vector<RenderBufferReference>* g_replayCopies = nullptr;
// Draws that export to memory (memexport) aren't run again for the pictures:
// what they write is the frame's, once (g_drawMemExport: the next draw is one).
bool g_drawMemExport = false;

// What a frame carries from the last one in textures: a resolved texture
// read in the frame before anything is resolved to it (the eye's adaptation
// to the brightness, afterimages and glows mixed with the last frame's).
// Each picture between runs the whole frame again, so each would move that
// state on once more (the effect sped up, and the next frame starting from a
// picture's state instead of the last frame's): it's saved before the
// pictures and put back after each one (SaveCarried, RestoreCarried).
std::unordered_map<HostTexture*, bool> g_carriedTouch;  // game thread: the frame's first use, true if read
void NoteCarriedRead(HostTexture* texture) {
  if (g_logOps) {
    g_carriedTouch.emplace(texture, true);
  }
}
void NoteCarriedWrite(HostTexture* texture) {
  if (g_logOps) {
    g_carriedTouch.emplace(texture, false);
  }
}
struct CarriedTexture {
  HostTexture* texture;  // (freed frames later, through g_garbage)
  RenderTextureLayout layout;  // its layout at the frame's end, as the frame left it
};
// The frame's carried textures, and the touches forgotten for the next.
std::vector<CarriedTexture> TakeCarried() {
  std::vector<CarriedTexture> carried;
  for (auto [texture, read] : g_carriedTouch) {
    if (read && texture->texture && texture->samples == RenderSampleCount::COUNT_1) {
      carried.push_back({texture, texture->layout});
    }
  }
  g_carriedTouch.clear();
  static size_t logged = ~size_t(0);
  if (carried.size() != logged) {
    logged = carried.size();
    std::string list;
    for (auto& c : carried) {
      list += fmt::format(" {}x{}", c.texture->width, c.texture->height);
    }
    REXLOG_INFO("video: interp: {} textures carried from frame to frame:{}", carried.size(), list);
  }
  return carried;
}
// Render thread: each carried texture's saved copy, kept while it's carried
// (one no longer used goes after a few frames: the GPU may still read it).
struct CarriedCopy {
  std::unique_ptr<RenderTexture> texture;
  uint32_t width = 0, height = 0;
  RenderFormat format = RenderFormat::UNKNOWN;
  uint64_t usedFrame = 0;
};
std::unordered_map<const HostTexture*, CarriedCopy> g_carriedCopies;
std::vector<std::pair<uint64_t, std::unique_ptr<RenderTexture>>> g_carriedRetired;
uint64_t g_carriedFrame = 0;
RenderTexture* CarriedCopyOf(const HostTexture& texture) {
  auto& copy = g_carriedCopies[&texture];
  if (copy.texture && (copy.width != texture.width || copy.height != texture.height ||
                       copy.format != texture.format)) {
    g_carriedRetired.emplace_back(g_carriedFrame, std::move(copy.texture));
  }
  if (!copy.texture) {
    copy.texture = g_device->createTexture(RenderTextureDesc::Texture2D(
        texture.width, texture.height, 1, texture.format, RenderTextureFlag::RENDER_TARGET));
    copy.width = texture.width, copy.height = texture.height, copy.format = texture.format;
  }
  copy.usedFrame = g_carriedFrame;
  return copy.texture.get();
}
// Before the first picture: each carried texture copied aside.
void SaveCarried(RenderCommandList* l, const std::vector<CarriedTexture>& carried) {
  g_carriedFrame++;
  for (auto& c : carried) {
    RenderTexture* copy = CarriedCopyOf(*c.texture);
    if (!copy) {
      continue;
    }
    l->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY,
                RenderTextureBarrier(c.texture->texture.get(), RenderTextureLayout::COPY_SOURCE));
    l->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(copy, RenderTextureLayout::COPY_DEST));
    l->copyTextureRegion(RenderTextureCopyLocation::Subresource(copy),
                         RenderTextureCopyLocation::Subresource(c.texture->texture.get()));
    if (c.layout != RenderTextureLayout::UNKNOWN) {
      l->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY,
                  RenderTextureBarrier(c.texture->texture.get(), c.layout));
    }
  }
  // Copies of textures no longer carried, and replaced ones, freed a few frames on.
  for (auto it = g_carriedCopies.begin(); it != g_carriedCopies.end();) {
    if (g_carriedFrame - it->second.usedFrame > 8) {
      g_carriedRetired.emplace_back(g_carriedFrame, std::move(it->second.texture));
      it = g_carriedCopies.erase(it);
    } else {
      ++it;
    }
  }
  std::erase_if(g_carriedRetired, [](auto& retired) { return g_carriedFrame - retired.first > 8; });
}
// After each picture: the carried textures as the last frame left them.
void RestoreCarried(RenderCommandList* l, const std::vector<CarriedTexture>& carried) {
  for (auto& c : carried) {
    auto it = g_carriedCopies.find(c.texture);
    if (it == g_carriedCopies.end() || !it->second.texture) {
      continue;
    }
    RenderTexture* copy = it->second.texture.get();
    l->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(copy, RenderTextureLayout::COPY_SOURCE));
    l->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY,
                RenderTextureBarrier(c.texture->texture.get(), RenderTextureLayout::COPY_DEST));
    l->copyTextureRegion(RenderTextureCopyLocation::Subresource(c.texture->texture.get()),
                         RenderTextureCopyLocation::Subresource(copy));
    l->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(copy, RenderTextureLayout::COPY_DEST));
    if (c.layout != RenderTextureLayout::UNKNOWN) {
      l->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY,
                  RenderTextureBarrier(c.texture->texture.get(), c.layout));
    }
  }
}

// The textures' layouts around the pictures. plume keeps a texture's layout
// on the object, as last recorded; the pictures run the frame's commands
// again, which were recorded for the layouts the frame found, but are
// recorded after the frame's own commands (and each after the one before),
// so plume would take those commands' last layouts for where a picture
// starts (a texture made this frame: as if loaded already). Each texture's
// layout as the frame found it is noted (game thread, in Transition, while
// the frame's commands are kept); each picture is recorded from those, and
// ends by putting the textures back to them, so the next picture and the
// frame's own commands start from them.
using TextureLayouts = std::vector<std::pair<HostTexture*, RenderTextureLayout>>;
std::unordered_map<HostTexture*, RenderTextureLayout> g_frameStartLayouts;
void NoteStartLayout(HostTexture& texture) {
  if (g_logOps) {
    g_frameStartLayouts.emplace(&texture, texture.layout);
  }
}
struct PictureLayouts {
  TextureLayouts start;  // as the frame found them
  TextureLayouts end;    // as a picture's commands (and the frame's) leave them
  // The frame's (game thread), and the notes forgotten for the next.
  void Take() {
    start.assign(g_frameStartLayouts.begin(), g_frameStartLayouts.end());
    g_frameStartLayouts.clear();
  }
  static void Track(const TextureLayouts& layouts) {  // (plume's idea of them)
    for (auto [texture, layout] : layouts) {
      if (texture->texture) {
        static_cast<plume::VulkanTexture*>(texture->texture.get())->textureLayout = layout;
      }
    }
  }
  // Before a picture's commands are recorded.
  void BeforeRecord() const { Track(start); }
  // After the first picture's: where they leave each texture.
  void NoteEnd() {
    end = start;
    for (auto& [texture, layout] : end) {
      if (texture->texture) {
        layout = static_cast<plume::VulkanTexture*>(texture->texture.get())->textureLayout;
      }
    }
  }
  // Before a picture's submit (its blit and the rest recorded then), and
  // after the pictures (for the frame's present and the next frame).
  void AtEnd() const { Track(end); }
  // Barriers back to where the frame started.
  void BackToStart(RenderCommandList* l) const {
    for (auto [texture, layout] : start) {
      auto* vk = texture->texture ? static_cast<plume::VulkanTexture*>(texture->texture.get()) : nullptr;
      if (vk && layout != RenderTextureLayout::UNKNOWN && vk->textureLayout != layout) {
        l->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY,
                    RenderTextureBarrier(texture->texture.get(), layout));
      }
    }
  }
};

// NG2_PROFILE: how long the Render Commands thread ran batches (ns), and of
// that in the presents (ns, by part), logged once a second by the presents.
std::atomic<int64_t> g_renderBusyNs{0};
class RenderThread {
 public:
  // At exit without DestroyHostDevice: let the thread go (joining could wait
  // on the driver; a joinable std::thread would terminate the process).
  ~RenderThread() {
    if (thread_.joinable()) {
      {
        std::lock_guard lock(mutex_);
        stop_ = true;
      }
      cv_.notify_one();
      thread_.detach();
    }
  }

  // On the game thread (under g_mutex): queued, sent on in batches.
  template <typename F>
  void Push(F&& f) {
    if (!batch_) {
      batch_ = TakeBatch();
    }
    batch_->Push(std::forward<F>(f));
    if (batch_->count() >= kBatchOps) {
      Send();
    }
  }

  // The queued work so far to the thread.
  void Send() {
    if (!batch_ || batch_->count() == 0) {
      return;
    }
    {
      std::lock_guard lock(mutex_);
      if (stop_) {
        batch_.reset();  // after Stop: nothing runs it any more
        return;
      }
      queue_.push_back(std::move(batch_));
      if (!thread_.joinable()) {
        thread_ = std::thread([this] { Main(); });
      }
    }
    cv_.notify_one();
  }

  // Until everything queued has been done.
  void Flush() {
    Send();
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return queue_.empty() && !busy_; });
  }

  void Stop() {
    Flush();
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  // Frames submitted by the thread (the game thread's frame count after each).
  std::atomic<uint64_t> submitted{0};
  std::mutex submitMutex;
  std::condition_variable submitCv;

 private:
  static constexpr size_t kBatchOps = 256;

  std::unique_ptr<OpArena> TakeBatch() {
    std::lock_guard lock(mutex_);
    if (!free_.empty()) {
      auto batch = std::move(free_.back());
      free_.pop_back();
      return batch;
    }
    return std::make_unique<OpArena>();
  }

  void Main() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "Render Commands");
#endif
    while (true) {
      std::unique_ptr<OpArena> batch;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return !queue_.empty() || stop_; });
        if (queue_.empty()) {
          return;
        }
        batch = std::move(queue_.front());
        queue_.pop_front();
        busy_ = true;
      }
      // (timed for NG2_PROFILE's log only)
      const bool timed = g_profile.enabled;
      auto runStart = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      batch->RunAll();
      if (timed) {
        g_renderBusyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - runStart).count();
      }
      {
        std::lock_guard lock(mutex_);
        free_.push_back(std::move(batch));
        busy_ = false;
        if (queue_.empty()) {
          idle_.notify_all();
        }
      }
    }
  }

  std::unique_ptr<OpArena> batch_;
  std::mutex mutex_;
  std::condition_variable cv_, idle_;
  std::deque<std::unique_ptr<OpArena>> queue_;
  std::vector<std::unique_ptr<OpArena>> free_;
  bool busy_ = false, stop_ = false;
  std::thread thread_;
};
RenderThread g_renderThread;

// Frames handed to the thread (game thread) and their submits (the thread):
// a frame's slot is reused once its submit is in and its fence has passed.
uint64_t g_framesQueued = 0;
uint64_t g_submitTicket[NUM_FRAMES] = {};
// The thread found the swapchain out of date: the game thread rebuilds it
// with the GPU idle, at its next frame.
std::atomic<bool> g_resizeRequested{false};

void FrameSubmitted() {
  {
    std::lock_guard lock(g_renderThread.submitMutex);
    g_renderThread.submitted.fetch_add(1, std::memory_order_release);
  }
  g_renderThread.submitCv.notify_all();
}

// The command list the hooks record into: every call is queued for the
// thread, its arguments copied.
class QueuedCommandList final : public RenderCommandList {
 public:
  template <typename F>
  void Op(F&& f) {
    if (g_logOps) {
      g_frameLogs[g_frame].Push(f);  // (a copy, run again for the pictures between: NG2_INTERP)
    }
    g_renderThread.Push(std::forward<F>(f));
  }

  void begin() override {}
  void end() override {}
  void barriers(RenderBarrierStages stages, const RenderBufferBarrier* bufferBarriers,
                uint32_t bufferCount, const RenderTextureBarrier* textureBarriers,
                uint32_t textureCount) override {
    std::vector<RenderBufferBarrier> buffers(bufferBarriers, bufferBarriers + bufferCount);
    std::vector<RenderTextureBarrier> textures(textureBarriers, textureBarriers + textureCount);
    Op([stages, buffers = std::move(buffers), textures = std::move(textures)](RenderCommandList* l) {
      l->barriers(stages, buffers.data(), uint32_t(buffers.size()), textures.data(),
                  uint32_t(textures.size()));
    });
  }
  void drawInstanced(uint32_t a, uint32_t b, uint32_t c, uint32_t d) override {
    bool once = g_logOps && g_drawMemExport;  // (not in the pictures: g_replayConstants set)
    Op([=](RenderCommandList* l) {
      if (!(once && g_replayConstants)) {
        l->drawInstanced(a, b, c, d);
      }
    });
  }
  void drawIndexedInstanced(uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) override {
    bool once = g_logOps && g_drawMemExport;
    Op([=](RenderCommandList* l) {
      if (!(once && g_replayConstants)) {
        l->drawIndexedInstanced(a, b, c, d, e);
      }
    });
  }
  void setPipeline(const RenderPipeline* p) override {
    Op([=](RenderCommandList* l) { l->setPipeline(p); });
  }
  void setBlendConstants(const float constants[4]) override {
    std::array<float, 4> c = {constants[0], constants[1], constants[2], constants[3]};
    Op([=](RenderCommandList* l) { l->setBlendConstants(c.data()); });
  }
  void setGraphicsPipelineLayout(const RenderPipelineLayout* p) override {
    Op([=](RenderCommandList* l) { l->setGraphicsPipelineLayout(p); });
  }
  void setGraphicsPushConstants(uint32_t range, const void* data, uint32_t offset,
                                uint32_t size) override {
    std::array<uint8_t, PUSH_CONSTANT_SIZE> bytes{};
    size = uint32_t(std::min<size_t>(size ? size : PUSH_CONSTANT_SIZE, bytes.size()));
    std::memcpy(bytes.data(), data, size);
    if (g_logOps && g_drawTag >= 0) {
      // A draw's: its constants are replaced when the log runs again.
      int32_t tag = g_drawTag;
      g_frameLogs[g_frame].Push([=](RenderCommandList* l) {
        if (g_replayConstants && size_t(tag) < g_replayConstants->size()) {
          l->setGraphicsPushConstants(range, (*g_replayConstants)[tag].data(), offset, size);
        } else {
          l->setGraphicsPushConstants(range, bytes.data(), offset, size);
        }
      });
      g_renderThread.Push(
          [=](RenderCommandList* l) { l->setGraphicsPushConstants(range, bytes.data(), offset, size); });
      return;
    }
    Op([=](RenderCommandList* l) { l->setGraphicsPushConstants(range, bytes.data(), offset, size); });
  }
  void setGraphicsDescriptorSet(RenderDescriptorSet* set, uint32_t index) override {
    Op([=](RenderCommandList* l) { l->setGraphicsDescriptorSet(set, index); });
  }
  void setIndexBuffer(const RenderIndexBufferView* view) override {
    std::optional<RenderIndexBufferView> copy;
    if (view) {
      copy = *view;
    }
    Op([=](RenderCommandList* l) { l->setIndexBuffer(copy ? &*copy : nullptr); });
  }
  void setViewports(const RenderViewport* viewports, uint32_t count) override {
    std::array<RenderViewport, 8> copy{};
    count = std::min<uint32_t>(count, 8);
    std::copy_n(viewports, count, copy.begin());
    Op([=](RenderCommandList* l) { l->setViewports(copy.data(), count); });
  }
  void setScissors(const RenderRect* rects, uint32_t count) override {
    std::array<RenderRect, 8> copy{};
    count = std::min<uint32_t>(count, 8);
    std::copy_n(rects, count, copy.begin());
    Op([=](RenderCommandList* l) { l->setScissors(copy.data(), count); });
  }
  void setFramebuffer(const RenderFramebuffer* framebuffer) override {
    Op([=](RenderCommandList* l) { l->setFramebuffer(framebuffer); });
  }
  void clearColor(uint32_t attachment, RenderColor color, const RenderRect* rects,
                  uint32_t count) override {
    std::vector<RenderRect> copy(rects, rects + (rects ? count : 0));
    Op([=, copy = std::move(copy)](RenderCommandList* l) {
      l->clearColor(attachment, color, copy.empty() ? nullptr : copy.data(), uint32_t(copy.size()));
    });
  }
  void clearDepthStencil(bool depth, bool stencil, float depthValue, uint32_t stencilValue,
                         const RenderRect* rects, uint32_t count) override {
    std::vector<RenderRect> copy(rects, rects + (rects ? count : 0));
    Op([=, copy = std::move(copy)](RenderCommandList* l) {
      l->clearDepthStencil(depth, stencil, depthValue, stencilValue,
                           copy.empty() ? nullptr : copy.data(), uint32_t(copy.size()));
    });
  }
  void copyBufferRegion(RenderBufferReference dst, RenderBufferReference src, uint64_t size) override {
    if (g_logOps && g_copyTag >= 0) {
      int32_t tag = g_copyTag;
      g_frameLogs[g_frame].Push([=](RenderCommandList* l) {
        if (g_replayCopies && size_t(tag) < g_replayCopies->size() && (*g_replayCopies)[tag].ref) {
          l->copyBufferRegion(dst, (*g_replayCopies)[tag], size);
        } else {
          l->copyBufferRegion(dst, src, size);
        }
      });
      g_renderThread.Push([=](RenderCommandList* l) { l->copyBufferRegion(dst, src, size); });
      return;
    }
    Op([=](RenderCommandList* l) { l->copyBufferRegion(dst, src, size); });
  }
  void copyTextureRegion(const RenderTextureCopyLocation& dst, const RenderTextureCopyLocation& src,
                         uint32_t x, uint32_t y, uint32_t z, const RenderBox* box) override {
    std::optional<RenderBox> boxCopy;
    if (box) {
      boxCopy = *box;
    }
    Op([=](RenderCommandList* l) { l->copyTextureRegion(dst, src, x, y, z, boxCopy ? &*boxCopy : nullptr); });
  }
  void resolveTextureRegion(const RenderTexture* dst, uint32_t x, uint32_t y,
                            const RenderTexture* src, const RenderRect* rect,
                            RenderResolveMode mode) override {
    std::optional<RenderRect> rectCopy;
    if (rect) {
      rectCopy = *rect;
    }
    Op([=](RenderCommandList* l) {
      l->resolveTextureRegion(dst, x, y, src, rectCopy ? &*rectCopy : nullptr, mode);
    });
  }

  // Not used by the renderer.
  [[noreturn]] static void Unused(const char* name) {
    REXLOG_ERROR("video: {} isn't queued for the render thread", name);
    std::abort();
  }
  void dispatch(uint32_t, uint32_t, uint32_t) override { Unused("dispatch"); }
  void traceRays(uint32_t, uint32_t, uint32_t, RenderBufferReference,
                 const RenderShaderBindingGroupsInfo&) override { Unused("traceRays"); }
  void setComputePipelineLayout(const RenderPipelineLayout*) override { Unused("setComputePipelineLayout"); }
  void setComputePushConstants(uint32_t, const void*, uint32_t, uint32_t) override { Unused("setComputePushConstants"); }
  void setComputeDescriptorSet(RenderDescriptorSet*, uint32_t) override { Unused("setComputeDescriptorSet"); }
  void setGraphicsRootDescriptor(RenderBufferReference, uint32_t) override { Unused("setGraphicsRootDescriptor"); }
  void setRaytracingPipelineLayout(const RenderPipelineLayout*) override { Unused("setRaytracingPipelineLayout"); }
  void setRaytracingPushConstants(uint32_t, const void*, uint32_t, uint32_t) override { Unused("setRaytracingPushConstants"); }
  void setRaytracingDescriptorSet(RenderDescriptorSet*, uint32_t) override { Unused("setRaytracingDescriptorSet"); }
  void setVertexBuffers(uint32_t, const RenderVertexBufferView*, uint32_t, const RenderInputSlot*) override { Unused("setVertexBuffers"); }
  void setDepthBias(float, float, float) override { Unused("setDepthBias"); }
  void copyBuffer(const RenderBuffer*, const RenderBuffer*) override { Unused("copyBuffer"); }
  void copyTexture(const RenderTexture*, const RenderTexture*) override { Unused("copyTexture"); }
  void resolveTexture(const RenderTexture*, const RenderTexture*) override { Unused("resolveTexture"); }
  void buildBottomLevelAS(const RenderAccelerationStructure*, RenderBufferReference,
                          const RenderBottomLevelASBuildInfo&) override { Unused("buildBottomLevelAS"); }
  void buildTopLevelAS(const RenderAccelerationStructure*, RenderBufferReference, RenderBufferReference,
                       const RenderTopLevelASBuildInfo&) override { Unused("buildTopLevelAS"); }
  void discardTexture(const RenderTexture*) override { Unused("discardTexture"); }
  // (not kept for interpolation: a query written twice)
  void resetQueryPool(const RenderQueryPool* pool, uint32_t first, uint32_t count) override {
    g_renderThread.Push([=](RenderCommandList* l) { l->resetQueryPool(pool, first, count); });
  }
  void writeTimestamp(const RenderQueryPool* pool, uint32_t index) override {
    g_renderThread.Push([=](RenderCommandList* l) { l->writeTimestamp(pool, index); });
  }
};
QueuedCommandList g_queuedCommandList;

// Work that isn't a command (descriptor writes, the frame's begin and
// submit): queued with the rest, or done at once without the thread.
template <typename F>
void OnRenderThread(F&& f) {
  if (RenderThreadEnabled()) {
    g_renderThread.Push(std::forward<F>(f));
  } else {
    RenderCommandList* list = g_commandLists[g_frame].get();
    f(list);
  }
}

// A texture heap descriptor written in order with the commands that use it.
void WriteTextureDescriptor(uint32_t index, RenderTexture* texture, RenderTextureView* view) {
  OnRenderThread([=](RenderCommandList*) {
    g_textureDescriptorSet->setTexture(index, texture, RenderTextureLayout::SHADER_READ, view);
  });
}

// Frame -----------------------------------------------------------------------

// Under gamescope with the display holding a screen image longer than usual
// (the Quick Access Menu open over the game), the game gets a frame ahead of
// the display and then waits for it every frame: that wait beats against the
// game's own 60 Hz clock and the frames come in pairs, 6 then 28 ms apart.
// After 20 frames in a row each held up over 4 ms here, the vblank timer
// skips a step (DrainRequested, native_gpu.cpp): one frame stays up two
// refreshes and the queue is a frame shorter.
std::atomic<bool> g_drainRequest{false};
void NoteFrameWait(int64_t ns) {
  static uint32_t held = 0;
  held = ns > 4'000'000 ? held + 1 : 0;
  if (held >= 20) {
    held = 0;
    g_drainRequest.store(true, std::memory_order_relaxed);
  }
}

void ApplyResolveReadbacks(uint32_t frame);
bool ResolveReadbacksQueued(uint32_t frame);

// noteWait: under gamescope, the wait counts toward a drain request
// (NoteFrameWait). Not the read-back wait at the end of Present: earlier in
// the frame, it is longer in a GPU-heavy scene without the game being ahead.
void WaitForFrame(uint32_t frame, bool noteWait = true) {
  if (g_commandListPending[frame]) {
    StageTimer timer(Stage::kWait);
    int64_t waitStart = SteadyNs();
    if (RenderThreadEnabled()) {
      // Its submit first (only then has the fence been handed to the GPU).
      g_renderThread.Send();
      std::unique_lock lock(g_renderThread.submitMutex);
      g_renderThread.submitCv.wait(lock, [&] {
        return g_renderThread.submitted.load(std::memory_order_acquire) >= g_submitTicket[frame];
      });
    }
    g_queue->waitForCommandFence(g_commandFences[frame].get());
    g_commandListPending[frame] = false;
    ApplyResolveReadbacks(frame);
    if (UnderGamescope() && noteWait) {
      NoteFrameWait(SteadyNs() - waitStart);
    }
  }
}

// NG2_INTERP: a frame's pictures are presented one at a time, a part of
// the frame interval apart, by a thread of their own (presents queued
// together may be shown as one, the newest, as Windows does), while the
// render thread goes on with the next frame.
// NG2_INTERP: long waits for a swapchain image, logged.
void NoteAcquire(int64_t ns) {
  static int64_t total = 0, longest = 0;
  static uint32_t count = 0, slow = 0;
  total += ns;
  longest = std::max(longest, ns);
  slow += ns > 4000000;
  if (++count == 1800) {
    REXLOG_INFO("video: interp: acquires {:.2f} ms each, max {:.1f}, {} over 4 ms; {} images",
                double(total) / 1800e6, double(longest) / 1e6, slow, g_swapChain->getTextureCount());
    total = longest = 0;
    count = slow = 0;
  }
}

// A picture's drawing's fence, waited for once after each submit (plume's
// wait resets it): by the presenter before presenting the picture, or by the
// render thread before the picture's list and fence are used again, three
// frames on (in exclusive full screen the presenter doesn't wait, and a
// fence submitted again while still signaled is invalid).
struct PictureFence {
  std::unique_ptr<RenderCommandFence> fence;
  std::atomic<int> state{0};  // 0: nothing to wait for, 1: submitted, 2: being waited for
  void Submitted() {
    state = 1;
  }
  void Wait() {
    int expected = 1;
    if (state.compare_exchange_strong(expected, 2)) {
      g_queue->waitForCommandFence(fence.get());
      state = 0;
      state.notify_all();
      return;
    }
    while (expected == 2) {  // (the other thread's wait: until it's done)
      state.wait(2);
      expected = state.load();
    }
  }
};

// Each frame slot's pictures between: their acquire semaphores, lists and
// fences (the render thread's; released in DestroyHostDevice, before the
// device).
std::unique_ptr<RenderCommandSemaphore> g_interpAcquire[NUM_FRAMES][kInterpMaxPictures - 1];
std::unique_ptr<RenderCommandList> g_interpLists[NUM_FRAMES][kInterpMaxPictures - 1];
PictureFence g_interpFences[NUM_FRAMES][kInterpMaxPictures - 1];

class Presenter {
 public:
  struct Item {
    uint32_t index;
    RenderCommandSemaphore* semaphore;
    int64_t atNs;
    uint64_t frame;                     // the game's frame it shows
    PictureFence* done = nullptr;        // its drawing's (a picture between)
    int64_t inputNs = 0;                 // the frame's pad read (NG2_LATENCY_LOG)
    uint8_t order = 0;                   // 1: the frame's first picture, 2: its own, 3: both
    bool timed = false;                  // a slow screen's: by the clock always
  };

  ~Presenter() {
    Stop();
  }

  // The thread stopped and joined (DestroyHostDevice, after WaitForGPU: nothing
  // queued is left); a later Push starts it again.
  void Stop() {
    if (thread_.joinable()) {
      {
        std::lock_guard lock(mutex_);
        stop_ = true;
      }
      cv_.notify_all();
      idle_.notify_all();
      thread_.join();
      std::lock_guard lock(mutex_);
      stop_ = false;
      queue_.clear();
      busy_ = 0;
      busyTimed_ = false;
      lastId_ = 0;
    }
  }

  void Push(const Item& item) {
    {
      std::lock_guard lock(mutex_);
      queue_.push_back(item);
      if (!thread_.joinable()) {
        thread_ = std::thread([this] { Main(); });
      }
    }
    cv_.notify_all();
  }

  // The pictures waiting for their time (a slow screen's) presented at once,
  // and waited for: before a frame's drawing that acquires images (in
  // exclusive full screen an image is freed only by a later present, which
  // queued behind that drawing's wait would never come: the GPU stood 12 s).
  void Hurry() {
    {
      std::lock_guard lock(mutex_);
      bool timed = busyTimed_;
      for (auto& queued : queue_) {
        timed |= queued.timed;
      }
      if (!timed) {
        return;  // (none waits for its time)
      }
      for (auto& queued : queue_) {
        queued.atNs = 0;
      }
      hurry_ = true;
    }
    WaitFor(0);
    hurry_ = false;
  }

  // Pictures handed over and not yet presented.
  size_t Pending() {
    std::lock_guard lock(mutex_);
    return queue_.size() + busy_;
  }

  // Until at most `left` pictures wait.
  void WaitFor(size_t left) {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return queue_.size() + busy_ <= left || stop_; });
  }

  std::atomic<bool> failed{false};  // a present found the swapchain out of date
  std::atomic<uint64_t> presented{0};
  std::atomic<int64_t> presentNs{0}, presentMaxNs{0};

 private:
  void Main() {
    while (true) {
      Item item;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return !queue_.empty() || stop_; });
        if (stop_) {
          return;
        }
        item = queue_.front();
        queue_.pop_front();
        busy_ = 1;
        busyTimed_ = item.timed;
        // Behind (a later frame waits): shown at once, to catch up (the
        // screen keeps the newest of them). Not one with its time on the
        // display's grid: the last frame's last picture still waits for its
        // refresh when the next frame's come, and shown at once it came a
        // refresh or two early, then a gap (a stutter in a turn).
        if (!item.timed && !queue_.empty() && queue_.back().frame > item.frame) {
          item.atNs = 0;
        }
      }
      // Presented once drawn: a present ahead of its picture's drawing is
      // shown when that ends, with the next one if it ends with it.
      // (Not while the swapchain holds the display, exclusive full screen:
      // there a drawing can wait for its image, freed by a later present, and
      // the display flips one a refresh itself. Asked for and not held, or
      // lost to an overlay, it's composed as in a window, and paced so.)
      const bool exclusive = g_swapChain->isFullScreenExclusive();
      if (item.done && !exclusive) {
        item.done->Wait();
      }
      // Exclusive full screen: the display flips one picture a refresh from
      // its queue itself, so each goes at once (a timer that let the queue
      // run dry showed a picture two or three refreshes, a double to the
      // eye in a turn).
      if (exclusive && !item.timed) {
        item.atNs = 0;
      }
#if defined(_WIN32)
      // Sleeps of a millisecond (the system timer at 1 ms: timeBeginPeriod)
      // to near the time, the rest spun. (Waiting for the display's vblank,
      // D3DKMT or DwmFlush, stalled the game in full screen.)
      static const bool period = [] {
        HMODULE winmm = LoadLibraryA("winmm.dll");
        using PeriodFn = UINT(WINAPI*)(UINT);
        auto begin = winmm ? reinterpret_cast<PeriodFn>(GetProcAddress(winmm, "timeBeginPeriod")) : nullptr;
        return begin && begin(1) == 0;
      }();
      (void)period;
      while (!hurry_.load() && item.atNs - SteadyNs() > 2000000) {
        Sleep(1);
      }
#endif
      for (int64_t now = SteadyNs(); now < item.atNs && !hurry_.load(); now = SteadyNs()) {
        if (item.atNs - now > 2000000) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
          std::this_thread::yield();
        }
      }
      if (item.atNs && lastId_) {
        int64_t until = SteadyNs() + 30000000;
        while (g_swapChain->pollPresent(lastId_) == 0 && SteadyNs() < until) {
#if defined(_WIN32)
          Sleep(0);
#else
          std::this_thread::yield();
#endif
        }
      }
      int64_t presentStart = SteadyNs();
      if (!g_swapChain->present(item.index, &item.semaphore, 1)) {
        failed = true;
      } else {
        g_picturesPresented.fetch_add(1, std::memory_order_relaxed);
      }
      lastId_ = g_swapChain->getPresentId();
      if (item.inputNs) {
        if (item.order & 1) {
          NotePadToPresent(SteadyNs() - item.inputNs, 0);
          if (lastId_) {
            std::lock_guard lock(g_taggedMutex);
            g_tagged[lastId_ % 64] = {lastId_, item.inputNs, SteadyNs()};
          }
        }
        if (item.order & 2) {
          NotePadToPresent(SteadyNs() - item.inputNs, 1);
        }
      }
      if (item.timed) {
        static int64_t lastNs = 0, lateSum = 0, lateMax = 0, gapMin = INT64_MAX, gapMax = 0;
        static uint32_t count = 0, gaps = 0, short4 = 0, long15 = 0;
        // (one hurried out, its time dropped: not late, counted apart; its
        // time 0 made the average hours late)
        static uint32_t hurried = 0;
        int64_t late = item.atNs ? presentStart - item.atNs : 0;
        hurried += item.atNs == 0;
        lateSum += late, lateMax = std::max(lateMax, late);
        if (lastNs && presentStart - lastNs < 100'000'000) {
          int64_t gap = presentStart - lastNs;
          gapMin = std::min(gapMin, gap), gapMax = std::max(gapMax, gap);
          short4 += gap < 4'000'000, long15 += gap > 15'000'000;
          gaps++;
        }
        lastNs = presentStart;
        if (++count == 120) {
          REXLOG_INFO("video: interp: slow screen presents: {:.2f} ms late on average (max {:.1f}); "
                      "{:.1f} to {:.1f} ms apart, {} under 4 ms, {} over 15 ms of {}; {} hurried out",
                      double(lateSum) / 120e6, double(lateMax) / 1e6, double(gapMin) / 1e6, double(gapMax) / 1e6,
                      short4, long15, gaps, hurried);
          lateSum = lateMax = gapMax = 0, gapMin = INT64_MAX;
          count = gaps = short4 = long15 = hurried = 0;
        }
      }
      int64_t took = SteadyNs() - presentStart;
      presentNs += took;
      presentMaxNs = std::max<int64_t>(presentMaxNs, took);
      presented++;
      {
        std::lock_guard lock(mutex_);
        busy_ = 0;
        busyTimed_ = false;
      }
      idle_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_, idle_;
  std::deque<Item> queue_;
  size_t busy_ = 0;
  std::atomic<bool> hurry_{false};
  bool busyTimed_ = false;  // the picture being presented is a timed one (mutex_)
  bool stop_ = false;
  uint64_t lastId_ = 0;  // the last present's (presenter thread)
  std::thread thread_;
};
Presenter g_presenter;

void WaitForGPU() {
  if (RenderThreadEnabled()) {
    g_renderThread.Flush();
  }
  g_presenter.WaitFor(0);
  for (uint32_t i = 0; i < NUM_FRAMES; i++) {
    WaitForFrame(i);
  }
}

RenderCommandList* CommandList();
void PrepareNullTextures();

// GPU timing (NG2_PROFILE): a mark's index in this frame's pool, or none.
uint32_t AllocGpuMark(GpuMark kind) {
  GpuTiming& t = g_gpuTiming[g_frame];
  // (the last two kept for the frame's present and end marks)
  uint32_t capacity = GpuMarkCapacity();
  uint32_t limit = kind == GpuMark::kPresentStart || kind == GpuMark::kFrameEnd ? capacity
                                                                                  : capacity - 2;
  if (!g_profile.enabled || !t.pool || t.count >= limit) {
    static bool logged = false;
    if (g_profile.perDraw && t.pool && !logged) {
      logged = true;
      REXLOG_WARN("video: over {} GPU timestamps in a frame: the rest of it isn't timed by draw",
                  limit);
    }
    return UINT32_MAX;
  }
  t.kinds.push_back(kind);
  return t.count++;
}
void WriteGpuMark(RenderCommandList* list, uint32_t frame, uint32_t index) {
  if (index != UINT32_MAX) {
    list->writeTimestamp(g_gpuTiming[frame].pool.get(), index);
  }
}
void MarkGpu(GpuMark kind) {
  uint32_t index = AllocGpuMark(kind);
  if (index != UINT32_MAX) {
    WriteGpuMark(CommandList(), g_frame, index);
  }
}

// The slot's last frame's marks (its fence passed), into the profile.
void ReadGpuTiming(uint32_t frame) {
  GpuTiming& t = g_gpuTiming[frame];
  if (!t.pool || t.count < 2) {
    t.count = 0;
    t.kinds.clear();
    return;
  }
  t.pool->queryResults(t.count);  // (the written ones: the rest would fail the read)
  const uint64_t* ns = t.pool->getResults();
  uint64_t start = ns[0], resolveStart = 0, presentStart = 0, resolves = 0;
  size_t pass = 0, draw = 0;
  uint64_t* passNs = nullptr;  // the pass the marks are in (its draws' marks included)
  for (uint32_t i = 0; i < t.count; i++) {
    uint64_t next = i + 1 < t.count ? ns[i + 1] : ns[i];
    uint64_t span = next > ns[i] ? next - ns[i] : 0;
    GpuMark kind = t.kinds[i];
    if (kind == GpuMark::kPass) {
      passNs = pass < t.passLabels.size() ? &g_profile.gpuPassNs[t.passLabels[pass++]] : nullptr;
    } else if (kind != GpuMark::kDraw && kind != GpuMark::kDrawEnd) {
      passNs = nullptr;
    }
    if (passNs) {
      *passNs += span;
    }
    // A draw's own time: its mark to the mark after it (kDrawEnd).
    if (t.kinds[i] == GpuMark::kDraw && draw < t.drawShaders.size()) {
      auto& [drawNs, draws] = g_profile.gpuDrawNs[t.drawShaders[draw++]];
      drawNs += span;
      draws++;
    }
    switch (t.kinds[i]) {
      case GpuMark::kResolveStart: resolveStart = ns[i]; break;
      case GpuMark::kResolveEnd: resolves += ns[i] - resolveStart; break;
      case GpuMark::kPresentStart: presentStart = ns[i]; break;
      case GpuMark::kFrameEnd:
        g_profile.gpuNs += ns[i] - start;
        g_profile.gpuPresentNs += presentStart ? ns[i] - presentStart : 0;
        g_profile.gpuResolveNs += resolves;
        g_profile.gpuFrames++;
        break;
      default: break;
    }
  }
  t.count = 0;
  t.kinds.clear();
  t.passLabels.clear();
  t.drawShaders.clear();
}

// The frame's pool reset and its start marked (once recording).
void BeginGpuTiming(uint32_t frame) {
  if (!g_profile.enabled) {
    return;
  }
  GpuTiming& t = g_gpuTiming[frame];
  if (!t.pool) {
    t.pool = g_device->createQueryPool(GpuMarkCapacity());
  }
  CommandList()->resetQueryPool(t.pool.get(), 0, GpuMarkCapacity());
  MarkGpu(GpuMark::kFrameStart);
}

// The frame's start (its slot's GPU work waited for, its upload memory and
// freed textures reset) before anything is allocated or freed in it: a draw
// first after a present would otherwise put a texture the previous frame
// still uses in this slot's list, freed at once by the start that follows.
void BeginFrame() {
  if (!g_recording) {
    CommandList();
  }
}

void SwitchInterp();

// The images a frame may hold at once (its pictures and its own) before any
// is presented: an acquire is only sure to return while no more than the
// count less the surface's minimum are held (gamescope: 3 of 8 the minimum).
uint32_t SpareImages() {
  uint32_t count = g_swapChain->getTextureCount();
  uint32_t least = g_swapChain->getMinTextureCount();
  return count >= least ? count - least + 1 : 0;
}

// The most pictures a frame, its own counted: what the swapchain's images
// leave (after each resize, which may change their count).
void SetInterpPictureCap() {
  g_interpPictureCap = std::max(1u, std::min(kInterpMaxPictures, SpareImages()));
}

// Exclusive full screen (the swapchain takes the display, its presents
// flipped by it instead of composed by DWM, which spoils the pictures'
// pacing): only for frame interpolation, in Window Mode Fullscreen
// (fullscreen and exclusive_fullscreen). NG2_EXCLUSIVE (0, 1, 2: left to the
// driver) over it. Decided again between frames: the menu changes both.
int ExclusiveMode() {
#if defined(_WIN32)
  static const char* forced = std::getenv("NG2_EXCLUSIVE");
  if (forced && forced[0]) {
    return std::clamp(std::atoi(forced), 0, 2);
  }
  return InterpEnabled() && rex::cvar::Query<bool>("fullscreen") &&
                 rex::cvar::Query<bool>("exclusive_fullscreen")
             ? 1
             : 0;
#else
  return 0;
#endif
}
int g_exclusiveMode = 0;  // the swapchain's (game thread)
// The swapchain's mode set to ExclusiveMode's; true if it changed (made
// again at the next resize).
bool UpdateExclusiveMode() {
  int mode = ExclusiveMode();
  if (!g_swapChain || mode == g_exclusiveMode) {
    return false;
  }
  REXLOG_INFO("video: exclusive full screen {}", mode ? "asked for" : "off");
  g_exclusiveMode = mode;
  g_swapChain->setFullScreenExclusive(mode);
  return true;
}

RenderCommandList* CommandList() {
  if (!g_recording) {
    SwitchInterp();
    if (UpdateExclusiveMode()) {
      g_resizeRequested = true;  // (Borderless from Fullscreen: no resize of its own)
    }
    if (g_resizeRequested.load()) {
      WaitForGPU();  // (queued presents may raise it again: cleared after)
      g_resizeRequested.store(false);
      g_swapChainFramebuffers.clear();
      g_swapChainValid = g_swapChain->resize();
      SetInterpPictureCap();
    }
    WaitForFrame(g_frame);
    ReadGpuTiming(g_frame);
    g_boundFramebuffer = nullptr;
    g_uploads[g_frame].Reset();
    g_interpUploads[g_frame].Reset();
    for (auto& texture : g_garbage[g_frame]) {
      g_descriptors.Free(texture->descriptor);
      ForgetTexture(texture.get());
    }
    g_garbage[g_frame].clear();
    // (its present, which ran the log again, has been submitted)
    g_frameLogs[g_frame].Clear();
    g_logOps = InterpEnabled();
    uint32_t frame = g_frame;
    if (RenderThreadEnabled()) {
      g_renderThread.Push([frame](RenderCommandList*) {
        g_threadList = g_commandLists[frame].get();
        g_threadList->begin();
      });
    } else {
      g_commandLists[g_frame]->begin();
    }
    g_recording = true;
    BeginGpuTiming(g_frame);
    PrepareNullTextures();
  }
  return RenderThreadEnabled() ? static_cast<RenderCommandList*>(&g_queuedCommandList)
                               : g_commandLists[g_frame].get();
}

void Transition(HostTexture& texture, RenderTextureLayout layout) {
  NoteStartLayout(texture);
  // A copy or resolve into a texture that was the last one's destination too
  // still needs a barrier: without it the earlier write (often the previous
  // frame's, a resolve redone each frame) can land after this one, and the
  // texture holds an old picture for that frame (flicker where the GPU runs
  // frames overlapped; the synchronization validation's write-after-write).
  bool rewrite = texture.layout == layout && (layout == RenderTextureLayout::COPY_DEST ||
                                              layout == RenderTextureLayout::RESOLVE_DEST);
  if (texture.layout != layout || rewrite) {
    g_profile.barriers++;
    CommandList()->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY,
                            RenderTextureBarrier(texture.texture.get(), layout));
    texture.layout = layout;
  }
}

void SetDescriptor(HostTexture& texture) {
  if (texture.descriptor == 0) {
    texture.descriptor = g_descriptors.Allocate();
  }
  if (texture.descriptor != 0) {
    if (!texture.view) {
      RenderTextureViewDesc viewDesc;
      viewDesc.format = texture.format;
      viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
      viewDesc.mipLevels = 1;
      texture.view = texture.texture->createTextureView(viewDesc);
    }
    WriteTextureDescriptor(texture.descriptor, texture.texture.get(), texture.view.get());
  }
}

// Render targets --------------------------------------------------------------

// The host texture for a guest D3DSurface (render target or depth/stencil),
// created on first use.
HostTexture* GetSurface(uint32_t surface, bool depth) {
  if (!surface) {
    return nullptr;
  }
  // Texture-level surfaces (Common bit 30) aren't EDRAM targets.
  if (guest::Load32(surface) & 0x40000000) {
    return nullptr;
  }
  uint32_t info = guest::Load32(surface + 28);
  uint32_t dims = guest::Load32(surface + 36);
  SurfaceKey key;
  key.edramBase = info & 0xFFF;
  // By host format: guest formats that share a host one (2_10_10_10_FLOAT and
  // its AS_16_16_16_16 view, the two depth formats) are the same EDRAM data
  // drawn and resolved through either.
  uint32_t guestFormat = (info >> 16) & (depth ? 1 : 0xF);
  key.format = depth ? 0x100 : uint32_t(ColorTargetFormat(guestFormat));
  key.width = ((dims >> 18) & 0x3FFF) + 1;
  key.height = ((dims >> 3) & 0x7FFF) + 1;
  key.msaa = guest::Load16(surface + 24) & 3;
  if (key.width > 4096 || key.height > 4096) {
    // No target is near this big: a surface read wrong (one would lose the GPU).
    static uint32_t logged = 0;
    if (logged++ < 4) {
      REXLOG_WARN("video: ignored a {} target {}x{} (surface {:#x})", depth ? "depth" : "color",
                  key.width, key.height, surface);
    }
    return nullptr;
  }

  auto& slot = g_surfaces[key];
  if (!slot) {
    auto host = std::make_unique<HostTexture>();
    host->scale = g_resolutionScale;
    host->width = key.width * host->scale;
    host->height = key.height * host->scale;
    host->samples = SampleCount(key.msaa);
    host->format = depth ? RenderFormat::D32_FLOAT_S8_UINT : ColorTargetFormat(guestFormat);
    RenderTextureDesc desc = RenderTextureDesc::Texture2D(
        host->width, host->height, 1, host->format,
        depth ? RenderTextureFlag::DEPTH_TARGET : RenderTextureFlag::RENDER_TARGET);
    desc.multisampling.sampleCount = host->samples;
    host->texture = g_device->createTexture(desc);
    if (!host->texture) {
      // Out of video memory, most likely at a high render scale. (No empty
      // entry left: the render scale's rebuild walks them. Logged once per
      // size and scale: each draw asks again.)
      static std::set<uint64_t> failed;
      static uint32_t failedScale = 0;
      if (failedScale != host->scale) {
        failed.clear(), failedScale = host->scale;
      }
      if (failed.insert(uint64_t(host->width) << 32 | host->height).second) {
        REXLOG_ERROR("video: no {}x{} render target", host->width, host->height);
      }
      g_surfaces.erase(key);
      return nullptr;
    }
    REXLOG_INFO("video: {} target {}x{} edram {:#x} format {} msaa {} (scale {})",
                depth ? "depth" : "color", key.width, key.height, key.edramBase, guestFormat,
                key.msaa, host->scale);
    slot = std::move(host);
  }
  // 2_10_10_10_FLOAT and its 16_16_16_16 view hold 7e3 color (ResolveByDraw).
  slot->float7e3 = !depth && (guestFormat == 3 || guestFormat == 12);
  return slot.get();
}

// Binds colors[0..colorCount) (no gaps) and depth.
void BindFramebuffer(HostTexture* const* colors, uint32_t colorCount, HostTexture* depth) {
  FramebufferKey key;
  for (uint32_t i = 0; i < colorCount; i++) {
    key.colors[i] = colors[i];
  }
  key.depth = depth;
  auto& framebuffer = g_framebuffers[key];
  if (!framebuffer) {
    const RenderTexture* attachments[4] = {};
    for (uint32_t i = 0; i < colorCount; i++) {
      attachments[i] = colors[i]->texture.get();
    }
    RenderFramebufferDesc desc;
    desc.colorAttachments = colorCount ? attachments : nullptr;
    desc.colorAttachmentsCount = colorCount;
    desc.depthAttachment = depth ? depth->texture.get() : nullptr;
    framebuffer = g_device->createFramebuffer(desc);
  }
  for (uint32_t i = 0; i < colorCount; i++) {
    Transition(*colors[i], RenderTextureLayout::COLOR_WRITE);
  }
  if (depth) {
    Transition(*depth, RenderTextureLayout::DEPTH_WRITE);
  }
  if (g_boundFramebuffer != framebuffer.get()) {
    g_profile.passes++;
    if (uint32_t mark = AllocGpuMark(GpuMark::kPass); mark != UINT32_MAX) {
      const HostTexture* first = colorCount ? colors[0] : depth;
      g_gpuTiming[g_frame].passLabels.push_back(
          first ? fmt::format("{}x{} fmt {} x{}, {} colors{}", first->width, first->height,
                              int(first->format), int(first->samples), colorCount,
                              depth ? ", depth" : "")
                : std::string("none"));
      WriteGpuMark(CommandList(), g_frame, mark);
    }
    CommandList()->setFramebuffer(framebuffer.get());
    g_boundFramebuffer = framebuffer.get();
  }
}

void BindFramebuffer(HostTexture* color, HostTexture* depth) {
  BindFramebuffer(&color, color ? 1 : 0, depth);
}

uint64_t ResolvedKey(uint32_t address, const xenos::xe_gpu_texture_fetch_t& fetch) {
  uint32_t data[4] = {address, uint32_t(fetch.size_2d.width), uint32_t(fetch.size_2d.height),
                      uint32_t(ResolveTextureFormat(fetch.format))};
  return XXH3_64bits(data, sizeof(data));
}

// Host texture backing a guest texture written by resolves.
// scale: the source's, so the copy is pixel for pixel.
uint64_t ResolvedMemoryHash(uint32_t address, uint32_t size);

HostTexture* GetResolveDestination(uint32_t address, const xenos::xe_gpu_texture_fetch_t& fetch,
                                   uint32_t scale) {
  RenderFormat format = ResolveTextureFormat(fetch.format);
  if (format == RenderFormat::UNKNOWN) {
    return nullptr;
  }
  uint32_t width = fetch.size_2d.width + 1;
  uint32_t height = fetch.size_2d.height + 1;
  auto& slot = g_resolvedTextures[ResolvedKey(address, fetch)];
  if (slot && slot->scale != scale) {
    g_garbage[g_frame].push_back(std::move(slot));  // resolved at another render scale
  }
  if (!slot) {
    auto host = std::make_unique<HostTexture>();
    host->scale = scale;
    host->width = width * scale;
    host->height = height * scale;
    host->format = format;
    host->texture = g_device->createTexture(RenderTextureDesc::Texture2D(
        host->width, host->height, 1, format, RenderTextureFlag::RENDER_TARGET));
    if (!host->texture) {
      // (once per size until the render scale changes: a failure each resolve
      // would flood the log and retry the allocation thousands of times)
      static std::set<uint64_t> failed;
      static uint32_t failedScale = 0;
      if (failedScale != scale) {
        failed.clear(), failedScale = scale;
      }
      if (failed.insert(uint64_t(host->width) << 32 | host->height).second) {
        REXLOG_ERROR("video: no {}x{} resolve texture", host->width, host->height);
      }
      g_resolvedTextures.erase(ResolvedKey(address, fetch));
      // The previous one (at another scale) is freed: not to be read as this
      // address's last resolve.
      g_lastResolve.erase(address);
      if (auto it = g_resolvedMemory.find(address); it != g_resolvedMemory.end()) {
        it->second.stale = true;
      }
      return nullptr;
    }
    // Depth textures read their single channel everywhere, as Xenos does.
    if (fetch.format == xenos::TextureFormat::k_24_8 ||
        fetch.format == xenos::TextureFormat::k_24_8_FLOAT) {
      RenderTextureViewDesc viewDesc = RenderTextureViewDesc::Texture2D(format);
      viewDesc.mipLevels = 1;
      viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::R, RenderSwizzle::R,
                                                         RenderSwizzle::R, RenderSwizzle::R);
      host->view = host->texture->createTextureView(viewDesc);
    }
    SetDescriptor(*host);
    REXLOG_INFO("video: resolve texture {:#x} {}x{} format {}", address, width, height,
                uint32_t(fetch.format));
    slot = std::move(host);
  }
  g_lastResolve[address] = slot.get();
  auto& memory = g_resolvedMemory[address];
  memory.size = uint32_t(std::min<uint64_t>(uint64_t(width) * height * RenderFormatSize(format),
                                            GUEST_MEMORY_SIZE - address));
  memory.hash = ResolvedMemoryHash(address, memory.size);
  memory.checkedFrame = g_frameCount;
  memory.stale = false;
  return slot.get();
}

// A hash of the guest memory a resolve went to: whole when small, else its
// first and last 4 KB and 64 pieces spread over it. The CPU replaces such
// memory wholesale (a texture loaded where a picture was resolved), and the
// whole of it (MBs, ~20 resolves a frame, each checked once a frame) was most
// of the renderer's hashing.
uint64_t ResolvedMemoryHash(uint32_t address, uint32_t size) {
  const uint8_t* data = guest::Physical(address);
  constexpr uint32_t kWhole = 64 << 10, kEdge = 4096, kPieces = 64, kPiece = 256;
  if (size <= kWhole) {
    return XXH3_64bits(data, size);
  }
  XXH3_state_t state;
  XXH3_64bits_reset(&state);
  XXH3_64bits_update(&state, data, kEdge);
  uint32_t step = (size - 2 * kEdge) / kPieces;
  for (uint32_t i = 0; i < kPieces; i++) {
    XXH3_64bits_update(&state, data + kEdge + i * step, std::min(kPiece, step));
  }
  XXH3_64bits_update(&state, data + size - kEdge, kEdge);
  return XXH3_64bits_digest(&state);
}

// False once the CPU has overwritten the memory resolved to address.
bool ResolveCurrent(uint32_t address) {
  auto it = g_resolvedMemory.find(address);
  if (it == g_resolvedMemory.end()) {
    return true;
  }
  auto& memory = it->second;
  if (!memory.stale && memory.checkedFrame != g_frameCount) {
    memory.checkedFrame = g_frameCount;
    memory.stale = ResolvedMemoryHash(address, memory.size) != memory.hash;
    if (memory.stale && Tracing()) {
      TraceLog("video: resolve memory {:#x} overwritten", address);
    }
    // NG2_TEST_STALE_LOG=1: each copy judged overwritten (flicker test), a
    // count per address once a second.
    static const bool staleLog = [] {
      const char* v = std::getenv("NG2_TEST_STALE_LOG");
      return v && *v == '1';
    }();
    if (staleLog && memory.stale) {
      static std::map<uint32_t, int> counts;
      static int64_t lastNs = 0;
      counts[address]++;
      if (int64_t now = SteadyNs(); now - lastNs > 1'000'000'000) {
        lastNs = now;
        std::string text;
        for (auto& [a, n] : counts) text += fmt::format(" {:#x} x{} ({} bytes)", a, n, g_resolvedMemory[a].size);
        REXLOG_INFO("stale: resolved memory overwritten:{}", text);
        counts.clear();
      }
    }
  }
  // NG2_TEST_NO_STALE=1: the GPU's copy always (flicker test).
  static const bool noStale = [] {
    const char* v = std::getenv("NG2_TEST_NO_STALE");
    return v && *v == '1';
  }();
  if (noStale) {
    return true;
  }
  return !memory.stale;
}

// Texture objects keep CPU virtual addresses in their fetch constant; the
// device's fetch constants (SetTexture) hold physical ones. Same conversion
// as D3D's (docs/d3d-map-c.md).
uint32_t PhysicalAddress(uint32_t address) {
  return (address & 0x1FFFFFFF) + (address >= 0xE0000000 ? 0x1000 : 0);
}

xenos::xe_gpu_texture_fetch_t ReadTextureFetch(uint32_t texture) {
  xenos::xe_gpu_texture_fetch_t fetch;
  uint32_t* dwords = &fetch.dword_0;
  for (uint32_t i = 0; i < 6; i++) {
    dwords[i] = guest::Load32(texture + guest::kTextureFetchConstant + i * 4);
  }
  return fetch;
}



// Copies [x1, y1, x2, y2) of a render target to (dx, dy) in a resolve
// destination, converting the format with a draw.
// Copies a rect of a render target (color, or depth into R32_FLOAT) to a
// texture by drawing, converting formats.
void ResolveByDraw(HostTexture& source, HostTexture& dest, int32_t x1, int32_t y1, int32_t x2,
                   int32_t y2, uint32_t dx, uint32_t dy, bool depth = false) {
  HostTexture* readable = &source;
  bool multisampled = source.samples != RenderSampleCount::COUNT_1;
  RenderShader* shader = g_copyShader.get();
  if (depth) {
    shader = multisampled ? g_depthCopyMsShader.get() : g_depthCopyShader.get();
  } else if (source.float7e3) {
    // Each sample is rounded before averaging, so no hardware resolve.
    shader = multisampled ? g_copy7e3MsShader.get() : g_copy7e3Shader.get();
  } else if (multisampled) {
    auto& temp = g_resolveTemps[&source];
    if (!temp) {
      temp = std::make_unique<HostTexture>();
      temp->width = source.width;
      temp->height = source.height;
      temp->format = source.format;
      temp->texture = g_device->createTexture(RenderTextureDesc::Texture2D(
          source.width, source.height, 1, source.format, RenderTextureFlag::RENDER_TARGET));
      if (!temp->texture) {
        REXLOG_ERROR("video: no {}x{} resolve copy", source.width, source.height);
        g_resolveTemps.erase(&source);
        return;
      }
    }
    Transition(source, RenderTextureLayout::RESOLVE_SOURCE);
    Transition(*temp, RenderTextureLayout::RESOLVE_DEST);
    RenderRect rect(x1, y1, x2, y2);
    CommandList()->resolveTextureRegion(temp->texture.get(), x1, y1, source.texture.get(), &rect);
    readable = temp.get();
  }
  SetDescriptor(*readable);
  if (!readable->descriptor) {
    return;
  }
  Transition(*readable, RenderTextureLayout::SHADER_READ);

  auto& pipeline =
      g_copyPipelines[(uint64_t(reinterpret_cast<uintptr_t>(shader)) << 8) ^ uint64_t(dest.format)];
  if (!pipeline) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_pipelineLayout.get();
    desc.vertexShader = g_fullscreenShader.get();
    desc.pixelShader = shader;
    desc.renderTargetFormat[0] = dest.format;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    pipeline = g_device->createGraphicsPipeline(desc);
  }

  BindFramebuffer(&dest, nullptr);
  auto* commandList = CommandList();
  RenderViewport viewport(float(dx), float(dy), float(x2 - x1), float(y2 - y1));
  RenderRect scissor(int32_t(dx), int32_t(dy), int32_t(dx) + (x2 - x1), int32_t(dy) + (y2 - y1));
  int32_t constants[3] = {int32_t(readable->descriptor), x1 - int32_t(dx), y1 - int32_t(dy)};
  commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
  commandList->setPipeline(pipeline.get());
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
  commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);
  commandList->setGraphicsPushConstants(0, constants, 0, sizeof(constants));
  commandList->setViewports(&viewport, 1);
  commandList->setScissors(&scissor, 1);
  commandList->drawInstanced(3, 1, 0, 0);
}

// Uploads ----------------------------------------------------------------------

Upload AllocateFrom(UploadAllocator& allocator, uint64_t size, uint64_t alignment = 256) {
  allocator.offset = (allocator.offset + alignment - 1) & ~(alignment - 1);
  // Move on while the current chunk can't hold it (chunks can be bigger
  // than UPLOAD_CHUNK_SIZE for big uploads).
  while (allocator.chunk < allocator.chunks.size() &&
         allocator.offset + size > allocator.chunks[allocator.chunk].size) {
    allocator.chunk++;
    allocator.offset = 0;
  }
  if (allocator.chunk >= allocator.chunks.size()) {
    UploadChunk chunk;
    chunk.size = std::max(UPLOAD_CHUNK_SIZE, size);
    chunk.buffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(
        chunk.size,
        RenderBufferFlag::CONSTANT | RenderBufferFlag::STORAGE | RenderBufferFlag::INDEX));
    if (!chunk.buffer) {
      REXLOG_ERROR("video: out of upload memory ({} MB)", chunk.size >> 20);
      std::_Exit(1);
    }
    chunk.data = static_cast<uint8_t*>(chunk.buffer->map());
    chunk.address = chunk.buffer->getDeviceAddress();
    allocator.chunks.push_back(std::move(chunk));
    REXLOG_INFO("video: upload chunk {}{} ({} MB)", allocator.chunks.size(),
                &allocator >= g_interpUploads && &allocator < g_interpUploads + NUM_FRAMES ? " (interpolation)" : "",
                allocator.chunks.back().size >> 20);
  }
  auto& chunk = allocator.chunks[allocator.chunk];
  Upload upload{chunk.buffer.get(), allocator.offset, chunk.data + allocator.offset,
                chunk.address + allocator.offset};
  allocator.offset += size;
  return upload;
}

Upload Allocate(uint64_t size, uint64_t alignment = 256) {
  return AllocateFrom(g_uploads[g_frame], size, alignment);
}

// The blank textures behind unbound slots, cleared to 0 and moved to
// SHADER_READ once, in the first frame's list (#18): their descriptors say
// SHADER_READ, and they were sampled in their creation layout (undefined),
// which AMD and NVIDIA read as anything (the view's swizzle makes it 0) but
// Intel's compressed images need not.
void PrepareNullTextures() {
  if (g_nullTexturesReady) {
    return;
  }
  g_nullTexturesReady = true;
  Upload zeros = Allocate(256, 256);
  std::memset(zeros.data, 0, 256);
  for (uint32_t i = 0; i < DESCRIPTOR_FIRST_FREE; i++) {
    HostTexture& texture = g_nullTextures[i];
    if (!texture.texture) {
      continue;
    }
    Transition(texture, RenderTextureLayout::COPY_DEST);
    uint32_t layers = i == DESCRIPTOR_NULL_CUBE ? 6 : 1;
    for (uint32_t layer = 0; layer < layers; layer++) {
      CommandList()->copyTextureRegion(
          RenderTextureCopyLocation::Subresource(texture.texture.get(), 0, layer),
          RenderTextureCopyLocation::PlacedFootprint(zeros.buffer, RenderFormat::R8_UNORM, 1, 1, 1,
                                                     256, zeros.offset));
    }
    Transition(texture, RenderTextureLayout::SHADER_READ);
  }
}


// A copy into g_guestMemory. Recorded in the frame's command list it ends the
// render pass (Vulkan allows no copy inside one) and takes two barriers, and
// a frame makes dozens, which looked like the cost on AMD's Windows driver
// (#2; it wasn't, see below). So a copy whose bytes nothing drawn earlier in the
// frame has read goes to the frame's prologue instead, one list run before
// the frame's with one barrier at each end; only a copy over memory already
// read this frame (a range rewritten between its draws, a ring buffer wrapped
// within a frame) stays in order. The data is the same either way: it is
// copied into the upload at this point (or at the frame's end for the late
// ranges), and the prologue runs after the previous frame's commands.
// Off unless asked for (PrologueMode, set when the device is made): every copy
// stays in order, as in 0.5.4. On AMD's Windows driver (#2) it didn't change
// the main pass's cost (19 ms with it or without), and on the Steam Deck under
// gamescope it made frames come in pairs (6 then 28 ms apart).
// NG2_PROLOGUE=0, 1 or 2 picks it: 0 in order; 1 the prologue in the frame's
// submit; 2 the prologue submitted on its own just before the frame.
int g_prologueMode = 0;
int PrologueMode() {
  return g_prologueMode;
}
// A barrier's stages on g_guestMemory: as given without the prologue; with it
// both, GRAPHICS and COPY, since plume takes a barrier's source stages from
// the last one recorded on the buffer, and the two lists are recorded out of
// execution order (from two threads with the render thread).
RenderBarrierStages GuestMemoryStages(RenderBarrierStages stages) {
  return g_prologueMode ? RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY : stages;
}

void RecordGuestMemoryCopy(uint32_t address, const Upload& upload, uint32_t size, bool inOrder) {
  if (!inOrder) {
    auto* prologue = g_prologueLists[g_frame].get();
    if (!g_prologueOpen[g_frame]) {
      g_prologueOpen[g_frame] = true;
      prologue->begin();
      prologue->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY,
                         RenderBufferBarrier(g_guestMemory.get(), RenderBufferAccess::WRITE));
    }
    prologue->copyBufferRegion(RenderBufferReference(g_guestMemory.get(), address),
                               RenderBufferReference(upload.buffer, upload.offset), size);
    g_profile.prologueCopies++;
    return;
  }
  g_profile.inOrderCopies++;
  auto* commandList = CommandList();
  commandList->barriers(GuestMemoryStages(RenderBarrierStage::COPY),
                        RenderBufferBarrier(g_guestMemory.get(), RenderBufferAccess::WRITE));
  commandList->copyBufferRegion(RenderBufferReference(g_guestMemory.get(), address),
                                RenderBufferReference(upload.buffer, upload.offset), size);
  commandList->barriers(GuestMemoryStages(RenderBarrierStage::GRAPHICS),
                        RenderBufferBarrier(g_guestMemory.get(),
                                            RenderBufferAccess::READ | RenderBufferAccess::WRITE));
}

// What the frame's draws have read of guest memory so far (every range
// synced, copied or not), and whether one wrote it (memory export): a copy
// over any of it stays in order (RecordGuestMemoryCopy).
struct FrameRead {
  uint32_t address, size;
};
std::vector<FrameRead> g_frameReads;
std::unordered_set<uint64_t> g_frameReadKeys;  // (address, size) already listed
bool g_frameMemExport = false;
bool ReadThisFrame(uint32_t address, uint32_t size) {
  for (const auto& read : g_frameReads) {
    if (address < read.address + read.size && read.address < address + size) {
      return true;
    }
  }
  return false;
}

// Makes guest physical [address, address + size) current in g_guestMemory.
// always: inline vertex data in the ring buffer, rewritten at any time.
// bones: a bone palette (fetch slot 94), late from the start (see GuestMemoryRange).
void SyncGuestMemory(uint32_t address, uint32_t size, bool always = false, bool bones = false) {
  if (!size || uint64_t(address) + size > GUEST_MEMORY_SIZE) {
    return;
  }
  StageTimer timer(Stage::kSync);
  bool inOrder = true;
  if (g_prologueMode) {
    inOrder = g_frameMemExport || ReadThisFrame(address, size);
    if (g_frameReadKeys.insert((uint64_t(address) << 32) | size).second) {
      g_frameReads.push_back({address, size});
    }
  }
  // (inline draws' data, always copied, at addresses and sizes that vary
  // without end: a range of their own, not kept)
  GuestMemoryRange scratch;
  auto& range = always ? scratch : g_guestMemoryRanges[(uint64_t(address) << 32) | size];
  if (bones) {
    range.late = true;
  }
  if (range.late) {
    if (range.lateFrame != g_frameCount) {
      range.lateFrame = g_frameCount;
      Upload upload = Allocate(size, 16);
      g_copyTag = bones && InterpEnabled() ? NoteInterpPalette(address, size) : -1;
      RecordGuestMemoryCopy(address, upload, size, inOrder);
      g_copyTag = -1;
      g_frameRanges.push_back({address, size, &range, upload.data});
    }
    return;
  }
  bool first = range.checkedFrame == ~0ull;
  // Unchanged for kRangeStableFrames: compared every kRangeRecheckFrames
  // frames (static geometry; comparing it all twice a frame was most of this
  // stage). A rewrite after that shows up to that many frames late.
  if (!always && !first && !range.dynamic &&
      g_frameCount - range.changedFrame > kRangeStableFrames &&
      g_frameCount - range.checkedFrame < kRangeRecheckFrames) {
    return;
  }
  if (!always && range.checkedFrame != g_frameCount) {
    g_frameRanges.push_back({address, size, &range, nullptr});
  }
  // Unchanged for a while (memory reused, a buffer patched once): back to once a frame.
  if (range.dynamic && range.changedFrame + 8 < g_frameCount) {
    range.dynamic = false;
  }
  bool stable = !first && !range.dynamic && range.changedFrame + 1 < g_frameCount;
  if (!always && stable && range.checkedFrame == g_frameCount) {
    return;
  }
  range.checkedFrame = g_frameCount;
  const uint8_t* source = guest::Physical(address);
  uint8_t* shadow = g_guestMemoryShadow + address;
  g_profile.comparedBytes += size;
  if (!first && g_frameCount - range.changedFrame > 60) {
    g_profile.quietBytes += size;
  }
  // Upload only the span between the first and last block that differ.
  uint32_t begin = 0, end = size;
  if (!first) {
    if (std::memcmp(source, shadow, size) == 0) {
      return;
    }
    constexpr uint32_t kBlock = 256;
    while (begin < size &&
           std::memcmp(source + begin, shadow + begin, std::min(kBlock, size - begin)) == 0) {
      begin += kBlock;
    }
    while (end > begin) {
      uint32_t blockStart = std::max(begin, (end - 1) / kBlock * kBlock);
      if (std::memcmp(source + blockStart, shadow + blockStart, end - blockStart) != 0) {
        break;
      }
      end = blockStart;
    }
  }
  // (the game may write it between the comparisons above: by the blocks it is
  // unchanged after all, and the GPU's copy already matches)
  if (begin >= end) {
    return;
  }
  uint32_t length = end - begin;
  range.changedFrame = g_frameCount;
  g_profile.uploadedBytes += length;
  std::memcpy(shadow + begin, source + begin, length);
  Upload upload = Allocate(length, 16);
  std::memcpy(upload.data, source + begin, length);
  RecordGuestMemoryCopy(address + begin, upload, length, inOrder);
}

// End of frame, before submitting: fills the late ranges' copies and finds
// the ranges written after their draws.
void FinishGuestMemory() {
  uint64_t startTicks = g_profile.enabled ? TimerTicks() : 0;
  // Find the ranges written after their draws first, against the shadow
  // before the late copies below change it.
  for (auto& frameRange : g_frameRanges) {
    auto& range = *frameRange.range;
    // Unchanged for over a second: looked at here every 8th time it comes
    // by (its own count, started from a hash of its address so each frame
    // takes a share; stable ranges come by only every 4th frame). Nearly all
    // the bytes here were such ranges, and this second comparison of them
    // was most of the present stage on CPUs without a large cache (#78). One
    // the game starts writing after its draws is still copied at its next
    // draw, which marks it changed and so looked at here every frame again:
    // found late a few frames later than before.
    if (!frameRange.lateCopy && g_frameCount - range.changedFrame > 60 &&
        (range.finishTurn++ + (frameRange.address * 0x9E3779B1u >> 29)) % 8 != 0) {
      continue;
    }
    if (!frameRange.lateCopy) {
      g_profile.finishBytes += frameRange.size;
    }
    if (frameRange.lateCopy ||
        std::memcmp(guest::Physical(frameRange.address),
                    g_guestMemoryShadow + frameRange.address, frameRange.size) == 0) {
      continue;
    }
    if (range.changedFrame + 1 >= g_frameCount) {
      // Checked at every draw this frame and still written after the last.
      range.late = true;
      REXLOG_DEBUG("video: {:#x} ({} bytes) is written after its draws", frameRange.address,
                   frameRange.size);
    } else {
      // Checked once this frame: it may be filled between two of its draws, so from now on
      // check it at every draw.
      range.changedFrame = g_frameCount;
      range.dynamic = true;
    }
  }
  for (auto& frameRange : g_frameRanges) {
    if (frameRange.lateCopy) {
      const uint8_t* source = guest::Physical(frameRange.address);
      std::memcpy(frameRange.lateCopy, source, frameRange.size);
      std::memcpy(g_guestMemoryShadow + frameRange.address, source, frameRange.size);
      g_profile.uploadedBytes += frameRange.size;
    }
  }
  g_frameRanges.clear();
  g_frameReads.clear();
  g_frameReadKeys.clear();
  g_frameMemExport = false;
  if (g_profile.enabled) {
    g_profile.finishNs += TicksToNs(TimerTicks() - startTicks);
  }
}

// Shaders ---------------------------------------------------------------------

const ShaderCacheEntry* FindShaderCacheEntry(uint64_t hash) {
  if (!g_shaderCacheHasMasks) {  // (a real reference: links only against a current cache)
    return nullptr;
  }
  auto* begin = g_shaderCacheEntries;
  auto* end = g_shaderCacheEntries + g_shaderCacheEntryCount;
  auto* it = std::lower_bound(begin, end, hash, [](const ShaderCacheEntry& entry,
                                                   uint64_t value) { return entry.hash < value; });
  return (it != end && it->hash == hash) ? it : nullptr;
}

// Vertex shaders ask for round toward zero (docs/shaders.md); a GPU without
// it gets them without: the capability, its extension and the mode removed.
// NG2_RTZ=0 removes it everywhere (a test switch for #18, Intel Arc).
bool RoundTowardZero() {
  static const bool on = [] {
    const char* v = std::getenv("NG2_RTZ");
    return g_device->getCapabilities().roundingModeRTZ && !(v && v[0] == '0');
  }();
  return on;
}

void DropRoundTowardZero(std::vector<uint8_t>& spirv) {
  constexpr uint32_t kOpExtension = 10, kOpExecutionMode = 16, kOpCapability = 17;
  constexpr uint32_t kRoundingModeRTZ = 4463, kCapabilityRTZ = 4468;
  std::vector<uint32_t> words(spirv.size() / 4);
  std::memcpy(words.data(), spirv.data(), words.size() * 4);
  std::vector<uint32_t> kept(words.begin(), words.begin() + std::min<size_t>(5, words.size()));
  for (size_t at = 5; at < words.size();) {
    uint32_t opcode = words[at] & 0xFFFF, count = words[at] >> 16;
    if (count == 0 || at + count > words.size()) {
      return;  // not well formed: left alone
    }
    bool drop = (opcode == kOpCapability && words[at + 1] == kCapabilityRTZ) ||
                (opcode == kOpExecutionMode && count > 2 && words[at + 2] == kRoundingModeRTZ) ||
                (opcode == kOpExtension &&
                 std::strncmp(reinterpret_cast<const char*>(&words[at + 1]), "SPV_KHR_float_controls",
                              (count - 1) * 4) == 0);
    if (!drop) {
      kept.insert(kept.end(), words.begin() + at, words.begin() + at + count);
    }
    at += count;
  }
  spirv.resize(kept.size() * 4);
  std::memcpy(spirv.data(), kept.data(), spirv.size());
}

RenderShader* HostShader(const ShaderCacheEntry* entry) {
  auto& host = g_hostShaders[entry];
  if (!host) {
    if (!g_spirvCache) {
      g_spirvCache = std::make_unique<uint8_t[]>(g_spirvCacheDecompressedSize);
      ZSTD_decompress(g_spirvCache.get(), g_spirvCacheDecompressedSize, g_compressedSpirvCache,
                      g_spirvCacheCompressedSize);
    }
    // On NVIDIA the shaders that hide cut-off parts interpolate the part id
    // themselves where the GPU has fragment shader barycentrics: its hardware
    // interpolation is a hair off there (speckled cut limbs). Elsewhere the
    // hardware's is exact enough. NG2_BARYCENTRIC=1 or 0 forces it on or off.
    static const bool barycentric = [] {
      const char* v = std::getenv("NG2_BARYCENTRIC");
      bool wanted = v ? v[0] == '1' : g_device->getDescription().vendor == RenderDeviceVendor::NVIDIA;
      bool on = g_device->getCapabilities().fragmentShaderBarycentric && wanted;
      REXLOG_INFO("video: part ids interpolated {}", on ? "from barycentrics" : "by the hardware");
      return on;
    }();
    bool variant = barycentric && entry->barycentricSpirvSize;
    const uint8_t* encoded =
        g_spirvCache.get() + (variant ? entry->barycentricSpirvOffset : entry->spirvOffset);
    uint32_t encodedSize = variant ? entry->barycentricSpirvSize : entry->spirvSize;
    size_t size = smolv::GetDecodedBufferSize(encoded, encodedSize);
    std::vector<uint8_t> spirv(size);
    if (smolv::Decode(encoded, encodedSize, spirv.data(), size)) {
      MakeTimer made(g_frameStats.shaders, g_frameStats.shaderNs);
      if (!RoundTowardZero()) {
        DropRoundTowardZero(spirv);
      }
      host = g_device->createShader(spirv.data(), spirv.size(), "main", RenderShaderFormat::SPIRV);
    }
  }
  return host.get();
}

RenderShader* GetHostShader(GuestShader& guestShader) {
  return guestShader.entry ? HostShader(guestShader.entry) : nullptr;
}


// Textures --------------------------------------------------------------------

struct TextureFormatInfo {
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t blockSize = 1;      // texels per block side
  uint32_t bytesPerBlock = 0;
};

TextureFormatInfo GetTextureFormatInfo(xenos::TextureFormat format) {
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_8: return {RenderFormat::R8_UNORM, 1, 1};
    case F::k_8_8: return {RenderFormat::R8G8_UNORM, 1, 2};
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_AS_16_16_16_16: return {RenderFormat::R8G8B8A8_UNORM, 1, 4};
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16: return {RenderFormat::R10G10B10A2_UNORM, 1, 4};
    // Red in the low bits: swapped with blue at upload (SwapRedBlue565), as Xenia does.
    case F::k_5_6_5: return {RenderFormat::B5G6R5_UNORM, 1, 2};
    case F::k_16: return {RenderFormat::R16_UNORM, 1, 2};
    case F::k_16_16: return {RenderFormat::R16G16_UNORM, 1, 4};
    case F::k_16_16_16_16: return {RenderFormat::R16G16B16A16_UNORM, 1, 8};
    case F::k_16_FLOAT: return {RenderFormat::R16_FLOAT, 1, 2};
    case F::k_16_16_FLOAT: return {RenderFormat::R16G16_FLOAT, 1, 4};
    case F::k_16_16_16_16_FLOAT:
    case F::k_16_16_16_16_EXPAND: return {RenderFormat::R16G16B16A16_FLOAT, 1, 8};
    case F::k_32_FLOAT: return {RenderFormat::R32_FLOAT, 1, 4};
    case F::k_32_32_FLOAT: return {RenderFormat::R32G32_FLOAT, 1, 8};
    case F::k_32_32_32_32_FLOAT: return {RenderFormat::R32G32B32A32_FLOAT, 1, 16};
    case F::k_DXT1: return {RenderFormat::BC1_UNORM, 4, 8};
    case F::k_DXT2_3: return {RenderFormat::BC2_UNORM, 4, 16};
    case F::k_DXT4_5: return {RenderFormat::BC3_UNORM, 4, 16};
    case F::k_DXN: return {RenderFormat::BC5_UNORM, 4, 16};
    case F::k_DXT5A: return {RenderFormat::BC4_UNORM, 4, 8};
    default: return {};
  }
}

// k_5_6_5 keeps red in the low 5 bits; the host format (VK R5G6B5_PACK16,
// DXGI B5G6R5) has it in the high ones.
void SwapRedBlue565(std::vector<uint8_t>& data) {
  for (size_t i = 0; i + 1 < data.size(); i += 2) {
    uint16_t v;
    std::memcpy(&v, data.data() + i, 2);
    v = uint16_t((v >> 11) | (v & 0x07E0) | (v << 11));
    std::memcpy(data.data() + i, &v, 2);
  }
}

// Address of block (x, y) in a tiled 2D texture, from Xenia
// (texture_util.cc, after UModel's UnTexture.cpp).
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytesPerBlockLog2) {
  pitch = (pitch + 31) & ~31u;
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytesPerBlockLog2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytesPerBlockLog2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

void EndianSwap(uint8_t* data, size_t size, xenos::Endian endian) {
  switch (endian) {
    // Whole words (the compiler vectorizes these loops).
    case xenos::Endian::k8in16:
      for (size_t i = 0; i + 1 < size; i += 2) {
        uint16_t v;
        std::memcpy(&v, data + i, 2);
        v = rex::byte_swap(v);
        std::memcpy(data + i, &v, 2);
      }
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        v = rex::byte_swap(v);
        std::memcpy(data + i, &v, 4);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        v = (v >> 16) | (v << 16);
        std::memcpy(data + i, &v, 4);
      }
      break;
    default:
      break;
  }
}

RenderSwizzle ConvertSwizzle(uint32_t component) {
  switch (component) {
    case 0: return RenderSwizzle::R;
    case 1: return RenderSwizzle::G;
    case 2: return RenderSwizzle::B;
    case 3: return RenderSwizzle::A;
    case 4: return RenderSwizzle::ZERO;
    default: return RenderSwizzle::ONE;
  }
}

RenderTextureAddressMode ConvertClamp(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat: return RenderTextureAddressMode::WRAP;
    case xenos::ClampMode::kMirroredRepeat: return RenderTextureAddressMode::MIRROR;
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway: return RenderTextureAddressMode::MIRROR_ONCE;
    case xenos::ClampMode::kClampToBorder:
    case xenos::ClampMode::kMirrorClampToBorder:
      return TestNoBorder() ? RenderTextureAddressMode::CLAMP : RenderTextureAddressMode::BORDER;
    default: return RenderTextureAddressMode::CLAMP;
  }
}

uint32_t GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch) {
  RenderSamplerDesc desc;
  desc.addressU = ConvertClamp(fetch.clamp_x);
  desc.addressV = ConvertClamp(fetch.clamp_y);
  desc.addressW = ConvertClamp(fetch.clamp_z);
  desc.magFilter = fetch.mag_filter == xenos::TextureFilter::kPoint ? RenderFilter::NEAREST
                                                                    : RenderFilter::LINEAR;
  desc.minFilter = fetch.min_filter == xenos::TextureFilter::kPoint ? RenderFilter::NEAREST
                                                                    : RenderFilter::LINEAR;
  desc.mipmapMode = fetch.mip_filter == xenos::TextureFilter::kLinear ? RenderMipmapMode::LINEAR
                                                                       : RenderMipmapMode::NEAREST;
  // The texture views hold only the levels the fetch constant has.
  // Without a base address only the mips are there (Xenia's
  // GetSubresourcesFromFetchConstant).
  desc.minLOD = float(fetch.base_address ? uint32_t(fetch.mip_min_level)
                                         : std::max(uint32_t(fetch.mip_min_level), 1u));
  desc.maxLOD = fetch.mip_filter == xenos::TextureFilter::kBaseMap ? desc.minLOD : FLT_MAX;
  desc.mipLODBias = std::clamp(float(fetch.lod_bias) / 32.0f, -15.0f, 15.0f);
  if (int32_t bias = REXCVAR_GET(texture_lod_bias); bias == 1 || bias == 2) {
    desc.mipLODBias = bias == 1 ? 0.0f : -1.0f;
  }
  // Anisotropy as the fetch constant asks (1x-16x), for filtered mipmapped
  // sampling like Xenia's; it implies linear filtering.
  uint32_t aniso = uint32_t(fetch.aniso_filter);
  if (int32_t forced = REXCVAR_GET(anisotropic_filtering); forced >= 0) {
    aniso = uint32_t(forced);  // the player's choice, as Xenia's anisotropic_override
  }
  if (aniso >= 2 && aniso <= 5 && fetch.mip_filter != xenos::TextureFilter::kBaseMap &&
      desc.minFilter == RenderFilter::LINEAR && desc.magFilter == RenderFilter::LINEAR) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = 1u << (aniso - 1);
  }
  // By field: the desc has padding bytes.
  float lod[3] = {desc.minLOD, desc.maxLOD, desc.mipLODBias};
  uint32_t keyData[10] = {uint32_t(desc.addressU), uint32_t(desc.addressV),
                          uint32_t(desc.addressW), uint32_t(desc.magFilter),
                          uint32_t(desc.minFilter), uint32_t(desc.mipmapMode),
                          desc.anisotropyEnabled ? desc.maxAnisotropy : 0u};
  std::memcpy(&keyData[7], lod, sizeof(lod));
  uint64_t hash = XXH3_64bits(keyData, sizeof(keyData));
  auto& sampler = g_samplers[hash];
  if (!sampler.second) {
    if (g_nextSampler >= SAMPLER_DESCRIPTOR_COUNT) {
      return SAMPLER_LINEAR_CLAMP;
    }
    sampler.first = g_nextSampler++;
    sampler.second = g_device->createSampler(desc);
    RenderSampler* made = sampler.second.get();
    uint32_t index = sampler.first;
    OnRenderThread([=](RenderCommandList*) { g_samplerDescriptorSet->setSampler(index, made); });
  }
  return sampler.first;
}

// Where each level of a guest texture lies in memory (Xenia's
// texture_util::GetGuestTextureLayout and GetPackedMipOffset, 2D only).
struct LevelLayout {
  uint32_t width, height;     // texels
  uint32_t pitchBlocks;       // row pitch, aligned to a 32-block tile
  uint32_t offsetX, offsetY;  // blocks into the packed mip tail
  uint64_t start;             // bytes from the base or mip address
  uint64_t size;              // bytes from start the level spans
};
struct TextureLayout {
  std::vector<LevelLayout> levels;  // the base, then the mips
  uint32_t mipAddress = 0;          // guest physical
  uint64_t mipsSize = 0;            // bytes from mipAddress the mips span
};

uint32_t Log2Ceil(uint32_t value) { return uint32_t(std::bit_width(value - 1)); }

// Bytes a level spans from its start. Tiled addresses of the last tile can
// reach past pitch x rows (the 8-bit last row of a 300x145 HUD texture):
// Xenia's GetTiledAddressUpperBound2D, from the origin of the last 32x32 tile.
uint64_t LevelExtent(bool tiled, uint32_t pitchBlocks, uint32_t rightBlocks,
                     uint32_t bottomBlocks, uint32_t bytesPerBlock) {
  if (!tiled) {
    return uint64_t(pitchBlocks) * (bottomBlocks - 1) * bytesPerBlock +
           uint64_t(rightBlocks) * bytesPerBlock;
  }
  uint32_t bppLog2 = uint32_t(std::countr_zero(bytesPerBlock));
  uint64_t bound = uint32_t(TiledOffset2D(int32_t((rightBlocks - 1) & ~31u),
                                          int32_t((bottomBlocks - 1) & ~31u), pitchBlocks,
                                          bppLog2));
  return bound + (bppLog2 == 0 ? 0xA00 : bppLog2 == 1 ? 0xC00 : 0x400u << bppLog2);
}

// Levels from packedLevel on share one 32x32 tile, each at its own offset:
// 16 texels across for the first (down for wide textures), then 8 and 4, then
// the 2x2 and 1x1 ones along the other axis.
void PackedMipOffset(uint32_t width, uint32_t height, uint32_t level, uint32_t blockSize,
                     uint32_t& x, uint32_t& y) {
  uint32_t log2Width = Log2Ceil(width), log2Height = Log2Ceil(height);
  uint32_t log2Size = std::min(log2Width, log2Height);
  uint32_t packedBase = log2Size > 4 ? log2Size - 4 : 0;
  uint32_t packed = level - packedBase;
  bool wide = log2Width > std::max(log2Height, 1u);  // 2x1 packs like a square
  x = y = 0;
  if (packed < 3) {
    (wide ? y : x) = 16 >> packed;
  } else if (wide) {
    x = (1u << (log2Width - packedBase)) >> (packed - 2);
  } else {
    y = (1u << (log2Height - packedBase)) >> (packed - 2);
  }
  x /= blockSize;
  y /= blockSize;
}

TextureLayout GetTextureLayout(const xenos::xe_gpu_texture_fetch_t& fetch,
                               const TextureFormatInfo& info, uint32_t width, uint32_t height) {
  TextureLayout layout;
  auto blocks = [&](uint32_t texels) { return (texels + info.blockSize - 1) / info.blockSize; };
  bool oneD = fetch.dimension == xenos::DataDimension::k1D;
  // (1D textures don't pack their mips: Xenia ignores the flag there.)
  uint32_t log2Size = std::min(Log2Ceil(width), Log2Ceil(height));
  uint32_t packedLevel =
      fetch.packed_mips && !oneD ? (log2Size > 4 ? log2Size - 4 : 0) : ~0u;

  LevelLayout base = {width, height};
  uint32_t pitchTexels = std::max(uint32_t(fetch.pitch) << 5, width);
  base.pitchBlocks = (blocks(pitchTexels) + 31) & ~31u;
  if (packedLevel == 0) {
    PackedMipOffset(width, height, 0, info.blockSize, base.offsetX, base.offsetY);
  }
  base.size = LevelExtent(fetch.tiled, base.pitchBlocks, blocks(width) + base.offsetX,
                          blocks(height) + base.offsetY, info.bytesPerBlock);
  layout.levels.push_back(base);

  // With the base in the mip tail, the whole tail is at the base address.
  uint32_t mipPage = packedLevel == 0 && fetch.base_address ? uint32_t(fetch.base_address)
                                                            : uint32_t(fetch.mip_address);
  // No mips for 1D or stacked textures (sampled as one 2D slice).
  uint32_t sizeMaxLevel = uint32_t(std::bit_width(std::max(width, height)) - 1);
  uint32_t maxLevel = 0;
  if (mipPage && !oneD && !fetch.stacked) {
    maxLevel = std::max(std::min(uint32_t(fetch.mip_max_level), sizeMaxLevel),
                        std::min(uint32_t(fetch.mip_min_level), sizeMaxLevel));
  }
  layout.mipAddress = mipPage << 12;
  // Mips are laid out as for power-of-two base sizes, each level's rows
  // aligned to 32 blocks (linear ones to 256 bytes) and its storage to 4 KB.
  uint32_t pitchAlignment = fetch.tiled ? 32 : std::max(256 / info.bytesPerBlock, 32u);
  uint64_t offset = 0;
  for (uint32_t level = 1; level <= maxLevel; level++) {
    uint32_t stored = std::min(level, packedLevel);
    LevelLayout mip = {std::max(width >> level, 1u), std::max(height >> level, 1u)};
    uint32_t pitch = blocks(std::max(std::bit_ceil(width) >> stored, 1u));
    mip.pitchBlocks = (pitch + pitchAlignment - 1) / pitchAlignment * pitchAlignment;
    if (level >= packedLevel) {
      PackedMipOffset(width, height, level, info.blockSize, mip.offsetX, mip.offsetY);
    }
    mip.start = offset;
    mip.size = LevelExtent(fetch.tiled, mip.pitchBlocks, blocks(mip.width) + mip.offsetX,
                           blocks(mip.height) + mip.offsetY, info.bytesPerBlock);
    layout.mipsSize = std::max(layout.mipsSize, mip.start + mip.size);
    if (level < packedLevel) {
      uint32_t rows = (blocks(std::max(std::bit_ceil(height) >> stored, 1u)) + 31) & ~31u;
      offset += (uint64_t(mip.pitchBlocks) * rows * info.bytesPerBlock + 0xFFF) & ~0xFFFull;
    }
    layout.levels.push_back(mip);
  }
  return layout;
}

// Untiles one level into rows of its blocks. data and size: the memory at
// the level's base or mip address, endian swapped.
// A tiled level's rows, a block at a time: Bytes fixed so each copy is a
// move, not a memcpy call (8- and 16-bit textures made one call a texel).
template <uint32_t Bytes>
void CopyTiledRows(const LevelLayout& level, uint32_t blocksX, uint32_t blocksY, uint32_t bppLog2,
                   const uint8_t* data, uint64_t size, uint8_t* out) {
  // In Xenos tiling a run of texels along x is contiguous: 8 one-byte texels,
  // else 16 bytes' worth (x's low bits land in the address's low 3 or 4 bits).
  constexpr uint32_t kRun = Bytes == 1 ? 8 : (Bytes >= 16 ? 1 : 16 / Bytes);
  bool aligned = level.offsetX % kRun == 0;
  for (uint32_t y = 0; y < blocksY; y++) {
    uint8_t* row = out + uint64_t(y) * blocksX * Bytes;
    int32_t guestY = int32_t(y + level.offsetY);
    uint32_t x = 0;
    if (aligned) {
      for (; x + kRun <= blocksX; x += kRun) {
        int32_t offset =
            TiledOffset2D(int32_t(x + level.offsetX), guestY, level.pitchBlocks, bppLog2);
        if (offset >= 0 && uint64_t(offset) + kRun * Bytes <= size) {
          std::memcpy(row + x * Bytes, data + offset, kRun * Bytes);
        } else if (offset >= 0) {
          for (uint32_t k = 0; k < kRun && uint64_t(offset) + (k + 1) * Bytes <= size; k++) {
            std::memcpy(row + (x + k) * Bytes, data + offset + k * Bytes, Bytes);
          }
        }
      }
    }
    for (; x < blocksX; x++) {
      int32_t offset = TiledOffset2D(int32_t(x + level.offsetX), guestY, level.pitchBlocks, bppLog2);
      if (offset >= 0 && uint64_t(offset) + Bytes <= size) {
        std::memcpy(row + x * Bytes, data + offset, Bytes);
      }
    }
  }
}

void CopyLevel(const xenos::xe_gpu_texture_fetch_t& fetch, const TextureFormatInfo& info,
               const LevelLayout& level, const uint8_t* data, uint64_t size, uint8_t* out) {
  uint32_t blocksX = (level.width + info.blockSize - 1) / info.blockSize;
  uint32_t blocksY = (level.height + info.blockSize - 1) / info.blockSize;
  uint32_t bppLog2 = uint32_t(std::countr_zero(info.bytesPerBlock));
  uint64_t rowBytes = uint64_t(blocksX) * info.bytesPerBlock;
  uint64_t linearRowBytes = uint64_t(level.pitchBlocks) * info.bytesPerBlock;
  if (level.start >= size) {
    return;
  }
  data += level.start;
  size -= level.start;
  if (fetch.tiled) {
    switch (info.bytesPerBlock) {
      case 1: return CopyTiledRows<1>(level, blocksX, blocksY, bppLog2, data, size, out);
      case 2: return CopyTiledRows<2>(level, blocksX, blocksY, bppLog2, data, size, out);
      case 4: return CopyTiledRows<4>(level, blocksX, blocksY, bppLog2, data, size, out);
      case 8: return CopyTiledRows<8>(level, blocksX, blocksY, bppLog2, data, size, out);
      case 16: return CopyTiledRows<16>(level, blocksX, blocksY, bppLog2, data, size, out);
      default: break;
    }
  }
  for (uint32_t y = 0; y < blocksY; y++) {
    uint8_t* row = out + y * rowBytes;
    uint32_t guestY = y + level.offsetY;
    if (fetch.tiled) {
      for (uint32_t x = 0; x < blocksX; x++) {
        int32_t offset = TiledOffset2D(int32_t(x + level.offsetX), int32_t(guestY),
                                       level.pitchBlocks, bppLog2);
        if (offset >= 0 && uint64_t(offset) + info.bytesPerBlock <= size) {
          std::memcpy(row + x * info.bytesPerBlock, data + offset, info.bytesPerBlock);
        }
      }
    } else {
      uint64_t start = guestY * linearRowBytes + uint64_t(level.offsetX) * info.bytesPerBlock;
      if (start + rowBytes <= size) {
        std::memcpy(row, data + start, rowBytes);
      }
    }
  }
}

// Uploads a guest texture and its mips: endian swap, untile.
std::unique_ptr<HostTexture> CreateTexture(const xenos::xe_gpu_texture_fetch_t& fetch,
                                           const TextureFormatInfo& info,
                                           const TextureLayout& layout, const uint8_t* baseData,
                                           uint64_t baseSize, const uint8_t* mipData,
                                           uint64_t mipSize, bool* retry = nullptr) {
  uint32_t width = layout.levels[0].width, height = layout.levels[0].height;
  std::vector<uint8_t> base(baseData, baseData + baseSize);
  EndianSwap(base.data(), base.size(), fetch.endianness);
  std::vector<uint8_t> mips(mipData, mipData + mipSize);
  EndianSwap(mips.data(), mips.size(), fetch.endianness);
  if (fetch.format == xenos::TextureFormat::k_5_6_5) {
    SwapRedBlue565(base);
    SwapRedBlue565(mips);
  }

  // The Options header while RECOMP SETTINGS is open: a wider picture of
  // ours, one level (its draws are widened to match in Draw).
  std::vector<uint8_t> header;
  uint32_t headerWidth = 0;
  if ((uint32_t(fetch.base_address) << 12) == g_menuHeader.base) {
    g_menuHeader.widthScale = 1.0f;  // something else there now, unless it's the header again
  }
  if ((uint32_t(fetch.base_address) << 12) == g_inGameHeaderBase) {
    g_inGameHeaderBase = ~0u;
  }
  if (info.format == RenderFormat::R8_UNORM && info.blockSize == 1 &&
      IsMenuHeaderSize(width, height)) {
    std::vector<uint8_t> level0(size_t(width) * height);
    CopyLevel(fetch, info, layout.levels[0], base.data(), base.size(), level0.data());
    MenuHeaderKind kind = MenuHeaderKind::kNone;
    bool ours = ReplaceMenuHeader(width, height, level0.data(), header, headerWidth, kind);
    if (kind == MenuHeaderKind::kOptions) {
      g_menuHeader = {uint32_t(fetch.base_address) << 12,
                      ours ? float(headerWidth) / float(width) : 1.0f};
    } else if (kind == MenuHeaderKind::kInGame) {
      g_inGameHeaderBase = uint32_t(fetch.base_address) << 12;
    }
  }
  TextureLayout replaced;
  const TextureLayout* used = &layout;
  if (!header.empty()) {
    replaced.levels = {LevelLayout{headerWidth, height, 0, 0, 0, 0, header.size()}};
    used = &replaced;
    width = headerWidth;
  }

  std::vector<Upload> uploads;
  bool isPrompts = false;
  uint32_t labelRows = 0;  // rows of the base level a menu label rewrote
  for (size_t i = 0; i < used->levels.size(); i++) {
    const LevelLayout& level = used->levels[i];
    uint64_t bytes = uint64_t((level.width + info.blockSize - 1) / info.blockSize) *
                     ((level.height + info.blockSize - 1) / info.blockSize) * info.bytesPerBlock;
    Upload upload = Allocate(bytes, 16);
    std::memset(upload.data, 0, bytes);
    if (!header.empty()) {
      std::memcpy(upload.data, header.data(), header.size());
    } else if (i == 0) {
      CopyLevel(fetch, info, level, base.data(), base.size(), upload.data);
      // The button atlas and its palette: the chosen prompts (prompts.cpp).
      isPrompts = prompts::Apply(level.width, level.height,
                     info.format == RenderFormat::R8_UNORM          ? prompts::Kind::kIndex8
                     : info.format == RenderFormat::R8G8B8A8_UNORM ? prompts::Kind::kRGBA8
                                                                    : prompts::Kind::kOther,
                     upload.data, bytes);
      // Another language's title atlas: the English one (loaded with it, one
      // package for every language) found in memory by its first 4 KB as
      // stored, and decoded alike, for its letters (menu_label.cpp).
      if (info.format == RenderFormat::R8_UNORM &&
          NeedsEnglishAtlas(level.width, level.height, upload.data)) {
        constexpr uint64_t kEnglishAtlasStart = 0x67d8418c93aeec2bull;
        constexpr uint32_t kUsual = 0x1037A000;  // where it is with this game's packages
        constexpr uint32_t kPhysicalSize = 512u << 20;
        bool found = false;
        auto tryAt = [&](uint32_t at) {
          if (at + baseSize > kPhysicalSize ||
              XXH3_64bits(guest::Physical(at), 4096) != kEnglishAtlasStart) {
            return false;
          }
          std::vector<uint8_t> english(guest::Physical(at), guest::Physical(at) + baseSize);
          EndianSwap(english.data(), english.size(), fetch.endianness);
          std::vector<uint8_t> pixels(size_t(level.width) * level.height);
          CopyLevel(fetch, info, level, english.data(), english.size(), pixels.data());
          return SetEnglishAtlas(pixels.data());
        };
        found = tryAt(kUsual);
        // Near this atlas first (the same package), then everywhere.
        uint32_t here = uint32_t(fetch.base_address) << 12;
        uint32_t nearFrom = here > (16u << 20) ? here - (16u << 20) : 0;
        uint32_t nearTo = std::min(here + (16u << 20), kPhysicalSize);
        for (uint32_t at = nearFrom & ~4095u; !found && at + 4096 <= nearTo; at += 4096) {
          found = at != kUsual && tryAt(at);
        }
        for (uint32_t at = 0; !found && at + 4096 <= kPhysicalSize; at += 4096) {
          found = at != kUsual && (at < nearFrom || at >= nearTo) && tryAt(at);
        }
        if (!found) {
          REXLOG_WARN("video: the title menu's English letters weren't found: its labels stay");
          EnglishAtlasMissing();
        }
      }
      if (info.format == RenderFormat::R8_UNORM) {
        LabelResult result = RewriteMenuLabel(level.width, level.height, upload.data, &labelRows);
        if (result == LabelResult::kRetry && retry) {
          *retry = true;
        }
        if (result == LabelResult::kRewritten && level.height == labelRows) {
          g_menuLabelBase = uint32_t(fetch.base_address) << 12;  // the label alone, ours
        }
      }
    } else {
      CopyLevel(fetch, info, level, mips.data(), mips.size(), upload.data);
      // The rewritten rows' mips: 2x2 averages of the level above.
      const LevelLayout& above = used->levels[i - 1];
      uint32_t rows = std::min((labelRows + (1u << i) - 1) >> i, level.height);
      for (uint32_t y = 0; labelRows && y < rows; y++) {
        for (uint32_t x = 0; x < level.width; x++) {
          uint32_t sum = 0;
          for (uint32_t k = 0; k < 4; k++) {
            uint32_t sx = std::min(2 * x + (k & 1), above.width - 1);
            uint32_t sy = std::min(2 * y + (k >> 1), above.height - 1);
            sum += uploads[i - 1].data[sy * above.width + sx];
          }
          upload.data[y * level.width + x] = uint8_t((sum + 2) / 4);
        }
      }
    }
    uploads.push_back(upload);
  }
  // The base level, for the dumps below.
  uint32_t blocksX = (width + info.blockSize - 1) / info.blockSize;
  uint32_t blocksY = (height + info.blockSize - 1) / info.blockSize;
  uint64_t rowBytes = uint64_t(blocksX) * info.bytesPerBlock;
  const Upload& upload = uploads[0];

  // NG2_DUMP_TEXTURES=<dir>: write decoded 8-bit and 8888 textures (PGM/PAM).
  static const char* dumpDir = std::getenv("NG2_DUMP_TEXTURES");
  // Block-compressed textures as DDS (DXT1/DXT3/DXT5).
  if (dumpDir && (info.format == RenderFormat::BC1_UNORM ||
                  info.format == RenderFormat::BC2_UNORM ||
                  info.format == RenderFormat::BC3_UNORM)) {
    std::string path = fmt::format("{}/{:08x}_{}x{}.dds", dumpDir,
                                   uint32_t(fetch.base_address) << 12, width, height);
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      uint32_t header[32] = {};
      header[0] = 0x20534444;  // "DDS "
      header[1] = 124;
      header[2] = 0x1007;  // caps, height, width, pixel format
      header[3] = height;
      header[4] = width;
      header[19] = 32;
      header[20] = 4;  // FOURCC
      header[21] = info.format == RenderFormat::BC1_UNORM   ? 0x31545844   // DXT1
                   : info.format == RenderFormat::BC2_UNORM ? 0x33545844   // DXT3
                                                            : 0x35545844;  // DXT5
      header[27] = 0x1000;
      std::fwrite(header, 4, 32, f);
      std::fwrite(upload.data, 1, rowBytes * blocksY, f);
      std::fclose(f);
    }
  }
  if (dumpDir && (info.format == RenderFormat::R8_UNORM ||
                  info.format == RenderFormat::R8G8B8A8_UNORM)) {
    bool gray = info.format == RenderFormat::R8_UNORM;
    std::string path = fmt::format("{}/{:08x}_{}x{}.{}", dumpDir,
                                   uint32_t(fetch.base_address) << 12, width, height,
                                   gray ? "pgm" : "pam");
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      if (gray) {
        std::fprintf(f, "P5\n%u %u\n255\n", width, height);
      } else {
        std::fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
                     width, height);
      }
      for (uint32_t y = 0; y < height; y++) {
        std::fwrite(upload.data + y * rowBytes, 1, width * info.bytesPerBlock, f);
      }
      std::fclose(f);
    }
  }

  if (dumpDir && info.format == RenderFormat::B5G6R5_UNORM) {  // (as RGB, red from the high bits)
    std::string path = fmt::format("{}/{:08x}_{}x{}_565.ppm", dumpDir,
                                   uint32_t(fetch.base_address) << 12, width, height);
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", width, height);
      for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
          uint16_t v;
          std::memcpy(&v, upload.data + y * rowBytes + x * 2, 2);
          uint8_t rgb[3] = {uint8_t((v >> 11) * 255 / 31), uint8_t(((v >> 5) & 63) * 255 / 63),
                            uint8_t((v & 31) * 255 / 31)};
          std::fwrite(rgb, 1, 3, f);
        }
      }
      std::fclose(f);
    }
  }
  auto host = std::make_unique<HostTexture>();
  host->prompts = isPrompts;
  host->width = width;
  host->height = height;
  host->format = info.format;
  uint32_t levelCount = uint32_t(used->levels.size());
  host->texture = g_device->createTexture(
      RenderTextureDesc::Texture2D(width, height, levelCount, info.format));
  if (!host->texture) {
    REXLOG_ERROR("video: no {}x{} texture", width, height);
    return nullptr;
  }
  Transition(*host, RenderTextureLayout::COPY_DEST);
  for (uint32_t i = 0; i < levelCount; i++) {
    const LevelLayout& level = used->levels[i];
    uint32_t rowTexels = (level.width + info.blockSize - 1) / info.blockSize * info.blockSize;
    CommandList()->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(host->texture.get(), i),
        RenderTextureCopyLocation::PlacedFootprint(uploads[i].buffer, info.format, level.width,
                                                   level.height, 1, rowTexels,
                                                   uploads[i].offset));
  }

  RenderTextureViewDesc viewDesc;
  viewDesc.format = info.format;
  viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
  viewDesc.mipLevels = levelCount;
  viewDesc.componentMapping = RenderComponentMapping(
      ConvertSwizzle(fetch.swizzle & 7), ConvertSwizzle((fetch.swizzle >> 3) & 7),
      ConvertSwizzle((fetch.swizzle >> 6) & 7), ConvertSwizzle((fetch.swizzle >> 9) & 7));
  host->view = host->texture->createTextureView(viewDesc);
  host->descriptor = g_descriptors.Allocate();
  if (host->descriptor) {
    WriteTextureDescriptor(host->descriptor, host->texture.get(), host->view.get());
  }
  return host;
}

// Logs each kind of texture the renderer can't sample yet, once.
void ReportUnsupportedTexture(const char* reason, const xenos::xe_gpu_texture_fetch_t& fetch) {
  static std::unordered_set<uint32_t> reported;
  uint32_t kind = (uint32_t(fetch.format) << 8) | (uint32_t(fetch.dimension) << 4) |
                  (reason[0] & 0xF);
  if (reported.insert(kind).second) {
    REXLOG_WARN("video: texture skipped ({}): format {} dimension {}", reason,
                uint32_t(fetch.format), uint32_t(fetch.dimension));
  }
}

// Descriptor index for a texture fetch constant, 0 if it can't be sampled.
// The bloom (blurred bright parts, blended with the previous frame's) is
// added to the picture by two passes, both from fetch slot 1: tone mapping
// and the last full-screen composite. The bloom setting acts on what those
// two read; the pass that blends in the previous frame still reads it all.
constexpr uint64_t kToneMapPixelShader = 0x2CB124864F1A4460;
constexpr uint64_t kCompositePixelShader = 0xEAE805C57979C8A5;
constexpr uint32_t kBloomSlot = 1;
std::unique_ptr<HostTexture> g_reducedBloom;  // bloom 1: the bloom at half strength

// The bloom as the composite should read it (descriptor with the scale bits),
// for bloom settings below 2.
uint32_t AdjustBloom(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t descriptor) {
  int32_t setting = REXCVAR_GET(bloom);
  if (setting >= 2 || !descriptor) {
    return descriptor;
  }
  if (setting <= 0) {
    return DESCRIPTOR_NULL_2D;  // blank: nothing added
  }
  // The resolved texture behind the descriptor (GetTexture's lookup).
  uint32_t base = uint32_t(fetch.base_address) << 12;
  HostTexture* source = nullptr;
  if (!ResolveCurrent(base)) {
    // GetTexture read guest memory instead
  } else if (auto it = g_resolvedTextures.find(ResolvedKey(base, fetch));
             it != g_resolvedTextures.end()) {
    source = it->second.get();
  } else if (auto last = g_lastResolve.find(base); last != g_lastResolve.end()) {
    source = last->second;
  }
  if (!source || !source->descriptor) {
    return descriptor;
  }
  auto& dest = g_reducedBloom;
  if (dest && (dest->width != source->width || dest->height != source->height ||
               dest->format != source->format || dest->scale != source->scale)) {
    g_garbage[g_frame].push_back(std::move(dest));
  }
  if (!dest) {
    dest = std::make_unique<HostTexture>();
    dest->width = source->width;
    dest->height = source->height;
    dest->format = source->format;
    dest->scale = source->scale;
    dest->texture = g_device->createTexture(RenderTextureDesc::Texture2D(
        dest->width, dest->height, 1, dest->format, RenderTextureFlag::RENDER_TARGET));
    if (!dest->texture) {
      dest.reset();
      return descriptor;
    }
    SetDescriptor(*dest);
  }
  auto& pipeline = g_copyPipelines[(uint64_t(reinterpret_cast<uintptr_t>(g_scaleShader.get())) << 8) ^
                                   uint64_t(dest->format)];
  if (!pipeline) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_pipelineLayout.get();
    desc.vertexShader = g_fullscreenShader.get();
    desc.pixelShader = g_scaleShader.get();
    desc.renderTargetFormat[0] = dest->format;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    pipeline = g_device->createGraphicsPipeline(desc);
  }
  Transition(*source, RenderTextureLayout::SHADER_READ);
  BindFramebuffer(dest.get(), nullptr);
  auto* commandList = CommandList();
  RenderViewport viewport(0.0f, 0.0f, float(dest->width), float(dest->height));
  RenderRect scissor(0, 0, int32_t(dest->width), int32_t(dest->height));
  float factor = 0.5f;
  uint32_t constants[2] = {source->descriptor, 0};
  std::memcpy(&constants[1], &factor, sizeof(factor));
  commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
  commandList->setPipeline(pipeline.get());
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
  commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);
  commandList->setGraphicsPushConstants(0, constants, 0, sizeof(constants));
  commandList->setViewports(&viewport, 1);
  commandList->setScissors(&scissor, 1);
  commandList->drawInstanced(3, 1, 0, 0);
  Transition(*dest, RenderTextureLayout::SHADER_READ);
  return dest->descriptor | ((dest->scale - 1) << 20);
}

uint32_t LookUpTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  bool oneD = fetch.dimension == xenos::DataDimension::k1D;
  if (!oneD && fetch.dimension != xenos::DataDimension::k2DOrStacked) {
    ReportUnsupportedTexture("3D or cube", fetch);
    return 0;
  }
  uint32_t base = uint32_t(fetch.base_address) << 12;

  // Render-to-texture results live on the GPU.
  if (ResolveCurrent(base)) {
    // The render scale above bit 20: shaders take fetch offsets in guest
    // texels (XenosRecomp's getGuestTexture2DDimensions).
    if (auto it = g_resolvedTextures.find(ResolvedKey(base, fetch));
        it != g_resolvedTextures.end()) {
      NoteCarriedRead(it->second.get());
      Transition(*it->second, RenderTextureLayout::SHADER_READ);
      return it->second->descriptor | ((it->second->scale - 1) << 20);
    }
    if (auto it = g_lastResolve.find(base); it != g_lastResolve.end()) {
      NoteCarriedRead(it->second);
      Transition(*it->second, RenderTextureLayout::SHADER_READ);
      return it->second->descriptor | ((it->second->scale - 1) << 20);
    }
  }

  TextureFormatInfo info = GetTextureFormatInfo(fetch.format);
  if (info.format == RenderFormat::UNKNOWN) {
    ReportUnsupportedTexture("format", fetch);
    return 0;
  }
  uint32_t width = oneD ? fetch.size_1d.width + 1 : fetch.size_2d.width + 1;
  uint32_t height = oneD ? 1 : fetch.size_2d.height + 1;
  TextureLayout layout = GetTextureLayout(fetch, info, width, height);
  uint64_t guestSize = layout.levels[0].size;
  if (guestSize == 0 || base + guestSize > GUEST_MEMORY_SIZE) {
    ReportUnsupportedTexture("size", fetch);
    return 0;
  }
  if (layout.mipAddress + layout.mipsSize > GUEST_MEMORY_SIZE) {
    layout.levels.resize(1);  // mips past the end of memory: the base alone
    layout.mipsSize = 0;
  }

  // Shape and view: dwords 0-2, the swizzle, the mip levels of dword 4, and
  // the dimension, packed mips bit and mip address of dword 5; sampler fields
  // don't matter.
  uint32_t keyData[6] = {fetch.dword_0 & ~0x3FFFFFu, fetch.dword_1, fetch.dword_2,
                         uint32_t(fetch.swizzle), fetch.dword_4 & 0x3FC,
                         fetch.dword_5 & 0xFFFFFE00};
  uint64_t key = XXH3_64bits(keyData, sizeof(keyData));
  auto& cached = g_textures[key];
  // The headers are made again when ours comes or goes: always checked.
  bool header = info.format == RenderFormat::R8_UNORM &&
                (base == g_menuHeader.base || base == g_inGameHeaderBase);
  // The button atlas and its palette too, with the prompts shown in the hash.
  bool promptTexture = cached.host && cached.host->prompts;
  header |= promptTexture;
  bool stable = cached.host && !header && cached.checkedFrame != ~0ull &&
                g_frameCount - cached.changedFrame > kTextureStableFrames;
  bool due = stable ? g_frameCount - cached.checkedFrame >= kTextureRecheckFrames
                    : cached.checkedFrame != g_frameCount;
  g_profile.textureLookups++;
  if (due) {
    cached.checkedFrame = g_frameCount;
    g_profile.hashedBytes += guestSize + layout.mipsSize;
    const uint8_t* data = guest::Physical(base);
    const uint8_t* mipData = guest::Physical(layout.mipAddress);
    uint64_t hash = XXH3_64bits(data, guestSize) ^
                    (layout.mipsSize ? XXH3_64bits(mipData, layout.mipsSize) * 0x9E3779B97F4A7C15ull
                                     : 0);
    if (header) {
      hash += MenuHeaderGeneration();
    }
    if (promptTexture) {
      // (once a frame, so the atlas and its palette are made for the same set)
      static uint64_t generationFrame = ~0ull;
      static uint32_t generation = 0;
      if (generationFrame != g_frameCount) {
        generationFrame = g_frameCount;
        generation = prompts::Generation();
      }
      hash += uint64_t(generation) * 0x9E3779B97F4A7C15ull;
    }
    if (hash != cached.contentHash || (!cached.host && cached.contentHash == 0)) {
      cached.changedFrame = g_frameCount;
      if (cached.host) {
        g_garbage[g_frame].push_back(std::move(cached.host));
      }
      bool retry = false;  // made differently once another texture is seen
      MakeTimer made(g_frameStats.textures, g_frameStats.textureNs);
      g_frameStats.textureBytes += guestSize + layout.mipsSize;
      g_profile.texturesMade++;
      g_profile.textureMadeBytes += guestSize + layout.mipsSize;
      cached.host =
          CreateTexture(fetch, info, layout, data, guestSize, mipData, layout.mipsSize, &retry);
      // (a texture that couldn't be made keeps its hash too: not retried until
      // its content changes)
      cached.contentHash = retry && cached.host ? 0 : hash;
    }
  }
  if (!cached.host) {
    return 0;  // (couldn't be made: drawn without it)
  }
  Transition(*cached.host, RenderTextureLayout::SHADER_READ);
  return cached.host->descriptor;
}

// The frame's texture lookups by fetch constant: a frame looks up thousands
// (every slot of every draw) of a few hundred textures. Forgotten at each
// frame, and a resolve forgets the ones at its destination (what it holds and
// the resolved texture's layout change).
struct FetchKey {
  uint32_t dwords[6];
  bool operator==(const FetchKey& other) const {
    return std::memcmp(dwords, other.dwords, sizeof(dwords)) == 0;
  }
};
struct FetchKeyHash {
  size_t operator()(const FetchKey& key) const { return XXH3_64bits(key.dwords, sizeof(key.dwords)); }
};
std::unordered_map<FetchKey, uint32_t, FetchKeyHash> g_frameTextures;
// The memo's keys by base address, for a resolve's (each list is the frame's
// it's stamped with: older ones are empty).
struct FrameTextureKeys {
  uint64_t frame = ~0ull;
  std::vector<FetchKey> keys;
};
std::unordered_map<uint32_t, FrameTextureKeys> g_frameTexturesByBase;
uint64_t g_frameTexturesFrame = ~0ull;

void ForgetFrameTextures(uint32_t base) {
  auto it = g_frameTexturesByBase.find(base);
  if (it == g_frameTexturesByBase.end() || it->second.frame != g_frameCount) {
    return;
  }
  for (const FetchKey& key : it->second.keys) {
    g_frameTextures.erase(key);
  }
  it->second.keys.clear();
}

uint32_t GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  StageTimer timer(Stage::kTexture);
  if (g_frameTexturesFrame != g_frameCount) {
    g_frameTexturesFrame = g_frameCount;
    g_frameTextures.clear();
  }
  FetchKey key;
  std::memcpy(key.dwords, &fetch.dword_0, sizeof(key.dwords));
  auto [it, inserted] = g_frameTextures.try_emplace(key, 0u);
  if (inserted) {
    it->second = LookUpTexture(fetch);
    FrameTextureKeys& byBase = g_frameTexturesByBase[uint32_t(fetch.base_address) << 12];
    if (byBase.frame != g_frameCount) {
      byBase.frame = g_frameCount;
      byBase.keys.clear();
    }
    byBase.keys.push_back(key);
  }
  return it->second;
}

// Device state ----------------------------------------------------------------

// The device keeps GPU register shadows in blocks (docs/d3d-map-c.md).
uint32_t Register(uint32_t device, uint32_t index) {
  struct Block {
    uint32_t first, count, offset;
  };
  static constexpr Block blocks[] = {
      {0x2000, 16, 0x2880}, {0x2100, 21, 0x28CC}, {0x2180, 5, 0x2920},  {0x2200, 12, 0x2934},
      {0x2280, 21, 0x2964}, {0x2300, 38, 0x29B8}, {0x2380, 8, 0x2A50},
  };
  for (const auto& block : blocks) {
    if (index >= block.first && index < block.first + block.count) {
      return guest::Load32(device + block.offset + (index - block.first) * 4);
    }
  }
  return 0;
}

float RegisterFloat(uint32_t device, uint32_t index) {
  uint32_t bits = Register(device, index);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Whether a target's blend reads the blend constants (RB_BLEND_RED..ALPHA):
// the game fades a frozen picture out over a cut with them (#58).
bool UsesBlendConstants(uint32_t blendControl) {
  reg::RB_BLENDCONTROL blend;
  blend.value = blendControl;
  auto constant = [](xenos::BlendFactor f) {
    return f == xenos::BlendFactor::kConstantColor || f == xenos::BlendFactor::kOneMinusConstantColor ||
           f == xenos::BlendFactor::kConstantAlpha || f == xenos::BlendFactor::kOneMinusConstantAlpha;
  };
  return constant(blend.color_srcblend) || constant(blend.color_destblend) ||
         constant(blend.alpha_srcblend) || constant(blend.alpha_destblend);
}

RenderBlend ConvertBlend(xenos::BlendFactor factor) {
  switch (factor) {
    case xenos::BlendFactor::kZero: return RenderBlend::ZERO;
    case xenos::BlendFactor::kOne: return RenderBlend::ONE;
    case xenos::BlendFactor::kSrcColor: return RenderBlend::SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor: return RenderBlend::INV_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha: return RenderBlend::SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha: return RenderBlend::INV_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor: return RenderBlend::DEST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor: return RenderBlend::INV_DEST_COLOR;
    case xenos::BlendFactor::kDstAlpha: return RenderBlend::DEST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha: return RenderBlend::INV_DEST_ALPHA;
    case xenos::BlendFactor::kConstantColor: return RenderBlend::BLEND_FACTOR;
    case xenos::BlendFactor::kOneMinusConstantColor: return RenderBlend::INV_BLEND_FACTOR;
    case xenos::BlendFactor::kConstantAlpha: return RenderBlend::BLEND_FACTOR_ALPHA;
    case xenos::BlendFactor::kOneMinusConstantAlpha: return RenderBlend::INV_BLEND_FACTOR_ALPHA;
    case xenos::BlendFactor::kSrcAlphaSaturate: return RenderBlend::SRC_ALPHA_SAT;
  }
  return RenderBlend::ONE;
}

RenderBlendOperation ConvertBlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kAdd: return RenderBlendOperation::ADD;
    case xenos::BlendOp::kSubtract: return RenderBlendOperation::SUBTRACT;
    case xenos::BlendOp::kMin: return RenderBlendOperation::MIN;
    case xenos::BlendOp::kMax: return RenderBlendOperation::MAX;
    case xenos::BlendOp::kRevSubtract: return RenderBlendOperation::REV_SUBTRACT;
  }
  return RenderBlendOperation::ADD;
}

RenderComparisonFunction ConvertCompare(xenos::CompareFunction function) {
  switch (function) {
    case xenos::CompareFunction::kNever: return RenderComparisonFunction::NEVER;
    case xenos::CompareFunction::kLess: return RenderComparisonFunction::LESS;
    case xenos::CompareFunction::kEqual: return RenderComparisonFunction::EQUAL;
    case xenos::CompareFunction::kLessEqual: return RenderComparisonFunction::LESS_EQUAL;
    case xenos::CompareFunction::kGreater: return RenderComparisonFunction::GREATER;
    case xenos::CompareFunction::kNotEqual: return RenderComparisonFunction::NOT_EQUAL;
    case xenos::CompareFunction::kGreaterEqual: return RenderComparisonFunction::GREATER_EQUAL;
    case xenos::CompareFunction::kAlways: return RenderComparisonFunction::ALWAYS;
  }
  return RenderComparisonFunction::ALWAYS;
}

RenderStencilOp ConvertStencilOp(xenos::StencilOp op) {
  switch (op) {
    case xenos::StencilOp::kKeep: return RenderStencilOp::KEEP;
    case xenos::StencilOp::kZero: return RenderStencilOp::ZERO;
    case xenos::StencilOp::kReplace: return RenderStencilOp::REPLACE;
    case xenos::StencilOp::kIncrementClamp: return RenderStencilOp::INCREMENT_AND_CLAMP;
    case xenos::StencilOp::kDecrementClamp: return RenderStencilOp::DECREMENT_AND_CLAMP;
    case xenos::StencilOp::kInvert: return RenderStencilOp::INVERT;
    case xenos::StencilOp::kIncrementWrap: return RenderStencilOp::INCREMENT_AND_WRAP;
    case xenos::StencilOp::kDecrementWrap: return RenderStencilOp::DECREMENT_AND_WRAP;
  }
  return RenderStencilOp::KEEP;
}

// D3DDECLTYPE on the 360: bits 0-5 GPU vertex format, 8 signed, 9 integer.
uint32_t ElementFlags(uint32_t declType) {
  uint32_t format = declType & 0x3F;
  uint32_t flags = format | (declType & 0x300);
  if (declType == 0x182886) {  // D3DDECLTYPE_D3DCOLOR: BGRA in memory
    flags |= 0x800;
  }
  return flags;
}

// D3D's scissor state in the device (docs/d3d-map-c.md, SetScissorRect):
// D3DRS_SCISSORTESTENABLE, and the rect (left, top, right, bottom).
constexpr uint32_t kScissorEnable = 11848, kScissorRect = 12668;

// Shared constants, laid out as docs/shaders.md describes.
struct SharedConstants {
  uint32_t textureIndices[4][32];
  uint32_t samplerIndices[32];
  uint32_t booleans[8];
  uint32_t swappedTexcoords;
  float alphaThreshold;
  float halfPixelOffset[2];
  uint64_t guestMemoryBase;
  uint32_t vertexFetch[96][2];
  uint32_t vertexElements[16][4];
  float ndcTransform[4];
  uint32_t alphaFunc;  // RB_COLORCONTROL alpha_func
  float resolutionScale;  // host pixels per guest pixel (g_resolutionScale)
  float debugColor[4];  // NG2_DEBUG_HIGHLIGHT
};
static_assert(offsetof(SharedConstants, samplerIndices) == 512);
static_assert(offsetof(SharedConstants, booleans) == 640);
static_assert(offsetof(SharedConstants, guestMemoryBase) == 688);
static_assert(offsetof(SharedConstants, vertexFetch) == 696);
static_assert(offsetof(SharedConstants, vertexElements) == 1464);
static_assert(offsetof(SharedConstants, ndcTransform) == 1720);
static_assert(offsetof(SharedConstants, alphaFunc) == 1736);
static_assert(offsetof(SharedConstants, resolutionScale) == 1740);
static_assert(offsetof(SharedConstants, debugColor) == 1744);

// Pipeline state that varies per draw (hashed into the pipeline key).
// The depth bias of a draw (PA_SU_POLY_OFFSET_*) in Vulkan's terms, picked
// as Xenia does (draw_util::GetPreferredFacePolygonOffset): one bias for the
// whole draw, the front face's unless that face is culled. The guest offset
// is in depth units, about 2^-24 each on the host's float buffer near 1; the
// scale is in 1/16 subpixels.
void PolygonOffset(uint32_t device, reg::PA_SU_SC_MODE_CNTL mode, uint32_t primitive,
                   float& constant, float& slope) {
  float scale = 0.0f, offset = 0.0f;
  auto type = xenos::PrimitiveType(primitive);
  bool polygonal = type != xenos::PrimitiveType::kPointList &&
                   type != xenos::PrimitiveType::kLineList &&
                   type != xenos::PrimitiveType::kLineStrip &&
                   type != xenos::PrimitiveType::kLineLoop &&
                   type != xenos::PrimitiveType::kRectangleList;
  if (polygonal) {
    if (mode.poly_offset_front_enable && !mode.cull_front) {
      scale = RegisterFloat(device, 0x2380);
      offset = RegisterFloat(device, 0x2381);
    }
    if (mode.poly_offset_back_enable && !mode.cull_back && scale == 0.0f && offset == 0.0f) {
      scale = RegisterFloat(device, 0x2382);
      offset = RegisterFloat(device, 0x2383);
    }
  } else if (mode.poly_offset_para_enable) {
    scale = RegisterFloat(device, 0x2380);
    offset = RegisterFloat(device, 0x2381);
  }
  // The pipeline takes whole units: rounded away from zero, as Xenia's
  // GetD3D10IntegerPolygonOffset (more bias is better than none).
  constant = std::copysign(std::ceil(std::abs(offset) * float(1u << 24)), offset);
  // Per host pixel the depth slope is smaller by the render scale (Xenia
  // multiplies by its resolution scale too).
  slope = scale * xenos::kPolygonOffsetScaleSubpixelUnit * float(g_resolutionScale);
}

struct PipelineKey {
  uint64_t vertexShader;  // shader cache entry hashes
  uint64_t pixelShader;
  uint32_t primitive;
  uint32_t colorFormats[4];  // RenderFormat::UNKNOWN past the bound targets
  uint32_t depthFormat;
  uint32_t samples;
  uint32_t blendControls[4];
  uint32_t colorMask;  // RB_COLOR_MASK, 4 bits a target, minus unwritten outputs
  uint32_t depthControl;
  uint32_t stencilMasks;
  uint32_t cull;
  uint32_t alphaTest;
  uint32_t alphaToCoverage;
  float depthBias;  // host units (PolygonOffset)
  float slopeDepthBias;
  uint32_t padding = 0;  // hashed and listed: no unset bytes
};
static_assert(sizeof(PipelineKey) == 96);

// NG2_PIPELINE_STATS=1: the driver's statistics of each shader pair's first
// pipeline (VK_KHR_pipeline_executable_properties: registers, scratch,
// instructions), to see what a slow draw's shaders compiled to on a driver
// that has no other way to show it (#2, AMD's Windows driver).
void LogPipelineStatistics(const PipelineKey& key, const RenderPipeline& pipeline) {
  static std::mutex mutex;
  static std::unordered_set<uint64_t> logged;
  uint64_t pair = key.vertexShader * 31 + key.pixelShader;
  {
    std::lock_guard lock(mutex);
    if (!logged.insert(pair).second) {
      return;
    }
  }
  std::string text = pipeline.getStatistics();
  if (text.empty()) {
    REXLOG_INFO("video: pipeline vs {:016X} ps {:016X}: no statistics from the driver",
                key.vertexShader, key.pixelShader);
    return;
  }
  for (size_t begin = 0, end; begin < text.size(); begin = end + 1) {
    end = text.find('\n', begin);
    if (end == std::string::npos) {
      end = text.size();
    }
    if (end > begin) {
      REXLOG_INFO("video: pipeline vs {:016X} ps {:016X} {}", key.vertexShader, key.pixelShader,
                  std::string_view(text).substr(begin, end - begin));
    }
  }
}

std::unique_ptr<RenderPipeline> MakePipeline(const PipelineKey& key, RenderShader* vertexShader,
                                             RenderShader* pixelShader) {
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_pipelineLayout.get();
  desc.vertexShader = vertexShader;
  desc.pixelShader = pixelShader;
  if (key.primitive == uint32_t(xenos::PrimitiveType::kRectangleList)) {
    desc.geometryShader = g_rectListShader.get();
    desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
  } else {
    switch (xenos::PrimitiveType(key.primitive)) {
      case xenos::PrimitiveType::kPointList:
        desc.primitiveTopology = RenderPrimitiveTopology::POINT_LIST;
        break;
      case xenos::PrimitiveType::kLineList:
        desc.primitiveTopology = RenderPrimitiveTopology::LINE_LIST;
        break;
      case xenos::PrimitiveType::kLineStrip:
        desc.primitiveTopology = RenderPrimitiveTopology::LINE_STRIP;
        break;
      case xenos::PrimitiveType::kTriangleFan:
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_FAN;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_STRIP;
        break;
      default:
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        break;
    }
  }
  reg::PA_SU_SC_MODE_CNTL mode;
  mode.value = key.cull;
  // Front faces are counter-clockwise unless PA_SU_SC_MODE_CNTL.face (as
  // Xenia sets VkPipelineRasterizationStateCreateInfo.frontFace); plume's
  // pipelines take clockwise as front, so a culled side is the other one.
  if (mode.cull_front && mode.cull_back) {
    desc.cullMode = RenderCullMode::NONE;  // everything culled: drawn as nothing below
  } else if (mode.cull_front) {
    desc.cullMode = mode.face ? RenderCullMode::FRONT : RenderCullMode::BACK;
  } else if (mode.cull_back) {
    desc.cullMode = mode.face ? RenderCullMode::BACK : RenderCullMode::FRONT;
  }
  for (uint32_t i = 0; i < 4 && key.colorFormats[i] != uint32_t(RenderFormat::UNKNOWN); i++) {
    desc.renderTargetFormat[i] = RenderFormat(key.colorFormats[i]);
    desc.renderTargetCount = i + 1;
    reg::RB_BLENDCONTROL blend;
    blend.value = key.blendControls[i];
    auto& target = desc.renderTargetBlend[i];
    target.srcBlend = ConvertBlend(blend.color_srcblend);
    target.dstBlend = ConvertBlend(blend.color_destblend);
    target.blendOp = ConvertBlendOp(blend.color_comb_fcn);
    target.srcBlendAlpha = ConvertBlend(blend.alpha_srcblend);
    target.dstBlendAlpha = ConvertBlend(blend.alpha_destblend);
    target.blendOpAlpha = ConvertBlendOp(blend.alpha_comb_fcn);
    target.blendEnabled =
        !(target.srcBlend == RenderBlend::ONE && target.dstBlend == RenderBlend::ZERO &&
          target.blendOp == RenderBlendOperation::ADD &&
          target.srcBlendAlpha == RenderBlend::ONE && target.dstBlendAlpha == RenderBlend::ZERO &&
          target.blendOpAlpha == RenderBlendOperation::ADD);
    if (target.blendEnabled && UsesBlendConstants(key.blendControls[i])) {
      desc.dynamicBlendConstantsEnabled = true;  // RB_BLEND_RED..ALPHA, set with each draw
    }
    target.renderTargetWriteMask = uint8_t((key.colorMask >> (i * 4)) & 0xF);
  }
  desc.depthTargetFormat = RenderFormat(key.depthFormat);
  if (desc.depthTargetFormat != RenderFormat::UNKNOWN) {
    reg::RB_DEPTHCONTROL depth;
    depth.value = key.depthControl;
    desc.depthEnabled = depth.z_enable;
    desc.depthWriteEnabled = depth.z_enable && depth.z_write_enable;
    desc.depthFunction = ConvertCompare(depth.zfunc);
    desc.depthBias = int32_t(key.depthBias);
    desc.slopeScaledDepthBias = key.slopeDepthBias;
    desc.stencilEnabled = depth.stencil_enable;
    desc.stencilFrontFace.compareFunction = ConvertCompare(depth.stencilfunc);
    desc.stencilFrontFace.failOp = ConvertStencilOp(depth.stencilfail);
    desc.stencilFrontFace.passOp = ConvertStencilOp(depth.stencilzpass);
    desc.stencilFrontFace.depthFailOp = ConvertStencilOp(depth.stencilzfail);
    if (depth.backface_enable) {
      desc.stencilBackFace.compareFunction = ConvertCompare(depth.stencilfunc_bf);
      desc.stencilBackFace.failOp = ConvertStencilOp(depth.stencilfail_bf);
      desc.stencilBackFace.passOp = ConvertStencilOp(depth.stencilzpass_bf);
      desc.stencilBackFace.depthFailOp = ConvertStencilOp(depth.stencilzfail_bf);
    } else {
      desc.stencilBackFace = desc.stencilFrontFace;
    }
    reg::RB_STENCILREFMASK stencil;
    stencil.value = key.stencilMasks;
    desc.stencilReadMask = stencil.stencilmask;
    desc.stencilWriteMask = stencil.stencilwritemask;
    desc.stencilReference = stencil.stencilref;
  }
  desc.multisampling.sampleCount = RenderSampleCounts(key.samples);
  desc.alphaToCoverageEnabled = key.alphaToCoverage != 0;
  RenderSpecConstant specConstant(0, key.alphaTest ? 0x2u : 0u);  // SPEC_CONSTANT_ALPHA_TEST
  desc.specConstants = &specConstant;
  desc.specConstantsCount = 1;
  static const bool statistics = std::getenv("NG2_PIPELINE_STATS") != nullptr;
  desc.captureStatistics = statistics;
  auto pipeline = g_device->createGraphicsPipeline(desc);
  if (statistics && pipeline) {
    LogPipelineStatistics(key, *pipeline);
  }
  return pipeline;
}

// Pipelines are made on worker threads (async_pipelines): a draw whose
// pipeline isn't made yet is left out until it is, rather than the game
// waiting tens of milliseconds for the driver. Every pipeline made is kept
// in a list next to the settings (ng2-pipelines.bin), and those are made in
// the background when the game starts, before they're needed again.
struct PipelineJob {
  uint64_t hash;
  PipelineKey key;
  RenderShader* vertexShader;
  RenderShader* pixelShader;
};
struct PipelineWorkers {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<PipelineJob> jobs;
  std::vector<std::pair<uint64_t, std::unique_ptr<RenderPipeline>>> made;
  std::vector<std::thread> threads;
  bool stop = false;
} g_pipelineWorkers;
std::unordered_set<uint64_t> g_pendingPipelines;  // asked of the workers, not taken yet
std::unordered_set<uint64_t> g_earlyQueued;  // asked early, still in the queue (under its mutex)
std::filesystem::path g_pipelineListPath;
std::unordered_set<uint64_t> g_listedPipelines;   // in the list file
constexpr uint32_t kPipelineListMagic = 0x4E473250;  // "NG2P"
constexpr uint32_t kPipelineListVersion = 1;

void PipelineWorker() {
  auto& w = g_pipelineWorkers;
  for (;;) {
    PipelineJob job;
    {
      std::unique_lock lock(w.mutex);
      w.wake.wait(lock, [&] { return w.stop || !w.jobs.empty(); });
      if (w.stop) {
        return;
      }
      job = w.jobs.front();
      w.jobs.pop_front();
      g_earlyQueued.erase(job.hash);
    }
    auto pipeline = MakePipeline(job.key, job.vertexShader, job.pixelShader);
    std::lock_guard lock(w.mutex);
    w.made.emplace_back(job.hash, std::move(pipeline));
  }
}

void StartPipelineWorkers() {
  uint32_t count = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
  for (uint32_t i = 0; i < count; i++) {
    g_pipelineWorkers.threads.emplace_back(PipelineWorker);
  }
}

void StopPipelineWorkers() {
  auto& w = g_pipelineWorkers;
  {
    std::lock_guard lock(w.mutex);
    w.stop = true;
    w.jobs.clear();
    g_earlyQueued.clear();
  }
  w.wake.notify_all();
  for (auto& thread : w.threads) {
    thread.join();
  }
  w.threads.clear();
  w.made.clear();
  w.stop = false;
  g_pendingPipelines.clear();
}

// A draw's pipeline goes first; the listed ones (early) wait their turn.
void AskForPipeline(uint64_t hash, const PipelineKey& key, RenderShader* vertexShader,
                    RenderShader* pixelShader, bool early = false) {
  if (!g_pendingPipelines.insert(hash).second) {
    if (!early) {
      // Asked for early (the list, the seed) and still waiting: a draw needs it now.
      std::lock_guard lock(g_pipelineWorkers.mutex);
      if (!g_earlyQueued.erase(hash)) {
        return;  // already made, being made, or already moved up
      }
      auto& jobs = g_pipelineWorkers.jobs;
      auto it = std::find_if(jobs.begin(), jobs.end(),
                             [&](const PipelineJob& job) { return job.hash == hash; });
      if (it != jobs.end() && it != jobs.begin()) {
        PipelineJob job = *it;
        jobs.erase(it);
        jobs.push_front(job);
      }
    }
    return;
  }
  {
    std::lock_guard lock(g_pipelineWorkers.mutex);
    PipelineJob job{hash, key, vertexShader, pixelShader};
    if (early) {
      g_earlyQueued.insert(hash);
      g_pipelineWorkers.jobs.push_back(job);
    } else {
      g_pipelineWorkers.jobs.push_front(job);
    }
  }
  g_pipelineWorkers.wake.notify_one();
}

void TakeMadePipelines() {
  std::vector<std::pair<uint64_t, std::unique_ptr<RenderPipeline>>> made;
  {
    std::lock_guard lock(g_pipelineWorkers.mutex);
    made.swap(g_pipelineWorkers.made);
  }
  for (auto& [hash, pipeline] : made) {
    g_pendingPipelines.erase(hash);
    // Kept only if none was made meanwhile (async_pipelines turned off):
    // that one may already be in a recorded command list.
    if (pipeline) {
      g_pipelines.try_emplace(hash, std::move(pipeline));
    }
  }
}

// fopen by a path, which on Windows may hold any character (path::string()
// there is the ANSI code page, and throws for a name it can't hold).
FILE* OpenFile(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
  std::wstring wide(mode, mode + std::strlen(mode));
  return _wfopen(path.c_str(), wide.c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

// A new pipeline into the list file (not the debug shader's).
void ListPipeline(uint64_t hash, const PipelineKey& key) {
  if (g_pipelineListPath.empty() || key.pixelShader == 0 ||
      !g_listedPipelines.insert(hash).second) {
    return;
  }
  std::error_code ec;  // (none of these throw: a list it can't use is just not used)
  bool fresh = !std::filesystem::exists(g_pipelineListPath, ec);
  if (FILE* f = OpenFile(g_pipelineListPath, "ab")) {
    if (fresh) {
      uint32_t header[3] = {kPipelineListMagic, kPipelineListVersion, uint32_t(sizeof(key))};
      std::fwrite(header, sizeof(header), 1, f);
    }
    std::fwrite(&key, sizeof(key), 1, f);
    std::fclose(f);
  }
}

RenderShader* HostShader(const ShaderCacheEntry* entry);

// The pipelines in a list file, asked of the workers. The player's list is
// repaired (a record cut short when the game stopped while writing it) or,
// from another version, dropped. The seed (packaging: pipelines recorded
// playing the game at the default 2x antialiasing) is only read, its
// multisampled pipelines moved to the antialiasing in use.
uint32_t AskForListedPipelines(const std::filesystem::path& path, bool seed) {
  FILE* f = OpenFile(path, "rb");
  if (!f) {
    return 0;
  }
  uint32_t header[3] = {};
  if (std::fread(header, sizeof(header), 1, f) != 1 || header[0] != kPipelineListMagic ||
      header[1] != kPipelineListVersion || header[2] != sizeof(PipelineKey)) {
    std::fclose(f);
    REXLOG_WARN("video: {} is from another version; {}", rex::path_to_utf8(path),
                seed ? "not used" : "starting a new one");
    if (!seed) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
    return 0;
  }
  if (!seed) {
    // A record cut short is cut off, so later ones line up.
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size < sizeof(header)) {
      std::fclose(f);
      return 0;
    }
    uint64_t whole =
        sizeof(header) + (size - sizeof(header)) / sizeof(PipelineKey) * sizeof(PipelineKey);
    if (whole != size) {
      std::fclose(f);
      std::filesystem::resize_file(path, whole, ec);
      if (ec) {
        REXLOG_WARN("video: can't repair {}: not used", rex::path_to_utf8(path));
        return 0;
      }
      f = OpenFile(path, "rb");
      if (!f || std::fseek(f, sizeof(header), SEEK_SET) != 0) {
        if (f) {
          std::fclose(f);
        }
        return 0;
      }
    }
  }
  uint32_t antialiasing = AntialiasingSetting();
  PipelineKey key;
  uint32_t asked = 0;
  while (std::fread(&key, sizeof(key), 1, f) == 1) {
    if (seed) {
      // Recorded at 2x antialiasing and render scale 1: a game 2x target had 2 samples, a 4x
      // one 4 (SampleCount), and the slope bias grows with the scale (PolygonOffset).
      if (key.samples > 1) {
        key.samples = antialiasing < 2 ? 1 : std::max(antialiasing, key.samples);
      }
      key.slopeDepthBias *= float(g_resolutionScale);
    }
    uint64_t hash = XXH3_64bits(&key, sizeof(key));
    if (!g_listedPipelines.insert(hash).second) {
      continue;
    }
    const ShaderCacheEntry* vs = FindShaderCacheEntry(key.vertexShader);
    const ShaderCacheEntry* ps = key.pixelShader == 1 ? nullptr : FindShaderCacheEntry(key.pixelShader);
    RenderShader* vertexShader = vs ? HostShader(vs) : nullptr;
    RenderShader* pixelShader = ps ? HostShader(ps) : nullptr;
    if (!vertexShader || (key.pixelShader != 1 && !pixelShader)) {
      continue;
    }
    AskForPipeline(hash, key, vertexShader, pixelShader, true);
    asked++;
  }
  std::fclose(f);
  return asked;
}

// The packaged seed: next to the executable, or in ../share/ng2 (the AppImage).
std::filesystem::path PipelineSeedPath() {
  std::filesystem::path dir = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  for (const auto& path : {dir / "ng2-pipelines-seed.bin",
                           dir.parent_path() / "share" / "ng2" / "ng2-pipelines-seed.bin"}) {
    if (std::filesystem::is_regular_file(path, ec)) {
      return path;
    }
  }
  return {};
}

// The listed pipelines (the player's, then the seed's), asked of the workers
// at start. The seed's aren't copied into the player's list: it's read at
// every start, so the player's list holds only what the seed lacks.
void MakeListedPipelines() {
  uint32_t own = 0, seeded = 0;
  if (!g_pipelineListPath.empty()) {
    own = AskForListedPipelines(g_pipelineListPath, false);
  }
  if (std::filesystem::path seed = PipelineSeedPath(); !seed.empty()) {
    seeded = AskForListedPipelines(seed, true);
  }
  REXLOG_INFO("video: making {} listed pipelines and {} from the seed in the background", own,
              seeded);
}

RenderPipeline* GetPipeline(const PipelineKey& key, RenderShader* vertexShader,
                            RenderShader* pixelShader) {
  StageTimer timer(Stage::kPipeline);
  uint64_t hash = XXH3_64bits(&key, sizeof(key));
  if (auto it = g_pipelines.find(hash); it != g_pipelines.end()) {
    return it->second.get();
  }
  TakeMadePipelines();
  if (auto it = g_pipelines.find(hash); it != g_pipelines.end()) {
    return it->second.get();
  }
  ListPipeline(hash, key);
  if (REXCVAR_GET(async_pipelines) && !g_pipelineWorkers.threads.empty()) {
    AskForPipeline(hash, key, vertexShader, pixelShader);
    return nullptr;  // the draw is left out until it's made
  }
  MakeTimer made(g_frameStats.pipelines, g_frameStats.pipelineNs);
  auto& pipeline = g_pipelines[hash];
  pipeline = MakePipeline(key, vertexShader, pixelShader);
  return pipeline.get();
}

// A draw with everything captured, ready to record.
struct DrawRecord {
  RenderPipeline* pipeline = nullptr;
  bool memExport = false;
  HostTexture* targets[4] = {};
  uint32_t targetCount = 0;
  HostTexture* depth = nullptr;
  RenderViewport viewport;
  RenderRect scissor;
  uint64_t pushConstants[3] = {};
  bool blendConstants = false;  // the pipeline takes them per draw
  float blendColor[4] = {};     // RB_BLEND_RED, GREEN, BLUE, ALPHA
  bool indexed = false;
  std::optional<RenderIndexBufferView> indexView;
  uint32_t drawCount = 0;
  uint32_t startVertex = 0;
  uint64_t vertexShader = 0, pixelShader = 0;  // hashes, for NG2_PROFILE=2
  int32_t interpIndex = -1;  // in g_interp.current (NG2_INTERP)
};

// BeginVertices / BeginIndexedVertices hand the game a pointer into the
// command ring and the game writes the data after the call: the draw is
// captured at Begin and recorded at the next hooked call.
struct InlineData {
  uint32_t vertexAddress = 0;  // guest virtual
  uint32_t vertexBytes = 0;
  uint32_t stride = 0;         // bytes
  uint32_t indexAddress = 0;   // guest virtual, 0 if not indexed
  uint32_t indexCount = 0;
  bool wideIndices = false;
  bool quadList = false;        // indexed quads (made triangles)
};
struct PendingDraw {
  DrawRecord record;
  InlineData data;
  float widen = 1.0f;  // the Options header's: x by this much from its left edge
  bool movie = false;  // a movie the game draws itself (a scroll's): 2D at other aspects
};
std::optional<PendingDraw> g_pendingDraw;


std::atomic<int64_t> g_modeRefreshNs{0};  // the display mode's refresh (SDL), 0 not known
// The swapchain's images: 8 with interpolation, else 3.
uint32_t SwapChainImages() {
  if (!InterpEnabled()) {
    return 3;
  }
  return 8u;
}

// What interpolation keeps on the GPU from frame to frame: the carried
// render targets' copies, the pictures' upload memory, their lists (with the
// GPU, the render thread and the presenter idle: switched off, or the device
// going).
void ReleaseInterpResources() {
  g_carriedCopies.clear();
  g_carriedRetired.clear();
  for (auto& allocator : g_interpUploads) {
    allocator.chunks.clear();
    allocator.Reset();
  }
  for (auto& lists : g_interpLists) {
    for (auto& list : lists) {
      list.reset();
    }
  }
  for (auto& semaphores : g_interpAcquire) {
    for (auto& semaphore : semaphores) {
      semaphore.reset();
    }
  }
  for (auto& fences : g_interpFences) {
    for (auto& fence : fences) {
      fence.fence.reset();
      fence.state = 0;
    }
  }
}

// Frame Interpolation switched in the menu: between frames, with the GPU and
// the build idle; the swapchain made again with its images (8 with it, 3
// without: FIFO's queue of 7 would be the latency of a frame that outruns
// the display).
void SwitchInterp() {
  bool wanted = InterpWanted();
  if (wanted == InterpEnabled()) {
    return;
  }
  WaitInterpBuild();
  WaitForGPU();
  ResetInterp(wanted);
  if (!wanted) {
    ReleaseInterpResources();  // (memory given back: nothing of it is used off)
  }
  UpdateExclusiveMode();  // (with interpolation only: made again below)
  if (g_frame >= FrameSlots()) {
    g_frame = 0;
  }
  g_swapChain->setTextureCount(SwapChainImages());
  // (with it, an acquire lets the presenter thread's presents through)
  g_swapChain->setAcquireInShortWaits(wanted);
  g_swapChainFramebuffers.clear();
  g_swapChainValid = g_swapChain->resize();
  SetInterpPictureCap();
  REXLOG_INFO("video: frame interpolation {} ({} swapchain images)", wanted ? "on" : "off",
              g_swapChain->getTextureCount());
}

void SubmitDraw(DrawRecord& record) {
  BindFramebuffer(record.targets, record.targetCount, record.depth);
  auto* commandList = CommandList();
  commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
  commandList->setPipeline(record.pipeline);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
  commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);
  NoteInterpDraw(record.interpIndex, record.pushConstants);
  g_drawTag = record.interpIndex;
  commandList->setGraphicsPushConstants(0, record.pushConstants, 0, sizeof(record.pushConstants));
  g_drawTag = -1;
  // Plume gives a pipeline one viewport per color target.
  RenderViewport viewports[4];
  RenderRect scissors[4];
  uint32_t viewportCount = std::max(record.targetCount, 1u);
  std::fill_n(viewports, viewportCount, record.viewport);
  std::fill_n(scissors, viewportCount, record.scissor);
  commandList->setViewports(viewports, viewportCount);
  commandList->setScissors(scissors, viewportCount);
  if (record.blendConstants) {
    commandList->setBlendConstants(record.blendColor);
  }
  if (g_profile.perDraw) {
    if (uint32_t mark = AllocGpuMark(GpuMark::kDraw); mark != UINT32_MAX) {
      g_gpuTiming[g_frame].drawShaders.emplace_back(record.vertexShader, record.pixelShader);
      WriteGpuMark(commandList, g_frame, mark);
    }
  }
  g_drawMemExport = record.memExport;
  if (record.indexed) {
    commandList->setIndexBuffer(&*record.indexView);
    commandList->drawIndexedInstanced(record.drawCount, 1, 0, int32_t(record.startVertex), 0);
  } else {
    commandList->drawInstanced(record.drawCount, 1, record.startVertex, 0);
  }
  g_drawMemExport = false;
  if (g_profile.perDraw) {
    // (what comes before the next draw, clears and copies, isn't the draw's)
    if (uint32_t mark = AllocGpuMark(GpuMark::kDrawEnd); mark != UINT32_MAX) {
      WriteGpuMark(commandList, g_frame, mark);
    }
  }
  // Memory export results feed later draws.
  if (record.memExport) {
    commandList->barriers(GuestMemoryStages(RenderBarrierStage::GRAPHICS),
                          RenderBufferBarrier(g_guestMemory.get(),
                                              RenderBufferAccess::READ | RenderBufferAccess::WRITE));
  }
}

template <typename T>
void SwapIndices(uint8_t* out, const uint8_t* in, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    reinterpret_cast<T*>(out)[i] = rex::byte_swap(reinterpret_cast<const T*>(in)[i]);
  }
}

// An indexed quad list's indices (big-endian, count of them), as a triangle
// list's: two triangles a quad, as the non-indexed quads below. Returns the
// index count drawn (a partial quad at the end is dropped).
template <typename T>
uint32_t SwapQuadIndices(uint8_t* out, const uint8_t* in, uint32_t count) {
  uint32_t quads = count / 4;
  auto* source = reinterpret_cast<const T*>(in);
  auto* dest = reinterpret_cast<T*>(out);
  for (uint32_t q = 0; q < quads; q++) {
    T v[4];
    for (uint32_t i = 0; i < 4; i++) {
      v[i] = rex::byte_swap(source[q * 4 + i]);
    }
    T* o = dest + q * 6;
    o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
    o[3] = v[0]; o[4] = v[2]; o[5] = v[3];
  }
  return quads * 6;
}

// Indices as the host draws them: swapped, and quads made triangles. Returns
// the index count.
uint32_t ConvertIndices(uint8_t* out, const uint8_t* in, uint32_t count, bool wide, bool quads) {
  if (quads) {
    return wide ? SwapQuadIndices<uint32_t>(out, in, count)
                : SwapQuadIndices<uint16_t>(out, in, count);
  }
  if (wide) {
    SwapIndices<uint32_t>(out, in, count);
  } else {
    SwapIndices<uint16_t>(out, in, count);
  }
  return count;
}

// Room for ConvertIndices' output.
uint64_t ConvertedIndexBytes(uint32_t count, bool wide, bool quads) {
  return uint64_t(quads ? count / 4 * 6 : count) * (wide ? 4 : 2);
}

void FlushPendingDraw() {
  if (!g_pendingDraw) {
    return;
  }
  PendingDraw pending = std::move(*g_pendingDraw);
  g_pendingDraw.reset();
  auto& data = pending.data;
  // The header is a quad of inline vertices (position first), made each
  // frame: wider by as much as the picture, from its left edge.
  if (pending.widen != 1.0f && data.stride >= 12) {
    uint32_t count = data.vertexBytes / data.stride;
    float left = std::numeric_limits<float>::max();
    for (uint32_t i = 0; i < count; i++) {
      left = std::min(left, guest::LoadFloat(data.vertexAddress + i * data.stride));
    }
    for (uint32_t i = 0; i < count; i++) {
      uint32_t address = data.vertexAddress + i * data.stride;
      float x = left + (guest::LoadFloat(address) - left) * pending.widen;
      uint32_t bits;
      std::memcpy(&bits, &x, sizeof(bits));
      *reinterpret_cast<uint32_t*>(guest::Virtual(address)) = rex::byte_swap(bits);
    }
  }
  // A movie in the 2D (a scroll's) is placed in the game's 1280x720 space
  // but not by the 2D's hooks (aspect.cpp): squeezed about the centre as the
  // 2D around it is, for the presenter to stretch back. A full-screen one
  // stays as it is.
  float sx = aspect::SqueezeX(), sy = aspect::SqueezeY();
  if (pending.movie && (sx != 1.0f || sy != 1.0f) && data.stride >= 8) {
    uint32_t count = data.vertexBytes / data.stride;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < count; i++) {
      float x = guest::LoadFloat(data.vertexAddress + i * data.stride);
      float y = guest::LoadFloat(data.vertexAddress + i * data.stride + 4);
      x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    // (pixels, not a clip-space quad, which aspect.cpp's movie hook places)
    bool pixels = x1 - x0 > 4.0f && y1 - y0 > 4.0f;
    if (pixels && !(x0 <= 1.0f && x1 >= 1279.0f && y0 <= 1.0f && y1 >= 719.0f)) {
      for (uint32_t i = 0; i < count; i++) {
        uint32_t address = data.vertexAddress + i * data.stride;
        float p[2] = {640.0f + (guest::LoadFloat(address) - 640.0f) * sx,
                      360.0f + (guest::LoadFloat(address + 4) - 360.0f) * sy};
        for (int k = 0; k < 2; k++) {
          uint32_t bits;
          std::memcpy(&bits, &p[k], sizeof(bits));
          *reinterpret_cast<uint32_t*>(guest::Virtual(address + 4 * k)) = rex::byte_swap(bits);
        }
      }
    }
  }
  SyncGuestMemory(PhysicalAddress(data.vertexAddress), data.vertexBytes, true);
  if (Tracing()) {
    const uint32_t* v = reinterpret_cast<const uint32_t*>(guest::Virtual(data.vertexAddress));
    // (up to the first 256 bytes)
    std::string words;
    for (uint32_t i = 0; i < std::min(data.vertexBytes, 256u) / 4; i++) {
      words += fmt::format(" {:08x}", rex::byte_swap(v[i]));
    }
    TraceLog("  inline vertices at {:#x} ({} bytes):{}", data.vertexAddress, data.vertexBytes,
             words);
  }
  if (data.indexAddress) {
    uint32_t indexSize = data.wideIndices ? 4 : 2;
    bool quads = data.quadList;
    Upload indices =
        Allocate(std::max<uint64_t>(ConvertedIndexBytes(data.indexCount, data.wideIndices, quads), 4), 16);
    const uint8_t* source = guest::Virtual(data.indexAddress);
    uint32_t drawn = ConvertIndices(indices.data, source, data.indexCount, data.wideIndices, quads);
    pending.record.drawCount = drawn;
    pending.record.indexView = RenderIndexBufferView(
        RenderBufferReference(indices.buffer, indices.offset), drawn * indexSize,
        data.wideIndices ? RenderFormat::R32_UINT : RenderFormat::R16_UINT);
    pending.record.indexed = true;
  }
  SubmitDraw(pending.record);
}

// Captures a draw from the device state; records it now, or keeps it for
// FlushPendingDraw when the data comes inline.
void Draw(uint32_t device, uint32_t primitive, uint32_t startVertex, uint32_t count,
          bool indexed, uint32_t startIndex, const InlineData* inlineData = nullptr) {
  StageTimer timer(Stage::kDraw);
  BeginFrame();
  auto findShader = [](uint32_t object) -> GuestShader* {
    auto it = g_guestShaders.find(object);
    return it != g_guestShaders.end() ? it->second.get() : nullptr;
  };
  GuestShader* vs = findShader(guest::Load32(device + 0x3190));
  GuestShader* ps = findShader(guest::Load32(device + 0x318C));
  RenderShader* vertexShader = vs ? GetHostShader(*vs) : nullptr;
  RenderShader* pixelShader = ps ? GetHostShader(*ps) : nullptr;
  // No pixel shader: a depth-only draw (Z prepass, shadow casters).
  bool depthOnly = guest::Load32(device + 0x318C) == 0;
  if (!vertexShader || (!pixelShader && !depthOnly) || count == 0) {
    return;
  }

  // Bound color targets from 0 up, while they match target 0's shape (one
  // framebuffer).
  HostTexture* targets[4] = {};
  uint32_t targetCount = 0;
  for (uint32_t i = 0; i < 4; i++) {
    HostTexture* surface =
        GetSurface(guest::Load32(device + guest::device::kRenderTargets + i * 4), false);
    if (!surface || (i > 0 && (surface->width != targets[0]->width ||
                               surface->height != targets[0]->height ||
                               surface->samples != targets[0]->samples))) {
      break;
    }
    targets[targetCount++] = surface;
  }
  HostTexture* target = targets[0];
  HostTexture* depth = GetSurface(guest::Load32(device + guest::device::kDepthStencil), true);
  if (!target && !depth) {
    return;
  }
  // A framebuffer's attachments may differ in size (the area is the
  // smallest); samples must match.
  if (target && depth && (depth->width < target->width || depth->height < target->height ||
                          target->samples != depth->samples)) {
    depth = nullptr;
  }

  // Shaders that write memory (bit 5 of the entry's mask, set by
  // XenosRecomp) must run even when nothing is rasterized.
  bool memExport = (vs->entry->specConstantsMask & 0x20) != 0;
  reg::PA_SU_SC_MODE_CNTL mode;
  mode.value = Register(device, 0x2205);
  bool rectList = xenos::PrimitiveType(primitive) == xenos::PrimitiveType::kRectangleList;
  if (rectList) {
    mode.cull_front = 0;  // Xenia never culls rectangle lists
    mode.cull_back = 0;
  }
  if (mode.cull_front && mode.cull_back) {
    if (!memExport) {
      return;
    }
    mode.cull_front = 0;
    mode.cull_back = 0;
  }

  // Shared constants.
  SharedConstants shared = {};
  for (auto& indices : shared.textureIndices) {
    for (auto& index : indices) {
      index = DESCRIPTOR_NULL_2D;
    }
  }
  shared.textureIndices[1][0] = DESCRIPTOR_NULL_3D;
  for (uint32_t i = 0; i < 32; i++) {
    shared.textureIndices[1][i] = DESCRIPTOR_NULL_3D;
    shared.textureIndices[2][i] = DESCRIPTOR_NULL_CUBE;
    shared.samplerIndices[i] = SAMPLER_LINEAR_CLAMP;
  }
  for (uint32_t i = 0; i < 8; i++) {
    shared.booleans[i] = guest::Load32(device + 0x2780 + i * 4);
  }
  // Textures: the fetch constants (24 bytes each) of the slots the shaders sample.
  bool widenHeader = false;  // the Options header, ours: a wider picture
  bool movieQuad = false;    // samples a movie's frame (movie.h)
  uint32_t textureSlots = vs->entry->textureSlotMask | (ps ? ps->entry->textureSlotMask : 0);
  uint32_t indexBase = 0, indexWidth = 0, indexHeight = 0;  // slot 0's, an 8-bit picture
  // (NG2_TEST_TOGGLE_LOG: each sampled texture's descriptor and memory, below)
  std::vector<uint32_t> textureRecords;
  for (uint32_t slot = 0; slot < 32; slot++) {
    if (!(textureSlots & (1u << slot))) {
      continue;
    }
    if ((guest::Load32(device + 0x480 + slot * 24) & 3) !=
        uint32_t(xenos::FetchConstantType::kTexture)) {
      continue;
    }
    xenos::xe_gpu_texture_fetch_t fetch;
    uint32_t* dwords = &fetch.dword_0;
    for (uint32_t i = 0; i < 6; i++) {
      dwords[i] = guest::Load32(device + 0x480 + slot * 24 + i * 4);
    }
    uint32_t descriptor = GetTexture(fetch);
    if (slot == 0 && fetch.format == xenos::TextureFormat::k_8) {
      indexBase = uint32_t(fetch.base_address) << 12;
      indexWidth = fetch.size_2d.width + 1, indexHeight = fetch.size_2d.height + 1;
    }
    movieQuad |= inlineData && movie::IsFrameTexture(uint32_t(fetch.base_address) << 12);
    widenHeader |= (uint32_t(fetch.base_address) << 12) == g_menuHeader.base &&
                   g_menuHeader.widthScale != 1.0f;
    if (slot == kBloomSlot && ps &&
        (ps->entry->hash == kToneMapPixelShader || ps->entry->hash == kCompositePixelShader)) {
      descriptor = AdjustBloom(fetch, descriptor);
    }
    {
      static const bool toggleLog = [] {
        const char* v = std::getenv("NG2_TEST_TOGGLE_LOG");
        return v && *v == '1';
      }();
      if (toggleLog) {
        uint32_t texBase = (uint32_t(fetch.base_address) << 12) & 0x1FFFFFFF;
        uint32_t bytes = std::min<uint32_t>(4u << 10, GUEST_MEMORY_SIZE - texBase);
        uint64_t h = XXH3_64bits(guest::Physical(texBase), bytes);
        textureRecords.insert(textureRecords.end(),
                              {0x4000u | slot, descriptor, uint32_t(h), uint32_t(h >> 32), texBase});
      }
    }
    if (Tracing()) {
      TraceLog("  tex {} fmt {} {}x{} base {:#x} tiled {} endian {} dim {} sign {}{}{}{} "
                  "swizzle {:#o} exp {} clamp {}{}{} mips {:#x} {}-{} filter {}{}{} -> {}",
                  slot, uint32_t(fetch.format), fetch.size_2d.width + 1,
                  fetch.size_2d.height + 1, uint32_t(fetch.base_address) << 12,
                  uint32_t(fetch.tiled), uint32_t(fetch.endianness), uint32_t(fetch.dimension),
                  uint32_t(fetch.sign_x), uint32_t(fetch.sign_y), uint32_t(fetch.sign_z),
                  uint32_t(fetch.sign_w), uint32_t(fetch.swizzle), int32_t(fetch.exp_adjust),
                  uint32_t(fetch.clamp_x), uint32_t(fetch.clamp_y), uint32_t(fetch.clamp_z),
                  uint32_t(fetch.mip_address) << 12, uint32_t(fetch.mip_min_level),
                  uint32_t(fetch.mip_max_level), uint32_t(fetch.min_filter),
                  uint32_t(fetch.mag_filter), uint32_t(fetch.mip_filter), descriptor);
    }
    if (descriptor) {
      shared.textureIndices[0][slot] = descriptor;  // 2D
      shared.textureIndices[3][slot] = descriptor;  // 1D (sampled as Nx1 2D)
      shared.samplerIndices[slot] = GetSampler(fetch);
    }
  }
  // The title menu's labels are 8-bit pictures looked up in a 256x1 palette
  // (slot 1). RECOMP SETTINGS, ours in place of the Marketplace label, is made
  // of the menu atlas's letters, so it takes the atlas's palette, drawn
  // before it in the frame: with the label's own, whose alphas differ, its
  // glow came out as a dark speckled box (#66).
  if (indexWidth == 520 && indexHeight == 990 && (textureSlots & 2)) {
    g_menuAtlasPalette = shared.textureIndices[0][1];
    g_menuAtlasPaletteFrame = g_frameCount;
  } else if (indexBase && indexBase == g_menuLabelBase && (textureSlots & 2) &&
             g_menuAtlasPalette && g_menuAtlasPaletteFrame == g_frameCount) {
    shared.textureIndices[0][1] = g_menuAtlasPalette;
    shared.textureIndices[3][1] = g_menuAtlasPalette;
  }
  // The header is a quad of inline vertices (position first) made each
  // frame: wider by as much as the picture, from its left edge.
  reg::RB_COLORCONTROL colorControl;
  colorControl.value = Register(device, 0x2202);
  shared.alphaThreshold = RegisterFloat(device, 0x210E);
  shared.alphaFunc = uint32_t(colorControl.alpha_func);
  shared.guestMemoryBase = g_guestMemoryAddress;
  for (uint32_t i = 0; i < 96; i++) {
    shared.vertexFetch[i][0] = guest::Load32(device + 0x480 + i * 8);
    shared.vertexFetch[i][1] = guest::Load32(device + 0x480 + i * 8 + 4);
  }
  if (inlineData) {
    // Inline data is stream 0; D3D puts its fetch constant in the command
    // stream, not in the device.
    shared.vertexFetch[95][0] = PhysicalAddress(inlineData->vertexAddress) |
                                uint32_t(xenos::FetchConstantType::kVertex);
    shared.vertexFetch[95][1] = ((inlineData->vertexBytes / 4) << 2) |
                                uint32_t(xenos::Endian::k8in32);
  }

  // Declared vertex elements, from the vertex declaration.
  // Pixel shaders can fetch vertex data too.
  uint32_t fetchMask[3];
  for (int i = 0; i < 3; i++) {
    fetchMask[i] = vs->entry->vertexFetchMask[i] | (ps ? ps->entry->vertexFetchMask[i] : 0);
  }
  uint32_t declaration = guest::Load32(device + 0x2E24);
  if (declaration && !vs->elements.empty()) {
    uint32_t declCount = guest::Load32(declaration + 24);
    for (size_t slot = 0; slot < vs->elements.size() && slot < 16; slot++) {
      auto [usage, usageIndex] = vs->elements[slot];
      for (uint32_t e = 0; e < declCount && e < 64; e++) {
        uint32_t element = declaration + 52 + e * 12;
        uint32_t streamOffset = guest::Load32(element);
        uint32_t type = guest::Load32(element + 4);
        uint32_t usageWord = guest::Load32(element + 8);
        if (((usageWord >> 16) & 0xFF) != usage || ((usageWord >> 8) & 0xFF) != usageIndex) {
          continue;
        }
        uint32_t stream = streamOffset >> 16;
        uint32_t offset = streamOffset & 0xFFFF;
        uint32_t stride = (inlineData && stream == 0)
                              ? inlineData->stride / 4
                              : guest::Virtual(device + 0x30E8 + stream)[0];
        uint32_t fetchSlot = 95 - std::min(stream, 95u);
        shared.vertexElements[slot][0] = fetchSlot;
        shared.vertexElements[slot][1] = offset / 4;
        shared.vertexElements[slot][2] = stride;
        shared.vertexElements[slot][3] = ElementFlags(type);
        fetchMask[fetchSlot / 32] |= 1u << (fetchSlot % 32);
        break;
      }
    }
  }

  // Vertex data the shader reads (inline data is synced when it has been
  // written).
  uint32_t interpBoneAddress = 0, interpBoneSize = 0;  // the bone palette (NG2_INTERP)
  // Which mesh this is (NG2_INTERP): its vertex and index data and range, the
  // same a frame on whatever the camera did (0: inline data, not known).
  uint64_t interpMesh = 0;
  if (!inlineData && InterpEnabled()) {
    interpMesh = (uint64_t(startVertex) << 32 | count) * 0x9E3779B97F4A7C15ull ^ startIndex;
    if (indexed) {
      if (uint32_t indexBuffer = guest::Load32(device + 0x308C)) {
        interpMesh = interpMesh * 0x100000001B3ull ^ guest::Load32(indexBuffer + 24);
      }
    }
  }
  for (uint32_t i = 0; i < (inlineData ? 95u : 96u); i++) {
    if (!(fetchMask[i / 32] & (1u << (i % 32)))) {
      continue;
    }
    uint32_t word0 = shared.vertexFetch[i][0];
    uint32_t word1 = shared.vertexFetch[i][1];
    if ((word0 & 3) != uint32_t(xenos::FetchConstantType::kVertex)) {
      continue;
    }
    uint32_t address = word0 & 0xFFFFFFFC;
    uint32_t size = word1 & 0x3FFFFFC;  // size in dwords at bit 2: bytes
    if (interpMesh && i != 94) {
      interpMesh = (interpMesh ^ (uint64_t(i) << 32 | address)) * 0x100000001B3ull;
    }
    if (size && size <= (16u << 20)) {
      SyncGuestMemory(address & 0x1FFFFFFF, size, false, i == 94);
      if (i == 94) {
        interpBoneAddress = address & 0x1FFFFFFF;
        interpBoneSize = size;
      }
    }
  }

  // Viewport: hardware transform, or positions in pixels.
  reg::PA_CL_VTE_CNTL vte;
  vte.value = Register(device, 0x2206);
  HostTexture* sizeSource = target ? target : depth;
  float targetWidth = float(sizeSource->width);
  float targetHeight = float(sizeSource->height);
  // The guest's viewport is in guest pixels; the target has scale host
  // pixels for each.
  float scale = float(sizeSource->scale);
  shared.resolutionScale = scale;
  // D3D's pixel centers (PA_SU_VTX_CNTL.pix_center 0) are at integer
  // coordinates: shift everything by half a guest pixel, as Xenia does, or
  // full-screen passes sample between texels and blurs drift.
  reg::PA_SU_VTX_CNTL vtxControl;
  vtxControl.value = Register(device, 0x2302);
  float halfPixel = REXCVAR_GET(half_pixel_offset) &&
                            vtxControl.pix_center == xenos::PixelCenter::kD3DZero
                        ? 0.5f
                        : 0.0f;
  RenderViewport viewport(0.0f, 0.0f, targetWidth, targetHeight, 0.0f, 1.0f);
  if (vte.vport_x_scale_ena) {
    float xScale = RegisterFloat(device, 0x210F) * scale;
    float xOffset = RegisterFloat(device, 0x2110) * scale;
    float yScale = RegisterFloat(device, 0x2111) * scale;
    float yOffset = RegisterFloat(device, 0x2112) * scale;
    float zScale = RegisterFloat(device, 0x2113), zOffset = RegisterFloat(device, 0x2114);
    viewport = RenderViewport(xOffset - std::fabs(xScale), yOffset - std::fabs(yScale),
                              2.0f * std::fabs(xScale), 2.0f * std::fabs(yScale),
                              // Window z = zOffset + z * zScale, as on Xenos: a
                              // negative scale (reversed depth) stays reversed,
                              // which Vulkan allows (minDepth > maxDepth).
                              zOffset, zOffset + zScale);
    // The host viewport is always top-down with positive sizes. D3D's usual
    // negative Y scale is what -fvk-invert-y (on every translated vertex
    // shader) expects; mirror only the unusual signs.
    shared.ndcTransform[0] = xScale < 0 ? -1.0f : 1.0f;
    shared.ndcTransform[1] = yScale < 0 ? 1.0f : -1.0f;
    // The half pixel moves the geometry, not the viewport (which clips): as
    // Xenia's ndc_offset. A host pixel is 1/|scale| in NDC; y is negated by
    // the shaders' -fvk-invert-y after this transform.
    if (xScale != 0.0f && yScale != 0.0f) {
      shared.ndcTransform[2] = halfPixel * scale / std::fabs(xScale);
      shared.ndcTransform[3] = -halfPixel * scale / std::fabs(yScale);
    }
  } else {
    // Window coordinates (guest pixels): map them to NDC over the target.
    shared.ndcTransform[0] = 2.0f * scale / targetWidth;
    shared.ndcTransform[1] = -2.0f * scale / targetHeight;
    shared.ndcTransform[2] = -1.0f + halfPixel * shared.ndcTransform[0];
    shared.ndcTransform[3] = 1.0f + halfPixel * shared.ndcTransform[1];
  }

  // Float constants, little-endian.
  Upload vsConstants = Allocate(256 * 16);
  if (g_traceConstants) {
    std::fwrite(guest::Virtual(device + 0x780), 1, 256 * 16, g_traceConstants);
    if (g_tracePsConstants) {
      std::fwrite(guest::Virtual(device + 0x1780), 1, 224 * 16, g_tracePsConstants);
    }
  }
  Upload psConstants = Allocate(224 * 16);
  const uint32_t* vsSource = reinterpret_cast<const uint32_t*>(guest::Virtual(device + 0x780));
  const uint32_t* psSource = reinterpret_cast<const uint32_t*>(guest::Virtual(device + 0x1780));
  // Only the registers the shader reads (the rest of the block is never read).
  auto copyConstants = [](uint8_t* out, const uint32_t* source, const uint64_t (&mask)[4],
                          uint32_t registers) {
    auto* dest = reinterpret_cast<uint32_t*>(out);
    for (uint32_t word = 0; word < 4; word++) {
      for (uint64_t bits = mask[word]; bits; bits &= bits - 1) {
        uint32_t r = word * 64 + uint32_t(__builtin_ctzll(bits));
        if (r >= registers) {
          break;
        }
        for (uint32_t c = 0; c < 4; c++) {
          dest[r * 4 + c] = rex::byte_swap(source[r * 4 + c]);
        }
      }
    }
  };
  copyConstants(vsConstants.data, vsSource, vs->entry->floatConstantMask, 256);
  if (ps) {
    copyConstants(psConstants.data, psSource, ps->entry->floatConstantMask, 224);
  }
  int32_t interpIndex = -1;
  if (InterpEnabled()) {
    const HostTexture* shape = target ? target : depth;
    uint64_t group = vs->entry->hash * 0x9E3779B97F4A7C15ull ^ (ps ? ps->entry->hash : 0) * 31 ^
                     (uint64_t(shape->width) << 40) ^ (uint64_t(shape->height) << 16) ^
                     uint64_t(shape->format) ^ (target ? 0 : 1ull << 63);
    interpIndex = CaptureInterpDraw(group, vsSource, vs->entry->floatConstantMask, ps ? psSource : nullptr,
                                    ps ? &ps->entry->floatConstantMask : nullptr);
    InterpDraw& captured = InterpCaptured(interpIndex);
    captured.boneAddress = interpBoneAddress;
    captured.boneSize = interpBoneSize;
    captured.mesh = interpMesh;
  }

  Upload sharedUpload = Allocate(sizeof(shared));
  std::memcpy(sharedUpload.data, &shared, sizeof(shared));

  // NG2_TEST_TOGGLE_LOG=1 (#21): draws whose shader constants switch between
  // two sets of values from frame to frame, logged with what differs.
  static const bool toggleLog = [] {
    const char* v = std::getenv("NG2_TEST_TOGGLE_LOG");
    return v && *v == '1';
  }();
  if (toggleLog) {
    struct Seen {
      std::vector<std::pair<uint64_t, std::vector<uint32_t>>> values;  // the last two sets
      std::vector<uint64_t> history;  // the last 8 frames'
      uint64_t frame = 0;
      bool logged = false;
    };
    static std::unordered_map<uint64_t, Seen> seen;
    static std::unordered_map<uint64_t, int> occurrences;
    static uint64_t occurrenceFrame = ~0ull;
    if (occurrenceFrame != g_frameCount) {
      occurrences.clear();
      occurrenceFrame = g_frameCount;
    }
    uint64_t base = vs->entry->hash * 31 + (ps ? ps->entry->hash : 0) * 7 + count;
    uint64_t key = base * 131 + occurrences[base]++;
    std::vector<uint32_t> now;
    auto collect = [&](const uint8_t* data, const uint64_t (&mask)[4], uint32_t registers, uint32_t tag) {
      const auto* words = reinterpret_cast<const uint32_t*>(data);
      for (uint32_t word = 0; word < 4; word++) {
        for (uint64_t bits = mask[word]; bits; bits &= bits - 1) {
          uint32_t r = word * 64 + uint32_t(__builtin_ctzll(bits));
          if (r >= registers) break;
          now.push_back(tag | r);
          for (uint32_t c = 0; c < 4; c++) now.push_back(words[r * 4 + c]);
        }
      }
    };
    collect(vsConstants.data, vs->entry->floatConstantMask, 256, 0);
    if (ps) collect(psConstants.data, ps->entry->floatConstantMask, 224, 0x1000);
    now.insert(now.end(), textureRecords.begin(), textureRecords.end());
    // Render state: the blend/depth/raster registers (tag 0x8000 | register).
    for (uint32_t r = 0x2100; r < 0x2115; r++) now.insert(now.end(), {0x8000u | r, Register(device, r), 0, 0, 0});
    for (uint32_t r = 0x2200; r < 0x220C; r++) now.insert(now.end(), {0x8000u | r, Register(device, r), 0, 0, 0});
    // The vertex data each fetch slot reads, as the guest memory holds it now
    // (tag 0x2000 | slot, the hash's two halves, address, size).
    for (uint32_t i = 0; i < 96u; i++) {
      if (!(fetchMask[i / 32] & (1u << (i % 32)))) continue;
      uint32_t word0 = shared.vertexFetch[i][0], word1 = shared.vertexFetch[i][1];
      if ((word0 & 3) != uint32_t(xenos::FetchConstantType::kVertex)) continue;
      uint32_t address = (word0 & 0xFFFFFFFC) & 0x1FFFFFFF, size = word1 & 0x3FFFFFC;
      if (!size || size > (16u << 20) || uint64_t(address) + size > GUEST_MEMORY_SIZE) continue;
      uint64_t h = XXH3_64bits(guest::Physical(address), size);
      now.push_back(0x2000 | i);
      now.push_back(uint32_t(h));
      now.push_back(uint32_t(h >> 32));
      now.push_back(address);
      now.push_back(size);
    }
    Seen& s = seen[key];
    if (s.frame + 1 != g_frameCount) {
      s.history.clear();
    }
    uint64_t h = XXH3_64bits(now.data(), now.size() * 4);
    if (s.values.size() < 2 || (s.values[0].first != h && s.values[1].first != h)) {
      if (s.values.size() == 2) s.values.erase(s.values.begin());
      s.values.push_back({h, now});
    }
    s.history.push_back(h);
    if (s.history.size() > 8) s.history.erase(s.history.begin());
    if (!s.logged && s.history.size() == 8) {
      std::unordered_set<uint64_t> distinct(s.history.begin(), s.history.end());
      int switches = 0;
      for (size_t i = 1; i < s.history.size(); i++) switches += s.history[i] != s.history[i - 1];
      // A, B, A: one frame unlike the ones either side (a single-frame glitch
      // as well as a steady flip-flop).
      size_t n = s.history.size();
      bool aba = s.history[n - 1] == s.history[n - 3] && s.history[n - 2] != s.history[n - 1];
      (void)distinct;
      if (aba && switches >= 2 && s.values.size() == 2) {
        s.logged = true;
        const auto& va = s.values[0].second;
        const auto& vb = s.values[1].second;
        std::string diff;
        for (size_t i = 0; i + 4 < va.size() && i + 4 < vb.size(); i += 5) {
          bool same = true;
          for (int c = 1; c <= 4; c++) same = same && va[i + c] == vb[i + c];
          if (!same && (va[i] & 0x8000)) {
            diff += fmt::format(" register {:#x}: {:#x} / {:#x}", va[i] & 0x7FFF, va[i + 1], vb[i + 1]);
          } else if (!same && (va[i] & 0x4000)) {
            diff += fmt::format(" texture slot {} at {:#x}: {}", va[i] & 0xFFF, va[i + 4],
                                va[i + 1] != vb[i + 1] ? fmt::format("descriptor {} / {}", va[i + 1], vb[i + 1])
                                                       : std::string("memory changed"));
          } else if (!same && (va[i] & 0x2000)) {
            diff += fmt::format(" vertex slot {} at {:#x}, {} bytes, changed", va[i] & 0xFFF, va[i + 3], va[i + 4]);
          } else if (!same) {
            float fa[4], fb[4];
            std::memcpy(fa, &va[i + 1], 16);
            std::memcpy(fb, &vb[i + 1], 16);
            diff += fmt::format(" {}c{}: {:.5g},{:.5g},{:.5g},{:.5g} / {:.5g},{:.5g},{:.5g},{:.5g}",
                                va[i] & 0x1000 ? "ps " : "vs ", va[i] & 0xFFF, fa[0], fa[1], fa[2], fa[3],
                                fb[0], fb[1], fb[2], fb[3]);
          }
        }
        REXLOG_INFO("toggle: vs {:016X} ps {:016X} count {} ({} switches in 8 frames):{}", vs->entry->hash,
                    ps ? ps->entry->hash : 0, count, switches, diff);
      }
    }
    s.frame = g_frameCount;
  }

  // Index data (converted to little-endian).
  uint32_t drawCount = count;
  std::optional<RenderIndexBufferView> indexView;
  auto primitiveType = xenos::PrimitiveType(primitive);
  if (inlineData && indexed) {
    // Indices arrive later (FlushPendingDraw).
  } else if (indexed) {
    uint32_t indexBuffer = guest::Load32(device + 0x308C);
    if (!indexBuffer) {
      return;
    }
    bool wide = guest::Load32(indexBuffer) & 0x80000000;
    uint32_t address = PhysicalAddress(guest::Load32(indexBuffer + 24));
    uint32_t indexSize = wide ? 4 : 2;
    bool quads = primitiveType == xenos::PrimitiveType::kQuadList;
    Upload indices = Allocate(std::max<uint64_t>(ConvertedIndexBytes(count, wide, quads), 4), 16);
    const uint8_t* source = guest::Physical(address + startIndex * indexSize);
    drawCount = ConvertIndices(indices.data, source, count, wide, quads);
    indexView = RenderIndexBufferView(RenderBufferReference(indices.buffer, indices.offset),
                                      drawCount * indexSize,
                                      wide ? RenderFormat::R32_UINT : RenderFormat::R16_UINT);
  } else if (primitiveType == xenos::PrimitiveType::kQuadList) {
    // Quads become two triangles each.
    uint32_t quads = count / 4;
    Upload indices = Allocate(uint64_t(quads) * 6 * 4, 16);
    auto* data = reinterpret_cast<uint32_t*>(indices.data);
    for (uint32_t q = 0; q < quads; q++) {
      uint32_t v = startVertex + q * 4;
      uint32_t* out = data + q * 6;
      out[0] = v; out[1] = v + 1; out[2] = v + 2;
      out[3] = v; out[4] = v + 2; out[5] = v + 3;
    }
    drawCount = quads * 6;
    indexView = RenderIndexBufferView(RenderBufferReference(indices.buffer, indices.offset),
                                      drawCount * 4, RenderFormat::R32_UINT);
    indexed = true;
    startVertex = 0;
  }

  PipelineKey key = {};
  key.vertexShader = vs->entry->hash;
  key.pixelShader = ps ? ps->entry->hash : 1;  // 0 is the debug shader's
  key.primitive = primitive;
  for (uint32_t i = 0; i < 4; i++) {
    key.colorFormats[i] =
        uint32_t(i < targetCount ? targets[i]->format : RenderFormat::UNKNOWN);
  }
  key.depthFormat = uint32_t(depth ? depth->format : RenderFormat::UNKNOWN);
  key.samples = uint32_t(sizeSource->samples);
  static constexpr uint32_t kBlendControls[4] = {0x2201, 0x2209, 0x220A, 0x220B};
  for (uint32_t i = 0; i < targetCount; i++) {
    key.blendControls[i] = Register(device, kBlendControls[i]);
  }
  // A target the pixel shader doesn't write keeps its contents.
  uint32_t written = 0;
  for (uint32_t i = 0; i < 4; i++) {
    written |= (ps && (ps->colorOutputs >> i & 1)) ? 0xFu << (i * 4) : 0;
  }
  key.colorMask = Register(device, 0x2104) & written & ((1u << (targetCount * 4)) - 1);
  key.depthControl = Register(device, 0x2200);
  key.stencilMasks = Register(device, 0x210D);
  key.cull = mode.value & 0x7;
  PolygonOffset(device, mode, primitive, key.depthBias, key.slopeDepthBias);
  key.alphaTest = colorControl.alpha_test_enable;
  key.alphaToCoverage = ps && colorControl.alpha_to_mask_enable;  // needs a fragment shader
  // =all colors the skinned draws only (bones in fetch slot 94): other
  // shaders include full-screen passes that would cover them.
  bool skinned = (vs->entry->vertexFetchMask[2] >> 30) & 1;
  // =rigid: the unskinned ones instead, small draws only (not full-screen
  // passes or level geometry).
  static const char* highlight = std::getenv("NG2_DEBUG_HIGHLIGHT");
  bool rigid = highlight && std::strcmp(highlight, "rigid") == 0;
  bool all = highlight && std::strcmp(highlight, "all") == 0;
  if (Highlighted(vs->entry->hash) &&
      (rigid ? !skinned && count > 6 && count < 3000 : skinned || !all) &&
      [&] {  // NG2_DEBUG_EXCLUDE=<hash>,<hash>: never these (full-screen passes)
        static const char* exclude = std::getenv("NG2_DEBUG_EXCLUDE");
        return !exclude || !std::strstr(exclude, fmt::format("{:016X}", vs->entry->hash).c_str());
      }()) {
    key.pixelShader = 0;  // the debug shader's pipelines
    pixelShader = g_debugShader.get();
    DebugColor(vs->entry->hash, reinterpret_cast<SharedConstants*>(sharedUpload.data)->debugColor);
  }
  // NG2_DEBUG_SKIP=<vs hash prefix>[:<count>],...: those draws aren't made (to
  // find what a picture is made of); NG2_DEBUG_SKIP_BLINK=1: only 3 s of every 6.
  if (static const char* skip = std::getenv("NG2_DEBUG_SKIP"); skip && [&] {
        static const bool blink = std::getenv("NG2_DEBUG_SKIP_BLINK") != nullptr;
        if (blink) {
          static const auto start = std::chrono::steady_clock::now();
          auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
          if ((secs / 3) % 2 == 0) {
            return false;
          }
        }
        std::string hash = fmt::format("{:016X}", vs->entry->hash);
        std::string list = skip;
        for (size_t at = 0; at <= list.size();) {
          size_t end = list.find(',', at);
          if (end == std::string::npos) end = list.size();
          std::string entry = list.substr(at, end - at);
          size_t colon = entry.find(':');
          std::string prefix = entry.substr(0, colon);
          if (!prefix.empty() && hash.compare(0, prefix.size(), prefix) == 0 &&
              (colon == std::string::npos || std::strtoul(entry.c_str() + colon + 1, nullptr, 10) == count)) {
            return true;
          }
          at = end + 1;
        }
        return false;
      }()) {
    return;
  }
  RenderPipeline* pipeline = GetPipeline(key, vertexShader, pixelShader);
  if (!pipeline) {
    return;
  }

  if (Tracing()) {
    for (size_t slot = 0; slot < vs->elements.size(); slot++) {
      TraceLog("  element {} usage {}/{}: fetch {} offset {} stride {} flags {:#x}", slot,
                  vs->elements[slot].first, vs->elements[slot].second,
                  shared.vertexElements[slot][0], shared.vertexElements[slot][1],
                  shared.vertexElements[slot][2], shared.vertexElements[slot][3]);
    }
    TraceLog("  fetch95 {:08x} {:08x} fetch94 {:08x} {:08x} vs container {:#x} ps container {:#x}",
                shared.vertexFetch[95][0], shared.vertexFetch[95][1], shared.vertexFetch[94][0],
                shared.vertexFetch[94][1], vs->container,
                ps ? ps->container : 0);
    TraceLog("draw prim {} count {} indexed {} vs {:016X} ps {:016X} target {}x{} fmt {} "
                "targets {} mask {:#x} blend {:#x} depth {:#x} vte {:#x} vp {} {} {} {} "
                "base {} indx_offset {:#x} min/max {:#x}/{:#x} colorcontrol {:#x} bools {:08x} "
                "bias {} {} cull {} depthfmt {} depthtex {:x} scissor {} {} {} {} {} key {:016X} "
                "vport {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} z {} {} clip {:#x} mode {:#x} vtx {:#x} {:#x} {:#x} viz {:#x}",
                primitive, count, indexed, vs->entry->hash, ps ? ps->entry->hash : 0,
                sizeSource->width, sizeSource->height, uint32_t(sizeSource->format), targetCount,
                key.colorMask, key.blendControls[0], key.depthControl, vte.value, viewport.x, viewport.y,
                viewport.width, viewport.height, startVertex, Register(device, 0x2102),
                Register(device, 0x2101), Register(device, 0x2100), colorControl.value,
                shared.booleans[4], key.depthBias, key.slopeDepthBias, key.cull, key.depthFormat,
                reinterpret_cast<uintptr_t>(depth), guest::Load32(device + kScissorEnable),
                int32_t(guest::Load32(device + kScissorRect)), int32_t(guest::Load32(device + kScissorRect + 4)),
                int32_t(guest::Load32(device + kScissorRect + 8)), int32_t(guest::Load32(device + kScissorRect + 12)),
                XXH3_64bits(&key, sizeof(key)), Register(device, 0x210F), Register(device, 0x2110),
                Register(device, 0x2111), Register(device, 0x2112), Register(device, 0x2113),
                Register(device, 0x2114), viewport.minDepth, viewport.maxDepth, Register(device, 0x2204),
                Register(device, 0x2205), Register(device, 0x2300), Register(device, 0x2301),
                Register(device, 0x2302), Register(device, 0x2293));
    if (!g_drawStack.empty()) {
      TraceLog("  stack {}", g_drawStack);
    }
    // The constants the shaders read, in full (with a hash of each set), to see
    // what changes between frames.
    auto readConstants = [](const uint8_t* data, const uint64_t (&mask)[4], uint32_t registers,
                            bool full) {
      const auto* words = reinterpret_cast<const uint32_t*>(data);
      std::vector<uint32_t> used;
      std::string text;
      for (uint32_t word = 0; word < 4; word++) {
        for (uint64_t bits = mask[word]; bits; bits &= bits - 1) {
          uint32_t r = word * 64 + uint32_t(__builtin_ctzll(bits));
          if (r >= registers) {
            break;
          }
          for (uint32_t c = 0; c < 4; c++) {
            used.push_back(words[r * 4 + c]);
          }
          if (full) {
            float f[4];
            std::memcpy(f, words + r * 4, sizeof(f));
            text += fmt::format(" c{}={:.5g},{:.5g},{:.5g},{:.5g}", r, f[0], f[1], f[2], f[3]);
          }
        }
      }
      return std::make_pair(XXH3_64bits(used.data(), used.size() * 4), text);
    };
    auto vsc = readConstants(vsConstants.data, vs->entry->floatConstantMask, 256, true);
    auto psc = ps ? readConstants(psConstants.data, ps->entry->floatConstantMask, 224, true)
                  : std::make_pair(uint64_t(0), std::string());
    TraceLog("  consts vs {:016x} ps {:016x}{} |{}", vsc.first, psc.first, psc.second, vsc.second);
  }
  DrawRecord record;
  record.pipeline = pipeline;
  for (uint32_t i = 0; i < targetCount; i++) {
    record.blendConstants |= UsesBlendConstants(key.blendControls[i]);
  }
  if (record.blendConstants) {
    for (uint32_t i = 0; i < 4; i++) {
      uint32_t bits = Register(device, 0x2105 + i);
      std::memcpy(&record.blendColor[i], &bits, sizeof(bits));
    }
  }
  record.vertexShader = vs->entry->hash;
  record.pixelShader = ps ? ps->entry->hash : 0;
  record.memExport = memExport;
  record.interpIndex = interpIndex;
  g_frameMemExport |= memExport;
  std::copy(targets, targets + targetCount, record.targets);
  record.targetCount = targetCount;
  record.depth = depth;
  record.viewport = viewport;
  record.scissor = RenderRect(0, 0, int32_t(targetWidth), int32_t(targetHeight));
  // The game's scissor (D3D's SetScissorRect, while D3DRS_SCISSORTESTENABLE
  // is set) in guest pixels: the HUD clips gauges with it.
  if (guest::Load32(device + kScissorEnable)) {
    auto edge = [&](uint32_t offset, float limit) {
      float at = float(int32_t(guest::Load32(device + kScissorRect + offset))) * scale;
      return int32_t(std::clamp(at, 0.0f, limit));
    };
    int32_t left = edge(0, targetWidth), top = edge(4, targetHeight);
    // An inverted rect clips everything, as an empty one.
    record.scissor = RenderRect(left, top, std::max(left, edge(8, targetWidth)),
                                std::max(top, edge(12, targetHeight)));
  }
  record.pushConstants[0] = vsConstants.address;
  record.pushConstants[1] = psConstants.address;
  record.pushConstants[2] = sharedUpload.address;
  record.indexed = indexed;
  record.indexView = indexView;
  record.drawCount = drawCount;
  record.startVertex = startVertex;
  if (inlineData) {
    g_pendingDraw = PendingDraw{std::move(record), *inlineData,
                                widenHeader ? g_menuHeader.widthScale : 1.0f, movieQuad};
  } else {
    SubmitDraw(record);
  }
}

// Present ---------------------------------------------------------------------

// F7's pictures: the front buffer copied to a readback buffer, written once
// the GPU is done with it (at the next present).
struct TracePicture {
  std::unique_ptr<RenderBuffer> buffer;
  uint64_t size = 0;
  std::string path;
  uint32_t width = 0, height = 0, rowWidth = 0;
  RenderFormat format = RenderFormat::UNKNOWN;
  bool pending = false;
};
TracePicture g_tracePicture;

// One in four pixels each way, as a PPM.
void WriteTracePicture() {
  TracePicture& picture = g_tracePicture;
  picture.pending = false;
  bool wide = picture.format == RenderFormat::R16G16B16A16_UNORM;
  uint32_t texel = wide ? 8 : 4;
  const auto* data = static_cast<const uint8_t*>(picture.buffer->map());
  FILE* file = data ? std::fopen(picture.path.c_str(), "wb") : nullptr;
  if (file) {
    constexpr uint32_t kStep = 4;
    uint32_t width = picture.width / kStep, height = picture.height / kStep;
    fmt::print(file, "P6\n{} {}\n255\n", width, height);
    std::vector<uint8_t> row(width * 3);
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        const uint8_t* at = data + (size_t(y * kStep) * picture.rowWidth + x * kStep) * texel;
        for (uint32_t c = 0; c < 3; c++) {
          row[x * 3 + c] = wide ? at[c * 2 + 1] : at[c];  // the high byte of a 16-bit channel
        }
      }
      std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
  }
  if (data) {
    picture.buffer->unmap();
  }
}

// F7's memory: every readable mapping inside guest memory, as records of
// <guest address u32><size u32><bytes>, zstd-compressed, for finding what the
// game held in the traced frame. The mappings come from /proc/self/maps on
// Linux and VirtualQuery on Windows.
void WriteTraceMemory(const std::string& path) {
  uintptr_t low = reinterpret_cast<uintptr_t>(guest::Virtual(0));
  uintptr_t high = low + 0x100000000ull;
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    return;
  }
  ZSTD_CCtx* zstd = ZSTD_createCCtx();
  ZSTD_CCtx_setParameter(zstd, ZSTD_c_compressionLevel, 1);
  std::vector<uint8_t> out(ZSTD_CStreamOutSize());
  auto write = [&](const void* data, size_t size, ZSTD_EndDirective end) {
    ZSTD_inBuffer in{data, size, 0};
    size_t left;
    do {
      ZSTD_outBuffer o{out.data(), out.size(), 0};
      left = ZSTD_compressStream2(zstd, &o, &in, end);
      std::fwrite(out.data(), 1, o.pos, file);
    } while (!ZSTD_isError(left) && (in.pos < in.size || (end == ZSTD_e_end && left)));
  };
  // One readable range [a, b), clipped to guest memory, in records of up to 16 MB.
  auto range = [&](uintptr_t a, uintptr_t b) {
    uintptr_t start = std::max<uintptr_t>(a, low), end = std::min<uintptr_t>(b, high);
    for (uintptr_t at = start; at < end; at += 0x1000000) {
      uint32_t header[2] = {uint32_t(at - low), uint32_t(std::min<uintptr_t>(end - at, 0x1000000))};
      write(header, sizeof(header), ZSTD_e_continue);
      write(reinterpret_cast<const void*>(at), header[1], ZSTD_e_continue);
    }
  };
#if defined(_WIN32)
  for (uintptr_t at = low; at < high;) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info))) {
      break;
    }
    uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    bool readable = info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD) && (info.Protect & kReadable);
    if (readable) {
      range(at, end);
    }
    at = std::max(end, at + 0x1000);
  }
#else
  FILE* maps = std::fopen("/proc/self/maps", "r");
  char line[512];
  while (maps && std::fgets(line, sizeof(line), maps)) {
    unsigned long a = 0, b = 0;
    char perms[8] = {};
    if (std::sscanf(line, "%lx-%lx %7s", &a, &b, perms) == 3 && perms[0] == 'r') {
      range(a, b);
    }
  }
  if (maps) {
    std::fclose(maps);
  }
#endif
  write(nullptr, 0, ZSTD_e_end);
  std::fclose(file);
  ZSTD_freeCCtx(zstd);
}

// The trace's start, end and pictures, at each present.
void TraceFrame(HostTexture* frontBuffer) {
  if (g_tracePicture.pending) {
    WaitForGPU();
    WriteTracePicture();
  }
  bool opened = false;
  if (g_traceRequest.exchange(false) && !g_traceFile) {
    opened = true;
    const char* home = std::getenv("HOME");
#if defined(_WIN32)
    if (!home) {
      home = std::getenv("USERPROFILE");  // (C:\Users\<you>; HOME is rarely set there)
    }
#endif
    for (int n = 1; n < 1000 && !g_traceFile; n++) {
      g_traceDir = fmt::format("{}/ng2-trace-{}", home ? home : ".", n);
      std::error_code error;
      if (std::filesystem::create_directory(g_traceDir, error)) {
        g_traceFile = std::fopen((g_traceDir + "/draws.txt").c_str(), "w");
      }
    }
    const char* frames = std::getenv("NG2_TRACE_FRAMES");
    g_traceLeft = frames ? std::max(std::atoi(frames), 1) : 60;
    g_traceFrames = 0;
    REXLOG_INFO("video: tracing {} frames to {}", g_traceLeft, g_traceDir);
    if (g_traceFile && !std::getenv("NG2_TRACE_NOMEMORY") && !TracePicturesOnly()) {
      WriteTraceMemory(fmt::format("{}/memory-{}.zst", g_traceDir, g_frameCount));
      g_traceConstants = std::fopen(fmt::format("{}/vs-constants-{}.bin", g_traceDir, g_frameCount).c_str(), "wb");
      if (TraceAllConstants()) {
        g_tracePsConstants = std::fopen(fmt::format("{}/ps-constants-{}.bin", g_traceDir, g_frameCount).c_str(), "wb");
      }
    }
  } else if (g_traceConstants && !TraceAllConstants()) {
    std::fclose(g_traceConstants);  // the first frame's only
    g_traceConstants = nullptr;
  }
  if (!g_traceFile) {
    return;
  }
  TraceLog("present frame {}", g_frameCount);
  std::fflush(g_traceFile);  // quitting doesn't flush stdio
  static const int pictureEvery = [] {  // NG2_TRACE_PICTURES=<n>: a picture every n frames (10)
    const char* v = std::getenv("NG2_TRACE_PICTURES");
    return v ? std::max(std::atoi(v), 0) : 10;  // (0: none)
  }();
  if (pictureEvery > 0 && g_traceFrames++ % pictureEvery == 0 && frontBuffer && frontBuffer->samples == RenderSampleCount::COUNT_1 &&
      (frontBuffer->format == RenderFormat::R8G8B8A8_UNORM ||
       frontBuffer->format == RenderFormat::R16G16B16A16_UNORM)) {
    TracePicture& picture = g_tracePicture;
    uint32_t texel = frontBuffer->format == RenderFormat::R16G16B16A16_UNORM ? 8 : 4;
    uint32_t rowWidth = (frontBuffer->width + 63) & ~63u;
    uint64_t size = uint64_t(rowWidth) * frontBuffer->height * texel;
    if (!picture.buffer || picture.size < size) {
      picture.buffer = g_device->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
      picture.size = picture.buffer ? size : 0;
    }
    if (picture.buffer) {  // (none if it couldn't be made: no picture this time)
      picture.path = fmt::format("{}/frame-{}.ppm", g_traceDir, g_frameCount);
      picture.width = frontBuffer->width, picture.height = frontBuffer->height;
      picture.rowWidth = rowWidth, picture.format = frontBuffer->format;
      Transition(*frontBuffer, RenderTextureLayout::COPY_SOURCE);
      CommandList()->copyTextureRegion(
          RenderTextureCopyLocation::PlacedFootprint(picture.buffer.get(), frontBuffer->format,
                                                     frontBuffer->width, frontBuffer->height, 1,
                                                     rowWidth),
          RenderTextureCopyLocation::Subresource(frontBuffer->texture.get()));
      picture.pending = true;
    }
  }
  // The frame starting now is traced: the count drops at the presents that end traced frames.
  if (!opened && --g_traceLeft <= 0) {
    std::fclose(g_traceFile);
    g_traceFile = nullptr;
    for (FILE** f : {&g_traceConstants, &g_tracePsConstants}) {
      if (*f) {
        std::fclose(*f);
        *f = nullptr;
      }
    }
    REXLOG_INFO("video: trace done: {}", g_traceDir);
  }
}

// Full-screen movies (the attract trailer): the movie stand-in
// (src/movie) decodes them and hands each frame over; the presenter shows
// the latest while frames keep coming, instead of the game's picture.
std::mutex g_movieMutex;
std::vector<uint8_t> g_movieRGBA;  // the latest frame, RGBA8
uint32_t g_movieWidth = 0, g_movieHeight = 0;
uint64_t g_movieSerial = 0;
std::chrono::steady_clock::time_point g_movieTime;
std::unique_ptr<HostTexture> g_movieTexture;
uint64_t g_movieUploaded = 0;
constexpr auto kMovieShown = std::chrono::milliseconds(250);

// The latest movie frame on the GPU, or null when no movie is showing.
// Before the frame's render pass (it records a copy).
HostTexture* MovieTexture() {
  std::lock_guard lock(g_movieMutex);
  if (!g_movieSerial || std::chrono::steady_clock::now() - g_movieTime > kMovieShown) {
    return nullptr;
  }
  if (!g_movieTexture || g_movieTexture->width != g_movieWidth ||
      g_movieTexture->height != g_movieHeight) {
    if (g_movieTexture) {
      g_garbage[g_frame].push_back(std::move(g_movieTexture));
    }
    g_movieTexture = std::make_unique<HostTexture>();
    g_movieTexture->width = g_movieWidth;
    g_movieTexture->height = g_movieHeight;
    g_movieTexture->format = RenderFormat::R8G8B8A8_UNORM;
    g_movieTexture->texture = g_device->createTexture(
        RenderTextureDesc::Texture2D(g_movieWidth, g_movieHeight, 1, RenderFormat::R8G8B8A8_UNORM));
    if (!g_movieTexture->texture) {
      g_movieTexture.reset();
      return nullptr;
    }
    SetDescriptor(*g_movieTexture);
    g_movieUploaded = 0;
  }
  if (g_movieUploaded != g_movieSerial) {
    g_movieUploaded = g_movieSerial;
    uint32_t rowBytes = g_movieWidth * 4;
    uint32_t pitch = (rowBytes + 255) & ~255u;
    Upload upload = Allocate(uint64_t(pitch) * g_movieHeight, 512);
    for (uint32_t y = 0; y < g_movieHeight; y++) {
      std::memcpy(upload.data + uint64_t(y) * pitch, g_movieRGBA.data() + uint64_t(y) * rowBytes,
                  rowBytes);
    }
    Transition(*g_movieTexture, RenderTextureLayout::COPY_DEST);
    CommandList()->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(g_movieTexture->texture.get()),
        RenderTextureCopyLocation::PlacedFootprint(upload.buffer, RenderFormat::R8G8B8A8_UNORM,
                                                   g_movieWidth, g_movieHeight, 1, pitch / 4,
                                                   upload.offset));
  }
  Transition(*g_movieTexture, RenderTextureLayout::SHADER_READ);
  return g_movieTexture.get();
}

void Present(HostTexture* frontBuffer) {
  StageTimer timer(Stage::kPresent);
  {
    static uint32_t lastTicks = 0;
    uint32_t ticks = guest::Load32(0x84C55B50);
    if (lastTicks) {
      uint32_t d = ticks - lastTicks;
      if (d == 0) {
        g_framesNoTick++;
      } else if (d >= 2 && d < 600) {  // (not a load or a pause)
        g_framesManyTicks++;
      }
    }
    lastTicks = ticks;
  }
  auto* commandList = CommandList();
  FinishGuestMemory();
  // NG2_TRACE_FILE=<file>: as F7 when the file appears (for test runs).
  static const char* traceFile = std::getenv("NG2_TRACE_FILE");
  if (traceFile && g_frameCount % 30 == 0 && !g_traceFile) {
    std::error_code error;
    if (std::filesystem::remove(traceFile, error)) {
      g_traceRequest = true;
    }
  }
  // NG2_TEST_WAIT_GPU=1: the CPU waits for the GPU at each present, as F7's
  // pictures make it (a test for races between the two).
  static const bool waitGpu = [] {
    const char* v = std::getenv("NG2_TEST_WAIT_GPU");
    return v && *v == '1';
  }();
  if (waitGpu) {
    WaitForGPU();
  }
  // The display's rate (SDL's, from the UI thread) for the display clock's
  // refreshes a frame, asked for every 5 s: a 120 Hz switch shows up.
  {
    static int64_t rateAskedAt = 0;
    if (int64_t now = SteadyNs(); now - rateAskedAt > 5'000'000'000) {
      rateAskedAt = now;
      RequestDisplayRate();
    }
  }
  if (g_traceFile || g_traceRequest || g_tracePicture.pending) {
    TraceFrame(frontBuffer);
  } else if (Tracing()) {
    TraceLog("present frame {}", g_frameCount);
  }

  // Takes effect through a resize. vsync = false from older settings means
  // off until present_mode is set.
  int32_t mode = REXCVAR_GET(present_mode);
  if (rex::cvar::GetFlagSource("present_mode") == rex::cvar::Source::kDefault &&
      !REXCVAR_GET(vsync)) {
    mode = 0;
  }

  // On the game thread: what reads its memory or the textures' layouts.
  HostTexture* movie = MovieTexture();
  if (frontBuffer) {
    Transition(*frontBuffer, RenderTextureLayout::SHADER_READ);
  }
  HostTexture* source = movie && movie->descriptor ? movie : frontBuffer;
  uint32_t sourceDescriptor = source ? source->descriptor : 0;
  float aspect = source && source == movie ? float(movie->width) / float(movie->height)
                                           : ng2::aspect::Aspect();
  uint32_t gamma = g_gammaTable ? g_gammaTable->descriptor : 0u;
  uint32_t frame = g_frame;
  // The pictures between the last frame and this one (NG2_INTERP); not for a
  // movie the presenter draws itself.
  // Made on their own thread while the game goes on to its next frame (in a
  // crowd it took up to 15 ms of the game's 16); the render thread waits for
  // them before drawing them.
  std::shared_future<std::shared_ptr<InterpFrames>> interpJob;
  const bool interpWanted = source == frontBuffer && sourceDescriptor;
  std::vector<CarriedTexture> carried;
  PictureLayouts layouts;
  if (InterpEnabled()) {
    interpJob = StartInterpBuild(frame, g_frameCount, SteadyNs());
    carried = TakeCarried();
    layouts.Take();
  }
  g_logOps = false;
  OpArena* frameLog = &g_frameLogs[frame];
  g_boundFramebuffer = nullptr;
  g_recording = false;
  (void)commandList;
  // The prologue's copies done before the frame's commands read the memory.
  bool prologue = g_prologueOpen[frame];
  if (prologue) {
    g_prologueOpen[frame] = false;
    auto* list = g_prologueLists[frame].get();
    list->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY,
                   RenderBufferBarrier(g_guestMemory.get(),
                                       RenderBufferAccess::READ | RenderBufferAccess::WRITE));
    list->end();
  }
  // The frame's submit: the prologue first, once.
  auto submit = [frame, prologue](RenderCommandList* list, RenderCommandSemaphore** waits,
                                  uint32_t waitCount, RenderCommandSemaphore** signals,
                                  uint32_t signalCount) mutable {
    const RenderCommandList* lists[2] = {g_prologueLists[frame].get(), list};
    if (prologue && PrologueMode() == 2) {
      g_queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, nullptr);
      prologue = false;
    }
    g_queue->executeCommandLists(prologue ? lists : lists + 1, prologue ? 2u : 1u, waits,
                                 waitCount, signals, signalCount, g_commandFences[frame].get());
    prologue = false;
  };

  // The frame's pad read (NG2_LATENCY_LOG), to its present.
  int64_t inputNs = g_handedInputNs.exchange(0);
  // GPU timing: the present pass's start and the frame's end (written below,
  // with the real list).
  uint32_t presentMark = AllocGpuMark(GpuMark::kPresentStart);
  uint32_t endMark = AllocGpuMark(GpuMark::kFrameEnd);  // (each written once: see the resize path)
  // The swapchain's part, with the frame's real command list.
  OnRenderThread([=](RenderCommandList* list) mutable {
    // (render thread timing, NG2_INTERP: what a frame's pictures waited on)
    static int64_t jobWaitNs = 0, jobWaitMaxNs = 0, replayNs = 0, replayMaxNs = 0, roomWaitNs = 0,
                   roomWaitMaxNs = 0;
    int64_t jobStart = SteadyNs();
    // (NG2_PROFILE, once a second: this thread's time and the present's parts)
    struct PresentParts {
      int64_t whole = 0, build = 0, record = 0, room = 0, acquire = 0, submit = 0, logAt = 0;
      uint32_t frames = 0;
    };
    static PresentParts parts;
    struct PartsTimed {
      int64_t start;
      ~PartsTimed() {
        int64_t end = SteadyNs();
        parts.whole += end - start;
        parts.frames++;
        if (!parts.logAt) {
          parts.logAt = end;
        }
        if (end - parts.logAt >= 1'000'000'000 && g_profile.enabled) {
          double s = double(end - parts.logAt) / 1e9;
          REXLOG_INFO("video: render commands thread: busy {:.0f} ms a second ({} frames); presents {:.0f} ms: "
                      "waiting for the build {:.0f}, recording pictures {:.0f}, waiting for the presenter {:.0f}, "
                      "acquiring images {:.0f}, submitting {:.0f}",
                      double(g_renderBusyNs.exchange(0)) / 1e6 / s, parts.frames, double(parts.whole) / 1e6 / s,
                      double(parts.build) / 1e6 / s, double(parts.record) / 1e6 / s, double(parts.room) / 1e6 / s,
                      double(parts.acquire) / 1e6 / s, double(parts.submit) / 1e6 / s);
          parts = PresentParts{};
          parts.logAt = end;
        }
      }
    } partsTimed{jobStart};
    std::shared_ptr<InterpFrames> interp = interpJob.valid() ? interpJob.get() : nullptr;
    parts.build += SteadyNs() - jobStart;
    if (interpJob.valid()) {
      int64_t took = SteadyNs() - jobStart;
      jobWaitNs += took;
      jobWaitMaxNs = std::max(jobWaitMaxNs, took);
    }
    if (!interpWanted) {
      interp.reset();
    }
    static int32_t loggedMode = -1;
    // NG2_INTERP: the presenter shows a picture each refresh itself; mailbox
    // presents don't block (a FIFO present in full screen may wait for the
    // screen holding the queue, and the frame's submit with it).
    if (InterpEnabled() && mode != 0) {
      mode = 2;
    }
    bool supported = g_swapChain->setPresentMode(RenderPresentMode(std::clamp(mode, 0, 3)));
    if (mode != loggedMode) {
      loggedMode = mode;
      REXLOG_INFO("video: present mode {}{}", mode, supported ? "" : " not supported, FIFO instead");
    }
    g_swapChainValid &= !g_swapChain->needsResize();
    if (!g_swapChainValid) {
      // Finish what was recorded, then rebuild the swapchain: here without
      // the thread, else by the game thread once the GPU is idle.
      WriteGpuMark(list, frame, presentMark);
      WriteGpuMark(list, frame, endMark);
      presentMark = endMark = UINT32_MAX;  // (without the thread the list goes on below)
      list->end();
      submit(list, nullptr, 0, nullptr, 0);
      if (RenderThreadEnabled()) {
        g_resizeRequested = true;
        FrameSubmitted();
        return;
      }
      g_commandListPending[frame] = true;
      WaitForGPU();
      g_swapChainFramebuffers.clear();
      g_swapChainValid = g_swapChain->resize();
      SetInterpPictureCap();
      list->begin();
    }

    // (waits that keep timing out: a compositor that doesn't report the
    // screen, as a hidden window may; Low is left off
    // for 5 s then, and tried again)
    static int timeouts = 0;
    static auto pausedUntil = std::chrono::steady_clock::time_point{};
    if (g_swapChainValid && REXCVAR_GET(input_latency) == 1 && !UnderGamescope() && !interp &&
        std::chrono::steady_clock::now() >= pausedUntil) {
      // Low leaves a frame one refresh to be ready, and a scene whose frames
      // take about that long shows many of them twice for minutes (#36,
      // #46: 20-30% of the frames). While frames repeat, one frame is left
      // queued, as Standard does, and Low comes back once they stop for a
      // while. NG2_TEST_LOW_ADAPT=0 keeps the plain wait.
      // A scene that stays heavy would have Low return and relax every few
      // seconds, a latency step each time: a relaxation soon after a return
      // holds twice as long as the last, up to a minute.
      static const bool adapt = !TestSwitch("NG2_TEST_LOW_ADAPT", "0");
      static bool relaxed = false;
      static auto relaxedUntil = std::chrono::steady_clock::time_point{};
      static auto returnedAt = std::chrono::steady_clock::time_point{};
      static std::chrono::seconds hold{5};
      auto now = std::chrono::steady_clock::now();
      int repeating = g_repeatsLastSecond.load();
      if (adapt && repeating >= 3) {
        if (!relaxed) {
          relaxed = true;
          bool soon = returnedAt != std::chrono::steady_clock::time_point{} &&
                      now - returnedAt < std::chrono::seconds(15);
          hold = soon ? std::min(hold * 2, std::chrono::seconds(60)) : std::chrono::seconds(5);
          g_swapChain->setMaxFrameLatency(2);
          int count = ++g_lowRelaxations;
          if (count <= 5) {
            REXLOG_INFO("video: Input Latency Low: {} frames stayed an extra refresh in the last "
                        "second, one frame queued for {} s", repeating, hold.count());
          }
        }
        relaxedUntil = std::max(relaxedUntil, now + hold);
      } else if (relaxed && now >= relaxedUntil && repeating == 0) {
        relaxed = false;
        returnedAt = now;
        g_swapChain->setMaxFrameLatency(1);
        if (g_lowRelaxations <= 5) {
          REXLOG_INFO("video: Input Latency Low: frames no longer repeat, no frame queued again");
        }
      }
      auto start = std::chrono::steady_clock::now();
      g_swapChain->wait();
      bool timedOut = std::chrono::steady_clock::now() - start > std::chrono::milliseconds(90);
      timeouts = timedOut ? timeouts + 1 : 0;
      if (timeouts == 3) {
        static bool logged = false;
        if (!logged) {
          logged = true;
          REXLOG_WARN("video: the waits for the screen time out (a hidden window?): Input Latency Low "
                      "paused for 5 s at a time while they do");
        }
        timeouts = 0;
        pausedUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      }
    }

    // The frame's picture into a swapchain image: index and the semaphore
    // its acquire signals.
    auto blit = [&](RenderCommandList* l, uint32_t index) {
      g_swapChainFramebuffers.resize(g_swapChain->getTextureCount());
      RenderTexture* target = g_swapChain->getTexture(index);
      auto& framebuffer = g_swapChainFramebuffers[index];
      if (!framebuffer) {
        const RenderTexture* attachments[] = {target};
        RenderFramebufferDesc desc;
        desc.colorAttachments = attachments;
        desc.colorAttachmentsCount = 1;
        framebuffer = g_device->createFramebuffer(desc);
      }
      l->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(target, RenderTextureLayout::COLOR_WRITE));
      l->setFramebuffer(framebuffer.get());
      l->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));

      if (sourceDescriptor) {
        // Fit the guest picture into the window at the chosen aspect ratio
        // (src/aspect.cpp: the game draws for it, into its 16:9 targets); a
        // movie at its own.
        float windowWidth = float(g_swapChain->getWidth());
        float windowHeight = float(g_swapChain->getHeight());
        ng2::aspect::SetWindowShape(g_swapChain->getWidth(), g_swapChain->getHeight());
        float width = std::floor(std::min(windowWidth, windowHeight * aspect));
        float height = std::floor(std::min(windowHeight, windowWidth / aspect));
        RenderViewport viewport((windowWidth - width) / 2, (windowHeight - height) / 2, width,
                                height);
        RenderRect scissor(0, 0, int32_t(windowWidth), int32_t(windowHeight));
        uint32_t constants[3] = {sourceDescriptor, SAMPLER_LINEAR_CLAMP, gamma};
        l->setGraphicsPipelineLayout(g_pipelineLayout.get());
        l->setPipeline(g_presentPipeline.get());
        l->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
        l->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
        l->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
        l->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);
        l->setGraphicsPushConstants(0, constants, 0, sizeof(constants));
        l->setViewports(&viewport, 1);
        l->setScissors(&scissor, 1);
        l->drawInstanced(3, 1, 0, 0);
      }
      l->barriers(RenderBarrierStage::GRAPHICS,
                     RenderTextureBarrier(target, RenderTextureLayout::PRESENT));
    };

    // Images in the order they are shown: the pictures between, then the
    // frame's (drawn first: its commands have run).
    constexpr uint32_t kMaxPictures = kInterpMaxPictures;
    // A slow screen's partial frame: the last picture again, not shown (the
    // last full frame's pictures are still being shown by the clock).
    // Too few images for a picture besides the frame's own: shown alone, as
    // with interpolation off (an acquire would wait for an image that only a
    // present behind it frees).
    const uint32_t spare = SpareImages();
    const bool starved = spare < 2;
    if (interp && starved) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        REXLOG_WARN("video: interp: the swapchain has {} images ({} its least): no pictures between",
                    g_swapChain->getTextureCount(), g_swapChain->getMinTextureCount());
      }
    }
    const bool hide = !starved && interp && interp->partial && (interp->slow || interp->paced);
    const bool ownHidden = hide || (!starved && interp && interp->paced && !interp->ownShown);
    // The pictures waiting for their time hurried out only when an acquire
    // could otherwise wait for an image that only their present frees: the
    // images not yet presented and those taken this frame near all the
    // swapchain's, less the display's own (hurried each frame, the last
    // frame's last picture came early: a stutter in a turn). Only in
    // exclusive full screen, where the display holds no more than that:
    // elsewhere a compositor (gamescope, DWM) keeps images of its own, and
    // an image taken whose release waited for a present behind its own
    // drawing stood the GPU still (the Steam Deck: 40 s) - there each frame
    // hurries the last one's out before its first acquire, as before.
    const bool exclusive = g_swapChain->isFullScreenExclusive();
    auto hurryIfShort = [&](uint32_t taken) {
      if (hide || !InterpEnabled()) {
        return;
      }
      if (exclusive ? g_presenter.Pending() + taken + 3 > g_swapChain->getTextureCount() : taken == 0) {
        g_presenter.Hurry();
      }
    };
    uint32_t indices[kMaxPictures] = {};
    uint32_t pictures = 0;
    uint32_t frameIndex = 0;
    // Each picture between in a list and submit of its own, before the
    // frame's (so its drawing's end is its own, and what the next frame finds
    // is the frame's): the frame's commands again, its draws part of the way
    // from the last frame's. Recorded first, one after another (plume keeps
    // each texture's and buffer's layout and stages on the object itself:
    // lists recorded at once raced on them, for wrong barriers), before
    // waiting for the presenter to have room.
    size_t recorded = 0;
    if (g_swapChainValid && interp) {
      // (with the frame's own image, no more than the swapchain spares)
      recorded = std::min<size_t>({interp->phases.size(), kMaxPictures - 1, size_t(spare > 1 ? spare - 1 : 0)});
      int64_t replayStart = SteadyNs();
      auto record = [&](size_t k) {
        layouts.BeforeRecord();
        if (k == 0) {
          SaveCarried(g_interpLists[frame][k].get(), carried);
          layouts.BackToStart(g_interpLists[frame][k].get());  // (SaveCarried leaves them as the frame ends)
        }
        g_replayConstants = &interp->phases[k];
        g_replayCopies = k < interp->copies.size() ? &interp->copies[k] : nullptr;
        frameLog->RunKeep(g_interpLists[frame][k].get());
        g_replayConstants = nullptr;
        g_replayCopies = nullptr;
        if (k == 0) {
          layouts.NoteEnd();
        }
      };
      for (size_t k = 0; k < recorded; k++) {
        if (!g_interpAcquire[frame][k]) {
          g_interpAcquire[frame][k] = g_device->createCommandSemaphore();
          g_interpLists[frame][k] = g_queue->createCommandList();
          g_interpFences[frame][k].fence = g_device->createCommandFence();
        }
        g_interpFences[frame][k].Wait();  // (its last drawing done: the list is free)
        g_interpLists[frame][k]->begin();
      }
      for (size_t k = 0; k < recorded; k++) {
        record(k);
      }
      int64_t took = SteadyNs() - replayStart;
      parts.record += took;
      replayNs += took;
      replayMaxNs = std::max(replayMaxNs, took);
    }
    // At most a frame's pictures waiting to be shown (the swapchain's images
    // last; plume's acquire lets go of the swapchain between short waits).
    if ((interp || InterpEnabled()) && !hide) {
      int64_t roomStart = SteadyNs();
      g_presenter.WaitFor(3);
      int64_t took = SteadyNs() - roomStart;
      parts.room += took;
      roomWaitNs += took;
      roomWaitMaxNs = std::max(roomWaitMaxNs, took);
      if (g_presenter.failed.exchange(false)) {
        g_swapChainValid = false;
      }
    }
    // The game's frame interval, from these presents (the pictures between are spread over it).
    static int64_t lastPresentNs = 0;
    static double intervalNs = 1e9 / 60.0;
    int64_t presentNs = SteadyNs();
    if (lastPresentNs) {
      double gap = double(presentNs - lastPresentNs);
      if (gap > 5e6 && gap < 70e6) {
        intervalNs += (gap - intervalNs) * 0.1;
      }
    }
    lastPresentNs = presentNs;
    auto growSemaphores = [] {
      while (g_renderSemaphores.size() < g_swapChain->getTextureCount()) {
        g_renderSemaphores.push_back(g_device->createCommandSemaphore());
      }
    };
    RenderCommandSemaphore* signalSemaphores[kMaxPictures] = {};
    PictureFence* doneFences[kMaxPictures] = {};
    if (g_swapChainValid && interp) {
      if (prologue && PrologueMode() == 2) {
        // (the frame's submit runs it too: copies of the same memory)
        const RenderCommandList* lists[] = {g_prologueLists[frame].get()};
        g_queue->executeCommandLists(lists, 1);
      }
      size_t k = 0;
      for (; k < recorded; k++) {
        auto& semaphore = g_interpAcquire[frame][k];
        auto& interpList = g_interpLists[frame][k];
        auto& fence = g_interpFences[frame][k];
        uint32_t index = 0;
        int64_t acquireStart = SteadyNs();
        hurryIfShort(pictures);
        if (!g_swapChain->acquireTexture(semaphore.get(), &index)) {
          break;
        }
        NoteAcquire(SteadyNs() - acquireStart);
        parts.acquire += SteadyNs() - acquireStart;
        growSemaphores();
        layouts.AtEnd();  // (the lists recorded since moved plume's idea of them on)
        blit(interpList.get(), index);
        RestoreCarried(interpList.get(), carried);  // (the next picture, or the frame, starts from them)
        layouts.BackToStart(interpList.get());
        interpList->end();
        RenderCommandSemaphore* wait = semaphore.get();
        RenderCommandSemaphore* signal = g_renderSemaphores[index].get();
        const RenderCommandList* lists[] = {interpList.get()};
        g_queue->executeCommandLists(lists, 1, &wait, 1, &signal, 1, fence.fence.get());
        fence.Submitted();
        indices[pictures] = index;
        signalSemaphores[pictures] = signal;
        doneFences[pictures] = &fence;
        pictures++;
      }
      for (; k < recorded; k++) {
        g_interpLists[frame][k]->end();  // (recorded, not shown: no image)
      }
      layouts.AtEnd();
    } else if (recorded) {
      for (size_t k = 0; k < recorded; k++) {
        g_interpLists[frame][k]->end();
      }
    }
    if (g_swapChainValid && !ownHidden) {
      hurryIfShort(pictures);
      int64_t acquireStart = SteadyNs();
      g_swapChainValid = g_swapChain->acquireTexture(g_acquireSemaphores[frame].get(), &frameIndex);
      if (InterpEnabled()) {
        NoteAcquire(SteadyNs() - acquireStart);
      }
      g_blockedNs = SteadyNs() - acquireStart;  // (the present's wait is added below)
      parts.acquire += SteadyNs() - acquireStart;
    }
    growSemaphores();
    if (g_swapChainValid) {
      WriteGpuMark(list, frame, presentMark);
      if (!ownHidden) {
        blit(list, frameIndex);
      }
    }
    WriteGpuMark(list, frame, endMark);

    list->end();
    if (g_swapChainValid && !hide && (!ownHidden || pictures)) {
      static int64_t submitNs = 0, submitMaxNs = 0;
      if (!ownHidden) {
        RenderCommandSemaphore* waits[] = {g_acquireSemaphores[frame].get()};
        RenderCommandSemaphore* signals[] = {g_renderSemaphores[frameIndex].get()};
        int64_t submitStart = SteadyNs();
        submit(list, waits, 1, signals, 1);
        int64_t took = SteadyNs() - submitStart;
        parts.submit += took;
        submitNs += took;
        submitMaxNs = std::max(submitMaxNs, took);
        indices[pictures] = frameIndex;
        signalSemaphores[pictures] = signals[0];
        pictures++;
      } else {
        submit(list, nullptr, 0, nullptr, 0);  // (its pictures between only)
      }
      FrameSubmitted();
      static uint64_t presents = 0;
      if (InterpEnabled() && interp && interp->paced && !starved) {
        // Each at its time on the display's grid.
        int64_t readyNs = SteadyNs();
        int64_t lag = readyNs - interp->presentNs;
        int64_t kept = g_interpReadyLagNs.load();
        g_interpReadyLagNs = lag > kept ? lag : kept - (kept - lag) / 64;  // (the slowest, let go slowly)
        static uint64_t late = 0, shownPaced = 0;
        for (uint32_t i = 0; i < pictures; i++) {
          int64_t at = i < interp->times.size() ? interp->times[i] : readyNs;
          late += at < readyNs;
          shownPaced++;
          g_presenter.Push({indices[i], signalSemaphores[i], at, presents, doneFences[i], inputNs,
                            uint8_t((i == 0 ? 1 : 0) | (i + 1 == pictures ? 2 : 0)), true});
        }
        if (shownPaced >= 6000) {
          REXLOG_INFO("video: interp: paced by the display ({:.2f} Hz, a picture each {} refresh): {} of {} "
                      "pictures ready after their time; made {:.1f} ms after the present at most lately",
                      1e9 / double(std::max<int64_t>(InterpRefreshNs(), 1)), InterpDivider(), late, shownPaced,
                      double(g_interpReadyLagNs.load()) / 1e6);
          late = shownPaced = 0;
        }
      } else if (InterpEnabled() && interp && !starved) {
        // One at a time, a part of the frame interval apart (Presenter); a
        // slow screen's evenly over the time since its last full frame, the
        // first at once.
        int64_t firstNs = SteadyNs();
        const bool timed = interp && interp->slow && !interp->partial && pictures > 1;
        // A slow screen's: one after another a spacing apart, on from the
        // last frame's (the game's frames come unevenly, 26 then 41 ms), the
        // first no sooner than now.
        static int64_t slowNext = 0;
        int64_t spacing = 0;
        if (timed) {
          spacing = interp->spacingNs ? interp->spacingNs : interp->spanNs / pictures;
          if (slowNext > firstNs + 4 * interp->spanNs || slowNext + 4 * interp->spanNs < firstNs) {
            slowNext = firstNs;  // (a new screen, or far off)
          }
          while (slowNext < firstNs) {
            slowNext += spacing;
          }
        }
        for (uint32_t i = 0; i < pictures; i++) {
          int64_t at = timed ? slowNext : firstNs + int64_t(intervalNs * (i + 1) / pictures);
          if (timed) {
            slowNext += spacing;
          }
          g_presenter.Push({indices[i], signalSemaphores[i], at, presents, doneFences[i], inputNs,
                            uint8_t((i == 0 ? 1 : 0) | (i + 1 == pictures ? 2 : 0)), timed});
        }
      } else {
        // (no pictures: autopause, a cut. Shown at once as with interpolation
        // off, after any still waiting: through the presenter it came a whole
        // frame later, and where the GPU took 10 ms a frame the game ran at 51)
        if (InterpEnabled()) {
          g_presenter.WaitFor(0);
        }
        EvenOutPresent();
        int64_t presentStart = SteadyNs();
        g_swapChainValid = g_swapChain->present(indices[0], &signalSemaphores[0], 1);
        if (g_swapChainValid) {
          g_picturesPresented.fetch_add(1, std::memory_order_relaxed);
        }
        // A full FIFO queue blocks the acquire (AMD, Mesa) or the present
        // (NVIDIA): either counts.
        if (g_blockedNs + SteadyNs() - presentStart >= 1'500'000) {
          g_acquireFreeRun.store(0, std::memory_order_relaxed);
        } else if (g_acquireFreeRun.load(std::memory_order_relaxed) < 1000) {
          g_acquireFreeRun.fetch_add(1, std::memory_order_relaxed);
        }
        // (timed at the present itself, on the thread that makes it)
        ReportPresentGaps(std::chrono::steady_clock::now());
        if (inputNs) {
          NotePadToPresent(SteadyNs() - inputNs, 0);
          NotePadToPresent(SteadyNs() - inputNs, 1);
        }
      }
      if (++presents == 600 && !InterpEnabled()) {
        presents = 0;  // (the stats below are interpolation's)
      }
      if (presents == 600) {
        uint64_t shown = g_presenter.presented.exchange(0);  // (once: the order arguments are read in isn't set)
        REXLOG_INFO("video: interp: {} pictures shown for 600 frames; present {:.2f} ms each (max "
                    "{:.1f}), the frame's submit {:.2f} ms (max {:.1f})",
                    shown,
                    double(g_presenter.presentNs.exchange(0)) / 1e6 / double(std::max<uint64_t>(shown, 1)),
                    double(g_presenter.presentMaxNs.exchange(0)) / 1e6, double(submitNs) / 600e6,
                    double(submitMaxNs) / 1e6);
        submitNs = submitMaxNs = 0;
        presents = 0;
        REXLOG_INFO("video: interp: render thread ms a frame: waiting for the build {:.2f} (max {:.1f}), "
                    "recording the pictures {:.2f} (max {:.1f}), waiting for the presenter {:.2f} (max {:.1f})",
                    double(jobWaitNs) / 600e6, double(jobWaitMaxNs) / 1e6, double(replayNs) / 600e6,
                    double(replayMaxNs) / 1e6, double(roomWaitNs) / 600e6, double(roomWaitMaxNs) / 1e6);
        jobWaitNs = jobWaitMaxNs = replayNs = replayMaxNs = roomWaitNs = roomWaitMaxNs = 0;
      }
      ReportLatency();
      if (inputNs && !InterpEnabled()) {
        if (uint64_t id = g_swapChain->getPresentId()) {
          std::lock_guard lock(g_taggedMutex);
          g_tagged[id % 64] = {id, inputNs, SteadyNs()};
        }
      }
    } else {
      submit(list, nullptr, 0, nullptr, 0);
      FrameSubmitted();
    }
  });
  g_commandListPending[frame] = true;
  g_submitTicket[frame] = ++g_framesQueued;
  if (RenderThreadEnabled()) {
    g_renderThread.Send();
  }
  g_frame = (g_frame + 1) % FrameSlots();
  g_frameCount++;
  // The frame before this one waited for here, when it read back resolves:
  // they are in guest memory before the game's next frame starts, every frame
  // alike. Applied only at the slot's next use (that frame's first draw, which
  // the game's logic may run before or after), the game read last frame's
  // values some frames and older ones others: the brightness it reads back
  // wavered (the shop's door and paper panels flickering, #21 and others), and
  // so did what it reads for objects' animation (chests, #87). The GPU is one
  // frame behind at most then, where it was two.
  // Not with frame interpolation (three slots): the previous frame's job there
  // includes its pictures and their room in the presenter, and waiting for it
  // cost the game frames (54.8 fps at worst against 60 in a Chapter 1 fight at
  // 120 Hz). NG2_READBACK_SYNC=0 leaves them to the slot's next use, as before
  // 0.5.65; =1 waits with interpolation too.
  static const int readbackSync = [] {
    const char* v = std::getenv("NG2_READBACK_SYNC");
    return v && *v == '0' ? 0 : v && *v == '1' ? 2 : 1;
  }();
  if (readbackSync == 2 || (readbackSync == 1 && !InterpEnabled())) {
    uint32_t slots = FrameSlots();
    uint32_t previous = (g_frame + slots - 2) % slots;  // (g_frame is the next frame's slot now)
    if (g_commandListPending[previous] && ResolveReadbacksQueued(previous)) {
      WaitForFrame(previous, false);  // (not toward a drain request)
    }
  }
}

// Setup -----------------------------------------------------------------------

bool GetRenderWindow(RenderWindow& renderWindow) {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!window) {
    REXLOG_ERROR("video: no SDL window");
    return false;
  }
  if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) {
    if (mode->refresh_rate > 20.0f) {
      g_modeRefreshNs = int64_t(1e9 / double(mode->refresh_rate));
      REXLOG_INFO("video: display mode {:.2f} Hz", mode->refresh_rate);
    }
  }
  // NG2_INTERP_REFRESH=<Hz>: interpolation paced as for that display (tests).
  if (const char* v = std::getenv("NG2_INTERP_REFRESH"); v && std::atof(v) > 20.0) {
    g_modeRefreshNs = int64_t(1e9 / std::atof(v));
  }
#if defined(__linux__)
  SDL_PropertiesID props = SDL_GetWindowProperties(window);
  if (auto* surface = static_cast<wl_surface*>(
          SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr))) {
    renderWindow.waylandSurface = surface;
    renderWindow.waylandDisplay = static_cast<wl_display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    renderWindow.userData = window;
    renderWindow.getSize = [](void* userData, uint32_t* width, uint32_t* height) {
      int w = 0, h = 0;
      SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(userData), &w, &h);
      *width = uint32_t(w);
      *height = uint32_t(h);
    };
    REXLOG_INFO("video: Wayland window");
    return true;
  }
  renderWindow.display = static_cast<Display*>(
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
  renderWindow.window =
      static_cast<Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (!renderWindow.display || !renderWindow.window) {
    REXLOG_ERROR("video: unsupported SDL video driver '{}'",
                 SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
    return false;
  }
  REXLOG_INFO("video: X11 window");
  return true;
#elif defined(_WIN32)
  renderWindow = static_cast<HWND>(SDL_GetPointerProperty(
      SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (!renderWindow) {
    REXLOG_ERROR("video: the SDL window has no HWND");
    return false;
  }
  REXLOG_INFO("video: Win32 window");
  return true;
#else
  REXLOG_ERROR("video: unsupported platform");
  return false;
#endif
}

std::unique_ptr<RenderShader> CreateShader(const uint8_t* data, size_t size) {
  return g_device->createShader(data, size, "main", RenderShaderFormat::SPIRV);
}

void CreatePipelines() {
  // Texture heap bound to sets 0-2 (2D, 3D, cube views), sampler heap to set
  // 3, push constants: three buffer addresses (vertex, pixel, shared).
  RenderDescriptorSetBuilder textureSetBuilder;
  textureSetBuilder.begin();
  textureSetBuilder.addTexture(0, TEXTURE_DESCRIPTOR_COUNT);
  textureSetBuilder.end(true, TEXTURE_DESCRIPTOR_COUNT);
  g_textureDescriptorSet = textureSetBuilder.create(g_device.get());

  RenderDescriptorSetBuilder samplerSetBuilder;
  samplerSetBuilder.begin();
  samplerSetBuilder.addSampler(0, SAMPLER_DESCRIPTOR_COUNT);
  samplerSetBuilder.end(true, SAMPLER_DESCRIPTOR_COUNT);
  g_samplerDescriptorSet = samplerSetBuilder.create(g_device.get());

  RenderPipelineLayoutBuilder layoutBuilder;
  layoutBuilder.begin(false, true);
  layoutBuilder.addDescriptorSet(textureSetBuilder);
  layoutBuilder.addDescriptorSet(textureSetBuilder);
  layoutBuilder.addDescriptorSet(textureSetBuilder);
  layoutBuilder.addDescriptorSet(samplerSetBuilder);
  layoutBuilder.addPushConstant(0, 4, PUSH_CONSTANT_SIZE,
                                RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
  layoutBuilder.end();
  g_pipelineLayout = layoutBuilder.create(g_device.get());

  // Blank textures for unbound slots.
  for (uint32_t i = 0; i < DESCRIPTOR_FIRST_FREE; i++) {
    RenderTextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.format = RenderFormat::R8_UNORM;
    RenderTextureViewDesc viewDesc;
    viewDesc.format = desc.format;
    // NG2_DEBUG_WHITE_TEXTURES=1: unbound textures read as white, to see
    // geometry before textures work.
    static const bool white = [] {
      const char* v = std::getenv("NG2_DEBUG_WHITE_TEXTURES");
      return v && v[0] == '1';
    }();
    RenderSwizzle blank = white ? RenderSwizzle::ONE : RenderSwizzle::ZERO;
    viewDesc.componentMapping = RenderComponentMapping(blank, blank, blank, blank);
    viewDesc.mipLevels = 1;
    if (i == DESCRIPTOR_NULL_3D) {
      desc.dimension = RenderTextureDimension::TEXTURE_3D;
      desc.arraySize = 1;
      viewDesc.dimension = RenderTextureViewDimension::TEXTURE_3D;
    } else if (i == DESCRIPTOR_NULL_CUBE) {
      desc.dimension = RenderTextureDimension::TEXTURE_2D;
      desc.arraySize = 6;
      desc.flags = RenderTextureFlag::CUBE;
      viewDesc.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
    } else {
      desc.dimension = RenderTextureDimension::TEXTURE_2D;
      desc.arraySize = 1;
      viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
    }
    g_nullTextures[i].texture = g_device->createTexture(desc);
    g_nullTextures[i].layout = RenderTextureLayout::UNKNOWN;
    g_nullTexturesReady = false;
    if (!g_nullTextures[i].texture) {
      REXLOG_ERROR("video: can't make a 1-pixel texture: no video memory");
      std::_Exit(1);
    }
    g_nullTextureViews[i] = g_nullTextures[i].texture->createTextureView(viewDesc);
    g_textureDescriptorSet->setTexture(i, g_nullTextures[i].texture.get(),
                                       RenderTextureLayout::SHADER_READ,
                                       g_nullTextureViews[i].get());
  }

  RenderSamplerDesc samplerDesc;
  samplerDesc.minFilter = RenderFilter::LINEAR;
  samplerDesc.magFilter = RenderFilter::LINEAR;
  samplerDesc.addressU = RenderTextureAddressMode::CLAMP;
  samplerDesc.addressV = RenderTextureAddressMode::CLAMP;
  samplerDesc.addressW = RenderTextureAddressMode::CLAMP;
  g_linearClampSampler = g_device->createSampler(samplerDesc);
  g_samplerDescriptorSet->setSampler(SAMPLER_LINEAR_CLAMP, g_linearClampSampler.get());

  g_fullscreenShader = CreateShader(g_fullscreen_vs, g_fullscreen_vs_size);
  g_presentShader = CreateShader(g_present_ps, g_present_ps_size);
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_pipelineLayout.get();
  desc.vertexShader = g_fullscreenShader.get();
  desc.pixelShader = g_presentShader.get();
  desc.renderTargetFormat[0] = BACKBUFFER_FORMAT;
  desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
  desc.renderTargetCount = 1;
  g_presentPipeline = g_device->createGraphicsPipeline(desc);

  g_rectListShader = CreateShader(g_rect_list_gs, g_rect_list_gs_size);
  g_copyShader = CreateShader(g_copy_ps, g_copy_ps_size);
  g_copy7e3Shader = CreateShader(g_copy_7e3_ps, g_copy_7e3_ps_size);
  g_copy7e3MsShader = CreateShader(g_copy_7e3_ms_ps, g_copy_7e3_ms_ps_size);
  g_scaleShader = CreateShader(g_scale_ps, g_scale_ps_size);
  g_debugShader = CreateShader(g_debug_ps, g_debug_ps_size);
  g_depthCopyShader = CreateShader(g_depth_copy_ps, g_depth_copy_ps_size);
  g_depthCopyMsShader = CreateShader(g_depth_copy_ms_ps, g_depth_copy_ms_ps_size);

  g_guestMemory = g_device->createBuffer(RenderBufferDesc::DefaultBuffer(
      GUEST_MEMORY_SIZE, RenderBufferFlag::CONSTANT | RenderBufferFlag::STORAGE));
  if (!g_guestMemory) {
    REXLOG_ERROR("video: no room on the GPU for the game's memory ({} MB)", GUEST_MEMORY_SIZE >> 20);
    std::_Exit(1);
  }
  g_guestMemoryAddress = g_guestMemory->getDeviceAddress();
  g_guestMemoryShadow = static_cast<uint8_t*>(std::calloc(GUEST_MEMORY_SIZE, 1));
}

}  // namespace

bool IsNativeRenderer() {
  // The native renderer unless NG2_RENDERER=xenos (the SDK's GPU emulation,
  // to compare with).
  static const bool native = [] {
    const char* renderer = std::getenv("NG2_RENDERER");
    return !renderer || std::strcmp(renderer, "xenos") != 0;
  }();
  return native;
}

// The display clock (Frame Pacing Auto, native_gpu.cpp) -------------------
//
// A thread looks every millisecond whether the next present has reached the
// screen (VK_KHR_present_wait, without waiting: the swapchain's calls are
// serialized) and notes when it did: a refresh of the display, to about a
// millisecond. From those
// the display's period (as many refreshes as our frames take) and its last
// refresh, once steady for two seconds. It counts the frames that stayed up
// an extra refresh too (a repeat, the drift between the guest's vblank timer
// and the display, or a slow frame).
// (below: the rate of the display the window is on, as SDL reports it)
extern std::atomic<double> g_displayModeHz;
void RequestDisplayRate();

namespace {
std::atomic<int64_t> g_displayRefreshNs{0}, g_displayPeriodNs{0};
std::atomic<bool> g_displaySteady{false}, g_displayRun{false};
// (left to run at exit if the device isn't destroyed, as the other workers)
std::thread* g_displayThread = nullptr;


bool LatencyLogOn() {
  static const bool on = [] {
    const char* v = std::getenv("NG2_LATENCY_LOG");
    return v && v[0] == '1';
  }();
  return on;
}

// (the display clock's thread)
void NoteInputToScreen(int64_t inputNs, int64_t presentNs, int64_t shownNs) {
  static double toPresent = 0, toScreen = 0, worst = 0, best = 1e9;
  static uint64_t count = 0;
  static int64_t logAt = 0;
  double screen = double(shownNs - inputNs) / 1e6;
  toPresent += double(presentNs - inputNs) / 1e6, toScreen += screen, count++;
  worst = std::max(worst, screen), best = std::min(best, screen);
  if (!logAt) {
    logAt = shownNs;
  }
  if (shownNs - logAt > 10'000'000'000 && count) {
    REXLOG_INFO("latency pad to present {:.1f} ms, pad to screen {:.1f} ms (best {:.1f}, worst "
                "{:.1f}; {} frames; present to screen {:.1f})",
                toPresent / double(count), toScreen / double(count), best, worst, count,
                (toScreen - toPresent) / double(count));
    toPresent = toScreen = worst = 0, best = 1e9, count = 0, logAt = shownNs;
  }
}

void DisplayClockThread() {
  uint64_t next = 0;
  int64_t last = 0, runStart = 0, logAt = 0;
  uint64_t runUnits = 0, uneven = 0, repeats = 0, frames = 0;
  uint64_t windowUnits = 0, windowUneven = 0;
  bool steady = false;
  int steadyChanges = 0;
  uint64_t secondRepeats = 0;
  int64_t secondStart = 0;
  double period = 1e9 / 60.0;
  // Refreshes a frame takes: 1 near 60 Hz; on a 120 or 240 Hz panel 2 or 4,
  // from SDL's rate, so frames are counted in the panel's own refreshes (the
  // gaps rounded to 60 Hz units read 61 Hz on a 120 Hz laptop panel, and Auto
  // never locked). 144-170 Hz, which 60 doesn't divide, stays at 1, as before.
  int every = 1;
  while (g_displayRun.load()) {
    // (the rate is asked for from the presents, every 5 s: see Present)
    if (double hz = g_displayModeHz.load(); hz > 0) {
      int n = std::clamp(int(hz / 60.0 + 0.5), 1, 4);
      if (n > 1 && std::abs(hz / n - 60.0) > 1.2) {
        n = 1;
      }
      if (n != every) {
        every = n;
        period = 1e9 / (60.0 * every);
        last = 0;  // (measured afresh, below)
        REXLOG_INFO("video: display clock counts {} refreshes a frame ({:.2f} Hz display)", every, hz);
      }
    }
    RenderSwapChain* swapChain = g_swapChain.get();
    uint64_t id = swapChain ? swapChain->getPresentId() : 0;
    if (!id) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    if (!next || next > id + 1) {
      next = id;
    }
    if (next > id) {
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      continue;
    }
    int shown = swapChain->pollPresent(next);
    if (shown <= 0) {
      if (shown < 0 || id - next > 4) {
        next = id + 1;  // an earlier swapchain's (or an error), or far behind: the next one
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    int64_t t = SteadyNs();
    {
      std::lock_guard lock(g_taggedMutex);
      TaggedPresent& tag = g_tagged[next % 64];
      if (tag.id == next && tag.inputNs) {
        NoteInputToScreen(tag.inputNs, tag.presentNs, t);
        tag.id = 0;
      }
    }
    next++;
    int64_t dt = t - last;
    if (!last || dt > 250'000'000) {
      runStart = t, runUnits = 0, uneven = 0;
      windowUnits = 0, windowUneven = 0, steady = false;
      secondRepeats = 0, secondStart = t;  // (a stall: not a second of repeats)
      g_repeatsLastSecond.store(0);
      g_displaySteady.store(false);
    } else if (dt > period / 4) {  // (another present shown at the same refresh: none)
      // (k refreshes; a frame not a whole number of frame periods is off time)
      uint64_t k = std::max<uint64_t>(1, uint64_t(double(dt) / period + 0.5));
      double residual = double(dt) - double(k) * period;
      // Off time: over a fifth of a refresh away, but at least 1.2 ms (shown
      // times come from a loop that polls every millisecond: at 240 Hz a fifth
      // is less than that).
      uint64_t off = std::abs(residual) > std::max(period * 0.2, 1.2e6) || k % every ? 1 : 0;
      uint64_t extra = k >= uint64_t(every) ? k / every - 1 : 0;
      uneven += off, windowUneven += off;
      repeats += extra, frames++;
      secondRepeats += extra;
      runUnits += k, windowUnits += k;
      if (runUnits >= 60u * every) {
        period = double(t - runStart) / double(runUnits);
      }
      // Steady: fewer than 1 in 10 refreshes off their expected time, judged
      // over the first 5 s, then over each 5 s; once steady, lost only past
      // 1 in 4 (a busy GPU makes the reports jitter, #46: judged over the
      // whole run, steady was lost for good after a while, and Frame Pacing
      // Auto went back to the guest's own timer, which drifts from the display;
      // with 1 in 20 to start, a display reporting 4-5% off took minutes).
      bool was = steady;
      if (runUnits < 300) {
        steady = runUnits >= 120 && uneven * 10 < runUnits;
      } else if (windowUnits >= 300) {
        steady = steady ? windowUneven * 4 <= windowUnits : windowUneven * 10 < windowUnits;
        if (steady != was && steadyChanges < 20) {
          steadyChanges++;
          REXLOG_INFO("video: display clock {} ({} of {} refreshes off time)",
                      steady ? "steady again" : "no longer steady", windowUneven, windowUnits);
        }
        windowUnits = 0, windowUneven = 0;
      }
      g_displayPeriodNs.store(int64_t(period));  // (a refresh: the lock takes every few)
      g_displayRefreshNs.store(t);
      g_displaySteady.store(steady);
    }
    last = t;
    if (t - secondStart > 1'000'000'000) {
      g_repeatsLastSecond.store(secondStart ? int(secondRepeats) : 0);
      secondRepeats = 0, secondStart = t;
    }
    if (t - logAt > 10'000'000'000) {
      if (logAt) {
        int relaxations = g_lowRelaxations.load();
        uint32_t noTick = g_framesNoTick.exchange(0), manyTicks = g_framesManyTicks.exchange(0);
        REXLOG_INFO("video: display {:.3f} Hz{}, {} frames shown, {} stayed an extra refresh{}{}",
                    1e9 / period, g_displaySteady.load() ? " (steady)" : "", frames, repeats,
                    relaxations ? fmt::format(", Low queued a frame {} times", relaxations) : "",
                    noTick || manyTicks
                        ? fmt::format("; game frames with no tick {}, with 2 or more {}", noTick, manyTicks)
                        : "");
      }
      logAt = t, frames = 0, repeats = 0;
    }
  }
}
}  // namespace

void NoteInputRead() {
  if (LatencyLogOn()) {
    g_inputNs.store(SteadyNs());
  }
}

void NoteFrameTaken() {
  if (LatencyLogOn()) {
    g_handedInputNs.store(g_inputNs.load());
  }
}

// The Windows display clock's report (win_display.cpp), where there is no
// VK_KHR_present_wait: the monitor's vblanks, not our frames, so no counts.
void ReportWindowsVblank(int64_t vblankNs, int64_t periodNs, bool steady) {
  g_displayPeriodNs.store(periodNs);
  g_displayRefreshNs.store(vblankNs);
  g_displaySteady.store(steady && periodNs > 0);
}

// The display's refresh: measured (VK_KHR_present_wait) when steady, else
// the display mode's; 0 not known.
int64_t InterpRefreshNs() {
  int64_t last = 0, period = 0;
  if (DisplayRefresh(&last, &period)) {
    return period;
  }
  return g_modeRefreshNs.load();
}

int64_t ModeRefreshNs() {
  return g_modeRefreshNs.load();
}

InterpUpload AllocateInterpUpload(uint32_t slot, uint64_t size, uint64_t alignment) {
  Upload upload = AllocateFrom(g_interpUploads[slot], size, alignment);
  return {upload.buffer, upload.offset, upload.data, upload.address};
}

bool UsesRenderThread() {
  return RenderThreadEnabled();
}

bool DisplayRefresh(int64_t* lastNs, int64_t* periodNs) {
  int64_t last = g_displayRefreshNs.load(), period = g_displayPeriodNs.load();
  if (!g_displaySteady.load() || !last || !period || SteadyNs() - last > 250'000'000) {
    return false;
  }
  *lastNs = last, *periodNs = period;
  return true;
}

// Under gamescope (the Steam Deck's Game Mode, or a game run through it):
// it reports a frame reaching the screen at uneven times, a few ms either way,
// so the two settings that go by those reports misbehave there. Input Latency
// Low (each frame waits until the last is shown) made the frames come in
// pairs, 6 then 28 ms apart, and Frame Pacing Auto (the guest's vblanks on the
// reported refreshes) made them uneven: there they act as Standard and Timer.
bool DrainRequested() {
  return g_drainRequest.exchange(false, std::memory_order_relaxed);
}

// The rate of the display the window is on, as SDL reports it (0 unknown):
// sampled on the UI thread (SDL's display queries belong there), asked for
// every 5 s from the vsync thread (the window may move to another display).
rex::ui::WindowedAppContext* g_appContext = nullptr;
std::atomic<double> g_displayModeHz{0.0};
std::atomic<bool> g_displayModeQueued{false};
std::atomic<bool> g_presentDrainCheck{false};  // off before the swapchain goes

void NoteDrawStack(const std::string& stack) {
  g_drawStack = stack;
}

void SetAppContext(rex::ui::WindowedAppContext* context) {
  g_appContext = context;
}

void RequestDisplayRate() {
  if (!g_appContext || g_displayModeQueued.exchange(true)) {
    return;
  }
  g_appContext->CallInUIThreadDeferred([] {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* window = (windows && count > 0) ? windows[0] : nullptr;
    SDL_free(windows);
    const SDL_DisplayMode* mode =
        window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)) : nullptr;
    g_displayModeHz.store(mode ? double(mode->refresh_rate) : 0.0);
    g_displayModeQueued.store(false);
  });
}

bool PresentQueueDrained() {
  // (FIFO as created, not Mailbox, which never blocks; not Low, which empties
  // the queue on purpose; a display near 60 Hz: a faster one or a variable
  // one never holds a frame at 60 fps, so there's nothing to refill)
  if (!g_presentDrainCheck.load() || !REXCVAR_GET(vsync) || REXCVAR_GET(input_latency) == 1 ||
      UnderGamescope() || !g_swapChain->isVsyncEnabled() ||
      g_acquireFreeRun.load(std::memory_order_relaxed) < 30) {
    return false;
  }
  static int64_t askedNs = 0;
  int64_t now = SteadyNs();
  if (!askedNs || now - askedNs > 5'000'000'000) {
    askedNs = now;
    RequestDisplayRate();
  }
  double rate = g_displayModeHz.load();
  return rate >= 59.0 && rate <= 61.0;
}

bool UnderGamescope() {
  static const bool under = [] {
    const char* v = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
    bool yes = v && v[0];
    if (yes) {
      REXLOG_INFO("video: under gamescope: Input Latency Low and Frame Pacing Auto act as "
                  "Standard and Timer (Auto not with NG2_TEST_GAMESCOPE_AUTO=1)");
    }
    return yes;
  }();
  return under;
}

int64_t DisplayClockNow() {
  return SteadyNs();
}

bool CreateHostDevice() {
  std::lock_guard lock(g_mutex);
  g_resolutionScale = uint32_t(std::clamp(REXCVAR_GET(render_scale), 1, 8));
  g_antialiasing = AntialiasingSetting();
  RenderWindow renderWindow{};
  if (!GetRenderWindow(renderWindow)) {
    return false;
  }

  g_interface = CreateVulkanInterface();
  if (!g_interface) {
    REXLOG_ERROR("video: Vulkan is not available");
    return false;
  }
  g_device = g_interface->createDevice();
  if (!g_device) {
    REXLOG_ERROR("video: no usable Vulkan device");
    return false;
  }
  REXLOG_INFO("video: Vulkan device '{}'", g_device->getDescription().name);
  {
    const char* v = std::getenv("NG2_PROLOGUE");
    g_prologueMode = v ? std::clamp(std::atoi(v), 0, 2) : 0;
    if (g_prologueMode) {
      REXLOG_INFO("video: guest memory copies before the frame ({})",
                  g_prologueMode == 2 ? "own submit" : "in the frame's submit");
    }
  }

  g_queue = g_device->createCommandQueue(RenderCommandListType::DIRECT);
  for (size_t i = 0; i < NUM_FRAMES; i++) {
    g_commandLists[i] = g_queue->createCommandList();
    g_prologueLists[i] = g_queue->createCommandList();
    g_prologueOpen[i] = false;
    g_commandFences[i] = g_device->createCommandFence();
    g_acquireSemaphores[i] = g_device->createCommandSemaphore();
  }

  g_swapChain = g_queue->createSwapChain(renderWindow, SwapChainImages(), BACKBUFFER_FORMAT, 1);
  g_swapChain->setAcquireInShortWaits(InterpEnabled());  // (see SwitchInterp)
  if (UpdateExclusiveMode()) {
    g_swapChain->resize();  // (made again with it)
  }
  SetInterpPictureCap();
  g_presentDrainCheck.store(g_swapChain != nullptr);
  // NG2_TEST_DXGI_CLOCK=1 (Windows): the DXGI display clock even with present_wait.
  static const bool forceDxgi = [] {
#if defined(_WIN32)
    const char* v = std::getenv("NG2_TEST_DXGI_CLOCK");
    return v && v[0] == '1';
#else
    return false;
#endif
  }();
  if (g_device->getCapabilities().presentWait && !forceDxgi) {
    g_displayRun.store(true);
    g_displayThread = new std::thread([] {
#if defined(__linux__)
      pthread_setname_np(pthread_self(), "Display Clock");
#endif
      DisplayClockThread();
    });
  }
#if defined(_WIN32)
  else {
    // (no present_wait, as on AMD's Windows driver: the monitor's vblanks from DXGI)
    g_displayRun.store(true);
    if (StartWindowsDisplayClock(renderWindow, &g_displayRun, SteadyNs, ReportWindowsVblank)) {
      REXLOG_INFO("video: display clock from the monitor's vblanks (DXGI)");
    } else {
      g_displayRun.store(false);
    }
  }
#endif
  SDL_AddEventWatch(WatchTraceKey, nullptr);
  // (whether the GPU can do Input Latency Low, not the setting: logged as both, #78)
  REXLOG_INFO("video: Input Latency Low supported (waits for the screen): {}; set to {}",
              g_device->getCapabilities().presentWait ? "yes" : "no (VK_KHR_present_wait missing)",
              REXCVAR_GET(input_latency) == 1 ? "Low" : "Standard");
  REXLOG_INFO("video: vertex shaders round toward zero: {}",
              RoundTowardZero()                          ? "yes"
              : g_device->getCapabilities().roundingModeRTZ ? "no (NG2_RTZ=0)"
                                                            : "no (the GPU can't)");
  // plume always returns an object; one that failed (no surface, no present
  // support, format not offered) has no images.
  if (!g_swapChain || g_swapChain->getTextureCount() == 0) {
    REXLOG_ERROR("video: cannot create the swapchain");
    return false;
  }
  g_swapChain->setVsyncEnabled(true);
  g_swapChainValid = !g_swapChain->needsResize();

  CreatePipelines();
  StartPipelineWorkers();
  MakeListedPipelines();
  return true;
}

void ShowMovieFrame(const uint8_t* const planes[3], const uint32_t pitches[3], uint32_t width,
                    uint32_t height) {
  if (!width || !height) {
    return;
  }
  // BT.601, limited range (WMV / VC-1), converted outside the lock.
  static thread_local std::vector<uint8_t> rgba;
  rgba.resize(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; y++) {
    const uint8_t* lumaRow = planes[0] + size_t(y) * pitches[0];
    const uint8_t* uRow = planes[1] + size_t(y / 2) * pitches[1];
    const uint8_t* vRow = planes[2] + size_t(y / 2) * pitches[2];
    uint8_t* out = rgba.data() + size_t(y) * width * 4;
    for (uint32_t x = 0; x < width; x++) {
      float luma = 1.164f * (float(lumaRow[x]) - 16.0f);
      float u = float(uRow[x / 2]) - 128.0f, v = float(vRow[x / 2]) - 128.0f;
      auto clamp8 = [](float f) { return uint8_t(std::clamp(f + 0.5f, 0.0f, 255.0f)); };
      out[x * 4 + 0] = clamp8(luma + 1.596f * v);
      out[x * 4 + 1] = clamp8(luma - 0.392f * u - 0.813f * v);
      out[x * 4 + 2] = clamp8(luma + 2.017f * u);
      out[x * 4 + 3] = 255;
    }
  }
  std::lock_guard lock(g_movieMutex);
  g_movieWidth = width, g_movieHeight = height;
  g_movieRGBA.swap(rgba);
  g_movieSerial++;
  g_movieTime = std::chrono::steady_clock::now();
}

void KeepMovieShown() {
  std::lock_guard lock(g_movieMutex);
  g_movieTime = std::chrono::steady_clock::now();
}

void MarkFrameStart() {
  g_frameStartNs = std::chrono::steady_clock::now().time_since_epoch().count();
}

void SetPipelineListPath(const std::filesystem::path& path) {
  g_pipelineListPath = path;
}

void StopDrawWorker();

namespace {

// Resolves the CPU reads ------------------------------------------------------
// The game decides some things from pictures it resolves to small textures and
// then reads on the CPU: a lens flare's visibility is 3600 points drawn into a
// 3x3 target with additive blending (each adds where the scene's depth leaves
// it in view), resolved, and summed by the CPU (#65). Resolves only write host
// textures, so small resolve destinations are also copied into a readback
// buffer and written into guest memory, in the texture's own layout (tiling,
// endianness), once the GPU has finished the frame: at the wait for the frame
// slot, two frames later. At a render scale above 1 a guest texel covers
// scale x scale host texels: float ones are added (counts of points, one host
// pixel each), normalized ones averaged (pictures). The resolved texture's
// memory hash is refreshed so the GPU keeps reading the host texture.
// NG2_RESOLVE_READBACK=0 turns this off.
struct ResolveReadbackItem {
  uint32_t address = 0;  // guest physical, the texture's base
  xenos::xe_gpu_texture_fetch_t fetch{};
  TextureFormatInfo info;
  uint32_t width = 0, height = 0;  // guest texels
  uint32_t scale = 1;
  uint32_t rowWidth = 0;  // host texels a row of the readback spans
  RenderFormat format = RenderFormat::UNKNOWN;
  const RenderBuffer* buffer = nullptr;
  uint64_t offset = 0;  // into the buffer
  bool toGuest = true;    // written into guest memory (else only dumped)
  std::string dumpPath;   // F7 trace: the host texels written raw to this file
};
struct ResolveReadback {
  std::unique_ptr<RenderBuffer> buffer;
  std::vector<std::unique_ptr<RenderBuffer>> retired;  // outgrown this frame: still read by its copies
  uint64_t capacity = 0, used = 0;
  std::vector<ResolveReadbackItem> items;
};
ResolveReadback g_resolveReadbacks[NUM_FRAMES];
// Resolves up to 64x64 are read back (the brightness chain, the lens flare's
// 3x3). Not by texel count: Chapter 9's opening resolves an 80x45 picture of
// the screen, and with it read back the game waits on it forever (#73).
constexpr uint32_t kReadbackSide = 64;

bool ResolveReadbackOn() {
  static const bool on = !TestSwitch("NG2_RESOLVE_READBACK", "0");
  return on;
}

float HalfToFloat(uint16_t h) {
  uint32_t sign = uint32_t(h & 0x8000) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {  // subnormal
      exponent = 1;
      while (!(mantissa & 0x400)) {
        mantissa <<= 1;
        exponent--;
      }
      mantissa &= 0x3FF;
      bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000 | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint16_t FloatToHalf(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  uint32_t sign = (bits >> 16) & 0x8000;
  int32_t exponent = int32_t((bits >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFF;
  if (((bits >> 23) & 0xFF) == 0xFF) {  // inf or NaN
    return uint16_t(sign | 0x7C00 | (mantissa ? 0x200 : 0));
  }
  if (exponent >= 31) {
    return uint16_t(sign | 0x7C00);  // too large: infinity
  }
  if (exponent <= 0) {
    if (exponent < -10) {
      return uint16_t(sign);
    }
    mantissa |= 0x800000;
    uint32_t shift = uint32_t(14 - exponent);
    uint32_t half = mantissa >> shift;
    if ((mantissa >> (shift - 1)) & 1) {
      half++;  // round half up
    }
    return uint16_t(sign | half);
  }
  uint32_t half = uint32_t(exponent << 10) | (mantissa >> 13);
  if (mantissa & 0x1000) {
    half++;  // round half up (may carry into the exponent, correctly)
  }
  return uint16_t(sign | half);
}

// One guest texel's bytes (before the endian swap) from its host texels.
void CombineReadbackTexels(const ResolveReadbackItem& item, const uint8_t* data, uint32_t x,
                           uint32_t y, uint8_t* out) {
  const uint32_t bytes = item.info.bytesPerBlock, scale = item.scale;
  auto at = [&](uint32_t hx, uint32_t hy) {
    return data + item.offset + (uint64_t(hy) * item.rowWidth + hx) * bytes;
  };
  if (scale == 1) {
    std::memcpy(out, at(x, y), bytes);
    return;
  }
  const uint32_t count = scale * scale;
  switch (item.format) {
    case RenderFormat::R32_FLOAT:
    case RenderFormat::R32G32_FLOAT:
    case RenderFormat::R32G32B32A32_FLOAT: {
      float sum[4] = {};
      for (uint32_t sy = 0; sy < scale; sy++) {
        for (uint32_t sx = 0; sx < scale; sx++) {
          const uint8_t* texel = at(x * scale + sx, y * scale + sy);
          for (uint32_t c = 0; c < bytes / 4; c++) {
            float v;
            std::memcpy(&v, texel + c * 4, 4);
            sum[c] += v;
          }
        }
      }
      std::memcpy(out, sum, bytes);
      return;
    }
    case RenderFormat::R16_FLOAT:
    case RenderFormat::R16G16_FLOAT:
    case RenderFormat::R16G16B16A16_FLOAT: {
      float sum[4] = {};
      for (uint32_t sy = 0; sy < scale; sy++) {
        for (uint32_t sx = 0; sx < scale; sx++) {
          const uint8_t* texel = at(x * scale + sx, y * scale + sy);
          for (uint32_t c = 0; c < bytes / 2; c++) {
            uint16_t h;
            std::memcpy(&h, texel + c * 2, 2);
            sum[c] += HalfToFloat(h);
          }
        }
      }
      for (uint32_t c = 0; c < bytes / 2; c++) {
        uint16_t h = FloatToHalf(sum[c]);
        std::memcpy(out + c * 2, &h, 2);
      }
      return;
    }
    case RenderFormat::R8_UNORM:
    case RenderFormat::R8G8_UNORM:
    case RenderFormat::R8G8B8A8_UNORM: {
      for (uint32_t c = 0; c < bytes; c++) {
        uint32_t sum = 0;
        for (uint32_t sy = 0; sy < scale; sy++) {
          for (uint32_t sx = 0; sx < scale; sx++) {
            sum += at(x * scale + sx, y * scale + sy)[c];
          }
        }
        out[c] = uint8_t((sum + count / 2) / count);
      }
      return;
    }
    case RenderFormat::R16_UNORM:
    case RenderFormat::R16G16_UNORM:
    case RenderFormat::R16G16B16A16_UNORM: {
      for (uint32_t c = 0; c < bytes / 2; c++) {
        uint32_t sum = 0;
        for (uint32_t sy = 0; sy < scale; sy++) {
          for (uint32_t sx = 0; sx < scale; sx++) {
            uint16_t v;
            std::memcpy(&v, at(x * scale + sx, y * scale + sy) + c * 2, 2);
            sum += v;
          }
        }
        uint16_t v = uint16_t((sum + count / 2) / count);
        std::memcpy(out + c * 2, &v, 2);
      }
      return;
    }
    default:
      std::memcpy(out, at(x * scale, y * scale), bytes);  // the first host texel
      return;
  }
}

// Records the copy of a small resolve destination into this frame's readback
// buffer, to be written into guest memory when the frame is done.
void QueueResolveReadback(uint32_t address, const xenos::xe_gpu_texture_fetch_t& fetch,
                          HostTexture& dest) {
  if (!ResolveReadbackOn() || !dest.texture || dest.scale == 0) {
    return;
  }
  uint32_t width = dest.width / dest.scale, height = dest.height / dest.scale;
  // An F7 trace also writes the small resolves (up to 512x512: a shadow map, a reflection,
  // a ripple map) as the GPU holds them, into its folder.
  bool dump = g_traceFile && !g_traceDir.empty() && Tracing() && width && height && width <= 512 &&
              height <= 512;
  bool toGuest = width && height && width <= kReadbackSide && height <= kReadbackSide;
  if (!toGuest && !dump) {
    return;
  }
  TextureFormatInfo info = GetTextureFormatInfo(fetch.format);
  // (a texel at least the endian swap's word: the swap is done per texel)
  uint32_t swapWord = fetch.endianness == xenos::Endian::k8in16 ? 2
                      : fetch.endianness == xenos::Endian::kNone ? 1 : 4;
  if (toGuest && (info.format == RenderFormat::UNKNOWN || info.blockSize != 1 ||
                  info.bytesPerBlock != RenderFormatSize(dest.format) || info.bytesPerBlock > 16 ||
                  info.bytesPerBlock < swapWord)) {
    static std::unordered_set<uint32_t> reported;
    if (reported.insert(uint32_t(fetch.format)).second) {
      REXLOG_INFO("video: resolves to texture format {} aren't read back for the CPU",
                  uint32_t(fetch.format));
    }
    toGuest = false;
    if (!dump) {
      return;
    }
  }
  if (!toGuest) {
    info.bytesPerBlock = RenderFormatSize(dest.format);
  }
  ResolveReadback& r = g_resolveReadbacks[g_frame];
  uint32_t rowWidth = (dest.width + 63) & ~63u;
  uint64_t size = uint64_t(rowWidth) * dest.height * info.bytesPerBlock;
  if (toGuest && size > (1u << 20)) {
    toGuest = false;  // (as without a trace: too big to be one the game reads back)
    if (!dump) {
      return;
    }
  }
  if (size > (16u << 20)) {
    return;  // (a trace's dump: up to 16 MB)
  }
  uint64_t offset = (r.used + 511) & ~511ull;
  if (!r.buffer || offset + size > r.capacity) {
    // A bigger buffer: the old one stays until this frame's copies into it
    // have been read (the copies recorded so far point at it).
    uint64_t capacity = std::max<uint64_t>(offset + size, std::max<uint64_t>(r.capacity * 2, 64 << 10));
    auto buffer = g_device->createBuffer(RenderBufferDesc::ReadbackBuffer(capacity));
    if (!buffer) {
      return;
    }
    if (r.buffer) {
      r.retired.push_back(std::move(r.buffer));
    }
    r.buffer = std::move(buffer);
    r.capacity = capacity;
    offset = 0;
  }
  Transition(dest, RenderTextureLayout::COPY_SOURCE);
  CommandList()->copyTextureRegion(
      RenderTextureCopyLocation::PlacedFootprint(r.buffer.get(), dest.format, dest.width,
                                                 dest.height, 1, rowWidth, offset),
      RenderTextureCopyLocation::Subresource(dest.texture.get()));
  ResolveReadbackItem item;
  item.address = address, item.fetch = fetch, item.info = info;
  item.width = width, item.height = height, item.scale = dest.scale, item.rowWidth = rowWidth;
  item.format = dest.format, item.buffer = r.buffer.get(), item.offset = offset;
  item.toGuest = toGuest;
  if (dump) {
    static uint32_t n = 0;
    item.dumpPath = fmt::format("{}/resolve-{}-{:03}-{:08x}-{}x{}-host{}.raw", g_traceDir, g_frameCount,
                                n++, address, dest.width, dest.height, uint32_t(dest.format));
  }
  r.items.push_back(item);
  r.used = offset + size;
  if (!toGuest) {
    return;
  }
  static std::unordered_set<uint64_t> logged;
  if (logged.insert(uint64_t(address) << 32 | width << 16 | height).second) {
    REXLOG_INFO("video: resolve {}x{} texture format {} at {:#x} read back for the CPU", width,
                height, uint32_t(fetch.format), address);
  }
}

// Writes one read-back resolve into guest memory, in the texture's layout.
void WriteResolveReadback(const ResolveReadbackItem& item, const uint8_t* data) {
  const uint32_t bytes = item.info.bytesPerBlock;
  TextureLayout layout = GetTextureLayout(item.fetch, item.info, item.width, item.height);
  const LevelLayout& level = layout.levels[0];
  if (uint64_t(item.address) + level.start + level.size > GUEST_MEMORY_SIZE) {
    return;
  }
  // Not over memory the CPU has changed since the resolve (freed and reused,
  // a texture loaded there): that is the game's now, and the resolved host
  // texture is already stale.
  if (auto it = g_resolvedMemory.find(item.address); it != g_resolvedMemory.end()) {
    if (it->second.stale || ResolvedMemoryHash(item.address, it->second.size) != it->second.hash) {
      return;
    }
  }
  uint8_t* out = guest::Physical(item.address) + level.start;
  uint32_t bppLog2 = uint32_t(std::countr_zero(bytes));
  uint8_t texel[16];
  for (uint32_t y = 0; y < item.height; y++) {
    for (uint32_t x = 0; x < item.width; x++) {
      CombineReadbackTexels(item, data, x, y, texel);
      EndianSwap(texel, bytes, item.fetch.endianness);
      int64_t offset;
      if (item.fetch.tiled) {
        offset = TiledOffset2D(int32_t(x + level.offsetX), int32_t(y + level.offsetY),
                               level.pitchBlocks, bppLog2);
      } else {
        offset = (int64_t(y + level.offsetY) * level.pitchBlocks + (x + level.offsetX)) * bytes;
      }
      if (offset >= 0 && uint64_t(offset) + bytes <= level.size) {
        std::memcpy(out + offset, texel, bytes);
      }
    }
  }
  // The CPU didn't write this memory: the resolved host texture stays current.
  if (auto it = g_resolvedMemory.find(item.address); it != g_resolvedMemory.end()) {
    it->second.hash = ResolvedMemoryHash(item.address, it->second.size);
  }
}

// Resolves of that frame slot waiting to be read back (its fence not yet waited for).
bool ResolveReadbacksQueued(uint32_t frame) {
  return !g_resolveReadbacks[frame].items.empty();
}

// After the frame's fence: its read-back resolves into guest memory.
void ApplyResolveReadbacks(uint32_t frame) {
  ResolveReadback& r = g_resolveReadbacks[frame];
  if (!r.items.empty()) {
    const RenderBuffer* mapped = nullptr;
    const uint8_t* data = nullptr;
    for (const ResolveReadbackItem& item : r.items) {
      if (item.buffer != mapped) {
        if (data) {
          const_cast<RenderBuffer*>(mapped)->unmap();
        }
        mapped = item.buffer;
        data = static_cast<const uint8_t*>(const_cast<RenderBuffer*>(mapped)->map());
      }
      if (data && item.toGuest) {
        WriteResolveReadback(item, data);
      }
      if (data && !item.dumpPath.empty()) {
        if (FILE* f = std::fopen(item.dumpPath.c_str(), "wb")) {
          uint32_t bytes = RenderFormatSize(item.format);
          for (uint32_t y = 0; y < item.height * item.scale; y++) {
            std::fwrite(data + item.offset + uint64_t(y) * item.rowWidth * bytes, bytes,
                        item.width * item.scale, f);
          }
          std::fclose(f);
        }
      }
    }
    if (data) {
      const_cast<RenderBuffer*>(mapped)->unmap();
    }
    r.items.clear();
  }
  r.retired.clear();
  r.used = 0;
}

}  // namespace

void DestroyHostDevice() {
  g_presentDrainCheck.store(false);
  StopDrawWorker();
  if (g_displayThread) {
    g_displayRun.store(false);
    g_displayThread->join();
    delete g_displayThread;
    g_displayThread = nullptr;
  }
  g_displayRun.store(false);  // (the Windows display clock's thread, detached, stops too)
  std::lock_guard lock(g_mutex);
  if (g_queue) {
    if (g_recording) {
      OnRenderThread([](RenderCommandList* list) { list->end(); });
      g_recording = false;
    }
    WaitForGPU();
    if (RenderThreadEnabled()) {
      g_renderThread.Stop();
    }
  }
  g_presenter.Stop();  // (its presents use the swapchain and queue)
  StopPipelineWorkers();
  g_pipelines.clear();
  g_guestShaders.clear();
  g_hostShaders.clear();
  g_textures.clear();
  for (auto& garbage : g_garbage) {
    garbage.clear();
  }
  for (auto& readback : g_resolveReadbacks) {
    readback = ResolveReadback();
  }
  g_samplers.clear();
  for (auto& allocator : g_uploads) {
    allocator.chunks.clear();
  }
  ReleaseInterpResources();
  g_guestMemory.reset();
  g_frameRanges.clear();
  std::free(g_guestMemoryShadow);
  g_guestMemoryShadow = nullptr;
  g_guestMemoryRanges.clear();
  g_resolveTemps.clear();
  g_copyPipelines.clear();
  g_copyShader.reset();
  g_copy7e3Shader.reset();
  g_copy7e3MsShader.reset();
  g_scaleShader.reset();
  g_reducedBloom.reset();
  g_rectListShader.reset();
  g_framebuffers.clear();
  g_surfaces.clear();
  g_gammaTable.reset();
  g_movieTexture.reset();
  g_movieUploaded = 0;
  g_lastResolve.clear();
  g_resolvedTextures.clear();
  g_resolvedMemory.clear();
  g_presentPipeline.reset();
  g_presentShader.reset();
  g_fullscreenShader.reset();
  for (auto& view : g_nullTextureViews) {
    view.reset();
  }
  for (auto& texture : g_nullTextures) {
    texture.texture.reset();
  }
  g_linearClampSampler.reset();
  g_pipelineLayout.reset();
  g_samplerDescriptorSet.reset();
  g_textureDescriptorSet.reset();
  g_swapChainFramebuffers.clear();
  g_swapChain.reset();
  for (size_t i = 0; i < NUM_FRAMES; i++) {
    g_commandLists[i].reset();
    g_prologueLists[i].reset();
    g_prologueOpen[i] = false;
    g_commandFences[i].reset();
    g_acquireSemaphores[i].reset();
  }
  g_renderSemaphores.clear();
  for (GpuTiming& t : g_gpuTiming) {  // (before the device: they belong to it)
    t.pool.reset();
    t.count = 0;
    t.kinds.clear();
  }
  g_queue.reset();
  g_device.reset();
  g_interface.reset();
}

// D3D hooks -------------------------------------------------------------------

void RunClear(uint32_t device, uint32_t rectCount, const int32_t (*rects)[4], uint32_t flags,
             uint32_t color, float z, uint32_t stencil) {
  std::lock_guard lock(g_mutex);
  if (!g_device) {
    return;
  }
  FlushPendingDraw();
  BeginFrame();
  HostTexture* target = GetSurface(guest::Load32(device + guest::device::kRenderTargets), false);
  HostTexture* depth = GetSurface(guest::Load32(device + guest::device::kDepthStencil), true);
  // 360 flags: 0x1-0x8 render targets 0-3, 0x10 Z, 0x20 stencil.
  bool clearColor = (flags & 0x1) && target;
  bool clearDepth = (flags & 0x10) && depth;
  bool clearStencil = (flags & 0x20) && depth;
  if (Tracing()) {
    TraceLog("clear flags {:#x} rects {} target {:x} {}x{} depth {:x} {}x{} z {} stencil {}", flags, rectCount,
             reinterpret_cast<uintptr_t>(target), target ? target->width : 0, target ? target->height : 0,
             reinterpret_cast<uintptr_t>(depth), depth ? depth->width : 0, depth ? depth->height : 0, z, stencil);
  }
  if (!clearColor && !(flags & 0xE) && !clearDepth && !clearStencil) {
    return;
  }
  if (!target && !depth) {
    return;  // targets 1-3 without 0: rects can't be clipped; not seen
  }
  // Attachments of different shapes can't share a framebuffer.
  bool together = target && depth && target->width == depth->width &&
                  target->height == depth->height && target->samples == depth->samples;

  // Rects clipped to the target; with rects given but none usable, nothing
  // to clear.
  int32_t limitX = int32_t(target ? target->width : depth->width);
  int32_t limitY = int32_t(target ? target->height : depth->height);
  int32_t scale = int32_t(target ? target->scale : depth->scale);  // guest to host pixels
  // As D3D's Clear, within the scissor while the game has it on (a whole
  // target clear then clears the scissor's rect).
  int32_t guestX = limitX / scale, guestY = limitY / scale;
  int32_t clip[4] = {0, 0, guestX, guestY};
  bool scissor = guest::Load32(device + kScissorEnable) != 0;
  if (scissor) {
    for (uint32_t k = 0; k < 4; k++) {
      int32_t edge = int32_t(guest::Load32(device + kScissorRect + 4 * k));
      clip[k] = k < 2 ? std::max(clip[k], edge) : std::min(clip[k], edge);
    }
  }
  const int32_t (*sources)[4] = rects;
  uint32_t sourceCount = rectCount;
  const int32_t whole[1][4] = {{0, 0, guestX, guestY}};
  if (!rectCount && scissor) {
    sources = whole, sourceCount = 1;
  }
  std::vector<RenderRect> clearRects;
  for (uint32_t i = 0; i < sourceCount; i++) {
    // Clamped in guest pixels first: huge rects would overflow when scaled.
    RenderRect rect(std::clamp(sources[i][0], clip[0], std::max(clip[0], clip[2])) * scale,
                    std::clamp(sources[i][1], clip[1], std::max(clip[1], clip[3])) * scale,
                    std::clamp(sources[i][2], clip[0], std::max(clip[0], clip[2])) * scale,
                    std::clamp(sources[i][3], clip[1], std::max(clip[1], clip[3])) * scale);
    if (rect.right > rect.left && rect.bottom > rect.top) {
      clearRects.push_back(rect);
    }
  }
  if (sourceCount && clearRects.empty()) {
    return;
  }
  const RenderRect* rectData = clearRects.empty() ? nullptr : clearRects.data();
  uint32_t rectDataCount = uint32_t(clearRects.size());

  RenderColor value(float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f,
                    float(color & 0xFF) / 255.0f, float(color >> 24) / 255.0f);
  if (clearColor) {
    BindFramebuffer(target, together ? depth : nullptr);
    CommandList()->clearColor(0, value, rectData, rectDataCount);
  }
  if (clearDepth || clearStencil) {
    if (!(clearColor && together)) {
      BindFramebuffer(nullptr, depth);
    }
    // The rects within the depth buffer too: it can be smaller than the
    // color target they were clipped to, and a clear outside its
    // attachment is invalid (a driver may fault).
    std::vector<RenderRect> depthRects;
    for (const RenderRect& r : clearRects) {
      RenderRect d(std::min(r.left, int32_t(depth->width)), std::min(r.top, int32_t(depth->height)),
                   std::min(r.right, int32_t(depth->width)), std::min(r.bottom, int32_t(depth->height)));
      if (d.right > d.left && d.bottom > d.top) {
        depthRects.push_back(d);
      }
    }
    if (clearRects.empty() || !depthRects.empty()) {
      CommandList()->clearDepthStencil(clearDepth, clearStencil, z, stencil & 0xFF,
                                       depthRects.empty() ? nullptr : depthRects.data(),
                                       uint32_t(depthRects.size()));
    }
  }
  for (uint32_t i = 1; i < 4; i++) {
    HostTexture* other =
        (flags & (1u << i))
            ? GetSurface(guest::Load32(device + guest::device::kRenderTargets + i * 4), false)
            : nullptr;
    if (other && (!rectDataCount || (other->width == uint32_t(limitX) &&
                                     other->height == uint32_t(limitY)))) {
      BindFramebuffer(other, nullptr);
      CommandList()->clearColor(0, value, rectData, rectDataCount);
    }
  }
}

void RunResolve(uint32_t device, uint32_t flags, uint32_t sourceRect, uint32_t destTexture,
               uint32_t destPoint, uint32_t clearColor, float clearZ, uint32_t clearStencil) {
  std::lock_guard lock(g_mutex);
  StageTimer timer(Stage::kResolve);
  if (!g_device) {
    return;
  }
  FlushPendingDraw();
  BeginFrame();
  struct ResolveSpan {
    ResolveSpan() { MarkGpu(GpuMark::kResolveStart); }
    ~ResolveSpan() { MarkGpu(GpuMark::kResolveEnd); }
  } resolveSpan;
  if (destTexture) {
    // (texture objects hold CPU virtual addresses; the memo's fetch constants physical)
    ForgetFrameTextures(PhysicalAddress(uint32_t(ReadTextureFetch(destTexture).base_address) << 12));
  }
  uint32_t source = flags & 7;
  bool depth = source == 4;
  uint32_t surface = depth ? guest::Load32(device + guest::device::kDepthStencil)
                           : guest::Load32(device + guest::device::kRenderTargets + source * 4);
  HostTexture* target = GetSurface(surface, depth);
  if (Tracing()) {
    TraceLog("resolve flags {:#x} from {} to {:#x}", flags,
                target ? fmt::format("{}x{} fmt {}", target->width, target->height,
                                     uint32_t(target->format))
                       : std::string("none"),
                destTexture ? uint32_t(ReadTextureFetch(destTexture).base_address) << 12 : 0u);
  }

  if (target && destTexture) {
    auto fetch = ReadTextureFetch(destTexture);
    uint32_t address = PhysicalAddress(uint32_t(fetch.base_address) << 12);
    HostTexture* dest = GetResolveDestination(address, fetch, target->scale);
    if (dest) {
      NoteCarriedWrite(dest);
      int32_t x1 = 0, y1 = 0, x2 = int32_t(target->width), y2 = int32_t(target->height);
      if (sourceRect) {
        x1 = guest::LoadS32(sourceRect);
        y1 = guest::LoadS32(sourceRect + 4);
        x2 = guest::LoadS32(sourceRect + 8);
        y2 = guest::LoadS32(sourceRect + 12);
      }
      uint32_t dx = destPoint ? guest::Load32(destPoint) : 0;
      uint32_t dy = destPoint ? guest::Load32(destPoint + 4) : 0;
      // Guest pixels to the targets' host pixels.
      if (sourceRect) {
        int32_t scale = int32_t(target->scale);
        x1 *= scale, y1 *= scale, x2 *= scale, y2 *= scale;
      }
      dx *= target->scale, dy *= target->scale;
      if (dx < dest->width && dy < dest->height) {
        x1 = std::max(x1, 0);
        y1 = std::max(y1, 0);
        x2 = std::min({x2, int32_t(target->width), x1 + int32_t(dest->width - dx)});
        y2 = std::min({y2, int32_t(target->height), y1 + int32_t(dest->height - dy)});
      } else {
        x2 = x1;
      }
      if (depth && dest->format != RenderFormat::R32_FLOAT) {
        static bool warned = false;
        if (!warned) {
          warned = true;
          REXLOG_WARN("video: depth resolved into texture format {}, not converted",
                      uint32_t(fetch.format));
        }
      }
      if (x2 > x1 && y2 > y1 && (depth || target->float7e3 || dest->format != target->format)) {
        ResolveByDraw(*target, *dest, x1, y1, x2, y2, dx, dy, depth);
      } else if (x2 > x1 && y2 > y1) {
        if (target->samples != RenderSampleCount::COUNT_1) {
          Transition(*target, RenderTextureLayout::RESOLVE_SOURCE);
          Transition(*dest, RenderTextureLayout::RESOLVE_DEST);
          RenderRect rect(x1, y1, x2, y2);
          CommandList()->resolveTextureRegion(dest->texture.get(), dx, dy, target->texture.get(),
                                              &rect);
        } else {
          Transition(*target, RenderTextureLayout::COPY_SOURCE);
          Transition(*dest, RenderTextureLayout::COPY_DEST);
          RenderBox box(x1, y1, x2, y2);
          CommandList()->copyTextureRegion(
              RenderTextureCopyLocation::Subresource(dest->texture.get()),
              RenderTextureCopyLocation::Subresource(target->texture.get()), dx, dy, 0, &box);
        }
      }
      // Small destinations wholly written go back to guest memory too, for
      // the CPU (depth doesn't: its texture holds floats, not the 24_8 the
      // CPU would read).
      if (!depth && dx == 0 && dy == 0 && x2 - x1 == int32_t(dest->width) &&
          y2 - y1 == int32_t(dest->height)) {
        QueueResolveReadback(address, fetch, *dest);
      }
    }
  }

  // Optional clears after the copy: 0x100 render target, 0x200 depth/stencil.
  if ((flags & 0x100) && target && !depth) {
    BindFramebuffer(target, nullptr);
    float rgba[4] = {};
    for (uint32_t i = 0; clearColor && i < 4; i++) {
      rgba[i] = guest::LoadFloat(clearColor + i * 4);
    }
    CommandList()->clearColor(0, RenderColor(rgba[0], rgba[1], rgba[2], rgba[3]));
  }
  if (flags & 0x200) {
    HostTexture* depthTarget =
        depth ? target : GetSurface(guest::Load32(device + guest::device::kDepthStencil), true);
    if (depthTarget) {
      BindFramebuffer(nullptr, depthTarget);
      CommandList()->clearDepthStencil(true, true, clearZ, clearStencil & 0xFF);
    }
  }
}

void RunCreateShader(uint32_t object, uint32_t container, bool pixel) {
  std::lock_guard lock(g_mutex);
  if (!g_device || !object || !container) {
    return;
  }
  uint32_t size = guest::Load32(container + 4) + guest::Load32(container + 8);
  uint64_t hash = XXH3_64bits(guest::Virtual(container), size);
  auto shader = std::make_unique<GuestShader>();
  shader->container = container;
  shader->entry = FindShaderCacheEntry(hash);
  if (!shader->entry) {
    REXLOG_WARN("video: {} shader {:016X} at {:#x} is not in the shader cache",
                pixel ? "pixel" : "vertex", hash, container);
  }
  if (pixel) {
    uint32_t header = container + guest::Load32(container + 24);
    shader->colorOutputs = guest::Load32(header + 28) & 0xF;
  } else {
    // Declared vertex elements (address, usage, usage index) in container order.
    uint32_t header = container + guest::Load32(container + 24);
    uint32_t base = guest::Load32(header + 24);
    uint32_t count = guest::Load32(header + 28);
    for (uint32_t i = 0; i < count && i < 16; i++) {
      uint32_t element = guest::Load32(header + 36 + (base + i) * 4);
      shader->elements.emplace_back(uint8_t((element >> 12) & 0xF), uint8_t((element >> 16) & 0xF));
    }
  }
  g_guestShaders[object] = std::move(shader);
}

void RunDraw(uint32_t device, uint32_t primitive, uint32_t startVertex, uint32_t count,
            bool indexed, uint32_t startIndex) {
  std::lock_guard lock(g_mutex);
  if (!g_device) {
    return;
  }
  FlushPendingDraw();
  Draw(device, primitive, startVertex, count, indexed, startIndex);
}

void RunBeginVertices(uint32_t device, uint32_t primitive, uint32_t vertexCount, uint32_t stride,
                     uint32_t vertexData) {
  std::lock_guard lock(g_mutex);
  if (!g_device || !vertexData) {
    return;
  }
  FlushPendingDraw();
  InlineData data;
  data.vertexAddress = vertexData;
  data.vertexBytes = vertexCount * stride;
  data.stride = stride;
  Draw(device, primitive, 0, vertexCount, false, 0, &data);
}

void RunBeginIndexedVertices(uint32_t device, uint32_t primitive, uint32_t baseVertex,
                            uint32_t vertexCount, uint32_t indexCount, bool wideIndices,
                            uint32_t stride, uint32_t indexData, uint32_t vertexData) {
  std::lock_guard lock(g_mutex);
  if (!g_device || !vertexData || !indexData) {
    return;
  }
  FlushPendingDraw();
  InlineData data;
  data.vertexAddress = vertexData;
  data.vertexBytes = vertexCount * stride;
  data.stride = stride;
  data.indexAddress = indexData;
  data.indexCount = indexCount;
  data.quadList = xenos::PrimitiveType(primitive) == xenos::PrimitiveType::kQuadList;
  data.wideIndices = wideIndices;
  Draw(device, primitive, baseVertex, indexCount, true, 0, &data);
}

// The display's gamma table, as D3D's SetGammaRamp leaves it at dev+0x3A8C:
// 256 16-bit entries for red, then green, then blue (DC_LUT_SEQ_COLOR, low
// 6 bits zero), already adjusted for the display gamma the kernel reports
// (VdGetCurrentDisplayGamma, BT.709). The console applies it on scan-out, as
// Xenia's presenter does; even an identity game ramp darkens the picture.
// Kept as a 256x1 texture for the present shader.
void UpdateGammaTable(uint32_t device) {
  // With a 10-bit front buffer (D3DFORMAT 0x28280136 at dev+0x3518) D3D keeps
  // the same bytes as 128 PWL {base, delta} pairs instead: not handled yet.
  if (guest::Load32(device + 0x3518) == 0x28280136) {
    if (g_gammaTable) {
      g_garbage[g_frame].push_back(std::move(g_gammaTable));  // a frame may still read it
    }
    return;
  }
  uint16_t table[3][256];
  bool any = false;
  for (uint32_t c = 0; c < 3; c++) {
    for (uint32_t i = 0; i < 256; i++) {
      table[c][i] = guest::Load16(device + 0x3A8C + (c * 256 + i) * 2);
      any |= table[c][i] != 0;
    }
  }
  if (!any) {
    return;  // not set up yet: present unchanged
  }
  uint64_t hash = XXH3_64bits(table, sizeof(table));
  if (g_gammaTable && hash == g_gammaTableHash) {
    return;
  }
  g_gammaTableHash = hash;
  if (!g_gammaTable) {
    g_gammaTable = std::make_unique<HostTexture>();
    g_gammaTable->width = 256;
    g_gammaTable->height = 1;
    g_gammaTable->format = RenderFormat::R16G16B16A16_UNORM;
    g_gammaTable->texture = g_device->createTexture(
        RenderTextureDesc::Texture2D(256, 1, 1, RenderFormat::R16G16B16A16_UNORM));
    if (!g_gammaTable->texture) {
      g_gammaTable.reset();  // (none: the picture without its gamma)
      return;
    }
    SetDescriptor(*g_gammaTable);
  }
  Upload upload = Allocate(256 * 8, 16);
  auto* texels = reinterpret_cast<uint16_t*>(upload.data);
  for (uint32_t i = 0; i < 256; i++) {
    // 10-bit values in the high bits: 1023 << 6 must read as 1.0.
    for (uint32_t c = 0; c < 3; c++) {
      texels[i * 4 + c] = uint16_t(table[c][i] | (table[c][i] >> 10));
    }
    texels[i * 4 + 3] = 0xFFFF;
  }
  Transition(*g_gammaTable, RenderTextureLayout::COPY_DEST);
  CommandList()->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(g_gammaTable->texture.get()),
      RenderTextureCopyLocation::PlacedFootprint(upload.buffer, RenderFormat::R16G16B16A16_UNORM,
                                                 256, 1, 1, 256, upload.offset));
  Transition(*g_gammaTable, RenderTextureLayout::SHADER_READ);
}

// A new render_scale or antialiasing, between frames: the render targets are
// made again at the new size and sample count (their contents start over).
void ApplyRenderScale() {
  uint32_t scale = uint32_t(std::clamp(REXCVAR_GET(render_scale), 1, 8));
  uint32_t samples = AntialiasingSetting();
  if (scale == g_resolutionScale && samples == g_antialiasing) {
    return;
  }
  WaitForGPU();
  for (auto& [key, texture] : g_surfaces) {
    g_descriptors.Free(texture->descriptor);
  }
  for (auto& [source, texture] : g_resolveTemps) {
    g_descriptors.Free(texture->descriptor);
  }
  g_framebuffers.clear();
  g_boundFramebuffer = nullptr;
  g_surfaces.clear();
  g_resolveTemps.clear();
  // Resolved textures stay (guest memory never got their pixels); each is
  // made again at the new scale by its next resolve (GetResolveDestination).
  g_resolutionScale = scale;
  g_antialiasing = samples;
  REXLOG_INFO("video: render scale {}, antialiasing {}", scale, samples);
}

void RunSwap(uint32_t device, uint32_t frontBuffer) {
  std::lock_guard lock(g_mutex);
  if (!g_device) {
    return;
  }
  FlushPendingDraw();
  BeginFrame();
  UpdateGammaTable(device);
  HostTexture* front = nullptr;
  if (frontBuffer) {
    auto fetch = ReadTextureFetch(frontBuffer);
    uint32_t address = PhysicalAddress(uint32_t(fetch.base_address) << 12);
    if (auto it = g_resolvedTextures.find(ResolvedKey(address, fetch));
        it != g_resolvedTextures.end()) {
      front = it->second.get();
    } else if (auto last = g_lastResolve.find(address); last != g_lastResolve.end()) {
      front = last->second;
    }
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("video: front buffer {:#x} {}", uint32_t(fetch.base_address) << 12,
                  front ? "found" : "not resolved");
    }
  }
  Present(front);
  ApplyRenderScale();
  ReportProfile();
}


// The draw worker (NG2_DRAW_WORKER=1) ----------------------------------------
//
// The game's render thread only records each Direct3D call: the device's
// 64-byte blocks that changed since the call before (the window 0x480-0x3200
// the drawing reads), the call's arguments, and what it points to that the
// game reuses at once (Clear's and Resolve's rects, the immediate-mode
// vertices). A worker replays the records with the same code (Run*) against
// a mirror of the device in guest memory, so the drawing reads the device as
// before. Guest memory the drawing reads (vertex and index buffers, textures,
// shaders, declarations) is read on the worker, later: the GPU steps
// (native_gpu.cpp) wait for the worker to pass the calls before them, so a
// fence the game waits on to reuse memory passes only once it was drawn, and
// the game is held at most one frame ahead (at Swap). Shader creation drains
// the worker and runs at the call. Edge of Time
// Recompiled's render thread did the same.

namespace {

constexpr uint32_t kWindowBegin = 0x480, kWindowEnd = 0x3200, kBlock = 64;
// The mirror: the device window, Swap's fields (the front buffer's format at
// +0x3518, the gamma table at +0x3A8C, 1536 bytes), then Resolve's rect,
// point and clear color.
constexpr uint32_t kFrontFormat = 0x3518, kGamma = 0x3A8C, kGammaBytes = 1536;
constexpr uint32_t kMirrorRect = 0x4100, kMirrorPoint = 0x4110, kMirrorColor = 0x4120;
// Copies of objects the device points to, which the game may change or free
// right after the call (on the console D3D copies them into the command
// buffer at the call): the index buffer's header, the vertex declaration, a
// resolve's or Swap's texture header. The mirror's pointer is set to the copy.
constexpr uint32_t kIndexBufferField = 0x308C, kDeclarationField = 0x2E24;
constexpr uint32_t kMirrorIndexBuffer = 0x4140, kIndexBufferBytes = 32;
constexpr uint32_t kMirrorTexture = 0x4180, kTextureBytes = 64;
constexpr uint32_t kMirrorDeclaration = 0x4200, kDeclarationMax = 52 + 64 * 12;
// The 4 render targets' and the depth surface's objects (their fields at +0
// to +40), the same way.
constexpr uint32_t kSurfaceFields = 0x3090, kSurfaceCount = 5, kSurfaceBytes = 48;
constexpr uint32_t kMirrorSurfaces = 0x4600;
constexpr uint32_t kMirrorSize = 0x4800;
// Immediate-mode data (HUD-sized), a half for each frame, from the game's
// physical memory (the drawing takes its physical address).
constexpr uint32_t kStagingHalf = 2u << 20;

enum class Rec : uint32_t { kDraw, kInline, kClear, kResolve, kSwap };
struct RecHeader {
  Rec type;
  uint32_t bytes;  // the whole record, this header and the delta included
};
struct DrawArgs {
  uint32_t primitive, start, count, indexed, startIndex;
};
// After the args of draws: the index buffer header (indexed draws), then the
// declaration (its size first, 0: none).
struct InlineArgs {
  uint32_t primitive, baseVertex, vertexCount, count, indexed, wide, stride, vertexBytes,
      indexBytes;
};
struct ClearArgs {
  uint32_t count, flags, color, stencil;
  float z;
  int32_t rects[kMaxClearRects][4];
};
struct ResolveArgs {
  uint32_t flags, texture, hasRect, hasPoint, hasColor, clearStencil;
  float clearZ;
  uint8_t rect[16], point[8], color[16], textureHeader[kTextureBytes];
};
struct SwapArgs {
  uint32_t frontBuffer;
  uint8_t format[4];
  uint8_t gamma[kGammaBytes];
  uint8_t frontHeader[kTextureBytes];
};

struct DrawWorker {
  std::mutex mutex;
  std::condition_variable work_cv, done_cv;
  std::vector<uint8_t> queue;  // (mutex) records not yet taken
  bool waiting = false;        // (mutex) the worker sleeps for work
  bool stop = false;           // (mutex)
  std::thread thread;
  std::atomic<bool> started{false};
  // Records queued / replayed; Swaps queued / replayed.
  std::atomic<uint64_t> queued{0}, done{0};
  // The lowest record count a waiter waits for (~0: none): the worker wakes
  // waiters only once it gets there, not at every record.
  std::atomic<uint64_t> wakeAt{~0ull};
  uint64_t lastSwapRecord = 0;  // (producer) the last Swap's record count

  // The game's render thread's side.
  std::mutex producer;
  std::vector<uint8_t> record;
  uint8_t shadow[kWindowEnd] = {};
  uint32_t device = 0;
  // An immediate-mode draw: its record (the device as at its call) waits for
  // its data, which the game writes after the call.
  struct {
    bool pending = false;
    std::vector<uint8_t> record;
    uint32_t vertexData = 0, vertexBytes = 0, indexData = 0, indexBytes = 0;
    std::thread::id thread;  // the one writing its data
  } inl;
  bool failed = false;  // (producer) no guest memory: the calls run at once

  // The worker's side: guest memory for the mirror and the staging.
  uint32_t mirror = 0, staging = 0, stagingUsed = 0, stagingHalf = 0;

};
// Never destroyed: an exit without DestroyHostDevice leaves the thread running
// on it.
DrawWorker& g_worker = *new DrawWorker;

bool DrawWorkerEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("NG2_DRAW_WORKER");
    return v && v[0] == '1';
  }();
  return on;
}

uint32_t AllocGuest(uint32_t size) {
  auto* heap = rex::Runtime::instance()->memory()->LookupHeapByType(true, 4096);
  uint32_t address = 0;
  if (!heap || !heap->Alloc(size, 4096,
                            rex::memory::kMemoryAllocationReserve |
                                rex::memory::kMemoryAllocationCommit,
                            rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                            true, &address)) {
    return 0;
  }
  return address;
}

void Put(const void* data, size_t bytes) {
  auto& r = g_worker.record;
  size_t at = r.size();
  r.resize(at + bytes);
  std::memcpy(r.data() + at, data, bytes);
}
template <class T>
void Put(const T& value) {
  Put(&value, sizeof(value));
}

// Starts a record: its header, then the device's changed blocks.
void BeginRecord(Rec type, uint32_t device) {
  auto& w = g_worker;
  w.record.clear();
  Put(RecHeader{type, 0});
  if (device != w.device) {
    w.device = device;
    std::memset(w.shadow, 0xA5, sizeof(w.shadow));  // all blocks sent again
    w.shadow[kWindowBegin] ^= 0xFF;
  }
  size_t countAt = w.record.size();
  Put(uint32_t(0));
  uint32_t count = 0;
  const uint8_t* dev = guest::Virtual(device);
  for (uint32_t off = kWindowBegin; off < kWindowEnd; off += kBlock) {
    if (std::memcmp(dev + off, w.shadow + off, kBlock) == 0) {
      continue;
    }
    std::memcpy(w.shadow + off, dev + off, kBlock);
    Put(off);
    Put(dev + off, kBlock);
    count++;
  }
  std::memcpy(w.record.data() + countAt, &count, 4);
}

// The surfaces' objects, after the delta of draws, clears and resolves.
void PutSurfaces(uint32_t device) {
  for (uint32_t i = 0; i < kSurfaceCount; i++) {
    uint8_t object[kSurfaceBytes] = {};
    uint32_t surface = guest::Load32(device + kSurfaceFields + i * 4);
    if (surface) {
      std::memcpy(object, guest::Virtual(surface), sizeof(object));
    }
    Put(uint32_t(surface != 0));
    Put(object, sizeof(object));
  }
}

void DrawWorkerThread();

// The worker is used (under the producer lock): started on first use; without
// its guest memory the calls run at once.
bool Recording() {
  auto& w = g_worker;
  if (!DrawWorkerEnabled() || w.failed) {
    return false;
  }
  if (!w.started) {
    // The mirror is only read through virtual addresses: the system heap.
    w.mirror = rex::Runtime::instance()->memory()->SystemHeapAlloc(kMirrorSize, 4096);
    w.staging = AllocGuest(kStagingHalf * 2);
    if (!w.mirror || !w.staging) {
      REXLOG_WARN("video: draw worker: no guest memory for it, the calls run at once");
      w.failed = true;
      return false;
    }
    REXLOG_INFO("video: draw worker on (mirror {:#x}, staging {:#x})", w.mirror, w.staging);
    w.started = true;
    w.thread = std::thread(DrawWorkerThread);
  }
  return true;
}

// Hands the record to the worker.
void SendRecord() {
  auto& w = g_worker;
  uint32_t bytes = uint32_t(w.record.size());
  std::memcpy(w.record.data() + offsetof(RecHeader, bytes), &bytes, 4);
  bool wake;
  {
    std::lock_guard lock(w.mutex);
    w.queue.insert(w.queue.end(), w.record.begin(), w.record.end());
    wake = w.waiting;
  }
  w.queued.fetch_add(1, std::memory_order_release);
  if (wake) {
    w.work_cv.notify_one();
  }
}

// An immediate-mode draw's data is written after its call: recorded at the
// next call (or GPU step).
void FlushInline() {
  auto& w = g_worker;
  if (!w.inl.pending) {
    return;
  }
  w.inl.pending = false;
  w.record.swap(w.inl.record);
  Put(guest::Virtual(w.inl.vertexData), w.inl.vertexBytes);
  if (w.inl.indexBytes) {
    Put(guest::Virtual(w.inl.indexData), w.inl.indexBytes);
  }
  SendRecord();
}

// A draw's copies: the index buffer's header, the vertex declaration.
void PutDrawCopies(uint32_t device, bool indexed) {
  if (indexed) {
    uint32_t ib = guest::Load32(device + kIndexBufferField);
    uint8_t header[kIndexBufferBytes] = {};
    if (ib) {
      std::memcpy(header, guest::Virtual(ib), sizeof(header));
    }
    Put(header, sizeof(header));
  }
  uint32_t declaration = guest::Load32(device + kDeclarationField);
  uint32_t bytes = 0;
  if (declaration) {
    bytes = std::min(52 + std::min(guest::Load32(declaration + 24), 64u) * 12, kDeclarationMax);
  }
  Put(bytes);
  if (bytes) {
    Put(guest::Virtual(declaration), bytes);
  }
}

// Records an immediate-mode draw's call (the device now), sent with its data.
void BeginInline(uint32_t device, const InlineArgs& args, uint32_t vertexData,
                 uint32_t indexData) {
  auto& w = g_worker;
  BeginRecord(Rec::kInline, device);
  PutSurfaces(device);
  Put(args);
  PutDrawCopies(device, false);
  w.inl.record.swap(w.record);
  w.inl.pending = true;
  w.inl.thread = std::this_thread::get_id();
  w.inl.vertexData = vertexData, w.inl.vertexBytes = args.vertexBytes;
  w.inl.indexData = indexData, w.inl.indexBytes = args.indexBytes;
}

void WaitForWorker(uint64_t records) {
  auto& w = g_worker;
  if (w.done.load(std::memory_order_acquire) >= records) {
    return;
  }
  std::unique_lock lock(w.mutex);
  w.done_cv.wait(lock, [&] {
    if (w.done.load(std::memory_order_seq_cst) >= records || w.stop) {
      return true;
    }
    // (lowered under the lock; MarkReplayed resets it under the lock)
    uint64_t at = w.wakeAt.load(std::memory_order_seq_cst);
    while (records < at && !w.wakeAt.compare_exchange_weak(at, records)) {
    }
    return w.done.load(std::memory_order_seq_cst) >= records;
  });
}

// The worker's copy of immediate-mode data, in guest memory.
uint32_t Stage(const uint8_t* data, uint32_t bytes) {
  auto& w = g_worker;
  uint32_t size = (bytes + 255) & ~255u;
  if (!w.staging || size > kStagingHalf) {
    return 0;
  }
  if (w.stagingUsed + size > kStagingHalf) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("video: draw worker: more than {} MB of immediate-mode data in a frame",
                  kStagingHalf >> 20);
    }
    w.stagingUsed = 0;
  }
  uint32_t address = w.staging + w.stagingHalf * kStagingHalf + w.stagingUsed;
  w.stagingUsed += size;
  std::memcpy(guest::Virtual(address), data, bytes);
  return address;
}

// A record replayed (the steps and the frame limit waiting for it may go).
void MarkReplayed() {
  auto& w = g_worker;
  uint64_t done = w.done.fetch_add(1, std::memory_order_seq_cst) + 1;
  if (done < w.wakeAt.load(std::memory_order_seq_cst)) {
    return;
  }
  {
    std::lock_guard lock(w.mutex);  // (a waiter checks done under it)
    w.wakeAt.store(~0ull, std::memory_order_seq_cst);  // the waiters still short set it again
  }
  w.done_cv.notify_all();
}

// Points the mirror's field at a copy in the mirror.
void PointAt(uint32_t field, uint32_t copy) {
  uint8_t* m = guest::Virtual(g_worker.mirror);
  uint32_t address = rex::byte_swap(g_worker.mirror + copy);
  std::memcpy(m + field, &address, 4);
}

// A draw's copies after its args: the index buffer header, the declaration.
void ApplyDrawCopies(const uint8_t*& at, bool indexed) {
  uint8_t* m = guest::Virtual(g_worker.mirror);
  if (indexed) {
    static constexpr uint8_t kNone[kIndexBufferBytes] = {};
    if (std::memcmp(at, kNone, kIndexBufferBytes) == 0) {
      std::memset(m + kIndexBufferField, 0, 4);  // none at the call
    } else {
      std::memcpy(m + kMirrorIndexBuffer, at, kIndexBufferBytes);
      PointAt(kIndexBufferField, kMirrorIndexBuffer);
    }
    at += kIndexBufferBytes;
  }
  uint32_t bytes;
  std::memcpy(&bytes, at, 4);
  at += 4;
  if (bytes) {
    std::memcpy(m + kMirrorDeclaration, at, bytes);
    PointAt(kDeclarationField, kMirrorDeclaration);
    at += bytes;
  } else {
    std::memset(m + kDeclarationField, 0, 4);
  }
}

void Replay(const uint8_t* p) {
  auto& w = g_worker;
  RecHeader h;
  std::memcpy(&h, p, sizeof(h));
  const RecHeader* header = &h;
  const uint8_t* at = p + sizeof(RecHeader);
  uint32_t count;
  std::memcpy(&count, at, 4);
  at += 4;
  uint8_t* mirror = guest::Virtual(w.mirror);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t off;
    std::memcpy(&off, at, 4);
    std::memcpy(mirror + off, at + 4, kBlock);
    at += 4 + kBlock;
  }
  if (header->type != Rec::kSwap) {
    for (uint32_t i = 0; i < kSurfaceCount; i++) {
      uint32_t present;
      std::memcpy(&present, at, 4);
      uint32_t copy = kMirrorSurfaces + i * kSurfaceBytes;
      std::memcpy(mirror + copy, at + 4, kSurfaceBytes);
      if (present) {
        PointAt(kSurfaceFields + i * 4, copy);
      } else {
        std::memset(mirror + kSurfaceFields + i * 4, 0, 4);
      }
      at += 4 + kSurfaceBytes;
    }
  }
  uint32_t device = w.mirror;
  switch (header->type) {
    case Rec::kDraw: {
      DrawArgs a;
      std::memcpy(&a, at, sizeof(a));
      at += sizeof(a);
      ApplyDrawCopies(at, a.indexed != 0);
      RunDraw(device, a.primitive, a.start, a.count, a.indexed != 0, a.startIndex);
      break;
    }
    case Rec::kInline: {
      InlineArgs a;
      std::memcpy(&a, at, sizeof(a));
      at += sizeof(a);
      ApplyDrawCopies(at, false);
      {
        // The previous immediate-mode draw is uploaded before staging may wrap over it.
        std::lock_guard lock(g_mutex);
        FlushPendingDraw();
      }
      uint32_t vertices = Stage(at, a.vertexBytes);
      uint32_t indices = a.indexBytes ? Stage(at + a.vertexBytes, a.indexBytes) : 0;
      if (!vertices || (a.indexBytes && !indices)) {
        break;
      }
      if (a.indexed) {
        RunBeginIndexedVertices(device, a.primitive, a.baseVertex, a.vertexCount, a.count,
                                a.wide != 0, a.stride, indices, vertices);
      } else {
        RunBeginVertices(device, a.primitive, a.vertexCount, a.stride, vertices);
      }
      break;
    }
    case Rec::kClear: {
      ClearArgs a;
      std::memcpy(&a, at, sizeof(a));
      RunClear(device, a.count, a.rects, a.flags, a.color, a.z, a.stencil);
      break;
    }
    case Rec::kResolve: {
      ResolveArgs a;
      std::memcpy(&a, at, sizeof(a));
      std::memcpy(mirror + kMirrorRect, a.rect, sizeof(a.rect));
      std::memcpy(mirror + kMirrorPoint, a.point, sizeof(a.point));
      std::memcpy(mirror + kMirrorColor, a.color, sizeof(a.color));
      std::memcpy(mirror + kMirrorTexture, a.textureHeader, sizeof(a.textureHeader));
      RunResolve(device, a.flags, a.hasRect ? w.mirror + kMirrorRect : 0,
                 a.texture ? w.mirror + kMirrorTexture : 0,
                 a.hasPoint ? w.mirror + kMirrorPoint : 0,
                 a.hasColor ? w.mirror + kMirrorColor : 0, a.clearZ, a.clearStencil);
      break;
    }
    case Rec::kSwap: {
      SwapArgs a;
      std::memcpy(&a, at, sizeof(a));
      std::memcpy(mirror + kFrontFormat, a.format, sizeof(a.format));
      std::memcpy(mirror + kGamma, a.gamma, sizeof(a.gamma));
      std::memcpy(mirror + kMirrorTexture, a.frontHeader, sizeof(a.frontHeader));
      RunSwap(device, a.frontBuffer ? w.mirror + kMirrorTexture : 0);
      w.stagingHalf ^= 1;
      w.stagingUsed = 0;
      break;
    }
  }
}

void DrawWorkerThread() {
#if defined(__linux__)
  pthread_setname_np(pthread_self(), "Draw Worker");
#endif
  auto& w = g_worker;
  std::vector<uint8_t> batch;
  for (;;) {
    {
      std::unique_lock lock(w.mutex);
      w.waiting = true;
      w.work_cv.wait(lock, [&] { return !w.queue.empty() || w.stop; });
      w.waiting = false;
      if (w.stop) {
        return;
      }
      batch.swap(w.queue);
    }
    for (size_t at = 0; at < batch.size();) {
      RecHeader header;
      std::memcpy(&header, batch.data() + at, sizeof(header));
      Replay(batch.data() + at);
      at += header.bytes;
      MarkReplayed();
    }
    batch.clear();
  }
}

}  // namespace

void StopDrawWorker() {
  auto& w = g_worker;
  if (!w.started) {
    return;
  }
  {
    std::lock_guard lock(w.mutex);
    w.stop = true;
  }
  w.work_cv.notify_all();
  w.done_cv.notify_all();
  if (w.thread.joinable()) {
    w.thread.join();
  }
}

uint64_t RecordedCalls() {
  if (!DrawWorkerEnabled()) {
    return 0;
  }
  auto& w = g_worker;
  std::lock_guard lock(w.producer);
  if (!w.started) {
    return 0;
  }
  // (only the thread writing a pending immediate-mode draw's data knows it's written)
  if (w.inl.pending && w.inl.thread == std::this_thread::get_id()) {
    FlushInline();
  }
  return w.queued.load(std::memory_order_acquire);
}

void WaitForRecordedCalls(uint64_t records) {
  if (DrawWorkerEnabled() && records) {
    WaitForWorker(records);
  }
}

void OnClear(uint32_t device, uint32_t rectCount, const int32_t (*rects)[4], uint32_t flags,
             uint32_t color, float z, uint32_t stencil) {
  std::unique_lock lock(g_worker.producer);
  if (!Recording()) {
    lock.unlock();
    RunClear(device, rectCount, rects, flags, color, z, stencil);
    return;
  }
  FlushInline();
  ClearArgs a{};
  a.count = std::min(rectCount, kMaxClearRects);
  a.flags = flags, a.color = color, a.stencil = stencil, a.z = z;
  if (rects) {
    std::memcpy(a.rects, rects, a.count * sizeof(a.rects[0]));
  }
  BeginRecord(Rec::kClear, device);
  PutSurfaces(device);
  Put(a);
  SendRecord();
}

void OnResolve(uint32_t device, uint32_t flags, uint32_t sourceRect, uint32_t destTexture,
               uint32_t destPoint, uint32_t clearColor, float clearZ, uint32_t clearStencil) {
  std::unique_lock lock(g_worker.producer);
  if (!Recording()) {
    lock.unlock();
    RunResolve(device, flags, sourceRect, destTexture, destPoint, clearColor, clearZ,
               clearStencil);
    return;
  }
  FlushInline();
  ResolveArgs a{};
  a.flags = flags, a.texture = destTexture, a.clearStencil = clearStencil, a.clearZ = clearZ;
  a.hasRect = sourceRect != 0, a.hasPoint = destPoint != 0, a.hasColor = clearColor != 0;
  if (sourceRect) {
    std::memcpy(a.rect, guest::Virtual(sourceRect), sizeof(a.rect));
  }
  if (destPoint) {
    std::memcpy(a.point, guest::Virtual(destPoint), sizeof(a.point));
  }
  if (clearColor) {
    std::memcpy(a.color, guest::Virtual(clearColor), sizeof(a.color));
  }
  if (destTexture) {
    std::memcpy(a.textureHeader, guest::Virtual(destTexture), sizeof(a.textureHeader));
  }
  BeginRecord(Rec::kResolve, device);
  PutSurfaces(device);
  Put(a);
  SendRecord();
}

void OnSwap(uint32_t device, uint32_t frontBuffer) {
  uint64_t swaps;
  {
    std::unique_lock lock(g_worker.producer);
    if (!Recording()) {
      lock.unlock();
      RunSwap(device, frontBuffer);
      return;
    }
    FlushInline();
    SwapArgs a{};
    a.frontBuffer = frontBuffer;
    std::memcpy(a.format, guest::Virtual(device + kFrontFormat), sizeof(a.format));
    std::memcpy(a.gamma, guest::Virtual(device + kGamma), sizeof(a.gamma));
    if (frontBuffer) {
      std::memcpy(a.frontHeader, guest::Virtual(frontBuffer), sizeof(a.frontHeader));
    }
    BeginRecord(Rec::kSwap, device);
    Put(a);
    uint64_t previous = g_worker.lastSwapRecord;
    SendRecord();
    g_worker.lastSwapRecord = g_worker.queued.load(std::memory_order_relaxed);
    swaps = previous;
  }
  // At most one frame ahead of the worker: the previous Swap replayed.
  WaitForWorker(swaps);
}

void OnCreateShader(uint32_t object, uint32_t container, bool pixel) {
  if (DrawWorkerEnabled()) {
    WaitForWorker(RecordedCalls());  // (rare: at loads) the worker idle, then at the call
  }
  RunCreateShader(object, container, pixel);
}

void OnDraw(uint32_t device, uint32_t primitive, uint32_t startVertex, uint32_t count,
            bool indexed, uint32_t startIndex) {
  std::unique_lock lock(g_worker.producer);
  if (!Recording()) {
    lock.unlock();
    RunDraw(device, primitive, startVertex, count, indexed, startIndex);
    return;
  }
  FlushInline();
  BeginRecord(Rec::kDraw, device);
  PutSurfaces(device);
  Put(DrawArgs{primitive, startVertex, count, indexed ? 1u : 0u, startIndex});
  PutDrawCopies(device, indexed);
  SendRecord();
}

void OnBeginVertices(uint32_t device, uint32_t primitive, uint32_t vertexCount, uint32_t stride,
                     uint32_t vertexData) {
  std::unique_lock lock(g_worker.producer);
  if (!Recording()) {
    lock.unlock();
    RunBeginVertices(device, primitive, vertexCount, stride, vertexData);
    return;
  }
  if (!vertexData) {
    return;
  }
  FlushInline();
  BeginInline(device,
              InlineArgs{primitive, 0, vertexCount, vertexCount, 0, 0, stride, vertexCount * stride, 0},
              vertexData, 0);
}

void OnBeginIndexedVertices(uint32_t device, uint32_t primitive, uint32_t baseVertex,
                            uint32_t vertexCount, uint32_t indexCount, bool wideIndices,
                            uint32_t stride, uint32_t indexData, uint32_t vertexData) {
  std::unique_lock lock(g_worker.producer);
  if (!Recording()) {
    lock.unlock();
    RunBeginIndexedVertices(device, primitive, baseVertex, vertexCount, indexCount, wideIndices,
                            stride, indexData, vertexData);
    return;
  }
  if (!vertexData || !indexData) {
    return;
  }
  FlushInline();
  BeginInline(device,
              InlineArgs{primitive, baseVertex, vertexCount, indexCount, 1, wideIndices ? 1u : 0u,
                         stride, vertexCount * stride, indexCount * (wideIndices ? 4u : 2u)},
              vertexData, indexData);
}

}  // namespace ng2::video
