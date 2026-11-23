// ng2 - Ninja Gaiden II static recompilation
//
// RECOMP SETTINGS in the game's Options screen (recomp_settings.cpp).

#pragma once

#include <filesystem>

namespace rex::ui {
class WindowedAppContext;
}

namespace ng2::recomp_settings {

// Where the settings are saved when the menu closes (the runtime's config).
void SetConfigPath(const std::filesystem::path& path);
// Before the config is loaded: quotes text settings older versions wrote
// unquoted (else the file isn't TOML and doesn't load). Then, with logging up,
// ReportConfigRepair logs what it did.
void RepairConfig(const std::filesystem::path& path);
void ReportConfigRepair();
// Where the window's settings are applied (its UI thread).
void SetAppContext(rex::ui::WindowedAppContext* context);
// Exclusive fullscreen, if set, once the window is open.
void ApplyWindowAtStart();

// Each frame: F11 / Alt+Enter's fullscreen choice and a moved window's place,
// saved.
void WindowFrame();

}  // namespace ng2::recomp_settings
