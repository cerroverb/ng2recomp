// ng2 - Ninja Gaiden II static recompilation
//
// Native renderer: the game's D3D calls are hooked and drawn with Vulkan
// through plume, after Unleashed Recompiled's gpu/video.cpp. It draws by
// default; with NG2_RENDERER=xenos ReXGlue's xenos plugin emulates the GPU.

#pragma once

#include <string>
#include <filesystem>

#include <cstdint>

namespace rex::ui {
class WindowedAppContext;
}

namespace ng2::video {

// True unless NG2_RENDERER=xenos: no GPU plugin, this renderer draws.
bool IsNativeRenderer();

// Creates the Vulkan device and swapchain on the game window (Wayland or X11).
bool CreateHostDevice();

void DestroyHostDevice();
// Where the pipelines made are listed, to make them early next time.
void SetPipelineListPath(const std::filesystem::path& path);
// The game starting a frame (for NG2_LATENCY_LOG).
void MarkFrameStart();

// A full-screen movie's frame (Y, U, V planes, 4:2:0), shown instead of the
// game's picture while frames keep coming.
void ShowMovieFrame(const uint8_t* const planes[3], const uint32_t pitches[3], uint32_t width,
                    uint32_t height);
// The full-screen movie is still playing (no new frame this time): its last
// frame stays up.
void KeepMovieShown();

// Called from the D3D hooks on the game's D3D thread, after the original
// function ran (guest addresses; docs/d3d-map-*.md has the signatures).
// The most rects a Clear passes on (D3D's has no limit; more are dropped).
constexpr uint32_t kMaxClearRects = 64;
void OnClear(uint32_t device, uint32_t rectCount, const int32_t (*rects)[4], uint32_t flags,
             uint32_t color, float z, uint32_t stencil);
void OnResolve(uint32_t device, uint32_t flags, uint32_t sourceRect, uint32_t destTexture,
               uint32_t destPoint, uint32_t clearColor, float clearZ, uint32_t clearStencil);
void OnSwap(uint32_t device, uint32_t frontBuffer);
void OnCreateShader(uint32_t object, uint32_t container, bool pixel);
void OnDraw(uint32_t device, uint32_t primitive, uint32_t startVertex, uint32_t count,
            bool indexed, uint32_t startIndex);
// Immediate-mode draws: the game writes the data into the returned memory
// after the call; the draw is recorded at the next hooked call.
void OnBeginVertices(uint32_t device, uint32_t primitive, uint32_t vertexCount, uint32_t stride,
                     uint32_t vertexData);
void OnBeginIndexedVertices(uint32_t device, uint32_t primitive, uint32_t baseVertex,
                            uint32_t vertexCount, uint32_t indexCount, bool wideIndices,
                            uint32_t stride, uint32_t indexData, uint32_t vertexData);

// The draw worker (NG2_DRAW_WORKER=1, video.cpp): the calls recorded so far
// (0 when it's off), and a wait until the worker has replayed that many. The
// GPU steps (native_gpu.cpp) run after the calls before them were drawn.
uint64_t RecordedCalls();

void WaitForRecordedCalls(uint64_t records);

// Under gamescope, where frames' times on screen are reported unevenly (video.cpp).
bool UnderGamescope();
// The game a frame ahead of the display and held up by it each frame: the vblank
// timer should skip a step (video.cpp, NoteFrameWait).
bool DrainRequested();
// The display's last refresh and period (steady_clock ns), from the
// presents reaching the screen; false unless steady and recent.
bool DisplayRefresh(int64_t* lastNs, int64_t* periodNs);
// With V-Sync FIFO, the display's queue has run dry: the last half second's
// swapchain acquires didn't block (native_gpu.cpp's timer refills it).
bool PresentQueueDrained();
// The app's window context, for the display queries that must run on its thread.
void SetAppContext(rex::ui::WindowedAppContext* context);
int64_t DisplayClockNow();
// NG2_LATENCY_LOG: the main thread read the pad; the render thread took the
// frame (the handoff).
void NoteInputRead();
void NoteFrameTaken();

// NG2_TRACE_STACKS=1: the guest call chain of the draw about to be made, for its trace line.
void NoteDrawStack(const std::string& stack);

}  // namespace ng2::video
