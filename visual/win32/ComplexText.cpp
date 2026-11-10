//---------------------------------------------------------------------------
// Complex script text shaping (HarfBuzz) and glyph-index rendering (GDI).
// See ComplexText.h.
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <windows.h>
#include <stdlib.h>
#include <map>
#include <string>
#include <vector>

#include "ComplexText.h"
#include "TextCluster.h"
#include "CharacterData.h"
#include "FontSystem.h"
#include "SysInitIntf.h"
#include "DebugIntf.h"
#include <math.h>

#include "hb.h"
#include "hb-ot.h"

extern FontSystem* TVPFontSystem;

//---------------------------------------------------------------------------
// script classification / cluster segmentation live in TextCluster.h so that
// plugins (KAGParser) can share exactly the same rules.
//---------------------------------------------------------------------------
using namespace TVPTextCluster;
bool TVPIsComplexScriptChar(tjs_char ch) { return IsComplexScriptChar(ch); }
bool TVPTextNeedsShaping(const tjs_char *text) { return TextNeedsShaping(text); }
tjs_int TVPGetComplexRunLength(const tjs_char *text) { return ComplexRunLength(text); }
tjs_int TVPGetTextClusterLength(const tjs_char *text) { return ClusterLength(text); }
//---------------------------------------------------------------------------
// font instances
//---------------------------------------------------------------------------
struct tTVPShapeFont
{
	HDC DC;
	HFONT Font;
	HGDIOBJ OldFont;
	hb_face_t *Face;
	hb_font_t *HbFont;
	std::wstring RealFace; // face name GDI actually selected
	bool Exact; // RealFace matches the requested face

