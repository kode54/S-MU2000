// license:BSD-3-Clause
//
// gdicompat.h の中身。CoreGraphics と CoreText で GDI のふりをする。
//
// **座標は GDI に合わせる。** GDI は左上が原点で y が下向き、CoreGraphics は
// 左下が原点で y が上向き。文脈を作るときに 1 度だけ上下を反転しておけば、
// 描く側（panel.cpp など）は今までどおりの座標で書ける。字も反転したままでは
// 裏返るので、文字を描くところで y をもう一度返す。

#include "gdicompat.h"

#ifndef _WIN32

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// ---- 道具 --------------------------------------------------------------------

struct gdi_obj {
	enum kind { brush, pen, font } what;
	bool     none = false;     // NULL_BRUSH / NULL_PEN。選ばれても描かない
	COLORREF color = 0;
	int      width = 1;        // ペンの太さ
	double   size  = 12.0;     // 書体の大きさ（px）
	int      weight = FW_NORMAL;
	std::string face;
	CTFontRef ct = nullptr;

	~gdi_obj() { if (ct) CFRelease(ct); }
};

struct gdi_dc {
	CGContextRef cg = nullptr;
	int   height = 0;
	bool  owns = false;

	HBRUSH brush = nullptr;
	HPEN   pen   = nullptr;
	HFONT  font  = nullptr;

	COLORREF text_color = 0;
	int      bk_mode = OPAQUE;
	int      fill_mode = ALTERNATE;
	POINT    cur{};
};

namespace {

void set_fill(CGContextRef cg, COLORREF c)
{
	CGContextSetRGBFillColor(cg, GetRValue(c) / 255.0, GetGValue(c) / 255.0,
	                         GetBValue(c) / 255.0, 1.0);
}

void set_stroke(CGContextRef cg, COLORREF c, int width)
{
	CGContextSetRGBStrokeColor(cg, GetRValue(c) / 255.0, GetGValue(c) / 255.0,
	                           GetBValue(c) / 255.0, 1.0);
	CGContextSetLineWidth(cg, width > 0 ? double(width) : 1.0);
}

// GDI の Rectangle / Ellipse などは「枠を塗って縁をなぞる」。
// 縁は線の中心が境界に乗るので、半ピクセルずらすと GDI に近い太さになる
CGRect to_cg(const RECT &r)
{
	return CGRectMake(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

void fill_and_stroke(gdi_dc *dc, CGPathRef path)
{
	if (dc->brush && !dc->brush->none) {
		set_fill(dc->cg, dc->brush->color);
		CGContextAddPath(dc->cg, path);
		CGContextFillPath(dc->cg);
	}
	if (dc->pen && !dc->pen->none) {
		set_stroke(dc->cg, dc->pen->color, dc->pen->width);
		CGContextAddPath(dc->cg, path);
		CGContextStrokePath(dc->cg);
	}
}

// 書体を用意する。Segoe UI は macOS に無いので、同じ役どころのものに置き換える
CTFontRef make_ct_font(const gdi_obj *f)
{
	CFStringRef name = CFSTR("Helvetica Neue");
	if (f->weight >= FW_BOLD)
		name = CFSTR("HelveticaNeue-Bold");
	CTFontRef ct = CTFontCreateWithName(name, f->size, nullptr);
	if (!ct)
		ct = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, f->size, nullptr);
	return ct;
}

} // namespace


// ---- 道具を作る・選ぶ・捨てる -------------------------------------------------

HBRUSH CreateSolidBrush(COLORREF c)
{
	gdi_obj *o = new gdi_obj;
	o->what = gdi_obj::brush;
	o->color = c;
	return o;
}

HPEN CreatePen(int, int width, COLORREF c)
{
	gdi_obj *o = new gdi_obj;
	o->what = gdi_obj::pen;
	o->color = c;
	o->width = width > 0 ? width : 1;
	return o;
}

HFONT CreateFontA(int height, int, int, int, int weight, DWORD, DWORD, DWORD, DWORD,
                  DWORD, DWORD, DWORD, DWORD, const char *face)
{
	gdi_obj *o = new gdi_obj;
	o->what = gdi_obj::font;
	// GDI は負の高さで「字そのものの高さ」を指す。こちらはどちらも px として扱う
	o->size = std::abs(double(height));
	if (o->size < 1.0)
		o->size = 12.0;
	o->weight = weight;
	o->face = face ? face : "";
	o->ct = make_ct_font(o);
	return o;
}

HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj)
{
	if (!dc || !obj)
		return nullptr;
	gdi_obj *old = nullptr;
	switch (obj->what) {
	case gdi_obj::brush: old = dc->brush; dc->brush = obj; break;
	case gdi_obj::pen:   old = dc->pen;   dc->pen   = obj; break;
	case gdi_obj::font:  old = dc->font;  dc->font  = obj; break;
	}
	return old;
}

