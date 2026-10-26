// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak, copied into DSperate's tests/ by
// the Makefile and registered by patches/0006-shaders.patch.
//
// The pak's COLOUR and PATTERN shaders: that the tables hold the shaders'
// own maths, that the NEON kernel matches its reference (GCC 8 has
// miscompiled a NEON kernel here before, patch 0002), that the scaler draws
// them where the frontend says and leaves everything else alone, and that
// the rows step the way the menu and the shortcuts expect.
#include "frontend/sdl/minui_resample.h"
#include "frontend/sdl/minui_shader.h"
#include "frontend/sdl/settings.h"
#include "frontend/video/layout.h"
#include "core/nds.h"
#include "check.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" double ds_pow_compat(double x, double y);

using ds::u16;
using ds::u32;
using ds::u8;
using ds::SCREEN_H;
using ds::SCREEN_W;
namespace sh = ds::sdl::shader;
using Gpu = ds::gpu::Gpu;

namespace {

u32 expand5(u32 c5) { const u32 e6 = c5 << 1; return (e6 << 2) | (e6 >> 4); }   // a 2D colour, as the engines widen it
u32 expand6(u32 c6) { return (c6 << 2) | (c6 >> 4); }
u32 pixel(u32 r, u32 g, u32 b) { return 0xFF000000u | r << 16 | g << 8 | b; }
int byte_of(double v) { return static_cast<int>(std::lround(std::min(1.0, std::max(0.0, v)) * 255.0)); }

const ds::sdl::Setting* video_row(const char* key) {
  for (int i = 0; ds::sdl::kVideoSettings[i].key; ++i)
    if (!std::strcmp(ds::sdl::kVideoSettings[i].key, key)) return &ds::sdl::kVideoSettings[i];
  return nullptr;
}
const ds::sdl::Setting* shader_row(const char* key) {
  for (int i = 0; ds::sdl::kShaderSettings[i].key; ++i)
    if (!std::strcmp(ds::sdl::kShaderSettings[i].key, key)) return &ds::sdl::kShaderSettings[i];
  return nullptr;
}

// A test image: every 5-bit colour somewhere, in runs and in noise.
std::vector<u32> test_image(unsigned seed) {
  std::vector<u32> img(SCREEN_W * SCREEN_H);
  std::mt19937 rng(seed);
  for (u32 y = 0; y < SCREEN_H; ++y)
    for (u32 x = 0; x < SCREEN_W; ++x) {
      const u32 i = y * SCREEN_W + x;
      const u32 c = y < 128 ? i & 0x7FFF : rng() & 0x7FFF;
      img[i] = pixel(expand5(c >> 10 & 31), expand5(c >> 5 & 31), expand5(c & 31));
    }
  return img;
}

std::vector<u16> nearest_xrun(u32 w, u32 x0, u32 x1) {
  std::vector<u16> xr(SCREEN_W + 1);
  for (u32 x = 0; x <= SCREEN_W; ++x) xr[x] = static_cast<u16>(std::min(std::max((x * w + SCREEN_W - 1) / SCREEN_W, x0), x1) - x0);
  return xr;
}

// ---- tables ------------------------------------------------------------------

// Every name parses back to its own value, labels fit the menu's value column,
// an unknown name is off, and the rows use these lists.
void test_presets() {
  std::set<std::string> seen;
  for (u8 i = 0; i < sh::kColourCount; ++i) {
    sh::Colour c;
    CHECK(sh::parse_colour(sh::kColours[i].value, c) && static_cast<u8>(c) == i);
    CHECK(std::strcmp(sh::colour_name(c), sh::kColours[i].value) == 0);
    CHECK(std::strlen(sh::kColours[i].label) <= 12);
    CHECK(seen.insert(std::string("c:") + sh::kColours[i].value).second);
  }
  for (u8 i = 0; i < sh::kPatternCount; ++i) {
    sh::Pattern p;
    CHECK(sh::parse_pattern(sh::kPatterns[i].value, p) && static_cast<u8>(p) == i);
    CHECK(std::strcmp(sh::pattern_name(p), sh::kPatterns[i].value) == 0);
    CHECK(std::strlen(sh::kPatterns[i].label) <= 12);
    CHECK(seen.insert(std::string("p:") + sh::kPatterns[i].value).second);
  }
  sh::Colour c = sh::Colour::Gba;
  CHECK(!sh::parse_colour("sepia", c) && c == sh::Colour::Off);
  sh::Pattern p = sh::Pattern::Lcd;
  CHECK(!sh::parse_pattern("hq4x", p) && p == sh::Pattern::Off);
  // Visual FX: SHADER PRESET after BILINEAR, off by default.
  const ds::sdl::Setting* v = ds::sdl::kVideoSettings;
  int lin = -1;
  for (int i = 0; v[i].key; ++i) if (!std::strcmp(v[i].key, "video.linear")) lin = i;
  CHECK(lin >= 0);
  CHECK(!std::strcmp(v[lin + 1].key, "video.preset") && v[lin + 1].choices == sh::kPresets && v[lin + 1].nchoices == sh::kPresetCount);
  CHECK(!std::strcmp(v[lin + 1].def, "off"));
  for (int i = 0; v[i].key; ++i) CHECK(std::strncmp(v[i].key, "video.colour", 12) && std::strncmp(v[i].key, "video.glow", 10));
  // Shaders (Advanced): every row, over the lists above.
  const ds::sdl::Setting* t = ds::sdl::kShaderSettings;
  const struct { const char* key; const ds::sdl::Choice* c; u8 n; const char* def; ds::sdl::Dep dep; } rows[] = {
    {"video.colour", sh::kColours, sh::kColourCount, "off", ds::sdl::Dep::None},
    {"video.scaling", sh::kScalings, sh::kScalingCount, "off", ds::sdl::Dep::Scaling},
    {"video.filter", sh::kFilters, sh::kFilterCount, "off", ds::sdl::Dep::None},
    {"video.pattern", sh::kPatterns, sh::kPatternCount, "off", ds::sdl::Dep::Pattern},
    {"video.glow", sh::kGlows, sh::kGlowCount, "auto", ds::sdl::Dep::None},
    {"video.glow_strength", nullptr, 0, "auto", ds::sdl::Dep::Glow},
    {"video.glow_radius", nullptr, 0, "auto", ds::sdl::Dep::Glow},
    {"video.bloom_threshold", nullptr, 0, "auto", ds::sdl::Dep::Bloom},
    {"video.curve", sh::kCurves, sh::kCurveCount, "off", ds::sdl::Dep::Scaling},
  };
  CHECK(ds::sdl::settings_count(t) == static_cast<int>(sizeof rows / sizeof rows[0]));
  for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
    CHECK(!std::strcmp(t[i].key, rows[i].key) && !std::strcmp(t[i].def, rows[i].def) && t[i].depends == rows[i].dep);
    if (rows[i].c) CHECK(t[i].choices == rows[i].c && t[i].nchoices == rows[i].n);
  }
  CHECK(t[6].hi == sh::GLOW_MAX_RADIUS);
  // Every list's labels fit the value column, and its names are unique.
  for (const auto& l : {std::make_pair(sh::kScalings, sh::kScalingCount), std::make_pair(sh::kFilters, sh::kFilterCount),
                        std::make_pair(sh::kCurves, sh::kCurveCount), std::make_pair(sh::kPresets, sh::kPresetCount),
                        std::make_pair(sh::kGlows, sh::kGlowCount)}) {
    std::set<std::string> names;
    for (u8 i = 0; i < l.second; ++i) { CHECK(std::strlen(l.first[i].label) <= 12); CHECK(names.insert(l.first[i].value).second); }
  }
  sh::Scaling sc; sh::Filter fi; sh::Curve cu;
  CHECK(sh::parse_scaling("bicubic", sc) && sc == sh::Scaling::Bicubic && !sh::parse_scaling("hq4x", sc) && sc == sh::Scaling::Off);
  CHECK(sh::parse_filter("pixel_art_aa", fi) && fi == sh::Filter::PixelArtAa && !sh::parse_filter("blur", fi) && fi == sh::Filter::Off);
  CHECK(sh::parse_curve("barrel", cu) && cu == sh::Curve::Barrel && !sh::parse_curve("pincushion", cu) && cu == sh::Curve::Off);
}

