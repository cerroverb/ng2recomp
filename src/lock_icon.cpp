// ng2 - Ninja Gaiden II static recompilation
//
// The secret achievements' padlock (lock_icon.h), step for step as
// tools/lock_art.py's build_lock (its constants and comments there).

#include "lock_icon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ng2::lock_icon {
namespace {

constexpr int kN = int(kSize);
constexpr int kSS = 6;          // supersampling of the drawn shapes
constexpr int kBig = kN * kSS;  // the drawing's grid

constexpr double kBodyCx = 32.0, kBodyCy = 43.0, kBodyHx = 16.5, kBodyHy = 12.0, kBodyR = 3.5;
constexpr double kShackleCx = 32.0, kShackleCy = 25.5, kShackleR = 10.0, kShackleW = 2.9;
constexpr double kKeyCx = 32.0, kKeyCy = 40.5, kKeyR = 3.3, kKeyFoot = 49.0;
constexpr double kLight[3] = {-0.52, -0.66, 0.54};
constexpr int kShadowX = 2, kShadowY = 2;  // (2.0, 2.4) rounded

using Rgb = std::array<double, 3>;

// A float image of c channels, row-major.
struct Image {
  int w = 0, h = 0, c = 1;
  std::vector<double> v;
  Image() = default;
  Image(int w_, int h_, int c_ = 1, double fill = 0.0) : w(w_), h(h_), c(c_), v(size_t(w_) * h_ * c_, fill) {}
  double& at(int x, int y, int k = 0) { return v[(size_t(y) * w + x) * c + k]; }
  double at(int x, int y, int k = 0) const { return v[(size_t(y) * w + x) * c + k]; }
};

// Gaussian blur, edges repeated (numpy's: rows, then columns).
Image Blur(const Image& in, double sigma) {
  int r = std::max(1, int(sigma * 3 + 0.5));
  std::vector<double> k(size_t(2 * r + 1));
  double sum = 0;
  for (int i = -r; i <= r; i++) {
    k[size_t(i + r)] = std::exp(-0.5 * (i / sigma) * (i / sigma));
    sum += k[size_t(i + r)];
  }
  for (double& x : k) {
    x /= sum;
  }
  Image a = in, b(in.w, in.h, in.c);
  for (int y = 0; y < a.h; y++) {
    for (int x = 0; x < a.w; x++) {
      for (int ch = 0; ch < a.c; ch++) {
        double s = 0;
        for (int i = -r; i <= r; i++) {
          s += k[size_t(i + r)] * a.at(x, std::clamp(y + i, 0, a.h - 1), ch);
        }
        b.at(x, y, ch) = s;
      }
    }
  }
  for (int y = 0; y < a.h; y++) {
    for (int x = 0; x < a.w; x++) {
      for (int ch = 0; ch < a.c; ch++) {
        double s = 0;
        for (int i = -r; i <= r; i++) {
          s += k[size_t(i + r)] * b.at(std::clamp(x + i, 0, a.w - 1), y, ch);
        }
        a.at(x, y, ch) = s;
      }
    }
  }
  return a;
}

double Lum(double r, double g, double b) {
  return r * 0.299 + g * 0.587 + b * 0.114;
}

// numpy's percentile (linear between the sorted values).
double Percentile(std::vector<double> values, double q) {
  std::sort(values.begin(), values.end());
  double index = q / 100.0 * double(values.size() - 1);
  size_t lo = size_t(std::floor(index));
  size_t hi = std::min(lo + 1, values.size() - 1);
  return values[lo] + (values[hi] - values[lo]) * (index - double(lo));
}

double Clip01(double x) {
  return std::clamp(x, 0.0, 1.0);
}

double RoundBox(double x, double y) {
  double qx = std::abs(x - kBodyCx) - (kBodyHx - kBodyR), qy = std::abs(y - kBodyCy) - (kBodyHy - kBodyR);
  return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - kBodyR;
}

// Inside, antialiased.
double Cover(double sd) {
  return Clip01(0.5 - sd * kSS / 2.0);
}

// A height field as chrome (_shade).
Image Shade(const Image& height, const Rgb& hiTone, const Rgb& midTone, const Rgb& loTone, const Image& wear,
            double wearScale) {
  Image h = Blur(height, kSS * 0.35);
  double norm = std::sqrt(kLight[0] * kLight[0] + kLight[1] * kLight[1] + kLight[2] * kLight[2]);
  double lx = kLight[0] / norm, ly = kLight[1] / norm, lz = kLight[2] / norm;
  double flat = lz;
  Image rgb(kBig, kBig, 3);
  auto grad = [&](int x, int y, bool alongX) {
    int n = kBig;
    int i = alongX ? x : y;
    auto f = [&](int j) { return alongX ? h.at(j, y) : h.at(x, j); };
    double d = i == 0 ? f(1) - f(0) : i == n - 1 ? f(n - 1) - f(n - 2) : (f(i + 1) - f(i - 1)) / 2.0;
    return d * kSS;
  };
  for (int y = 0; y < kBig; y++) {
    for (int x = 0; x < kBig; x++) {
      double gx = grad(x, y, true), gy = grad(x, y, false);
      double nx = -gx, ny = -gy, nz = 1.0, len = std::sqrt(nx * nx + ny * ny + nz * nz);
      double lit = (nx * lx + ny * ly + nz * lz) / len;
      double up = Clip01((lit - flat) / (1 - flat));
      double down = Clip01((flat - lit) / (flat + 0.55));
      double band = std::exp(-std::pow((lit - (flat - 0.2)) / 0.07, 2)) * 0.22;
      double rough = 1 + wearScale * wear.at(x, y);
      for (int k = 0; k < 3; k++) {
        double steel = midTone[k] + (hiTone[k] - midTone[k]) * 0.70;
        double c = steel + (hiTone[k] * 1.04 - steel) * std::pow(up, 0.6);
        c = c + (loTone[k] - c) * std::pow(down, 0.8);
        rgb.at(x, y, k) = c * (1 - band) * rough;
      }
    }
  }
  return rgb;
}

// Mean over each kSS x kSS block.
Image Down(const Image& in) {
  Image out(kN, kN, in.c);
  for (int y = 0; y < kBig; y++) {
    for (int x = 0; x < kBig; x++) {
      for (int k = 0; k < in.c; k++) {
        out.at(x / kSS, y / kSS, k) += in.at(x, y, k) / double(kSS * kSS);
      }
    }
  }
  return out;
}

// numpy's roll: out[i] = in[i - shift].
Image Roll(const Image& in, int dy, int dx) {
  Image out(in.w, in.h, in.c);
  for (int y = 0; y < in.h; y++) {
    for (int x = 0; x < in.w; x++) {
      int sy = ((y - dy) % in.h + in.h) % in.h, sx = ((x - dx) % in.w + in.w) % in.w;
      for (int k = 0; k < in.c; k++) {
        out.at(x, y, k) = in.at(sx, sy, k);
      }
    }
  }
  return out;
}

}  // namespace

