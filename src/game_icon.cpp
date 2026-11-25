// ng2 - Ninja Gaiden II static recompilation
//
// The window's icon made from the game's own title art (game_icon.h). The
// source is sprite\demo\Demo.spr, the title screen's sprite pack (file 4373
// of the archives, in other.ng2; on another disc's numbering the pack is
// found by its textures). Its texpack is an XPR2 package of Xbox 360 tiled
// textures, 8-bit indices each with a 256-colour palette texture:
//   t_0x20004 (890x224)  the title logo "NINJA GAIDEN", chrome letters on a
//                        dark metal plate
//   t_0x20006 (413x499)  the red brush-stroke "II"
// The N is cut from the logo with its part of the plate, the plate's left
// bevel mirrored onto the cut, and laid left on a black rounded tile; the II
// stands over the plate's right edge with a red glow behind and a soft shadow
// under it. Drawn at 512 and reduced, with the small sizes' own tweaks: from
// 48 down the chrome rim is brightened, from 32 down the II is widened and
// its frayed edges made solid.
//
// Every step is done as Pillow does it (resampling, box-blur Gaussian,
// alpha compositing, rounding), so the result matches the mock-ups the icon
// was chosen from (out/icon/make_icons.py, candidate n_plate_over_ii2).

#include "game_icon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string_view>

namespace fs = std::filesystem;

namespace ng2::game_icon {
namespace {

// ---- The archives --------------------------------------------------------

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

struct Entry {
  std::string archive;
  uint32_t offset = 0, padded = 0, size = 0;
};

bool ReadFile(const fs::path& path, std::vector<uint8_t>* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool ReadPart(const fs::path& path, uint32_t offset, uint32_t size, std::vector<uint8_t>* out) {
  // At most what the archive holds there, and never more than 64 MB (the
  // title's pack is 12 MB): a damaged load_img.bin can name any size.
  std::error_code ec;
  uint64_t total = fs::file_size(path, ec);
  if (ec || offset >= total) return false;
  size = uint32_t(std::min<uint64_t>({size, total - offset, 64u << 20}));
  std::ifstream f(path, std::ios::binary);
  if (!f || !f.seekg(offset)) return false;
  out->resize(size);
  f.read(reinterpret_cast<char*>(out->data()), size);
  out->resize(size_t(f.gcount()));
  return true;
}

// Every disc file's place: pack_img.bin's four tables (ids, counts, offsets,
// sizes), each a word per slot pointing at a word per archive, as mods.cpp's
// Locate reads them, and the size from load_img.bin (after 16 bytes, 8 a
// file; the archives' sizes are padded). A file in two places counts where it
// is found first, as with the game's loader. (All of them, not one id: the
// pack is looked for among other.ng2's files when it isn't file 4373.)
bool Index(const fs::path& game, std::map<uint32_t, Entry>* out) {
  std::vector<uint8_t> pack, load;
  if (!ReadFile(game / "pack_img.bin", &pack) || !ReadFile(game / "load_img.bin", &load)) {
    return false;
  }
  bool ok = true;
  auto w = [&](uint32_t at) -> uint32_t {
    if (size_t(at) + 4 > pack.size()) {
      ok = false;
      return 0;
    }
    return Be32(pack.data() + at);
  };
  std::vector<std::vector<std::string>> slots = {{"cmn.ng2"}, {"char.ng2"}, {}, {"rtm.ng2"},
                                                 {"other.ng2"}, {"other_rtm.ng2"}};
  for (int c = 1; c <= 14; ++c) {
    char name[24];
    std::snprintf(name, sizeof(name), "s_chap_%02d.ng2", c);
    slots[2].push_back(name);
  }
  uint32_t table[4];
  for (uint32_t i = 0; i < 4; ++i) table[i] = w(i * 4);
  for (uint32_t slot = 0; slot < slots.size() && ok; ++slot) {
    uint32_t p[4];
    for (int i = 0; i < 4; ++i) p[i] = w(table[i] + 4 * slot);
    for (uint32_t a = 0; a < slots[slot].size() && ok; ++a) {
      uint32_t ids = w(p[0] + 4 * a), count = w(p[1] + 4 * a), offs = w(p[2] + 4 * a),
               sizes = w(p[3] + 4 * a);
      if (count > 0x10000) return false;
      for (uint32_t k = 0; k < count && ok; ++k) {
        uint32_t fid = w(ids + 4 * k);
        if (!ok || out->count(fid)) continue;
        size_t e = 16 + size_t(8) * fid;
        Entry entry{slots[slot][a], w(offs + 4 * k), w(sizes + 4 * k),
                    e + 4 <= load.size() ? Be32(load.data() + e) : 0};
        (*out)[fid] = std::move(entry);
      }
    }
  }
  return ok;
}

// ---- Pictures ------------------------------------------------------------

struct Rgba {
  int w = 0, h = 0;
  std::vector<uint8_t> p;  // R G B A rows
  Rgba() = default;
  Rgba(int w_, int h_) : w(w_), h(h_), p(size_t(w_) * h_ * 4, 0) {}
  uint8_t* at(int x, int y) { return &p[(size_t(y) * w + x) * 4]; }
  const uint8_t* at(int x, int y) const { return &p[(size_t(y) * w + x) * 4]; }
};

struct Gray {
  int w = 0, h = 0;
  std::vector<uint8_t> p;
  Gray() = default;
  Gray(int w_, int h_) : w(w_), h(h_), p(size_t(w_) * h_, 0) {}
};

// ---- The sprite pack -----------------------------------------------------

// Xbox 360 tiled 2D texel's byte offset; bytes per texel 1 (lg 0) or 4 (lg 2),
// the stored pitch the width rounded up to 32.
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t lg) {
  uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (lg + 7);
  uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << lg;
  uint32_t off = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((off & ~0x1FFu) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F);
}

struct Texture {
  std::string name;
  uint32_t tiled, fmt, endian, at, w, h;
};

// A sprite pack (sprpack): its first section is a texpack holding an XPR2
// package of TX2D textures, k_8 (format 2) or k_8_8_8_8 (format 6).
struct Spr {
  const std::vector<uint8_t>* d = nullptr;
  size_t data0 = 0;
  std::vector<Texture> tex;
  std::map<std::string, size_t> byname;

