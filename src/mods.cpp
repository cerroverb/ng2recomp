// ng2 - Ninja Gaiden II static recompilation
//
// Mods: a mod is a folder in the mods folder. Its files/ holds game files by
// their path in the game (files/char/ryu_b/ryu_b.gmd stands for the game's
// char\ryu_b\ryu_b.gmd), loaded in place of the game's own. Its optional
// mod.toml names it (name, version), can turn it off (enabled = false), and
// can load one of the game's own files in place of another:
//
//   [replace]
//   'char/ryu_b/ryu_b.gmd' = 'char/ryu_c/ryu_c.gmd'
//
// Mods are taken in their folders' name order; when two replace the same
// file, the first one's is loaded.
//
// How a game file is loaded, and where a mod's goes in (TU3 addresses):
// - Every file has an id: the executable's table at 0x83B01718 gives its path
//   ("char\ryu_b\ryu_b.gmd"), 5219 of them (5099 on the disc, the rest the
//   Mission Mode DLC's). load_img.bin has the disc files' sizes, pack_img.bin
//   where each sits in the .ng2 archives.
// - The loader's request for a file (sub_823D61D8) carries a byte that, when
//   set, has the file loaded loose (sub_823D69F8), from D:\databin\<its path>
//   or D:\<its path>: the developers' way to try a file outside the archives.
//   For a mod's file that byte is set, and the loose open is given the mod's
//   file instead (on a read-only device of the mods folder), so the game loads
//   it through its own file calls.
// - The game asks a file's size (sub_823D6428) before it makes room for it: a
//   mod's file larger than the game's gets its own size there, and in
//   load_img.bin's table once that is loaded (sub_8366C178).

#include "mods.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gpu/guest.h"

REXCVAR_DEFINE_BOOL(log_file_loads, false, "Mods",
                    "Log every file the game loads, by id and path (to find the file to mod)");

namespace fs = std::filesystem;

