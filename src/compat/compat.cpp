// license:BSD-3-Clause
//
// 互換層の実体。ログと、移植の突き合わせ用の命令追跡。

#include "mamecompat.h"
#include "platform.h"

#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace smu2000 {

// 端末に UTF-8 を出せるようにする。Windows の既定は CP932 で、
// このソースの表示は UTF-8 なので化ける。macOS は元から UTF-8
void console_utf8()
{
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif
}

bool g_verbose = false;

// 命令の追跡。MAME の debugger_instruction_hook に相当する。
// MAME 側にも同じものを入れてあるので、突き合わせて最初に食い違う命令を探せる。
//
//   g_pc_hash  ブロック（65536 命令）ごとに畳んだ値。どこで食い違うかを安く探す
//   g_pc_trace 生の PC 列。g_pc_skip で頭を飛ばし、g_pc_trace_left 命令ぶん出す
std::FILE *g_pc_trace = nullptr;
u64        g_pc_trace_left = 0;
u64        g_pc_skip = 0;
std::FILE *g_pc_hash = nullptr;
std::FILE *g_port_trace = nullptr;
u64        g_pc_cycles = 0;   // 追跡に添えるサイクル数
std::FILE *g_upd_trace = nullptr;

static u64 s_count = 0, s_h = 0;

void pc_hash(u32 pc, u64 regs)
{
	s_h = (s_h * 1000003 ^ pc) * 1000003 ^ regs;
	if (!(++s_count & 0xffff))
		std::fprintf(g_pc_hash, "%llu %016llx\n",
		             (unsigned long long)s_count, (unsigned long long)s_h);
}

void pc_trace(u32 pc, const char *regs)
{
	if (g_pc_skip) {
		g_pc_skip--;
		return;
	}
	if (!g_pc_trace_left) {
		g_pc_trace = nullptr;
		return;
	}
	g_pc_trace_left--;
	std::fprintf(g_pc_trace, "%08X C=%llu%s\n", pc, (unsigned long long)g_pc_cycles, regs);
}

} // namespace smu2000
