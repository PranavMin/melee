#ifndef GALE01_40CD40
#define GALE01_40CD40

#include <Runtime/platform.h>

typedef struct TextGlyphTexture {
    /*0x00*/ u8 data[512];
} TextGlyphTexture;

/* 287 vanilla glyphs + 4 kiosk button shapes (sislib_font_extra.inc);
 * the three lookup tables in hsd_3A76.c carry one row per glyph. */
#define SIS_FONT_GLYPHS (287 + 4)
/* 40CD40 */ extern TextGlyphTexture HSD_SisLib_FontAtlas[SIS_FONT_GLYPHS];

#endif
