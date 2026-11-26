// ng2 - Ninja Gaiden II static recompilation
//
// A scripted controller, for driving the game through its menus in test runs
// with nobody at it (the syntax is the NGB test rig's):
//
//   NG2_PAD_SCRIPT=20000:start,24000:a,26000:down,27000:a:400
//
// Each entry is <ms>:<button>[+<button>...][:<hold ms>], with times measured
// from the first time the game reads the pad and a default hold of 150 ms.
// NG2_PAD_SCRIPT=@file reads the same syntax from a file, one entry per line
// or comma separated.
//
// NG2_PAD_LIVE=<file> drives it while the game runs: each line appended to
// the file (<button>[+<button>...][:<hold ms>], no time) is pressed as soon
// as it is read, one after another with a 200 ms release between, so "down"
// then "a" never land in the same poll. Use a regular file appended with >>
// (not a FIFO); what it holds when the game starts is skipped, and it may be
// truncated to start over.
//
// Buttons: up down left right start back lthumb rthumb lb rb a b x y, lt rt
// (triggers fully pressed), lleft lright lup ldown / rleft rright rup rdown
// (a stick at full throw; "rright@20" at 20%); "ptr640x360" puts the menu pointer at that point of
// a 1280x720 window and "click" clicks its left button (menu_mouse.h); "trace" presses F7 (the renderer's trace) as the
// step fires, so a trace starts at the same point of a scripted run. Times are decimal. Every step is logged as it
// fires; a step whose window passed while the game wasn't reading the pad
// (a load) is pressed late for one read rather than dropped.
//
// The device is synthetic, so it feeds player one alongside any real pad.

#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>

namespace ng2::video {
void RequestTrace();  // src/gpu/video.cpp: as F7 (the "trace" step)
}
namespace ng2::menu_mouse {
void TestPointer(int32_t x, int32_t y);  // src/menu_mouse.cpp: the "ptr" and "click" steps
void TestClick();
}

namespace ng2 {

using rex::X_RESULT;
using rex::X_STATUS;

class ScriptInputDriver : public rex::input::InputDriver {
 public:
  ScriptInputDriver(std::string script, std::string livePath)
      : InputDriver(nullptr, 0), livePath_(std::move(livePath)) {
    Parse(script, nullptr);
    // Lines left from an earlier run are not this run's input.
    std::error_code ec;
    if (!livePath_.empty()) {
      auto size = std::filesystem::file_size(livePath_, ec);
      consumed_ = ec ? 0 : size_t(size);
    }
  }

