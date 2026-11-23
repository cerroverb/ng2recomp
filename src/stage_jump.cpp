// ng2 - Ninja Gaiden II static recompilation
//
// NG2_CHAPTER=<n> or NG2_SCENARIO=<scenario>[:<entry>]: a test aid that starts
// another scenario in place of Chapter 1's (New Game's, or a Chapter 1 save's),
// so a headless run can reach any chapter from the New Game path
// (tools/stage_sweep.sh runs them all). Experimental, for debugging.
//
// A scenario is what a "stage start" loads: its story (.sty), battle (.btl),
// event camera, enemy group and message files, by difficulty. The game's
// scenario table (24-byte records, its address at 0x83AA5CC8 + 4, read by
// sub_823D76E8) has 115: 9-22 the chapters (chapter n is 8 + n; 23-25, chapters
// 15-17, have no archive on the disc and stay on Now Loading), 27-30 and 109
// the demos, 31-57 the cutscene rooms (s_r01_10 ...), 58-108 Mission Mode's
// missions 00-50 (most of them need the Mission Mode DLC's battle files), 110
// the trial, the rest test scenarios. <scenario> is a number or the story
// file's name without s_ and the difficulty (chap_05, r03_10, mission04).
// <entry> is a u16 the game keeps beside it, in hex (default FFFF).
//
// sub_82445080 starts a scenario: it reads the id from the u16 at 0x842B033E
// and the entry from the one before it, and opens that scenario's chapter
// archive. Its caller writes 0x200 there for "none". Rewriting Chapter 1's id
// before its start sends the game's own load path to the new scenario: a
// chapter shows its card, then plays its opening, as when the story gets
// there. Every scenario start is logged ("scenario start: ...").
//
// (Earlier notes called this id a stage and read its name from the stage
// table at 0x83B08F98; that table is indexed differently. New Game's 9 is
// chapter 1, not YASIK_04.)

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#include "gpu/guest.h"

