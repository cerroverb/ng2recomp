// ng2 - Ninja Gaiden II static recompilation

#include "windows_setup.h"

#ifdef _WIN32

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <SDL3/SDL.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include "dlc.h"

namespace ng2::windows_setup {
namespace fs = std::filesystem;

namespace {

constexpr const char* kTitle = "Ninja Gaiden II";
// Title Update 3's default.xexp for the World disc (media ID 69F555CB): its
// size (ng2_app.h checks the same) and SHA-1 (the Japanese release's TU3 has
// the same size, and doesn't apply to this disc).
constexpr uintmax_t kTitleUpdate3Size = 2416640;
constexpr uint8_t kTitleUpdate3Sha1[20] = {0xe9, 0xdf, 0x57, 0x87, 0x81, 0x35, 0x5d,
                                           0x9a, 0x25, 0xe1, 0xc0, 0xad, 0xba, 0x8f,
                                           0xbb, 0x85, 0x93, 0x33, 0x45, 0x6a};
constexpr const char* kTitleUpdatePackage = "TU_1A461UL_000000G000000.00000000000O4";

// UTF-8 for SDL and the settings (path::string() is the ANSI code page on
// Windows, and throws for characters it can't hold).
std::string Utf8(const fs::path& path, bool generic = false) {
  std::u8string text = generic ? path.generic_u8string() : path.u8string();
  return std::string(text.begin(), text.end());
}

void Message(const std::string& text, bool error) {
  SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, kTitle,
                           text.c_str(), nullptr);
}

// A question with two buttons: true for the first.
bool Ask(const std::string& text, const char* yes, const char* no) {
  const SDL_MessageBoxButtonData buttons[] = {
      {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes},
      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, no},
  };
  SDL_MessageBoxData box = {};
  box.flags = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
  box.title = kTitle;
  box.message = text.c_str();
  box.numbuttons = 2;
  box.buttons = buttons;
  int chosen = 0;
  return SDL_ShowMessageBox(&box, &chosen) && chosen == 1;
}

fs::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  fs::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
    result = path;
  }
  CoTaskMemFree(path);
  return result;
}

// The shell's file dialog: a folder, or a file; empty if cancelled.
fs::path Pick(bool folder, const wchar_t* title) {
  fs::path result;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) {
    return result;
  }
  FILEOPENDIALOGOPTIONS options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
  dialog->SetTitle(title);
  IShellItem* item = nullptr;
  if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item))) {
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
      result = path;
    }
    CoTaskMemFree(path);
    item->Release();
  }
  dialog->Release();
  return result;
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

