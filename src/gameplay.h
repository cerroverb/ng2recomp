// ng2 - Ninja Gaiden II static recompilation
//
// The Gameplay page's settings (gameplay.cpp).

#pragma once

namespace ng2::gameplay {

bool SkipIntro();
// A cutscene script is running (its skip argument, u16 0x84A69AD4, is set).
bool InCutscene();

}  // namespace ng2::gameplay
