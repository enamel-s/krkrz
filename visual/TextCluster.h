//---------------------------------------------------------------------------
// Text cluster segmentation for complex scripts (Burmese, Indic, Thai ...).
//
// Header-only and dependency free on purpose: the engine core and plugins
// that lay text out (KAGParser) include the same file, so both agree on
// where a string may be split.
//---------------------------------------------------------------------------
#ifndef __TEXT_CLUSTER_H__
#define __TEXT_CLUSTER_H__

namespace TVPTextCluster {

//---------------------------------------------------------------------------
// script classification
//---------------------------------------------------------------------------
enum {
	csNone = 0,
	csThai, csLao, csTibetan, csMyanmar, csKhmer, csTaiTham,
	csIndic // csIndic + n : one id per Indic block
};
static inline int ComplexScriptOf(wchar_t ch)
{
	if(ch < 0x0900) return csNone;
	if(ch <= 0x0DFF) return csIndic + ((ch - 0x0900) >> 7);
	if(ch >= 0x0E00 && ch <= 0x0E7F) return csThai;
	if(ch >= 0x0E80 && ch <= 0x0EFF) return csLao;
	if(ch >= 0x0F00 && ch <= 0x0FFF) return csTibetan;
	if(ch >= 0x1000 && ch <= 0x109F) return csMyanmar;
	if(ch >= 0x1780 && ch <= 0x17FF) return csKhmer;
	if(ch >= 0x19E0 && ch <= 0x19FF) return csKhmer;
	if(ch >= 0x1A20 && ch <= 0x1AAF) return csTaiTham;
	if(ch >= 0xA9E0 && ch <= 0xA9FF) return csMyanmar; // Myanmar Extended-B
	if(ch >= 0xAA60 && ch <= 0xAA7F) return csMyanmar; // Myanmar Extended-A
	return csNone;
}
static inline bool IsJoiner(wchar_t ch)
{
	return ch == 0x200C || ch == 0x200D; // ZWNJ, ZWJ
}
//---------------------------------------------------------------------------
static inline bool IsComplexScriptChar(wchar_t ch)
{
	return ComplexScriptOf(ch) != csNone;
}
//---------------------------------------------------------------------------
static inline bool TextNeedsShaping(const wchar_t *text)
{
	if(!text) return false;
	for(; *text; text++)
	{
		if(*text >= 0x0900 && ComplexScriptOf(*text) != csNone) return true;
	}
	return false;
}
//---------------------------------------------------------------------------
static inline int ComplexRunLength(const wchar_t *text)
{
	int script = ComplexScriptOf(text[0]);
	if(script == csNone) return 0;
	int len = 1;
	while(text[len])
	{
		if(IsJoiner(text[len]))
		{
			// a joiner belongs to the run only when the run continues after it
			if(ComplexScriptOf(text[len+1]) != script) break;
		}
		else if(ComplexScriptOf(text[len]) != script)
		{
			break;
		}
		len++;
	}
	return len;
}
//---------------------------------------------------------------------------
// grapheme cluster segmentation
//---------------------------------------------------------------------------
static inline bool IsMyanmarExtender(wchar_t ch)
{
	// characters that never start a cluster
	if(ch >= 0x102B && ch <= 0x103E) return true; // vowel signs, anusvara, tone marks, virama, asat, medials
	if(ch >= 0x1056 && ch <= 0x1059) return true; // vocalic R/L signs
	if(ch >= 0x105E && ch <= 0x1060) return true; // Mon medials
	if(ch >= 0x1062 && ch <= 0x1064) return true; // Sgaw Karen
	if(ch >= 0x1067 && ch <= 0x106D) return true; // Western Pwo Karen
	if(ch >= 0x1071 && ch <= 0x1074) return true; // extra vowel signs
	if(ch >= 0x1082 && ch <= 0x108D) return true; // Shan
	if(ch == 0x108F) return true;
	if(ch >= 0x109A && ch <= 0x109D) return true; // Khamti / Aiton
	if(ch == 0xA9E5) return true;
	if(ch >= 0xAA7B && ch <= 0xAA7D) return true;
	return false;
}
static inline bool IsClusterExtender(wchar_t ch, int script)
{
	if(IsJoiner(ch)) return true;
	if(ch >= 0x0300 && ch <= 0x036F) return true; // combining diacritical marks
	if(ch >= 0xFE00 && ch <= 0xFE0F) return true; // variation selectors
	switch(script)
	{
	case csMyanmar:
		return IsMyanmarExtender(ch);
	case csThai:
		return ch == 0x0E31 || (ch >= 0x0E33 && ch <= 0x0E3A) || (ch >= 0x0E47 && ch <= 0x0E4E);
	case csLao:
		return ch == 0x0EB1 || (ch >= 0x0EB3 && ch <= 0x0EBC) || (ch >= 0x0EC8 && ch <= 0x0ECD);
	case csKhmer:
		return (ch >= 0x17B4 && ch <= 0x17D3) || ch == 0x17DD;
	case csTibetan:
		return (ch >= 0x0F18 && ch <= 0x0F19) || ch == 0x0F35 || ch == 0x0F37 || ch == 0x0F39 ||
			(ch >= 0x0F3E && ch <= 0x0F3F) || (ch >= 0x0F71 && ch <= 0x0F84) ||
			(ch >= 0x0F86 && ch <= 0x0F87) || (ch >= 0x0F8D && ch <= 0x0FBC) || ch == 0x0FC6;
	case csTaiTham:
		return (ch >= 0x1A55 && ch <= 0x1A7F);
	default:
		break;
	}
	if(script >= csIndic)
	{
		// every Indic block shares the same layout: signs at +0x00..+0x03,
		// dependent vowels / virama at +0x3A..+0x57, more signs at +0x62..+0x63
		unsigned int ofs = (ch - 0x0900) & 0x7F;
		if(ofs <= 0x03) return true;
		if(ofs >= 0x3A && ofs <= 0x57 && ofs != 0x3D && ofs != 0x50) return true;
		if(ofs == 0x62 || ofs == 0x63) return true;
	}
	return false;
}
static inline bool IsStackingVirama(wchar_t ch, int script)
{
	// a virama that glues the following consonant into the same cluster
	switch(script)
	{
	case csMyanmar: return ch == 0x1039;
	case csKhmer:   return ch == 0x17D2;
	case csTaiTham: return ch == 0x1A60;
	default: break;
	}
	if(script >= csIndic) return ((ch - 0x0900) & 0x7F) == 0x4D;
	return false;
}
//---------------------------------------------------------------------------
static inline int ClusterLength(const wchar_t *text)
{
	if(!text || !text[0]) return 0;
	int len = 1;
	// keep surrogate pairs together
	if(text[0] >= 0xD800 && text[0] <= 0xDBFF && text[1] >= 0xDC00 && text[1] <= 0xDFFF) len = 2;

	int script = ComplexScriptOf(text[0]);
	if(script == csNone)
	{
		// plain text: only attach generic combining marks
		while(text[len] && (
			(text[len] >= 0x0300 && text[len] <= 0x036F) ||
			(text[len] >= 0xFE00 && text[len] <= 0xFE0F))) len++;
		return len;
	}

	for(;;)
	{
		wchar_t ch = text[len];
		if(!ch) break;
		if(IsStackingVirama(text[len-1], script) && ComplexScriptOf(ch) == script)
		{
			len++; // stacked / conjunct consonant
			continue;
		}
		if(IsClusterExtender(ch, script))
		{
			len++;
			continue;
		}
		if(script == csMyanmar && ch >= 0x1000 && ch <= 0x1021 &&
			text[len+1] == 0x103A && text[len+2] != 0x1039)
		{
			// consonant + asat closes the previous syllable (a "killed" final
			// consonant); keep it with the syllable so that a line never
			// starts with it.
			len++;
			continue;
		}
		break;
	}
	return len;
}

} // namespace TVPTextCluster

#endif
