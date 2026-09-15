// license:BSD-3-Clause
//
// pc_window の macOS 版。NSWindow の中に CAMetalLayer を持つ view を置き、
// ImGui を osx + metal の backend で描く（pc_window.cpp の Direct3D 11 と対）。
//
// **窓ごとに ImGui の文脈を持つ。** エディタと一覧を同時に開けるようにするため。
// 描くたびに ImGui::SetCurrentContext() で切り替える。
//
// 閉じても壊さず隠すだけなので、開き直すと同じ状態で出る（Windows 版と同じ）。

#include "pc_window.h"

#include "imgui.h"
#include "backends/imgui_impl_metal.h"
#include "backends/imgui_impl_osx.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

// 日本語の出る字。macOS に入っているものを順に探す（配らない）。
//
// **ImGui は字の取りこぼしを肩代わりしてくれない。** CoreText は無い字を
// 別の書体から拾ってくるが（パネルの側はそれで日本語が出ている）、ImGui は
// 図柄帳に無ければそのまま「?」になる。だから日本語の入った書体を渡す。
// 1.92 の図柄帳は使う字をその場で足すので、範囲の指定は要らない
const char *const FONTS[] = {
	"/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
	"/System/Library/Fonts/Hiragino Sans GB.ttc",
	"/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
	"/Library/Fonts/Arial Unicode.ttf",
};

bool file_exists(const char *path)
{
	return [[NSFileManager defaultManager] fileExistsAtPath:@(path)];
}

// 日本語の出る字を図柄帳へ入れる。見つからなければ既定のまま（英数字だけ）
void add_japanese_font(ImGuiIO &io, float px)
{
	for (const char *path : FONTS)
		if (file_exists(path) && io.Fonts->AddFontFromFileTTF(path, px))
			return;
	std::fprintf(stderr, "日本語の書体が見つからない。PC エディタの日本語は ? になる\n");
}

// UTF-16 の題を NSString に（imgui_view::title() は wchar_t を返す）
NSString *to_ns(const wchar_t *s)
{
	if (!s)
		return @"";
	std::vector<unichar> u16;
	for (const wchar_t *p = s; *p; p++) {
		const uint32_t cp = uint32_t(*p);
		if (cp < 0x10000) {
			u16.push_back(unichar(cp));
		} else {
			const uint32_t v = cp - 0x10000;
			u16.push_back(unichar(0xd800 + (v >> 10)));
			u16.push_back(unichar(0xdc00 + (v & 0x3ff)));
		}
	}
	return [NSString stringWithCharacters:u16.data() length:u16.size()];
}

} // namespace


// 描く場所。CAMetalLayer を自分の裏地にする
@interface MetalPane : NSView
@property (nonatomic, strong) id<MTLDevice> device;
@end

@implementation MetalPane

+ (Class)layerClass { return [CAMetalLayer class]; }

- (CALayer *)makeBackingLayer
{
	CAMetalLayer *l = [CAMetalLayer layer];
	l.device = self.device;
	l.pixelFormat = MTLPixelFormatBGRA8Unorm;
	l.framebufferOnly = YES;
	return l;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer      { return YES; }

// 画面の細かさが変わったら（別のモニタへ移したときなど）裏地の大きさも直す
- (void)viewDidChangeBackingProperties
{
	[super viewDidChangeBackingProperties];
	CAMetalLayer *l = (CAMetalLayer *)self.layer;
	l.contentsScale = self.window ? self.window.backingScaleFactor : 1.0;
}

@end


// 閉じるボタンで壊さず隠す。Windows 版と同じ振る舞い
@interface PCWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation PCWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)w
{
	[w orderOut:nil];
	return NO;
}
@end


