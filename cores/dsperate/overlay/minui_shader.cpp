// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. See minui_shader.h, which also
// credits the shaders each preset comes from.
#include "frontend/sdl/minui_shader.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#if DSPERATE_NEON
#include <arm_neon.h>
#endif

// The core's pow, pinned to the glibc symbol the ABI gate allows (pow_compat.c).
extern "C" double ds_pow_compat(double x, double y);

namespace ds::sdl::shader {
namespace {

constexpr double kPi = 3.14159265358979323846;

double clamp01(double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }
double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
double mix(double a, double b, double t) { return a + (b - a) * t; }
double smoothstep(double e0, double e1, double x) {
  const double t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3.0 - 2.0 * t);
}
double powp(double x, double e) { return x <= 0.0 ? 0.0 : ds_pow_compat(x, e); }

// Row-major 3x3, the GLSL mat3(...) constants already transposed.
void mat3(const double m[9], double c[3]) {
  const double r = m[0] * c[0] + m[1] * c[1] + m[2] * c[2];
  const double g = m[3] * c[0] + m[4] * c[1] + m[5] * c[2];
  const double b = m[6] * c[0] + m[7] * c[1] + m[8] * c[2];
  c[0] = r; c[1] = g; c[2] = b;
}

// nds-color (hunterk, Pokefan531): gamma 1.91 in and out, lum 0.89.
void nds(double c[3]) {
  static const double m[9] = {0.87, 0.255, -0.125,
                              0.10, 0.645, 0.255,
                              0.10, 0.17, 0.73};
  for (int k = 0; k < 3; ++k) c[k] = clamp01(powp(c[k], 1.91) * 0.89);
  mat3(m, c);
  for (int k = 0; k < 3; ++k) c[k] = clamp01(powp(c[k], 1.0 / 1.91));
}

// Natural Vision (ShadX): gamma 2.2 in, YIQ with Y raised to 1.1 and I and Q
// scaled by 1.1, clamped to YIQ's range, gamma 2.2 out. The NTSC matrices.
void natural_vision(double c[3]) {
  static const double to_yiq[9] = {0.299, 0.587, 0.114,
                                   0.595716, -0.274453, -0.321263,
                                   0.211456, -0.522591, 0.311135};
  static const double to_rgb[9] = {1.0, 0.95629572, 0.62102442,
                                   1.0, -0.27212210, -0.64738060,
                                   1.0, -1.10698902, 1.70461500};
  for (int k = 0; k < 3; ++k) c[k] = powp(c[k], 2.2);
  mat3(to_yiq, c);
  c[0] = clampd(powp(c[0], 1.1), 0.0, 1.0);
  c[1] = clampd(c[1] * 1.1, -0.595716, 0.595716);
  c[2] = clampd(c[2] * 1.1, -0.522591, 0.522591);
  mat3(to_rgb, c);
  for (int k = 0; k < 3; ++k) c[k] = clamp01(powp(c[k], 1.0 / 2.2));
}

// The RG DS panel tones: gamma 2.2 in, a channel-bleed matrix, gamma out,
// then black and white lifted to the panel's.
void panel(double c[3], const double m[9], double gout, const double lo[3], const double hi[3]) {
  for (int k = 0; k < 3; ++k) c[k] = powp(c[k], 2.2);
  mat3(m, c);
  for (int k = 0; k < 3; ++k) c[k] = clamp01(mix(lo[k], hi[k], powp(c[k], 1.0 / gout)));
}

// vivid()/the beads' and tiles' tone: saturation that favours dull colours,
// then part of the way to a smoothstep contrast curve.
void vivid(double c[3], double k0, double k1, double curve) {
  const double l = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
  const double chroma = std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]});
  const double k = k0 + k1 * (1.0 - chroma);
  for (int i = 0; i < 3; ++i) {
    const double v = clamp01(mix(l, c[i], k));
    c[i] = mix(v, v * v * (3.0 - 2.0 * v), curve);
  }
}

// Pokefan531's GBA model, as rgds-core's gba_color has it.
void gba(double c[3]) {
  static const double m[9] = {0.82, 0.24, -0.06,
                              0.125, 0.665, 0.21,
                              0.195, 0.075, 0.73};
  for (int k = 0; k < 3; ++k) c[k] = clamp01(powp(c[k], 2.7) * 0.94);
  mat3(m, c);
  for (int k = 0; k < 3; ++k) c[k] = clamp01(powp(c[k], 1.0 / 2.2));
}

void mono(double c[3], const double ink[3], const double paper[3]) {
  const double l = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
  for (int k = 0; k < 3; ++k) c[k] = mix(ink[k], paper[k], l);
}

// The 8-bit value the output stage gives a 6-bit DS colour (expand_colours).
u32 expand6(u32 c6) { return (c6 << 2) | (c6 >> 4); }

u8 to_byte(double v) { return static_cast<u8>(std::lround(clamp01(v) * 255.0)); }

// ---- timing ----

std::atomic<u64> g_colour_ns{0}, g_pattern_ns{0}, g_glow_ns{0}, g_scaling_ns{0}, g_filter_ns{0}, g_curve_ns{0};
u64 now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
}

} // namespace

bool parse_colour(const std::string& v, Colour& out) {
  for (u8 i = 0; i < kColourCount; ++i) if (v == kColours[i].value) { out = static_cast<Colour>(i); return true; }
  out = Colour::Off;
  return false;
}
bool parse_pattern(const std::string& v, Pattern& out) {
  for (u8 i = 0; i < kPatternCount; ++i) if (v == kPatterns[i].value) { out = static_cast<Pattern>(i); return true; }
  out = Pattern::Off;
  return false;
}
const char* colour_name(Colour c) { return c < Colour::Count ? kColours[static_cast<u8>(c)].value : "off"; }
const char* pattern_name(Pattern p) { return p < Pattern::Count ? kPatterns[static_cast<u8>(p)].value : "off"; }

bool parse_scaling(const std::string& v, Scaling& out) {
  for (u8 i = 0; i < kScalingCount; ++i) if (v == kScalings[i].value) { out = static_cast<Scaling>(i); return true; }
  out = Scaling::Off;
  return false;
}
bool parse_filter(const std::string& v, Filter& out) {
  for (u8 i = 0; i < kFilterCount; ++i) if (v == kFilters[i].value) { out = static_cast<Filter>(i); return true; }
  out = Filter::Off;
  return false;
}
bool parse_curve(const std::string& v, Curve& out) {
  for (u8 i = 0; i < kCurveCount; ++i) if (v == kCurves[i].value) { out = static_cast<Curve>(i); return true; }
  out = Curve::Off;
  return false;
}