int DeleteObject(HGDIOBJ obj)
{
	delete obj;
	return TRUE;
}

// ---- 描く -------------------------------------------------------------------

int FillRect(HDC dc, const RECT *r, HBRUSH b)
{
	if (!dc || !r || !b)
		return 0;
	set_fill(dc->cg, b->color);
	CGContextFillRect(dc->cg, to_cg(*r));
	return 1;
}

int Rectangle(HDC dc, int l, int t, int r, int b)
{
	if (!dc) return FALSE;
	CGPathRef p = CGPathCreateWithRect(CGRectMake(l, t, r - l, b - t), nullptr);
	fill_and_stroke(dc, p);
	CGPathRelease(p);
	return TRUE;
}

int RoundRect(HDC dc, int l, int t, int r, int b, int ew, int eh)
{
	if (!dc) return FALSE;
	const double rad = std::min({ double(ew) / 2.0, double(eh) / 2.0,
	                              double(r - l) / 2.0, double(b - t) / 2.0 });
	CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(l, t, r - l, b - t),
	                                          rad, rad, nullptr);
	fill_and_stroke(dc, p);
	CGPathRelease(p);
	return TRUE;
}

int Ellipse(HDC dc, int l, int t, int r, int b)
{
	if (!dc) return FALSE;
	CGPathRef p = CGPathCreateWithEllipseInRect(CGRectMake(l, t, r - l, b - t), nullptr);
	fill_and_stroke(dc, p);
	CGPathRelease(p);
	return TRUE;
}

int Polygon(HDC dc, const POINT *pts, int n)
{
	if (!dc || !pts || n < 2) return FALSE;
	CGMutablePathRef p = CGPathCreateMutable();
	CGPathMoveToPoint(p, nullptr, pts[0].x, pts[0].y);
	for (int i = 1; i < n; i++)
		CGPathAddLineToPoint(p, nullptr, pts[i].x, pts[i].y);
	CGPathCloseSubpath(p);
	fill_and_stroke(dc, p);
	CGPathRelease(p);
	return TRUE;
}

int Polyline(HDC dc, const POINT *pts, int n)
{
	if (!dc || !pts || n < 2) return FALSE;
	CGMutablePathRef p = CGPathCreateMutable();
	CGPathMoveToPoint(p, nullptr, pts[0].x, pts[0].y);
	for (int i = 1; i < n; i++)
		CGPathAddLineToPoint(p, nullptr, pts[i].x, pts[i].y);
	if (dc->pen && !dc->pen->none) {
		set_stroke(dc->cg, dc->pen->color, dc->pen->width);
		CGContextAddPath(dc->cg, p);
		CGContextStrokePath(dc->cg);
	}
	CGPathRelease(p);
	return TRUE;
}

int MoveToEx(HDC dc, int x, int y, POINT *old)
{
	if (!dc) return FALSE;
	if (old) *old = dc->cur;
	dc->cur.x = x;
	dc->cur.y = y;
	return TRUE;
}

int LineTo(HDC dc, int x, int y)
{
	if (!dc || !dc->pen || dc->pen->none) { if (dc) { dc->cur.x = x; dc->cur.y = y; } return FALSE; }
	set_stroke(dc->cg, dc->pen->color, dc->pen->width);
	CGContextBeginPath(dc->cg);
	CGContextMoveToPoint(dc->cg, dc->cur.x, dc->cur.y);
	CGContextAddLineToPoint(dc->cg, x, y);
	CGContextStrokePath(dc->cg);
	dc->cur.x = x;
	dc->cur.y = y;
	return TRUE;
}

int SetPixel(HDC dc, int x, int y, COLORREF c)
{
	if (!dc) return FALSE;
	set_fill(dc->cg, c);
	CGContextFillRect(dc->cg, CGRectMake(x, y, 1, 1));
	return TRUE;
}

COLORREF SetTextColor(HDC dc, COLORREF c)
{
	if (!dc) return 0;
	const COLORREF old = dc->text_color;
	dc->text_color = c;
	return old;
}

int SetBkMode(HDC dc, int mode)
{
	if (!dc) return 0;
	const int old = dc->bk_mode;
	dc->bk_mode = mode;
	return old;
}