  // NG2_PAD_SCRIPT / NG2_PAD_LIVE, or nullptr when neither is set.
  static std::unique_ptr<ScriptInputDriver> FromEnvironment() {
    const char* spec = std::getenv("NG2_PAD_SCRIPT");
    const char* live = std::getenv("NG2_PAD_LIVE");
    if ((!spec || !*spec) && (!live || !*live)) {
      return nullptr;
    }
    std::string script = spec ? spec : "";
    if (!script.empty() && script[0] == '@') {
      std::ifstream file(script.substr(1), std::ios::binary);
      if (!file) {
        REXLOG_ERROR("pad: cannot open script {}", script.substr(1));
        script.clear();
      } else {
        std::stringstream text;
        text << file.rdbuf();
        script = text.str();
      }
    }
    auto driver = std::make_unique<ScriptInputDriver>(script, live ? live : "");
    REXLOG_INFO("pad: script of {} step(s){}{}", driver->steps_.size(),
                driver->livePath_.empty() ? "" : ", live input from ",
                driver->livePath_);
    return driver;
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "Script";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                 rex::input::X_INPUT_CAPABILITIES* out) override {
    if (id != kDevice) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out->gamepad.buttons = 0xFFFF;
      out->gamepad.left_trigger = 0xFF;
      out->gamepad.right_trigger = 0xFF;
      out->gamepad.thumb_lx = int16_t(0x7FFF);
      out->gamepad.thumb_ly = int16_t(0x7FFF);
      out->gamepad.thumb_rx = int16_t(0x7FFF);
      out->gamepad.thumb_ry = int16_t(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kDevice) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    std::lock_guard lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    if (!started_) {
      started_ = true;
      start_ = now;
    }
    uint64_t t = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count();
    PollLive(t);

    Pad pad;
    for (auto& step : steps_) {
      bool late = t >= step.at + step.hold;
      // Steps missed in a stall go one at a time, with a release between.
      if (t < step.at || (late && (step.announced || t < lateResume_))) {
        continue;
      }
      if (late) {
        lateResume_ = t + 350;
      }
      if (!step.announced) {
        step.announced = true;
        REXLOG_INFO("pad: t={} ms \"{}\"{}", t, step.text, late ? " (late)" : "");
        if (step.trace) {
          video::RequestTrace();
        }
        if (step.pointerX >= 0) {
          menu_mouse::TestPointer(step.pointerX, step.pointerY);
        }
        if (step.click) {
          menu_mouse::TestClick();
        }
      }
      pad.buttons |= step.buttons;
      pad.lt = std::max(pad.lt, step.lt);
      pad.rt = std::max(pad.rt, step.rt);
      for (int i = 0; i < 4; i++) {
        if (step.axes[i]) {
          pad.axes[i] = step.axes[i];
        }
      }
    }
    // Finished steps go, so a long live session never grows the list.
    std::erase_if(steps_, [t](const Step& s) { return s.announced && t >= s.at + s.hold; });

    if (pad != last_) {
      last_ = pad;
      packet_++;
    }
    if (out) {
      out->packet_number = packet_;
      out->gamepad.buttons = pad.buttons;
      out->gamepad.left_trigger = pad.lt;
      out->gamepad.right_trigger = pad.rt;
      out->gamepad.thumb_lx = pad.axes[0];
      out->gamepad.thumb_ly = pad.axes[1];
      out->gamepad.thumb_rx = pad.axes[2];
      out->gamepad.thumb_ry = pad.axes[3];
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                              rex::input::X_INPUT_KEYSTROKE*) override {
    // NG2 only reads XamInputGetState.
    return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  static constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x4E473250);  // "NG2P"

  struct Pad {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t axes[4] = {};  // LX LY RX RY, up positive
    bool operator!=(const Pad& o) const {
      return buttons != o.buttons || lt != o.lt || rt != o.rt ||
             std::memcmp(axes, o.axes, sizeof(axes)) != 0;
    }
  };

  // A decimal millisecount, at most ~11 days so sums cannot wrap.
  static bool ParseMs(std::string_view text, uint64_t& out) {
    text = Trim(text);
    if (text.empty() || text.size() > 9) {
      return false;
    }
    uint64_t value = 0;
    for (char c : text) {
      if (c < '0' || c > '9') {
        return false;
      }
      value = value * 10 + uint64_t(c - '0');
    }
    out = value;
    return true;
  }

  static std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
      text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
      text.remove_suffix(1);
    }
    return text;
  }

  struct Step : Pad {
    uint64_t at = 0, hold = 150;
    std::string text;
    bool announced = false;
    bool trace = false;  // "trace": F7 as the step fires
    int32_t pointerX = -1, pointerY = -1;  // "ptr<x>x<y>": the menu pointer there (1280x720)
    bool click = false;                    // "click": a left click in a menu
  };

  static bool AddButton(std::string_view name, Step& step) {
    static constexpr struct {
      const char* name;
      uint16_t bit;
    } kButtons[] = {
        {"up", 0x0001},     {"down", 0x0002},   {"left", 0x0004}, {"right", 0x0008},
        {"start", 0x0010},  {"back", 0x0020},   {"lthumb", 0x0040}, {"rthumb", 0x0080},
        {"lb", 0x0100},     {"rb", 0x0200},     {"a", 0x1000},    {"b", 0x2000},
        {"x", 0x4000},      {"y", 0x8000},
    };
    static constexpr struct {
      const char* name;
      int axis;
      int16_t value;
    } kSticks[] = {
        {"lleft", 0, -32767}, {"lright", 0, 32767}, {"lup", 1, 32767},  {"ldown", 1, -32767},
        {"rleft", 2, -32767}, {"rright", 2, 32767}, {"rup", 3, 32767},  {"rdown", 3, -32767},
    };
    auto same = [&name](const char* word) {
      return std::strlen(word) == name.size() &&
             std::equal(name.begin(), name.end(), word, [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == b;
             });
    };
    for (const auto& b : kButtons) {
      if (same(b.name)) {
        step.buttons |= b.bit;
        return true;
      }
    }
    // "<stick>@<percent>": that stick part of the way ("rright@20", a slow turn)
    int percent = 100;
    const bool scaled = name.find('@') != std::string_view::npos;
    if (size_t at = name.find('@'); at != std::string_view::npos) {
      std::string_view digits = name.substr(at + 1);
      name = name.substr(0, at);
      percent = 0;
      for (char c : digits) {
        if (c < '0' || c > '9' || percent > 100) {
          return false;
        }
        percent = percent * 10 + (c - '0');
      }
      if (digits.empty() || percent > 100) {
        return false;
      }
    }
    for (const auto& s : kSticks) {
      if (same(s.name)) {
        step.axes[s.axis] = int16_t(s.value * percent / 100);
        return true;
      }
    }
    if (scaled) {
      return false;  // "@" only goes with a stick
    }
    if (same("trace")) {
      step.trace = true;
      return true;
    }
    if (same("click")) {
      step.click = true;
      return true;
    }
    if (name.size() > 3 && name.substr(0, 3) == "ptr") {
      int x = 0, y = 0;
      if (std::sscanf(std::string(name.substr(3)).c_str(), "%dx%d", &x, &y) == 2 && x >= 0 && y >= 0) {
        step.pointerX = x, step.pointerY = y;
        return true;
      }
      return false;
    }
    if (same("lt")) {
      step.lt = 0xFF;
      return true;
    }
    if (same("rt")) {
      step.rt = 0xFF;
      return true;
    }
    return false;
  }