const Preset* find_preset(const std::string& v) {
  for (const Preset& p : kPresetTable) if (v == p.value) return &p;
  return nullptr;
}

std::vector<std::pair<std::string, std::string>> preset_values(const Preset& p) {
  std::vector<std::pair<std::string, std::string>> out = {
    {"video.colour", kColours[static_cast<u8>(p.colour)].value},
    {"video.scaling", kScalings[static_cast<u8>(p.scaling)].value},
    {"video.filter", kFilters[static_cast<u8>(p.filter)].value},
    {"video.pattern", kPatterns[static_cast<u8>(p.pattern)].value},
    {"video.glow", kGlows[static_cast<u8>(p.glow)].value},
    {"video.curve", kCurves[static_cast<u8>(p.curve)].value},
  };
  for (const char* k : kGlowNumberKeys) out.emplace_back(k, "auto");
  return out;
}

bool preset_matches(const Preset& p, const std::function<std::string(const char*)>& get) {
  for (const auto& kv : preset_values(p)) {
    std::string v = get(kv.first.c_str());
    // Unset rows are their defaults: GLOW and its numbers auto, the rest off.
    if (v.empty()) v = kv.first.rfind("video.glow", 0) == 0 || kv.first == "video.bloom_threshold" ? "auto" : "off";
    if (v != kv.second) return false;
  }
  return true;
}

bool pattern_is_table(Pattern p) { return p != Pattern::Off && p < Pattern::Count; }

Tone pattern_tone(Pattern p) {
  return p == Pattern::ZfastLcd ? Tone::ZfastGamma : p == Pattern::RetroV2 ? Tone::RetroV2 : Tone::None;
}
bool pattern_is_mask(Pattern p) {
  return p == Pattern::Lcd || p == Pattern::ZfastLcd || p == Pattern::Scanlines || p == Pattern::SoftGrid;
}

// ---- colour ----------------------------------------------------------------

void colour_of(Colour k, Tone tone, const double in[3], double out[3]) {
  double c[3] = {in[0], in[1], in[2]};
  switch (k) {
  case Colour::Off: case Colour::Count: break;
  case Colour::Nds: nds(c); break;
  case Colour::NaturalVision: natural_vision(c); break;
  case Colour::NdsNaturalVision: {
    // Two passes in rgds-core, through an 8-bit framebuffer between them.
    nds(c);
    for (double& v : c) v = to_byte(v) / 255.0;
    natural_vision(c);
    break;
  }
  case Colour::DsClassic: {
    static const double m[9] = {0.86, 0.11, 0.03, 0.07, 0.84, 0.09, 0.04, 0.10, 0.86};
    static const double lo[3] = {0.045, 0.05, 0.06}, hi[3] = {0.92, 0.94, 0.96};
    panel(c, m, 2.45, lo, hi);
    break;
  }
  case Colour::DsLite: {
    static const double m[9] = {0.90, 0.08, 0.02, 0.05, 0.89, 0.06, 0.03, 0.07, 0.90};
    static const double lo[3] = {0.025, 0.03, 0.04}, hi[3] = {0.97, 0.98, 1.0};
    panel(c, m, 2.3, lo, hi);
    break;
  }
  case Colour::Natural: {
    static const double m[9] = {0.88, 0.09, 0.03, 0.07, 0.86, 0.07, 0.04, 0.08, 0.88};
    static const double lo[3] = {0.03, 0.03, 0.04}, hi[3] = {0.97, 0.97, 0.98};
    panel(c, m, 2.3, lo, hi);
    break;
  }
  case Colour::Vivid: vivid(c, 1.15, 0.30, 0.45); break;
  case Colour::Bold: vivid(c, 1.2, 0.35, 0.35); break;
  case Colour::Gba: gba(c); break;
  case Colour::WarmPaper: {
    static const double k0[3] = {0.055, 0.04, 0.025}, t[3] = {1.0, 0.95, 0.86};
    for (int i = 0; i < 3; ++i) c[i] = mix(k0[i], c[i], 0.94) * t[i];
    break;
  }
  case Colour::CoolCrisp: {
    static const double t[3] = {0.93, 0.98, 1.0};
    for (int i = 0; i < 3; ++i) c[i] = clamp01((c[i] - 0.5) * 1.06 + 0.5) * t[i];
    break;
  }
  case Colour::PocketGreen: {
    static const double ink[3] = {0.10, 0.19, 0.15}, paper[3] = {0.77, 0.84, 0.57};
    mono(c, ink, paper);
    break;
  }
  case Colour::Amber: {
    static const double ink[3] = {0.12, 0.055, 0.018}, paper[3] = {1.0, 0.79, 0.43};
    mono(c, ink, paper);
    break;
  }
  }
  for (int i = 0; i < 3; ++i) {
    c[i] = clamp01(c[i]);
    if (tone == Tone::ZfastGamma) c[i] *= 0.6 + 0.4 * c[i];
    else if (tone == Tone::RetroV2) c[i] = powp(c[i], 2.4 / 2.2);
    out[i] = c[i];
  }
}

void colour_line(const u32* lut, const u32* src, u32* dst) {
  const u64 t0 = timing_on() ? now_ns() : 0;
  u32 was = ~src[0], is = 0;
  for (u32 i = 0; i < SCREEN_W; ++i) {
    const u32 c = src[i];
    if (c != was) { was = c; is = lut[colour_index(c)]; }
    dst[i] = is;
  }
  if (t0) g_colour_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
}

ColourTable::~ColourTable() { stop(); }

void ColourTable::stop() {
  if (!build_.joinable()) return;
  cancel_ = true;
  build_.join();
  cancel_ = false;
}

