// license:BSD-3-Clause
//
// macOS の画面。実機のフロントパネルを出して、その場で鳴らす。
//
//   build/gui_mac <rom ディレクトリ> [--midi 番号] [--layout panel.txt] [--factory]
//
// 絵は ui::panel がそのまま描く（Windows 版と同じ .cpp）。GDI は
// compat/gdicompat_mac.mm が CoreGraphics で肩代わりしている。
// 音は ui::audio_out（CoreAudio）、MIDI は ui::midi_in（CoreMIDI）。
//
// **画面と音源は ui::bridge 越しにしか触れ合わない**（Windows 版と同じ）。
// 音源を回すのは音声の糸だけで、画面は写しを読んで描く。

#include "compat/gdicompat.h"

#include "mu2000.h"
#include "nvram.h"
#include "ui/audio_out.h"
#include "ui/bridge.h"
#include "ui/driver.h"
#include "ui/midi_in.h"
#include "ui/panel.h"
#include "ui/pc_editor.h"
#include "ui/pc_window.h"
#include "ui/overview.h"
#include "ui/fx_editor.h"
#include "ui/snapshot.h"

// **Quickdraw の Polygon と GDI の Polygon がぶつかる。**
// Cocoa は今でも Quickdraw のヘッダを引き込んでいて、あちらの Polygon は
// 構造体、こちらは関数なので同じ名前にできない。この翻訳単位の中だけ
// あちらの名前をずらす（Quickdraw そのものは使わない）
#define Polygon QuickdrawPolygon
#import <Cocoa/Cocoa.h>
#undef Polygon

#include <atomic>
#include <memory>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr u32 RATE = ui::AUDIO_RATE;

// ---- 音源側。gui.cpp の engine を、画面に要るところだけ持ってきたもの

struct engine {
	mu2000       mu;
	ui::bridge  &br;
	ui::midi_in &midi;
	ui::driver   drv;

	std::atomic<int>  state{0};      // 0 起動中 / 1 鳴らせる / 2 だめ
	std::atomic<bool> in_fill{false};
	std::string       message = "起動中...";
	bool              use_nvram = true;

	engine(ui::bridge &b, ui::midi_in &m) : br(b), midi(m) {}

	bool load(const std::string &dir)
	{
		if (!mu.load_program(dir + "/mu2000_flash.bin")) { message = mu.error(); return false; }
		if (!mu.load_wave(dir + "/dump"))                { message = mu.error(); return false; }
		if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
			std::fprintf(stderr, "警告: %s\n", mu.error().c_str());
		if (!mu.load_lcd_font(dir + "/hd44780u_b04.bin") &&
		    !mu.load_lcd_font(dir + "/standin/hd44780u_b04.bin"))
			std::fprintf(stderr, "警告: %s\n", mu.error().c_str());
		return true;
	}

	bool boot()
	{
		mu.set_threaded(true);
		if (use_nvram && smu2000::nvram::load(mu))
			std::printf("設定: %s\n", smu2000::nvram::path(mu).c_str());
		mu.reset();
		const size_t limit = size_t(30.0 * RATE);
		size_t i = 0;
		s32 l, r;
		for (; i < limit && !mu.midi_ready(); i++)
			mu.run_sample(l, r);
		if (i >= limit) {
			message = "起動しなかった";
			return false;
		}
		publish();
		return true;
	}

	void publish()
	{
		if (state.load() == 1)
			ui::driver::publish_now(mu, br, true, nullptr);
		else
			ui::driver::publish_message(br, message.c_str());
	}