  bool Parse(const std::vector<uint8_t>& data) {
    d = &data;
    auto be = [&](size_t at, uint32_t* v) {
      if (at + 4 > data.size()) return false;
      *v = Be32(data.data() + at);
      return true;
    };
    auto is = [&](size_t at, std::string_view s) {
      return at + s.size() <= data.size() && std::memcmp(data.data() + at, s.data(), s.size()) == 0;
    };
    uint32_t first, t, xo;
    if (!is(0, "sprpack") || !be(0x20, &first) || !be(first, &t) || !is(t, "texpack") ||
        !be(size_t(t) + 0x18, &xo)) {
      return false;
    }
    size_t x = size_t(t) + xo;
    uint32_t hsize, n;
    if (!is(x, "XPR2") || !be(x + 4, &hsize) || !be(x + 12, &n) || n > 4096) return false;
    size_t base = x + 12;
    data0 = (x + 12 + hsize + 0x7FF) & ~size_t(0x7FF);  // the texels follow the header, on 2048
    for (uint32_t i = 0; i < n; ++i) {
      size_t e = x + 16 + size_t(i) * 16;
      uint32_t off, name;
      if (!be(e + 4, &off) || !be(e + 12, &name)) return false;
      if (!is(e, "TX2D")) continue;
      size_t s = base + name, end = s;
      while (end < data.size() && data[end]) ++end;
      if (end >= data.size()) return false;
      uint32_t f[6];  // the GPU's texture fetch constant
      for (int k = 0; k < 6; ++k) {
        if (!be(base + off + 28 + 4 * k, &f[k])) return false;
      }
      Texture tx{std::string(reinterpret_cast<const char*>(data.data() + s), end - s),
                 f[0] >> 31, f[1] & 63, (f[1] >> 6) & 3, (f[1] >> 12) << 12,
                 (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1};
      byname[tx.name] = tex.size();
      tex.push_back(std::move(tx));
    }
    return true;
  }

  const Texture* Find(const std::string& name) const {
    auto it = byname.find(name);
    return it == byname.end() ? nullptr : &tex[it->second];
  }

  // A texture's texels: 1 byte (k_8) or R G B A (k_8_8_8_8) each.
  bool Texels(const Texture& e, std::vector<uint8_t>* out) const {
    uint32_t bpp = e.fmt == 2 ? 1 : e.fmt == 6 ? 4 : 0;
    if (!bpp || !e.tiled) return false;
    uint32_t pitch = (e.w + 31) & ~31u, lg = bpp == 4 ? 2 : 0;
    size_t start = data0 + e.at;
    if (start > d->size()) return false;
    size_t avail = std::min(d->size() - start,
                            (size_t(pitch) * ((e.h + 31) & ~31u) + 4096) * bpp);
    const uint8_t* raw = d->data() + start;
    out->resize(size_t(e.w) * e.h * bpp);
    for (uint32_t y = 0; y < e.h; ++y) {
      for (uint32_t x = 0; x < e.w; ++x) {
        size_t at = TiledOffset(x, y, pitch, lg);
        if (at + bpp > avail) return false;
        uint8_t* o = &(*out)[(size_t(y) * e.w + x) * bpp];
        if (bpp == 1) {
          o[0] = raw[at];
        } else if (e.endian == 2) {  // 8in32: B G R A once swapped
          o[0] = raw[at + 1], o[1] = raw[at + 2], o[2] = raw[at + 3], o[3] = raw[at];
        } else {
          o[0] = raw[at + 2], o[1] = raw[at + 1], o[2] = raw[at], o[3] = raw[at + 3];
        }
      }
    }
    return true;
  }

  // A k_8 texture through its PALETTE_<name> texture, or a k_8_8_8_8 one.
  bool Picture(const std::string& name, Rgba* out) const {
    const Texture* e = Find(name);
    std::vector<uint8_t> texels;
    if (!e || !Texels(*e, &texels)) return false;
    *out = Rgba(int(e->w), int(e->h));
    if (e->fmt == 6) {
      out->p = std::move(texels);
      return true;
    }
    const Texture* pe = Find("PALETTE_" + name);
    std::vector<uint8_t> pal;
    if (!pe || pe->fmt != 6 || !Texels(*pe, &pal) || pal.size() < 256 * 4) return false;
    for (size_t i = 0; i < texels.size(); ++i) {
      std::memcpy(&out->p[i * 4], &pal[size_t(texels[i]) * 4], 4);
    }
    return true;
  }
};

// The title screen's sprite pack: file 4373, or (another disc's numbering) the
// sprite pack in other.ng2 with the title menu's textures.
bool TitlePack(const fs::path& game, std::vector<uint8_t>* data, Spr* spr, std::string* why) {
  std::map<uint32_t, Entry> ix;
  if (!Index(game, &ix)) {
    *why = "pack_img.bin or load_img.bin can't be read";
    return false;
  }
  const std::pair<const char*, std::pair<uint32_t, uint32_t>> need[] = {
      {"t_0x20004", {890, 224}}, {"t_0x20006", {413, 499}}, {"t_0x20001", {1280, 720}}};
  auto ok = [&](uint32_t fid) {
    auto it = ix.find(fid);
    if (it == ix.end()) return false;
    const Entry& e = it->second;
    std::vector<uint8_t> head;
    if (!ReadPart(game / e.archive, e.offset, 7, &head) || head.size() != 7 ||
        std::memcmp(head.data(), "sprpack", 7) != 0) {
      return false;
    }
    if (!ReadPart(game / e.archive, e.offset, e.size ? e.size : e.padded, data)) return false;
    *spr = Spr();
    if (!spr->Parse(*data) || !spr->Find("TID_TITLE_MENU")) return false;
    for (const auto& [name, wh] : need) {
      const Texture* t = spr->Find(name);
      if (!t || t->w != wh.first || t->h != wh.second) return false;
    }
    return true;
  };
  if (ok(4373)) return true;
  for (const auto& [fid, e] : ix) {
    if (fid != 4373 && e.archive == "other.ng2" && e.size > (4u << 20) && ok(fid)) return true;
  }
  *why = "the title screen's sprite pack isn't in the archives";
  return false;
}

// ---- Pillow's operations -------------------------------------------------

// Python's round(): halves to even (the default rounding mode).
int Round(double v) { return int(std::nearbyint(v)); }

// Pillow's two-pass resampling (Resample.c): coefficients in doubles, made
// fixed point with 22 bits, horizontal pass then vertical, 8 bits between.
constexpr int kPrecision = 22;

struct Coeffs {
  int ksize = 0;
  std::vector<int> xmin, count;
  std::vector<int32_t> k;
};

double Sinc(double x) {
  if (x == 0.0) return 1.0;
  x *= 3.14159265358979323846;
  return std::sin(x) / x;
}
double Lanczos(double x) { return (-3.0 <= x && x < 3.0) ? Sinc(x) * Sinc(x / 3) : 0.0; }
double Box(double x) { return (x > -0.5 && x <= 0.5) ? 1.0 : 0.0; }

Coeffs Precompute(int in_size, int out_size, bool lanczos) {
  double scale = double(in_size) / out_size, filterscale = std::max(scale, 1.0);
  double support = (lanczos ? 3.0 : 0.5) * filterscale;
  Coeffs c;
  c.ksize = int(std::ceil(support)) * 2 + 1;
  c.xmin.resize(out_size), c.count.resize(out_size), c.k.assign(size_t(out_size) * c.ksize, 0);
  std::vector<double> kk(c.ksize);
  for (int xx = 0; xx < out_size; ++xx) {
    double center = (xx + 0.5) * scale, ww = 0.0, ss = 1.0 / filterscale;
    int xmin = std::max(int(center - support + 0.5), 0);
    int xmax = std::min(int(center + support + 0.5), in_size) - xmin;
    for (int x = 0; x < xmax; ++x) {
      double w = (lanczos ? Lanczos : Box)((x + xmin - center + 0.5) * ss);
      kk[x] = w, ww += w;
    }
    for (int x = 0; x < xmax; ++x) {
      double v = ww != 0.0 ? kk[x] / ww : kk[x];
      c.k[size_t(xx) * c.ksize + x] =
          v < 0 ? int(-0.5 + v * (1 << kPrecision)) : int(0.5 + v * (1 << kPrecision));
    }
    c.xmin[xx] = xmin, c.count[xx] = xmax;
  }
  return c;
}

uint8_t Clip8(int v) {
  if (v >= (1 << kPrecision << 8)) return 255;
  if (v <= 0) return 0;
  return uint8_t(v >> kPrecision);
}

// One pass over channels-wide pixels: along x (horizontal) or y.
std::vector<uint8_t> ResamplePass(const std::vector<uint8_t>& in, int w, int h, int channels,
                                  int out_len, bool horizontal, bool lanczos) {
  Coeffs c = Precompute(horizontal ? w : h, out_len, lanczos);
  int ow = horizontal ? out_len : w, oh = horizontal ? h : out_len;
  std::vector<uint8_t> out(size_t(ow) * oh * channels);
  for (int y = 0; y < oh; ++y) {
    for (int x = 0; x < ow; ++x) {
      int i = horizontal ? x : y;
      const int32_t* k = &c.k[size_t(i) * c.ksize];
      for (int ch = 0; ch < channels; ++ch) {
        int ss = 1 << (kPrecision - 1);
        for (int j = 0; j < c.count[i]; ++j) {
          int sx = horizontal ? c.xmin[i] + j : x, sy = horizontal ? y : c.xmin[i] + j;
          ss += in[(size_t(sy) * w + sx) * channels + ch] * k[j];
        }
        out[(size_t(y) * ow + x) * channels + ch] = Clip8(ss);
      }
    }
  }
  return out;
}

std::vector<uint8_t> Resample(std::vector<uint8_t> px, int w, int h, int channels, int nw, int nh,
                              bool lanczos) {
  if (nw != w) px = ResamplePass(px, w, h, channels, nw, true, lanczos);
  if (nh != h) px = ResamplePass(px, nw, h, channels, nh, false, lanczos);
  return px;
}

// An RGBA picture resized: premultiplied for it (RGBa), as Image.resize does.
Rgba Resize(const Rgba& im, int nw, int nh, bool lanczos = true) {
  if (nw == im.w && nh == im.h) return im;
  std::vector<uint8_t> pm(im.p.size());
  for (size_t i = 0; i < im.p.size(); i += 4) {
    unsigned a = im.p[i + 3];
    for (int c = 0; c < 3; ++c) {
      unsigned t = im.p[i + c] * a + 128;
      pm[i + c] = uint8_t(((t >> 8) + t) >> 8);
    }
    pm[i + 3] = uint8_t(a);
  }
  Rgba out(nw, nh);
  out.p = Resample(std::move(pm), im.w, im.h, 4, nw, nh, lanczos);
  for (size_t i = 0; i < out.p.size(); i += 4) {
    unsigned a = out.p[i + 3];
    if (a == 255 || a == 0) continue;
    for (int c = 0; c < 3; ++c) out.p[i + c] = uint8_t(std::min(255u, 255u * out.p[i + c] / a));
  }
  return out;
}

Gray Resize(const Gray& im, int nw, int nh) {
  Gray out(nw, nh);
  out.p = Resample(im.p, im.w, im.h, 1, nw, nh, true);
  return out;
}

// Pillow's GaussianBlur: three box blurs each way with a fractional radius
// (BoxBlur.c), the edges extended.
float BoxRadius(float radius, int passes) {
  float sigma2 = radius * radius / passes;
  float L = float(std::sqrt(12.0 * sigma2 + 1.0));
  float l = float(std::floor((L - 1.0) / 2.0));
  float a = (2 * l + 1) * (l * (l + 1) - 3 * sigma2);
  a /= 6 * (sigma2 - (l + 1) * (l + 1));
  return l + a;
}

// One line, stride apart, blurred in place.
void BoxLine(uint8_t* line, int n, size_t stride, float fradius, std::vector<uint8_t>& tmp) {
  int radius = int(fradius);
  uint32_t ww = uint32_t(float(1 << 24) / (fradius * 2 + 1));
  uint32_t fw = ((1u << 24) - uint32_t(radius * 2 + 1) * ww) / 2;
  int last = n - 1, edge_a = std::min(radius + 1, n), edge_b = std::max(n - radius - 1, 0);
  tmp.resize(n);
  for (int x = 0; x < n; ++x) tmp[x] = line[x * stride];
  const uint8_t* in = tmp.data();
  auto save = [&](int x, uint32_t bulk) { line[x * stride] = uint8_t((bulk + (1 << 23)) >> 24); };
  uint32_t acc = in[0] * uint32_t(radius + 1);
  for (int x = 0; x < edge_a - 1; ++x) acc += in[x];
  acc += in[last] * uint32_t(radius - edge_a + 1);
  if (edge_a <= edge_b) {
    for (int x = 0; x < edge_a; ++x) {
      acc += in[x + radius] - in[0];
      save(x, acc * ww + (in[0] + in[x + radius + 1]) * fw);
    }
    for (int x = edge_a; x < edge_b; ++x) {
      acc += in[x + radius] - in[x - radius - 1];
      save(x, acc * ww + (in[x - radius - 1] + in[x + radius + 1]) * fw);
    }
    for (int x = edge_b; x <= last; ++x) {
      acc += in[last] - in[x - radius - 1];
      save(x, acc * ww + (in[x - radius - 1] + in[last]) * fw);
    }
  } else {
    for (int x = 0; x < edge_b; ++x) {
      acc += in[x + radius] - in[0];
      save(x, acc * ww + (in[0] + in[last]) * fw);
    }
    for (int x = edge_b; x < edge_a; ++x) {
      acc += in[last] - in[0];
      save(x, acc * ww + (in[0] + in[last]) * fw);
    }
    for (int x = edge_a; x <= last; ++x) {
      acc += in[last] - in[x - radius - 1];
      save(x, acc * ww + (in[x - radius - 1] + in[last]) * fw);
    }
  }
}

// channels-wide pixels, each channel blurred.
void GaussianBlur(uint8_t* px, int w, int h, int channels, float radius) {
  float r = BoxRadius(radius, 3);
  std::vector<uint8_t> tmp;
  for (int pass = 0; pass < 3; ++pass) {
    for (int y = 0; y < h; ++y) {
      for (int c = 0; c < channels; ++c) BoxLine(px + size_t(y) * w * channels + c, w, channels, r, tmp);
    }
  }
  for (int pass = 0; pass < 3; ++pass) {
    for (int x = 0; x < w; ++x) {
      for (int c = 0; c < channels; ++c) BoxLine(px + size_t(x) * channels + c, h, size_t(w) * channels, r, tmp);
    }
  }
}

// Image.alpha_composite at (dx, dy), what falls outside dropped (AlphaComposite.c).
void Composite(Rgba& dst, const Rgba& src, int dx = 0, int dy = 0) {
  for (int y = std::max(0, dy); y < std::min(dst.h, dy + src.h); ++y) {
    for (int x = std::max(0, dx); x < std::min(dst.w, dx + src.w); ++x) {
      const uint8_t* s = src.at(x - dx, y - dy);
      uint8_t* d = dst.at(x, y);
      if (s[3] == 0) continue;
      uint32_t blend = d[3] * (255u - s[3]);
      uint32_t outa255 = s[3] * 255u + blend;
      uint32_t coef1 = s[3] * 255u * 255u * (1u << 7) / outa255;
      uint32_t coef2 = 255u * (1u << 7) - coef1;
      for (int c = 0; c < 3; ++c) {
        uint32_t t = s[c] * coef1 + d[c] * coef2 + (0x80u << 7);
        d[c] = uint8_t((((t >> 8) + t) >> 8) >> 7);
      }
      uint32_t t = outa255 + 0x80;
      d[3] = uint8_t(((t >> 8) + t) >> 8);
    }
  }
}

// ---- The icon's parts (make_icons.py) ------------------------------------

constexpr int kM = 512;  // drawn at 512 and reduced
constexpr int kRim = 6;  // the chrome rim's width round a letter of the logo, texels

// m spread by 4-neighbour steps (at most steps of them), only into pixels of
// through: a flood fill.
std::vector<uint8_t> Grow(int w, int h, int sx, int sy, const std::vector<uint8_t>& through, int steps) {
  std::vector<uint8_t> m(size_t(w) * h, 0);
  std::vector<int> ring = {sy * w + sx}, next;
  m[ring[0]] = 1;
  for (int s = 0; s < steps && !ring.empty(); ++s) {
    next.clear();
    for (int i : ring) {
      int x = i % w, y = i / w;
      const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
      for (const auto& q : n) {
        if (q[0] < 0 || q[0] >= w || q[1] < 0 || q[1] >= h) continue;
        int j = q[1] * w + q[0];
        if (!m[j] && through[j]) m[j] = 1, next.push_back(j);
      }
    }
    ring.swap(next);
  }
  return m;
}

// m spread by the rim's width with a round brush (wrapping at the edges, as np.roll).
std::vector<uint8_t> RimSpread(int w, int h, const std::vector<uint8_t>& m) {
  std::vector<uint8_t> out(m.size(), 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (!m[size_t(y) * w + x]) continue;
      for (int dy = -kRim; dy <= kRim; ++dy) {
        for (int dx = -kRim; dx <= kRim; ++dx) {
          if (dx * dx + dy * dy > kRim * kRim + 1) continue;
          out[size_t((y + dy + h) % h) * w + (x + dx + w) % w] = 1;
        }
      }
    }
  }
  return out;
}

// The logo's N with its section of the metal plate: the plate's own left end,
// top and bottom edge, cut a rim's width right of the N, the plate's left
// bevel mirrored onto the cut. Under the N's right foot the plate carries the
// G of GAIDEN: that corner (outside the N) is refilled with the plate's own
// texture from beside it.
bool PlateN(const Rgba& logo, Rgba* out) {
  const int w = logo.w, h = logo.h;
  std::vector<uint8_t> solid(size_t(w) * h), face(size_t(w) * h);
  for (int i = 0; i < w * h; ++i) {
    const uint8_t* q = &logo.p[size_t(i) * 4];
    solid[i] = q[3] > 200;
    face[i] = solid[i] && q[0] + q[1] + q[2] < 42;  // (mean under 14)
  }
  // The N's black face: the leftmost black shape at least 15 wide (a 15x15
  // minimum, the edges extended), in the logo's first quarter.
  std::vector<uint8_t> rows(size_t(w) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t v = 1;
      for (int k = -7; k <= 7 && v; ++k) v = face[size_t(y) * w + std::clamp(x + k, 0, w - 1)];
      rows[size_t(y) * w + x] = v;
    }
  }
  int sx = -1, sy = -1;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w / 4 && (sx < 0 || x < sx); ++x) {
      uint8_t v = 1;
      for (int k = -7; k <= 7 && v; ++k) v = rows[size_t(std::clamp(y + k, 0, h - 1)) * w + x];
      if (v) sx = x, sy = y;
    }
  }
  if (sx < 0) return false;
  std::vector<uint8_t> rim = RimSpread(w, h, Grow(w, h, sx, sy, face, 600));
  int x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (rim[size_t(y) * w + x] && solid[size_t(y) * w + x]) {
        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
      }
    }
  }
  if (x1 < 0) return false;
  x0 -= 1, y0 -= 1, x1 += 2, y1 += 2;  // the N's box

  Rgba a = logo;
  int bottom = -1;  // the plate's lower edge under the N
  for (int y = 0; y < h; ++y) {
    if (solid[size_t(y) * w + (x0 + x1) / 2]) bottom = y + 1;
  }
  int cut = x1 + kRim + 1;
  if (bottom < 0 || cut > w || y0 < 0 || x1 - 47 - 22 < 0) return false;
  std::vector<uint8_t> keep;
  int nx = x0 + kRim + 4, ny = (y0 + y1) / 2;  // (inside the N's stem)
  if (face[size_t(ny) * w + nx]) {
    keep = RimSpread(w, h, Grow(w, h, nx, ny, face, 600));
  } else {
    keep.assign(size_t(w) * h, 0);
  }
  int gx = x1 - 47, gy = y1 - 15;  // the corner the G reaches into, mirrored about its left side
  for (int y = gy; y < bottom; ++y) {
    for (int x = gx; x < cut; ++x) {
      if (keep[size_t(y) * w + x]) continue;
      int m = (x - gx) % 44;
      std::memcpy(a.at(x, y), a.at(gx - 1 - (m < 22 ? m : 43 - m), y), 4);
    }
  }
  if (y0 + 40 >= h) return false;  // (another logo than the one measured)
  for (int y = y0; y < bottom; ++y) {
    int sy2 = std::max(y, y0 + 40);  // (above, the left end is the slanted tip)
    int left = -1;
    for (int x = 0; x < x0 + 20 && left < 0; ++x) {
      if (a.at(x, sy2)[3] > 200) left = x;
    }
    if (left < 0) return false;
    for (int k = 0; k <= kRim; ++k) std::memcpy(a.at(cut - 1 - k, y), a.at(left + k, sy2), 4);
  }
  *out = Rgba(cut, bottom);
  for (int y = 0; y < bottom; ++y) std::memcpy(out->at(0, y), a.at(0, y), size_t(cut) * 4);
  return true;
}