namespace ui {

pc_window::~pc_window()
{
	destroy();
}

bool pc_window::create(std::string &err)
{
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	if (!dev) {
		err = "Metal の装置が無い";
		return false;
	}
	id<MTLCommandQueue> queue = [dev newCommandQueue];
	if (!queue) {
		err = "Metal の待ち行列を作れない";
		return false;
	}

	const int w = m_view->default_width(), h = m_view->default_height();
	const NSRect frame = NSMakeRect(0, 0, w > 0 ? w : 900, h > 0 ? h : 600);

	NSWindow *win = [[NSWindow alloc]
	    initWithContentRect:frame
	              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                         NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
	                backing:NSBackingStoreBuffered
	                  defer:NO];
	win.title = to_ns(m_view->title());
	win.releasedWhenClosed = NO;          // 隠すだけなので、閉じても壊さない
	[win center];

	MetalPane *pane = [[MetalPane alloc] initWithFrame:frame];
	pane.device = dev;
	pane.wantsLayer = YES;
	((CAMetalLayer *)pane.layer).device = dev;
	((CAMetalLayer *)pane.layer).contentsScale = win.backingScaleFactor;
	pane.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	win.contentView = pane;

	static PCWindowDelegate *del = [[PCWindowDelegate alloc] init];
	win.delegate = del;

	// **窓ごとに ImGui の文脈を持つ。** 同時に 2 つ開いても混ざらない。
	//
	// ただし **文脈を空のまま放り出してはいけない。** imgui_impl_osx は view に
	// 自前の応答役を差し込み、こちらの描く番でないときにも（鍵や文字が来たとき）
	// ImGui::GetIO() を呼ぶ。そのとき現在の文脈が無いと "No current context" で
	// 落ちる。だから作ったらそのまま現在の文脈にしておき、戻さない
	IMGUI_CHECKVERSION();
	m_imgui = ImGui::CreateContext();
	ImGui::SetCurrentContext(m_imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;             // 置き場所を汚さない
	ImGui::StyleColorsDark();
	ImGui::GetStyle().FrameRounding = 3;
	add_japanese_font(io, 16.0f);
	if (!ImGui_ImplOSX_Init(pane) || !ImGui_ImplMetal_Init(dev)) {
		ImGui::DestroyContext(m_imgui);
		m_imgui = nullptr;
		ImGui::SetCurrentContext(nullptr);
		err = "ImGui の backend を始められない";
		return false;
	}

	m_win   = (__bridge_retained void *)win;
	m_mtk   = (__bridge_retained void *)pane;
	m_dev   = (__bridge_retained void *)dev;
	m_queue = (__bridge_retained void *)queue;
	return true;
}

bool pc_window::show(std::string &err)
{
	if (!m_win && !create(err))
		return false;
	NSWindow *win = (__bridge NSWindow *)m_win;
	[win makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];
	m_was_visible = true;
	return true;
}

bool pc_window::visible() const
{
	if (!m_win)
		return false;
	NSWindow *win = (__bridge NSWindow *)m_win;
	return win.isVisible ? true : false;
}

void pc_window::shutdown(bridge &br)
{
	if (m_view && m_was_visible)
		m_view->hidden(br);
	m_was_visible = false;
	destroy();
}

void pc_window::destroy()
{
	if (m_imgui) {
		ImGuiContext *prev = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(m_imgui);
		ImGui_ImplMetal_Shutdown();
		ImGui_ImplOSX_Shutdown();
		ImGui::DestroyContext(m_imgui);
		// 自分を消したなら、現在の文脈は空になる。他の窓のものなら戻す
		ImGui::SetCurrentContext(prev == m_imgui ? nullptr : prev);
		m_imgui = nullptr;
	}
	if (m_win) {
		NSWindow *win = (__bridge_transfer NSWindow *)m_win;
		[win orderOut:nil];
		win.delegate = nil;
		m_win = nullptr;
	}
	if (m_mtk)   { (void)(__bridge_transfer MetalPane *)m_mtk;          m_mtk = nullptr; }
	if (m_dev)   { (void)(__bridge_transfer id<MTLDevice>)m_dev;        m_dev = nullptr; }
	if (m_queue) { (void)(__bridge_transfer id<MTLCommandQueue>)m_queue; m_queue = nullptr; }
}

// gui のタイマーから。見えていなければ何もしない
void pc_window::frame(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	if (!m_win || !m_imgui)
		return;

	NSWindow *win = (__bridge NSWindow *)m_win;
	if (!win.isVisible) {
		// 隠れた瞬間だけ中身に知らせる（鳴らしている音を止めるなど）
		if (m_was_visible && m_view)
			m_view->hidden(br);
		m_was_visible = false;
		return;
	}
	m_was_visible = true;

	MetalPane *pane = (__bridge MetalPane *)m_mtk;
	CAMetalLayer *layer = (CAMetalLayer *)pane.layer;
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_queue;

	// 裏地の大きさを view に合わせる（窓を掴んで変えられる）
	const CGSize want = CGSizeMake(pane.bounds.size.width  * layer.contentsScale,
	                               pane.bounds.size.height * layer.contentsScale);
	if (want.width > 0 && want.height > 0 &&
	    (layer.drawableSize.width != want.width || layer.drawableSize.height != want.height))
		layer.drawableSize = want;

	id<CAMetalDrawable> drawable = [layer nextDrawable];
	if (!drawable)
		return;

	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = drawable.texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.10, 0.10, 0.11, 1.0);

	ImGui::SetCurrentContext(m_imgui);

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(float(pane.bounds.size.width), float(pane.bounds.size.height));
	io.DisplayFramebufferScale = ImVec2(float(layer.contentsScale), float(layer.contentsScale));

	ImGui_ImplMetal_NewFrame(pass);
	ImGui_ImplOSX_NewFrame(pane);
	ImGui::NewFrame();

	m_view->draw(m, ram, br);

	ImGui::Render();
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
	ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmd, enc);
	[enc endEncoding];