// Some presets written out again here from their GLSL, independently of
// minui_shader.cpp, at a few colours.
void test_colour_against_glsl() {
  auto p = [](double x, double e) { return x <= 0.0 ? 0.0 : ds_pow_compat(x, e); };
  auto cl = [](double v) { return std::min(1.0, std::max(0.0, v)); };
  const double samples[][3] = {{0, 0, 0}, {1, 1, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.5, 0.25, 0.75}, {0.2, 0.6, 0.4}};
  for (const auto& s : samples) {
    double out[3];
    // warm_paper: mix(vec3(0.055,0.04,0.025), c, 0.94) * vec3(1.0,0.95,0.86)
    sh::colour_of(sh::Colour::WarmPaper, sh::Tone::None, s, out);
    const double wp[3] = {(0.055 * 0.06 + s[0] * 0.94) * 1.0, (0.04 * 0.06 + s[1] * 0.94) * 0.95, (0.025 * 0.06 + s[2] * 0.94) * 0.86};
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(out[k] - wp[k]) < 1e-9);
    // pocket_green: mix(ink, paper, dot(c, vec3(0.299,0.587,0.114)))
    sh::colour_of(sh::Colour::PocketGreen, sh::Tone::None, s, out);
    const double l = 0.299 * s[0] + 0.587 * s[1] + 0.114 * s[2];
    const double pg[3] = {0.10 + (0.77 - 0.10) * l, 0.19 + (0.84 - 0.19) * l, 0.15 + (0.57 - 0.15) * l};
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(out[k] - pg[k]) < 1e-9);
    // gba_color, its mat3 read column by column.
    sh::colour_of(sh::Colour::Gba, sh::Tone::None, s, out);
    double c[3];
    for (int k = 0; k < 3; ++k) c[k] = cl(p(s[k], 2.7) * 0.94);
    const double g[3] = {cl(p(0.82 * c[0] + 0.24 * c[1] - 0.06 * c[2], 1 / 2.2)),
                         cl(p(0.125 * c[0] + 0.665 * c[1] + 0.21 * c[2], 1 / 2.2)),
                         cl(p(0.195 * c[0] + 0.075 * c[1] + 0.73 * c[2], 1 / 2.2))};
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(out[k] - g[k]) < 1e-9);
    // nds-color: color * screen, its mat4 columns (r, rg, rb), (gr, g, gb), (br, bg, b).
    sh::colour_of(sh::Colour::Nds, sh::Tone::None, s, out);
    for (int k = 0; k < 3; ++k) c[k] = cl(p(s[k], 1.91) * 0.89);
    const double n[3] = {cl(p(0.87 * c[0] + 0.255 * c[1] - 0.125 * c[2], 1 / 1.91)),
                         cl(p(0.10 * c[0] + 0.645 * c[1] + 0.255 * c[2], 1 / 1.91)),
                         cl(p(0.10 * c[0] + 0.17 * c[1] + 0.73 * c[2], 1 / 1.91))};
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(out[k] - n[k]) < 1e-9);
    // zfast_lcd's gamma, and retro-v2's, on top of anything.
    sh::colour_of(sh::Colour::Off, sh::Tone::ZfastGamma, s, out);
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(out[k] - s[k] * (0.6 + 0.4 * s[k])) < 1e-9);
    sh::colour_of(sh::Colour::Off, sh::Tone::RetroV2, s, out);
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(out[k] - p(s[k], 2.4 / 2.2)) < 1e-9);
  }
  // Natural Vision leaves grey grey, and white white.
  const double grey[3] = {0.5, 0.5, 0.5}, white[3] = {1, 1, 1};
  double out[3];
  sh::colour_of(sh::Colour::NaturalVision, sh::Tone::None, grey, out);
  CHECK(std::fabs(out[0] - out[1]) < 1e-6 && std::fabs(out[1] - out[2]) < 1e-6);
  sh::colour_of(sh::Colour::NaturalVision, sh::Tone::None, white, out);
  for (double v : out) CHECK(std::fabs(v - 1.0) < 1e-6);
}

// The table holds every colour the DS can show (6 bits a channel, widened to
// 8 as the output stage does) exactly as the double-precision maths gives it.
void test_colour_table() {
  for (u8 i = 0; i < sh::kColourCount; ++i)
    for (sh::Tone tone : {sh::Tone::None, sh::Tone::ZfastGamma, sh::Tone::RetroV2}) {
      const sh::Colour c = static_cast<sh::Colour>(i);
      sh::ColourTable t;
      t.stage(c, tone);
      t.finish();
      if (c == sh::Colour::Off && tone == sh::Tone::None) { CHECK(t.lut() == nullptr); continue; }
      CHECK(t.lut() != nullptr && t.colour() == c && t.tone() == tone);
      int bad = 0;
      for (u32 k = 0; k < sh::COLOUR_ENTRIES; ++k) {
        const u32 r = expand6(k >> 12 & 63), g = expand6(k >> 6 & 63), b = expand6(k & 63);
        const double in[3] = {r / 255.0, g / 255.0, b / 255.0};
        double out[3];
        sh::colour_of(c, tone, in, out);
        bad += t.lut()[sh::colour_index(pixel(r, g, b))] != pixel(byte_of(out[0]), byte_of(out[1]), byte_of(out[2]));
      }
      CHECK(bad == 0);
    }
  // Every 6-bit colour has its own entry.
  std::set<u32> seen;
  for (u32 k = 0; k < sh::COLOUR_ENTRIES; ++k) seen.insert(sh::colour_index(pixel(expand6(k >> 12 & 63), expand6(k >> 6 & 63), expand6(k & 63))));
  CHECK(seen.size() == sh::COLOUR_ENTRIES && *seen.rbegin() == sh::COLOUR_ENTRIES - 1);
}

// stage() builds aside; nothing the scaler holds changes until commit().
void test_colour_double_buffer() {
  sh::ColourTable t;
  CHECK(!t.commit());
  t.stage(sh::Colour::Gba, sh::Tone::None);
  CHECK(t.lut() == nullptr);
  t.finish();
  const u32* first = t.lut();
  CHECK(first && t.colour() == sh::Colour::Gba);
  CHECK(!t.commit());   // nothing new
  const u32 white = first[sh::COLOUR_ENTRIES - 1];
  t.stage(sh::Colour::Amber, sh::Tone::None);
  // Built aside: until it is committed the live table is untouched.
  CHECK(t.lut() == first && first[sh::COLOUR_ENTRIES - 1] == white && t.colour() == sh::Colour::Gba);
  while (!t.commit()) std::this_thread::yield();
  CHECK(t.lut() != first && t.colour() == sh::Colour::Amber);
  // Two changes before a commit: the later one is what lands.
  t.stage(sh::Colour::Nds, sh::Tone::None);
  t.stage(sh::Colour::Vivid, sh::Tone::None);
  t.finish();
  CHECK(t.colour() == sh::Colour::Vivid);
  t.stage(sh::Colour::Off, sh::Tone::None);
  CHECK(t.lut() != nullptr);
  CHECK(t.commit());
  CHECK(t.lut() == nullptr);
  // apply() with nothing staged copies.
  const std::vector<u32> img = test_image(1);
  std::vector<u32> out(img.size());
  t.apply(img.data(), out.data(), img.size());
  CHECK(out == img);
}

// ---- patterns ----------------------------------------------------------------

