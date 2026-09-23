#include "lbbuttonglyph.h"

#include <string.h>

#include <sysdolphin/baselib/sislib.h>

/* How sislib lays glyphs out with kerning on (hsd_3A76.c, our overlays use
 * default_kerning = 1): every glyph is a 32-unit cell scaled by s; the
 * kerning table HSD_SisLib_8040CB00 holds {left, right} blank columns per
 * glyph index; the quad is drawn at pen - (left-1)*s, so ink starts at
 * pen + 1*s, and the pen advances (34 - left - right)*s. */
#define CELL 32.0f

/* Extra atlas glyphs (tools/gen_button_glyphs.py). */
enum lbButton_Shape { SH_DISC, SH_RSQ, SH_PILL, SH_CROSS };
static const char* const shape_str[4] = { "\x85\x40", "\x85\x41", "\x85\x42",
                                          "\x85\x43" };
static const int shape_glyph[4] = { 287, 288, 289, 290 };

/* Letter drawn over the shape, as a fraction of the shape's scale. */
#define LETTER_SCALE 0.58f

struct lbButton_Def {
    char marker;
    u8 shape;
    const char* letters; /* "" = none */
    GXColor fill;
    GXColor ink;
};

/* GameCube controller colours. X/Y/C are light, so their letter is dark. */
/* Macros, not const objects: MWCC (C89) wants literal aggregates here. */
#define WHITE { 255, 255, 255, 255 }
#define DARK { 30, 30, 40, 255 }
static const struct lbButton_Def defs[] = {
    { 'A', SH_DISC, "A", { 63, 174, 109, 255 }, WHITE },
    { 'B', SH_DISC, "B", { 217, 69, 61, 255 }, WHITE },
    { 'X', SH_DISC, "X", { 210, 211, 220, 255 }, DARK },
    { 'Y', SH_DISC, "Y", { 210, 211, 220, 255 }, DARK },
    { 'Z', SH_RSQ, "Z", { 90, 82, 184, 255 }, WHITE },
    { 'L', SH_RSQ, "L", { 138, 140, 153, 255 }, WHITE },
    { 'R', SH_RSQ, "R", { 138, 140, 153, 255 }, WHITE },
    { 'S', SH_PILL, "ST", { 138, 140, 153, 255 }, WHITE },
    { 'C', SH_DISC, "C", { 231, 195, 58, 255 }, DARK },
    { 'D', SH_CROSS, "", { 138, 140, 153, 255 }, WHITE },
};

/* Glyph index of an ASCII char exactly as the SIS encoder maps it
 * (hsd_3A64.c), plus the punctuation we translate to Shift-JIS ourselves.
 * -1 = not drawable. */
static int glyphIndex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'Z') {
        return 10 + (c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return 36 + (c - 'a');
    }
    switch (c) {
    case ' ':
        return 227;
    case ',':
        return 230;
    case '.':
        return 231;
    case ':':
        return 233;
    case '?':
        return 235;
    case '!':
        return 236;
    case '/':
        return 240;
    case '\'':
        return 243;
    case '"':
        return 244;
    case '(':
        return 245;
    case ')':
        return 246;
    case '+':
        return 251;
    case '-':
        return 252;
    }
    return -1;
}

/* Second byte of the Shift-JIS pair (lead 0x81) for punctuation the encoder
 * does not map from ASCII; 0 = pass the ASCII byte through. */
static char sjisLow(char c)
{
    switch (c) {
    case '+':
        return '{';
    case '(':
        return 'i';
    case ')':
        return 'j';
    case '/':
        return '^';
    case '!':
        return 'I';
    case '?':
        return 'H';
    }
    return 0;
}

static f32 kernLeft(int g)
{
    return (f32) HSD_SisLib_8040CB00[2 * g];
}

static f32 kernRight(int g)
{
    return (f32) HSD_SisLib_8040CB00[2 * g + 1];
}

static f32 advance(int g, f32 s)
{
    return (CELL + 2.0f - kernLeft(g) - kernRight(g)) * s;
}

/* Width of a plain text run. */
static f32 textWidth(const char* str, int n, f32 s)
{
    f32 w = 0.0f;
    int i;
    for (i = 0; i < n; i++) {
        int g = glyphIndex(str[i]);
        if (g >= 0) {
            w += advance(g, s);
        }
    }
    return w;
}

/* Offset from the pen to the centre of a run's ink. A glyph's quad sits at
 * pen - (left-1)*s, so its ink runs from pen + 1*s to pen + (33-left-right)*s;
 * for a run, the last glyph's ink end is offset by the advances before it. */