// One file from an STFS package (LIVE, PIRS, CON) at its root, as
// tools/stfs_extract.py does; empty if it isn't there.
std::vector<uint8_t> ExtractFromPackage(const std::vector<uint8_t>& d, const std::string& wanted) {
  constexpr uint32_t kBlock = 0x1000, kPerTable = 0xAA, kPerL1 = 0x70E4;
  if (d.size() < 0x3A0 || (std::memcmp(d.data(), "LIVE", 4) && std::memcmp(d.data(), "PIRS", 4) &&
                           std::memcmp(d.data(), "CON ", 4))) {
    return {};
  }
  auto be32 = [&](size_t at) {
    return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
  };
  uint32_t headerSize = be32(0x340);
  const uint8_t* vd = d.data() + 0x379;
  uint32_t shift = (vd[2] & 1) ? 0 : 1;
  uint32_t tableBlocks = uint32_t(vd[3]) | uint32_t(vd[4]) << 8;
  uint32_t tableStart = uint32_t(vd[5]) | uint32_t(vd[6]) << 8 | uint32_t(vd[7]) << 16;
  uint64_t first = (uint64_t(headerSize) + 0xFFF) & ~uint64_t(0xFFF);
  auto backing = [&](uint32_t b) -> uint64_t {
    uint64_t r = (uint64_t((b + kPerTable) / kPerTable) << shift) + b;
    if (b < kPerTable) {
      return r;
    }
    if (b < kPerL1) {
      return r + (uint64_t((b + kPerL1) / kPerL1) << shift);
    }
    return (uint64_t(1) << shift) + r + (uint64_t((b + kPerL1) / kPerL1) << shift);
  };
  auto offset = [&](uint32_t b) { return first + backing(b) * kBlock; };
  auto hashTable = [&](uint32_t b) {
    uint64_t n = uint64_t(b / kPerTable) * (kPerTable + (1u << shift));
    if (b >= kPerTable) {
      n += uint64_t(b / kPerL1 + 1) << shift;
    }
    if (b >= kPerL1) {
      n += uint64_t(1) << shift;
    }
    return first + n * kBlock;
  };
  auto nextBlock = [&](uint32_t b) -> uint32_t {
    uint64_t e = hashTable(b) + (b % kPerTable) * 0x18;
    if (e + 0x18 > d.size()) {
      return 0xFFFFFF;
    }
    return uint32_t(d[e + 0x15]) << 16 | uint32_t(d[e + 0x16]) << 8 | d[e + 0x17];
  };
  uint32_t b = tableStart;
  for (uint32_t t = 0; t < tableBlocks; t++) {
    uint64_t table = offset(b);
    if (table + kBlock > d.size()) {
      return {};
    }
    for (uint32_t i = 0; i < kBlock / 0x40; i++) {
      const uint8_t* e = d.data() + table + i * 0x40;
      uint8_t flags = e[0x28];
      std::string name(reinterpret_cast<const char*>(e), flags & 0x3F);
      bool atRoot = e[0x32] == 0xFF && e[0x33] == 0xFF;
      if (name.empty() || (flags & 0x80) || !atRoot || _stricmp(name.c_str(), wanted.c_str()) != 0) {
        continue;
      }
      if (shift && !(flags & 0x40)) {
        return {};  // fragmented in a two-table package: not supported
      }
      uint32_t size = uint32_t(e[0x34]) << 24 | uint32_t(e[0x35]) << 16 | uint32_t(e[0x36]) << 8 | e[0x37];
      if (size > d.size()) {
        return {};  // can't be in this package
      }
      uint32_t block = uint32_t(e[0x2F]) | uint32_t(e[0x30]) << 8 | uint32_t(e[0x31]) << 16;
      std::vector<uint8_t> data;
      while (data.size() < size) {
        uint64_t at = offset(block);
        if (at >= d.size()) {
          return {};  // truncated
        }
        size_t n = size_t(std::min<uint64_t>(kBlock, d.size() - at));
        data.insert(data.end(), d.begin() + at, d.begin() + at + n);
        block = (flags & 0x40) ? block + 1 : nextBlock(block);
      }
      data.resize(size);
      return data;
    }
    if (shift && tableBlocks > 1) {
      return {};  // a file table over blocks of a two-table package: not supported
    }
    b = nextBlock(b);
  }
  return {};
}

// The game's disc: a disc image (XDVDFS) or a Games on Demand package (the
// header file next to its <name>.data folder of Data#### files), read by
// 2048-byte sector as tools/xdvdfs_extract.py does.
class Disc {
 public:
  static constexpr uint32_t kSector = 2048;