// The NEON kernel against the reference: every width up to past a panel row,
// a destination off the 16-byte grid, and both ends of the multipliers.
void test_modulate_neon() {
  std::mt19937 rng(7);
  std::vector<u32> src(1400), a(1404), b(1404);
  std::vector<u8> mul(1400 * 4), add(1400 * 4), bright(1400 * 4);
  const u8 ramps[][2] = {{0, 255}, {191, 255}, {64, 65}, {200, 200}};
  u8 weights[4][256];
  for (int r = 0; r < 4; ++r) sh::spread_weights(ramps[r][0], ramps[r][1], weights[r]);
  for (int round = 0; round < 3; ++round) {
    for (u32& v : src) v = rng();
    for (size_t i = 0; i < mul.size(); ++i) {
      mul[i] = round == 0 ? static_cast<u8>(rng()) : round == 1 ? (i & 1 ? 255 : 0) : 128;
      add[i] = round == 0 ? static_cast<u8>(rng() & 63) : round == 1 ? (i & 2 ? 255 : 0) : 0;
      bright[i] = round == 0 ? static_cast<u8>(rng()) : round == 1 ? (i & 4 ? 0 : 255) : 128;
    }
    for (int spread = 0; spread <= 4; ++spread)
      for (u32 n = 1; n <= 1300; n += n < 40 ? 1 : 37) {
        const u8* br = spread ? bright.data() : nullptr;
        const u8* wt = spread ? weights[spread - 1] : nullptr;
        std::fill(a.begin(), a.end(), 0x5A5A5A5Au);
        std::fill(b.begin(), b.end(), 0x5A5A5A5Au);
        sh::ref::modulate_row(src.data(), mul.data(), br, wt, add.data(), n, a.data() + 1);
        sh::modulate_row(src.data(), mul.data(), br, wt, add.data(), n, b.data() + 1);
        CHECK(a == b);
        CHECK(b[0] == 0x5A5A5A5Au && b[n + 1] == 0x5A5A5A5Au);
      }
  }
  // 1.0 and nothing added is the identity, spread or not.
  for (u32& v : src) v = rng();
  std::fill(mul.begin(), mul.end(), 128);
  std::fill(bright.begin(), bright.end(), 128);
  std::fill(add.begin(), add.end(), 0);
  sh::modulate_row(src.data(), mul.data(), nullptr, nullptr, add.data(), 1300, b.data());
  CHECK(std::equal(src.begin(), src.begin() + 1300, b.begin()));
  sh::modulate_row(src.data(), mul.data(), bright.data(), weights[0], add.data(), 1300, b.data());
  CHECK(std::equal(src.begin(), src.begin() + 1300, b.begin()));
  // Black takes the dark row, white the bright one (all but exactly, at the
  // full ramp), and the vivid_lcd ramp leaves everything below 191 dark.
  std::fill(mul.begin(), mul.end(), 64);
  std::fill(bright.begin(), bright.end(), 192);
  for (const u32 c : {0xFF000000u, 0xFF101010u, 0xFFFFFFFFu, 0xFFBEBEBEu}) {
    const u32 grey = c & 255;
    for (const auto& r : {std::make_pair(0, 255), std::make_pair(191, 255)}) {
      u32 out;
      u8 wt[256];
      sh::spread_weights(static_cast<u8>(r.first), static_cast<u8>(r.second), wt);
      sh::modulate_row(&c, mul.data(), bright.data(), wt, add.data(), 1, &out);
      const double w = std::min(1.0, std::max(0.0, (static_cast<int>(grey) - r.first) / double(r.second - r.first)));
      const double m = 64 + 128 * (w * w * (3 - 2 * w));
      CHECK(std::abs(static_cast<int>(out & 255) - static_cast<int>(std::min(255.0, std::round(grey * m / 128)))) <= 2);
    }
  }
}

struct Size { u32 w, h; };
const Size kPanels[] = {{640, 480}, {720, 720}, {1024, 768}, {1280, 720}};

// Each view the layouts place gets rows that hold the pattern at the phase of
// the run each panel pixel is drawn in, with no more classes than a DS line's
// rows can make, every row in one, and nothing at all at 1x or below.
void test_pattern_tables() {
  for (const Size& panel : kPanels)
    for (ds::frontend::Mode mode : {ds::frontend::Mode::Single, ds::frontend::Mode::Vertical, ds::frontend::Mode::Pip}) {
      ds::frontend::Layout l;
      l.mode = mode;
      ds::frontend::View v[2];
      ds::frontend::place(l, static_cast<int>(panel.w), static_cast<int>(panel.h), v);
      for (const ds::frontend::View& view : v) {
        if (!view.shown) continue;
        const u32 w = static_cast<u32>(view.rect.w), h = static_cast<u32>(view.rect.h);
        for (u8 pi = 0; pi < sh::kPatternCount; ++pi) {
          const sh::Pattern p = static_cast<sh::Pattern>(pi);
          sh::PatternStore st;
          const bool built = sh::build_pattern(p, w, h, 0, w, st);
          if (!sh::pattern_is_table(p) || w <= SCREEN_W || h <= SCREEN_H) { CHECK(!built && !st.get()); continue; }
          CHECK(built && st.get() && st.rows.w == w);
          // A fixed count of lines over the view differs row by row; the rest repeat with the DS's rows.
          const bool by_row = p == sh::Pattern::ResScanlines || p == sh::Pattern::ResScanlinesAuto;
          CHECK(st.classes <= (by_row ? h : 2 * static_cast<u32>(std::ceil(h / 192.0)) + 1));
          const double ax = 256.0 / w, ay = 192.0 / h;
          for (u32 y = 0; y < h; ++y) {
            const u32 c = st.row_class[y];
            CHECK(c < st.classes);
            // The row's phase, from the line that draws it (emit_scaled's spans).
            const u32 L = y * SCREEN_H / h;
            const u32 y0 = (L * h + SCREEN_H - 1) / SCREEN_H, y1 = ((L + 1) * h + SCREEN_H - 1) / SCREEN_H;
            const double fy = (y - y0 + 0.5) / (y1 - y0);
            for (u32 x = 0; x < w; x += 7) {
              const u32 s = x * SCREEN_W / w, a = (s * w + 255) / 256, b = ((s + 1) * w + 255) / 256;
              double m[3], ad[3], br[3];
              const sh::Place at{x + 0.5, y + 0.5, static_cast<double>(w), static_cast<double>(h)};
              sh::pattern_at(p, (x - a + 0.5) / (b - a), fy, ax, ay, m, ad, br, &at);
              const u8* mb = &st.mul[(static_cast<size_t>(c) * w + x) * 4];
              const u8* ab = &st.add[(static_cast<size_t>(c) * w + x) * 4];
              u8 lo, hi;
              const bool spread = sh::pattern_spread(p, lo, hi);
              CHECK(spread == (st.rows.bright != nullptr));
              for (int ch = 0; ch < 3; ++ch) {
                CHECK(std::fabs(mb[2 - ch] / 128.0 - std::min(m[ch], 255.0 / 128)) <= 0.5 / 128 + 1e-9);
                CHECK(std::abs(ab[2 - ch] - byte_of(ad[ch])) == 0);
                if (spread) CHECK(std::fabs(st.bright[(static_cast<size_t>(c) * w + x) * 4 + 2 - ch] / 128.0 - std::min(br[ch], 255.0 / 128)) <= 0.5 / 128 + 1e-9);
                else CHECK(m[ch] == br[ch]);
              }
              CHECK(mb[3] == 128 && ab[3] == 0);
            }
          }
          // Plain classes are exactly the identity, bright rows included.
          for (u32 c = 0; c < st.classes; ++c) {
            bool ident = true;
            for (u32 i = 0; i < w * 4; ++i)
              ident = ident && st.mul[c * w * 4 + i] == 128 && st.add[c * w * 4 + i] == 0 && (st.bright.empty() || st.bright[c * w * 4 + i] == 128);
            CHECK(static_cast<bool>(st.plain[c]) == ident);
          }
        }
      }
    }
}

// The patterns the display engine draws as a black overlay only ever darken,
// the same in every channel, with nothing added; the rest do not, so they
// are refused there.
void test_mask_patterns() {
  for (u8 pi = 0; pi < sh::kPatternCount; ++pi) {
    const sh::Pattern p = static_cast<sh::Pattern>(pi);
    bool darken_only = sh::pattern_is_table(p);
    for (int j = 0; j < 32 && darken_only; ++j)
      for (int i = 0; i < 32 && darken_only; ++i)
        for (double scale : {1.25, 2.5, 4.0}) {
          double m[3], a[3];
          const sh::Place at{i + 0.5, j * 1.37 + 0.5, 32, 45};   // over a view too, for the ones placed by it
          sh::pattern_at(p, (i + 0.5) / 32, (j + 0.5) / 32, 1 / scale, 1 / scale, m, a, nullptr, &at);
          darken_only = darken_only && m[0] <= 1.0 + 1e-9 && m[0] == m[1] && m[1] == m[2] && a[0] == 0 && a[1] == 0 && a[2] == 0;
        }
    if (darken_only != sh::pattern_is_mask(p)) std::fprintf(stderr, "mask rule: %s\n", sh::pattern_name(p));
    CHECK(darken_only == sh::pattern_is_mask(p));
  }
  // On the overlay layer, black at 1 - mul of the same phase the scaler uses.
  const int pw = 640, ph = 480;
  std::vector<u32> stage(static_cast<size_t>(pw) * ph, 0x12345678u);
  for (sh::Pattern p : {sh::Pattern::Lcd, sh::Pattern::ZfastLcd, sh::Pattern::Scanlines, sh::Pattern::SoftGrid}) {
    sh::draw_mask(p, stage.data(), pw, 0, pw, 0, ph, 256, 192, 0);
    sh::PatternStore st;
    CHECK(sh::build_pattern(p, pw, ph, 0, pw, st));
    for (int y = 0; y < ph; y += 3)
      for (int x = 0; x < pw; x += 5) {
        const u32 px = stage[static_cast<size_t>(y) * pw + x];
        CHECK((px & 0x00FFFFFFu) == 0);
        const double mul = st.mul[(static_cast<size_t>(st.row_class[y]) * pw + x) * 4 + 1] / 128.0;
        CHECK(std::fabs((255 - static_cast<int>(px >> 24)) / 255.0 - mul) <= 1.0 / 255 + 0.5 / 128 + 1e-9);
      }
  }
  // Anything else draws nothing.
  std::fill(stage.begin(), stage.end(), 0u);
  sh::draw_mask(sh::Pattern::Beads, stage.data(), pw, 0, pw, 0, ph, 256, 192, 0);
  for (u32 px : stage) CHECK(px == 0);
}