// The picture cropped to where its alpha is above 24 (the brush's faint
// spatters don't set the size).
Rgba Trim(const Rgba& im) {
  int x0 = im.w, y0 = im.h, x1 = -1, y1 = -1;
  for (int y = 0; y < im.h; ++y) {
    for (int x = 0; x < im.w; ++x) {
      if (im.at(x, y)[3] > 24) {
        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
      }
    }
  }
  if (x1 < 0) return im;
  Rgba out(x1 - x0 + 1, y1 - y0 + 1);
  for (int y = 0; y < out.h; ++y) std::memcpy(out.at(0, y), im.at(x0, y0 + y), size_t(out.w) * 4);
  return out;
}

// im scaled so its width (by_height: height) is frac of a size x size canvas,
// centred, moved by dx, dy (fractions).
Rgba Fit(const Rgba& im, int size, double frac, double dx, double dy, bool by_height = false) {
  double k = size * frac / (by_height ? im.h : im.w);
  int w = Round(im.w * k), h = Round(im.h * k);
  Rgba r = Resize(im, w, h);
  Rgba c(size, size);
  auto floordiv2 = [](int v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); };
  Composite(c, r, floordiv2(size - w) + Round(dx * size), floordiv2(size - h) + Round(dy * size));
  return c;
}

