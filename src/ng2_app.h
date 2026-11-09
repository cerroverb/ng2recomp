// ng2 - Ninja Gaiden II static recompilation
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <utility>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/system.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xmemory.h>

#include "controls.h"
#include "aspect.h"
#include "dlc.h"
#include "gameplay.h"
#include "mods.h"
#include "gpu/native_gpu.h"
#include "gpu/video.h"
#include "movie/movie.h"
#include "null_audio.h"
#include "recomp_settings.h"
#include "script_input.h"
#include "stderr_log.h"
#include "windows_setup.h"

class Ng2App : public rex::ReXApp {
 public:
  // Title Update 3's default.xexp, for the World disc (the installers also
  // check its SHA-1: the Japanese release's TU3 has the same size).
  static constexpr uintmax_t kTitleUpdate3Size = 2416640;

  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ng2App>(new Ng2App(ctx, "ng2",
        PPCImageConfig));
  }

  // NG2_CONFIG=<file>: settings file instead of <exe_dir>/ng2.toml (test
  // runs keep away from the player's settings).
  void OnConfigurePaths(rex::PathConfig& paths) override {
#ifdef _WIN32
    // The game folder and Title Update 3 (asked for once), settings and
    // saves in %LOCALAPPDATA%: what the AppImage's launcher does on Linux.
    if (!ng2::windows_setup::ConfigurePaths(paths)) {
      std::_Exit(0);
    }
#endif
    const char* config = std::getenv("NG2_CONFIG");
    if (config && *config) {
      paths.config_path = config;
    }
    ng2::recomp_settings::SetConfigPath(paths.config_path);
    ng2::video::SetPipelineListPath(paths.config_path.parent_path() / "ng2-pipelines.bin");
    ng2::recomp_settings::SetAppContext(&app_context());
    ng2::controls::SetAppContext(&app_context());
    ng2::video::SetAppContext(&app_context());
  }

  // Game-specific defaults, unless the user set them: the game reads the
  // zero page (from Xenia's compatibility notes for 544307D5), and its
  // system link socket stays off the network (no LAN play here; on Windows
  // it would bring up a firewall prompt).
  void OnPostInitLogging() override {
    ng2::stderr_log::Start();  // the libraries' and the driver's errors into ng2.log
    for (auto [name, value] : {std::pair{"protect_zero", "false"}, {"net_bind_loopback", "true"}}) {
      const auto* info = rex::cvar::GetFlagInfo(name);
      if (info && info->source == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(name, value);
      }
    }
    ng2::controls::ApplyDefaults();  // keyboard and mouse
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The native renderer (unless NG2_RENDERER=xenos) draws and presents, and
    // native_gpu.cpp does the rest of what the game's D3D asks of a GPU: no GPU
    // emulation, no GPU plugin.
    if (ng2::video::IsNativeRenderer()) {
      config.gpu_plugin.clear();
      config.graphics = ng2::gpu::CreateNativeGraphicsSystem();
    }

    // Which of the package's two builds runs (src/cpu_handover.cpp), for bug reports.
#if defined(__AVX2__)
    REXLOG_INFO("Ninja Gaiden II Recompiled {}, build: x86-64-v3 (AVX2)", NG2_VERSION);
#else
    REXLOG_INFO("Ninja Gaiden II Recompiled {}, build: x86-64-v2 (SSE4.2)", NG2_VERSION);
#endif

    // NG2_AUDIO=0: run the audio pipeline without an output device.
    const char* audio = std::getenv("NG2_AUDIO");
    if (audio && audio[0] == '0') {
      config.audio_factory = REX_AUDIO_BACKEND(ng2::NullAudioSystem);
      REXLOG_INFO("NG2_AUDIO=0: sound off (no output device)");
    } else {
      config.audio_factory = REX_AUDIO_BACKEND(ng2::SdlOrSilentAudioSystem);
    }

    // NG2_PAD_SCRIPT / NG2_PAD_LIVE: a scripted controller for player one.
    if (std::getenv("NG2_PAD_SCRIPT") || std::getenv("NG2_PAD_LIVE")) {
      config.input_factory = [](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
        auto input = rex::input::CreateDefaultInputSystem(tool_mode);
        if (auto script = ng2::ScriptInputDriver::FromEnvironment()) {
          input->AddDriver(std::move(script));
        }
        return input;
      };
    }
  }

  void OnPostSetup() override {
    // The recompiled code is Title Update 3's. With another update (or none)
    // next to default.xex the game's data wouldn't fit that code.
    std::error_code ec;
    if (std::filesystem::file_size(game_data_root() / "default.xexp", ec) != kTitleUpdate3Size) {
      REXLOG_ERROR("No Title Update 3 default.xexp in {}", rex::path_to_utf8(game_data_root()));
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                                "Title Update 3 is missing: its default.xexp must be next to "
                                "default.xex in the game folder.");
      std::_Exit(1);
    }
    ng2::movie::SetGameDataRoot(game_data_root());
    if (ng2::video::IsNativeRenderer() && !ng2::video::CreateHostDevice()) {
      // (half made, nothing could draw: stopped here rather than at the first draw)
      REXLOG_ERROR("native renderer: cannot create the Vulkan device");
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                                "Vulkan can't be started on this GPU (the log, ng2.log, says why). "
                                "The game needs a GPU with Vulkan 1.3 drivers.");
      std::_Exit(1);
    }
    ng2::recomp_settings::ApplyWindowAtStart();
  }

  void OnShutdown() override {
    if (ng2::video::IsNativeRenderer()) {
      ng2::video::DestroyHostDevice();
    }
  }

  // Downloadable content from the game folder's dlc/ (installed once).
  void OnPreLaunchModule() override {
    ng2::dlc::InstallPackages(runtime()->kernel_state(), game_data_root(), user_data_root());
  }

  // NG2_DUMP_IMAGE=<path>: write the loaded (decrypted, decompressed) XEX
  // image for offline analysis, e.g. finding code reached only by pointer.
  void OnPostLoadXexImage() override {
    // Aspect Ratio (aspect.cpp) writes the game's 16:9 constant, which sits
    // in read-only image data: its page is made writable.
    auto* heap = runtime()->memory()->LookupHeap(ng2::aspect::kConstantAddress);
    if (!heap || !heap->Protect(ng2::aspect::kConstantAddress, 4,
                                rex::memory::kMemoryProtectRead |
                                    rex::memory::kMemoryProtectWrite)) {
      REXLOG_ERROR("aspect ratio: the game's aspect constant can't be made writable");
    } else {
      ng2::aspect::SetConstantWritable(true);
    }
    // Mods: the folder next to the saves' (the data folder, or userdata/ in
    // portable mode).
    const std::filesystem::path data = user_data_root().parent_path();
    ng2::mods::Activate(data / "mods", game_data_root(), data / "mods-cache");
    const char* path = std::getenv("NG2_DUMP_IMAGE");
    if (!path || !*path) {
      return;
    }
    const uint8_t* image =
        runtime()->memory()->TranslateVirtual<const uint8_t*>(REX_IMAGE_BASE);
    FILE* f = std::fopen(path, "wb");
    if (!f) {
      REXLOG_ERROR("NG2_DUMP_IMAGE: cannot open {}", path);
      return;
    }
    size_t written = std::fwrite(image, 1, REX_IMAGE_SIZE, f);
    std::fclose(f);
    REXLOG_INFO("NG2_DUMP_IMAGE: wrote {} of {} bytes to {}", written,
                REX_IMAGE_SIZE, path);
  }
};