// CRT is the pak's own: scanlines and a grille that tint each channel, so not
// a mask, and lifted so a pixel's light over its cell stays near the original.
void test_crt() {
  CHECK(!sh::pattern_is_mask(sh::Pattern::Crt) && sh::pattern_is_table(sh::Pattern::Crt));
  double sum[3] = {0, 0, 0};
  const int n = 64;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      double m[3], a[3];
      sh::pattern_at(sh::Pattern::Crt, (i + 0.5) / n, (j + 0.5) / n, 0.25, 0.25, m, a);
      for (int k = 0; k < 3; ++k) { sum[k] += m[k]; CHECK(a[k] == 0 && m[k] > 0 && m[k] < 255.0 / 128); }
    }
  for (double v : sum) CHECK(std::fabs(v / (n * n) - 1.0) < 0.1);
  // The bright beam is wider: brighter in the gap, and near the original over the pixel.
  double gap_dark[3], gap_bright[3], add[3], bsum[3] = {0, 0, 0};
  sh::pattern_at(sh::Pattern::Crt, 0.5, 0.02, 0.25, 0.25, gap_dark, add, gap_bright);
  for (int k = 0; k < 3; ++k) CHECK(gap_bright[k] > gap_dark[k] + 0.2);
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      double m[3], a[3], b[3];
      sh::pattern_at(sh::Pattern::Crt, (i + 0.5) / n, (j + 0.5) / n, 0.25, 0.25, m, a, b);
      for (int k = 0; k < 3; ++k) bsum[k] += b[k];
    }
  for (double v : bsum) CHECK(std::fabs(v / (n * n) - 1.0) < 0.1);
  // At 1x it is gone, as the RG DS patterns are.
  double m[3], a[3];
  sh::pattern_at(sh::Pattern::Crt, 0.0, 0.0, 1.0, 1.0, m, a);
  for (double v : m) CHECK(v == 1.0);
}

// ---- through the scaler --------------------------------------------------------

// The scaler with the hooks: nothing set is upstream's nearest to the byte;
// colour then pattern is the table looked up and the class row applied,
// rows cropped by an overscale included.
void test_scaler() {
  const std::vector<u32> img = test_image(3);
  const u32 W = 640, H = 480;
  for (bool crop : {false, true}) {
    const u32 y_lo = crop ? 60 : 0, y_hi = crop ? 420 : H;
    const std::vector<u16> xr = nearest_xrun(W, 0, W);
    sh::ColourTable colour;
    colour.stage(sh::Colour::DsClassic, sh::Tone::None);
    colour.finish();
    sh::PatternStore pat;   // crt: the spread's bright rows too
    CHECK(sh::build_pattern(sh::Pattern::Crt, W, H, 0, W, pat) && pat.rows.bright);
    for (int hooks = 0; hooks < 2; ++hooks) {
      ds::NDS n;
      std::vector<u32> out(W * (y_hi - y_lo), 0xDEADBEEF), other(W * H);
      Gpu::ScaleTarget t{out.data(), W, H, xr.data()};
      t.y_lo = y_lo; t.y_hi = y_hi;
      if (hooks) { t.colour_lut = colour.lut(); t.colour = sh::colour_line; t.pattern = pat.get(); t.modulate = sh::modulate_class; }
      n.gpu.set_scale_target(0, t);
      n.gpu.set_scale_target(1, Gpu::ScaleTarget{other.data(), W, H, xr.data()});
      n.gpu.scale_image(0, img.data());
      int bad = 0;
      for (u32 y = y_lo; y < y_hi; ++y) {
        const u32 L = y * SCREEN_H / H;
        for (u32 x = 0; x < W; ++x) {
          const u32 s = x * SCREEN_W / W;
          u32 want = img[L * SCREEN_W + s];
          if (hooks) {
            want = colour.lut()[sh::colour_index(want)];
            const size_t at = (static_cast<size_t>(pat.row_class[y]) * W + x) * 4;
            sh::ref::modulate_row(&want, &pat.mul[at], &pat.bright[at], pat.rows.weight, &pat.add[at], 1, &want);
          }
          if (out[(y - y_lo) * W + x] != want && ++bad < 4)
            std::fprintf(stderr, "hooks %d crop %d (%u,%u) = %08x, want %08x\n", hooks, crop, x, y, out[(y - y_lo) * W + x], want);
        }
      }
      CHECK(bad == 0);
    }
  }
}

// Colour comes before everything the scaler does: through bilinear and
// chunky, a coloured line is the line coloured first.
void test_colour_comes_first() {
  const std::vector<u32> img = test_image(5);
  sh::ColourTable colour;
  colour.stage(sh::Colour::Vivid, sh::Tone::None);
  colour.finish();
  std::vector<u32> pre(img.size());
  colour.apply(img.data(), pre.data(), img.size());
  const u32 W = 640, H = 480;
  const std::vector<u16> xr = nearest_xrun(W, 0, W);
  std::vector<u16> pairs = xr;   // chunky's: the even pixel's run widened over the odd one's (display.cpp)
  for (u32 x = 1; x < SCREEN_W; x += 2) pairs[x] = pairs[x + 1];
  std::vector<u16> lsx(W);
  std::vector<u8> lwx(W);
  for (u32 x = 0; x < W; ++x) { lsx[x] = static_cast<u16>(std::min<u32>(254, x * 255 / W)); lwx[x] = static_cast<u8>(x * 37); }
  for (int mode = 0; mode < 2; ++mode) {
    std::vector<u32> a(W * H), b(W * H), other(W * H);
    for (int pass = 0; pass < 2; ++pass) {
      ds::NDS n;
      Gpu::ScaleTarget t{pass ? b.data() : a.data(), W, H, mode ? pairs.data() : xr.data()};
      if (mode == 0) { t.bilinear = true; t.lin_sx = lsx.data(); t.lin_wx = lwx.data(); }
      else t.chunky = 2;
      if (pass) { t.colour_lut = colour.lut(); t.colour = sh::colour_line; }
      n.gpu.set_scale_target(0, t);
      n.gpu.set_scale_target(1, Gpu::ScaleTarget{other.data(), W, H, xr.data()});
      n.gpu.scale_image(0, pass ? img.data() : pre.data());
    }
    CHECK(a == b);
  }
}

// ---- glow ----------------------------------------------------------------------

// The rows resolve as the page says: auto is crt's own halation where crt is
// drawn and nothing elsewhere, auto values are the kind's, and numbers clamp.
void test_glow_spec() {
  using G = sh::Glow;
  CHECK(sh::glow_spec(G::Auto, "auto", "auto", "auto", false).kind == G::Off);
  const sh::GlowSpec crt = sh::glow_spec(G::Auto, "auto", "auto", "auto", true);
  CHECK(crt.kind == G::Halation && crt.strength == sh::glow_defaults(G::Halation, true).strength && crt.radius == 4);
  CHECK(sh::glow_spec(G::Off, "1", "8", "0.5", true).kind == G::Off);
  const sh::GlowSpec b = sh::glow_spec(G::Bloom, "0.4", "2", "0.8", false);
  CHECK(b.kind == G::Bloom && b.strength == 0.4 && b.radius == 2 && b.threshold == 0.8);
  const sh::GlowSpec c = sh::glow_spec(G::Glow, "3", "40", "x", false);
  CHECK(c.strength == 1.0 && c.radius == sh::GLOW_MAX_RADIUS);
  G g;
  CHECK(sh::parse_glow("halation", g) && g == G::Halation);
  CHECK(!sh::parse_glow("sparkle", g) && g == G::Auto);
  CHECK(sh::kGlowCount == static_cast<u8>(G::Count));
}

