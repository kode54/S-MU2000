// license:BSD-3-Clause
//
// 文字の入れ替え。ソースは UTF-8 で書いてあるので、Windows へ渡すときは
// UTF-16 に直す。**A 付きの API（AppendMenuA など）に UTF-8 をそのまま
// 渡すと、CP932 と思われて文字化けする**。
//
// 機器の名前も同じで、W 付きの API から取って UTF-8 に直しておく。
// そうしておけば、こちらの中は全部 UTF-8 で揃う。

#ifndef S_MU2000_UI_TEXT_H
#define S_MU2000_UI_TEXT_H

#pragma once

#include <string>

#include "compat/gdicompat.h"

namespace ui {

inline std::wstring to_wide(const std::string &utf8)
{
	if (utf8.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()),
	                                  nullptr, 0);
	if (n <= 0)
		return {};
	std::wstring out(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
	return out;
}

inline std::string to_utf8(const wchar_t *w)
{
	if (!w || !*w)
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 1)
		return {};
	std::string out(size_t(n - 1), '\0');   // 終端は入れない
	WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
	return out;
}

} // namespace ui

#endif // S_MU2000_UI_TEXT_H
