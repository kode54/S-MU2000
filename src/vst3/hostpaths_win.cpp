// license:BSD-3-Clause
//
// hostpaths.h の Windows 版。engine.cpp から切り出したもの（中身は変えていない）。

#include "hostpaths.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace smu2000 {
namespace vst3 {


// ---- プラグイン本体（DLL）の置かれている場所

static std::string module_dir()
{
	HMODULE self = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCSTR>(&module_dir), &self))
		return {};
	char buf[MAX_PATH * 2] = {};
	const DWORD n = GetModuleFileNameA(self, buf, sizeof(buf));
	if (!n || n >= sizeof(buf))
		return {};
	std::string s(buf, n);
	const size_t slash = s.find_last_of("\\/");
	return slash == std::string::npos ? std::string() : s.substr(0, slash);
}

static std::string env(const char *name)
{
	char buf[MAX_PATH * 4];
	const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
	return (n && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}

static bool is_file(const std::string &p)
{
	const DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// そのディレクトリが ROM 置き場かどうか
static bool has_roms(const std::string &dir)
{
	return !dir.empty() && is_file(dir + "\\mu2000_flash.bin");
}

// roms.txt に書かれた場所を読む（1 行目だけ）
static std::string read_pointer_file(const std::string &path)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return {};
	char line[1024] = {};
	if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); return {}; }
	std::fclose(f);
	std::string s(line);
	// メモ帳などが付ける BOM を落とす。これがあると場所を見失う
	if (s.size() >= 3 && (unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb &&
	    (unsigned char)s[2] == 0xbf)
		s.erase(0, 3);
	while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
	                      s.back() == ' '  || s.back() == '\t'))
		s.pop_back();
	return s;
}

// ---- 記録。画面が無いので、うまくいかなかったときはここを見てもらう

static std::string log_path()
{
	const std::string base = env("LOCALAPPDATA");
	if (base.empty())
		return {};
	const std::string dir = base + "\\S-MU2000";
	CreateDirectoryA(dir.c_str(), nullptr);
	return dir + "\\log.txt";
}

void logf(const char *fmt, ...)
{
	static const std::string path = log_path();
	if (path.empty())
		return;
	const bool fresh = !is_file(path);
	std::FILE *f = std::fopen(path.c_str(), "ab");
	if (!f)
		return;
	if (fresh)
		std::fwrite("\xef\xbb\xbf", 1, 3, f);   // UTF-8 の印。無いと化けて読まれる
	SYSTEMTIME t;
	GetLocalTime(&t);
	std::fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay,
	             t.wHour, t.wMinute, t.wSecond);
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(f, fmt, ap);
	va_end(ap);
	std::fputc('\n', f);
	std::fclose(f);
}

// ROM 置き場を探す。見つかった場所を返す。無ければ空で、探した場所が tried に入る
std::string state_dir(const char *leaf)
{
	const std::string base = env("LOCALAPPDATA");
	if (base.empty() || !leaf || !*leaf)
		return {};
	std::string dir = base + "\\S-MU2000";
	CreateDirectoryA(dir.c_str(), nullptr);
	dir += "\\";
	dir += leaf;
	CreateDirectoryA(dir.c_str(), nullptr);
	const DWORD a = GetFileAttributesA(dir.c_str());
	if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
		return {};
	return dir;
}

std::string resource_dir()
{
	const std::string dir = module_dir();
	if (dir.empty())
		return {};
	const std::string res = dir + "\\..\\Resources";
	const DWORD a = GetFileAttributesA(res.c_str());
	if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
		return {};
	return res;
}

std::string find_roms(std::string &tried)
{
	std::vector<std::string> cand;

	// 1. 環境変数。一番強い
	const std::string ev = env("S_MU2000_ROMS");
	if (!ev.empty())
		cand.push_back(ev);

	const std::string dir = module_dir();
	if (!dir.empty()) {
		// 2. バンドルの Resources。
		//    <名前>.vst3/Contents/x86_64-win/ に DLL がいるので 1 つ上
		cand.push_back(dir + "\\..\\Resources");
		cand.push_back(dir + "\\..\\Resources\\roms");
		// 3. DLL のすぐ横
		cand.push_back(dir + "\\roms");
		cand.push_back(dir);
		// 4. 場所を書いた紙
		const std::string notes[2] = { dir + "\\..\\Resources\\roms.txt",
		                               dir + "\\roms.txt" };
		for (const std::string &p : notes) {
			const std::string s = read_pointer_file(p);
			if (!s.empty())
				cand.push_back(s);
		}
	}

	// 5. 決め打ちの置き場
	const std::string local = env("LOCALAPPDATA");
	if (!local.empty())
		cand.push_back(local + "\\S-MU2000\\roms");
	const std::string home = env("USERPROFILE");
	if (!home.empty())
		cand.push_back(home + "\\Documents\\S-MU2000\\roms");

	for (const std::string &c : cand) {
		char full[MAX_PATH * 2] = {};
		const DWORD n = GetFullPathNameA(c.c_str(), sizeof(full), full, nullptr);
		const std::string p = (n && n < sizeof(full)) ? std::string(full, n) : c;
		if (has_roms(p))
			return p;
		tried += "  " + p + "\n";
	}
	return {};
}


} // namespace vst3
} // namespace smu2000
