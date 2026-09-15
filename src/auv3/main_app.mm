// license:BSD-3-Clause
//
// AUv3 を入れておくための器のアプリ。
//
// macOS は **アプリの中に入っている .appex しか AUv3 として認めない**ので、
// プラグインだけを配ることができない。このアプリ自体は音を出さない。
// 一度起動するとシステムが中の .appex を見つけ、DAW の一覧に出るようになる。
//
// 窓には ROM の置き場だけ出す（音が出ないときはたいていここ）。

#import <Cocoa/Cocoa.h>

#include "vst3/hostpaths.h"

#include <string>

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate {
	NSWindow *_window;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
	(void)note;

	// ROM が見つかるかどうかを、プラグインと同じやり方で確かめて見せる
	std::string tried;
	const std::string dir = smu2000::vst3::find_roms(tried);

	NSString *body;
	if (dir.empty()) {
		body = [NSString stringWithFormat:
		        @"ROM が見つかりません。\n\n"
		         "次のどれかに置いてください:\n%s\n"
		         "または環境変数 S_MU2000_ROMS で場所を渡します。\n\n"
		         "rom ディレクトリは tools/make_roms.py で組めます。",
		        tried.c_str()];
	} else {
		body = [NSString stringWithFormat:
		        @"ROM: %s\n\n"
		         "AudioUnit として登録されています。\n"
		         "DAW の音源の一覧に「S-MU2000: MU2000」が出ます。\n\n"
		         "  出力      MAIN OUT L/R\n"
		         "  入力      A/D INPUT（AD1 が左、AD2 が右）\n"
		         "  MIDI 入   ケーブル 0 = IN A（パート 1-16）\n"
		         "            ケーブル 1 = IN B（パート 17-32）\n"
		         "  MIDI 出   MIDI OUT（firmware の返事）",
		        dir.c_str()];
	}

	const NSRect frame = NSMakeRect(0, 0, 560, 340);
	_window = [[NSWindow alloc]
	    initWithContentRect:frame
	              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                         NSWindowStyleMaskMiniaturizable)
	                backing:NSBackingStoreBuffered
	                  defer:NO];
	_window.title = @"S-MU2000";
	[_window center];

	NSTextView *text = [[NSTextView alloc] initWithFrame:NSInsetRect(frame, 16, 16)];
	text.editable = NO;
	text.drawsBackground = NO;
	text.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
	text.string = body;

	NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSInsetRect(frame, 16, 16)];
	scroll.documentView = text;
	scroll.hasVerticalScroller = YES;
	scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	_window.contentView = scroll;

	[_window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
	(void)app;
	return YES;
}

@end

int main(int argc, const char *argv[])
{
	(void)argc; (void)argv;
	@autoreleasepool {
		NSApplication *app = [NSApplication sharedApplication];
		AppDelegate *del = [[AppDelegate alloc] init];
		app.delegate = del;
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];
		[app run];
	}
	return 0;
}
