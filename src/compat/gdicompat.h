// license:BSD-3-Clause
//
// GDI を肩代わりする最小限の層（macOS）。
//
// パネル・エディタ・エフェクトの画面は GDI で描いてある（src/ui/draw.h と
// src/ui/panel.cpp）。絵の組み立て方そのものは機種に関係がないので、
// **描く .cpp には一切手を触れず**、GDI の側をこちらで用意する。
// mamecompat.h が MAME に対してやっているのと同じ考え方。
//
// 方針:
//   - 実際に使われているものだけを作る（下の表がすべて）
//   - 中身は CoreGraphics と CoreText。HDC は CGContextRef に被せた入れ物
//   - 座標は GDI と同じく「左上が原点、y は下向き」にする。CoreGraphics は
//     下向きが正なので、文脈側で 1 度だけ上下を反転させておく
//
// Windows では windows.h がそのまま使われる。このヘッダは macOS でしか要らない。

#ifndef S_MU2000_GDICOMPAT_H
#define S_MU2000_GDICOMPAT_H

#pragma once

#ifdef _WIN32
#include <windows.h>
#else

#include <cstdint>
#include <cstddef>

// ---- 型 ---------------------------------------------------------------------

// LONG は Windows と同じく long にしておく。描く側に std::max(1L, r.right - r.left)
// のような書き方があり、int32_t にすると型が食い違って通らない
using LONG     = long;
using INT      = int;
using UINT     = std::uint32_t;
using DWORD    = std::uint32_t;
using BYTE     = std::uint8_t;
using COLORREF = std::uint32_t;

// BOOL は作らない。Objective-C の BOOL とぶつかる（あちらは bool、GDI は int）。
// この層の返り値は素の int にしてある
#ifndef FALSE
#define FALSE 0
#define TRUE  1
#endif

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

struct RECT  { LONG left = 0, top = 0, right = 0, bottom = 0; };
struct POINT { LONG x = 0, y = 0; };

// GDI と同じ並び（下位から R, G, B）
#define RGB(r, g, b) (COLORREF((BYTE(r)) | (UINT(BYTE(g)) << 8) | (UINT(BYTE(b)) << 16)))
#define GetRValue(c)  BYTE((c) & 0xff)
#define GetGValue(c)  BYTE(((c) >> 8) & 0xff)
#define GetBValue(c)  BYTE(((c) >> 16) & 0xff)

// 描く道具。中身は gdicompat_mac.mm にある
struct gdi_dc;
struct gdi_obj;
using HDC     = gdi_dc *;
using HGDIOBJ = gdi_obj *;
using HBRUSH  = gdi_obj *;
using HPEN    = gdi_obj *;
using HFONT   = gdi_obj *;

// ---- 定数（使っているものだけ） ----------------------------------------------

enum : UINT {
	DT_LEFT       = 0x0000,
	DT_CENTER     = 0x0001,
	DT_RIGHT      = 0x0002,
	DT_TOP        = 0x0000,
	DT_VCENTER    = 0x0004,
	DT_BOTTOM     = 0x0008,
	DT_WORDBREAK  = 0x0010,
	DT_SINGLELINE = 0x0020,
	DT_CALCRECT   = 0x0400,
	DT_NOPREFIX   = 0x0800,
};

enum : int {
	PS_SOLID = 0,

	TRANSPARENT = 1,
	OPAQUE      = 2,

	FW_NORMAL = 400,
	FW_BOLD   = 700,

	DEFAULT_CHARSET     = 1,
	OUT_TT_PRECIS       = 4,
	CLIP_DEFAULT_PRECIS = 0,
	CLEARTYPE_QUALITY   = 5,
	ANTIALIASED_QUALITY = 4,
	VARIABLE_PITCH      = 2,
	FF_SWISS            = 32,

	ALTERNATE = 1,
	WINDING   = 2,

	// GetStockObject で引く「何もしない」道具
	NULL_BRUSH = 5,
	NULL_PEN   = 8,
	WHITE_BRUSH = 0,
	BLACK_BRUSH = 4,
};

enum : UINT { CP_UTF8 = 65001 };

// ---- 道具を作る・選ぶ・捨てる -------------------------------------------------

HBRUSH  CreateSolidBrush(COLORREF c);
HPEN    CreatePen(int style, int width, COLORREF c);
// 使っているのは高さ・太さ・書体名だけ。ほかの引数は形を合わせるためにある
HFONT   CreateFontA(int height, int width, int escapement, int orientation, int weight,
                    DWORD italic, DWORD underline, DWORD strikeout, DWORD charset,
                    DWORD out_precision, DWORD clip_precision, DWORD quality,
                    DWORD pitch_and_family, const char *face);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj);
int     DeleteObject(HGDIOBJ obj);
HGDIOBJ GetStockObject(int which);

// ---- 描く -------------------------------------------------------------------

int FillRect(HDC dc, const RECT *r, HBRUSH b);
int Rectangle(HDC dc, int l, int t, int rr, int b);
int RoundRect(HDC dc, int l, int t, int rr, int b, int ew, int eh);
int Ellipse(HDC dc, int l, int t, int rr, int b);
int Polygon(HDC dc, const POINT *pts, int n);
int Polyline(HDC dc, const POINT *pts, int n);
int PolyPolygon(HDC dc, const POINT *pts, const INT *counts, int n);
int SetPolyFillMode(HDC dc, int mode);
// 楕円の弧。(x1,y1) から (x2,y2) へ、画面の上で反時計回りに
int Arc(HDC dc, int l, int t, int rr, int b, int x1, int y1, int x2, int y2);
int MoveToEx(HDC dc, int x, int y, POINT *old);
int LineTo(HDC dc, int x, int y);
int SetPixel(HDC dc, int x, int y, COLORREF c);
int InflateRect(RECT *r, int dx, int dy);

COLORREF SetTextColor(HDC dc, COLORREF c);
int      SetBkMode(HDC dc, int mode);
int      DrawTextW(HDC dc, const wchar_t *s, int len, RECT *r, UINT flags);

// ---- 文字の変換（CP_UTF8 のときだけ） -----------------------------------------

int MultiByteToWideChar(UINT cp, DWORD flags, const char *in, int in_len,
                        wchar_t *out, int out_len);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *in, int in_len,
                        char *out, int out_len, const char *def, int *used_def);

// 自分の実行ファイルの道。layout.cpp が絵の置き場を探すのに使う
DWORD GetModuleFileNameA(void *module, char *out, DWORD n);

// ---- 文脈を作る（こちら側だけの口。gui の macOS 版が使う） ---------------------

// CGContextRef に被せる。dc は使い終わったら gdi_dc_release で捨てる。
// height は上下の反転に要る（GDI は左上が原点）
HDC  gdi_dc_from_cg(void *cg_context, int height);
void gdi_dc_release(HDC dc);

#endif // _WIN32

#endif // S_MU2000_GDICOMPAT_H