void ColourTable::stage(Colour c, Tone tone) {
  stop();   // a change before the last was done: start again
  pending_ = true;
  staged_colour_ = c;
  staged_tone_ = tone;
  if ((c == Colour::Off || c >= Colour::Count) && tone == Tone::None) { staged_ = -1; built_ = true; return; }
  const int b = live_ == 0 ? 1 : 0;
  staged_ = b;
  built_ = false;
  std::vector<u32>& t = buf_[b];
  t.resize(COLOUR_ENTRIES);
  build_ = std::thread([&t, c, tone, this] {
    for (u32 i = 0; i < COLOUR_ENTRIES; ++i) {
      if ((i & 4095) == 0 && cancel_.load(std::memory_order_relaxed)) return;
      const double in[3] = {expand6((i >> 12) & 63) / 255.0, expand6((i >> 6) & 63) / 255.0, expand6(i & 63) / 255.0};
      double out[3];
      colour_of(c, tone, in, out);
      t[i] = 0xFF000000u | static_cast<u32>(to_byte(out[0])) << 16 | static_cast<u32>(to_byte(out[1])) << 8 | to_byte(out[2]);
    }
    built_.store(true, std::memory_order_release);
  });
}

bool ColourTable::commit() {
  if (!pending_ || !built_.load(std::memory_order_acquire)) return false;
  if (build_.joinable()) build_.join();
  pending_ = false;
  live_ = staged_;
  live_colour_ = staged_colour_;
  live_tone_ = staged_tone_;
  return true;
}

void ColourTable::finish() {
  if (build_.joinable()) build_.join();
  commit();
}

void ColourTable::apply(const u32* src, u32* dst, size_t n) const {
  const u32* lut = this->lut();
  if (!lut) { if (dst != src) std::memcpy(dst, src, n * sizeof(u32)); return; }
  const u64 t0 = timing_on() ? now_ns() : 0;
  u32 was = n ? ~src[0] : 0, is = 0;
  for (size_t i = 0; i < n; ++i) {
    const u32 c = src[i];
    if (c != was) { was = c; is = lut[colour_index(c)]; }
    dst[i] = is;
  }
  if (t0) g_colour_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
}

// ---- pattern ---------------------------------------------------------------

namespace {

// Box-filtered coverage of the R, G and B thirds of a pixel by a panel pixel
// `w` wide centred on x, as rgds-core's stripes().
double stripe_cover(int ch, double x, double w) {
  const double lo = ch / 3.0, hi = lo + 1.0 / 3.0, a = x - 0.5 * w, b = x + 0.5 * w;
  double c = 0.0;
  for (int k = -1; k <= 1; ++k) c += clampd(std::min(b, hi + k) - std::max(a, lo + k), 0.0, w);
  return c / w;
}

// The RG DS grid: dark between pixels by how far the panel pixel is into the seam.
double edge(double f, double off, double w) { return clamp01((std::min(f, 1.0 - f) - off) / w + 0.5); }

} // namespace

// The pattern for dark pixels; pattern_at adds the bright one.
void pattern_at_dark(Pattern p, double fx, double fy, double ax, double ay, double mul[3], double add[3], const Place& at);

bool pattern_spread(Pattern p, u8& lo, u8& hi) {
  // vivid_lcd: smoothstep(0.75, 1.0, dot(c, vec3(0.333))).
  if (p == Pattern::RgbStripes) { lo = 191; hi = 255; return true; }
  if (p == Pattern::Crt) { lo = 0; hi = 255; return true; }
  lo = 0; hi = 255;
  return false;
}

void pattern_at(Pattern p, double fx, double fy, double ax, double ay, double mul[3], double add[3], double bright[3], const Place* at) {
  double spare[3];
  if (!bright) bright = spare;
  const Place top_left;
  pattern_at_dark(p, fx, fy, ax, ay, mul, add, at ? *at : top_left);
  for (int i = 0; i < 3; ++i) bright[i] = mul[i];
  const double s = clamp01(1.0 / std::max(ax, ay) - 1.0);
  if (p == Pattern::RgbStripes) {
    // The grid alone: the stripes are gone on highlights.
    const double g = mix(0.62, 1.0, edge(fx, 0.08, ax) * edge(fy, 0.08, ay));
    for (int i = 0; i < 3; ++i) bright[i] = mix(1.0, g * 1.12, s);
  } else if (p == Pattern::Crt) {
    // A bright beam is wider: it all but fills the gap to the next row.
    const double beam = mix(0.80, 1.0, std::exp(-3.0 * (fy - 0.5) * (fy - 0.5)));
    for (int i = 0; i < 3; ++i) {
      const double grille = 1.0 + 0.25 * (3.0 * stripe_cover(i, fx, ax) - 1.0);
      bright[i] = mix(1.0, beam * grille * 1.05, s);
    }
  }
}