	// 文脈は戻さない（上の理由）。次に描く窓が自分で付け替える

	[cmd presentDrawable:drawable];
	[cmd commit];
}

} // namespace ui

// ---- 画面に出さずに 1 コマ描いて PNG に落とす -----------------------------------
//
// 窓も画面収録の許しも要らない。Metal の中で描いて、texture を読み戻すだけ。
// paneltest がパネルに対してやっているのと同じことを、PC エディタに対してやる。

#import <ImageIO/ImageIO.h>

namespace ui {

bool pc_window::shot(const char *path, int w, int h,
                     xg::model &m, const xg_snapshot &ram, bridge &br, std::string &err)
{
	if (w <= 0) w = m_view->default_width();
	if (h <= 0) h = m_view->default_height();
	if (w <= 0) w = 900;
	if (h <= 0) h = 600;

	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	if (!dev) { err = "Metal の装置が無い"; return false; }
	id<MTLCommandQueue> queue = [dev newCommandQueue];

	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                       width:w height:h mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeManaged;
	id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
	if (!tex) { err = "texture を作れない"; return false; }

	// この撮影だけの文脈。窓に紐づく方は触らない
	ImGuiContext *prev = ImGui::GetCurrentContext();
	ImGuiContext *ctx = ImGui::CreateContext();
	ImGui::SetCurrentContext(ctx);
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::GetIO().DisplaySize = ImVec2(float(w), float(h));
	ImGui::GetIO().DisplayFramebufferScale = ImVec2(1, 1);
	ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
	ImGui::StyleColorsDark();
	add_japanese_font(ImGui::GetIO(), 16.0f);
	if (!ImGui_ImplMetal_Init(dev)) {
		ImGui::DestroyContext(ctx);
		ImGui::SetCurrentContext(prev);
		err = "ImGui の metal backend を始められない";
		return false;
	}

	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = tex;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.10, 0.10, 0.11, 1.0);

	// 2 コマ描く。ImGui は 1 コマ目で大きさを決めるものがあるので
	for (int pass_i = 0; pass_i < 2; pass_i++) {
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		ImGui_ImplMetal_NewFrame(pass);
		ImGui::NewFrame();
		m_view->draw(m, ram, br);
		ImGui::Render();
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
		ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmd, enc);
		[enc endEncoding];
		if (pass_i == 1) {
			id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
			[blit synchronizeResource:tex];
			[blit endEncoding];
		}
		[cmd commit];
		[cmd waitUntilCompleted];
	}

	ImGui_ImplMetal_Shutdown();
	ImGui::DestroyContext(ctx);
	ImGui::SetCurrentContext(prev == ctx ? nullptr : prev);

	// 読み戻して PNG へ
	std::vector<uint8_t> px(size_t(w) * h * 4);
	[tex getBytes:px.data() bytesPerRow:size_t(w) * 4
	   fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];

	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef bmp = CGBitmapContextCreate(px.data(), w, h, 8, size_t(w) * 4, cs,
	                                         kCGImageAlphaPremultipliedFirst |
	                                         kCGBitmapByteOrder32Little);
	CGColorSpaceRelease(cs);
	if (!bmp) { err = "器を作れない"; return false; }
	CGImageRef img = CGBitmapContextCreateImage(bmp);
	CFStringRef p = CFStringCreateWithCString(nullptr, path, kCFStringEncodingUTF8);
	CFURLRef u = CFURLCreateWithFileSystemPath(nullptr, p, kCFURLPOSIXPathStyle, false);
	CGImageDestinationRef d = CGImageDestinationCreateWithURL(u, CFSTR("public.png"), 1, nullptr);
	bool ok = false;
	if (d) {
		CGImageDestinationAddImage(d, img, nullptr);
		ok = CGImageDestinationFinalize(d);
		CFRelease(d);
	}
	CFRelease(u); CFRelease(p);
	CGImageRelease(img); CGContextRelease(bmp);
	if (!ok) err = "PNG を書けない";
	return ok;
}

} // namespace ui
