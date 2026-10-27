// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. Compiled into DSperate (GPLv3) by
// patches/0003-minui-integration.patch.
//
// The drawing the MinUI-style pause menu (minui_menu.h) is built from: the
// launcher's own font through stb_truetype, pills and rounded windows,
// straight-alpha blending onto a Canvas, slot thumbnails through stb_image,
// the battery, and the look picked by launch.sh.
//
// Nothing here includes SDL or minui.h, so the ctest build, which cannot run
// an SDL binary, exercises all of it. The only upstream types it touches are
// the public Canvas and the 5x7 fallback font from menu.h.
#pragma once
#include "frontend/sdl/menu.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ds::sdl::minui_gfx {

// 0xAARRGGBB, the Canvas format.
constexpr u32 argb(u32 rgb, u32 a = 0xFF) { return (a << 24) | (rgb & 0x00FFFFFF); }

// minarch's palette (common/defines.h).
constexpr u32 kWhite = argb(0xFFFFFF), kBlack = argb(0x000000), kGray = argb(0x999999);
constexpr u32 kLightGray = argb(0x7F7F7F), kDarkGray = argb(0x262626), kRed = argb(0xFF3300);

// Which launcher the menu is dressed for. launch.sh decides and exports it;
// see dsp_detect_style.
struct Theme {
  bool nextui = false;
  std::string font;   // empty: the 5x7 bitmap font
  // NextUI's seven theme colours (minuisettings.txt color1..7), in its own
  // roles: 1 main, 2 accent, 3 accent2, 4 list text, 5 selected list text,
  // 6 hint, 7 background. Defaults are NextUI's (common/config.h).
  u32 color[7] = {argb(0xFFFFFF), argb(0x9B2257), argb(0x1E2329), argb(0xFFFFFF),
                  argb(0x000000), argb(0xFFFFFF), argb(0x000000)};

  // MINUI_STYLE, MINUI_FONT and MINUI_COLOR1..7.
  static Theme from_env();

  // The roles the menu draws with, mapped onto either look.
  u32 title_pill() const { return nextui ? color[1] : kBlack; }
  u32 title_text() const { return nextui ? color[5] : kWhite; }
  u32 selected_pill() const { return nextui ? color[0] : kWhite; }
  u32 selected_text() const { return nextui ? color[4] : kBlack; }
  u32 text() const { return nextui ? color[3] : kWhite; }
  u32 group_pill() const { return nextui ? color[2] : kBlack; }
  u32 hint_text() const { return nextui ? color[5] : kWhite; }
  u32 option_row() const { return nextui ? color[1] : kDarkGray; }
  // minarch draws a black copy of every unselected row offset by (2, 1);
  // NextUI dropped it.
  bool shadow() const { return !nextui; }
};

// "0xRRGGBBAA" (NextUI's own format) or "0xRRGGBB", with or without the 0x.
// The alpha byte is dropped: the menu's pills are always opaque.
bool parse_colour(const std::string& s, u32& out);

// Straight-alpha source-over, correct onto a destination that is itself
// transparent: the display-engine and GPU tiers hand the menu an overlay
// cleared to 0, which the hardware then blends over the picture.
u32 blend(u32 dst, u32 src);

// Columns and rows outside the clip are left alone. Set around a row whose
// text must not run into its neighbour.
struct Clip { int x0 = 0, y0 = 0, x1 = 1 << 30, y1 = 1 << 30; };

class Painter {
public:
  explicit Painter(const Canvas& c) : c_(c) {}
  const Canvas& canvas() const { return c_; }
  void set_clip(const Clip& k) { clip_ = k; }
  const Clip& clip() const { return clip_; }
  // Scales everything drawn after it, text included, so a whole pill can
  // fade out. 255 is opaque. clear() ignores it: it is a copy, not a blend.
  void set_alpha(u32 a) { alpha_ = a > 255 ? 255 : a; }
  u32 alpha() const { return alpha_; }