static f32 inkCentre(const char* str, f32 s)
{
    f32 end = 0.0f;
    int n = (int) strlen(str);
    int last = glyphIndex(str[n - 1]);
    int i;
    for (i = 0; i < n - 1; i++) {
        end += advance(glyphIndex(str[i]), s);
    }
    end += (CELL + 1.0f - kernLeft(last) - kernRight(last)) * s;
    return 0.5f * (1.0f * s + end);
}

static const struct lbButton_Def* findDef(char marker)
{
    int i;
    for (i = 0; i < (int) (sizeof(defs) / sizeof(defs[0])); i++) {
        if (defs[i].marker == marker) {
            return &defs[i];
        }
    }
    return NULL;
}

static f32 iconAdvance(const struct lbButton_Def* d, f32 s)
{
    return advance(shape_glyph[d->shape], s);
}

/* Draws one icon at pen (x, y); returns its advance. */
static f32 drawIcon(HSD_Text* text, f32 x, f32 y, f32 s,
                    const struct lbButton_Def* d)
{
    int g = shape_glyph[d->shape];
    f32 cx = x + (1.0f + 0.5f * (CELL - kernLeft(g) - kernRight(g))) * s;
    /* Vertical: every entry's scale opcode is pushed at its start and popped
     * at its end, so each line is measured against the text's default scale
     * (1.0) and a glyph at scale s is drawn 32*(1-s) below the entry's y
     * (bottom-aligned to a 32-unit line). The shape's ink centre is thus at
     * y + 32 - 16s; the letter, at scale t, must be given
     * ly = cy_ink - 32 + 16.5t so its centre (rows 2..28) lands there. */
    f32 cy = y + CELL - 0.5f * CELL * s; /* shape ink centre on screen */
    GXColor c;
    int entry;

    entry = HSD_SisLib_803A6B98(text, x, y, "%s", shape_str[d->shape]);
    HSD_SisLib_803A7548(text, entry, s, s);
    c = d->fill;
    HSD_SisLib_803A74F0(text, entry, &c);

    if (d->letters[0] != '\0') {
        f32 t = s * LETTER_SCALE;
        f32 lx = cx - inkCentre(d->letters, t);
        f32 ly = cy - CELL + 16.5f * t;
        entry = HSD_SisLib_803A6B98(text, lx, ly, "%s", d->letters);
        HSD_SisLib_803A7548(text, entry, t, t);
        c = d->ink;
        HSD_SisLib_803A74F0(text, entry, &c);
    }
    return iconAdvance(d, s);
}

/* Copies a text run into buf, translating the punctuation the SIS encoder
 * cannot map from ASCII into its Shift-JIS pair. */
static void encodeRun(char* buf, int cap, const char* str, int n)
{
    int i;
    int o = 0;
    for (i = 0; i < n && o < cap - 3; i++) {
        char lo = sjisLow(str[i]);
        if (lo != 0) {
            buf[o++] = (char) 0x81;
            buf[o++] = lo;
        } else {
            buf[o++] = str[i];
        }
    }
    buf[o] = '\0';
}

/* Shared walker: draw == NULL measures only. */
static f32 walk(HSD_Text* text, f32 x, f32 y, f32 s, const char* fmt,
                bool draw)
{
    f32 pen = x;
    const char* p = fmt;
    char buf[96];

    while (*p != '\0') {
        const struct lbButton_Def* d = NULL;
        const char* run = p;
        int n = 0;
        /* Text run up to the next marker. */
        while (p[n] != '\0' && !(p[n] == '#' && findDef(p[n + 1]) != NULL)) {
            n++;
        }
        if (n > 0) {
            if (draw) {
                int entry;
                encodeRun(buf, sizeof(buf), run, n);
                entry = HSD_SisLib_803A6B98(text, pen, y, "%s", buf);
                HSD_SisLib_803A7548(text, entry, s, s);
            }
            pen += textWidth(run, n, s);
            p += n;
        }
        if (*p == '#') {
            d = findDef(p[1]);
            if (draw) {
                pen += drawIcon(text, pen, y, s, d);
            } else {
                pen += iconAdvance(d, s);
            }
            p += 2;
        }
    }
    return pen - x;
}

f32 lbButton_Line(HSD_Text* text, f32 x, f32 y, f32 scale, const char* fmt)
{
    return walk(text, x, y, scale, fmt, true);
}

f32 lbButton_Measure(f32 scale, const char* fmt)
{
    return walk(NULL, 0.0f, 0.0f, scale, fmt, false);
}