// Against the same maths in double precision: a Gaussian of sigma radius/2
// over the light (bloom's above its threshold), screen-blended back.
void test_glow_maths() {
  const std::vector<u32> img = test_image(13);
  for (sh::Glow kind : {sh::Glow::Glow, sh::Glow::Bloom, sh::Glow::Halation})
    for (int radius : {1, 3, 8}) {
      const sh::GlowSpec spec = sh::glow_spec(kind, "auto", std::to_string(radius), "auto", false);
      sh::GlowFilter f;
      f.configure(spec);
      CHECK(f.on() && f.delay() == static_cast<u32>(radius));
      std::vector<u32> out(img.size());
      f.frame(img.data(), out.data());
      const double sigma = std::max(0.5, radius / 2.0);
      std::vector<double> w(2 * radius + 1);
      double total = 0;
      for (int k = -radius; k <= radius; ++k) total += w[k + radius] = std::exp(-0.5 * k * k / (sigma * sigma));
      for (double& v : w) v /= total;
      const double thr = kind == sh::Glow::Bloom ? std::round(spec.threshold * 255) : 0;
      const double tint[3] = {1.0, kind == sh::Glow::Halation ? 0.6 : 1.0, kind == sh::Glow::Halation ? 0.35 : 1.0};
      int worst = 0;
      for (int ch = 0; ch < 3; ++ch) {
        const int sh_ = 16 - 8 * ch;
        auto light = [&](int x, int y) {
          x = std::min(255, std::max(0, x)); y = std::min(191, std::max(0, y));
          const double c = (img[y * 256 + x] >> sh_) & 255;
          return thr > 0 ? (c <= thr ? 0.0 : (c - thr) * 255 / (255 - thr)) : c;
        };
        for (int y = 0; y < 192; y += 5)
          for (int x = 0; x < 256; x += 3) {
            double g = 0;
            for (int j = -radius; j <= radius; ++j)
              for (int i = -radius; i <= radius; ++i) g += w[j + radius] * w[i + radius] * light(x + i, y + j);
            const double c = (img[y * 256 + x] >> sh_) & 255;
            const double want = c + spec.strength * tint[ch] * g * (255 - c) / 255;
            worst = std::max(worst, std::abs(static_cast<int>((out[y * 256 + x] >> sh_) & 255) - static_cast<int>(std::lround(want))));
          }
      }
      std::printf("minui_shader: glow %-8s radius %d worst %d/255 off the double-precision blur\n", sh::glow_name(kind), radius, worst);
      CHECK(worst <= 3);
    }
  // Bloom leaves a picture with nothing above its threshold alone.
  std::vector<u32> dim(img.size(), 0xFF404850u), out(img.size());
  sh::GlowFilter f;
  f.configure(sh::glow_spec(sh::Glow::Bloom, "1", "4", "0.5", false));
  f.frame(dim.data(), out.data());
  CHECK(out == dim);
  // And off is off.
  f.configure(sh::glow_spec(sh::Glow::Off, "1", "4", "0.5", false));
  CHECK(!f.on());
}

// The same frame through the scaler's held-back lines comes out as the
// whole-frame glow scaled, at every radius, with colour before it and a
// pattern after.
void test_glow_scaler() {
  const std::vector<u32> img = test_image(17);
  const u32 W = 640, H = 480;
  const std::vector<u16> xr = nearest_xrun(W, 0, W);
  sh::ColourTable colour;
  colour.stage(sh::Colour::Gba, sh::Tone::None);
  colour.finish();
  std::vector<u32> pre(img.size());
  colour.apply(img.data(), pre.data(), img.size());
  for (int radius = 1; radius <= sh::GLOW_MAX_RADIUS; ++radius) {
    sh::GlowFilter f, g;
    const sh::GlowSpec spec = sh::glow_spec(sh::Glow::Halation, "0.5", std::to_string(radius), "auto", false);
    f.configure(spec);
    g.configure(spec);
    std::vector<u32> glowed(img.size());
    g.frame(pre.data(), glowed.data());
    std::vector<u32> a(W * H), b(W * H), other(W * H);
    for (int pass = 0; pass < 2; ++pass) {
      ds::NDS n;
      Gpu::ScaleTarget t{pass ? b.data() : a.data(), W, H, xr.data()};
      if (pass) {
        t.colour_lut = colour.lut(); t.colour = sh::colour_line;
        t.filter_delay = f.delay(); t.filter_state = &f;
        t.filter_push = sh::GlowFilter::push_hook; t.filter_pull = sh::GlowFilter::pull_hook;
      }
      n.gpu.set_scale_target(0, t);
      n.gpu.set_scale_target(1, Gpu::ScaleTarget{other.data(), W, H, xr.data()});
      n.gpu.scale_image(0, pass ? img.data() : glowed.data());
    }
    CHECK(a == b);
  }
}

// ---- presets -------------------------------------------------------------------

// Every preset names real rows, sets all of them (GLOW's numbers back to
// auto), and matches its own rows and no row moved off them.
void test_preset_table() {
  CHECK(!std::strcmp(sh::kPresetTable[0].value, "custom"));
  CHECK(sh::find_preset("old_tv") && !sh::find_preset("hq4x"));
  for (u8 i = 1; i < sh::kPresetCount; ++i) {
    const sh::Preset& pr = sh::kPresetTable[i];
    CHECK(pr.colour < sh::Colour::Count && pr.scaling < sh::Scaling::Count && pr.filter < sh::Filter::Count &&
          pr.pattern < sh::Pattern::Count && pr.glow < sh::Glow::Count && pr.curve < sh::Curve::Count);
    const auto vals = sh::preset_values(pr);
    CHECK(vals.size() == 9);
    std::map<std::string, std::string> rows(vals.begin(), vals.end());
    auto get = [&](const char* k) { const auto it = rows.find(k); return it == rows.end() ? std::string() : it->second; };
    CHECK(sh::preset_matches(pr, get));
    rows["video.glow_strength"] = "0.5";
    CHECK(!sh::preset_matches(pr, get));
  }
  // An empty config is the off preset.
  CHECK(sh::preset_matches(*sh::find_preset("off"), [](const char*) { return std::string(); }));
  // NextUI's, as their .cfg files chain their shaders.
  const sh::Preset& gba = *sh::find_preset("real_gba");
  CHECK(gba.scaling == sh::Scaling::Pixellate && gba.pattern == sh::Pattern::Lcd3x);
  const sh::Preset& tv = *sh::find_preset("old_tv");
  CHECK(tv.curve == sh::Curve::Barrel && tv.pattern == sh::Pattern::ResScanlines);
  CHECK(sh::find_preset("nextui_scanlines")->pattern == sh::Pattern::ResScanlinesAuto);
  CHECK(sh::find_preset("pixelperfect_smooth")->scaling == sh::Scaling::Smooth3x);
}

// ---- resamplers ------------------------------------------------------------------

// Every resampler writes every row of its view exactly once per frame, and
// nothing outside it, at each panel size, cropped or not.
void test_resampler_rows() {
  const std::vector<u32> img = test_image(21);
  for (u8 k = 1; k < sh::kScalingCount; ++k) {
    const sh::Scaling kind = static_cast<sh::Scaling>(k);
    for (const Size& sz : {Size{640, 480}, Size{1024, 768}, Size{320, 240}, Size{200, 150}})
      for (bool crop : {false, true}) {
        auto rs = sh::make_resampler(kind);
        CHECK(rs && rs->kind() == kind);
        const u32 x0 = crop ? 7 : 0, x1 = sz.w - (crop ? 5 : 0);
        rs->build(sz.w, sz.h, x0, x1);
        const u32 n = x1 - x0, y_lo = crop ? 3 : 0, y_hi = sz.h - (crop ? 4 : 0);
        std::vector<u32> out(static_cast<size_t>(n) * (sz.h + 2), 0x00C0FFEEu);
        Gpu::ScaleTarget t{out.data() + n, n, sz.h};
        t.y_lo = y_lo; t.y_hi = y_hi;
        for (u32 l = 0; l < SCREEN_H; ++l) rs->run(t, l, &img[l * SCREEN_W]);
        int missing = 0, stray = 0;
        for (u32 y = 0; y < sz.h + 2; ++y)
          for (u32 x = 0; x < n; ++x) {
            const bool inside = y >= 1 && y - 1 < y_hi - y_lo;
            const bool untouched = out[static_cast<size_t>(y) * n + x] == 0x00C0FFEEu;
            missing += inside && untouched;
            stray += !inside && !untouched;
          }
        if (missing || stray) std::fprintf(stderr, "%s %ux%u crop %d: %d missing, %d stray\n", sh::kScalings[k].value, sz.w, sz.h, crop, missing, stray);
        CHECK(!missing && !stray);
      }
  }
}