namespace {

constexpr uint32_t kScenarioTable = 0x83AA5CC8;  // +4: the table's holder
constexpr uint32_t kScenarioCount = 115;
constexpr uint32_t kScenarioId = 0x842B033E;  // u16, entry (u16) at kScenarioId - 2
constexpr uint32_t kNone = 0x200;
constexpr uint32_t kFileNames = 0x83B01718;  // a path per file id (mods.cpp)
constexpr uint32_t kFileCount = 5219;
constexpr uint32_t kFirstChapter = 9;
constexpr uint32_t kChapters = 14;  // 15-17 have no archive on the disc

// A scenario's name: its story file's, without "story\s_", the difficulty
// and ".sty" ("chap_05", "r03_10"). Empty while the table isn't loaded.
std::string ScenarioName(uint32_t id) {
  uint32_t holder = ng2::guest::Load32(kScenarioTable + 4);
  if (!holder || id >= kScenarioCount) return {};
  uint32_t table = holder + ng2::guest::Load32(holder);
  const uint8_t* record = ng2::guest::Virtual(table + id * 24);
  uint32_t story = uint32_t(record[0] << 8 | record[1]);
  if (story >= kFileCount) return {};
  uint32_t path = ng2::guest::Load32(kFileNames + story * 4);
  if (!path) return {};
  std::string name(reinterpret_cast<const char*>(ng2::guest::Virtual(path)));
  if (size_t slash = name.rfind('\\'); slash != std::string::npos) name.erase(0, slash + 1);
  if (name.rfind("s_", 0) == 0) name.erase(0, 2);
  if (size_t dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);
  for (const char* level : {"_nml", "_hd", "_vh", "_mn"}) {
    size_t n = std::strlen(level);
    if (name.size() > n && name.compare(name.size() - n, n, level) == 0) name.resize(name.size() - n);
  }
  return name;
}

// The scenario NG2_CHAPTER or NG2_SCENARIO asks for, or -1. Read at the first
// start, when the scenario table is loaded.
int RequestedScenario(uint32_t& entry) {
  static int scenario = -2;
  static uint32_t requestedEntry = 0xFFFF;
  if (scenario != -2) {
    entry = requestedEntry;
    return scenario;
  }
  scenario = -1;
  if (const char* chapter = std::getenv("NG2_CHAPTER"); chapter && *chapter) {
    char* end = nullptr;
    unsigned long n = std::strtoul(chapter, &end, 10);
    if (*end == '\0' && n >= 1 && n <= kChapters) {
      scenario = int(kFirstChapter + n - 1);
    } else {
      REXLOG_WARN("scenario jump: NG2_CHAPTER={} isn't 1-{}", chapter, kChapters);
    }
  } else if (const char* spec = std::getenv("NG2_SCENARIO"); spec && *spec) {
    std::string name(spec);
    if (size_t colon = name.find(':'); colon != std::string::npos) {
      const char* text = name.c_str() + colon + 1;
      char* end = nullptr;
      unsigned long value = std::strtoul(text, &end, 16);
      if (end != text && *end == '\0' && value <= 0xFFFF) {
        requestedEntry = uint32_t(value);
      } else {
        REXLOG_WARN("scenario jump: entry '{}' isn't a hex u16, using FFFF", text);
      }
      name.resize(colon);
    }
    char* end = nullptr;
    unsigned long number = std::strtoul(name.c_str(), &end, 10);
    if (end != name.c_str() && *end == '\0') {
      if (number < kScenarioCount) scenario = int(number);
    } else if (name.rfind("s_", 0) == 0 ? name.size() > 2 : !name.empty()) {
      if (name.rfind("s_", 0) == 0) name.erase(0, 2);
      for (uint32_t i = 0; i < kScenarioCount && scenario < 0; ++i) {
        if (name == ScenarioName(i)) scenario = int(i);
      }
    }
    if (scenario < 0) REXLOG_WARN("scenario jump: no scenario '{}'", spec);
  }
  entry = requestedEntry;
  return scenario;
}

}  // namespace

REX_EXTERN(__imp__sub_82445080);
REX_HOOK_RAW(sub_82445080) {
  uint8_t* field = ng2::guest::Virtual(kScenarioId - 2);
  uint32_t id = uint32_t(field[2] << 8 | field[3]);
  uint32_t entry = uint32_t(field[0] << 8 | field[1]);
  uint32_t newEntry = 0xFFFF;
  // Only Chapter 1's starts (every New Game; not a save of another chapter,
  // a demo or a mission), at the start itself: the scenario table is loaded
  // by then, for names.
  int scenario = id == kFirstChapter ? RequestedScenario(newEntry) : -1;
  if (scenario >= 0 && uint32_t(scenario) == id) {
    REXLOG_INFO("scenario jump: {} {} is the one asked for", id, ScenarioName(id));
  } else if (scenario >= 0) {
    REXLOG_INFO("scenario jump: {} {} becomes {} {} (entry {:04X})", id, ScenarioName(id), scenario,
                ScenarioName(uint32_t(scenario)), newEntry);
    id = uint32_t(scenario), entry = newEntry;
    field[0] = uint8_t(entry >> 8), field[1] = uint8_t(entry);
    field[2] = uint8_t(id >> 8), field[3] = uint8_t(id);
  }
  // NG2_SCENARIO_LIST=1: every scenario's name, once (the table is loaded by now).
  static bool listed = false;
  if (id < kNone && !listed && std::getenv("NG2_SCENARIO_LIST")) {
    listed = true;
    for (uint32_t i = 0; i < kScenarioCount; ++i) {
      REXLOG_INFO("scenario {}: {}", i, ScenarioName(i));
    }
  }
  if (id < kNone) {
    REXLOG_INFO("scenario start: {} {} entry {:04X}", id, ScenarioName(id), entry);
  }
  __imp__sub_82445080(ctx, base);
}
