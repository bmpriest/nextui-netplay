// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. Compiled into DSperate (GPLv3) by
// patches/0006-shaders.patch.
//
// The shader rows: SHADER PRESET on the Visual FX page, and COLOUR,
// SCALING, FILTER, PATTERN, GLOW and CURVE on its Shaders (Advanced) page.
// The names the ini takes and the labels the menu shows, in one place so
// settings.cpp, the frontend and the tests agree. Data only, so including it
// adds no link input. What each one does is minui_shader.cpp's and
// minui_resample.cpp's business; README.md lists where each comes from.
#pragma once
#include "frontend/sdl/settings.h"

namespace ds::sdl::shader {

// In the order of the arrays below.
enum class Colour : u8 {
  Off, Nds, NaturalVision, NdsNaturalVision, DsClassic, DsLite, Natural, Vivid, Bold, Gba,
  WarmPaper, CoolCrisp, PocketGreen, Amber, Count
};
// How the picture is resampled to the panel: nearest, or one of these.
enum class Scaling : u8 {
  Off, SharpBilinear, Pixellate, SharpShimmerless, ShimmerlessGrid, ShimmerlessVrgb, Edge1Pixel,
  Smooth3x, Bicubic, Aa4, Waterpaint, Scale3x, Count
};
// A filter over each DS pixel and its neighbours, at the DS's resolution.
enum class Filter : u8 { Off, FastSharpen, PixelArtAa, Count };
enum class Pattern : u8 {
  Off, Lcd, ZfastLcd, Scanlines, SoftGrid, DsClassic, DsLite, Gba, RgbStripes, Beads, Tiles, Crt,
  Lcd3x, Scanline, ResScanlines, ResScanlinesAuto, RetroV2, Count
};
// GLOW: auto is the crt pattern's own halation when that pattern is drawn, else off.
enum class Glow : u8 { Auto, Off, Glow, Bloom, Halation, Count };
// The screen's shape: flat, or curved as NextUI's barrel-distortion curves it.
enum class Curve : u8 { Off, Barrel, Count };

inline constexpr Choice kColours[] = {
  {"off", "OFF"},
  {"nds", "NDS"},
  {"natural_vision", "NAT. VISION"},
  {"nds_natural_vision", "NDS+NAT. VIS"},
  {"ds_classic", "DS CLASSIC"},
  {"ds_lite", "DS LITE"},
  {"natural", "NATURAL"},
  {"vivid", "VIVID"},
  {"bold", "BOLD"},
  {"gba", "GBA"},
  {"warm_paper", "WARM PAPER"},
  {"cool_crisp", "COOL CRISP"},
  {"pocket_green", "POCKET GREEN"},
  {"amber", "AMBER"},
};
inline constexpr Choice kScalings[] = {
  {"off", "NEAREST"},
  {"sharp_bilinear", "SHARP BILIN."},
  {"pixellate", "PIXELLATE"},
  {"sharp_shimmerless", "SHIMMERLESS"},
  {"shimmerless_grid", "SHIMMER GRID"},
  {"shimmerless_vrgb", "SHIMMER VRGB"},
  {"edge1pixel", "EDGE 1 PIXEL"},
  {"smooth_3x", "SMOOTH 3X"},
  {"bicubic", "BICUBIC"},
  {"aa_4", "AA 4.0"},
  {"waterpaint", "WATERPAINT"},
  {"scale3x", "SCALE3X"},
};
inline constexpr Choice kFilters[] = {
  {"off", "OFF"},
  {"fast_sharpen", "FAST SHARPEN"},
  {"pixel_art_aa", "PIXEL ART AA"},
};
inline constexpr Choice kPatterns[] = {
  {"off", "OFF"},
  {"lcd", "LCD"},
  {"zfast_lcd", "ZFAST LCD"},
  {"scanlines", "SCANLINES"},
  {"soft_grid", "SOFT GRID"},
  {"ds_classic", "DS CLASSIC"},
  {"ds_lite", "DS LITE"},
  {"gba", "GBA"},
  {"rgb_stripes", "RGB STRIPES"},
  {"beads", "BEADS"},
  {"tiles", "TILES"},
  {"crt", "CRT"},
  {"lcd3x", "LCD3X"},
  {"scanline", "SINE LINES"},
  {"res_scanlines", "LINES 224"},
  {"res_scanlines_auto", "LINES AUTO"},
  {"retro_v2", "RETRO V2"},
};
inline constexpr Choice kGlows[] = {
  {"auto", "AUTO"},
  {"off", "OFF"},
  {"glow", "GLOW"},
  {"bloom", "BLOOM"},
  {"halation", "HALATION"},
};
inline constexpr Choice kCurves[] = {
  {"off", "FLAT"},
  {"barrel", "BARREL"},
};

// SHADER PRESET: every shader row at once. NextUI's six, rgds-core's named
// ones, and this pak's crt. "custom" is what the row says once a row no
// longer matches the preset; it is shown, never picked.
struct Preset {
  const char* value;
  const char* label;
  Colour colour;
  Scaling scaling;
  Filter filter;
  Pattern pattern;
  Glow glow;
  Curve curve;
};
inline constexpr Preset kPresetTable[] = {
  {"custom", "CUSTOM", Colour::Off, Scaling::Off, Filter::Off, Pattern::Off, Glow::Auto, Curve::Off},
  {"off", "OFF", Colour::Off, Scaling::Off, Filter::Off, Pattern::Off, Glow::Auto, Curve::Off},
  // NextUI's (skeleton/BASE/Shaders/*.cfg).
  {"old_tv", "OLD TV", Colour::Off, Scaling::Off, Filter::Off, Pattern::ResScanlines, Glow::Off, Curve::Barrel},
  {"real_gba", "REAL GBA", Colour::Off, Scaling::Pixellate, Filter::Off, Pattern::Lcd3x, Glow::Off, Curve::Off},
  {"real_gameboy", "REAL GB", Colour::Off, Scaling::Pixellate, Filter::Off, Pattern::Lcd3x, Glow::Off, Curve::Off},
  {"nextui_scanlines", "SCANLINES", Colour::Off, Scaling::Off, Filter::Off, Pattern::ResScanlinesAuto, Glow::Off, Curve::Off},
  {"pixelperfect_sharp", "PIXEL SHARP", Colour::Off, Scaling::Pixellate, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"pixelperfect_smooth", "PIXEL SMOOTH", Colour::Off, Scaling::Smooth3x, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  // rgds-core's (assets/shaders/catalog.tsv), in its order.
  {"sharp_bilinear", "SHARP BILIN.", Colour::Off, Scaling::SharpBilinear, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"lcd1x", "LCD1X", Colour::Off, Scaling::Off, Filter::Off, Pattern::Lcd, Glow::Off, Curve::Off},
  {"zfast_lcd", "ZFAST LCD", Colour::Off, Scaling::Off, Filter::Off, Pattern::ZfastLcd, Glow::Off, Curve::Off},
  {"nds_color", "NDS COLOR", Colour::Nds, Scaling::Off, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"natural_vision", "NAT. VISION", Colour::NaturalVision, Scaling::Off, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"lcd1x_nds", "LCD+NDS", Colour::Nds, Scaling::Off, Filter::Off, Pattern::Lcd, Glow::Off, Curve::Off},
  {"lcd1x_nv", "LCD+NAT.VIS", Colour::NaturalVision, Scaling::Off, Filter::Off, Pattern::Lcd, Glow::Off, Curve::Off},
  {"lcd1x_both", "LCD+BOTH", Colour::NdsNaturalVision, Scaling::Off, Filter::Off, Pattern::Lcd, Glow::Off, Curve::Off},
  {"sharp_nds", "SHARP+NDS", Colour::Nds, Scaling::SharpBilinear, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"sharp_nv", "SHARP+NAT.V", Colour::NaturalVision, Scaling::SharpBilinear, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"sharp_both", "SHARP+BOTH", Colour::NdsNaturalVision, Scaling::SharpBilinear, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"zfast_nds", "ZFAST+NDS", Colour::Nds, Scaling::Off, Filter::Off, Pattern::ZfastLcd, Glow::Off, Curve::Off},
  {"zfast_nv", "ZFAST+NAT.V", Colour::NaturalVision, Scaling::Off, Filter::Off, Pattern::ZfastLcd, Glow::Off, Curve::Off},
  {"zfast_both", "ZFAST+BOTH", Colour::NdsNaturalVision, Scaling::Off, Filter::Off, Pattern::ZfastLcd, Glow::Off, Curve::Off},
  {"soft_scanlines", "SOFT LINES", Colour::Off, Scaling::Off, Filter::Off, Pattern::Scanlines, Glow::Off, Curve::Off},
  {"warm_paper", "WARM PAPER", Colour::WarmPaper, Scaling::Off, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"cool_crisp", "COOL CRISP", Colour::CoolCrisp, Scaling::Off, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"pocket_green", "POCKET GREEN", Colour::PocketGreen, Scaling::Off, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"amber", "AMBER", Colour::Amber, Scaling::Off, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"soft_lcd_grid", "SOFT GRID", Colour::Off, Scaling::Off, Filter::Off, Pattern::SoftGrid, Glow::Off, Curve::Off},
  {"ds_classic", "DS CLASSIC", Colour::DsClassic, Scaling::Off, Filter::Off, Pattern::DsClassic, Glow::Off, Curve::Off},
  {"ds_lite", "DS LITE", Colour::DsLite, Scaling::Off, Filter::Off, Pattern::DsLite, Glow::Off, Curve::Off},
  {"natural_glow", "NAT. GLOW", Colour::Natural, Scaling::Off, Filter::Off, Pattern::Off, Glow::Glow, Curve::Off},
  {"vivid_lcd", "VIVID LCD", Colour::Vivid, Scaling::Off, Filter::Off, Pattern::RgbStripes, Glow::Off, Curve::Off},
  {"natural_smooth", "NAT. SMOOTH", Colour::Natural, Scaling::Off, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"vivid_smooth", "VIVID SMOOTH", Colour::Vivid, Scaling::Off, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"pixel_beads", "PIXEL BEADS", Colour::Bold, Scaling::Off, Filter::Off, Pattern::Beads, Glow::Off, Curve::Off},
  {"glossy_tiles", "GLOSSY TILES", Colour::Bold, Scaling::Off, Filter::Off, Pattern::Tiles, Glow::Off, Curve::Off},
  {"gba_color", "GBA COLOR", Colour::Gba, Scaling::Off, Filter::Off, Pattern::Off, Glow::Off, Curve::Off},
  {"gba_screen", "GBA SCREEN", Colour::Gba, Scaling::Off, Filter::Off, Pattern::Gba, Glow::Off, Curve::Off},
  // The pak's own.
  {"crt", "CRT", Colour::Off, Scaling::Off, Filter::Off, Pattern::Crt, Glow::Auto, Curve::Off},
};
inline constexpr u8 kPresetCount = static_cast<u8>(sizeof kPresetTable / sizeof kPresetTable[0]);

// The choices the SHADER PRESET row steps through, built from the table.
struct PresetChoices {
  Choice c[kPresetCount];
  constexpr PresetChoices() : c() { for (u8 i = 0; i < kPresetCount; ++i) c[i] = Choice{kPresetTable[i].value, kPresetTable[i].label}; }
};
inline constexpr PresetChoices kPresetChoices{};
inline constexpr const Choice* kPresets = kPresetChoices.c;

inline constexpr u8 kColourCount = static_cast<u8>(sizeof kColours / sizeof kColours[0]);
inline constexpr u8 kScalingCount = static_cast<u8>(sizeof kScalings / sizeof kScalings[0]);
inline constexpr u8 kFilterCount = static_cast<u8>(sizeof kFilters / sizeof kFilters[0]);
inline constexpr u8 kPatternCount = static_cast<u8>(sizeof kPatterns / sizeof kPatterns[0]);
inline constexpr u8 kGlowCount = static_cast<u8>(sizeof kGlows / sizeof kGlows[0]);
inline constexpr u8 kCurveCount = static_cast<u8>(sizeof kCurves / sizeof kCurves[0]);
static_assert(kColourCount == static_cast<u8>(Colour::Count), "one choice per colour");
static_assert(kScalingCount == static_cast<u8>(Scaling::Count), "one choice per scaling");
static_assert(kFilterCount == static_cast<u8>(Filter::Count), "one choice per filter");
static_assert(kPatternCount == static_cast<u8>(Pattern::Count), "one choice per pattern");
static_assert(kGlowCount == static_cast<u8>(Glow::Count), "one choice per glow");
static_assert(kCurveCount == static_cast<u8>(Curve::Count), "one choice per curve");

// The rows the Shaders (Advanced) page shows that SHADER PRESET writes.
// GLOW's three numbers go back to auto with them.
inline constexpr const char* kPresetKeys[] = {"video.colour", "video.scaling", "video.filter", "video.pattern",
                                              "video.glow", "video.curve"};
inline constexpr const char* kGlowNumberKeys[] = {"video.glow_strength", "video.glow_radius", "video.bloom_threshold"};

} // namespace ds::sdl::shader
