// SPDX-License-Identifier: GPL-3.0-or-later
// Vendored by minui-nintendo-dsperate-pak. See minui_gfx.h.
#include "minui_gfx.h"

// Declarations only; minui_stb.cpp holds the one copy of the implementations.
#include "stb_image.h"
#include "stb_truetype.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <dirent.h>

namespace ds::sdl::minui_gfx {

namespace {

std::string env_str(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

// One code point, advancing `i` past its bytes. A malformed sequence is one
// U+FFFD rather than a run of garbage.
u32 next_cp(const std::string& s, size_t& i) {
  const unsigned char b0 = static_cast<unsigned char>(s[i++]);
  if (b0 < 0x80) return b0;
  const int len = b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : b0 >= 0xC0 ? 2 : 0;
  if (len == 0) return 0xFFFD;
  u32 cp = b0 & (0x7Fu >> len);
  for (int k = 1; k < len; ++k) {
    if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
  }
  return cp;
}

// Drops the last code point, so trimming for width never leaves a lead byte.
void pop_cp(std::string& s) {
  while (!s.empty()) {
    const unsigned char b = static_cast<unsigned char>(s.back());
    s.pop_back();
    if ((b & 0xC0) != 0x80) break;
  }
}

std::string read_line(const std::string& path) {
  std::ifstream f(path);
  std::string line;
  if (!f || !std::getline(f, line)) return {};
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
  return line;
}

} // namespace

bool parse_colour(const std::string& in, u32& out) {
  std::string s = in;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.erase(0, 2);
  else if (!s.empty() && s[0] == '#') s.erase(0, 1);
  if (s.size() != 6 && s.size() != 8) return false;
  for (char c : s)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  u32 v = static_cast<u32>(std::strtoul(s.c_str(), nullptr, 16));
  if (s.size() == 8) v >>= 8;   // RRGGBBAA: the alpha is not ours to honour
  out = argb(v);
  return true;
}

Theme Theme::from_env() {
  Theme t;
  t.nextui = env_str("MINUI_STYLE") == "nextui";
  t.font = env_str("MINUI_FONT");
  for (int i = 0; i < 7; ++i) {
    const std::string name = "MINUI_COLOR" + std::to_string(i + 1);
    u32 c;
    if (parse_colour(env_str(name.c_str()), c)) t.color[i] = c;
  }
  return t;
}

u32 blend(u32 dst, u32 src) {
  const u32 sa = src >> 24;
  if (sa == 0xFF) return src;
  if (sa == 0) return dst;
  const u32 da = dst >> 24;
  // Everything in 0..255 squared, so one division per channel at the end.
  const u32 dw = da * (255 - sa);         // the destination's surviving weight, x255
  const u32 oa = sa * 255 + dw;           // out alpha, x255
  if (oa == 0) return 0;
  u32 out = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const u32 sc = (src >> shift) & 0xFF, dc = (dst >> shift) & 0xFF;
    const u32 c = (sc * sa * 255 + dc * dw + oa / 2) / oa;
    out |= std::min<u32>(c, 255) << shift;
  }
  return out | (((oa + 127) / 255) << 24);
}

bool Painter::inside(int x, int y) const {
  return x >= 0 && y >= 0 && x < c_.w && y < c_.h && x >= clip_.x0 && x < clip_.x1 && y >= clip_.y0 && y < clip_.y1;
}

void Painter::plot(int x, int y, u32 colour, u32 cover) {
  if (alpha_ < 255) cover = (cover * alpha_ + 127) / 255;
  if (!inside(x, y) || cover == 0) return;
  const u32 a = ((colour >> 24) * cover + 127) / 255;
  u32& d = c_.px[static_cast<size_t>(y) * c_.pitch + static_cast<size_t>(x)];
  d = blend(d, (colour & 0x00FFFFFF) | (a << 24));
}

void Painter::fill(int x, int y, int w, int h, u32 colour) {
  if (alpha_ < 255) colour = (colour & 0x00FFFFFF) | ((((colour >> 24) * alpha_ + 127) / 255) << 24);
  const int x0 = std::max({x, 0, clip_.x0}), x1 = std::min({x + w, c_.w, clip_.x1});
  const int y0 = std::max({y, 0, clip_.y0}), y1 = std::min({y + h, c_.h, clip_.y1});
  for (int yy = y0; yy < y1; ++yy) {
    u32* row = c_.px + static_cast<size_t>(yy) * c_.pitch;
    for (int xx = x0; xx < x1; ++xx) row[xx] = blend(row[xx], colour);
  }
}

void Painter::clear(u32 colour) {
  for (int yy = 0; yy < c_.h; ++yy) {
    u32* row = c_.px + static_cast<size_t>(yy) * c_.pitch;
    std::fill(row, row + c_.w, colour);
  }
}

void Painter::rounded(int x, int y, int w, int h, int radius, u32 colour) {
  if (w <= 0 || h <= 0) return;
  const int r = std::clamp(radius, 0, std::min(w, h) / 2);
  if (r == 0) { fill(x, y, w, h, colour); return; }
  // The straight middle band, then the two end bands with their corners
  // shaded by distance from the corner's centre.
  fill(x, y + r, w, h - 2 * r, colour);
  fill(x + r, y, w - 2 * r, r, colour);
  fill(x + r, y + h - r, w - 2 * r, r, colour);
  const double rr = r;
  for (int dy = 0; dy < r; ++dy) {
    for (int dx = 0; dx < r; ++dx) {
      // Distance from this pixel's centre to the corner circle's centre.
      const double fx = rr - (dx + 0.5), fy = rr - (dy + 0.5);
      const double cover = std::clamp(rr + 0.5 - std::sqrt(fx * fx + fy * fy), 0.0, 1.0);
      if (cover <= 0.0) continue;
      const u32 c = static_cast<u32>(cover * 255.0 + 0.5);
      plot(x + dx, y + dy, colour, c);
      plot(x + w - 1 - dx, y + dy, colour, c);
      plot(x + dx, y + h - 1 - dy, colour, c);
      plot(x + w - 1 - dx, y + h - 1 - dy, colour, c);
    }
  }
}

// ---------------------------------------------------------------------------
// Fonts

struct Font::Info {
  stbtt_fontinfo info;
  int ascent = 0, descent = 0, gap = 0;
};

Font::Font() = default;
Font::~Font() = default;

bool Font::load(const std::string& path) {
  info_.reset();
  cache_.clear();
  data_.clear();
  if (path.empty()) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "minui: cannot open font %s; using the built-in font\n", path.c_str());
    return false;
  }
  data_.assign(std::istreambuf_iterator<char>(f), {});
  auto info = std::make_unique<Info>();
  const int at = stbtt_GetFontOffsetForIndex(data_.data(), 0);
  if (data_.empty() || at < 0 || !stbtt_InitFont(&info->info, data_.data(), at)) {
    std::fprintf(stderr, "minui: cannot read font %s; using the built-in font\n", path.c_str());
    data_.clear();
    return false;
  }
  stbtt_GetFontVMetrics(&info->info, &info->ascent, &info->descent, &info->gap);
  info_ = std::move(info);
  return true;
}