// The black rounded square, drawn at 4x and reduced for a clean edge. Its
// corners are quarter ellipses traced as Pillow's ImageDraw traces them
// (Draw.c's quarter_next, in half pixels: from (d, 0) to (0, d), each step the
// one of up, left and up-left that stays nearest the curve), giving each
// corner row's first pixel.
Rgba Tile(int size, double radius = 0.19) {
  const int big = size * 4, d = Round(big * radius) * 2, r = d / 2, last = big - 1;
  std::vector<int> first(r + 1, 0);  // corner row (0 the top) -> first pixel
  {
    const int64_t a2 = int64_t(d) * d, a2b2 = a2 * a2;
    auto delta = [&](int64_t x, int64_t y) { return std::llabs(a2 * y * y + a2 * x * x - a2b2); };
    int cx = d, cy = 0;
    std::vector<int> widest(d + 1, -1);
    for (;;) {
      widest[cy] = std::max(widest[cy], cx);
      if (cx == 0 && cy == d) break;
      int nx = cx, ny = cy + 2;
      int64_t nd = delta(nx, ny);
      if (nx > 1) {
        int64_t q = delta(cx - 2, cy + 2);
        if (nd > q) nx = cx - 2, ny = cy + 2, nd = q;
        q = delta(cx - 2, cy);
        if (nd > q) nx = cx - 2, ny = cy;
      }
      cx = nx, cy = ny;
    }
    for (int y = 0; y <= d; y += 2) first[(d - y) / 2] = (d - widest[y]) / 2;
  }
  Gray m(big, big);
  for (int y = 0; y < big; ++y) {
    int row = y <= r ? y : y >= last - r ? last - y : -1;
    int x0 = row < 0 ? 0 : first[row];
    std::memset(&m.p[size_t(y) * big + x0], 255, size_t(big - 2 * x0));
  }
  Gray a = Resize(m, size, size);
  Rgba t(size, size);
  for (int i = 0; i < size * size; ++i) t.p[size_t(i) * 4 + 3] = a.p[i];
  return t;
}