  // Null (and why in *error) if the file is neither.
  static std::unique_ptr<Disc> Open(const fs::path& path, std::string* error) {
    auto disc = std::unique_ptr<Disc>(new Disc());
    std::ifstream file(path, std::ios::binary);
    char head[0x3A0] = {};
    file.read(head, sizeof(head));
    std::error_code ec;
    bool package = !std::memcmp(head, "LIVE", 4) || !std::memcmp(head, "PIRS", 4) ||
                   !std::memcmp(head, "CON ", 4);
    if (package) {
      const auto* h = reinterpret_cast<const uint8_t*>(head);
      fs::path folder = path;
      folder += ".data";
      if (file.gcount() < 0x3A0 || Be32(h + 0x344) != 0x7000 || Be32(h + 0x360) != 0x544307D5 ||
          !fs::is_directory(folder, ec)) {
        *error = "That package isn't Ninja Gaiden II. Choose the Games on Demand package (the "
                 "file next to its .data folder, in 544307D5\\00007000).";
        return nullptr;
      }
      disc->god_ = true;
      disc->enhanced_ = (h[0x379 + 0x18] & 0x40) != 0;
      disc->start_ = uint32_t(h[0x379 + 0x1C]) | uint32_t(h[0x379 + 0x1D]) << 8 |
                     uint32_t(h[0x379 + 0x1E]) << 16;
      // Data0000, Data0001, ...: by number, so a missing one isn't skipped over.
      std::vector<fs::path> names;
      for (auto it = fs::directory_iterator(folder, ec); !ec && it != fs::directory_iterator();
           it.increment(ec)) {
        const auto& entry = *it;
        std::string name = Utf8(entry.path().filename());
        if (name.size() == 8 && name.rfind("Data", 0) == 0 &&
            name.find_first_not_of("0123456789", 4) == std::string::npos) {
          size_t number = std::stoul(name.substr(4));
          names.resize(std::max(names.size(), number + 1));
          names[number] = entry.path();
        }
      }
      if (names.empty()) {
        *error = "The package's .data folder has no Data files.";
        return nullptr;
      }
      for (size_t i = 0; i < names.size(); i++) {
        if (names[i].empty()) {
          char missing[16];
          std::snprintf(missing, sizeof(missing), "Data%04zu", i);
          *error = std::string("The package's .data folder is missing ") + missing + ".";
          return nullptr;
        }
        disc->files_.emplace_back(names[i], std::ios::binary);
      }
      // Where the volume descriptor is tells the layout; a single file
      // carries the package header (0xB000) first.
      if (!disc->enhanced_ && !disc->MagicAt(0x12000) && !disc->MagicAt(0x2000)) {
        disc->extra_ = 0xB000;
      }
    } else {
      disc->files_.emplace_back(path, std::ios::binary);
      bool found = false;
      for (uint64_t base : {0ull, 0x18300000ull, 0xFD90000ull, 0x2080000ull}) {
        disc->base_ = base;
        if (disc->MagicAt(base + 32 * kSector)) {
          found = true;
          break;
        }
      }
      if (!found) {
        *error = "That file isn't a disc image of the game.";
        return nullptr;
      }
    }
    std::vector<uint8_t> volume;
    if (!disc->Read(32, 28, volume) || std::memcmp(volume.data(), kMagic, 20) != 0) {
      *error = "That disc or package can't be read (an unknown layout, or files missing).";
      return nullptr;
    }
    disc->root_sector_ = Le32(volume.data() + 20);
    disc->root_size_ = Le32(volume.data() + 24);
    return disc;
  }

  struct Entry {
    std::string path;  // with '/', UTF-8
    bool directory;
    uint32_t sector;
    uint32_t size;
  };

  // Every file and folder (directory tables are AVL trees of entries); false
  // if a table can't be read or a name isn't safe.
  bool List(std::vector<Entry>& out) {
    budget_ = 100000;  // a damaged directory can't make the walk endless
    table_bytes_ = 0;
    return Walk(root_sector_, root_size_, "", out, 0);
  }

  bool Read(uint32_t sector, size_t size, std::vector<uint8_t>& out) {
    out.resize(size);
    size_t done = 0;
    while (done < size) {
      size_t want = size - done;
      uint64_t at = base_ + uint64_t(sector) * kSector;
      size_t file = 0;
      if (god_) {
        // Sectors are contiguous up to the next hash table: one before
        // every 0x198 sectors, one more before every 0xA1C4 of those.
        constexpr uint64_t kPerL0 = 0x198, kPerL1 = 0xA1C4, kPerFile = 0x14388;
        constexpr uint64_t kFileSize = 0xA290000;
        if (uint64_t(sector) + (enhanced_ ? 2 : 0) < uint64_t(start_) * 2) {
          return false;
        }
        uint64_t block = uint64_t(sector) - uint64_t(start_) * 2 + (enhanced_ ? 2 : 0);
        want = std::min<size_t>(want, (kPerL0 - block % kPerL0) * kSector);
        file = size_t(block / kPerFile);
        block %= kPerFile;
        uint64_t l0 = block / kPerL0 + 1;
        at = block * kSector + (l0 + l0 / kPerL1 + 1) * 0x1000 + extra_;
        if (at >= kFileSize) {
          file += 1;
          at = at % kFileSize + 0x2000;
        }
      }
      if (file >= files_.size()) {
        return false;
      }
      auto& f = files_[file];
      f.clear();
      f.seekg(std::streamoff(at));
      if (!f.read(reinterpret_cast<char*>(out.data() + done), std::streamsize(want))) {
        return false;
      }
      done += want;
      sector += uint32_t((want + kSector - 1) / kSector);
    }
    return true;
  }