std::vector<u32> resample(sh::Scaling kind, const std::vector<u32>& img, u32 w, u32 h) {
  auto rs = sh::make_resampler(kind);
  rs->build(w, h, 0, w);
  std::vector<u32> out(static_cast<size_t>(w) * h);
  rs->frame(img.data(), out.data(), w);
  return out;
}

// At a whole scale the sharp ones are nearest, exactly.
void test_resampler_nearest() {
  const std::vector<u32> img = test_image(23);
  struct { sh::Scaling k; u32 w, h; } cases[] = {
    {sh::Scaling::SharpBilinear, 512, 384}, {sh::Scaling::SharpBilinear, 1024, 768},
    {sh::Scaling::Pixellate, 512, 384}, {sh::Scaling::Pixellate, 1024, 768},
    {sh::Scaling::SharpShimmerless, 512, 384}, {sh::Scaling::SharpShimmerless, 768, 576},
    {sh::Scaling::Smooth3x, 768, 576},
  };
  for (const auto& c : cases) {
    const std::vector<u32> out = resample(c.k, img, c.w, c.h);
    int bad = 0;
    for (u32 y = 0; y < c.h; ++y)
      for (u32 x = 0; x < c.w; ++x) bad += out[y * c.w + x] != (img[(y * SCREEN_H / c.h) * SCREEN_W + x * SCREEN_W / c.w] | 0xFF000000u);
    if (bad) std::fprintf(stderr, "%s at %ux%u: %d off nearest\n", sh::kScalings[static_cast<u8>(c.k)].value, c.w, c.h, bad);
    CHECK(bad == 0);
  }
}

// The resamplers' own maths, at a few places worked out by hand.
void test_resampler_maths() {
  // Black and white columns.
  std::vector<u32> bars(SCREEN_W * SCREEN_H);
  for (u32 y = 0; y < SCREEN_H; ++y) for (u32 x = 0; x < SCREEN_W; ++x) bars[y * SCREEN_W + x] = x & 1 ? 0xFFFFFFFFu : 0xFF000000u;
  // bicubic, at a DS pixel's centre (3x: panel x = 3k + 1): Mitchell's 1/18, 16/18, 1/18.
  {
    const std::vector<u32> out = resample(sh::Scaling::Bicubic, bars, 768, 576);
    const double want_black = 255.0 * 2 * 0.3333 / 6, want_white = 255.0 * (6 - 2 * 0.3333) / 6;
    CHECK(std::abs(static_cast<int>(out[100 * 768 + 3 * 10 + 1] & 255) - static_cast<int>(std::lround(want_black))) <= 1);
    CHECK(std::abs(static_cast<int>(out[100 * 768 + 3 * 11 + 1] & 255) - static_cast<int>(std::lround(want_white))) <= 1);
  }
  // pixellate blends in linear light: half black, half white is 255 * 0.5^(1/2.2), not 128.
  {
    const u32 w = 640;   // 2.5x: column 2 straddles DS pixels 0 and 1 half and half
    const std::vector<u32> out = resample(sh::Scaling::Pixellate, bars, w, 480);
    CHECK(std::abs(static_cast<int>(out[100 * w + 2] & 255) - static_cast<int>(std::lround(255 * std::pow(0.5, 1 / 2.2)))) <= 1);
  }
  // sharp-shimmerless-subpixel-vrgb: across a black-to-white edge down the
  // picture, red is sampled a third of a panel pixel higher than blue.
  {
    std::vector<u32> half(SCREEN_W * SCREEN_H);
    for (u32 y = 0; y < SCREEN_H; ++y) for (u32 x = 0; x < SCREEN_W; ++x) half[y * SCREEN_W + x] = y < 96 ? 0xFF000000u : 0xFFFFFFFFu;
    const u32 w = 640, h = 480;   // DS line 96 starts at panel row 240
    const std::vector<u32> out = resample(sh::Scaling::ShimmerlessVrgb, half, w, h);
    const u32 px = out[239 * w + 100];
    CHECK(((px >> 16) & 255) < (px & 255));   // red still in black, blue already in white
  }
  // scale3x: the top-left third of a pixel whose top and left match, and differ from it, takes their colour.
  {
    std::vector<u32> img(SCREEN_W * SCREEN_H, 0xFF000000u);
    img[10 * SCREEN_W + 9] = img[9 * SCREEN_W + 10] = 0xFFFF0000u;   // D and B red, E black
    const std::vector<u32> out = resample(sh::Scaling::Scale3x, img, 768, 576);
    CHECK(out[30 * 768 + 30] == 0xFFFF0000u);   // E0
    CHECK(out[31 * 768 + 31] == 0xFF000000u);   // E itself
  }
  // aa-shader-4.0 leaves a flat picture flat; waterpaint takes it to 0.78 of itself.
  {
    std::vector<u32> flat(SCREEN_W * SCREEN_H, 0xFF808080u);
    for (const u32 px : resample(sh::Scaling::Aa4, flat, 640, 480)) CHECK(std::abs(static_cast<int>(px & 255) - 128) <= 1);
    for (const u32 px : resample(sh::Scaling::Waterpaint, flat, 640, 480)) CHECK(std::abs(static_cast<int>(px & 255) - static_cast<int>(std::lround(0.78 * 128))) <= 1);
  }
  // edge1pixel at 2.5x: inside a DS pixel nearest, on its edge the two halves.
  {
    const std::vector<u32> out = resample(sh::Scaling::Edge1Pixel, bars, 640, 480);
    CHECK((out[100 * 640 + 1] & 255) == 0);                  // DS pixel 0, in the middle
    CHECK(std::abs(static_cast<int>(out[100 * 640 + 2] & 255) - 128) <= 1);   // between 0 and 1
  }
}

// Through the scaler's hook, a frame is the resampler's own frame, crop and
// pattern included, with colour and the filter chain before it.
void test_resampler_scaler() {
  const std::vector<u32> img = test_image(29);
  const u32 W = 640, H = 480;
  const std::vector<u16> xr = nearest_xrun(W, 0, W);
  for (sh::Scaling kind : {sh::Scaling::Pixellate, sh::Scaling::Bicubic, sh::Scaling::Scale3x, sh::Scaling::Aa4}) {
    auto a = sh::make_resampler(kind), b = sh::make_resampler(kind);
    a->build(W, H, 0, W); b->build(W, H, 0, W);
    sh::PatternStore pat;
    CHECK(sh::build_pattern(sh::Pattern::Lcd3x, W, H, 0, W, pat));
    std::vector<u32> want(W * H), got(W * H), other(W * H);
    b->frame(img.data(), want.data(), W, pat.get());
    ds::NDS n;
    Gpu::ScaleTarget t{got.data(), W, H, xr.data()};
    t.resample = sh::Resampler::hook; t.resample_state = a.get();
    t.pattern = pat.get(); t.modulate = sh::modulate_class;
    n.gpu.set_scale_target(0, t);
    n.gpu.set_scale_target(1, Gpu::ScaleTarget{other.data(), W, H, xr.data()});
    n.gpu.scale_image(0, img.data());
    CHECK(got == want);
  }
}

// ---- filters ---------------------------------------------------------------------

