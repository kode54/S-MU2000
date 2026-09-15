// license:BSD-3-Clause
//
// GDI の肩代わり（compat/gdicompat_mac.mm）で、実際にパネルの絵が描けるかを見る。
// 画面も音源も要らない。描いて PNG に落とすだけ。
//
//   build/paneltest out.png [横幅] [縦幅]

#include "compat/gdicompat.h"
#include "ui/panel.h"
#include "ui/snapshot.h"

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <CoreServices/CoreServices.h>

#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "panel.png";
	const int w = argc > 2 ? atoi(argv[2]) : 1400;
	const int h = argc > 3 ? atoi(argv[3]) : 560;

	ui::panel p;
	p.resize(w, h);
	const int W = p.width(), H = p.height();
	std::printf("パネル %d x %d\n", W, H);
	if (W <= 0 || H <= 0) { std::fprintf(stderr, "大きさが決まらない\n"); return 1; }

	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef cg = CGBitmapContextCreate(nullptr, W, H, 8, 0, cs,
	                                        kCGImageAlphaPremultipliedFirst |
	                                        kCGBitmapByteOrder32Little);
	CGColorSpaceRelease(cs);
	if (!cg) { std::fprintf(stderr, "器を作れない\n"); return 1; }

	// LCD に何か出しておく（点を市松に）
	ui::snapshot s{};
	for (size_t i = 0; i < sizeof(s.dots); i++)
		s.dots[i] = (i % 3) ? 0x15 : 0x0a;

	HDC dc = gdi_dc_from_cg(cg, H);
	p.paint(dc, s, 0, "gdicompat の試し描き");
	gdi_dc_release(dc);

	CGImageRef img = CGBitmapContextCreateImage(cg);
	CFStringRef path = CFStringCreateWithCString(nullptr, out, kCFStringEncodingUTF8);
	CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, path, kCFURLPOSIXPathStyle, false);
	CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
	int ok = 0;
	if (dst) {
		CGImageDestinationAddImage(dst, img, nullptr);
		ok = CGImageDestinationFinalize(dst);
		CFRelease(dst);
	}
	CFRelease(url); CFRelease(path);
	CGImageRelease(img); CGContextRelease(cg);
	std::printf(ok ? "書き出した: %s\n" : "書き出せない: %s\n", out);
	return ok ? 0 : 1;
}