	// 音声の糸から。n サンプルぶん作る（16bit 2ch インタリーブ）
	void fill(s16 *out, u32 n)
	{
		in_fill.store(true);
		if (state.load() != 1) {
			in_fill.store(false);
			std::memset(out, 0, size_t(n) * 4);
			return;
		}

		drv.apply_buttons(mu, br);
		drv.pump_midi(mu, br);
		drv.pump_wheel(mu, br);

		u8 b;
		while (midi.pop(b)) {
			mu.midi_in(b, 0);
			drv.watch(b, 0);
		}

		const float g = br.gain();
		for (u32 i = 0; i < n; i++) {
			s32 l = 0, r = 0;
			mu.run_sample(l, r);
			l = s32(l * g) * 32768 / mu2000::DAC_FULL_SCALE;
			r = s32(r * g) * 32768 / mu2000::DAC_FULL_SCALE;
			out[i * 2 + 0] = s16(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
			out[i * 2 + 1] = s16(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
		}
		drv.pump_out(mu, br);
		drv.publish(mu, br, n, RATE, true, nullptr);
		in_fill.store(false);
	}
};

engine        *g_eng   = nullptr;
ui::bridge    *g_br    = nullptr;
ui::panel     *g_panel = nullptr;
u64            g_pressed = 0;

// PC エディタの窓。閉じても壊さず隠すだけなので、開き直すと同じ姿で出る
ui::pc_window *g_pc   = nullptr;   // F2  パートの詳細
ui::pc_window *g_list = nullptr;   // F3  32 パートの一覧
ui::pc_window *g_fx   = nullptr;   // F4  インサーションの設定

void open_editor(ui::pc_window *w)
{
	if (!w)
		return;
	std::string err;
	if (!w->show(err))
		std::fprintf(stderr, "窓を出せない: %s\n", err.c_str());
}

} // namespace


// ---- パネルを描く view ---------------------------------------------------------

@interface PanelView : NSView
@end

@implementation PanelView

- (BOOL)isFlipped        { return YES; }   // 左上を原点に（GDI と同じ向き）
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque         { return YES; }

- (void)drawRect:(NSRect)dirty
{
	(void)dirty;
	if (!g_panel || !g_br)
		return;

	ui::snapshot s{};
	g_br->read(s);
	g_panel->tick(*g_br);

	const NSRect b = self.bounds;
	CGContextRef cg = (CGContextRef)NSGraphicsContext.currentContext.CGContext;

	// **isFlipped が YES なので、もう上下は返っている。**
	// gdi_dc_from_cg はさらに 1 度返すので、ここでは自前で戻しておく
	CGContextSaveGState(cg);
	CGContextTranslateCTM(cg, 0, b.size.height);
	CGContextScaleCTM(cg, 1, -1);

	HDC dc = gdi_dc_from_cg(cg, int(b.size.height));
	const char *status = "大きなダイヤルはホイールで回す / ボタンはクリック";
	g_panel->paint(dc, s, g_pressed, status);
	gdi_dc_release(dc);

	CGContextRestoreGState(cg);
}

// ---- 入力。panel がそのまま受け取る

- (NSPoint)panelPoint:(NSEvent *)e
{
	return [self convertPoint:e.locationInWindow fromView:nil];
}

- (void)mouseDown:(NSEvent *)e
{
	const NSPoint p = [self panelPoint:e];
	if (g_panel && g_panel->press(int(p.x), int(p.y), *g_br))
		self.needsDisplay = YES;
}

- (void)mouseDragged:(NSEvent *)e
{
	const NSPoint p = [self panelPoint:e];
	if (g_panel && g_panel->drag(int(p.x), int(p.y), *g_br))
		self.needsDisplay = YES;
}

- (void)mouseUp:(NSEvent *)e
{
	(void)e;
	if (g_panel && g_panel->release(*g_br))
		self.needsDisplay = YES;
}

- (void)keyDown:(NSEvent *)e
{
	// F2 / F3 / F4 で PC エディタの窓を出す（Windows 版と同じ割り当て）
	switch (e.keyCode) {
	case 120: open_editor(g_pc);   return;   // F2
	case 99:  open_editor(g_list); return;   // F3
	case 118: open_editor(g_fx);   return;   // F4
	default: break;
	}
	[super keyDown:e];
}

- (void)scrollWheel:(NSEvent *)e
{
	const NSPoint p = [self panelPoint:e];
	// 実機のロータリーエンコーダ。目盛りの数だけ回す
	int steps = int(e.scrollingDeltaY);
	if (e.hasPreciseScrollingDeltas)
		steps = int(e.scrollingDeltaY / 10.0);
	if (!steps)
		steps = e.scrollingDeltaY > 0 ? 1 : (e.scrollingDeltaY < 0 ? -1 : 0);
	if (steps && g_panel && g_panel->wheel_at(int(p.x), int(p.y), steps, *g_br))
		self.needsDisplay = YES;
}

@end


// ---- 窓 ------------------------------------------------------------------------

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSWindow *window;
@property (strong) PanelView *view;
@property (strong) NSTimer *timer;
@end

@implementation AppDelegate

- (void)tick:(NSTimer *)t
{
	(void)t;
	self.view.needsDisplay = YES;
	// PC エディタの窓も同じ拍で描く（見えていなければ何もしない）
	if (g_panel && g_br) {
		ui::xg_snapshot ram{};
		g_br->read_xg(ram);
		if (g_pc)   g_pc->frame(g_panel->xg(), ram, *g_br);
		if (g_list) g_list->frame(g_panel->xg(), ram, *g_br);
		if (g_fx)   g_fx->frame(g_panel->xg(), ram, *g_br);

		// Metal の道が通っているかを 1 度だけ知らせる
		static int frames = 0;
		if (g_pc && g_pc->visible() && ++frames == 60)
			std::printf("PC エディタ: Metal で 60 コマ描いた\n");
	}
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a
{
	(void)a;
	return YES;
}

@end


int main(int argc, char **argv)
{
	// 端末へ繋がっていないと丸ごと溜め込まれ、途中で止めたときに何も出ない
	std::setvbuf(stdout, nullptr, _IOLBF, 0);

	std::string dir, layout_path;
	int  midi_dev = -1;
	bool factory = false;
	const char *open_at_start = nullptr;   // 起動と同時に開く窓（pc / list / fx）
	const char *shot_path = nullptr;       // PC エディタを画面に出さず 1 枚撮って終わる
	int  win_w = 1400, win_h = 560;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) midi_dev = atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--layout") && i + 1 < argc) layout_path = argv[++i];
		else if (!std::strcmp(argv[i], "--factory")) factory = true;
		else if (!std::strcmp(argv[i], "--open") && i + 1 < argc) open_at_start = argv[++i];
		else if (!std::strcmp(argv[i], "--shot-editor") && i + 1 < argc) shot_path = argv[++i];
		else if (!std::strcmp(argv[i], "--size") && i + 1 < argc)
			std::sscanf(argv[++i], "%dx%d", &win_w, &win_h);
		else if (!std::strcmp(argv[i], "--list")) {
			std::printf("MIDI 入力:\n");
			const auto in = ui::midi_in::list();
			for (size_t k = 0; k < in.size(); k++)
				std::printf("  %zu: %s\n", k, in[k].c_str());
			std::printf("音声の出口:\n");
			const auto outs = ui::audio_out::list();
			for (size_t k = 0; k < outs.size(); k++)
				std::printf("  %zu: %s\n", k, outs[k].c_str());
			return 0;
		}
		else if (dir.empty()) dir = argv[i];
	}
	if (dir.empty()) {
		std::fprintf(stderr,
			"使い方: gui_mac <rom ディレクトリ> [--midi 番号] [--layout panel.txt]\n"
			"        [--size 1400x560] [--factory] [--open pc|list|fx] [--list]\n"
			"  F2 パートの詳細 / F3 32 パートの一覧 / F4 インサーション\n");
		return 1;
	}