// DrawTextW。使われている組み合わせだけを見る:
//   DT_LEFT / DT_CENTER / DT_RIGHT、DT_TOP / DT_VCENTER、DT_SINGLELINE、DT_WORDBREAK
int DrawTextW(HDC dc, const wchar_t *s, int len, RECT *r, UINT flags)
{
	if (!dc || !s || !r)
		return 0;
	if (len < 0) {
		len = 0;
		while (s[len]) len++;
	}
	if (!len)
		return 0;

	// wchar_t（macOS は 4 バイト）を UTF-16 へ
	std::vector<UniChar> u16;
	u16.reserve(size_t(len) * 2);
	for (int i = 0; i < len; i++) {
		const std::uint32_t cp = std::uint32_t(s[i]);
		if (cp < 0x10000) {
			u16.push_back(UniChar(cp));
		} else {
			const std::uint32_t v = cp - 0x10000;
			u16.push_back(UniChar(0xd800 + (v >> 10)));
			u16.push_back(UniChar(0xdc00 + (v & 0x3ff)));
		}
	}

	CTFontRef font = (dc->font && dc->font->ct) ? dc->font->ct : nullptr;
	CTFontRef tmp = nullptr;
	if (!font) {
		tmp = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 12.0, nullptr);
		font = tmp;
	}

	const COLORREF c = dc->text_color;
	CGColorRef col = CGColorCreateGenericRGB(GetRValue(c) / 255.0, GetGValue(c) / 255.0,
	                                         GetBValue(c) / 255.0, 1.0);
	CFStringRef keys[] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
	CFTypeRef   vals[] = { font, col };
	CFDictionaryRef attrs = CFDictionaryCreate(nullptr, (const void **)keys,
	                                           (const void **)vals, 2,
	                                           &kCFTypeDictionaryKeyCallBacks,
	                                           &kCFTypeDictionaryValueCallBacks);
	CFStringRef str = CFStringCreateWithCharacters(nullptr, u16.data(), CFIndex(u16.size()));
	CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, attrs);

	const double box_w = r->right - r->left;
	const double box_h = r->bottom - r->top;
	int used_h = 0;

	if (flags & DT_WORDBREAK) {
		// 折り返す。CTFramesetter に任せる
		CTFramesetterRef fs = CTFramesetterCreateWithAttributedString(as);
		CGSize fit = CTFramesetterSuggestFrameSizeWithConstraints(
		    fs, CFRangeMake(0, 0), nullptr, CGSizeMake(box_w, CGFLOAT_MAX), nullptr);
		used_h = int(std::ceil(fit.height));
		if (!(flags & DT_CALCRECT)) {
			double y = r->top;
			if (flags & DT_VCENTER) y += (box_h - fit.height) / 2.0;
			CGRect box = CGRectMake(r->left, y, box_w, fit.height);
			CGPathRef path = CGPathCreateWithRect(box, nullptr);
			CTFrameRef frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), path, nullptr);
			// 字は上下を戻して描く（文脈は反転したまま）
			CGContextSaveGState(dc->cg);
			CGContextTranslateCTM(dc->cg, 0, box.origin.y * 2 + box.size.height);
			CGContextScaleCTM(dc->cg, 1, -1);
			CTFrameDraw(frame, dc->cg);
			CGContextRestoreGState(dc->cg);
			CFRelease(frame);
			CGPathRelease(path);
		}
		CFRelease(fs);
	} else {
		CTLineRef line = CTLineCreateWithAttributedString(as);
		CGFloat asc = 0, desc = 0, lead = 0;
		const double w = CTLineGetTypographicBounds(line, &asc, &desc, &lead);
		used_h = int(std::ceil(asc + desc));
		if (!(flags & DT_CALCRECT)) {
			double x = r->left;
			if (flags & DT_CENTER)      x += (box_w - w) / 2.0;
			else if (flags & DT_RIGHT)  x += box_w - w;
			// GDI の DT_VCENTER は字の高さで真ん中に置く
			double top = r->top;
			if (flags & DT_VCENTER) top += (box_h - (asc + desc)) / 2.0;
			const double baseline = top + asc;

			CGContextSaveGState(dc->cg);
			CGContextTranslateCTM(dc->cg, 0, baseline * 2);
			CGContextScaleCTM(dc->cg, 1, -1);
			CGContextSetTextPosition(dc->cg, x, baseline);
			CTLineDraw(line, dc->cg);
			CGContextRestoreGState(dc->cg);
		}
		CFRelease(line);
	}

	if (flags & DT_CALCRECT)
		r->bottom = r->top + used_h;

	CFRelease(as);
	CFRelease(str);
	CFRelease(attrs);
	CGColorRelease(col);
	if (tmp) CFRelease(tmp);
	return used_h;
}