	tTVPShapeFont() : DC(NULL), Font(NULL), OldFont(NULL), Face(NULL), HbFont(NULL), Exact(false) {}
};
struct tTVPShapeFontKey
{
	std::wstring Face;
	tjs_int Height;
	tjs_uint32 Flags;
	bool operator < (const tTVPShapeFontKey &rhs) const
	{
		if(Height != rhs.Height) return Height < rhs.Height;
		if(Flags != rhs.Flags) return Flags < rhs.Flags;
		return Face < rhs.Face;
	}
};
typedef std::map<tTVPShapeFontKey, tTVPShapeFont*> tTVPShapeFontMap;
static tTVPShapeFontMap TVPShapeFonts;
//---------------------------------------------------------------------------
static hb_blob_t *TVPShapeFontReferenceTable(hb_face_t *face, hb_tag_t tag, void *user_data)
{
	HDC dc = (HDC)user_data;
	// GDI wants the tag in file (big endian) byte order
	DWORD gditag = ((tag & 0xFF) << 24) | ((tag & 0xFF00) << 8) | ((tag >> 8) & 0xFF00) | ((tag >> 24) & 0xFF);
	DWORD size = ::GetFontData(dc, gditag, 0, NULL, 0);
	if(size == GDI_ERROR || size == 0) return hb_blob_get_empty();
	char *buf = (char*)malloc(size);
	if(!buf) return hb_blob_get_empty();
	if(::GetFontData(dc, gditag, 0, buf, size) == GDI_ERROR)
	{
		free(buf);
		return hb_blob_get_empty();
	}
	return hb_blob_create(buf, size, HB_MEMORY_MODE_WRITABLE, buf, free);
}
//---------------------------------------------------------------------------
static void TVPClearShapeFonts()
{
	for(tTVPShapeFontMap::iterator it = TVPShapeFonts.begin(); it != TVPShapeFonts.end(); it++)
	{
		tTVPShapeFont *f = it->second;
		if(f->HbFont) hb_font_destroy(f->HbFont);
		if(f->Face) hb_face_destroy(f->Face);
		if(f->DC)
		{
			if(f->OldFont) ::SelectObject(f->DC, f->OldFont);
			::DeleteDC(f->DC);
		}
		if(f->Font) ::DeleteObject(f->Font);
		delete f;
	}
	TVPShapeFonts.clear();
}
//---------------------------------------------------------------------------
static tTVPShapeFont *TVPGetShapeFont(const ttstr &face, tjs_int height, tjs_uint32 flags, bool resolve)
{
	tTVPShapeFontKey key;
	key.Face = face.AsStdString();
	key.Height = height < 0 ? -height : height;
	key.Flags = flags & (TVP_TF_ITALIC|TVP_TF_BOLD);
	if(key.Height == 0) key.Height = 1;

	tTVPShapeFontMap::iterator it = TVPShapeFonts.find(key);
	if(it != TVPShapeFonts.end()) return it->second;

	// too many instances (many font sizes in use): start over
	if(TVPShapeFonts.size() > 64) TVPClearShapeFonts();

	std::wstring name = key.Face;
	if(resolve && TVPFontSystem) name = TVPFontSystem->GetBeingFont(name);
	if(!name.empty() && name[0] == L'@') name = name.substr(1); // vertical variants share glyphs

	tTVPShapeFont *sf = new tTVPShapeFont();
	LOGFONTW lf;
	ZeroMemory(&lf, sizeof(lf));
	lf.lfHeight = -key.Height;
	lf.lfItalic = (key.Flags & TVP_TF_ITALIC) ? TRUE : FALSE;
	lf.lfWeight = (key.Flags & TVP_TF_BOLD) ? 700 : 400;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfQuality = DEFAULT_QUALITY;
	lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	wcsncpy(lf.lfFaceName, name.c_str(), LF_FACESIZE - 1);
	lf.lfFaceName[LF_FACESIZE-1] = 0;

	sf->DC = ::CreateCompatibleDC(NULL);
	sf->Font = ::CreateFontIndirectW(&lf);
	if(sf->DC && sf->Font)
	{
		sf->OldFont = ::SelectObject(sf->DC, sf->Font);
		wchar_t real[LF_FACESIZE + 1];
		real[0] = 0;
		::GetTextFaceW(sf->DC, LF_FACESIZE, real);
		sf->RealFace = real;
		sf->Exact = (_wcsicmp(real, lf.lfFaceName) == 0);

		sf->Face = hb_face_create_for_tables(TVPShapeFontReferenceTable, (void*)sf->DC, NULL);
		if(sf->Face && hb_face_get_glyph_count(sf->Face) > 0)
		{
			sf->HbFont = hb_font_create(sf->Face);
			hb_ot_font_set_funcs(sf->HbFont);
			// 26.6 fixed point pixels; GDI's negative lfHeight is the em size
			hb_font_set_scale(sf->HbFont, key.Height * 64, key.Height * 64);
		}
	}
	TVPShapeFonts[key] = sf;
	return sf;
}
//---------------------------------------------------------------------------
static bool TVPShapeFontHasChar(tTVPShapeFont *sf, tjs_char ch)
{
	if(!sf || !sf->HbFont) return false;
	hb_codepoint_t glyph = 0;
	return hb_font_get_nominal_glyph(sf->HbFont, ch, &glyph) && glyph != 0;
}
//---------------------------------------------------------------------------
// fallback fonts
//---------------------------------------------------------------------------
static std::vector<ttstr> TVPComplexFallbackFonts;
static bool TVPComplexFallbackFontsInit = false;
static void TVPInitComplexFallbackFonts()
{
	if(TVPComplexFallbackFontsInit) return;
	TVPComplexFallbackFontsInit = true;

	// -complexfont="Face 1,Face 2" : fonts tried (in order) when the current
	// font has no glyphs for a complex-script run.
	tTJSVariant val;
	if(TVPGetCommandLine(TJS_W("-complexfont"), &val))
	{
		ttstr str(val);
		const tjs_char *p = str.c_str();
		ttstr cur;
		for(;; p++)
		{
			if(*p == TJS_W(',') || *p == 0)
			{
				if(!cur.IsEmpty()) TVPComplexFallbackFonts.push_back(cur);
				cur.Clear();
				if(*p == 0) break;
			}
			else if(!(cur.IsEmpty() && *p == TJS_W(' ')))
			{
				cur += *p;
			}
		}
	}
	static const tjs_char * const defaults[] = {
		TJS_W("Myanmar Text"), TJS_W("Padauk"), TJS_W("Noto Sans Myanmar"),
		TJS_W("Pyidaungsu"), TJS_W("Myanmar3"),
		TJS_W("Nirmala UI"), TJS_W("Leelawadee UI"), TJS_W("Microsoft Himalaya"),
		TJS_W("Segoe UI"), NULL
	};
	for(int i = 0; defaults[i]; i++) TVPComplexFallbackFonts.push_back(ttstr(defaults[i]));
}
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// vertical fitting
//---------------------------------------------------------------------------
// Burmese fonts are much taller than Latin ones at the same em size: marks
// above the line and stacked consonants below it reach well outside the box
// a game laid out for Latin (or Japanese) text reserves for one line, so the
// text gets cut off. The run is therefore drawn slightly smaller and, when
// needed, moved a little, so that it stays inside the vertical band that the
// game font's own Latin text occupies.
//   -complexscale=NN  fixed size in percent (50..100) instead of automatic
struct tTVPFitKey
{
	std::wstring Main, Shape;
	tjs_int Height;
	tjs_uint32 Flags;
	bool operator < (const tTVPFitKey &rhs) const
	{
		if(Height != rhs.Height) return Height < rhs.Height;
		if(Flags != rhs.Flags) return Flags < rhs.Flags;
		if(Main != rhs.Main) return Main < rhs.Main;
		return Shape < rhs.Shape;
	}
};
struct tTVPFit
{
	tjs_int Height; // em height to shape and draw with
	tjs_int ShiftY; // pixels to move the run down (negative: up)
};
static std::map<tTVPFitKey, tTVPFit> TVPFits;
//---------------------------------------------------------------------------
static bool TVPMeasureInk(tTVPShapeFont *sf, const wchar_t * const *samples, double &top, double &bottom)
{
	// highest and lowest ink (pixels above / below the baseline) of the samples
	if(!sf || !sf->HbFont) return false;
	bool any = false;
	top = bottom = 0;
	for(int s = 0; samples[s]; s++)
	{
		hb_buffer_t *buf = hb_buffer_create();
		hb_buffer_add_utf16(buf, (const uint16_t*)samples[s], -1, 0, -1);
		hb_buffer_guess_segment_properties(buf);
		hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
		hb_shape(sf->HbFont, buf, NULL, 0);
		unsigned int count = 0;
		hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &count);
		hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &count);
		for(unsigned int i = 0; i < count; i++)
		{
			hb_glyph_extents_t ext;
			if(info[i].codepoint == 0) continue;
			if(!hb_font_get_glyph_extents(sf->HbFont, info[i].codepoint, &ext)) continue;
			if(ext.height == 0) continue;
			double t = (pos[i].y_offset + ext.y_bearing) / 64.0;
			double b = -(pos[i].y_offset + ext.y_bearing + ext.height) / 64.0;
			if(!any || t > top) top = t;
			if(!any || b > bottom) bottom = b;
			any = true;
		}
		hb_buffer_destroy(buf);
	}
	return any;
}
//---------------------------------------------------------------------------
static tTVPFit TVPGetFit(const tTVPFont &font, tTVPShapeFont *mainfont, tTVPShapeFont *sf, const ttstr &face)
{
	tjs_int height = font.Height < 0 ? -font.Height : font.Height;
	if(height == 0) height = 1;

	tTVPFitKey key;
	key.Main = font.Face.AsStdString();
	key.Shape = face.AsStdString();
	key.Height = height;
	key.Flags = font.Flags & (TVP_TF_ITALIC|TVP_TF_BOLD);
	std::map<tTVPFitKey, tTVPFit>::iterator it = TVPFits.find(key);
	if(it != TVPFits.end()) return it->second;
	if(TVPFits.size() > 256) TVPFits.clear();

	tTVPFit fit;
	fit.Height = height;
	fit.ShiftY = 0;

	static const wchar_t * const burmese[] = {
		L"\x1000\x103C\x102D\x102F",                         // medial ra + upper vowel
		L"\x1019\x103C\x102D\x102F\x1037",                   // + dot below
		L"\x1000\x103B\x103D\x1014\x103A\x102F\x1015\x103A",  // medials below
		L"\x101B\x103E\x102F", L"\x101C\x103E\x1030",         // ha + u / uu
		L"\x1026\x1038", L"\x1005\x103D\x1032",               // tall independent vowel, ai
		L"\x1004\x103C\x102D\x1019\x1037\x103A",
		L"\x1000\x103C\x103D\x1031\x1038",
		L"\x101E\x1004\x103A\x1039\x1018\x1031\x102C",        // kinzi
		NULL };
	static const wchar_t * const latin[] = { L"gjpqy", NULL };

	double top = 0, bottom = 0;
	TEXTMETRICW tm;
	ZeroMemory(&tm, sizeof(tm));
	if(mainfont && mainfont->DC && ::GetTextMetricsW(mainfont->DC, &tm) && tm.tmAscent > 0 &&
		TVPMeasureInk(sf, burmese, top, bottom) && top > 0)
	{
		// the band the game font's Latin text uses
		double asc = tm.tmAscent;
		double desc = tm.tmDescent;
		double ltop, lbottom;
		if(mainfont->HbFont && TVPMeasureInk(mainfont, latin, ltop, lbottom) && lbottom > 0)
			desc = lbottom + 1;
		if(desc < height / 5.0) desc = height / 5.0;
		if(bottom < 0) bottom = 0;

		double scale = 1.0;
		if(top + bottom > asc + desc) scale = (asc + desc) / (top + bottom);
		if(scale < 0.70) scale = 0.70;

		tTJSVariant val;
		if(TVPGetCommandLine(TJS_W("-complexscale"), &val))
		{
			tjs_int percent = (tjs_int)val;
			if(percent >= 50 && percent <= 100) scale = percent / 100.0;
		}

		fit.Height = (tjs_int)floor(height * scale + 0.5);
		if(fit.Height < 4) fit.Height = height < 4 ? height : 4;
		scale = (double)fit.Height / height;

		double lo = top * scale - asc;      // > 0: must move down by this much
		double hi = desc - bottom * scale;  // may move down by at most this much
		double shift;
		if(lo <= 0 && hi >= 0) shift = 0;
		else if(lo > 0 && hi >= lo) shift = lo;
		else if(hi < 0 && lo <= hi) shift = hi;
		else shift = (lo + hi) / 2;         // cannot fit: share the overflow
		fit.ShiftY = (tjs_int)floor(shift + 0.5);

		if(fit.Height != height || fit.ShiftY != 0)
		{
			TVPAddLog(ttstr(TJS_W("(info) Complex text: '")) + face + TJS_W("' is drawn at ") +
				ttstr(fit.Height) + TJS_W("px inside '") + font.Face + TJS_W("' ") +
				ttstr(height) + TJS_W("px lines (vertical shift ") + ttstr(fit.ShiftY) + TJS_W("px)"));
		}
	}
	TVPFits[key] = fit;
	return fit;
}
//---------------------------------------------------------------------------
static tTVPShapeFont *TVPSelectShapeFontUnfitted(const tTVPFont &font, const tjs_char *text, tjs_int len, ttstr &face);
static tTVPShapeFont *TVPSelectShapeFont(const tTVPFont &font, const tjs_char *text, tjs_int len,
	ttstr &face, tjs_int &height, tjs_int &shifty)
{
	height = font.Height < 0 ? -font.Height : font.Height;
	if(height == 0) height = 1;
	shifty = 0;
	tTVPShapeFont *sf = TVPSelectShapeFontUnfitted(font, text, len, face);
	if(!sf || !sf->HbFont) return sf;

	// only Burmese is fitted; other scripts keep their size
	tjs_char probe = text[0];
	bool myanmar = (probe >= 0x1000 && probe <= 0x109F) || (probe >= 0xA9E0 && probe <= 0xA9FF) ||
		(probe >= 0xAA60 && probe <= 0xAA7F);
	if(!myanmar) return sf;

	tTVPShapeFont *mainfont = TVPGetShapeFont(font.Face, font.Height, font.Flags, true);
	tTVPFit fit = TVPGetFit(font, mainfont, sf, face);
	shifty = fit.ShiftY;
	if(fit.Height != height)
	{
		tTVPShapeFont *scaled = TVPGetShapeFont(face, fit.Height, font.Flags, true);
		if(scaled && scaled->HbFont)
		{
			height = fit.Height;
			return scaled;
		}
	}
	return sf;
}
//---------------------------------------------------------------------------
static tTVPShapeFont *TVPSelectShapeFontUnfitted(const tTVPFont &font, const tjs_char *text, tjs_int len, ttstr &face)
{
	// probe with the first character that is not a joiner
	tjs_char probe = text[0];
	for(tjs_int i = 0; i < len; i++)
	{
		if(!IsJoiner(text[i])) { probe = text[i]; break; }
	}

	tTVPShapeFont *mainfont = TVPGetShapeFont(font.Face, font.Height, font.Flags, true);
	if(TVPShapeFontHasChar(mainfont, probe))
	{
		face = font.Face;
		return mainfont;
	}

	TVPInitComplexFallbackFonts();
	for(size_t i = 0; i < TVPComplexFallbackFonts.size(); i++)
	{
		tTVPShapeFont *sf = TVPGetShapeFont(TVPComplexFallbackFonts[i], font.Height, font.Flags, false);
		if(sf->Exact && TVPShapeFontHasChar(sf, probe))
		{
			face = TVPComplexFallbackFonts[i];
			return sf;
		}
	}

	// nothing has the glyphs: draw with the current font (shows .notdef boxes)
	face = font.Face;
	return (mainfont && mainfont->HbFont) ? mainfont : NULL;
}
//---------------------------------------------------------------------------
// shaping
//---------------------------------------------------------------------------
static inline tjs_int TVPRound26_6(hb_position_t v)
{
	return (tjs_int)((v + (v >= 0 ? 32 : -32)) / 64);
}
//---------------------------------------------------------------------------
bool TVPShapeComplexRun(const tTVPFont &font, const tjs_char *text, tjs_int len,
	std::vector<tTVPShapedGlyph> &glyphs, ttstr &face, tjs_int &height, tjs_int &width)
{
	glyphs.clear();
	width = 0;
	height = font.Height < 0 ? -font.Height : font.Height;
	if(len <= 0) return false;

	tjs_int shifty = 0;
	tTVPShapeFont *sf = TVPSelectShapeFont(font, text, len, face, height, shifty);
	if(!sf || !sf->HbFont) return false;

	hb_buffer_t *buf = hb_buffer_create();
	if(!hb_buffer_allocation_successful(buf))
	{
		hb_buffer_destroy(buf);
		return false;
	}
	hb_buffer_add_utf16(buf, (const uint16_t*)text, len, 0, len);
	hb_buffer_guess_segment_properties(buf);
	// the engine lays text out left to right; keep glyphs in visual order
	hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
	hb_shape(sf->HbFont, buf, NULL, 0);

	unsigned int count = 0;
	hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &count);
	hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &count);

	hb_position_t pen = 0;
	glyphs.reserve(count);
	for(unsigned int i = 0; i < count; i++)
	{
		tTVPShapedGlyph g;
		g.Glyph = (tjs_uint16)info[i].codepoint;
		g.X = TVPRound26_6(pen + pos[i].x_offset);
		g.Y = TVPRound26_6(pos[i].y_offset) - shifty;
		glyphs.push_back(g);
		pen += pos[i].x_advance;
	}
	width = TVPRound26_6(pen);
	if(width < 0) width = 0;

	hb_buffer_destroy(buf);
	return true;
}
//---------------------------------------------------------------------------
tjs_int TVPGetComplexRunWidth(const tTVPFont &font, const tjs_char *text, tjs_int len)
{
	// tiny cache: KAG measures every piece of text right before drawing it
	static ttstr last_text;
	static tTVPFont last_font;
	static tjs_int last_width = -1;
	ttstr cur(text, len);
	if(last_width >= 0 && cur == last_text && last_font == font) return last_width;

	std::vector<tTVPShapedGlyph> glyphs;
	ttstr face;
	tjs_int width, height;
	if(!TVPShapeComplexRun(font, text, len, glyphs, face, height, width)) return -1;
	last_text = cur;
	last_font = font;
	last_width = width;
	return width;
}
//---------------------------------------------------------------------------
// glyph rendering
//---------------------------------------------------------------------------
tTVPCharacterData *TVPGetShapedGlyphBitmap(
	const tTVPFontAndCharacterData &font, tjs_int aofsx, tjs_int aofsy)
{
	tTVPCharacterData *data = new tTVPCharacterData();
	data->Metrics.CellIncX = 0;
	data->Metrics.CellIncY = 0;
	data->Gray = 65;
	data->BlackBoxX = 0;
	data->BlackBoxY = 0;
	data->OriginX = aofsx;
	data->OriginY = aofsy;
	data->Pitch = 0;
	data->Antialiased = font.Antialiased;
	data->FullColored = false;
	data->Blured = font.Blured;
	data->BlurWidth = font.BlurWidth;
	data->BlurLevel = font.BlurLevel;

	// font.Font.Face is either the layer font or a fallback face; resolving
	// an exact face name through the font system is harmless.
	tTVPShapeFont *sf = TVPGetShapeFont(font.Font.Face, font.Font.Height, font.Font.Flags, true);
	if(!sf || !sf->DC || !sf->Font) return data;

	static MAT2 no_transform_matrix = { {0,1}, {0,0}, {0,0}, {0,1} };
	GLYPHMETRICS gm;
	ZeroMemory(&gm, sizeof(gm));
	UINT format = (font.Antialiased ? GGO_GRAY8_BITMAP : GGO_BITMAP) | GGO_GLYPH_INDEX;
	UINT code = (UINT)font.Character;

	DWORD size = ::GetGlyphOutlineW(sf->DC, code, format, &gm, 0, NULL, &no_transform_matrix);
	if(size == GDI_ERROR || size == 0 || gm.gmBlackBoxY == 0) return data;

	data->BlackBoxX = gm.gmBlackBoxX;
	data->BlackBoxY = gm.gmBlackBoxY;
	data->OriginX = gm.gmptGlyphOrigin.x + aofsx;
	data->OriginY = -gm.gmptGlyphOrigin.y + aofsy;
	if(font.Antialiased)
		data->Pitch = (size / gm.gmBlackBoxY) & ~0x03;
	else
		data->Pitch = (((gm.gmBlackBoxX - 1) >> 5) + 1) << 2;

	try
	{
		data->Alloc(size);
		::GetGlyphOutlineW(sf->DC, code, format, &gm, size, data->GetData(), &no_transform_matrix);
		if(!font.Antialiased) data->Expand();
		if(font.Blured) data->Blur();
	}
	catch(...)
	{
		data->Release();
		throw;
	}
	return data;
}
//---------------------------------------------------------------------------