  // Blends `colour` scaled by `cover` (0..255) at one pixel.
  void plot(int x, int y, u32 colour, u32 cover = 255);
  void fill(int x, int y, int w, int h, u32 colour);
  // Opaque copy, no blending: the options pages paint the whole canvas
  // black first, as minarch does.
  void clear(u32 colour);
  // Anti-aliased rounded rectangle. A radius of half the height is a pill.
  void rounded(int x, int y, int w, int h, int radius, u32 colour);
  void pill(int x, int y, int w, int h, u32 colour) { rounded(x, y, w, h, h / 2, colour); }
  void circle(int cx, int cy, int r, u32 colour) { rounded(cx - r, cy - r, 2 * r, 2 * r, r, colour); }

private:
  bool inside(int x, int y) const;
  Canvas c_;
  Clip clip_;
  u32 alpha_ = 255;
};

// A TrueType or CFF font at any pixel size, with the 5x7 bitmap font as the
// fallback when no file was given or it would not parse. Sizes follow
// SDL_ttf's (TTF_OpenFont's ptsize is the em height in pixels at 72 dpi),
// so a size 16 here is minarch's FONT_LARGE at the same scale.
class Font {
public:
  Font();
  ~Font();
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;

  bool load(const std::string& path);
  bool loaded() const { return info_ != nullptr; }

  // Width of `s` in pixels at `px`.
  int width(int px, const std::string& s) const;
  // The height SDL_ttf would give a rendered line: ascent minus descent.
  int line_height(int px) const;
  // Draws with the top of the line box at `y`, as an SDL_ttf surface blitted
  // at `y` would. Returns the x past the text. `keep_case` matters only to
  // the bitmap fallback, which otherwise folds a-z onto capitals; the name
  // editor needs the player to see which case they are typing.
  int draw(Painter& p, int x, int y, int px, u32 colour, const std::string& s, bool keep_case = false) const;
  // Cut to `max_w` with "..." at the end. Unchanged when it fits.
  std::string fit(int px, const std::string& s, int max_w) const;

private:
  struct Glyph { int w = 0, h = 0, xoff = 0, yoff = 0, advance = 0; std::vector<u8> alpha; };
  const Glyph& glyph(int px, u32 cp) const;
  // The 5x7 font's pixel scale for a line `px` tall.
  static int bitmap_scale(int px);

  std::vector<u8> data_;
  struct Info;
  std::unique_ptr<Info> info_;
  mutable std::map<std::pair<int, u32>, Glyph> cache_;
};

// Decoded image, 0xAARRGGBB.
struct Image {
  int w = 0, h = 0;
  std::vector<u32> px;
  bool empty() const { return px.empty(); }
};

// PNG or BMP by content, not by name: NextUI and DSperate write PNG bytes to
// the .bmp path the launcher reads, and MinUI writes a real BMP there.
Image load_image(const std::string& path);
Image decode_image(const u8* data, size_t size);
// Box-filtered when shrinking, which is every thumbnail on every panel here.
Image scale_image(const Image& src, int w, int h);
void blit(Painter& p, const Image& img, int x, int y);

// From /sys/class/power_supply, whichever supply says its type is Battery.
// `percent` is -1 when there is none, as on a desktop.
struct Battery { int percent = -1; bool charging = false; };
Battery read_battery(const std::string& root = "/sys/class/power_supply");

// minarch's layout units, times a scale picked from the canvas. FIXED_SCALE
// is 3 on the 1024x768 Brick and 2 everywhere else; anything smaller than a
// 480-line panel is the 256x192 SDL fallback and gets a fraction of a unit.
struct Metrics {
  double k = 2.0;
  int padding = 10;   // in units
  int u(double units) const { return static_cast<int>(units * k + 0.5); }

  static Metrics for_canvas(int w, int h);

  static constexpr int PILL_SIZE = 30, BUTTON_SIZE = 20, BUTTON_MARGIN = 5, BUTTON_PADDING = 12;
  static constexpr int OPTION_PADDING = 8, WINDOW_RADIUS = 4, PAGINATION_HEIGHT = 6, SCROLL_HEIGHT = 4;
  static constexpr int FONT_LARGE = 16, FONT_MEDIUM = 14, FONT_SMALL = 12, FONT_TINY = 10;
};

} // namespace ds::sdl::minui_gfx
