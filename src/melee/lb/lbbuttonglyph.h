#ifndef MELEE_LB_LBBUTTONGLYPH_H
#define MELEE_LB_LBBUTTONGLYPH_H

#include <Runtime/platform.h>

#include <sysdolphin/baselib/sislib.h>

/* Kiosk overlay text with inline GameCube button icons.
 *
 * fmt is ASCII (what HSD_SisLib_803A6B98 accepts) plus icon markers:
 *   #A #B #X #Y #Z #L #R      the buttons
 *   #S                        Start (pill with "ST")
 *   #C                        C-stick (yellow disc with "C")
 *   #D                        D-pad (grey cross)
 * and the ASCII punctuation the SIS encoder cannot map on its own
 * ( + ( ) / ! ? ) is translated to its Shift-JIS glyph, so callers write
 * plain "#Z+#X FOR HANDWARMER".
 *
 * An icon is two SIS entries: a coloured shape glyph (extra atlas glyphs
 * 287-290, tools/gen_button_glyphs.py) and the font's own letter laid over
 * it. Everything is positioned from the font's kerning table, so the return
 * value is the exact drawn width and lbButton_Measure lets a caller centre a
 * line: x = (640 - lbButton_Measure(s, fmt)) / 2. Text segments keep the
 * HSD_Text's default colour. */
f32 lbButton_Line(HSD_Text* text, f32 x, f32 y, f32 scale, const char* fmt);
f32 lbButton_Measure(f32 scale, const char* fmt);

/* Every kiosk text context and text object must be created on this SIS font
 * index (a slot the game never loads) and the font installed right after the
 * context is made - the button shapes are served through it. */
int lbButton_Font(void);
void lbButton_InstallFont(void);

#endif