// A soft light of the layer's shape (its alpha blurred), to put under it.
Rgba Glow(const Rgba& layer, float radius, const uint8_t colour[3], double gain) {
  std::vector<uint8_t> a(size_t(layer.w) * layer.h);
  for (size_t i = 0; i < a.size(); ++i) a[i] = layer.p[i * 4 + 3];
  GaussianBlur(a.data(), layer.w, layer.h, 1, radius);
  Rgba g(layer.w, layer.h);
  for (size_t i = 0; i < a.size(); ++i) {
    std::memcpy(&g.p[i * 4], colour, 3);
    g.p[i * 4 + 3] = uint8_t(std::min(255, int(a[i] * gain)));
  }
  return g;
}

// At 32 and below the strokes' frayed edges turn to haze: the alpha made harder.
void Bold(Rgba& layer, int size) {
  if (size > 32) return;
  for (size_t i = 3; i < layer.p.size(); i += 4) {
    layer.p[i] = uint8_t(std::clamp((layer.p[i] - 70) * 3, 0, 255));
  }
}

// At 48 and below the chrome rim is under a pixel wide: brightened so the
// letter keeps its outline.
void SmallLift(Rgba& im, int size) {
  if (size > 48) return;
  float k = size > 32 ? 1.3f : 1.6f;
  for (size_t i = 0; i < im.p.size(); i += 4) {
    for (int c = 0; c < 3; ++c) im.p[i + c] = uint8_t(std::min(255.0f, im.p[i + c] * k));
  }
}