  // Entries "<ms>:<buttons>[:<hold>]"; with timeless set, "<buttons>[:<hold>]"
  // scheduled at *timeless. Returns the end of the last parsed step.
  uint64_t Parse(std::string_view text, uint64_t* timeless) {
    uint64_t end = 0;
    size_t pos = 0;
    while (pos < text.size()) {
      size_t next = text.find_first_of(",\r\n", pos);
      if (next == std::string_view::npos) {
        next = text.size();
      }
      std::string_view entry = Trim(text.substr(pos, next - pos));
      pos = next + 1;
      if (entry.empty()) {
        continue;
      }

      Step step;
      step.text = std::string(entry);
      if (timeless) {
        step.at = *timeless;
      } else {
        size_t colon = entry.find(':');
        if (colon == std::string_view::npos || !ParseMs(entry.substr(0, colon), step.at)) {
          REXLOG_WARN("pad: script entry without <ms>: \"{}\"", entry);
          continue;
        }
        entry.remove_prefix(colon + 1);
      }
      size_t colon = entry.find(':');
      if (colon != std::string_view::npos) {
        if (!ParseMs(entry.substr(colon + 1), step.hold) || step.hold == 0) {
          REXLOG_WARN("pad: bad hold time in \"{}\"", step.text);
          continue;
        }
        entry = entry.substr(0, colon);
      }
      bool ok = !Trim(entry).empty();
      while (ok && !entry.empty()) {
        size_t plus = entry.find('+');
        std::string_view name = Trim(entry.substr(0, plus));
        if (!AddButton(name, step)) {
          REXLOG_WARN("pad: unknown button \"{}\"", name);
          ok = false;
        }
        entry = plus == std::string_view::npos ? std::string_view() : entry.substr(plus + 1);
      }
      if (!ok) {
        REXLOG_WARN("pad: step \"{}\" skipped", step.text);
        continue;
      }
      end = step.at + step.hold;
      if (timeless) {
        *timeless = end + 200;
      }
      steps_.push_back(std::move(step));
    }
    return end;
  }

  // Schedule whatever complete lines were appended to the live file since the
  // last poll (at most every 50 ms).
  void PollLive(uint64_t t) {
    if (livePath_.empty() || (lastPoll_ && t - lastPoll_ < 50)) {
      return;
    }
    lastPoll_ = t ? t : 1;
    FILE* file = std::fopen(livePath_.c_str(), "rb");
    if (!file) {
      return;
    }
    // Truncated or replaced by a shorter file: start over from its beginning.
    if (std::fseek(file, 0, SEEK_END) == 0 && size_t(std::ftell(file)) < consumed_) {
      consumed_ = 0;
      pending_.clear();
    }
    if (std::fseek(file, long(consumed_), SEEK_SET) == 0) {
      char buffer[4096];
      size_t n;
      while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        pending_.append(buffer, n);
        consumed_ += n;
      }
    }
    std::fclose(file);
    size_t lineEnd = pending_.find_last_of("\r\n");
    if (lineEnd == std::string::npos) {
      return;
    }
    uint64_t at = std::max(t, liveFree_);
    Parse(std::string_view(pending_).substr(0, lineEnd), &at);
    liveFree_ = at;
    pending_.erase(0, lineEnd + 1);
  }

  std::mutex mutex_;
  std::vector<Step> steps_;
  bool started_ = false;
  std::chrono::steady_clock::time_point start_;
  Pad last_;
  uint32_t packet_ = 1;
  uint64_t lateResume_ = 0;

  std::string livePath_;
  size_t consumed_ = 0;
  uint64_t lastPoll_ = 0;
  uint64_t liveFree_ = 0;
  std::string pending_;
};

}  // namespace ng2