 private:
  static constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

  static uint32_t Be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  }
  static uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
  }

  bool MagicAt(uint64_t offset) {
    char magic[20] = {};
    auto& f = files_[0];
    f.clear();
    f.seekg(std::streamoff(offset));
    return f.read(magic, 20) && std::memcmp(magic, kMagic, 20) == 0;
  }

  // A name Windows can store as given (no device names, no trailing dot or space).
  static bool SafeName(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ' ||
        name.find_first_of("/\\:*?\"<>|") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](char c) { return (c & 0x80) || c < 0x20; })) {
      return false;
    }
    std::string stem = name.substr(0, name.find('.'));
    for (const char* device : {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                               "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                               "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"}) {
      if (_stricmp(stem.c_str(), device) == 0) {
        return false;
      }
    }
    return true;
  }

  bool Walk(uint32_t sector, uint32_t size, const std::string& prefix, std::vector<Entry>& out,
            int depth) {
    std::vector<uint8_t> table;
    table_bytes_ += size;
    if (depth > 32 || size > (64u << 20) || table_bytes_ > (256ull << 20) ||
        !Read(sector, size, table)) {
      return false;
    }
    // Subfolders are walked after this table is done with (and freed).
    std::vector<Entry> folders;
    std::vector<size_t> stack = {0};
    std::vector<bool> seen(table.size() / 4 + 1);
    while (!stack.empty()) {
      size_t off = stack.back();
      stack.pop_back();
      if (off + 14 > table.size() || seen[off / 4]) {
        continue;
      }
      seen[off / 4] = true;
      const uint8_t* e = table.data() + off;
      uint16_t left = uint16_t(e[0] | e[1] << 8), right = uint16_t(e[2] | e[3] << 8);
      if (left == 0xFFFF) {
        continue;  // padding
      }
      if (--budget_ < 0) {
        return false;
      }
      uint8_t attributes = e[12], length = e[13];
      if (off + 14 + length > table.size()) {
        return false;
      }
      std::string name(reinterpret_cast<const char*>(e + 14), length);
      if (!SafeName(name)) {
        return false;
      }
      Entry entry{prefix.empty() ? name : prefix + "/" + name, (attributes & 0x10) != 0, Le32(e + 4),
                  Le32(e + 8)};
      out.push_back(entry);
      if (entry.directory && entry.size) {
        folders.push_back(entry);
      }
      if (left) {
        stack.push_back(size_t(left) * 4);
      }
      if (right) {
        stack.push_back(size_t(right) * 4);
      }
    }
    table = {};
    for (const auto& folder : folders) {
      if (!Walk(folder.sector, folder.size, folder.path, out, depth + 1)) {
        return false;
      }
    }
    return true;
  }

  bool god_ = false, enhanced_ = false;
  uint32_t start_ = 0, root_sector_ = 0, root_size_ = 0;
  int budget_ = 0;
  uint64_t table_bytes_ = 0;
  uint64_t base_ = 0, extra_ = 0;
  std::vector<std::ifstream> files_;
};

// The shell's progress window, for the long copies.
class Progress {
 public:
  Progress(const wchar_t* title, const wchar_t* line) {
    if (SUCCEEDED(CoCreateInstance(CLSID_ProgressDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog_)))) {
      dialog_->SetTitle(title);
      dialog_->SetLine(1, line, FALSE, nullptr);
      dialog_->StartProgressDialog(nullptr, nullptr, PROGDLG_NORMAL | PROGDLG_AUTOTIME, nullptr);
    }
  }
  ~Progress() {
    if (dialog_) {
      dialog_->StopProgressDialog();
      dialog_->Release();
    }
  }
  // False once the player pressed Cancel.
  bool Update(uint64_t done, uint64_t total) {
    if (!dialog_) {
      return true;
    }
    dialog_->SetProgress64(done, total);
    return !dialog_->HasUserCancelled();
  }

 private:
  IProgressDialog* dialog_ = nullptr;
};