	@autoreleasepool {
		static ui::bridge  br;
		static ui::midi_in midi;
		static ui::panel   panel;
		static engine      eng(br, midi);

		static ui::pc_window pc  { std::make_unique<ui::pc_editor>() };
		static ui::pc_window list{ std::make_unique<ui::overview>() };
		static ui::pc_window fx  { std::make_unique<ui::fx_editor>() };

		g_br = &br;
		g_panel = &panel;
		g_eng = &eng;
		g_pc = &pc; g_list = &list; g_fx = &fx;
		eng.use_nvram = !factory;

		if (!eng.load(dir)) {
			std::fprintf(stderr, "%s\n", eng.message.c_str());
			return 1;
		}
		if (!layout_path.empty()) {
			std::string lerr;
			if (panel.lay().load(layout_path, lerr))
				std::printf("配置: %s\n", layout_path.c_str());
			else
				std::printf("配置: %s を開けない。組み込みの配置を使う\n", layout_path.c_str());
			if (!lerr.empty())
				std::fprintf(stderr, "%s", lerr.c_str());
		}
		panel.resize(win_w, win_h);

		// MIDI 入力
		if (midi_dev < 0 && !ui::midi_in::list().empty())
			midi_dev = 0;
		if (midi_dev >= 0) {
			std::string merr;
			if (midi.open(midi_dev, merr))
				std::printf("MIDI 入力: %s\n", midi.device_name().c_str());
			else
				std::fprintf(stderr, "MIDI 入力 %d: %s\n", midi_dev, merr.c_str());
		}

		// 画面に出さずに PC エディタを 1 枚撮って終わる（Metal の道の確かめ）
		if (shot_path) {
			ui::xg_snapshot ram{};
			br.read_xg(ram);
			std::string serr;
			const bool ok = (open_at_start && !std::strcmp(open_at_start, "list") ? list
			                 : open_at_start && !std::strcmp(open_at_start, "fx") ? fx : pc)
			                .shot(shot_path, 0, 0, panel.xg(), ram, br, serr);
			std::printf(ok ? "撮った: %s\n" : "撮れない: %s\n", ok ? shot_path : serr.c_str());
			return ok ? 0 : 1;
		}

		// 起動は別の糸で。窓はすぐ出す
		std::thread boot_thread([] {
			g_eng->state.store(g_eng->boot() ? 1 : 2);
			g_eng->publish();
		});

		// 音を出す
		static ui::audio_out out;
		std::string aerr;
		if (!out.start(20, [](s16 *o, u32 n) { g_eng->fill(o, n); }, aerr))
			std::fprintf(stderr, "音が出せない: %s\n", aerr.c_str());
		else
			std::printf("音声の出口: %s\n", out.device_name().c_str());

		// 窓
		NSApplication *app = [NSApplication sharedApplication];
		AppDelegate *del = [[AppDelegate alloc] init];
		app.delegate = del;
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];