void pattern_at_dark(Pattern p, double fx, double fy, double ax, double ay, double mul[3], double add[3], const Place& at) {
  for (int i = 0; i < 3; ++i) { mul[i] = 1.0; add[i] = 0.0; }
  auto all = [&](double m) { for (int i = 0; i < 3; ++i) mul[i] = m; };
  const double aa = std::max(ax, ay);
  const double s = clamp01(1.0 / aa - 1.0);   // the RG DS fade-in over 1x..2x
  auto grid = [&](double off, double dark, double gain, double stripe) {
    const double g = mix(dark, 1.0, edge(fx, off, ax) * edge(fy, off, ay));
    for (int i = 0; i < 3; ++i) {
      const double st = stripe > 0.0 ? 1.0 + stripe * (3.0 * stripe_cover(i, fx, ax) - 1.0) : 1.0;
      mul[i] = mix(1.0, g * st * gain, s);
    }
  };
  switch (p) {
  case Pattern::Off: case Pattern::Count: return;
  // NextUI's (skeleton/BASE/Shaders/glsl), as their own constants have them.
  case Pattern::Lcd3x: {
    // Gigaherz: one dark scanline per DS row, and blue against red and green across it.
    const double yf = (6.0 + std::sin(2.0 * kPi * fy)) / 7.0, xs = std::sin(2.0 * kPi * fx);
    mul[0] = mul[1] = yf * (24.0 + xs) / 25.0;
    mul[2] = yf * (24.0 - xs) / 25.0;
    return;
  }
  case Pattern::Scanline:
    // A sine down each DS row, and a ripple that flips every panel column.
    all(0.95 + 0.05 * std::sin(kPi * at.x) + 0.15 * std::sin(2.0 * kPi * fy));
    return;
  case Pattern::ResScanlines: case Pattern::ResScanlinesAuto: {
    // A fixed number of lines over the view (224, or the DS's 192 on auto),
    // counted up from its bottom as GL counts gl_FragCoord.
    const double lines = p == Pattern::ResScanlines ? 224.0 : SCREEN_H;
    const double angle = (at.h - at.y) / at.h * kPi * lines + 0.5;
    all(std::fabs(1.25 * std::sin(angle)));
    return;
  }
  case Pattern::RetroV2: {
    // guest(r)'s: a light pixel with dark borders 0.16 of a DS pixel wide.
    // Its 2.4 and 1/2.2 gammas leave c^(12/11), in the colour table, and
    // the shading to the power 1/2.2 here.
    auto border = [](double f, double ps) { return std::max(0.0, std::min(ps, clamp01(f + 0.5 * ps) - 0.84)) / ps; };
    const double edge_x = border(fx, ax), edge_y = border(fy, ay);
    const double m = mix(1.04 + fx * fy, 0.36, std::max(edge_x, edge_y));
    all(powp(m, 1.0 / 2.2));
    return;
  }
  case Pattern::Lcd: {
    // lcd1x with rgds-core's brightening (BRIGHTEN_SCANLINES 24, BRIGHTEN_LCD 6).
    const double yf = (24.0 + std::sin(2.0 * kPi * (fy - 0.25))) / 25.0;
    const double xf = (6.0 + std::sin(2.0 * kPi * (fx - 0.25))) / 7.0;
    all(xf * yf);
    return;
  }
  case Pattern::ZfastLcd: {
    // zfast_lcd with rgds-core's BORDERMULT 16.
    double y = std::max(std::fabs(fx - 0.5), std::fabs(fy - 0.5));
    y *= y;
    const double yy = y * y, yyy = yy * y;
    all(1.0 - 16.0 * (yy - 2.7 * yyy));
    return;
  }
  case Pattern::Scanlines:
    all(mix(0.88, 1.0, smoothstep(0.05, 0.4, fy)));
    return;
  case Pattern::SoftGrid:
    all(0.90 + 0.10 * smoothstep(0.04, 0.23, fx) * smoothstep(0.04, 0.23, fy));
    return;
  case Pattern::DsClassic: grid(0.07, 0.74, 1.04, 0.16); return;
  case Pattern::DsLite: grid(0.05, 0.78, 1.03, 0.0); return;
  case Pattern::Gba: grid(0.05, 0.80, 1.04, 0.0); return;
  case Pattern::RgbStripes: grid(0.08, 0.62, 1.12, 0.22); return;
  case Pattern::Beads: {
    const double qx = fx - 0.5, qy = fy - 0.5;
    const double bead = clamp01((0.56 - std::hypot(qx, qy)) / aa + 0.5);
    const double dome = 1.0 - 0.38 * smoothstep(0.0, 0.62, std::hypot(qx + 0.12, qy + 0.12));
    const double spec = smoothstep(0.16, 0.0, std::hypot(qx + 0.15, qy + 0.16)) * 0.45;
    const double m = (1.0 - s) + s * ((1.0 - bead) * 0.40 + bead * (1.18 * dome + 0.4 * spec));
    const double a = s * ((1.0 - bead) * 0.01 + bead * 0.6 * spec);
    for (int i = 0; i < 3; ++i) { mul[i] = m; add[i] = a; }
    return;
  }
  case Pattern::Crt: {
    // The pak's own, not rgds-core's: a beam brightest across the middle of
    // each DS row with a dark gap between rows, under an aperture grille of
    // one red, green and blue stripe per DS pixel, lifted to make up for
    // the light both take away. Fades in over 1x..2x like the RG DS ones.
    // This is the dark pixels' beam; pattern_at widens it for bright ones.
    const double beam = mix(0.45, 1.0, std::exp(-8.0 * (fy - 0.5) * (fy - 0.5)));
    for (int i = 0; i < 3; ++i) {
      const double grille = 1.0 + 0.25 * (3.0 * stripe_cover(i, fx, ax) - 1.0);
      mul[i] = mix(1.0, beam * grille * 1.2, s);
    }
    return;
  }
  case Pattern::Tiles: {
    const double qx = fx - 0.5, qy = fy - 0.5;
    const double inside = 0.16 - std::hypot(std::max(std::fabs(qx) - 0.28, 0.0), std::max(std::fabs(qy) - 0.28, 0.0));
    const double tile = clamp01(inside / aa + 0.5);
    const double rim = smoothstep(0.12, 0.0, inside);
    const double nx = qx + 0.0001, ny = qy + 0.0001, len = std::hypot(nx, ny);
    const double facing = (nx * -0.7071 + ny * -0.7071) / len;
    const double d = qx + qy;
    const double gloss = smoothstep(0.05, -0.12, d) * smoothstep(-0.48, -0.30, d) * 0.22 * (1.0 - rim);
    const double m = (1.0 - s) + s * ((1.0 - tile) * 0.48 + tile * (1.06 + 0.32 * rim * facing + 0.3 * gloss));
    const double a = s * ((1.0 - tile) * 0.01 + tile * 0.7 * gloss);
    for (int i = 0; i < 3; ++i) { mul[i] = m; add[i] = a; }
    return;
  }
  }
}

namespace {

// Where panel pixel x sits in the run of the source pixel it shows, when
// `n` source pixels cover `len` panel pixels in runs [ceil(s*len/n), ceil((s+1)*len/n)).
double run_phase(u32 x, u32 len, u32 n) {
  const u32 s = static_cast<u32>((static_cast<u64>(x) * n) / len);
  const u32 a = static_cast<u32>((static_cast<u64>(s) * len + n - 1) / n);
  const u32 b = static_cast<u32>((static_cast<u64>(s + 1) * len + n - 1) / n);
  return (static_cast<double>(x - a) + 0.5) / static_cast<double>(b - a);
}

u8 q7(double m) { return static_cast<u8>(std::clamp<long>(std::lround(m * 128.0), 0, 255)); }

} // namespace