// The disc's files into dest (a new folder, renamed into place when
// complete): an empty string, or what went wrong.
std::string ExtractDisc(const fs::path& image, const fs::path& dest) {
  std::string error;
  auto disc = Disc::Open(image, &error);
  if (!disc) {
    return error;
  }
  std::vector<Disc::Entry> entries;
  if (!disc->List(entries)) {
    return "That disc or package can't be read (a damaged directory).";
  }
  // The console's system update ($SystemUpdate) isn't the game's.
  std::erase_if(entries, [](const Disc::Entry& e) {
    return _strnicmp(e.path.c_str(), "$SystemUpdate", 13) == 0 &&
           (e.path.size() == 13 || e.path[13] == '/');
  });
  bool game = false;
  uint64_t total = 0;
  for (const auto& e : entries) {
    game |= !e.directory && _stricmp(e.path.c_str(), "default.xex") == 0;
    total += e.directory ? 0 : e.size;
  }
  if (!game) {
    return "There's no default.xex on that disc.";
  }
  std::error_code ec;
  fs::path part = dest;
  part += ".part";
  fs::remove_all(part, ec);
  fs::create_directories(part, ec);
  auto space = fs::space(part, ec);
  if (!ec && space.available < total + (256ull << 20)) {
    fs::remove_all(part, ec);
    return "Not enough free space in " + Utf8(dest.parent_path()) + ": the game needs " +
           std::to_string((total >> 30) + 1) + " GB.";
  }
  Progress progress(L"Ninja Gaiden II", L"Copying the game from the disc...");
  uint64_t done = 0;
  std::vector<uint8_t> chunk;
  std::ofstream out;
  auto fail = [&](std::string why) {
    out.close();  // an open file can't be removed
    fs::remove_all(part, ec);
    return why;
  };
  for (const auto& e : entries) {
    fs::path to = part / fs::path(std::u8string(e.path.begin(), e.path.end()));
    if (e.directory) {
      fs::create_directories(to, ec);
      continue;
    }
    fs::create_directories(to.parent_path(), ec);
    out.open(to, std::ios::binary);
    constexpr uint32_t kChunk = 4u << 20;
    for (uint64_t at = 0; at < e.size; at += kChunk) {
      uint32_t n = uint32_t(std::min<uint64_t>(kChunk, e.size - at));
      if (!disc->Read(e.sector + uint32_t(at / Disc::kSector), n, chunk)) {
        return fail("That disc or package can't be read (" + e.path + ").");
      }
      if (!out.write(reinterpret_cast<const char*>(chunk.data()), n)) {
        return fail("Can't write to " + Utf8(part) + ".");
      }
      done += n;
      if (!progress.Update(done, total)) {
        return fail("cancel");
      }
    }
    out.close();
    if (!out) {
      return fail("Can't write to " + Utf8(part) + ".");
    }
  }
  // The new copy replaces the old one, which is set aside first and only
  // deleted once its downloadable content and title update are moved over.
  fs::path old = dest;
  old += ".old";
  // One left by a swap that failed halfway: its content first.
  for (const char* keep : {"dlc", "default.xexp"}) {
    if (!fs::exists(dest / keep, ec) && fs::exists(old / keep, ec)) {
      fs::rename(old / keep, part / keep, ec);
    }
  }
  fs::remove_all(old, ec);
  if (fs::exists(dest, ec)) {
    fs::rename(dest, old, ec);
    if (ec) {
      return fail("Can't replace " + Utf8(dest) + ": close what has it open and try again.");
    }
  }
  fs::rename(part, dest, ec);
  if (ec) {
    std::error_code back;
    fs::rename(old, dest, back);
    return fail("Can't write to " + Utf8(dest) + ".");
  }
  bool kept = true;
  for (const char* keep : {"dlc", "default.xexp"}) {
    if (fs::exists(old / keep, ec)) {
      fs::rename(old / keep, dest / keep, ec);
      kept &= !ec;
    }
  }
  if (kept) {
    fs::remove_all(old, ec);
  }
  return {};
}