namespace ng2::mods {
namespace {

constexpr uint32_t kFileNames = 0x83B01718;  // a path string per file id
constexpr uint32_t kFileCount = 5219;        // the loader's (0x1463)
constexpr uint32_t kDiscFileCount = 5099;    // load_img.bin's
constexpr uint32_t kLoadImg = 0x84F59208;    // load_img.bin as loaded: its buffer
// pack_img.bin's first slots and their archives (slots 6 on hold the chapters
// again, one each).
const std::vector<std::vector<std::string>>& SlotArchives() {
  static const std::vector<std::vector<std::string>> slots = [] {
    std::vector<std::vector<std::string>> s = {{"cmn.ng2"}, {"char.ng2"}, {}, {"rtm.ng2"},
                                               {"other.ng2"}, {"other_rtm.ng2"}};
    for (int c = 1; c <= 14; ++c) {
      char name[24];
      std::snprintf(name, sizeof(name), "s_chap_%02d.ng2", c);
      s[2].push_back(name);
    }
    return s;
  }();
  return slots;
}

// A path as the game writes it, for comparing: backslashes, lower case, no
// leading separator.
std::string Key(std::string_view path) {
  std::string k;
  k.reserve(path.size());
  for (char ch : path) {
    if (ch == '/') ch = '\\';
    if (k.empty() && ch == '\\') continue;
    k.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
  }
  return k;
}

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

std::vector<uint8_t> ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Where a disc file sits in the archives: pack_img.bin has four tables (ids,
// counts, offsets, sizes), each a word per slot pointing at a word per archive.
struct Located {
  std::string archive;
  uint32_t offset = 0, size = 0;
};
bool Locate(const std::vector<uint8_t>& pack, uint32_t id, Located* out) {
  auto word = [&](uint32_t at, uint32_t* v) {
    if (size_t(at) + 4 > pack.size()) return false;
    *v = Be32(pack.data() + at);
    return true;
  };
  uint32_t table[4];
  for (uint32_t i = 0; i < 4; ++i) {
    if (!word(i * 4, &table[i])) return false;
  }
  const auto& slots = SlotArchives();
  for (uint32_t slot = 0; slot < slots.size(); ++slot) {
    uint32_t p[4];
    for (int i = 0; i < 4; ++i) {
      if (!word(table[i] + 4 * slot, &p[i])) return false;
    }
    for (uint32_t a = 0; a < slots[slot].size(); ++a) {
      uint32_t ids, count, offsets, sizes;
      if (!word(p[0] + 4 * a, &ids) || !word(p[1] + 4 * a, &count) ||
          !word(p[2] + 4 * a, &offsets) || !word(p[3] + 4 * a, &sizes) || count > kFileCount) {
        return false;
      }
      for (uint32_t k = 0; k < count; ++k) {
        uint32_t fid;
        if (!word(ids + 4 * k, &fid)) return false;
        if (fid == id) {
          out->archive = slots[slot][a];
          return word(offsets + 4 * k, &out->offset) && word(sizes + 4 * k, &out->size);
        }
      }
    }
  }
  return false;
}

// A NUL-ended guest string of at most max bytes, or null.
const char* GuestString(uint32_t va, size_t max, size_t* len) {
  if (va < 0x10000u || va > 0xFFFFFFFFu - max) return nullptr;  // (the game's stacks are at 0x7...)
  const char* s = reinterpret_cast<const char*>(ng2::guest::Virtual(va));
  size_t n = 0;
  while (n < max && s[n]) ++n;
  if (n == max) return nullptr;
  *len = n;
  return s;
}

bool Mount(const char* device, const char* link, const fs::path& root) {
  auto* vfs = REX_KERNEL_FS();
  auto d = std::make_unique<rex::filesystem::HostPathDevice>(device, root, true);
  if (!vfs || !d->Initialize() || !vfs->RegisterDevice(std::move(d)) ||
      !vfs->RegisterSymbolicLink(link, device)) {
    REXLOG_WARN("mods: can't read {}", rex::path_to_utf8(root));
    return false;
  }
  return true;
}

struct Entry {
  uint32_t id = 0;
  uint32_t size = 0;
  uint32_t guestPath = 0;  // the file to the game's CreateFile
  std::string mod;
};

// Set up by Activate before the game runs; read-only after.
struct State {
  std::atomic<bool> active{false};
  bool logLoads = false;
  std::vector<std::string> names;  // the game's path per file id
  std::vector<Entry> entries;
  std::vector<int32_t> byId;  // entry per file id, or -1
  std::unordered_map<std::string, size_t> byKey;
  std::atomic<uint32_t> loaded{0};
} g;

std::vector<std::string> ReadNames() {
  std::vector<std::string> names;
  names.reserve(kFileCount);
  for (uint32_t id = 0; id < kFileCount; ++id) {
    size_t len = 0;
    const char* s = GuestString(ng2::guest::Load32(kFileNames + 4 * id), 256, &len);
    if (!s || !len) {
      REXLOG_WARN("mods: the game's file table ends early, at {}: mods off", id);
      return {};
    }
    names.emplace_back(s, len);
  }
  return names;
}

struct Mod {
  std::string folder, name, version;
  fs::path dir;
  bool enabled = true;
  std::vector<std::pair<std::string, std::string>> replace;  // game file, file loaded instead
};

bool ReadManifest(Mod& mod) {
  const fs::path file = mod.dir / "mod.toml";
  std::error_code ec;
  if (!fs::exists(file, ec)) return true;
  try {
    const toml::table root = toml::parse_file(rex::path_to_utf8(file));  // (UTF-8, on Windows too)
    const toml::table* t = root["mod"].as_table() ? root["mod"].as_table() : &root;
    if (auto v = (*t)["name"].value<std::string>()) mod.name = *v;
    if (auto v = (*t)["version"].value<std::string>()) mod.version = *v;
    if (auto v = (*t)["enabled"].value<bool>()) mod.enabled = *v;
    if (const toml::table* r = root["replace"].as_table()) {
      for (const auto& [target, source] : *r) {
        if (auto s = source.value<std::string>()) {
          mod.replace.emplace_back(std::string(target.str()), *s);
        }
      }
    }
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("mods: {}: mod.toml can't be read ({}): left out", mod.folder,
                std::string(e.description()));
    return false;
  }
  return true;
}

}  // namespace

// game-files.txt in the mods folder: every game file's path, the names a mod's
// files/ uses (written once; delete it to have it written again).
static void WriteFileList(const fs::path& folder) {
  std::error_code ec;
  const fs::path list = folder / "game-files.txt";
  if (fs::exists(list, ec)) return;
  std::vector<std::string> names = ReadNames();
  if (names.size() != kFileCount) return;
  fs::create_directories(folder, ec);
  std::string text =
      "# The game's files, by the path a mod uses: a mod's files/<path> replaces the file.\n"
      "# (the Mission Mode DLC's are the last ones; written by the game, delete to remake)\n";
  for (std::string name : names) {
    std::replace(name.begin(), name.end(), '\\', '/');
    text += name;
    text += '\n';
  }
  const fs::path temp = folder / "game-files.txt.tmp";
  bool ok = false;
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(text.data(), std::streamsize(text.size()));
    out.close();
    ok = bool(out);
  }
  if (ok) fs::rename(temp, list, ec);
  if (!ok || ec) {
    fs::remove(temp, ec);
  } else {
    REXLOG_INFO("mods: the game's file paths written to {}", rex::path_to_utf8(list));
  }
}

void Activate(const fs::path& folder, const fs::path& gameRoot, const fs::path& cacheDir) {
  g.logLoads = REXCVAR_GET(log_file_loads);
  WriteFileList(folder);
  std::error_code ec;
  std::vector<Mod> mods;
  if (fs::is_directory(folder, ec)) {
    for (auto it = fs::directory_iterator(folder, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {  // (increment(ec): an error mid-scan ends it, never throws)
      const auto& item = *it;
      if (!item.is_directory(ec)) continue;
      Mod mod;
      mod.dir = item.path();
      mod.folder = rex::path_to_utf8(item.path().filename());
      mod.name = mod.folder;
      if (ReadManifest(mod)) mods.push_back(std::move(mod));
    }
  }
  std::sort(mods.begin(), mods.end(), [](const Mod& a, const Mod& b) {
    return Key(a.folder) < Key(b.folder);
  });
  const bool any = std::any_of(mods.begin(), mods.end(), [](const Mod& m) { return m.enabled; });
  if (!any && !g.logLoads) {
    if (!mods.empty()) REXLOG_INFO("mods: {} in {}, none on", mods.size(), rex::path_to_utf8(folder));
    return;
  }
  g.names = ReadNames();
  if (g.names.size() != kFileCount) return;
  std::unordered_map<std::string, uint32_t> ids;
  for (uint32_t id = 0; id < kFileCount; ++id) ids.emplace(Key(g.names[id]), id);
  g.byId.assign(kFileCount, -1);

  struct Pending {
    uint32_t id;
    uint32_t size;
    std::string guestPath;
    std::string mod;
  };
  std::vector<Pending> pending;
  auto add = [&](const Mod& mod, const std::string& target, const fs::path& host,
                 const std::string& guestPath) {
    auto id = ids.find(Key(target));
    if (id == ids.end()) {
      REXLOG_WARN("mods: {}: {} isn't one of the game's files: left out", mod.name, target);
      return false;
    }
    if (g.byId[id->second] >= 0) {
      REXLOG_INFO("mods: {}: {} is another mod's already", mod.name, target);
      return false;
    }
    std::error_code e;
    const uint64_t size = fs::file_size(host, e);
    if (e || size == 0 || size > 0x7FFFFFFF) {
      REXLOG_WARN("mods: {}: {} is empty or too large: left out", mod.name, target);
      return false;
    }
    g.byId[id->second] = int32_t(pending.size());
    pending.push_back({id->second, uint32_t(size), guestPath, mod.name});
    return true;
  };

  std::vector<uint8_t> pack, loadImg;  // (read when a [replace] needs them)
  bool cacheUsed = false;
  int modsOn = 0;
  for (const Mod& mod : mods) {
    if (!mod.enabled) continue;
    int brought = 0;
    const fs::path files = mod.dir / "files";
    if (fs::is_directory(files, ec)) {
      for (auto it = fs::recursive_directory_iterator(files, ec);
           !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        // (UTF-8: the guest path below goes back to a host path as UTF-8)
        std::string rel = rex::path_to_utf8(fs::relative(it->path(), files, ec));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        std::string guest = rel;
        std::replace(guest.begin(), guest.end(), '/', '\\');
        brought += add(mod, rel, it->path(), "ng2mods:\\" + mod.folder + "\\files\\" + guest);
      }
    }
    for (const auto& [target, source] : mod.replace) {
      auto src = ids.find(Key(source));
      if (src == ids.end() || src->second >= kDiscFileCount) {
        REXLOG_WARN("mods: {}: [replace] {}: {} isn't one of the disc's files", mod.name, target,
                    source);
        continue;
      }
      if (pack.empty()) pack = ReadAll(gameRoot / "pack_img.bin");
      if (loadImg.empty()) loadImg = ReadAll(gameRoot / "load_img.bin");
      Located at;
      // The file's own size (load_img.bin, after a 16-byte header, 8 bytes a
      // file); the archive's is padded.
      const size_t entry = 16 + 8 * size_t(src->second);
      const uint32_t size = loadImg.size() >= entry + 4 ? Be32(loadImg.data() + entry) : 0;
      if (!Locate(pack, src->second, &at) || size == 0 || size > at.size) {
        REXLOG_WARN("mods: {}: [replace] {}: {} isn't in the game's archives", mod.name, target,
                    source);
        continue;
      }
      const std::string leaf = std::to_string(src->second) + ".bin";
      const fs::path cached = cacheDir / leaf;
      std::error_code e;
      if (fs::file_size(cached, e) != size) {
        fs::create_directories(cacheDir, e);
        std::ifstream in(gameRoot / at.archive, std::ios::binary);
        std::vector<char> bytes(size);
        in.seekg(at.offset);
        in.read(bytes.data(), std::streamsize(size));
        std::ofstream out(cached, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), std::streamsize(size));
        out.close();  // (flushed and checked: a full disk mustn't leave a short copy in use)
        if (!in || !out) {
          fs::remove(cached, e);
          REXLOG_WARN("mods: {}: [replace] {}: can't copy {} from {}", mod.name, target, source,
                      at.archive);
          continue;
        }
      }
      cacheUsed = true;
      brought += add(mod, target, cached, "ng2modcache:\\" + leaf);
    }
    if (brought) {
      ++modsOn;
      REXLOG_INFO("mods: on: {}{}{}, {} file(s)", mod.name, mod.version.empty() ? "" : " ",
                  mod.version, brought);
    }
  }

  // The paths the game's CreateFile is given instead, in guest memory.
  if (!pending.empty()) {
    auto* memory = rex::Runtime::instance()->memory();
    size_t total = 0;
    for (const Pending& p : pending) total += p.guestPath.size() + 1;
    const uint32_t base = memory->SystemHeapAlloc(uint32_t(total));
    if (!base || !Mount("\\Device\\Ng2Mods", "ng2mods:", folder) ||
        (cacheUsed && !Mount("\\Device\\Ng2ModCache", "ng2modcache:", cacheDir))) {
      REXLOG_ERROR("mods: can't set up the mods' files: mods off");
      g.byId.assign(kFileCount, -1);
      if (!g.logLoads) return;
      pending.clear();
    }
    uint32_t va = base;
    for (const Pending& p : pending) {
      std::memcpy(memory->TranslateVirtual<char*>(va), p.guestPath.c_str(), p.guestPath.size() + 1);
      g.entries.push_back({p.id, p.size, va, p.mod});
      g.byKey.emplace(Key(g.names[p.id]), g.entries.size() - 1);
      va += uint32_t(p.guestPath.size() + 1);
    }
  }
  REXLOG_INFO("mods: {} on, {} game file(s) replaced{}", modsOn, g.entries.size(),
              g.logLoads ? "; every file the game loads is logged" : "");
  g.active.store(true, std::memory_order_release);
}

}  // namespace ng2::mods

using ng2::mods::g;

// The loader's request for a file (sub_823D61D8): r3 the file id, r10 its
// load-loose byte, just before it is tested. ng2_codegen.toml places it.
void Ng2ModFileRequest(PPCRegister& r3, PPCRegister& r10) {
  if (!g.active.load(std::memory_order_acquire)) return;
  const uint32_t id = r3.u32 & 0xFFFF;
  const bool modded = id < g.byId.size() && g.byId[id] >= 0;
  if (g.logLoads && id < g.names.size()) {
    REXLOG_INFO("mods: the game loads {} {}{}", id, g.names[id], modded ? " (a mod's)" : "");
  }
  if (!modded) return;
  r10.u64 = 1;
  if (g.loaded.fetch_add(1, std::memory_order_relaxed) < 64 && !g.logLoads) {
    REXLOG_INFO("mods: {} from {}", g.names[id], g.entries[size_t(g.byId[id])].mod);
  }
}

// The loose load (sub_823D69F8) just before its CreateFile: r3 the path it
// made, D:\databin\<path> or D:\<path>. A mod's file is opened instead.
void Ng2ModOpenPath(PPCRegister& r3) {
  if (!g.active.load(std::memory_order_acquire) || g.entries.empty()) return;
  size_t len = 0;
  const char* path = ng2::mods::GuestString(r3.u32, 300, &len);
  if (!path) return;
  std::string_view p(path, len);
  for (std::string_view prefix : {std::string_view("d:\\databin\\"), std::string_view("d:\\")}) {
    if (p.size() > prefix.size() && ng2::mods::Key(p.substr(0, prefix.size())) == prefix) {
      auto it = g.byKey.find(ng2::mods::Key(p.substr(prefix.size())));
      if (it != g.byKey.end()) r3.u64 = g.entries[it->second].guestPath;
      return;
    }
  }
}

// A file's size (sub_823D6428), r11 before its rounding to 2 KB, r31 the id:
// at least the mod's file's.
void Ng2ModFileSize(PPCRegister& r11, PPCRegister& r31) {
  if (!g.active.load(std::memory_order_acquire)) return;
  const uint32_t id = r31.u32 & 0xFFFF;
  if (id >= g.byId.size() || g.byId[id] < 0) return;
  const uint32_t size = g.entries[size_t(g.byId[id])].size;
  if (size > r11.u32) r11.u64 = size;
}

// load_img.bin loaded and its offsets made pointers (sub_8366C178): the mods'
// larger files' sizes in its table, for what reads it directly.
void Ng2ModFileTable(PPCRegister& /*r1*/) {
  if (!g.active.load(std::memory_order_acquire) || g.entries.empty()) return;
  const uint32_t buffer = ng2::guest::Load32(ng2::mods::kLoadImg);
  if (!buffer) return;
  const uint32_t sizes = ng2::guest::Load32(buffer);
  if (!sizes || ng2::guest::Load32(buffer + 4) != ng2::mods::kDiscFileCount) {
    REXLOG_WARN("mods: load_img.bin isn't as expected: its sizes left as they are");
    return;
  }
  for (const auto& e : g.entries) {
    if (e.id >= ng2::mods::kDiscFileCount) continue;
    uint8_t* p = ng2::guest::Virtual(sizes + 8 * e.id);
    uint32_t old;
    std::memcpy(&old, p, 4);
    if (e.size > rex::byte_swap(old)) {
      const uint32_t v = rex::byte_swap(e.size);
      std::memcpy(p, &v, 4);
    }
  }
}