bool build_pattern(Pattern p, u32 w, u32 h, u32 x0, u32 x1, PatternStore& out) {
  out.clear();
  if (!pattern_is_table(p) || w <= SCREEN_W || h <= SCREEN_H || x0 >= x1 || x1 > w) return false;
  const u32 n = x1 - x0;
  const double ax = static_cast<double>(SCREEN_W) / w, ay = static_cast<double>(SCREEN_H) / h;
  std::vector<double> fx(n);
  for (u32 i = 0; i < n; ++i) fx[i] = run_phase(i + x0, w, SCREEN_W);
  // Patterns placed down the view rather than by the DS pixel see a row's
  // own place, so rows are classed by it as well (scanline's ripple is by
  // column, which every row's table already has).
  const bool placed = p == Pattern::ResScanlines || p == Pattern::ResScanlinesAuto;
  const size_t row_bytes = static_cast<size_t>(n) * 4;
  u8 lo = 0, hi = 255;
  const bool spread = pattern_spread(p, lo, hi);
  std::vector<u8> mul_row(row_bytes), add_row(row_bytes), bright_row(row_bytes);
  std::map<double, u16> by_phase;   // rows at the same phase share a class without recomputing it
  out.row_class.assign(h, 0);
  for (u32 y = 0; y < h; ++y) {
    const double fy = run_phase(y, h, SCREEN_H);
    const double key = placed ? fy + 2.0 * (y + 1) : fy;
    const auto hit = by_phase.find(key);
    if (hit != by_phase.end()) { out.row_class[y] = hit->second; continue; }
    bool plain = true;
    for (u32 i = 0; i < n; ++i) {
      double m[3], a[3], b[3];
      const Place at{i + x0 + 0.5, y + 0.5, static_cast<double>(w), static_cast<double>(h)};
      pattern_at(p, fx[i], fy, ax, ay, m, a, b, &at);
      // Pixel byte order: B, G, R, A. Alpha passes through at 1.0.
      u8* mb = &mul_row[static_cast<size_t>(i) * 4];
      u8* ab = &add_row[static_cast<size_t>(i) * 4];
      u8* bb = &bright_row[static_cast<size_t>(i) * 4];
      mb[0] = q7(m[2]); mb[1] = q7(m[1]); mb[2] = q7(m[0]); mb[3] = 128;
      ab[0] = to_byte(a[2]); ab[1] = to_byte(a[1]); ab[2] = to_byte(a[0]); ab[3] = 0;
      bb[0] = q7(b[2]); bb[1] = q7(b[1]); bb[2] = q7(b[0]); bb[3] = 128;
      plain = plain && mb[0] == 128 && mb[1] == 128 && mb[2] == 128 && !ab[0] && !ab[1] && !ab[2] &&
              (!spread || (bb[0] == 128 && bb[1] == 128 && bb[2] == 128));
    }
    // A row the same as one already made is that class.
    u32 c = 0;
    for (; c < out.classes; ++c)
      if (!std::memcmp(&out.mul[c * row_bytes], mul_row.data(), row_bytes) && !std::memcmp(&out.add[c * row_bytes], add_row.data(), row_bytes) &&
          (!spread || !std::memcmp(&out.bright[c * row_bytes], bright_row.data(), row_bytes))) break;
    if (c == out.classes) {
      out.mul.insert(out.mul.end(), mul_row.begin(), mul_row.end());
      out.add.insert(out.add.end(), add_row.begin(), add_row.end());
      if (spread) out.bright.insert(out.bright.end(), bright_row.begin(), bright_row.end());
      out.plain.push_back(plain ? 1 : 0);
      ++out.classes;
    }
    by_phase[key] = static_cast<u16>(c);
    out.row_class[y] = static_cast<u16>(c);
  }
  if (spread) { out.weight.resize(256); spread_weights(lo, hi, out.weight.data()); }
  out.rows = ds::gpu::Gpu::PatternRows{n, out.mul.data(), out.add.data(), out.row_class.data(), out.plain.data(),
                                        spread ? out.bright.data() : nullptr, spread ? out.weight.data() : nullptr};
  return true;
}

void spread_weights(u8 lo, u8 hi, u8 out[256]) {
  for (u32 m = 0; m < 256; ++m) {
    const double t = hi > lo ? clamp01((static_cast<double>(m) - lo) / (hi - lo)) : (m >= lo ? 1.0 : 0.0);
    out[m] = to_byte(t * t * (3.0 - 2.0 * t));
  }
}

namespace {
// x / 255, rounded, for x up to 255 * 255.
inline u32 div255(u32 x) { x += 128; return (x + (x >> 8)) >> 8; }
} // namespace

void ref::modulate_row(const u32* src, const u8* mul, const u8* bright, const u8* weight, const u8* add, u32 n, u32* dst) {
  const u8* s = reinterpret_cast<const u8*>(src);
  u8* d = reinterpret_cast<u8*>(dst);
  for (size_t px = 0; px < n; ++px) {
    const size_t at = px * 4;
    const u32 w = bright ? weight[channel_mean(s[at], s[at + 1], s[at + 2])] : 0;
    for (size_t k = 0; k < 4; ++k) {
      const u32 m = bright ? div255(mul[at + k] * (255 - w) + bright[at + k] * w) : mul[at + k];
      const u32 v = std::min<u32>(255, (static_cast<u32>(s[at + k]) * m + 64) >> 7);
      d[at + k] = static_cast<u8>(std::min<u32>(255, v + add[at + k]));
    }
  }
}

