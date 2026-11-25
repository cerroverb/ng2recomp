// ng2 - Ninja Gaiden II static recompilation
//
// Button prompts: the PlayStation, Nintendo or keyboard buttons drawn instead
// of the Xbox 360's (prompts.cpp; the Button Prompts setting).

#pragma once

#include <cstdint>

namespace ng2::prompts {

enum class Set { kXbox, kPlayStation, kNintendo, kKeyboard };
enum class Kind { kIndex8, kRGBA8, kOther };

// The set the atlas is made with now.
Set CurrentSet();

// Changes whenever the set shown changes (the setting, the device in use, the
// keys bound): the renderer adds it to the atlas's and palette's hashes, which
// it checks every frame (renderer thread).
uint32_t Generation();

// From the texture loader, with a texture's untiled base level (the host
// format's bytes) before it is uploaded: the button atlas or its palette,
// rewritten for the set shown. True if it is one of them (the renderer
// checks those every frame).
bool Apply(uint32_t width, uint32_t height, Kind kind, uint8_t* data, uint64_t bytes);

}  // namespace ng2::prompts
