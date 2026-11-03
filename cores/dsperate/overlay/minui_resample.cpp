// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. See minui_resample.h, which also
// credits the shaders each resampler and filter comes from.
#include "frontend/sdl/minui_resample.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if DSPERATE_NEON
#include <arm_neon.h>
#endif

extern "C" double ds_pow_compat(double x, double y);

namespace ds::sdl::shader {
namespace {

using Gpu = ds::gpu::Gpu;

double clamp01(double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }
double frac(double v) { return v - std::floor(v); }
u8 to_byte(double v) { return static_cast<u8>(std::lround(clamp01(v) * 255.0)); }
u32 pack(const double c[3]) { return 0xFF000000u | static_cast<u32>(to_byte(c[0])) << 16 | static_cast<u32>(to_byte(c[1])) << 8 | to_byte(c[2]); }
void unpack(u32 p, double c[3]) { c[0] = ((p >> 16) & 255) / 255.0; c[1] = ((p >> 8) & 255) / 255.0; c[2] = (p & 255) / 255.0; }
s32 clampi(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- the separable resamplers ------------------------------------------------

// A tap: a DS pixel along one axis and its weight.
struct Tap { s32 at; double w; };
using Taps = std::vector<Tap>;

// GL_LINEAR at texel coordinate p (centres at k + 0.5).
Taps linear(double p) {
  const double q = p - 0.5;
  const s32 i = static_cast<s32>(std::floor(q));
  const double f = q - i;
  return {{i, 1.0 - f}, {i + 1, f}};
}

// sharp-bilinear (Themaister): the sample point for the pixel centred at t,
// with a prescale of `scale` whole panel pixels per DS pixel.
double sharp_point(double t, double scale) {
  const double fl = std::floor(t), cd = t - fl - 0.5;
  const double range = 0.5 - 0.5 / scale;
  return fl + (cd - std::min(range, std::max(-range, cd))) * scale + 0.5;
}

// sharp-shimmerless (zadpos): output pixel i, offset by `off` panel pixels
// (the subpixel version's), box-filtered between the two DS pixels it straddles.
double shimmerless_point(u32 i, double off, double inv) {
  const double tl = i + off, br = i + 1.0 + off;
  const double ttl = std::floor(inv * tl), tbr = std::floor(inv * br);
  double p = tbr + 0.5;
  if (ttl < tbr) p -= tbr / inv - tl;
  return p;
}

constexpr double kGridRatio = 1.0 - 0.3, kGridAlpha = 1.0 - 0.3;   // GRID_RATIO_*, GRID_OPACITY_* 0.3

// sharp-shimmerless-grid: the sample point, and its grid's shading along this axis.
double shimmer_grid_point(u32 i, double inv, double* shade) {
  const double ptl = inv * i, pbr = inv * (i + 1.0);
  const double ttl = std::floor(ptl), tbr = std::floor(pbr);
  const double ftl = frac(ptl), fbr = frac(pbr);
  const double area_tl = ftl >= kGridRatio ? kGridAlpha * (1.0 - kGridRatio) : kGridRatio - ftl + kGridAlpha * (1.0 - kGridRatio);
  const double area_br = fbr >= kGridRatio ? kGridRatio + kGridAlpha * (fbr - kGridRatio) : fbr;
  double ratio = area_br / (area_tl + area_br);
  if (ttl >= tbr) ratio = 1.0;
  if (shade) {
    double s = (fbr + std::min(1.0, std::max(0.0, kGridRatio - ftl))) / inv;
    if (fbr >= kGridRatio) s = kGridRatio / inv;
    if (ttl >= tbr) s = std::min(1.0, std::max(0.0, (kGridRatio - ftl) / inv));
    *shade = kGridAlpha + (1.0 - kGridAlpha) * s;
  }
  return tbr - 0.5 + ratio;
}

// bicubic's Mitchell-Netravali weight, B = C = 0.3333 as its #defines have them.
double mitchell(double x) {
  constexpr double B = 0.3333, C = 0.3333;
  const double ax = std::fabs(x);
  if (ax < 1.0) return (x * x * ((12.0 - 9.0 * B - 6.0 * C) * ax + (-18.0 + 12.0 * B + 6.0 * C)) + (6.0 - 2.0 * B)) / 6.0;
  if (ax < 2.0) return (x * x * ((-B - 6.0 * C) * ax + (6.0 * B + 30.0 * C)) + (-12.0 * B - 48.0 * C) * ax + (8.0 * B + 24.0 * C)) / 6.0;
  return 0.0;
}

// The taps for output pixel i of `len`, `n` DS pixels across, channel `ch`
// (only the subpixel version tells channels apart, and only down).
Taps axis_taps(Scaling s, u32 i, u32 len, u32 n, bool down, int ch, double prescale) {
  const double t = (i + 0.5) * n / len, inv = static_cast<double>(n) / len;
  switch (s) {
  case Scaling::SharpBilinear: return linear(sharp_point(t, prescale));
  case Scaling::Pixellate: {
    const double r = inv / 2.0 * 0.999, left = t - r, right = t + r;
    const s32 a = static_cast<s32>(std::floor(left)), b = static_cast<s32>(std::floor(right));
    if (a == b) return {{a, 1.0}, {a, 0.0}};
    const double border = std::min(right, std::max(left, std::floor(t + 0.5)));
    return {{a, (border - left) / (2.0 * r)}, {b, (right - border) / (2.0 * r)}};
  }
  case Scaling::SharpShimmerless: return linear(shimmerless_point(i, 0.0, inv));
  case Scaling::ShimmerlessGrid: return linear(shimmer_grid_point(i, inv, nullptr));
  case Scaling::ShimmerlessVrgb: return linear(shimmerless_point(i, down ? (ch == 0 ? -0.33 : ch == 2 ? 0.33 : 0.0) : 0.0, inv));
  case Scaling::Edge1Pixel: {
    const double f = t - std::floor(t) + 1e-3, half = 0.5 * inv;
    const bool inside = f >= half && f < 1.0 - half;
    return linear(inside ? std::floor(t) + 0.5 : std::floor(t + 0.5));
  }
  case Scaling::Smooth3x: {
    // NextUI's pixelperfect-smooth: stock at three times, then GL_LINEAR to the view.
    const s32 m = static_cast<s32>(3 * n);
    const double q = (i + 0.5) * m / len - 0.5;
    const s32 k0 = static_cast<s32>(std::floor(q));
    const double f = q - k0;
    const s32 a = clampi(k0, 0, m - 1), b = clampi(k0 + 1, 0, m - 1);
    return {{a / 3, 1.0 - f}, {b / 3, f}};
  }
  case Scaling::Bicubic: {
    const double pos = t + 0.5, f = frac(pos);
    const s32 base = static_cast<s32>(std::floor(pos)) - 2;
    const double x = 1.0 - f;
    double w[4] = {mitchell(x - 2.0), mitchell(x - 1.0), mitchell(x), mitchell(x + 1.0)};
    const double sum = w[0] + w[1] + w[2] + w[3];
    return {{base, w[0] / sum}, {base + 1, w[1] / sum}, {base + 2, w[2] / sum}, {base + 3, w[3] / sum}};
  }
  default: return {{static_cast<s32>(t), 1.0}};
  }
}

// Quantised to 1/4096, summing to exactly 4096, positions clamped to the edge.
void quantise(const Taps& taps, u32 k, s32 n, s16* at, s32* w) {
  s32 sum = 0, big = 0;
  for (u32 j = 0; j < k; ++j) {
    const Tap tp = j < taps.size() ? taps[j] : Tap{taps.empty() ? 0 : taps[0].at, 0.0};
    at[j] = static_cast<s16>(clampi(tp.at, 0, n - 1));
    w[j] = static_cast<s32>(std::lround(tp.w * 4096.0));
    sum += w[j];
    if (std::abs(w[j]) > std::abs(w[big])) big = static_cast<s32>(j);
  }
  w[big] += 4096 - sum;
}

// Linear light for pixellate: its 2.2 in, in 1/65535, and 2.2 out.
struct Gamma22 {
  u32 in[256];
  u8 out[65536];
  Gamma22() {
    for (u32 v = 0; v < 256; ++v) in[v] = static_cast<u32>(std::lround(ds_pow_compat(v / 255.0, 2.2) * 65535.0));
    for (u32 v = 0; v < 65536; ++v) out[v] = to_byte(ds_pow_compat(v / 65535.0, 1.0 / 2.2));
  }
};
const Gamma22& gamma22() { static const Gamma22 g; return g; }

// Every separable one: per column K taps across, then per row K taps down
// (per channel: the subpixel version samples each at its own height). Each
// DS line is widened once, as it arrives, into 1/256 (or linear light's
// 1/65535) per channel, and each row takes its lines from those.
class Separable final : public Resampler {
public:
  explicit Separable(Scaling s) : Resampler(s), k_(s == Scaling::Bicubic ? 4 : 2), linear_(s == Scaling::Pixellate) {}
private:
  void setup() override {
    // sharp-bilinear's automatic prescale, from the height alone (outsize.y / InputSize.y).
    const double prescale = std::max(1.0, std::floor(static_cast<double>(h_) / SCREEN_H + 0.01));
    cat_.assign(static_cast<size_t>(n_) * k_, 0); cw_.assign(static_cast<size_t>(n_) * k_, 0);
    for (u32 x = 0; x < n_; ++x)
      quantise(axis_taps(kind_, x + x0_, w_, SCREEN_W, false, 0, prescale), k_, SCREEN_W, &cat_[x * k_], &cw_[x * k_]);
    rat_.assign(static_cast<size_t>(h_) * 3 * k_, 0); rw_.assign(static_cast<size_t>(h_) * 3 * k_, 0);
    last_.assign(h_, 0);
    u32 span = 1;
    for (u32 y = 0; y < h_; ++y) {
      s32 lo = SCREEN_H, hi = 0;
      for (int ch = 0; ch < 3; ++ch) {
        const size_t at = (static_cast<size_t>(y) * 3 + static_cast<size_t>(ch)) * k_;
        quantise(axis_taps(kind_, y, h_, SCREEN_H, true, ch, prescale), k_, SCREEN_H, &rat_[at], &rw_[at]);
        for (u32 j = 0; j < k_; ++j) { lo = std::min<s32>(lo, rat_[at + j]); hi = std::max<s32>(hi, rat_[at + j]); }
      }
      // Never decreasing: a row waits for every line a row above it waited for.
      last_[y] = std::max<u32>(static_cast<u32>(hi), y ? last_[y - 1] : 0);
      span = std::max<u32>(span, last_[y] - static_cast<u32>(lo) + 1);
    }
    window_ = span + 1;
    (k_ == 2 ? planes_ : planes_).assign(k_ == 2 ? 0 : static_cast<size_t>(window_) * 3 * n_, 0);
    planes16_.assign(k_ == 2 ? static_cast<size_t>(window_) * 3 * n_ : 0, 0);
    mix_.assign(k_ == 2 ? 0 : static_cast<size_t>(3) * n_, 0);
    mix16_.assign(k_ == 2 ? static_cast<size_t>(3) * n_ : 0, 0);
    prev_.assign(n_, 0);
    prev_y_ = ~0u;
  }
  u32 window() const override { return window_; }
  u32 last_line(u32 y) const override { return last_[y]; }
  void on_line(u32 line) override {
    // The line in this resampler's units, a channel at a time, then across.
    const u32* src = line_at(static_cast<s32>(line));
    const Gamma22& g = gamma22();
    alignas(16) s32 in[3][SCREEN_W];
    for (u32 x = 0; x < SCREEN_W; ++x) {
      const u32 c = src[x];
      in[0][x] = static_cast<s32>(linear_ ? g.in[(c >> 16) & 255] : ((c >> 16) & 255) << 8);
      in[1][x] = static_cast<s32>(linear_ ? g.in[(c >> 8) & 255] : ((c >> 8) & 255) << 8);
      in[2][x] = static_cast<s32>(linear_ ? g.in[c & 255] : (c & 255) << 8);
    }
    const size_t slot = static_cast<size_t>(line % window_) * 3 * n_;
    for (int ch = 0; ch < 3; ++ch) {
      const s32* v = in[ch];
      if (k_ == 2) {
        // Two taps of non-negative weight: 16 bits a channel is enough.
        u16* out = &planes16_[slot + static_cast<size_t>(ch) * n_];
        for (u32 x = 0; x < n_; ++x) {
          const s16* at = &cat_[x * 2];
          const s32* w = &cw_[x * 2];
          out[x] = static_cast<u16>((w[0] * v[at[0]] + w[1] * v[at[1]] + 2048) >> 12);
        }
      } else {
        s32* out = &planes_[slot + static_cast<size_t>(ch) * n_];
        for (u32 x = 0; x < n_; ++x) {
          const s16* at = &cat_[x * k_];
          const s32* w = &cw_[x * k_];
          s32 acc = 2048;
          for (u32 j = 0; j < k_; ++j) acc += w[j] * v[at[j]];
          out[x] = acc >> 12;
        }
      }
    }
  }
  // Two rows with the same taps, one after the other, are the same row.
  bool same_taps(u32 a, u32 b) const {
    const size_t n = static_cast<size_t>(3) * k_;
    return std::equal(&rat_[a * n], &rat_[a * n] + n, &rat_[b * n]) && std::equal(&rw_[a * n], &rw_[a * n] + n, &rw_[b * n]);
  }
  // Two taps down, from 16-bit planes: blended, then packed into pixels.
  void row2(u32 y, u32* out) {
    const Gamma22& g = gamma22();
    const u16* v[3] = {&mix16_[0], &mix16_[n_], &mix16_[2 * static_cast<size_t>(n_)]};
    for (int ch = 0; ch < 3; ++ch) {
      const size_t at = (static_cast<size_t>(y) * 3 + static_cast<size_t>(ch)) * 2;
      const u16* a = &planes16_[(static_cast<size_t>(static_cast<u32>(rat_[at]) % window_) * 3 + static_cast<size_t>(ch)) * n_];
      const u16* b = &planes16_[(static_cast<size_t>(static_cast<u32>(rat_[at + 1]) % window_) * 3 + static_cast<size_t>(ch)) * n_];
      const u32 w0 = static_cast<u32>(rw_[at]), w1 = static_cast<u32>(rw_[at + 1]);
      if (w1 == 0) { v[ch] = a; continue; }   // one line alone: its plane as it is
      u16* o = &mix16_[static_cast<size_t>(ch) * n_];
      u32 x = 0;
#if DSPERATE_NEON
      for (; x + 8 <= n_; x += 8) {
        const uint16x8_t pa = vld1q_u16(a + x), pb = vld1q_u16(b + x);
        uint32x4_t lo = vmlal_n_u16(vmull_n_u16(vget_low_u16(pa), static_cast<u16>(w0)), vget_low_u16(pb), static_cast<u16>(w1));
        uint32x4_t hi = vmlal_n_u16(vmull_n_u16(vget_high_u16(pa), static_cast<u16>(w0)), vget_high_u16(pb), static_cast<u16>(w1));
        vst1q_u16(o + x, vcombine_u16(vrshrn_n_u32(lo, 12), vrshrn_n_u32(hi, 12)));
      }
#endif
      for (; x < n_; ++x) o[x] = static_cast<u16>((w0 * a[x] + w1 * b[x] + 2048) >> 12);
    }
    u32 x = 0;
    if (linear_) {
      for (; x < n_; ++x) out[x] = 0xFF000000u | static_cast<u32>(g.out[v[0][x]]) << 16 | static_cast<u32>(g.out[v[1][x]]) << 8 | g.out[v[2][x]];
      return;
    }
#if DSPERATE_NEON
    for (; x + 8 <= n_; x += 8) {
      uint8x8x4_t px;
      px.val[0] = vqrshrn_n_u16(vld1q_u16(v[2] + x), 8);
      px.val[1] = vqrshrn_n_u16(vld1q_u16(v[1] + x), 8);
      px.val[2] = vqrshrn_n_u16(vld1q_u16(v[0] + x), 8);
      px.val[3] = vdup_n_u8(255);
      vst4_u8(reinterpret_cast<u8*>(out + x), px);
    }
#endif
    for (; x < n_; ++x)
      out[x] = 0xFF000000u | std::min<u32>(255, (v[0][x] + 128u) >> 8) << 16 | std::min<u32>(255, (v[1][x] + 128u) >> 8) << 8 |
               std::min<u32>(255, (v[2][x] + 128u) >> 8);
  }
  void row(u32 y, u32* out) override {
    if (y > 0 && prev_y_ == y - 1 && same_taps(y, y - 1)) {
      std::memcpy(out, prev_.data(), n_ * sizeof(u32));
      prev_y_ = y;
      return;
    }
    if (k_ == 2) {
      row2(y, out);
    } else {
      s32* const v[3] = {&mix_[0], &mix_[n_], &mix_[2 * static_cast<size_t>(n_)]};
      for (int ch = 0; ch < 3; ++ch) {
        const size_t at = (static_cast<size_t>(y) * 3 + static_cast<size_t>(ch)) * k_;
        const s32* r[4] = {};
        for (u32 j = 0; j < k_; ++j) r[j] = &planes_[(static_cast<size_t>(static_cast<u32>(rat_[at + j]) % window_) * 3 + static_cast<size_t>(ch)) * n_];
        const s32 w0 = rw_[at], w1 = rw_[at + 1], w2 = rw_[at + 2], w3 = rw_[at + 3];
        s32* __restrict o = v[ch];
        for (u32 x = 0; x < n_; ++x) o[x] = (w0 * r[0][x] + w1 * r[1][x] + w2 * r[2][x] + w3 * r[3][x] + 2048) >> 12;
      }
      for (u32 x = 0; x < n_; ++x)
        out[x] = 0xFF000000u | static_cast<u32>(clampi((v[0][x] + 128) >> 8, 0, 255)) << 16 |
                 static_cast<u32>(clampi((v[1][x] + 128) >> 8, 0, 255)) << 8 | static_cast<u32>(clampi((v[2][x] + 128) >> 8, 0, 255));
    }
    std::memcpy(prev_.data(), out, n_ * sizeof(u32));
    prev_y_ = y;
  }
  u32 k_;
  bool linear_;
  u32 window_ = 2;
  std::vector<s16> cat_, rat_;
  std::vector<s32> cw_, rw_;
  std::vector<u32> last_;
  std::vector<s32> planes_, mix_;     // four taps: a sign and more than 16 bits
  std::vector<u16> planes16_, mix16_; // two
  std::vector<u32> prev_;
  u32 prev_y_ = ~0u;
};

// ---- scale3x (Andrea Mazzoleni) ----------------------------------------------

class Scale3x final : public Resampler {
public:
  Scale3x() : Resampler(Scaling::Scale3x) {}
private:
  void setup() override {
    sx_.resize(n_); fx_.resize(n_); ly_.resize(h_); fy_.resize(h_);
    for (u32 x = 0; x < n_; ++x) {
      const double t = centre(x + x0_, w_, SCREEN_W);
      sx_[x] = clampi(static_cast<s32>(t), 0, SCREEN_W - 1);
      fx_[x] = static_cast<u8>(std::min(2.0, std::floor(3.0 * frac(t))));
    }
    for (u32 y = 0; y < h_; ++y) {
      const double t = centre(y, h_, SCREEN_H);
      ly_[y] = clampi(static_cast<s32>(t), 0, SCREEN_H - 1);
      fy_[y] = static_cast<u8>(std::min(2.0, std::floor(3.0 * frac(t))));
    }
  }
  u32 window() const override { return 4; }
  u32 last_line(u32 y) const override { return static_cast<u32>(std::min<s32>(SCREEN_H - 1, ly_[y] + 1)); }
  void row(u32 y, u32* out) override {
    const u32* up = line_at(ly_[y] - 1);
    const u32* mid = line_at(ly_[y]);
    const u32* dn = line_at(ly_[y] + 1);
    const u32 fy = fy_[y];
    for (u32 x = 0; x < n_; ++x) {
      const s32 s = sx_[x], l = std::max(0, s - 1), r = std::min<s32>(SCREEN_W - 1, s + 1);
      // A B C / D E F / G H I, compared as the shader does: exactly.
      const u32 A = up[l] | 0xFF000000u, B = up[s] | 0xFF000000u, C = up[r] | 0xFF000000u;
      const u32 D = mid[l] | 0xFF000000u, E = mid[s] | 0xFF000000u, F = mid[r] | 0xFF000000u;
      const u32 G = dn[l] | 0xFF000000u, H = dn[s] | 0xFF000000u, I = dn[r] | 0xFF000000u;
      u32 c = E;
      if (B != H && D != F) {
        const bool eqBD = B == D, eqBF = B == F, eqHD = H == D, eqHF = H == F;
        const bool neqEA = E != A, neqEC = E != C, neqEG = E != G, neqEI = E != I;
        switch (fy * 3 + fx_[x]) {
        case 0: c = eqBD ? B : E; break;
        case 1: c = (eqBD && neqEC) || (eqBF && neqEA) ? B : E; break;
        case 2: c = eqBF ? B : E; break;
        case 3: c = (eqBD && neqEG) || (eqHD && neqEA) ? D : E; break;
        case 4: c = E; break;
        case 5: c = (eqBF && neqEI) || (eqHF && neqEC) ? F : E; break;
        case 6: c = eqHD ? H : E; break;
        case 7: c = (eqHD && neqEI) || (eqHF && neqEG) ? H : E; break;
        default: c = eqHF ? H : E; break;
        }
      }
      out[x] = c;
    }
  }
  std::vector<s32> sx_, ly_;
  std::vector<u8> fx_, fy_;
};

// ---- aa-shader-4.0 (guest(r), ShadX) -------------------------------------------

// INTERNAL_RES 1 and nearest sampling, as libretro's preset has it: a grid of
// four points a DS pixel, each the edge-weighted blend of the four DS pixels
// a quarter pixel away diagonally, and each panel pixel the edge-weighted
// corners of its grid cell, blended bilinearly.
class Aa4 final : public Resampler {
public:
  Aa4() : Resampler(Scaling::Aa4) {}
private:
  struct Rgb { float r, g, b; };
  void setup() override {
    cx_.resize(n_); fpx_.resize(n_); cy_.resize(h_); fpy_.resize(h_);
    gx_lo_ = 1 << 30; gx_hi_ = -(1 << 30);
    for (u32 x = 0; x < n_; ++x) {
      const double p = 4.0 * centre(x + x0_, w_, SCREEN_W);
      cx_[x] = static_cast<s32>(std::floor(p)); fpx_[x] = static_cast<float>(p - cx_[x]);
      gx_lo_ = std::min(gx_lo_, cx_[x] - 1); gx_hi_ = std::max(gx_hi_, cx_[x] + 2);
    }
    for (u32 y = 0; y < h_; ++y) {
      const double p = 4.0 * centre(y, h_, SCREEN_H);
      cy_[y] = static_cast<s32>(std::floor(p)); fpy_[y] = static_cast<float>(p - cy_[y]);
    }
    const size_t cols = static_cast<size_t>(gx_hi_ - gx_lo_ + 1);
    for (auto& r : crow_) r.assign(cols, Rgb{0, 0, 0});
    for (auto& r : drow_) r.assign(cols, Rgb{0, 0, 0});
    last_y_ = ~0u;
  }
  u32 window() const override { return 4; }
  // Its corner rows c and c + 1 need grid rows c - 1 to c + 1, the last a quarter pixel below: DS line (c + 2) / 4.
  u32 last_line(u32 y) const override { return static_cast<u32>(clampi((cy_[y] + 2) >> 2, 0, SCREEN_H - 1)); }
  static Rgb px(u32 p) { return Rgb{((p >> 16) & 255) / 255.0f, ((p >> 8) & 255) / 255.0f, (p & 255) / 255.0f}; }
  static float diff(const Rgb& a, const Rgb& b) { return std::fabs(a.r - b.r) + std::fabs(a.g - b.g) + std::fabs(a.b - b.b); }
  static Rgb blend(const Rgb& a, const Rgb& b, const Rgb& c, const Rgb& d, float m1, float m2, float k) {
    // k (m2 (a + b) + m1 (c + d)) / (m1 + m2)
    const float s = k / (m1 + m2);
    return Rgb{s * (m2 * (a.r + b.r) + m1 * (c.r + d.r)), s * (m2 * (a.g + b.g) + m1 * (c.g + d.g)), s * (m2 * (a.b + b.b) + m1 * (c.b + d.b))};
  }
  // The grid points of row gy: texture2d() at each.
  void c_row(s32 gy, std::vector<Rgb>& out) {
    const u32* top = line_at((gy - 1) >> 2);
    const u32* bot = line_at((gy + 1) >> 2);
    for (s32 gx = gx_lo_; gx <= gx_hi_; ++gx) {
      const s32 l = clampi((gx - 1) >> 2, 0, SCREEN_W - 1), r = clampi((gx + 1) >> 2, 0, SCREEN_W - 1);
      const Rgb s00 = px(top[l]), s20 = px(top[r]), s22 = px(bot[r]), s02 = px(bot[l]);
      const float m1 = diff(s00, s22) + 0.001f, m2 = diff(s02, s20) + 0.001f;
      out[static_cast<size_t>(gx - gx_lo_)] = blend(s00, s22, s02, s20, m1, m2, 0.5f);
    }
  }
  // A cell's corner: the four grid points around it, edge-weighted.
  void d_row(const std::vector<Rgb>& above, const std::vector<Rgb>& here, std::vector<Rgb>& out) {
    for (size_t i = 1; i < here.size(); ++i) {
      // C0 C1 / C3 C4 around the corner at the top-left of cell i.
      const Rgb& c0 = above[i - 1]; const Rgb& c1 = above[i]; const Rgb& c3 = here[i - 1]; const Rgb& c4 = here[i];
      out[i] = blend(c0, c4, c1, c3, diff(c0, c4) + 0.001f, diff(c1, c3) + 0.001f, 1.0f);
    }
  }
  // Grid and corner rows, kept by row for the panel rows after: a frame's
  // lines do not change once in, so they hold until the next frame.
  const std::vector<Rgb>& grid(s32 gy) {
    // By row mod 3, so the rows a corner row needs never share a slot.
    const int slot = ((gy % 3) + 3) % 3;
    if (cvalid_[slot] && ckey_[slot] == gy) return crow_[slot];
    c_row(gy, crow_[slot]);
    ckey_[slot] = gy; cvalid_[slot] = true;
    return crow_[slot];
  }
  const std::vector<Rgb>& corners(s32 gy) {
    const int slot = gy & 1;   // corner rows gy and gy + 1, both held by a panel row
    if (dvalid_[slot] && dkey_[slot] == gy) return drow_[slot];
    const std::vector<Rgb>& above = grid(gy - 1);
    const std::vector<Rgb>& here = grid(gy);
    d_row(above, here, drow_[slot]);
    dkey_[slot] = gy; dvalid_[slot] = true;
    return drow_[slot];
  }
  void row(u32 y, u32* out) override {
    if (y <= last_y_) cvalid_[0] = cvalid_[1] = cvalid_[2] = dvalid_[0] = dvalid_[1] = false;   // a new frame
    last_y_ = y;
    const std::vector<Rgb>& up = corners(cy_[y]);
    const std::vector<Rgb>& dn = corners(cy_[y] + 1);
    const float fy = fpy_[y];
    for (u32 x = 0; x < n_; ++x) {
      const size_t i = static_cast<size_t>(cx_[x] - gx_lo_);
      const Rgb& ul = up[i]; const Rgb& ur = up[i + 1]; const Rgb& dl = dn[i]; const Rgb& dr = dn[i + 1];
      const float fx = fpx_[x];
      auto ch = [&](float a, float b, float c, float d) { return 0.5f * ((d * fx + c * (1.0f - fx)) * fy + (b * fx + a * (1.0f - fx)) * (1.0f - fy)); };
      const double c[3] = {ch(ul.r, ur.r, dl.r, dr.r), ch(ul.g, ur.g, dl.g, dr.g), ch(ul.b, ur.b, dl.b, dr.b)};
      out[x] = pack(c);
    }
  }
  std::vector<s32> cx_, cy_;
  std::vector<float> fpx_, fpy_;
  s32 gx_lo_ = 0, gx_hi_ = 0;
  std::vector<Rgb> crow_[3], drow_[2];
  s32 ckey_[3] = {0, 0, 0}, dkey_[2] = {0, 0};
  bool cvalid_[3] = {false, false, false}, dvalid_[2] = {false, false};
  u32 last_y_ = ~0u;
};

// ---- waterpaint ----------------------------------------------------------------

// Nearest samples half a DS pixel around each panel pixel's centre, blended
// by where it sits, then pushed towards white where they disagree.
class Waterpaint final : public Resampler {
public:
  Waterpaint() : Resampler(Scaling::Waterpaint) {}
private:
  struct Axis { s32 lo, mid, hi; float f; };
  static Axis axis(double t, s32 n) {
    return Axis{clampi(static_cast<s32>(std::floor(t - 0.5)), 0, n - 1), clampi(static_cast<s32>(std::floor(t)), 0, n - 1),
                clampi(static_cast<s32>(std::floor(t + 0.5)), 0, n - 1), static_cast<float>(frac(t + 0.5))};
  }
  void setup() override {
    ax_.resize(n_); ay_.resize(h_);
    for (u32 x = 0; x < n_; ++x) ax_[x] = axis(centre(x + x0_, w_, SCREEN_W), SCREEN_W);
    for (u32 y = 0; y < h_; ++y) ay_[y] = axis(centre(y, h_, SCREEN_H), SCREEN_H);
  }
  u32 window() const override { return 3; }
  u32 last_line(u32 y) const override { return static_cast<u32>(ay_[y].hi); }
  void row(u32 y, u32* out) override {
    const Axis& v = ay_[y];
    const u32* r0 = line_at(v.lo);
    const u32* r1 = line_at(v.mid);
    const u32* r2 = line_at(v.hi);
    for (u32 x = 0; x < n_; ++x) {
      const Axis& u = ax_[x];
      u32 px = 0xFF000000u;
      for (int ch = 0; ch < 3; ++ch) {
        const int sh = 16 - 8 * ch;
        auto at = [sh](const u32* line, s32 i) { return ((line[i] >> sh) & 255) / 255.0f; };
        // cXY: X across (0 left, 2 right), Y down, half a DS pixel either way.
        const float c00 = at(r0, u.lo), c01 = at(r1, u.lo), c02 = at(r2, u.lo);
        const float c10 = at(r0, u.mid), c12 = at(r2, u.mid);
        const float c20 = at(r0, u.hi), c21 = at(r1, u.hi), c22 = at(r2, u.hi);
        const float first = c00 + u.f * (c20 - c00), second = c02 + u.f * (c22 - c02);
        const float mid_h = c01 + u.f * (c21 - c01), mid_v = c10 + v.f * (c12 - c10);
        const float res = first + v.f * (second - first);
        const float fin = 0.26f * (res + mid_h + mid_v) + 3.5f * std::fabs(res - (mid_h + 0.5f * (mid_v - mid_h)));
        const float c = std::min(1.0f, std::max(0.0f, fin - 0.8f * std::min(100.0f, std::max(0.0f, fin - 0.8f))));
        px |= static_cast<u32>(std::lround(c * 255.0f)) << sh;
      }
      out[x] = px;
    }
  }
  std::vector<Axis> ax_, ay_;
};

} // namespace

// ---- the resampler's frame -----------------------------------------------------

void Resampler::build(u32 w, u32 h, u32 x0, u32 x1) {
  w_ = w; h_ = h; x0_ = x0; x1_ = std::max(x0, x1); n_ = x1_ - x0_;
  setup();
  lines_.assign(static_cast<size_t>(window()) * SCREEN_W, 0xFF000000u);
  scratch_.assign(n_, 0);
  next_row_ = 0;
  kept_ = 0;
}

const u32* Resampler::line_at(s32 l) const {
  const u32 c = static_cast<u32>(clampi(l, 0, SCREEN_H - 1));
  return &lines_[static_cast<size_t>(c % window()) * SCREEN_W];
}

void Resampler::run(const Gpu::ScaleTarget& t, u32 line, const u32* src) {
  if (!n_ || (t.xrun && t.xrun[SCREEN_W] != n_)) return;
  const u64 t0 = timing_on() ? timing_now() : 0;
  if (line == 0) next_row_ = 0;
  std::memcpy(&lines_[static_cast<size_t>(line % window()) * SCREEN_W], src, SCREEN_W * sizeof(u32));
  kept_ = line;
  on_line(line);
  const u32 y_hi = t.y_hi ? t.y_hi : t.h;
  while (next_row_ < h_ && last_line(next_row_) <= line) {
    const u32 y = next_row_++;
    if (y < t.y_lo || y >= y_hi) continue;
    row(y, scratch_.data());
    u32* dst = t.px + static_cast<size_t>(y - t.y_lo) * t.pitch;
    const Gpu::PatternRows* p = t.pattern;
    if (p && t.modulate && p->w == n_ && !p->plain[p->row_class[y]]) t.modulate(*p, p->row_class[y], scratch_.data(), dst);
    else std::memcpy(dst, scratch_.data(), n_ * sizeof(u32));
  }
  if (t0) timing_add_scaling(timing_now() - t0);
}

void Resampler::frame(const u32* src, u32* dst, u32 pitch, const Gpu::PatternRows* pattern) {
  Gpu::ScaleTarget t;
  t.px = dst; t.pitch = pitch; t.h = h_; t.y_hi = h_;
  t.pattern = pattern;
  t.modulate = modulate_class;
  for (u32 l = 0; l < SCREEN_H; ++l) run(t, l, src + static_cast<size_t>(l) * SCREEN_W);
}

std::unique_ptr<Resampler> make_resampler(Scaling s) {
  switch (s) {
  case Scaling::Off: case Scaling::Count: return nullptr;
  case Scaling::Scale3x: return std::make_unique<Scale3x>();
  case Scaling::Aa4: return std::make_unique<Aa4>();
  case Scaling::Waterpaint: return std::make_unique<Waterpaint>();
  default: return std::make_unique<Separable>(s);
  }
}

void shimmer_grid_shading(u32 len, u32 n, u32 from, u32 count, std::vector<double>& out) {
  out.resize(count);
  const double inv = static_cast<double>(n) / len;
  for (u32 i = 0; i < count; ++i) shimmer_grid_point(from + i, inv, &out[i]);
}

// ---- FILTER ------------------------------------------------------------------

void NeighbourFilter::push(u32 line, const u32* src) {
  std::memcpy(lines_[line % 3], src, sizeof lines_[0]);
  last_ = static_cast<s32>(line);
}

void NeighbourFilter::pull(u32 line, u32* out) {
  const u64 t0 = timing_on() ? timing_now() : 0;
  auto row = [&](s32 l) { return lines_[static_cast<u32>(clampi(l, 0, std::min<s32>(last_, SCREEN_H - 1))) % 3]; };
  const u32* up = row(static_cast<s32>(line) - 1);
  const u32* mid = row(static_cast<s32>(line));
  const u32* dn = row(static_cast<s32>(line) + 1);
  for (s32 x = 0; x < static_cast<s32>(SCREEN_W); ++x) {
    const s32 l = std::max(0, x - 1), r = std::min<s32>(SCREEN_W - 1, x + 1);
    double out_c[3];
    if (kind_ == Filter::FastSharpen) {
      // guest(r)'s, SHARPEN 1, CONTR 0.07, DETAILS 1, at the DS's own pixels
      // with GL_LINEAR's third-of-a-pixel taps.
      double c11[3], c10[3], c01[3], c12[3], c21[3];
      unpack(mid[x], c11);
      auto lerp = [](u32 p, u32 q, double f, double o[3]) { double u[3], v[3]; unpack(p, u); unpack(q, v); for (int k = 0; k < 3; ++k) o[k] = u[k] + f * (v[k] - u[k]); };
      lerp(up[x], up[r], 0.3333, c10);
      lerp(mid[l], up[l], 0.3333, c01);
      lerp(dn[x], dn[l], 0.3333, c12);
      lerp(mid[r], dn[r], 0.3333, c21);
      const double contrast = 0.14 + (0.07 - 0.14) * std::max({c11[0], c11[1], c11[2]});
      for (int k = 0; k < 3; ++k) {
        const double b11 = (c10[k] + c01[k] + c12[k] + c21[k]) * 0.25;
        const double mn = std::min(std::min(std::min(c10[k], c01[k]), std::min(c12[k], c21[k])), c11[k] * (1.0 - contrast));
        const double mx = std::max(std::max(std::max(c10[k], c01[k]), std::max(c12[k], c21[k])), c11[k] * (1.0 + contrast));
        out_c[k] = std::min(mx, std::max(mn, 2.0 * c11[k] - b11));
      }
    } else {
      // pixel_art_AA, with its own letters: A is up and right, C up and left.
      const u32 A = up[r] & 0xFFFFFFu, B = up[x] & 0xFFFFFFu, C = up[l] & 0xFFFFFFu;
      const u32 D = mid[r] & 0xFFFFFFu, F = mid[l] & 0xFFFFFFu;
      const u32 G = dn[r] & 0xFFFFFFu, H = dn[x] & 0xFFFFFFu, I = dn[l] & 0xFFFFFFu;
      double e[3], d[3], f[3], h[3], bb[3];
      unpack(mid[x], e); unpack(D, d); unpack(F, f); unpack(H, h); unpack(B, bb);
      auto lum = [](const double c[3]) { return 0.3 * c[0] + 0.6 * c[1] + 0.1 * c[2]; };
      auto same = [](const double a3[3], const double b3[3]) { return a3[0] == b3[0] && a3[1] == b3[1] && a3[2] == b3[2]; };
      auto avg = [](double a3[3], const double b3[3]) { for (int k = 0; k < 3; ++k) a3[k] = (a3[k] + b3[k]) / 2.0; };
      const u32 E0 = mid[x] & 0xFFFFFFu;
      const bool L = D == B && B == C && E0 != D && B != A;
      const bool R = A == B && A == F && E0 != F && B != C;
      const bool DL = D == H && D == I && E0 != D && H != G;
      const bool DR = F == G && F == H && E0 != F && H != I;
      if ((L && lum(e) < lum(d)) || (DL && lum(e) < lum(d))) avg(e, d);
      if ((R && lum(e) < lum(f)) || (DR && lum(e) < lum(f))) avg(e, f);
      const bool GL = same(e, h) && same(e, f) && !same(e, d);
      const bool GR = same(e, d) && same(e, h) && !same(e, f);
      const bool GDL = same(e, bb) && same(e, f) && !same(e, d);
      const bool GDR = same(e, d) && same(e, bb) && !same(e, f);
      if ((GL && lum(e) > lum(d)) || (GDL && lum(e) > lum(d))) avg(e, d);
      if ((GR && lum(e) > lum(f)) || (GDR && lum(e) > lum(f))) avg(e, f);
      const bool SL = B == D && B == G && !same(e, d);
      const bool SR = B == F && B == I && !same(e, f);
      const bool SDL = H == D && H == A && !same(e, d);
      const bool SDR = H == F && H == C && !same(e, f);
      if ((SL && lum(e) < lum(d)) || (SDL && lum(e) < lum(d))) avg(e, d);
      if ((SR && lum(e) < lum(f)) || (SDR && lum(e) < lum(f))) avg(e, f);
      for (int k = 0; k < 3; ++k) out_c[k] = e[k];
    }
    out[x] = pack(out_c);
  }
  if (t0) timing_add_filter(timing_now() - t0);
}

void build_curve_map(Curve c, u32 rw, u32 rh, u32 x0, u32 x1, u32 y0, u32 y1, std::vector<u32>& map) {
  map.clear();
  if (c == Curve::Off || x1 <= x0 || y1 <= y0) return;
  // barrel-distortion.glsl: BARREL_DISTORTION 0.25, its rescale 1 - 0.25 * that.
  const double k = 0.25, rescale = 1.0 - 0.25 * k;
  const u32 w = x1 - x0;
  map.resize(static_cast<size_t>(w) * (y1 - y0));
  for (u32 y = y0; y < y1; ++y) {
    const double ty = (y + 0.5) / rh - 0.5;
    u32* out = map.data() + static_cast<size_t>(y - y0) * w;
    for (u32 x = x0; x < x1; ++x) {
      const double tx = (x + 0.5) / rw - 0.5;
      const double rsq = tx * tx + ty * ty;
      const double ux = (tx + tx * k * rsq) * rescale, uy = (ty + ty * k * rsq) * rescale;
      u32 m = kCurveBlack;
      if (std::fabs(ux) <= 0.5 && std::fabs(uy) <= 0.5) {
        const u32 sx = std::min(static_cast<u32>((ux + 0.5) * rw), rw - 1), sy = std::min(static_cast<u32>((uy + 0.5) * rh), rh - 1);
        if (sx >= x0 && sx < x1 && sy >= y0 && sy < y1) m = (sy - y0) * w + (sx - x0);
      }
      out[x - x0] = m;
    }
  }
}

void curve_row(const u32* map, const u32* src, u32 n, u32* dst) {
  for (u32 i = 0; i < n; ++i) dst[i] = map[i] == kCurveBlack ? 0xFF000000u : src[map[i]];
}

} // namespace ds::sdl::shader