void modulate_row(const u32* src, const u8* mul, const u8* bright, const u8* weight, const u8* add, u32 n, u32* dst) {
#if DSPERATE_NEON
  const u8* s = reinterpret_cast<const u8*>(src);
  u8* d = reinterpret_cast<u8*>(dst);
  if (!bright) {
    const size_t bytes = static_cast<size_t>(n) * 4;
    size_t i = 0;
    for (; i + 16 <= bytes; i += 16) {
      const uint8x16_t c = vld1q_u8(s + i), m = vld1q_u8(mul + i), a = vld1q_u8(add + i);
      const uint16x8_t l = vmull_u8(vget_low_u8(c), vget_low_u8(m));
      const uint16x8_t h = vmull_u8(vget_high_u8(c), vget_high_u8(m));
      vst1q_u8(d + i, vqaddq_u8(vcombine_u8(vqrshrn_n_u16(l, 7), vqrshrn_n_u16(h, 7)), a));
    }
    if (i < bytes) ref::modulate_row(src + i / 4, mul + i, nullptr, weight, add + i, static_cast<u32>((bytes - i) / 4), dst + i / 4);
    return;
  }
#if defined(__aarch64__)
  // Eight pixels at a time, a channel to a register; the weight from a
  // 256-byte table in four 64-byte lookups.
  // GCC 8 has no vld1q_u8_x4.
  auto table = [weight](int at) { return uint8x16x4_t{{vld1q_u8(weight + at), vld1q_u8(weight + at + 16), vld1q_u8(weight + at + 32), vld1q_u8(weight + at + 48)}}; };
  const uint8x16x4_t t0 = table(0), t1 = table(64), t2 = table(128), t3 = table(192);
  size_t px = 0;
  for (; px + 8 <= n; px += 8) {
    const uint8x8x4_t c = vld4_u8(s + px * 4), m = vld4_u8(mul + px * 4), b = vld4_u8(bright + px * 4), a = vld4_u8(add + px * 4);
    const uint16x8_t sum = vaddw_u8(vaddl_u8(c.val[0], c.val[1]), c.val[2]);
    // (sum * 21846) >> 16 as vqdmulh's (2 * sum * 10923) >> 16.
    const uint8x8_t idx = vmovn_u16(vreinterpretq_u16_s16(vqdmulhq_n_s16(vreinterpretq_s16_u16(sum), 10923)));
    uint8x8_t w = vqtbl4_u8(t0, idx);
    w = vqtbx4_u8(w, t1, vsub_u8(idx, vdup_n_u8(64)));
    w = vqtbx4_u8(w, t2, vsub_u8(idx, vdup_n_u8(128)));
    w = vqtbx4_u8(w, t3, vsub_u8(idx, vdup_n_u8(192)));
    const uint8x8_t iw = vsub_u8(vdup_n_u8(255), w);
    uint8x8x4_t out;
    for (int k = 0; k < 4; ++k) {
      uint16x8_t x = vmlal_u8(vmull_u8(m.val[k], iw), b.val[k], w);
      x = vaddq_u16(x, vdupq_n_u16(128));
      const uint8x8_t mm = vshrn_n_u16(vaddq_u16(x, vshrq_n_u16(x, 8)), 8);
      out.val[k] = vqadd_u8(vqrshrn_n_u16(vmull_u8(c.val[k], mm), 7), a.val[k]);
    }
    vst4_u8(d + px * 4, out);
  }
  if (px < n) ref::modulate_row(src + px, mul + px * 4, bright + px * 4, weight, add + px * 4, n - static_cast<u32>(px), dst + px);
#else
  ref::modulate_row(src, mul, bright, weight, add, n, dst);
#endif
#else
  ref::modulate_row(src, mul, bright, weight, add, n, dst);
#endif
}

void modulate_class(const ds::gpu::Gpu::PatternRows& p, u32 cls, const u32* src, u32* dst) {
  const u64 t0 = timing_on() ? now_ns() : 0;
  const size_t at = static_cast<size_t>(cls) * p.w * 4;
  modulate_row(src, p.mul + at, p.bright ? p.bright + at : nullptr, p.weight, p.add + at, p.w, dst);
  if (t0) g_pattern_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
}

// ---- a resampler's own grid ---------------------------------------------------

bool build_pattern_from(const std::vector<double>& col, const std::vector<double>& row, PatternStore& out) {
  out.clear();
  const u32 n = static_cast<u32>(col.size()), h = static_cast<u32>(row.size());
  if (!n || !h) return false;
  const size_t row_bytes = static_cast<size_t>(n) * 4;
  std::map<double, u16> by_value;
  out.row_class.assign(h, 0);
  for (u32 y = 0; y < h; ++y) {
    const auto hit = by_value.find(row[y]);
    if (hit != by_value.end()) { out.row_class[y] = hit->second; continue; }
    const u16 c = static_cast<u16>(out.classes++);
    bool plain = true;
    for (u32 i = 0; i < n; ++i) {
      const u8 m = q7(col[i] * row[y]);
      out.mul.insert(out.mul.end(), {m, m, m, 128});
      plain = plain && m == 128;
    }
    out.add.insert(out.add.end(), row_bytes, 0);
    out.plain.push_back(plain ? 1 : 0);
    by_value[row[y]] = c;
    out.row_class[y] = c;
  }
  out.rows = ds::gpu::Gpu::PatternRows{n, out.mul.data(), out.add.data(), out.row_class.data(), out.plain.data(), nullptr, nullptr};
  return true;
}

// ---- the display engine's mask -------------------------------------------------

void draw_mask(Pattern p, u32* stage, int panel_w, int px0, int px1, int py0, int py1, int nx, int ny, int rot) {
  const int pw = px1 - px0, ph = py1 - py0;
  if (!pattern_is_mask(p) || pw <= 0 || ph <= 0 || nx <= 0 || ny <= 0) return;
  const bool turned = rot == 90 || rot == 270;
  // DS pixels per panel pixel, along the DS picture's own x and y.
  const double ax = turned ? static_cast<double>(ny) / ph : static_cast<double>(nx) / pw;
  const double ay = turned ? static_cast<double>(nx) / pw : static_cast<double>(ny) / ph;
  // Only a few distinct phases on each axis: the alpha for each pair, once.
  std::vector<double> ux, uy;
  std::vector<int> ix(static_cast<size_t>(pw)), iy(static_cast<size_t>(ph));
  auto index_of = [](std::vector<double>& seen, double v) {
    for (size_t k = 0; k < seen.size(); ++k) if (seen[k] == v) return static_cast<int>(k);
    seen.push_back(v);
    return static_cast<int>(seen.size() - 1);
  };
  for (int x = 0; x < pw; ++x) ix[static_cast<size_t>(x)] = index_of(ux, run_phase(static_cast<u32>(x), static_cast<u32>(pw), static_cast<u32>(nx)));
  for (int y = 0; y < ph; ++y) iy[static_cast<size_t>(y)] = index_of(uy, run_phase(static_cast<u32>(y), static_cast<u32>(ph), static_cast<u32>(ny)));
  std::vector<u32> alpha(ux.size() * uy.size());
  for (size_t j = 0; j < uy.size(); ++j)
    for (size_t i = 0; i < ux.size(); ++i) {
      // The composite turns the DS picture by rot (DispOut::comp_rect).
      const double cx = ux[i], cy = uy[j];
      double fu = cx, fv = cy;
      switch (rot) {
        case 90:  fu = cy;       fv = 1.0 - cx; break;
        case 180: fu = 1.0 - cx; fv = 1.0 - cy; break;
        case 270: fu = 1.0 - cy; fv = cx;       break;
        default: break;
      }
      double m[3], a[3];
      pattern_at(p, fu, fv, ax, ay, m, a, nullptr, nullptr);
      alpha[j * ux.size() + i] = static_cast<u32>(to_byte(1.0 - m[1])) << 24;
    }
  for (int y = 0; y < ph; ++y) {
    u32* row = stage + static_cast<size_t>(py0 + y) * static_cast<size_t>(panel_w) + px0;
    const u32* a = &alpha[static_cast<size_t>(iy[static_cast<size_t>(y)]) * ux.size()];
    for (int x = 0; x < pw; ++x) row[x] = a[ix[static_cast<size_t>(x)]];
  }
}

