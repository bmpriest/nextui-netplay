// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. Compiled into DSperate (GPLv3) by
// patches/0006-shaders.patch.
//
// The SCALING and FILTER rows: NextUI's (and rgds-core's sharp-bilinear)
// resampling and neighbour shaders on the CPU, the way minui_shader.h does
// the colours and patterns.
//
// A resampler takes the scaler's place for its view (Gpu::ScaleTarget::
// resample). It keeps the last few DS lines it was handed and writes each
// row of the view once the last DS line that row needs has arrived, so a
// frame still goes out line by line and on time. Patterns are drawn over its
// rows as over the nearest scaler's.
//
// Each follows its GLSL as NextUI's minarch runs it: a pass at the view's
// size over the game's texture, sampled with the filter libretro's preset for
// it names (linear for the sharp ones, which rely on it; nearest for the rest),
// clamped at the edges. pixellate blends in linear light, as its own
// INTERPOLATE_IN_LINEAR_GAMMA default says.
//
// A filter (LineFilter, minui_shader.h) works at the DS's own resolution, a
// line behind, and its output goes on through GLOW and the scaler.
//
// Credits, with the licence each original carries (THIRD_PARTY.md):
//  - sharp-bilinear: Themaister, public domain.
//  - pixellate: Fes, ISC-style permissive.
//  - sharp-shimmerless (and -grid, -subpixel-vrgb): zadpos, public domain.
//  - edge1pixel: decavoid, no notice in NextUI's or libretro's copy.
//  - bicubic: Team XBMC and Stefanos A., GPL.
//  - aa-shader-4.0: guest(r), with part of the code by ShadX, ported by
//    Hyllian and hunterk; no notice in NextUI's or libretro's copy.
//  - waterpaint: no author or notice in NextUI's copy.
//  - scale3x: Andrea Mazzoleni, GPL.
//  - fast-sharpen: guest(r), GPL-2.0-or-later.
//  - pixel_art_AA: DariusG, GPL-2.0-or-later.
//  - barrel-distortion: davej, GPL-2.0-or-later.
#pragma once
#include "frontend/sdl/minui_shader.h"
#include "core/gpu/gpu.h"
#include "core/types.h"

#include <memory>
#include <vector>

namespace ds::sdl::shader {

// One view's resampler: build() for its geometry, between frames; run() (the
// scaler's hook) on whichever thread scales that screen, in line order.
class Resampler {
public:
  virtual ~Resampler() = default;
  // The view: a w x h rect, columns [x0, x1) drawn (integer overscale crops the rest).
  void build(u32 w, u32 h, u32 x0, u32 x1);
  Scaling kind() const { return kind_; }
  static void hook(void* self, const ds::gpu::Gpu::ScaleTarget& t, u32 line, const u32* src) {
    static_cast<Resampler*>(self)->run(t, line, src);
  }
  void run(const ds::gpu::Gpu::ScaleTarget& t, u32 line, const u32* src);
  // A whole DS frame into `dst` (the rect, `pitch` pixels a row), as run()
  // would draw it, pattern included when `pattern` is given.
  void frame(const u32* src, u32* dst, u32 pitch, const ds::gpu::Gpu::PatternRows* pattern = nullptr);

protected:
  explicit Resampler(Scaling k) : kind_(k) {}
  virtual void setup() = 0;                   // after build(): tables
  virtual u32 window() const = 0;             // DS lines kept
  virtual u32 last_line(u32 y) const = 0;     // the last DS line row y needs, 0..191; never decreasing in y
  virtual void on_line(u32 line) { (void)line; }   // after a line is kept
  virtual void row(u32 y, u32* out) = 0;      // row y, n_ pixels
  const u32* line_at(s32 l) const;            // a kept line, clamped to the frame
  // Output pixel i's centre along an axis of `len` panel pixels showing `n` DS pixels, in DS pixels.
  static double centre(u32 i, u32 len, u32 n) { return (i + 0.5) * n / len; }
  Scaling kind_;
  u32 w_ = 0, h_ = 0, x0_ = 0, x1_ = 0, n_ = 0;

private:
  std::vector<u32> lines_;
  u32 kept_ = 0;
  u32 next_row_ = 0;
  std::vector<u32> scratch_;
};

std::unique_ptr<Resampler> make_resampler(Scaling s);

// sharp-shimmerless-grid's shading along one axis, for pixels [from, from +
// count) of `len` showing `n` DS pixels: a pattern of its own (build_pattern_from).
void shimmer_grid_shading(u32 len, u32 n, u32 from, u32 count, std::vector<double>& out);

// FILTER: fast-sharpen or pixel_art_AA over each DS pixel and its eight
// neighbours, a line behind.
class NeighbourFilter final : public LineFilter {
public:
  void configure(Filter f) { kind_ = f; }
  Filter kind() const { return kind_; }
  bool on() const override { return kind_ != Filter::Off; }
  u32 delay() const override { return 1; }
  void push(u32 line, const u32* src) override;
  void pull(u32 line, u32* out) override;
private:
  Filter kind_ = Filter::Off;
  u32 lines_[3][SCREEN_W] = {};   // by line % 3
  s32 last_ = -1;
};

// CURVE: barrel-distortion over a view's finished rows, at the panel's
// resolution. NextUI runs it as its own pass, nearest, before the scanlines,
// so the pattern is drawn after it and stays straight. The map holds, for
// each pixel of rows [y0, y1) and columns [x0, x1) of a rw x rh view, the
// pixel of the same window it shows (row * (x1 - x0) + column - x0), or
// kCurveBlack outside the curve or the window. Empty for Curve::Off.
inline constexpr u32 kCurveBlack = 0xFFFFFFFFu;
void build_curve_map(Curve c, u32 rw, u32 rh, u32 x0, u32 x1, u32 y0, u32 y1, std::vector<u32>& map);
// One row through the map: n pixels of `map` from `src`, the window's pixels.
void curve_row(const u32* map, const u32* src, u32 n, u32* dst);

} // namespace ds::sdl::shader
