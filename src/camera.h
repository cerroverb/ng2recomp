#pragma once

namespace rex::memory {
class Memory;
}

namespace ng2::camera {
// Master Edition's camera code and state into the loaded image (camera.cpp).
void WriteCameraCode(rex::memory::Memory* memory);
}  // namespace ng2::camera
