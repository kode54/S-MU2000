// license:BSD-3-Clause
//
// S-MU2000: 機種ごとに違うところをここにまとめる。
//
// Windows（MinGW）と macOS の両方で作れるようにするための薄い層。
// 音の出方には一切関わらない（時計・待ち・端末の文字符号だけ）。

#ifndef S_MU2000_PLATFORM_H
#define S_MU2000_PLATFORM_H

#pragma once

#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#endif

namespace smu2000 {

// 回して待つ輪の中で CPU に「今は詰め込まなくていい」と教える。
// x86 は PAUSE。arm64 は ISB（YIELD は Apple の CPU でほぼ何もしない）
inline void cpu_relax()
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
	_mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
	__asm__ __volatile__("isb" ::: "memory");
#else
	// 知らない機械では何もしない
#endif
}

// 単調に進む時計。ナノ秒。Windows の QueryPerformanceCounter に相当する
inline std::uint64_t now_ns()
{
	using clock = std::chrono::steady_clock;
	return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                         clock::now().time_since_epoch())
	                         .count());
}

// 端末に UTF-8 を出せるようにする。macOS は既にそうなので何もしない。
// 中身は compat.cpp（windows.h をここへ持ち込まないため）
void console_utf8();

} // namespace smu2000

#endif
