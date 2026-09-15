// license:BSD-3-Clause
//
// hostpaths.h の macOS 版。
//
// **自分の置かれた場所は dladdr で引く。** バンドルの中の実行ファイルから
// Contents/Resources までは決まった形なので、そこを起点に ROM を探す。
//
//   S-MU2000.component/Contents/MacOS/S-MU2000   ← この関数がいる実行ファイル
//   S-MU2000.component/Contents/Resources/       ← ROM か roms.txt を置く場所
//
// CoreFoundation の CFBundle は使わない。AUv3 は他所のプロセス（AUHostingService）
// に読み込まれることがあり、そのときの「主バンドル」はこちらではないので当てにならない。

#include "hostpaths.h"

#include <cstdarg>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

namespace smu2000 {
namespace vst3 {

namespace {

// 自分（プラグイン本体）が入っている実行ファイルのあるディレクトリ
std::string module_dir()
{
	Dl_info info{};
	if (!dladdr(reinterpret_cast<const void *>(&module_dir), &info) || !info.dli_fname)
		return {};
	std::string s(info.dli_fname);
	const size_t slash = s.find_last_of('/');
	return slash == std::string::npos ? std::string() : s.substr(0, slash);
}

std::string env(const char *name)
{
	const char *v = std::getenv(name);
	return (v && *v) ? std::string(v) : std::string();
}

std::string home()
{
	return env("HOME");
}

bool is_file(const std::string &p)
{
	struct stat st{};
	return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// そのディレクトリが ROM 置き場かどうか
bool has_roms(const std::string &dir)
{
	return !dir.empty() && is_file(dir + "/mu2000_flash.bin");
}

// roms.txt に書かれた場所を読む（1 行目だけ）
std::string read_pointer_file(const std::string &path)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return {};
	char line[1024] = {};
	if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); return {}; }
	std::fclose(f);
	std::string s(line);
	// 何かが付けた BOM を落とす。これがあると場所を見失う
	if (s.size() >= 3 && (unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb &&
	    (unsigned char)s[2] == 0xbf)
		s.erase(0, 3);
	while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
	                      s.back() == ' '  || s.back() == '\t'))
		s.pop_back();
	// 頭の ~ は自分の家に開く
	if (s == "~" || (s.size() > 1 && s[0] == '~' && s[1] == '/')) {
		const std::string h = home();
		if (!h.empty())
			s = h + s.substr(1);
	}
	return s;
}

// ---- 記録。画面が無いので、うまくいかなかったときはここを見てもらう。
// ~/Library/Logs/ に置くと Console.app から読める（macOS の作法）

std::string log_path()
{
	const std::string h = home();
	if (h.empty())
		return {};
	const std::string lib = h + "/Library";
	::mkdir(lib.c_str(), 0755);
	const std::string dir = lib + "/Logs";
	::mkdir(dir.c_str(), 0755);
	return dir + "/S-MU2000.log";
}

} // namespace


void logf(const char *fmt, ...)
{
	static const std::string path = log_path();
	if (path.empty())
		return;
	std::FILE *f = std::fopen(path.c_str(), "ab");
	if (!f)
		return;
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
	::localtime_r(&now, &tm);
	std::fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  ", tm.tm_year + 1900, tm.tm_mon + 1,
	             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(f, fmt, ap);
	va_end(ap);
	std::fputc('\n', f);
	std::fclose(f);
}

std::string state_dir(const char *leaf)
{
	const std::string h = home();
	if (h.empty() || !leaf || !*leaf)
		return {};
	// **親から順に作る。** ~/Library が無い家（作るときに使う空の HOME など）も
	// あるので、1 段ずつ掘らないと途中で落ちる
	std::string dir = h;
	for (const char *part : { "Library", "Application Support", "S-MU2000", leaf }) {
		dir += "/";
		dir += part;
		if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
			return {};
	}
	return dir;
}

std::string resource_dir()
{
	const std::string dir = module_dir();
	if (dir.empty())
		return {};
	const std::string res = dir + "/../Resources";
	struct stat st{};
	if (::stat(res.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
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
		//    <名前>.component/Contents/MacOS/ に本体がいるので 1 つ上
		cand.push_back(dir + "/../Resources");
		cand.push_back(dir + "/../Resources/roms");
		// 3. 本体のすぐ横
		cand.push_back(dir + "/roms");
		cand.push_back(dir);
		// 4. 場所を書いた紙
		const std::string notes[2] = { dir + "/../Resources/roms.txt",
		                               dir + "/roms.txt" };
		for (const std::string &p : notes) {
			const std::string s = read_pointer_file(p);
			if (!s.empty())
				cand.push_back(s);
		}
	}

	// 5. 決め打ちの置き場
	const std::string h = home();
	if (!h.empty()) {
		cand.push_back(h + "/Library/Application Support/S-MU2000/roms");
		cand.push_back(h + "/Documents/S-MU2000/roms");
	}

	for (const std::string &c : cand) {
		// realpath は無い道には失敗するので、そのときは元の綴りで見る
		std::string p = c;
		if (char *full = ::realpath(c.c_str(), nullptr)) {
			p = full;
			std::free(full);
		}
		if (has_roms(p))
			return p;
		tried += "  " + p + "\n";
	}
	return {};
}

} // namespace vst3
} // namespace smu2000