int Font::bitmap_scale(int px) { return std::max(1, px / 8); }

const Font::Glyph& Font::glyph(int px, u32 cp) const {
  const auto key = std::make_pair(px, cp);
  const auto it = cache_.find(key);
  if (it != cache_.end()) return it->second;
  Glyph g;
  const float scale = stbtt_ScaleForMappingEmToPixels(&info_->info, static_cast<float>(px));
  int index = stbtt_FindGlyphIndex(&info_->info, static_cast<int>(cp));
  if (index == 0 && cp != ' ') index = stbtt_FindGlyphIndex(&info_->info, '?');
  int adv = 0, lsb = 0;
  stbtt_GetGlyphHMetrics(&info_->info, index, &adv, &lsb);
  g.advance = static_cast<int>(std::lround(adv * scale));
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  stbtt_GetGlyphBitmapBox(&info_->info, index, scale, scale, &x0, &y0, &x1, &y1);
  g.w = x1 - x0;
  g.h = y1 - y0;
  g.xoff = x0;
  g.yoff = y0;
  if (g.w > 0 && g.h > 0) {
    g.alpha.assign(static_cast<size_t>(g.w) * g.h, 0);
    stbtt_MakeGlyphBitmap(&info_->info, g.alpha.data(), g.w, g.h, g.w, scale, scale, index);
  }
  return cache_.emplace(key, std::move(g)).first->second;
}

int Font::line_height(int px) const {
  if (!info_) return px + px / 4;
  const float scale = stbtt_ScaleForMappingEmToPixels(&info_->info, static_cast<float>(px));
  return static_cast<int>(std::lround(info_->ascent * scale) - std::lround(info_->descent * scale));
}

int Font::width(int px, const std::string& s) const {
  if (!info_) return text_width(bitmap_scale(px), s.c_str());
  int w = 0;
  for (size_t i = 0; i < s.size();) w += glyph(px, next_cp(s, i)).advance;
  return w;
}