		const NSRect frame = NSMakeRect(0, 0, panel.width(), panel.height());
		NSWindow *w = [[NSWindow alloc]
		    initWithContentRect:frame
		              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
		                         NSWindowStyleMaskMiniaturizable)
		                backing:NSBackingStoreBuffered
		                  defer:NO];
		w.title = @"S-MU2000";
		[w center];
		PanelView *v = [[PanelView alloc] initWithFrame:frame];
		w.contentView = v;
		del.window = w;
		del.view = v;
		[w makeKeyAndOrderFront:nil];
		[w makeFirstResponder:v];
		[app activateIgnoringOtherApps:YES];

		if (open_at_start) {
			if (!std::strcmp(open_at_start, "pc"))   open_editor(g_pc);
			else if (!std::strcmp(open_at_start, "list")) open_editor(g_list);
			else if (!std::strcmp(open_at_start, "fx"))   open_editor(g_fx);
		}

		// 30 コマ／秒で描き直す
		del.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0
		                                             target:del
		                                           selector:@selector(tick:)
		                                           userInfo:nil
		                                            repeats:YES];

		[app run];

		pc.shutdown(br);
		list.shutdown(br);
		fx.shutdown(br);
		out.stop();
		midi.close();
		if (boot_thread.joinable())
			boot_thread.join();
		// 音は止まっている。ここで機械に触ってよい
		if (eng.state.load() == 1 && !smu2000::nvram::save(eng.mu))
			std::fprintf(stderr, "設定を残せなかった\n");
	}
	return 0;
}