// ---- 文字の変換 --------------------------------------------------------------

int MultiByteToWideChar(UINT, DWORD, const char *in, int in_len, wchar_t *out, int out_len)
{
	if (!in)
		return 0;
	const size_t n = (in_len < 0) ? std::string(in).size() + 1 : size_t(in_len);
	std::vector<wchar_t> tmp;
	tmp.reserve(n);
	size_t i = 0;
	const size_t limit = (in_len < 0) ? n - 1 : n;
	while (i < limit) {
		const unsigned char c = in[i];
		std::uint32_t cp = c;
		int extra = 0;
		if      (c < 0x80) { extra = 0; cp = c; }
		else if ((c & 0xe0) == 0xc0) { extra = 1; cp = c & 0x1f; }
		else if ((c & 0xf0) == 0xe0) { extra = 2; cp = c & 0x0f; }
		else if ((c & 0xf8) == 0xf0) { extra = 3; cp = c & 0x07; }
		else { i++; continue; }
		if (i + extra >= limit + (in_len < 0 ? 1 : 0) && i + extra >= limit) {
			if (i + size_t(extra) >= limit) break;
		}
		for (int k = 0; k < extra; k++)
			cp = (cp << 6) | (std::uint32_t(in[i + 1 + k]) & 0x3f);
		i += size_t(extra) + 1;
		tmp.push_back(wchar_t(cp));
	}
	if (in_len < 0)
		tmp.push_back(L'\0');

	if (!out || out_len <= 0)
		return int(tmp.size());
	const int n_out = int(std::min<size_t>(tmp.size(), size_t(out_len)));
	for (int k = 0; k < n_out; k++)
		out[k] = tmp[size_t(k)];
	return n_out;
}

int WideCharToMultiByte(UINT, DWORD, const wchar_t *in, int in_len, char *out, int out_len,
                        const char *, int *)
{
	if (!in)
		return 0;
	size_t n = 0;
	if (in_len < 0) { while (in[n]) n++; n++; }
	else            { n = size_t(in_len); }

	std::string tmp;
	for (size_t i = 0; i < n; i++) {
		const std::uint32_t cp = std::uint32_t(in[i]);
		if (cp < 0x80) {
			tmp.push_back(char(cp));
		} else if (cp < 0x800) {
			tmp.push_back(char(0xc0 | (cp >> 6)));
			tmp.push_back(char(0x80 | (cp & 0x3f)));
		} else if (cp < 0x10000) {
			tmp.push_back(char(0xe0 | (cp >> 12)));
			tmp.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
			tmp.push_back(char(0x80 | (cp & 0x3f)));
		} else {
			tmp.push_back(char(0xf0 | (cp >> 18)));
			tmp.push_back(char(0x80 | ((cp >> 12) & 0x3f)));
			tmp.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
			tmp.push_back(char(0x80 | (cp & 0x3f)));
		}
	}
	if (!out || out_len <= 0)
		return int(tmp.size());
	const int n_out = int(std::min<size_t>(tmp.size(), size_t(out_len)));
	std::copy(tmp.begin(), tmp.begin() + n_out, out);
	return n_out;
}

// ---- 足りていなかったもの ----------------------------------------------------

HGDIOBJ GetStockObject(int which)
{
	// 使い回す。捨てられても困らないよう DeleteObject では消さない
	static gdi_obj null_brush = [] { gdi_obj o; o.what = gdi_obj::brush; o.none = true; return o; }();
	static gdi_obj null_pen   = [] { gdi_obj o; o.what = gdi_obj::pen;   o.none = true; return o; }();
	static gdi_obj white      = [] { gdi_obj o; o.what = gdi_obj::brush; o.color = RGB(255,255,255); return o; }();
	static gdi_obj black      = [] { gdi_obj o; o.what = gdi_obj::brush; o.color = RGB(0,0,0); return o; }();
	switch (which) {
	case NULL_BRUSH:  return &null_brush;
	case NULL_PEN:    return &null_pen;
	case WHITE_BRUSH: return &white;
	case BLACK_BRUSH: return &black;
	default:          return &null_brush;
	}
}

int InflateRect(RECT *r, int dx, int dy)
{
	if (!r) return FALSE;
	r->left -= dx; r->right  += dx;
	r->top  -= dy; r->bottom += dy;
	return TRUE;
}