void test_filters() {
  // Both leave a flat picture alone.
  std::vector<u32> flat(SCREEN_W * SCREEN_H, 0xFF4080C0u), out(flat.size());
  for (sh::Filter f : {sh::Filter::FastSharpen, sh::Filter::PixelArtAa}) {
    sh::NeighbourFilter nf;
    nf.configure(f);
    CHECK(nf.on() && nf.delay() == 1);
    nf.frame(flat.data(), out.data());
    CHECK(out == flat);
  }
  // fast-sharpen pushes an edge apart, within its own bounds.
  std::vector<u32> edge(SCREEN_W * SCREEN_H);
  for (u32 y = 0; y < SCREEN_H; ++y) for (u32 x = 0; x < SCREEN_W; ++x) edge[y * SCREEN_W + x] = x < 128 ? 0xFF404040u : 0xFFC0C0C0u;
  sh::NeighbourFilter sharp;
  sharp.configure(sh::Filter::FastSharpen);
  sharp.frame(edge.data(), out.data());
  CHECK((out[50 * SCREEN_W + 127] & 255) < 0x40 && (out[50 * SCREEN_W + 128] & 255) > 0xC0);
  CHECK((out[50 * SCREEN_W + 60] & 255) == 0x40);
  // pixel_art_AA: the step in a staircase is averaged with the darker side it meets.
  std::vector<u32> stairs(SCREEN_W * SCREEN_H, 0xFFFFFFFFu);
  for (u32 y = 0; y < SCREEN_H; ++y) for (u32 x = 0; x < SCREEN_W; ++x) if (x > y) stairs[y * SCREEN_W + x] = 0xFF000000u;
  sh::NeighbourFilter aa;
  aa.configure(sh::Filter::PixelArtAa);
  aa.frame(stairs.data(), out.data());
  int changed = 0;
  for (size_t i = 0; i < out.size(); ++i) changed += out[i] != stairs[i];
  CHECK(changed > 0);
  // FILTER then GLOW through the scaler is the chain's own frame.
  sh::NeighbourFilter f1, f2;
  sh::GlowFilter g1, g2;
  f1.configure(sh::Filter::FastSharpen); f2.configure(sh::Filter::FastSharpen);
  const sh::GlowSpec spec = sh::glow_spec(sh::Glow::Bloom, "0.5", "3", "0.5", false);
  g1.configure(spec); g2.configure(spec);
  sh::FilterChain c1, c2;
  c1.set(&f1, &g1); c2.set(&f2, &g2);
  CHECK(c1.delay() == 4);
  const std::vector<u32> img = test_image(31);
  std::vector<u32> filtered(img.size());
  c2.frame(img.data(), filtered.data());
  const u32 W = 512, H = 384;
  const std::vector<u16> xr = nearest_xrun(W, 0, W);
  std::vector<u32> a(W * H), b(W * H), other(W * H);
  for (int pass = 0; pass < 2; ++pass) {
    ds::NDS n;
    Gpu::ScaleTarget t{pass ? b.data() : a.data(), W, H, xr.data()};
    if (pass) {
      t.filter_delay = c1.delay(); t.filter_state = static_cast<sh::LineFilter*>(&c1);
      t.filter_push = sh::LineFilter::push_hook; t.filter_pull = sh::LineFilter::pull_hook;
    }
    n.gpu.set_scale_target(0, t);
    n.gpu.set_scale_target(1, Gpu::ScaleTarget{other.data(), W, H, xr.data()});
    n.gpu.scale_image(0, pass ? img.data() : filtered.data());
  }
  CHECK(a == b);
}

// NextUI's patterns, at their constants: lcd3x's blue against red and green,
// scanline's ripple flipping with every panel column, and res-independent
// scanlines' 224 lines over the view.
void test_nextui_patterns() {
  double m[3], a[3];
  sh::pattern_at(sh::Pattern::Lcd3x, 0.25, 0.25, 0.25, 0.25, m, a);
  CHECK(std::fabs(m[0] - (7.0 / 7.0) * (25.0 / 25.0)) < 1e-9 && std::fabs(m[2] - (23.0 / 25.0)) < 1e-9);
  const sh::Place even{0.5, 10.5, 640, 480}, odd{1.5, 10.5, 640, 480};
  double me[3], mo[3];
  sh::pattern_at(sh::Pattern::Scanline, 0.5, 0.5, 0.4, 0.4, me, a, nullptr, &even);
  sh::pattern_at(sh::Pattern::Scanline, 0.5, 0.5, 0.4, 0.4, mo, a, nullptr, &odd);
  CHECK(std::fabs(me[0] - mo[0] - 0.1) < 1e-9);
  // |1.25 sin(gl_FragCoord.y / OutputSize.y * pi * imageSize + 0.5)|, counted up from the bottom.
  for (u32 y : {0u, 7u, 239u, 479u})
    for (const auto& lines : {std::make_pair(sh::Pattern::ResScanlines, 224.0), std::make_pair(sh::Pattern::ResScanlinesAuto, 192.0)}) {
      const sh::Place at{0.5, y + 0.5, 640, 480};
      sh::pattern_at(lines.first, 0.5, 0.5, 0.4, 0.4, m, a, nullptr, &at);
      const double frag_y = 480 - (y + 0.5);
      CHECK(std::fabs(m[0] - std::fabs(1.25 * std::sin(frag_y / 480 * 3.141592654 * lines.second + 0.5))) < 1e-6);
    }
  CHECK(sh::pattern_tone(sh::Pattern::RetroV2) == sh::Tone::RetroV2 && sh::pattern_tone(sh::Pattern::ZfastLcd) == sh::Tone::ZfastGamma);
}

// ---- stepping ------------------------------------------------------------------

// Enough of a host for step_value: the display engine's rule for PATTERN.
struct StepHost final : ds::sdl::SettingsHost {
  bool disp = false;
  std::string get(const char*) const override { return ""; }
  void set(const char*, const std::string&) override {}
  bool enabled(const ds::sdl::Setting&) const override { return true; }
  const char* disabled_reason(const ds::sdl::Setting&) const override { return ""; }
  bool value_allowed(const ds::sdl::Setting& s, const char* v) const override {
    if (!std::strcmp(s.key, "video.preset")) return std::strcmp(v, "custom") != 0;
    if (!disp || std::strcmp(s.key, "video.pattern") != 0) return true;
    sh::Pattern p;
    return sh::parse_pattern(v, p) && (p == sh::Pattern::Off || sh::pattern_is_mask(p));
  }
  void commit() override {}
  bool save_per_game() const override { return false; }
  void set_save_per_game(bool) override {}
  bool has_game() const override { return false; }
  int binding_count(bool) const override { return 0; }
  Binding binding(bool, int) const override { return {}; }
  bool has_pad() const override { return false; }
  void begin_capture(bool) override {}
  void cancel_capture() override {}
  bool capturing() const override { return false; }
  std::string take_capture() override { return ""; }
  void bind(const std::string&, const std::string&) override {}
  void reset_bindings(bool) override {}
  std::vector<std::string> collisions() const override { return {}; }
  const char* user_settings_note() const override { return nullptr; }
};

// Forward goes round every value and back to off; back goes the other way.
// On the display engine only the masks come round.
void test_stepping() {
  StepHost h;
  for (const char* key : {"video.colour", "video.pattern", "video.scaling", "video.filter", "video.curve"}) {
    const ds::sdl::Setting* row = shader_row(key);
    CHECK(row != nullptr);
    std::string v = "off";
    for (int i = 1; i < row->nchoices; ++i) { v = ds::sdl::step_value(*row, v, 1, h); CHECK(v == row->choices[i].value); }
    CHECK(ds::sdl::step_value(*row, v, 1, h) == "off");
    CHECK(ds::sdl::step_value(*row, "off", -1, h) == row->choices[row->nchoices - 1].value);
  }
  // SHADER PRESET never lands on custom, either way round.
  const ds::sdl::Setting* preset = video_row("video.preset");
  std::string at = "custom";
  for (int i = 0; i < 2 * preset->nchoices; ++i) { at = ds::sdl::step_value(*preset, at, i & 1 ? -1 : 1, h); CHECK(at != "custom"); }
  at = "off";
  for (int i = 0; i < preset->nchoices; ++i) { at = ds::sdl::step_value(*preset, at, 1, h); CHECK(at != "custom"); }
  h.disp = true;
  const ds::sdl::Setting* row = shader_row("video.pattern");
  std::string v = "off";
  std::set<std::string> seen;
  for (int i = 0; i < 2 * row->nchoices; ++i) {
    v = ds::sdl::step_value(*row, v, 1, h);
    sh::Pattern p;
    CHECK(sh::parse_pattern(v, p) && (p == sh::Pattern::Off || sh::pattern_is_mask(p)));
    seen.insert(v);
  }
  CHECK(seen.size() == 5);
}

// ---- timing --------------------------------------------------------------------