std::vector<uint8_t> Build(const std::vector<std::vector<uint8_t>>& tileBytes) {
  std::vector<Image> a;
  for (const auto& bytes : tileBytes) {
    if (bytes.size() != size_t(kN) * kN * 4) {
      continue;
    }
    Image tile(kN, kN, 3);
    for (size_t p = 0; p < size_t(kN) * kN; p++) {
      for (int k = 0; k < 3; k++) {
        tile.v[p * 3 + size_t(k)] = bytes[p * 4 + size_t(k)];
      }
    }
    a.push_back(std::move(tile));
  }
  int n = int(a.size());
  if (n < 5) {
    return {};
  }

  // The ground the tiles share (tile_ground).
  Image ground(kN, kN, 3), est(kN, kN, 3), weight(kN, kN, 1);
  std::vector<uint8_t> sure(size_t(kN) * kN);
  auto apart = [&](int i, int j, int x, int y) {
    double d = 0;
    for (int k = 0; k < 3; k++) {
      d = std::max(d, std::abs(a[size_t(i)].at(x, y, k) - a[size_t(j)].at(x, y, k)));
    }
    return d;
  };
  for (int y = 0; y < kN; y++) {
    for (int x = 0; x < kN; x++) {
      int best = 0, bestCount = -1;
      for (int i = 0; i < n; i++) {
        int count = 0;
        for (int j = 0; j < n; j++) {
          count += apart(i, j, x, y) < 14;
        }
        if (count > bestCount) {
          best = i, bestCount = count;
        }
      }
      Rgb sum{};
      int members = 0;
      for (int j = 0; j < n; j++) {
        if (apart(best, j, x, y) < 14) {
          members++;
          for (int k = 0; k < 3; k++) {
            sum[size_t(k)] += a[size_t(j)].at(x, y, k);
          }
        }
      }
      for (int k = 0; k < 3; k++) {
        ground.at(x, y, k) = sum[size_t(k)] / members;
      }
      sure[size_t(y) * kN + x] = bestCount >= 5;
      weight.at(x, y) = bestCount >= 5 ? 1.0 : 0.0;
    }
  }
  est = ground;
  for (int pass = 0; pass < 6; pass++) {
    if (*std::min_element(weight.v.begin(), weight.v.end()) > 0.5) {
      break;
    }
    Image weighted(kN, kN, 3);
    for (size_t p = 0; p < size_t(kN) * kN; p++) {
      for (int k = 0; k < 3; k++) {
        weighted.v[p * 3 + size_t(k)] = est.v[p * 3 + size_t(k)] * weight.v[p];
      }
    }
    Image num = Blur(weighted, 1.5), den = Blur(weight, 1.5);
    for (size_t p = 0; p < size_t(kN) * kN; p++) {
      if (weight.v[p] < 0.5 && den.v[p] > 0.05) {
        for (int k = 0; k < 3; k++) {
          est.v[p * 3 + size_t(k)] = num.v[p * 3 + size_t(k)] / den.v[p];
        }
        weight.v[p] = 1.0;
      }
    }
  }
  for (int y = 0; y < kN; y++) {
    for (int x = 0; x < kN; x++) {
      if (sure[size_t(y) * kN + x]) {
        continue;
      }
      int nearest = 0;
      double nearestOff = 1e30;
      for (int i = 0; i < n; i++) {
        double off = 0;
        for (int k = 0; k < 3; k++) {
          off = std::max(off, std::abs(a[size_t(i)].at(x, y, k) - est.at(x, y, k)));
        }
        if (off < nearestOff) {
          nearest = i, nearestOff = off;
        }
      }
      for (int k = 0; k < 3; k++) {
        ground.at(x, y, k) = nearestOff < 26 ? a[size_t(nearest)].at(x, y, k) : est.at(x, y, k);
      }
    }
  }

  // The metal's tones (metal_tones).
  std::array<std::vector<double>, 3> bright, dark, stone;
  for (int y = 0; y < kN; y++) {
    for (int x = 0; x < kN; x++) {
      double g = Lum(ground.at(x, y, 0), ground.at(x, y, 1), ground.at(x, y, 2));
      for (int k = 0; k < 3; k++) {
        stone[size_t(k)].push_back(ground.at(x, y, k));
      }
      for (int i = 0; i < n; i++) {
        const Image& t = a[size_t(i)];
        double diff = Lum(t.at(x, y, 0), t.at(x, y, 1), t.at(x, y, 2)) - g;
        for (int k = 0; k < 3; k++) {
          if (diff > 60) {
            bright[size_t(k)].push_back(t.at(x, y, k));
          } else if (diff < -35) {
            dark[size_t(k)].push_back(t.at(x, y, k));
          }
        }
      }
    }
  }
  Rgb hi{250, 250, 244}, lo{14, 14, 16}, mid{};
  for (size_t k = 0; k < 3; k++) {
    if (bright[k].size() > 50) {
      hi[k] = Percentile(bright[k], 92);
    }
    if (dark[k].size() > 50) {
      lo[k] = Percentile(dark[k], 12);
    }
    mid[k] = stone[k].empty() ? 128.0 : Percentile(stone[k], 62);
  }

  // The wear: the ground's fine detail, enlarged (bilinear; the mock-up's
  // bicubic), with a turned copy over it.
  Image lum(kN, kN, 1);
  for (size_t p = 0; p < size_t(kN) * kN; p++) {
    lum.v[p] = Lum(ground.v[p * 3], ground.v[p * 3 + 1], ground.v[p * 3 + 2]);
  }
  Image lumBlur = Blur(lum, 2.0), detail(kN, kN, 1);
  for (size_t p = 0; p < detail.v.size(); p++) {
    detail.v[p] = std::clamp((lum.v[p] - lumBlur.v[p]) / 70.0, -0.5, 0.5);
  }
  Image wearBase(kBig, kBig, 1);
  for (int y = 0; y < kBig; y++) {
    for (int x = 0; x < kBig; x++) {
      double sx = std::clamp((x + 0.5) / kSS - 0.5, 0.0, double(kN - 1));
      double sy = std::clamp((y + 0.5) / kSS - 0.5, 0.0, double(kN - 1));
      int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, kN - 1), y1 = std::min(y0 + 1, kN - 1);
      double fx = sx - x0, fy = sy - y0;
      wearBase.at(x, y) = (detail.at(x0, y0) * (1 - fx) + detail.at(x1, y0) * fx) * (1 - fy) +
                          (detail.at(x0, y1) * (1 - fx) + detail.at(x1, y1) * fx) * fy;
    }
  }
  Image turned(kBig, kBig, 1);
  for (int y = 0; y < kBig; y++) {
    for (int x = 0; x < kBig; x++) {
      turned.at(x, y) = wearBase.at(kBig - 1 - x, kBig - 1 - y);
    }
  }
  turned = Roll(turned, 11 * kSS, 7 * kSS);
  Image wear(kBig, kBig, 1);
  for (size_t p = 0; p < wear.v.size(); p++) {
    wear.v[p] = wearBase.v[p] + 0.6 * turned.v[p];
  }

  // The shapes and their heights (_shapes).
  Image body(kBig, kBig), shackle(kBig, kBig), key(kBig, kBig), hBody(kBig, kBig), hShackle(kBig, kBig);
  for (int py = 0; py < kBig; py++) {
    for (int px = 0; px < kBig; px++) {
      double x = (px + 0.5) / kSS, y = (py + 0.5) / kSS;
      double b = RoundBox(x, y);
      double dx = x - kShackleCx, dy = y - kShackleCy;
      double ring = dy < 0 ? std::abs(std::hypot(dx, dy) - kShackleR) : std::abs(std::abs(dx) - kShackleR);
      double s = std::max(ring - kShackleW, y - (kBodyCy - kBodyHy + 3.0));
      double t = Clip01((y - kKeyCy) / (kKeyFoot - kKeyCy));
      double slot = std::max(std::abs(x - kKeyCx) - (1.3 + 1.5 * t), std::max(kKeyCy - y, y - kKeyFoot));
      double k = std::min(std::hypot(x - kKeyCx, y - kKeyCy) - kKeyR, slot);
      double e = Clip01(-b / 3.2);
      double hb = 3.0 + 2.4 * std::pow(std::sin(e * std::numbers::pi / 2), 0.8);
      double ux = (x - kBodyCx) / kBodyHx, uy = (y - kBodyCy) / kBodyHy;
      hb += 1.7 * (1 - ux * ux) * (1 - uy * uy);
      double grooveY = kBodyCy - kBodyHy + 5.4;
      if (std::abs(x - kBodyCx) < kBodyHx - 3.2) {
        hb -= 0.55 * std::exp(-std::pow((y - grooveY) / 0.55, 2));
      }
      if (k > 0) {
        hb += 0.5 * std::exp(-std::pow(k / 1.1, 2));
      }
      hb -= 2.6 * Clip01(-k / 1.0);
      body.at(px, py) = b, shackle.at(px, py) = s, key.at(px, py) = k, hBody.at(px, py) = hb;
      hShackle.at(px, py) = 2.6 * std::sqrt(Clip01(1 - (ring / kShackleW) * (ring / kShackleW)));
    }
  }

  Image rgbBody = Shade(hBody, hi, mid, lo, wear, 0.30);
  Image rgbShackle = Shade(hShackle, hi, mid, lo, wear, 0.2);
  Rgb outline{lo[0] * 0.8, lo[1] * 0.8, lo[2] * 0.8};
  constexpr double kEdge = 1.15;
  Image col(kBig, kBig, 3), alpha(kBig, kBig);
  for (int py = 0; py < kBig; py++) {
    for (int px = 0; px < kBig; px++) {
      double x = (px + 0.5) / kSS, y = (py + 0.5) / kSS;
      // the glints, the keyhole's pit, the shackle darker into the body
      double u = (x - kBodyCx) + 0.55 * (y - kBodyCy);
      double glint = 0.16 * std::exp(-std::pow((u + 7.5) / 2.6, 2)) + 0.09 * std::exp(-std::pow((u - 3.0) / 1.2, 2)) -
                     0.10 * std::exp(-std::pow((u - 11.0) / 4.0, 2));
      double pit = Cover(key.at(px, py) + 0.45) * 0.86;
      double into = Clip01((double(py) / kSS - (kBodyCy - kBodyHy - 3.5)) / 3.5);
      double alS = Cover(shackle.at(px, py) - kEdge), inS = Cover(shackle.at(px, py) + 0.15);
      double alB = Cover(body.at(px, py) - kEdge), inB = Cover(body.at(px, py) + 0.15);
      for (int k = 0; k < 3; k++) {
        double cb = rgbBody.at(px, py, k) * (1 + glint);
        cb = cb + (lo[size_t(k)] * 0.9 - cb) * pit;
        double cs = rgbShackle.at(px, py, k) * (1 - 0.55 * into);
        cb = outline[size_t(k)] + (cb - outline[size_t(k)]) * inB;
        cs = outline[size_t(k)] + (cs - outline[size_t(k)]) * inS;
        col.at(px, py, k) = cs * (1 - alB) + cb * alB;
      }
      alpha.at(px, py) = alS + alB - alS * alB;
    }
  }

  // Down to the icon's grid, premultiplied.
  Image premul(kBig, kBig, 3);
  for (size_t p = 0; p < size_t(kBig) * kBig; p++) {
    for (int k = 0; k < 3; k++) {
      premul.v[p * 3 + size_t(k)] = col.v[p * 3 + size_t(k)] * alpha.v[p];
    }
  }
  Image al64 = Down(alpha), col64 = Down(premul);
  for (size_t p = 0; p < size_t(kN) * kN; p++) {
    for (int k = 0; k < 3; k++) {
      col64.v[p * 3 + size_t(k)] /= std::max(al64.v[p], 1e-6);
    }
  }

  // The drop shadow and light rim on the stone, the lock over it, its edge
  // softened.
  Image shadow = Blur(Roll(al64, kShadowY, kShadowX), 1.5);
  Image rimBlur = Blur(Roll(al64, -1, -1), 0.8);
  Image out(kN, kN, 3), mask(kN, kN);
  for (size_t p = 0; p < size_t(kN) * kN; p++) {
    double rim = Clip01(rimBlur.v[p] - al64.v[p]);
    for (int k = 0; k < 3; k++) {
      double o = ground.v[p * 3 + size_t(k)] * (1 - 0.72 * shadow.v[p]);
      o = o + (hi[size_t(k)] - o) * (0.10 * rim);
      out.v[p * 3 + size_t(k)] = o * (1 - al64.v[p]) + col64.v[p * 3 + size_t(k)] * al64.v[p];
    }
    mask.v[p] = std::max(al64.v[p], shadow.v[p]);
  }
  Image soft = Blur(out, 0.42);
  mask = Blur(mask, 1.0);
  std::vector<uint8_t> rgba(size_t(kN) * kN * 4, 255);
  for (size_t p = 0; p < size_t(kN) * kN; p++) {
    for (int k = 0; k < 3; k++) {
      double o = out.v[p * 3 + size_t(k)] * (1 - mask.v[p]) + soft.v[p * 3 + size_t(k)] * mask.v[p];
      rgba[p * 4 + size_t(k)] = uint8_t(std::nearbyint(std::clamp(o, 0.0, 255.0)));
    }
  }
  return rgba;
}

}  // namespace ng2::lock_icon