// The icon drawn at 512 for a size: the N on its bevelled plate, 68% of the
// tile (66% above 32), left; the II in front of the plate's right margin and
// bevel, its tails running to the tile's bottom edge; the II's red light
// behind the plate and a soft shadow lifting the II off it.
Rgba Master(const Rgba& plate, const Rgba& two, int size) {
  const bool big = size > 32;
  static const uint8_t kRed[3] = {255, 20, 12}, kBlack[3] = {0, 0, 0};
  Rgba t = Tile(kM);
  Rgba c = t;
  Rgba n = Fit(plate, kM, big ? 0.66 : 0.68, -0.14, 0.0);
  SmallLift(n, size);
  // At 32 and below the II is widened (1.3x, 1.5x at 16), or the gap between
  // its strokes is under a pixel.
  Rgba ii = big ? two : Resize(two, Round(two.w * (size > 16 ? 1.3 : 1.5)), two.h);
  const double ph = 0.97;
  Rgba s = Fit(ii, kM, ph, 0.25 + (big ? 0 : 0.02), (1.03 - ph) / 2 + 0.02, true);
  Bold(s, size);
  Composite(c, Glow(s, 30, kRed, big ? 0.7 : 0.4));
  Composite(c, n);
  Composite(c, Glow(s, 7, kBlack, 1.1), -3, 4);
  Composite(c, s);
  for (size_t i = 3; i < c.p.size(); i += 4) c.p[i] = uint8_t(c.p[i] * t.p[i] / 255);
  return c;
}