// What each costs per frame here. Not a device: for comparing presets only.
void report_timing() {
  using clk = std::chrono::steady_clock;
  const std::vector<u32> img = test_image(11);
  std::vector<u32> line(SCREEN_W);
  for (u8 i = 1; i < sh::kColourCount; ++i) {
    sh::ColourTable t;
    const auto b0 = clk::now();
    t.stage(static_cast<sh::Colour>(i), sh::Tone::None);
    t.finish();
    const auto b1 = clk::now();
    for (int rep = 0; rep < 10; ++rep)
      for (u32 y = 0; y < 2 * SCREEN_H; ++y) sh::colour_line(t.lut(), &img[(y % SCREEN_H) * SCREEN_W], line.data());
    const auto b2 = clk::now();
    std::printf("minui_shader: colour %-18s build %6.2f ms, %6.1f us/frame\n", sh::colour_name(static_cast<sh::Colour>(i)),
                std::chrono::duration<double, std::milli>(b1 - b0).count(), std::chrono::duration<double, std::micro>(b2 - b1).count() / 10);
  }
  for (sh::Glow kind : {sh::Glow::Glow, sh::Glow::Bloom, sh::Glow::Halation})
    for (int radius : {2, 4, 8}) {
      sh::GlowFilter f;
      f.configure(sh::glow_spec(kind, "auto", std::to_string(radius), "auto", false));
      std::vector<u32> out(img.size());
      const auto g0 = clk::now();
      for (int rep = 0; rep < 10; ++rep) { f.frame(img.data(), out.data()); f.frame(img.data(), out.data()); }
      std::printf("minui_shader: glow %-8s radius %d %6.1f us/frame (both screens)\n", sh::glow_name(kind), radius,
                  std::chrono::duration<double, std::micro>(clk::now() - g0).count() / 10);
    }
  for (u8 k = 1; k < sh::kScalingCount; ++k)
    for (const Size& sz : {Size{640, 480}, Size{1024, 768}}) {
      auto rs = sh::make_resampler(static_cast<sh::Scaling>(k));
      rs->build(sz.w, sz.h, 0, sz.w);
      std::vector<u32> out(static_cast<size_t>(sz.w) * sz.h);
      const auto r0 = clk::now();
      for (int rep = 0; rep < 3; ++rep) rs->frame(img.data(), out.data(), sz.w);
      std::printf("minui_shader: scaling %-18s %4ux%-4u %8.1f us/frame (one screen)\n", sh::kScalings[k].value, sz.w, sz.h,
                  std::chrono::duration<double, std::micro>(clk::now() - r0).count() / 3);
    }
  for (u8 k = 1; k < sh::kFilterCount; ++k) {
    sh::NeighbourFilter f;
    f.configure(static_cast<sh::Filter>(k));
    std::vector<u32> out(img.size());
    const auto f0 = clk::now();
    for (int rep = 0; rep < 5; ++rep) { f.frame(img.data(), out.data()); f.frame(img.data(), out.data()); }
    std::printf("minui_shader: filter %-14s %8.1f us/frame (both screens)\n", sh::kFilters[k].value,
                std::chrono::duration<double, std::micro>(clk::now() - f0).count() / 5);
  }
  for (const Size& sz : {Size{640, 480}, Size{1024, 768}}) {
    std::vector<u32> map, src(static_cast<size_t>(sz.w) * sz.h), out(sz.w);
    sh::build_curve_map(sh::Curve::Barrel, sz.w, sz.h, 0, sz.w, 0, sz.h, map);
    const auto c0 = clk::now();
    for (int rep = 0; rep < 5; ++rep)
      for (u32 y = 0; y < sz.h; ++y) sh::curve_row(map.data() + static_cast<size_t>(y) * sz.w, src.data(), sz.w, out.data());
    std::printf("minui_shader: curve barrel %4ux%-4u %8.1f us/frame (one screen)\n", sz.w, sz.h,
                std::chrono::duration<double, std::micro>(clk::now() - c0).count() / 5);
  }
  for (const Size& s : kPanels) {
    std::vector<u32> row(s.w), out(s.w);
    for (u8 i = 2; i < sh::kPatternCount; ++i) {
      sh::PatternStore st;
      sh::build_pattern(static_cast<sh::Pattern>(i), s.w, s.h, 0, s.w, st);
      const auto b0 = clk::now();
      for (int rep = 0; rep < 10; ++rep)
        for (u32 y = 0; y < s.h; ++y) {
          const u32 c = st.row_class[y];
          if (!st.plain[c]) sh::modulate_class(st.rows, c, row.data(), out.data());
        }
      const auto b1 = clk::now();
      std::printf("minui_shader: pattern %-14s %4ux%-4u %u classes, %6.1f us/frame (one screen, every row)\n",
                  sh::pattern_name(static_cast<sh::Pattern>(i)), s.w, s.h, st.classes,
                  std::chrono::duration<double, std::micro>(b1 - b0).count() / 10);
    }
  }
}

} // namespace

// CURVE: barrel-distortion.glsl's map, nearest: the centre stays, the
// corners go black, every pixel kept is in the window, and a cropped window
// maps only into itself.
void test_curve_map() {
  std::vector<u32> map;
  sh::build_curve_map(sh::Curve::Off, 512, 384, 0, 512, 0, 384, map);
  CHECK(map.empty());
  for (u32 w : {256u, 512u, 640u, 1024u}) {
    const u32 h = w * 3 / 4;
    sh::build_curve_map(sh::Curve::Barrel, w, h, 0, w, 0, h, map);
    CHECK(map.size() == static_cast<size_t>(w) * h);
    CHECK(map[0] == sh::kCurveBlack && map[w - 1] == sh::kCurveBlack && map[static_cast<size_t>(h - 1) * w] == sh::kCurveBlack);
    const u32 c = map[static_cast<size_t>(h / 2) * w + w / 2];
    CHECK(c != sh::kCurveBlack && std::abs(static_cast<int>(c % w) - static_cast<int>(w / 2)) <= 1 && std::abs(static_cast<int>(c / w) - static_cast<int>(h / 2)) <= 1);
    size_t black = 0;
    for (u32 y = 0; y < h; ++y)
      for (u32 x = 0; x < w; ++x) {
        const u32 m = map[static_cast<size_t>(y) * w + x];
        if (m == sh::kCurveBlack) { ++black; continue; }
        CHECK(m < w * h);
        // The shader's own maths, in floats.
        const double tx = (x + 0.5) / w - 0.5, ty = (y + 0.5) / h - 0.5, r = tx * tx + ty * ty;
        const double ux = (tx + tx * 0.25 * r) * 0.9375 + 0.5, uy = (ty + ty * 0.25 * r) * 0.9375 + 0.5;
        CHECK(std::fabs((m % w + 0.5) / w - ux) <= 1.0 / w && std::fabs((m / w + 0.5) / h - uy) <= 1.0 / h);
      }
    // The rescale leaves the edges' midpoints on screen and the corners off it.
    CHECK(black > 0 && black < static_cast<size_t>(w) * h / 10);
    CHECK(map[static_cast<size_t>(h / 2) * w] != sh::kCurveBlack && map[w / 2] != sh::kCurveBlack);
  }
  // A window cropped out of a larger view (integer overscale).
  sh::build_curve_map(sh::Curve::Barrel, 1024, 768, 100, 900, 50, 700, map);
  CHECK(map.size() == 800u * 650u);
  for (u32 m : map) CHECK(m == sh::kCurveBlack || m < 800u * 650u);
  std::vector<u32> src(800u * 650u), row(800);
  for (size_t i = 0; i < src.size(); ++i) src[i] = 0xFF000000u | static_cast<u32>(i);
  sh::curve_row(map.data() + 325u * 800u, src.data(), 800, row.data());
  for (u32 x = 0; x < 800; ++x) {
    const u32 m = map[325u * 800u + x];
    CHECK(row[x] == (m == sh::kCurveBlack ? 0xFF000000u : src[m]));
  }
}

int main() {
  test_presets();
  test_colour_against_glsl();
  test_colour_table();
  test_colour_double_buffer();
  test_modulate_neon();
  test_pattern_tables();
  test_mask_patterns();
  test_crt();
  test_scaler();
  test_colour_comes_first();
  test_glow_spec();
  test_glow_maths();
  test_glow_scaler();
  test_preset_table();
  test_resampler_rows();
  test_resampler_nearest();
  test_resampler_maths();
  test_resampler_scaler();
  test_filters();
  test_nextui_patterns();
  test_stepping();
  test_curve_map();
  report_timing();
  std::puts("minui_shader: ok");
  return 0;
}