// Downloadable content, offered once: the packages in a chosen folder (at any
// depth, as the console keeps them) copied into the game folder's dlc\,
// which the game installs from at start.
void OfferDlc(const fs::path& data, const fs::path& game) {
  fs::path asked = data / "dlc_asked";
  std::error_code ec;
  if (fs::exists(asked, ec)) {
    return;
  }
  bool add = Ask("Add downloadable content? Choose the folder with the packages (for example "
                 "544307D5\\00000002). You can also put them in the dlc folder inside the game "
                 "folder later.",
                 "Add DLC", "Skip");
  std::ofstream(asked) << "1\n";  // answered: not asked again
  if (!add) {
    return;
  }
  fs::path from = Pick(true, L"The folder with the downloadable content");
  if (from.empty()) {
    return;
  }
  // One of each name, none from the game's own dlc folder. Only files that
  // can be packages are opened (no extension, 40 KB or more), and Windows'
  // own folders are passed over, so a whole drive is quick to search.
  std::vector<fs::path> packages;
  std::wstring target = fs::weakly_canonical(game / "dlc", ec).wstring() + L"\\";
  constexpr const wchar_t* kSkip[] = {L"Windows", L"Program Files", L"Program Files (x86)",
                                      L"ProgramData", L"AppData", L"$Recycle.Bin",
                                      L"System Volume Information", L"node_modules", L".git"};
  // Folder by folder: one that can't be listed (access denied, a drive with
  // no disc, a cloud placeholder) is passed over and the rest still searched.
  std::vector<fs::path> folders = {from};
  while (!folders.empty()) {
    fs::path folder = std::move(folders.back());
    folders.pop_back();
    std::error_code le;
    for (auto it = fs::directory_iterator(folder, le); !le && it != fs::directory_iterator();
         it.increment(le)) {
      std::error_code fe;
      const fs::path& path = it->path();
      if (it->is_directory(fe)) {
        std::wstring name = path.filename().wstring();
        bool skip = it->is_symlink(fe) || std::any_of(std::begin(kSkip), std::end(kSkip),
                                                      [&](const wchar_t* s) {
                                                        return _wcsicmp(name.c_str(), s) == 0;
                                                      });
        if (!skip) {
          folders.push_back(path);
        }
        continue;
      }
      if (!it->is_regular_file(fe) || path.has_extension() || it->file_size(fe) < 0xA000 ||
          !dlc::IsGameDlc(path)) {
        continue;
      }
      std::wstring where = fs::weakly_canonical(path, fe).wstring();
      if (_wcsnicmp(where.c_str(), target.c_str(), target.size()) == 0) {
        continue;
      }
      bool seen = std::any_of(packages.begin(), packages.end(), [&](const fs::path& p) {
        return _wcsicmp(p.filename().c_str(), path.filename().c_str()) == 0;
      });
      if (!seen) {
        packages.push_back(path);
      }
    }
  }
  if (packages.empty()) {
    Message("There's no downloadable content for this game in " + Utf8(from) + ".", true);
    return;
  }
  fs::create_directories(game / "dlc", ec);
  int copied = 0;
  for (const auto& package : packages) {
    if (fs::copy_file(package, game / "dlc" / package.filename(),
                      fs::copy_options::overwrite_existing, ec)) {
      copied++;
    }
  }
  if (copied < int(packages.size())) {
    Message("Can't copy the downloadable content to " + Utf8(game / "dlc") + ".", true);
  }
}

bool IsGameFolder(const fs::path& dir) {
  std::error_code ec;
  return !dir.empty() && fs::is_regular_file(dir / "default.xex", ec);
}

// Whether these bytes are Title Update 3's default.xexp (Windows' own SHA-1).
bool IsTitleUpdate3(const std::vector<uint8_t>& xexp) {
  if (xexp.size() != kTitleUpdate3Size) {
    return false;
  }
  uint8_t digest[20] = {};
  BCRYPT_ALG_HANDLE sha1 = nullptr;
  bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&sha1, BCRYPT_SHA1_ALGORITHM, nullptr, 0)) &&
            BCRYPT_SUCCESS(BCryptHash(sha1, nullptr, 0, const_cast<PUCHAR>(xexp.data()),
                                      ULONG(xexp.size()), digest, sizeof(digest)));
  if (sha1) {
    BCryptCloseAlgorithmProvider(sha1, 0);
  }
  return ok && std::memcmp(digest, kTitleUpdate3Sha1, sizeof(digest)) == 0;
}