// The master reduced: by area at the small sizes (where 512 divides), then
// sharpened a little so the strokes and the chrome rim stay crisp.
Rgba Reduce(const Rgba& im, int size) {
  if (size >= 128) return Resize(im, size, size);
  Rgba r = Resize(im, size, size, kM % size != 0);
  std::vector<uint8_t> blur(r.p);
  for (size_t i = 3; i < blur.size(); i += 4) blur[i] = 255;  // (RGB: the fourth byte 255)
  std::vector<uint8_t> in = blur;
  GaussianBlur(blur.data(), size, size, 4, 0.6f);
  int percent = size <= 32 ? 90 : 50;
  for (size_t i = 0; i < r.p.size(); i += 4) {
    for (int c = 0; c < 3; ++c) {
      int diff = in[i + c] - blur[i + c];
      r.p[i + c] = diff != 0 ? uint8_t(std::clamp(in[i + c] + diff * percent / 100, 0, 255)) : in[i + c];
    }
  }
  return r;
}

}  // namespace

bool Make(const fs::path& game_root, std::vector<Picture>* out, std::string* why) {
  std::vector<uint8_t> data;
  Spr spr;
  if (!TitlePack(game_root, &data, &spr, why)) return false;
  Rgba logo, two, plate;
  if (!spr.Picture("t_0x20004", &logo) || !spr.Picture("t_0x20006", &two)) {
    *why = "the title logo's textures can't be read";
    return false;
  }
  if (!PlateN(logo, &plate)) {
    *why = "the logo's N wasn't found";
    return false;
  }
  two = Trim(two);
  // The masters differ only at 48 (the lift), 32 and 16.
  std::map<int, Rgba> masters;
  auto key = [](int size) { return size > 48 ? kM : size; };
  out->clear();
  for (int size : kSizes) {
    auto it = masters.find(key(size));
    if (it == masters.end()) it = masters.emplace(key(size), Master(plate, two, size)).first;
    Rgba im = size == kM ? it->second : Reduce(it->second, size);
    out->push_back({size, std::move(im.p)});
  }
  return true;
}

}  // namespace ng2::game_icon
