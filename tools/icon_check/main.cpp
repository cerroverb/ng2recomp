// ng2 - Ninja Gaiden II static recompilation
//
// Dev check for src/game_icon.cpp: makes the icon from a game folder and
// writes each size as <out>/ng2_<size>.pam (no image library needed), for
// compare.py to set against the Python mock-ups (out/icon/make_icons.py).
// The pictures are game art: write them under out/ only. Built by hand:
//   c++ -O2 -std=c++20 -Isrc src/game_icon.cpp tools/icon_check/main.cpp -o out/icon/check
//   out/icon/check <game dir> <out dir>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "game_icon.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <game dir> <out dir>\n", argv[0]);
    return 2;
  }
  std::vector<ng2::game_icon::Picture> pictures;
  std::string why;
  auto start = std::chrono::steady_clock::now();
  if (!ng2::game_icon::Make(argv[1], &pictures, &why)) {
    std::fprintf(stderr, "no icon: %s\n", why.c_str());
    return 1;
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::printf("made in %.1f ms\n", ms);
  std::filesystem::create_directories(argv[2]);
  for (const auto& p : pictures) {
    std::string path = std::string(argv[2]) + "/ng2_" + std::to_string(p.size) + ".pam";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return 1;
    std::fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", p.size, p.size);
    std::fwrite(p.rgba.data(), 1, p.rgba.size(), f);
    std::fclose(f);
  }
  return 0;
}
