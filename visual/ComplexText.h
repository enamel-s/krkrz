//---------------------------------------------------------------------------
// Complex script (Burmese, Indic, Thai, Khmer ...) text shaping support.
//
// Runs of complex-script characters are shaped with HarfBuzz and drawn by
// glyph index, so reordering, stacking, ligatures and mark positioning work.
// Text that contains no such characters never reaches this code and is drawn
// exactly as before.
//---------------------------------------------------------------------------
#ifndef __COMPLEX_TEXT_H__
#define __COMPLEX_TEXT_H__

#include "tjsCommHead.h"
#include "tvpfontstruc.h"
#include <vector>

struct tTVPShapedGlyph
{
	tjs_uint16 Glyph; // glyph index in the font that was used for shaping
	tjs_int X; // pen position from the start of the run (pixels, rightwards)
	tjs_int Y; // vertical offset from the baseline (pixels, upwards)
};

// Returns true when the character belongs to a script that needs shaping.
bool TVPIsComplexScriptChar(tjs_char ch);

// Returns true when the text contains at least one complex-script character.
bool TVPTextNeedsShaping(const tjs_char *text);

// Length (in UTF-16 units) of the complex-script run that starts at "text".
// Returns 0 when "text" does not start with a complex-script character.
tjs_int TVPGetComplexRunLength(const tjs_char *text);

// Length (in UTF-16 units) of the first grapheme cluster of "text": a base
// character plus everything that has to be drawn together with it (medials,
// vowel signs, tone marks, stacked consonants ...). Returns 0 only for an
// empty string. Text layout code must not split a cluster.
tjs_int TVPGetTextClusterLength(const tjs_char *text);

// Shapes a run. "face" receives the font face actually used (the requested
// face, or a fallback face when the requested one has no glyphs for the run)
// and "height" the em height in pixels it has to be drawn with: Burmese is
// drawn slightly smaller than the layer font so that it fits the line.
// "width" receives the total advance of the run in pixels.
bool TVPShapeComplexRun(const tTVPFont &font, const tjs_char *text, tjs_int len,
	std::vector<tTVPShapedGlyph> &glyphs, ttstr &face, tjs_int &height, tjs_int &width);

// Width in pixels of a run, or -1 when it cannot be shaped.
tjs_int TVPGetComplexRunWidth(const tTVPFont &font, const tjs_char *text, tjs_int len);

// Renders one glyph (font.Character holds a glyph index, font.IsGlyph is set).
class tTVPCharacterData *TVPGetShapedGlyphBitmap(
	const struct tTVPFontAndCharacterData &font, tjs_int aofsx, tjs_int aofsy);

#endif