int Font::draw(Painter& p, int x, int y, int px, u32 colour, const std::string& s, bool keep_case) const {
  if (!info_) {
    // The bitmap font, centred in the line box a real font would have used.
    const int gs = bitmap_scale(px);
    const int gy = y + (line_height(px) - 7 * gs) / 2;
    if (p.alpha() == 255) return draw_text(p.canvas(), x, gy, gs, colour, s.c_str(), keep_case) + gs;
    // Upstream's draw_text writes opaque pixels, so a fading line is drawn
    // into a scratch canvas first and blended from there.
    const int w = width(px, s) + gs, h = 7 * gs;
    std::vector<u32> scratch(static_cast<size_t>(w) * h, 0);
    draw_text(Canvas{scratch.data(), static_cast<u32>(w), w, h}, 0, 0, gs, colour, s.c_str(), keep_case);
    for (int yy = 0; yy < h; ++yy)
      for (int xx = 0; xx < w; ++xx)
        if (const u32 c = scratch[static_cast<size_t>(yy) * w + xx]) p.plot(x + xx, gy + yy, c);
    return x + w;
  }
  const float scale = stbtt_ScaleForMappingEmToPixels(&info_->info, static_cast<float>(px));
  const int baseline = y + static_cast<int>(std::lround(info_->ascent * scale));
  for (size_t i = 0; i < s.size();) {
    const Glyph& g = glyph(px, next_cp(s, i));
    for (int gy = 0; gy < g.h; ++gy)
      for (int gx = 0; gx < g.w; ++gx) {
        const u8 a = g.alpha[static_cast<size_t>(gy) * g.w + gx];
        if (a) p.plot(x + g.xoff + gx, baseline + g.yoff + gy, colour, a);
      }
    x += g.advance;
  }
  return x;
}

std::string Font::fit(int px, const std::string& s, int max_w) const {
  if (width(px, s) <= max_w) return s;
  std::string out = s;
  while (!out.empty() && width(px, out + "...") > max_w) pop_cp(out);
  return out + "...";
}

// ---------------------------------------------------------------------------
// Images

Image decode_image(const u8* data, size_t size) {
  Image img;
  if (!data || size == 0) return img;
  int w = 0, h = 0, n = 0;
  stbi_uc* rgba = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &n, 4);
  if (!rgba) return img;
  img.w = w;
  img.h = h;
  img.px.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < img.px.size(); ++i) {
    const stbi_uc* q = rgba + i * 4;
    img.px[i] = (static_cast<u32>(q[3]) << 24) | (static_cast<u32>(q[0]) << 16) | (static_cast<u32>(q[1]) << 8) | q[2];
  }
  stbi_image_free(rgba);
  return img;
}

Image load_image(const std::string& path) {
  if (path.empty()) return {};
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  const std::vector<u8> bytes{std::istreambuf_iterator<char>(f), {}};
  return decode_image(bytes.data(), bytes.size());
}

Image scale_image(const Image& src, int w, int h) {
  Image out;
  if (src.empty() || w <= 0 || h <= 0) return out;
  out.w = w;
  out.h = h;
  out.px.resize(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const int sy0 = y * src.h / h, sy1 = std::max(sy0 + 1, (y + 1) * src.h / h);
    for (int x = 0; x < w; ++x) {
      const int sx0 = x * src.w / w, sx1 = std::max(sx0 + 1, (x + 1) * src.w / w);
      u32 a = 0, r = 0, g = 0, b = 0, n = 0;
      for (int sy = sy0; sy < sy1 && sy < src.h; ++sy)
        for (int sx = sx0; sx < sx1 && sx < src.w; ++sx) {
          const u32 c = src.px[static_cast<size_t>(sy) * src.w + sx];
          a += c >> 24; r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF; ++n;
        }
      if (n == 0) n = 1;
      out.px[static_cast<size_t>(y) * w + x] = ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
    }
  }
  return out;
}

void blit(Painter& p, const Image& img, int x, int y) {
  for (int yy = 0; yy < img.h; ++yy)
    for (int xx = 0; xx < img.w; ++xx) p.plot(x + xx, y + yy, img.px[static_cast<size_t>(yy) * img.w + xx]);
}

// ---------------------------------------------------------------------------
// Battery

Battery read_battery(const std::string& root) {
  Battery b;
  DIR* d = opendir(root.c_str());
  if (!d) return b;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string dir = root + "/" + e->d_name;
    if (read_line(dir + "/type") != "Battery") continue;
    const std::string cap = read_line(dir + "/capacity");
    if (cap.empty()) continue;
    b.percent = std::clamp(std::atoi(cap.c_str()), 0, 100);
    b.charging = read_line(dir + "/status") == "Charging";
    break;
  }
  closedir(d);
  return b;
}

// ---------------------------------------------------------------------------
// Metrics

Metrics Metrics::for_canvas(int w, int h) {
  Metrics m;
  if (w == 1024 && h == 768) {
    // The Brick: minarch's FIXED_SCALE 3, PADDING 5.
    m.k = 3.0;
    m.padding = 5;
  } else if (h >= 480) {
    m.k = 2.0;
    m.padding = 10;
  } else {
    // The 256x192 scratch the SDL renderer tier scales like a frame: keep
    // minarch's proportions, which were laid out for a 320x240 screen at 1x.
    m.k = std::max(0.5, h / 320.0);
    m.padding = 10;
  }
  return m;
}

} // namespace ds::sdl::minui_gfx