// ---- glow ------------------------------------------------------------------

bool parse_glow(const std::string& v, Glow& out) {
  for (u8 i = 0; i < kGlowCount; ++i) if (v == kGlows[i].value) { out = static_cast<Glow>(i); return true; }
  out = Glow::Auto;
  return false;
}
const char* glow_name(Glow g) { return g < Glow::Count ? kGlows[static_cast<u8>(g)].value : "off"; }

GlowSpec glow_defaults(Glow kind, bool for_crt) {
  GlowSpec s;
  s.kind = kind;
  switch (kind) {
  case Glow::Glow:     s.strength = 0.35; s.radius = 3; break;
  case Glow::Bloom:    s.strength = 0.60; s.radius = 4; s.threshold = 0.65; break;
  // CRT's own is fainter and tighter than the row's, under its scanlines.
  case Glow::Halation: s.strength = for_crt ? 0.20 : 0.30; s.radius = for_crt ? 4 : 6; break;
  default: s.kind = Glow::Off; break;
  }
  return s;
}

GlowSpec glow_spec(Glow kind, const std::string& strength, const std::string& radius, const std::string& threshold, bool crt_drawn) {
  bool for_crt = false;
  if (kind == Glow::Auto) { kind = crt_drawn ? Glow::Halation : Glow::Off; for_crt = crt_drawn; }
  GlowSpec s = glow_defaults(kind, for_crt);
  if (s.kind == Glow::Off) return s;
  auto number = [](const std::string& v, double& out) {
    if (v.empty() || v == "auto") return;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (end != v.c_str() && !*end) out = d;
  };
  double r = s.radius;
  number(strength, s.strength);
  number(radius, r);
  number(threshold, s.threshold);
  s.strength = clampd(s.strength, 0.0, 1.0);
  s.radius = static_cast<int>(clampd(std::round(r), 1.0, GLOW_MAX_RADIUS));
  s.threshold = clampd(s.threshold, 0.0, 0.95);
  return s;
}

void GlowFilter::configure(const GlowSpec& s) {
  spec_ = s;
  r_ = s.kind == Glow::Off ? 0 : std::max(1, std::min(GLOW_MAX_RADIUS, s.radius));
  ring_ = static_cast<u32>(2 * r_ + 1);
  // Gaussian, sigma half the radius, in 1/256s with the rounding put back in the middle.
  w_.assign(ring_, 0);
  const double sigma = std::max(0.5, r_ / 2.0);
  double total = 0.0;
  std::vector<double> g(ring_);
  for (int k = -r_; k <= r_; ++k) total += g[static_cast<size_t>(k + r_)] = std::exp(-0.5 * k * k / (sigma * sigma));
  u32 sum = 0;
  for (u32 i = 0; i < ring_; ++i) sum += w_[i] = static_cast<u16>(std::lround(g[i] / total * 256.0));
  w_[static_cast<size_t>(r_)] = static_cast<u16>(w_[static_cast<size_t>(r_)] + 256 - static_cast<s32>(sum));
  // Bloom glows only above its threshold, stretched back to the full range.
  const u32 thr = s.kind == Glow::Bloom ? static_cast<u32>(std::lround(s.threshold * 255.0)) : 0;
  for (u32 c = 0; c < 256; ++c) pass_[c] = c <= thr && thr ? 0 : static_cast<u8>(thr ? (c - thr) * 255 / (255 - thr) : c);
  // Halation is the warm light a CRT's glass scatters: red most, blue least.
  static const double warm[3] = {1.0, 0.6, 0.35}, white[3] = {1.0, 1.0, 1.0};
  const double* tint = s.kind == Glow::Halation ? warm : white;
  for (int ch = 0; ch < 3; ++ch) k_[ch] = static_cast<u32>(std::lround(clamp01(s.strength) * tint[ch] * 256.0));
  orig_.assign(static_cast<size_t>(ring_) * SCREEN_W, 0);
  blur_.assign(static_cast<size_t>(ring_) * 3 * SCREEN_W, 0);
}

void GlowFilter::push(u32 line, const u32* src) {
  const u64 t0 = timing_on() ? now_ns() : 0;
  const u32 slot = line % ring_;
  std::memcpy(&orig_[static_cast<size_t>(slot) * SCREEN_W], src, SCREEN_W * sizeof(u32));
  // Across: each channel's light, edge pixels repeated, through the weights.
  alignas(16) u16 pad[3][SCREEN_W + 2 * GLOW_MAX_RADIUS];
  for (u32 x = 0; x < SCREEN_W; ++x) {
    const u32 c = src[x];
    pad[0][x + static_cast<u32>(r_)] = pass_[(c >> 16) & 255];
    pad[1][x + static_cast<u32>(r_)] = pass_[(c >> 8) & 255];
    pad[2][x + static_cast<u32>(r_)] = pass_[c & 255];
  }
  for (int ch = 0; ch < 3; ++ch) {
    u16* const p = pad[ch];
    for (int k = 0; k < r_; ++k) { p[k] = p[r_]; p[SCREEN_W + static_cast<u32>(r_ + k)] = p[SCREEN_W + static_cast<u32>(r_) - 1]; }
    u16* __restrict out = &blur_[(static_cast<size_t>(slot) * 3 + static_cast<size_t>(ch)) * SCREEN_W];
#if DSPERATE_NEON
    for (u32 x = 0; x < SCREEN_W; x += 8) {
      uint16x8_t v = vmulq_n_u16(vld1q_u16(p + x), w_[0]);
      for (u32 k = 1; k < ring_; ++k) v = vmlaq_n_u16(v, vld1q_u16(p + x + k), w_[k]);
      vst1q_u16(out + x, v);
    }
#else
    const u16 w0 = w_[0];
    for (u32 x = 0; x < SCREEN_W; ++x) out[x] = static_cast<u16>(w0 * p[x]);
    for (u32 k = 1; k < ring_; ++k) {
      const u16 w = w_[k];
      const u16* __restrict in = p + k;
      for (u32 x = 0; x < SCREEN_W; ++x) out[x] = static_cast<u16>(out[x] + w * in[x]);
    }
#endif
  }
  if (t0) g_glow_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
}

