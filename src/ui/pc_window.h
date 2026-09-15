// license:BSD-3-Clause
//
// ImGui の画面（imgui_view）を載せる窓。
//   Windows  Win32 の窓に Direct3D 11 で描く（pc_window.cpp）
//   macOS    NSWindow の中の CAMetalLayer に Metal で描く（pc_window_mac.mm）
//
// 窓は gui の画面の糸で作り、gui のタイマーから frame() を呼んで描く。
// 閉じても消さずに隠すだけなので、開き直すと同じ状態で出る。
// **窓ごとに ImGui の文脈を持つ**ので、エディタと一覧を同時に開ける。

#ifndef S_MU2000_UI_PC_WINDOW_H
#define S_MU2000_UI_PC_WINDOW_H

#pragma once

#include "xg_ui.h"

#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
#endif

struct ImGuiContext;

namespace ui {

class pc_window
{
public:
	explicit pc_window(std::unique_ptr<imgui_view> view) : m_view(std::move(view)) {}
	~pc_window();

	// 出す。初めてなら窓と描画装置を作る。失敗したら err に理由
#ifdef _WIN32
	bool show(HINSTANCE inst, std::string &err);
#else
	bool show(std::string &err);
#endif
	bool visible() const;
	// gui を終えるとき。中身に「閉じた」と知らせる（ミュートを外すなど）
	void shutdown(bridge &br);

	// タイマーから。見えていなければ何もしない
	void frame(xg::model &m, const xg_snapshot &ram, bridge &br);

#ifndef _WIN32
	// 画面に出さずに 1 コマだけ描いて PNG に落とす（Metal の道の確かめ用）。
	// 窓は要らない。画面収録の許しも要らない
	bool shot(const char *path, int w, int h,
	          xg::model &m, const xg_snapshot &ram, bridge &br, std::string &err);
#endif

	// ファイルを窓に落とされたときに呼ぶ先（gui が MIDI ファイルを流す）。
	// 窓を作る前に決めておく。決めていなければ落とせない
	static void set_drop_handler(void (*fn)(const std::wstring &path)) { s_drop = fn; }

private:
	void destroy();
	static inline void (*s_drop)(const std::wstring &) = nullptr;

	std::unique_ptr<imgui_view> m_view;
	ImGuiContext           *m_imgui = nullptr;
	bool m_was_visible = false;              // 前のコマで見えていたか（隠れた瞬間を知る）

#ifdef _WIN32
	bool create(HINSTANCE inst, std::string &err);
	bool create_device(std::string &err);
	void make_target();
	void drop_target();
	static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

	HWND m_hwnd = nullptr;
	ID3D11Device           *m_dev = nullptr;
	ID3D11DeviceContext    *m_ctx = nullptr;
	IDXGISwapChain         *m_swap = nullptr;
	ID3D11RenderTargetView *m_rtv = nullptr;
	UINT m_resize_w = 0, m_resize_h = 0;     // WM_SIZE で受けて、次に描く前に直す
#else
	bool create(std::string &err);
	// Objective-C の物は型を出さずに持つ（このヘッダは .cpp からも読まれる）
	void *m_win   = nullptr;   // NSWindow
	void *m_mtk   = nullptr;   // 描く view（CAMetalLayer を持つ）
	void *m_dev   = nullptr;   // id<MTLDevice>
	void *m_queue = nullptr;   // id<MTLCommandQueue>
#endif
};

} // namespace ui

#endif // S_MU2000_UI_PC_WINDOW_H