// Title Update 3 next to default.xex: asked for once (its package, or an
// extracted default.xexp) and written there, over an older update.
bool EnsureTitleUpdate(const fs::path& game) {
  std::error_code ec;
  if (fs::file_size(game / "default.xexp", ec) == kTitleUpdate3Size &&
      IsTitleUpdate3(ReadFile(game / "default.xexp"))) {
    return true;
  }
  Message(std::string("This port runs the game with Title Update 3 (for the World release, media "
                      "ID 69F555CB). Choose the update: its package (") +
              kTitleUpdatePackage + ") or an extracted default.xexp.",
          false);
  fs::path update = Pick(false, L"Title Update 3 (package or default.xexp)");
  if (update.empty()) {
    return false;
  }
  constexpr uintmax_t kMaxPackage = 64ull << 20;  // the update is a few MB
  constexpr const char* kWrong =
      "That isn't Title Update 3 for this disc (the World release's: TU_1A461UL_000000G000000."
      "00000000000O4).";
  if (fs::file_size(update, ec) > kMaxPackage || ec) {
    Message(kWrong, true);
    return false;
  }
  std::vector<uint8_t> file = ReadFile(update);
  std::vector<uint8_t> xexp = file.size() >= 4 && std::memcmp(file.data(), "XEX2", 4) == 0
                                  ? file
                                  : ExtractFromPackage(file, "default.xexp");
  if (!IsTitleUpdate3(xexp)) {
    Message(kWrong, true);
    return false;
  }
  fs::path part = game / "default.xexp.part";
  bool written = false;
  {
    std::ofstream out(part, std::ios::binary);
    written = out.write(reinterpret_cast<const char*>(xexp.data()), std::streamsize(xexp.size())) &&
              out.flush();
  }
  if (written) {
    fs::rename(part, game / "default.xexp", ec);
  }
  if (!written || ec) {
    fs::remove(part, ec);
    Message("Can't write to " + Utf8(game) + ": copy default.xexp there yourself.", true);
    return false;
  }
  return true;
}

}  // namespace

