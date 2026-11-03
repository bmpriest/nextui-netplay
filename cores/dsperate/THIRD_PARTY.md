# Third-party work

The Visual FX page's SHADER PRESET row and its Shaders page port other people's screen shaders to the CPU. The port is `overlay/minui_shader.{h,cpp}` and `overlay/minui_resample.{h,cpp}`, GPL-3.0-or-later like the rest of the DSperate binary it is built into (`LICENSE.DSperate`). These are the shaders it comes from, who wrote them and under what licence.

| Shader | Authors | Licence | Used for |
| --- | --- | --- | --- |
| The RG DS presets (`ds_classic`, `ds_lite`, `natural_glow`, `natural_smooth`, `vivid_lcd`, `vivid_smooth`, `pixel_beads`, `glossy_tiles`, `gba_color`, `gba_screen`, `soft_scanlines`, `soft_lcd_grid`, `warm_paper`, `cool_crisp`, `pocket_green`, `amber`) | [kleinrein/rgds-core](https://github.com/kleinrein/rgds-core) | GPL-3.0-or-later | Every COLOUR and PATTERN but the ones below |
| lcd1x | jdgleaver, from Gigaherz's lcd3x | GPL-2.0-or-later; lcd3x is public domain | PATTERN `lcd` |
| zfast_lcd | Greg Hogan (SoltanGris42) | GPL-2.0-or-later | PATTERN `zfast_lcd` |
| nds-color | hunterk, modified by Pokefan531 | Public domain | COLOUR `nds` |
| The GBA colour model | Pokefan531 | Public domain | rgds-core's `gba_color` and `gba_screen`, so COLOUR `gba` |
| sharp-bilinear | Themaister | Public domain | SCALING `sharp_bilinear` |
| Natural Vision | ShadX, ported by Hyllian and parameterised by Sp00kyFox | None stated | COLOUR `natural_vision`. With no licence to go by, no code was copied: the maths (gamma 2.2, Y raised to 1.1 and I and Q scaled by 1.1 in NTSC YIQ, gamma 2.2) was written out afresh |

NextUI's shaders come from the copies in [LoveRetro/NextUI](https://github.com/LoveRetro/NextUI) (`skeleton/BASE/Shaders`). NextUI did not write them; most are libretro's, each under its own authors' terms rather than NextUI's:

| Shader | Authors | Licence | Used for |
| --- | --- | --- | --- |
| NextUI's presets (`old-tv`, `real-gba`, `real-gameboy`, `scanlines`, `pixelperfect-sharp`, `pixelperfect-smooth`) | The NextUI team | PolyForm Noncommercial 1.0.0, NextUI's own. No NextUI file or code is copied: each preset is recorded only as which of the shaders below it runs | SHADER PRESET |
| pixellate | Fes | ISC-style permissive | SCALING `pixellate` |
| sharp-shimmerless, sharp-shimmerless-grid, sharp-shimmerless-subpixel-vrgb | zadpos | Public domain | SCALING `sharp_shimmerless`, `shimmerless_grid`, `shimmerless_vrgb` |
| edge1pixel | decavoid | None stated | SCALING `edge1pixel` |
| stock | | None stated | SCALING `smooth_3x`'s first pass, which is plain nearest |
| bicubic | Team XBMC, Stefanos A. | GPL, version not stated | SCALING `bicubic` |
| aa-shader-4.0 | guest(r), part of the code by ShadX, ported by Hyllian and hunterk | None stated | SCALING `aa_4` |
| waterpaint | Not named | None stated | SCALING `waterpaint` |
| scale3x | Andrea Mazzoleni | GPL, version not stated | SCALING `scale3x` |
| fast-sharpen | guest(r) | GPL-2.0-or-later | FILTER `fast_sharpen` |
| pixel_art_AA | DariusG | GPL-2.0-or-later | FILTER `pixel_art_aa` |
| lcd3x | Gigaherz | Public domain, as lcd1x's notice says; no notice in its own copy | PATTERN `lcd3x` |
| scanline | Not named | None stated | PATTERN `scanline` |
| res-independent-scanlines | RiskyJumps | Public domain | PATTERN `res_scanlines`, `res_scanlines_auto` |
| retro-v2 | Not named | None stated | PATTERN `retro_v2` |
| barrel-distortion | davej | GPL-2.0-or-later | CURVE `barrel` |

edge1pixel, stock, aa-shader-4.0, waterpaint, scanline and retro-v2 carry no licence notice in NextUI's copy or libretro's, and lcd3x's own copy carries none either. They are ported on the understanding that they are free to use as part of libretro's collection, as NextUI ships them.

PATTERN `crt`, its beam spread and the GLOW row are written for this pak and come from none of these, though `crt`'s stripes are box-filtered the way rgds-core's `ds_classic` and `vivid_lcd` box-filter theirs.

The GPL-2.0-or-later shaders are used under the "or later" terms, as GPL-3.0, and the GPL ones that state no version under GPL-3.0. The libretro originals are in [libretro/glsl-shaders](https://github.com/libretro/glsl-shaders). lcd1x and zfast_lcd are ported with the constants rgds-core gives them (`BRIGHTEN_SCANLINES 24`, `BRIGHTEN_LCD 6`, `BORDERMULT 16`).
