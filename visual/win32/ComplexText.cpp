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
static tTVPShapeFont *TVPSelectShapeFont(const tTVPFont &font, const tjs_char *text, tjs_int len, ttstr &face)
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
	std::vector<tTVPShapedGlyph> &glyphs, ttstr &face, tjs_int &width)
{
	glyphs.clear();
	width = 0;
	if(len <= 0) return false;

	tTVPShapeFont *sf = TVPSelectShapeFont(font, text, len, face);
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
		g.Y = TVPRound26_6(pos[i].y_offset);
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
	tjs_int width;
	if(!TVPShapeComplexRun(font, text, len, glyphs, face, width)) return -1;
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