bool ConfigurePaths(rex::PathConfig& paths) {
  // The file dialogs and progress window are COM objects.
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  fs::path data = KnownFolder(FOLDERID_LocalAppData) / "NinjaGaiden2";
  std::error_code ec;
  // Portable mode: with portable.txt next to ng2.exe, everything (settings,
  // saves, the log, a copied game) goes in a userdata folder next to it. The
  // first start offers it when that folder can be written (not under
  // Program Files).
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  fs::path beside = fs::path(exe).parent_path();
  // (a Normal answer is kept as portable_asked in the usual folder)
  bool firstStart = !fs::exists(data / "game_dir.txt", ec) && !fs::exists(data / "portable_asked", ec) &&
                    !fs::exists(beside / "portable.txt", ec);
  if (firstStart && !std::getenv("NG2_CONFIG")) {
    fs::path probe = beside / "portable.probe";
    bool writable = std::ofstream(probe).good();
    fs::remove(probe, ec);
    if (writable &&
        Ask("Keep the settings, saves and the installed game in a userdata folder next to ng2.exe ("
                + Utf8(beside) + "), instead of in your user folder?",
            "Portable", "Normal")) {
      std::ofstream(beside / "portable.txt");
    } else {
      fs::create_directories(data, ec);
      std::ofstream(data / "portable_asked");
    }
  }
  if (fs::exists(beside / "portable.txt", ec)) {
    data = beside / "userdata";
  }
  fs::create_directories(data / "user", ec);

  // Settings, saves and the log in %LOCALAPPDATA% (the program's folder may
  // not be writable), unless given.
  const char* config = std::getenv("NG2_CONFIG");
  if (!config || !*config) {
    paths.config_path = data / "ng2.toml";
  }
  auto given = [](const char* name) {
    const auto* info = rex::cvar::GetFlagInfo(name);
    return info && info->source != rex::cvar::Source::kDefault;
  };
  if (!given("user_data_root")) {
    paths.user_data_root = data / "user";
  }
  if (!given("cache_root")) {
    paths.cache_root = data / "cache";
  }
  const auto* logFile = rex::cvar::GetFlagInfo("log_file");
  if (logFile && logFile->source == rex::cvar::Source::kDefault) {
    // Forward slashes: the settings file is TOML, where a backslash escapes.
    rex::cvar::SetFlagByName("log_file", Utf8(data / "ng2.log", true));
  }

  // The game folder: given, remembered, or asked for. The setup's own folder
  // (remembered or chosen) gets the downloadable content question; one given
  // on the command line (a launcher, a test run) has its dlc folder for that.
  fs::path remembered = data / "game_dir.txt";
  bool from_setup = false;
  if (!given("game_data_root")) {
    std::ifstream in(remembered);
    std::string line;
    if (std::getline(in, line)) {
      paths.game_data_root = fs::path(std::u8string(line.begin(), line.end()));
      from_setup = true;
      // The copy in the data folder, where the remembered folder is gone (a
      // portable folder moved, a drive given another letter).
      if (!IsGameFolder(paths.game_data_root) && IsGameFolder(data / "game")) {
        paths.game_data_root = data / "game";
        std::ofstream(remembered, std::ios::trunc) << Utf8(paths.game_data_root) << '\n';
      }
    }
  }
  if (!IsGameFolder(paths.game_data_root)) {
    Message("Choose your copy of the game: a disc image (.iso), the Games on Demand package "
            "(the file in 544307D5\\00007000, next to its .data folder), or default.xex in an "
            "extracted copy. A disc image or package is copied into " +
                Utf8(data / "game") + ".",
            false);
    fs::path chosen = Pick(false, L"The game: disc image, Games on Demand package or default.xex");
    if (chosen.empty()) {
      return false;
    }
    if (_wcsicmp(chosen.filename().c_str(), L"default.xex") == 0) {
      chosen = chosen.parent_path();
    } else {
      std::string error = ExtractDisc(chosen, data / "game");
      if (error == "cancel") {
        return false;
      }
      if (!error.empty()) {
        Message(error, true);
        return false;
      }
      chosen = data / "game";
    }
    if (!IsGameFolder(chosen)) {
      Message("There's no default.xex in " + Utf8(chosen) + ".", true);
      return false;
    }
    paths.game_data_root = chosen;
    std::ofstream(remembered) << Utf8(chosen) << '\n';
    from_setup = true;
  }
  if (!EnsureTitleUpdate(paths.game_data_root)) {
    return false;
  }
  if (from_setup) {
    OfferDlc(data, paths.game_data_root);
  }
  return true;
}

// Power throttling (EcoQoS) off: Windows may run a throttled process slower,
// on its efficient cores, and ignore its timer requests. And the system timer
// at 1 ms, as SDL already asks for by default (SDL_HINT_TIMER_RESOLUTION): so
// the game's 1 ms sleeps and timeouts don't last a 15.6 ms tick if that hint
// is ever changed.
void TuneProcess() {
  const char* v = std::getenv("NG2_TIMER_PERIOD");
  if (v && v[0] == '0') {
    REXLOG_INFO("Windows: power throttling left as it is (NG2_TIMER_PERIOD=0; SDL still sets a 1 ms timer)");
    return;
  }
  PROCESS_POWER_THROTTLING_STATE power{};
  power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  power.ControlMask =
      PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  power.StateMask = 0;
  bool unthrottled =
      SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &power, sizeof(power)) != 0;
  if (!unthrottled) {
    // (Windows 10: the timer flag is Windows 11's, and a mask with it is refused whole)
    power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    unthrottled = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &power, sizeof(power)) != 0;
  }
  bool period = false;
  if (HMODULE winmm = LoadLibraryA("winmm.dll")) {
    using PeriodFn = UINT(WINAPI*)(UINT);
    auto begin = reinterpret_cast<PeriodFn>(GetProcAddress(winmm, "timeBeginPeriod"));
    period = begin && begin(1) == 0;
  }
  REXLOG_INFO("Windows: power throttling {}, system timer {}", unthrottled ? "off" : "unchanged",
              period ? "at 1 ms" : "unchanged");
}

}  // namespace ng2::windows_setup

#endif  // _WIN32