int SetPolyFillMode(HDC dc, int mode)
{
	if (!dc) return 0;
	const int old = dc->fill_mode;
	dc->fill_mode = mode;
	return old;
}

// 穴あきの図形。counts はそれぞれの輪の点数
int PolyPolygon(HDC dc, const POINT *pts, const INT *counts, int n)
{
	if (!dc || !pts || !counts || n <= 0) return FALSE;
	CGMutablePathRef p = CGPathCreateMutable();
	int at = 0;
	for (int i = 0; i < n; i++) {
		const int m = counts[i];
		if (m >= 2) {
			CGPathMoveToPoint(p, nullptr, pts[at].x, pts[at].y);
			for (int k = 1; k < m; k++)
				CGPathAddLineToPoint(p, nullptr, pts[at + k].x, pts[at + k].y);
			CGPathCloseSubpath(p);
		}
		at += m;
	}
	if (dc->brush && !dc->brush->none) {
		set_fill(dc->cg, dc->brush->color);
		CGContextAddPath(dc->cg, p);
		// ALTERNATE は偶奇、WINDING は巻き数
		if (dc->fill_mode == WINDING) CGContextFillPath(dc->cg);
		else                          CGContextEOFillPath(dc->cg);
	}
	if (dc->pen && !dc->pen->none) {
		set_stroke(dc->cg, dc->pen->color, dc->pen->width);
		CGContextAddPath(dc->cg, p);
		CGContextStrokePath(dc->cg);
	}
	CGPathRelease(p);
	return TRUE;
}

// 楕円の弧。GDI は (x1,y1) から (x2,y2) へ「画面の上で反時計回り」に描く。
// 点は中心からの向きだけを見る（楕円の上に落とす）。
// CoreGraphics の回り方の決まりに迷わないよう、点を並べて線で結ぶ
int Arc(HDC dc, int l, int t, int r, int b, int x1, int y1, int x2, int y2)
{
	if (!dc || !dc->pen || dc->pen->none) return FALSE;
	const double cx = (l + r) / 2.0, cy = (t + b) / 2.0;
	const double rx = (r - l) / 2.0,  ry = (b - t) / 2.0;
	if (rx <= 0.0 || ry <= 0.0) return FALSE;

	// y は下向きのまま測る。画面の上での反時計回りは、この角の減る向き
	auto ang = [&](double x, double y) { return std::atan2((y - cy) / ry, (x - cx) / rx); };
	double a1 = ang(x1, y1), a2 = ang(x2, y2);
	while (a2 > a1) a2 -= 2.0 * M_PI;
	if (a1 - a2 < 1e-9) a2 = a1 - 2.0 * M_PI;   // 同じ点なら 1 周

	set_stroke(dc->cg, dc->pen->color, dc->pen->width);
	CGContextBeginPath(dc->cg);
	const int steps = std::max(8, int((a1 - a2) / (2.0 * M_PI) * 96.0) + 8);
	for (int i = 0; i <= steps; i++) {
		const double a = a1 + (a2 - a1) * (double(i) / steps);
		const double px = cx + rx * std::cos(a), py = cy + ry * std::sin(a);
		if (i == 0) CGContextMoveToPoint(dc->cg, px, py);
		else        CGContextAddLineToPoint(dc->cg, px, py);
	}
	CGContextStrokePath(dc->cg);
	return TRUE;
}

DWORD GetModuleFileNameA(void *, char *out, DWORD n)
{
	if (!out || !n) return 0;
	Dl_info info{};
	if (!dladdr(reinterpret_cast<const void *>(&GetModuleFileNameA), &info) || !info.dli_fname)
		return 0;
	std::string s(info.dli_fname);
	const DWORD len = DWORD(std::min<size_t>(s.size(), size_t(n) - 1));
	std::copy(s.begin(), s.begin() + len, out);
	out[len] = 0;
	return len;
}

// ---- 文脈 -------------------------------------------------------------------

HDC gdi_dc_from_cg(void *cg_context, int height)
{
	gdi_dc *dc = new gdi_dc;
	dc->cg = static_cast<CGContextRef>(cg_context);
	dc->height = height;
	// GDI と同じ向きにする（左上が原点、y は下向き）
	CGContextTranslateCTM(dc->cg, 0, height);
	CGContextScaleCTM(dc->cg, 1, -1);
	CGContextSetShouldAntialias(dc->cg, true);
	return dc;
}

void gdi_dc_release(HDC dc)
{
	delete dc;
}

#endif // _WIN32