void GlowFilter::pull(u32 line, u32* out) {
  const u64 t0 = timing_on() ? now_ns() : 0;
  const u32* c = &orig_[static_cast<size_t>(line % ring_) * SCREEN_W];
  // Down: the lines around this one, the first and last repeated past the
  // edges, to the glow's light per channel (0..255).
  alignas(16) u16 glow[3][SCREEN_W];
  const u16* rows[2 * GLOW_MAX_RADIUS + 1];
  for (int k = -r_; k <= r_; ++k) {
    const s32 at = std::min<s32>(SCREEN_H - 1, std::max<s32>(0, static_cast<s32>(line) + k));
    rows[k + r_] = &blur_[static_cast<size_t>(static_cast<u32>(at) % ring_) * 3 * SCREEN_W];
  }
  for (int ch = 0; ch < 3; ++ch) {
    const size_t off = static_cast<size_t>(ch) * SCREEN_W;
#if DSPERATE_NEON
    for (u32 x = 0; x < SCREEN_W; x += 8) {
      uint32x4_t lo = vdupq_n_u32(0), hi = vdupq_n_u32(0);
      for (u32 k = 0; k < ring_; ++k) {
        const uint16x8_t r = vld1q_u16(rows[k] + off + x);
        lo = vmlal_n_u16(lo, vget_low_u16(r), w_[k]);
        hi = vmlal_n_u16(hi, vget_high_u16(r), w_[k]);
      }
      vst1q_u16(&glow[ch][x], vcombine_u16(vshrn_n_u32(lo, 16), vshrn_n_u32(hi, 16)));
    }
#else
    for (u32 x = 0; x < SCREEN_W; ++x) {
      u32 a = 0;
      for (u32 k = 0; k < ring_; ++k) a += static_cast<u32>(w_[k]) * rows[k][off + x];
      glow[ch][x] = static_cast<u16>(a >> 16);
    }
#endif
  }
  for (u32 x = 0; x < SCREEN_W; ++x) {
    u32 px = 0xFF000000u;
    for (int ch = 0; ch < 3; ++ch) {
      const int sh = 16 - 8 * ch;
      const u32 v = (c[x] >> sh) & 255, g = glow[ch][x];
      // Screen blend: c + k g (255 - c) / (256 * 255), rounded; t / 65280 as (t + t / 256) / 65536.
      const u32 t = k_[ch] * g * (255 - v);
      px |= std::min<u32>(255, v + ((t + (t >> 8) + 32768) >> 16)) << sh;
    }
    out[x] = px;
  }
  if (t0) g_glow_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
}

void LineFilter::frame(const u32* src, u32* dst) {
  if (!on()) { if (dst != src) std::memcpy(dst, src, sizeof(u32) * SCREEN_W * SCREEN_H); return; }
  const u32 d = delay();
  for (u32 l = 0; l < SCREEN_H; ++l) {
    push(l, src + static_cast<size_t>(l) * SCREEN_W);
    if (l >= d) pull(l - d, dst + static_cast<size_t>(l - d) * SCREEN_W);
  }
  for (u32 l = SCREEN_H > d ? SCREEN_H - d : 0; l < SCREEN_H; ++l) pull(l, dst + static_cast<size_t>(l) * SCREEN_W);
}

void FilterChain::push(u32 line, const u32* src) {
  const bool a = a_ && a_->on(), b = b_ && b_->on();
  if (!a) { if (b) b_->push(line, src); return; }
  if (!b) { a_->push(line, src); return; }
  // The first's lines go on into the second as they come out of it.
  alignas(16) u32 mid[SCREEN_W];
  const u32 d = a_->delay();
  a_->push(line, src);
  if (line >= d) { a_->pull(line - d, mid); b_->push(line - d, mid); }
  if (line + 1 == SCREEN_H)
    for (u32 l = line + 1 > d ? line + 1 - d : 0; l < SCREEN_H; ++l) { a_->pull(l, mid); b_->push(l, mid); }
}

void FilterChain::pull(u32 line, u32* out) {
  const bool b = b_ && b_->on();
  if (b) b_->pull(line, out); else if (a_ && a_->on()) a_->pull(line, out);
}

// ---- timing ----------------------------------------------------------------

bool timing_on() {
  static const bool on = [] { const char* e = std::getenv("MINUI_SHADER_TIMING"); return e && !std::strcmp(e, "1"); }();
  return on;
}

void timing_add_scaling(u64 ns) { g_scaling_ns.fetch_add(ns, std::memory_order_relaxed); }
void timing_add_filter(u64 ns) { g_filter_ns.fetch_add(ns, std::memory_order_relaxed); }
void timing_add_curve(u64 ns) { g_curve_ns.fetch_add(ns, std::memory_order_relaxed); }
u64 timing_now() { return now_ns(); }

void timing_frame(u32 now_ms, Colour c, Scaling sc, Filter f, Pattern p, Glow g, Curve v) {
  if (!timing_on()) return;
  static u32 frames = 0, mark = 0;
  static bool started = false;
  if (!started) { started = true; mark = now_ms; }
  ++frames;
  if (now_ms - mark < 5000) return;
  const double colour_us = g_colour_ns.exchange(0) / 1000.0 / frames;
  const double pattern_us = g_pattern_ns.exchange(0) / 1000.0 / frames;
  const double glow_us = g_glow_ns.exchange(0) / 1000.0 / frames;
  const double scaling_us = g_scaling_ns.exchange(0) / 1000.0 / frames;
  const double filter_us = g_filter_ns.exchange(0) / 1000.0 / frames;
  const double curve_us = g_curve_ns.exchange(0) / 1000.0 / frames;
  std::fprintf(stderr, "shader timing: colour %s %.0f us/frame, scaling %s %.0f us/frame, filter %s %.0f us/frame, "
               "pattern %s %.0f us/frame, glow %s %.0f us/frame, curve %s %.0f us/frame\n",
               colour_name(c), colour_us, kScalings[static_cast<u8>(sc)].value, scaling_us, kFilters[static_cast<u8>(f)].value, filter_us,
               pattern_name(p), pattern_us, glow_name(g), glow_us, kCurves[static_cast<u8>(v)].value, curve_us);
  frames = 0;
  mark = now_ms;
}

} // namespace ds::sdl::shader
