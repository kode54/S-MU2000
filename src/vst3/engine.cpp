// license:BSD-3-Clause

#include "engine.h"

#include "compat/platform.h"
#include "hostpaths.h"
#include "mu2000.h"
#include "nvram.h"
#include "smartmedia.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace smu2000 {
namespace vst3 {



// ---- 起動後の姿の写し。
//
// firmware の起動は実時間で 1-2 秒、遅い機械ならもっとかかる。中身は毎回
// 同じ（同じ ROM・同じ設定なら同じ所に落ち着く）ので、**一度やったら写して
// おいて、次からは戻すだけにする**。
//
// 鍵は「プログラム ROM の中身」と「起動時の NVRAM の中身」の両方。
// どちらかが変われば写しは使えない（設定を変えたら起動のしかたも変わる）。
// 保存の形が変わったときは load_state が版を見て断るので、そのまま作り直される。

namespace {

u64 fnv1a(const u8 *p, size_t n)
{
	u64 h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

// 写しの名前。鍵は「ROM の中身」「起動時の NVRAM の中身」「回した秒数」
std::string boot_cache_name(u64 rom_key, u64 nv_key)
{
	char name[64];
	std::snprintf(name, sizeof(name), "/%016llx-%016llx.bin",
	              (unsigned long long)rom_key, (unsigned long long)nv_key);
	return name;
}

// 書ける置き場（ここへ残す）
std::string boot_cache_path(u64 rom_key, u64 nv_key)
{
	const std::string dir = state_dir("bootcache");
	return dir.empty() ? std::string() : dir + boot_cache_name(rom_key, nv_key);
}

// バンドルに焼いてある写し（読むだけ）。作るときに用意しておくと、
// 初めて挿したときも待たされない
std::string baked_cache_path(u64 rom_key, u64 nv_key)
{
	const std::string dir = resource_dir();
	return dir.empty() ? std::string()
	                   : dir + "/bootcache" + boot_cache_name(rom_key, nv_key);
}

bool read_blob(const std::string &path, std::vector<u8> &out)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (n <= 0) { std::fclose(f); return false; }
	out.resize(size_t(n));
	const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
	std::fclose(f);
	return ok;
}

// 一時ファイルに書いてから置き換える。何枚も同時に起動しても壊れない
void write_blob(const std::string &path, const std::vector<u8> &data)
{
	const std::string tmp = path + ".tmp";
	std::FILE *f = std::fopen(tmp.c_str(), "wb");
	if (!f)
		return;
	const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
	if (std::fclose(f) != 0 || !ok) {
		std::remove(tmp.c_str());
		return;
	}
	if (std::rename(tmp.c_str(), path.c_str()) != 0)
		std::remove(tmp.c_str());
}

} // namespace


// ---- 読み込んだ ROM の使い回し。
// 誰も使わなくなったら消えるよう、控えは weak_ptr で持つ

namespace {

struct rom_set {
	mu2000::u8rom  prog, wave;
	mu2000::u16rom sintab;
	mu2000::u8rom  font;
	std::string    warn;
};

std::mutex             g_rom_mutex;
std::string            g_rom_dir;
std::weak_ptr<rom_set> g_roms;

} // namespace


engine::engine()
{
	build_table();
	for (std::vector<uint8_t> &p : m_pending)
		p.reserve(4096);
}

engine::~engine()
{
	m_abort.store(true, std::memory_order_relaxed);
	if (m_thread.joinable())
		m_thread.join();
	// 音声スレッドはもう回っていない。SmartMedia に書いたものをその場で残す
	if (m_mu && !card_path().empty()) {
		std::vector<smartmedia::block> blocks;
		m_mu->card().take_dirty_blocks(blocks);
		std::string err;
		if (!smartmedia::write_blocks(card_path(), blocks, err))
			log_line(err.c_str());
	}
	delete m_mu;
}

std::string engine::message() const
{
	return state() == status::loading ? std::string("起動中") : m_message;
}

void engine::log_line(const char *text)
{
	logf("%s", text);
}

void engine::start()
{
	if (!m_thread.joinable())
		m_thread = std::thread([this] { boot(); });
}

void engine::boot()
{
	std::string tried;
	const std::string dir = find_roms(tried);
	if (dir.empty()) {
		m_message = "ROM が見つからない。探した場所:\n" + tried +
		            "環境変数 S_MU2000_ROMS で場所を指定できる";
		logf("ROM が見つからない。探した場所:\n%s", tried.c_str());
		ui::driver::publish_message(m_bridge, "ROM が見つからない");
		m_state.store(status::failed, std::memory_order_release);
		return;
	}
	logf("ROM: %s", dir.c_str());
	ui::driver::publish_message(m_bridge, "ROM 読み込み中");

	const uint64_t t_rom = smu2000::now_ns();
	mu2000 *mu = new mu2000;
	std::string warn;
	{
		// ROM は読むだけなので、この DLL の中で 1 組あればいい。
		// トラックごとに挿されると 36MB × 枚数になってしまう
		std::lock_guard<std::mutex> lock(g_rom_mutex);
		std::shared_ptr<rom_set> shared;
		if (g_rom_dir == dir)
			shared = g_roms.lock();

		if (shared) {
			mu->set_program_rom(shared->prog);
			mu->set_wave_rom(shared->wave);
			mu->set_sintab_rom(shared->sintab);
			mu->set_lcd_font(shared->font);
			warn = shared->warn;
			logf("ROM は読み込み済みのものを借りた");
		} else {
			if (!mu->load_program(dir + "/mu2000_flash.bin") ||
			    !mu->load_wave(dir + "/dump")) {
				m_message = mu->error();
				logf("%s", m_message.c_str());
				delete mu;
				m_state.store(status::failed, std::memory_order_release);
				return;
			}
			if (!mu->load_sintab(dir + "/standin/sin-table.bin")) {
				warn = mu->error();
				logf("警告: %s", warn.c_str());
			}
			// LCD の字の絵。無くても音は出るが、画面に何も映らなくなる
			if (!mu->load_lcd_font(dir + "/hd44780u_b04.bin") &&
			    !mu->load_lcd_font(dir + "/standin/hd44780u_b04.bin"))
				logf("警告: %s", mu->error().c_str());
			shared = std::make_shared<rom_set>();
			shared->prog   = mu->program_rom();
			shared->wave   = mu->wave_rom();
			shared->sintab = mu->sintab_rom();
			shared->font   = mu->lcd_font();
			shared->warn   = warn;
			g_rom_dir = dir;
			g_roms    = shared;
		}
		// 借り手が 1 人でも生きている限り、次の人も借りられる
		m_roms = shared;
	}

	logf("ROM 読み込み: %.0f ms", 1e-6 * double(smu2000::now_ns() - t_rom));

	mu->set_threaded(true);
	// gui / live が残した設定で起動する。**読むだけで書かない。**VST3 の中で
	// 変えたものは DAW のプロジェクトに残るし、何枚も挿されたときに
	// 同じファイルを取り合わずに済む
	if (nvram::load(*mu))
		logf("設定: %s", nvram::path(*mu).c_str());
	mu->reset();

	ui::driver::publish_message(m_bridge, "MU2000 起動中");

	// ---- 起動を飛ばす。
	//
	// 一度起動したら、そのときの姿を写しておく。次からは戻すだけでよい。
	// 鍵が合わなければ（ROM か設定が変わっていれば）普通に起動して写し直す。
	// S_MU2000_NO_BOOT_CACHE=1 で切れる（食い違いを疑うとき用）
	const uint64_t t0 = smu2000::now_ns();
	const std::vector<u8> &nv = mu->nvram();
	const u64 rk = nvram::rom_key(*mu);
	const u64 nk = fnv1a(nv.data(), nv.size());
	// 起動をどこまで回すか。0 なら midi_ready で止める（既定）。
	//
	// **midi_ready から先まで回しても、鳴る音は変わらない。**
	// 起動 8 秒と 30 秒で同じ曲を鳴らして突き合わせたところ、1 秒ごとの
	// rms の差は 0.0-0.1% しかなかった（音色も並びも同じ）。midi_ready の
	// 時点で機械の状態そのものは確かにまだ動いているが、その違いは
	// 音に出ない。だから既定はここで止め、**render と同じ姿**から始める。
	// 道具ごとに違う姿から始めると、食い違ったときに切り分けられない。
	//
	// 最後まで回したければ S_MU2000_BOOT_SECONDS に秒数を渡す。
	// **鍵に混ぜる。** 長さを変えたのに前の写しを使うと、
	// 途中で止めた姿から始めてしまう
	double want = 0.0;
	if (const char *e = std::getenv("S_MU2000_BOOT_SECONDS"))
		want = std::atof(e);

	const char *no_cache = std::getenv("S_MU2000_NO_BOOT_CACHE");
	const std::string cache =
	    (no_cache && *no_cache && *no_cache != '0')
	        ? std::string()
	        : boot_cache_path(rk, nk ^ (u64(want * 1000.0) * 0x9e3779b97f4a7c15ull));

	// 焼いてあるものを先に、次に自分で残したものを見る
	bool skipped = false;
	if (!cache.empty()) {
		const u64 dk = nk ^ (u64(want * 1000.0) * 0x9e3779b97f4a7c15ull);
		const std::string baked = baked_cache_path(rk, dk);
		const std::string tries[2] = { baked, cache };
		for (const std::string &p : tries) {
			if (p.empty())
				continue;
			std::vector<u8> blob;
			std::string lerr;
			if (!read_blob(p, blob))
				continue;
			if (mu->load_state(blob.data(), blob.size(), lerr)) {
				skipped = true;
				logf("起動を飛ばした: %s（%.0f ms）", p.c_str(),
				     1e-6 * double(smu2000::now_ns() - t0));
				break;
			}
			// 版違いなど。自分で残したものなら捨てて作り直す
			logf("起動の写しを使えない（%s）: %s", lerr.c_str(), p.c_str());
			if (p == cache)
				std::remove(p.c_str());
		}
	}

	// 起動を走らせる。既定は midi_ready まで（上の want を見よ）。
	//
	// **どこで止めても、口を開けるのは回し終えてから。**
	// 回している間に来た MIDI は engine::midi() が溜めておき、ready に
	// なってから順に流れるので、起動の途中の機械に書き込むことはない。
	// 出てくる音はここで捨てる（測ったところ起動中の MU2000 は 32 秒ぶん
	// 丸ごと無音なので、捨てるものは実際には無い）
	if (!skipped) {
		const int64_t need  = int64_t(want * NATIVE_RATE);
		const int64_t limit = int64_t((want > 30.0 ? want + 10.0 : 40.0) * NATIVE_RATE);

		int64_t ready_at = -1;
		int64_t i = 0;
		for (; i < limit; i++) {
			if (!(i & 4095) && m_abort.load(std::memory_order_relaxed)) {
				delete mu;
				return;
			}
			if (ready_at < 0 && mu->midi_ready())
				ready_at = i;
			// MIDI を取りこぼさなくなり、かつ決めた時間まで回したら終わり
			if (ready_at >= 0 && i >= need)
				break;
			s32 l = 0, r = 0;
			mu->run_sample(l, r);      // **音はここで捨てる**
		}
		if (ready_at < 0) {
			m_message = "MU2000 が起動しなかった（ROM が壊れている可能性）";
			logf("%s", m_message.c_str());
			delete mu;
			m_state.store(status::failed, std::memory_order_release);
			return;
		}
		logf("起動: MIDI が通り始めたのが %.2f 秒、回したのが %.2f 秒ぶん"
		     " / 実時間 %.2f 秒",
		     double(ready_at) / NATIVE_RATE, double(i) / NATIVE_RATE,
		     1e-9 * double(smu2000::now_ns() - t0));

		if (!cache.empty()) {
			write_blob(cache, mu->save_state());
			logf("起動の写しを残した: %s", cache.c_str());
		}
	}

	m_mu = mu;
	m_message = warn.empty() ? std::string("ROM: ") + dir
	                         : std::string("ROM: ") + dir + "\n警告: " + warn;
	m_state.store(status::ready, std::memory_order_release);
	ui::driver::publish_now(*m_mu, m_bridge, true, nullptr);
}


// ---- 標本化周波数の変換

void engine::build_table()
{
	m_tab.resize(size_t(HALF) * STEPS + 2);
	for (size_t k = 0; k < m_tab.size(); k++) {
		const double d = double(k) / STEPS;             // 中心からの距離
		const double x = M_PI * d;
		const double sinc = (k == 0) ? 1.0 : std::sin(x) / x;
		// ブラックマン窓。TAPS 本で阻止域 -74dB くらい
		const double t = (d + HALF) / double(TAPS);
		const double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * t)
		                      + 0.08 * std::cos(4.0 * M_PI * t);
		m_tab[k] = float(sinc * w);
	}
}

bool engine::wait_ready(double seconds)
{
	const uint64_t limit = uint64_t(seconds * 1e9);
	const uint64_t t0 = smu2000::now_ns();
	while (state() == status::loading) {
		if (smu2000::now_ns() - t0 > limit)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return state() == status::ready;
}

void engine::set_output_rate(double rate)
{
	// 変換器の入れ物を作り直すので、音声スレッドと重ならないようにする
	std::lock_guard<std::mutex> lock(m_machine);
	if (rate <= 0.0)
		rate = NATIVE_RATE;
	m_direct = std::fabs(rate - NATIVE_RATE) < 1e-6;
	m_step   = NATIVE_RATE / rate;
	// 上へ変換するときは入力のナイキストまで通す。
	// 下へ変換するときは出力のナイキストで切らないと折り返す
	m_cutoff = std::min(1.0, rate / NATIVE_RATE) * 0.955;
	// 音源側は「必要な先の音」をその場で作れるので、変換に先読みの遅れは無い
	m_latency = 0;
	m_in_rs.configure(rate, NATIVE_RATE);
	m_in_w = m_in_r = 0;
	flush_resampler();
}

void engine::flush_resampler()
{
	std::memset(m_ring_l, 0, sizeof(m_ring_l));
	std::memset(m_ring_r, 0, sizeof(m_ring_r));
	m_written = 0;
	m_pos     = 0.0;
}

// ホストから来たものを自前の列へ。溢れたら捨てる（65536 バイトは
// バルクダンプが通る大きさ。ここが溢れるのは何かがおかしいとき）
void engine::push_midi(uint8_t b, int port)
{
	const size_t next = (m_in_head[port] + 1) & IN_FIFO_MASK;
	if (next == m_in_tail[port]) {
		m_in_dropped++;
		return;
	}
	m_in_fifo[port][m_in_head[port]] = b;
	m_in_head[port] = next;
}

// 1 サンプルにつき 1 回。線が空いていれば 1 バイトだけ渡す。
//
// **音源の受信線そのものが 31250bps で流れる**ので、こちらは「線に 1 バイト
// 以上溜めない」ことだけを守ればよい。そうすれば渡す速さは線の速さに一致する。
// まとめて入れてしまうと線の中に何十バイトも積み上がり、firmware が
// 受け切れないところまで行ってしまう
void engine::drain_midi()
{
	// 切り分け用。1 にすると絞らずに全部渡す（実機より速く入る）
	static const bool off = [] {
		const char *e = std::getenv("S_MU2000_NO_MIDI_THROTTLE");
		return e && *e && *e != '0';
	}();

	for (int port = 0; port < 2; port++) {
		if (off) {
			while (m_in_head[port] != m_in_tail[port]) {
				const uint8_t b = m_in_fifo[port][m_in_tail[port]];
				m_in_tail[port] = (m_in_tail[port] + 1) & IN_FIFO_MASK;
				m_mu->midi_in(b, port);
				m_drv.watch(b, port);
			}
			continue;
		}
		if (m_in_head[port] == m_in_tail[port])
			continue;
		if (m_mu->midi_queued(port) >= 1)
			continue;                     // 線がまだ塞がっている
		const uint8_t b = m_in_fifo[port][m_in_tail[port]];
		m_in_tail[port] = (m_in_tail[port] + 1) & IN_FIFO_MASK;
		m_mu->midi_in(b, port);
		m_drv.watch(b, port);
	}
}

void engine::one_sample(float &l, float &r)
{
	drain_midi();
	s32 li = 0, ri = 0;
	// A/D INPUT。溜めが空なら無音（入力の変換器の先読みの分だけ、頭が少し欠ける）
	if (m_in_r != m_in_w) {
		m_mu->set_audio_input(m_in_q[m_in_r * 2], m_in_q[m_in_r * 2 + 1]);
		m_in_r = (m_in_r + 1) & IN_MASK;
	} else {
		m_mu->set_audio_input(0, 0);
	}
	m_mu->run_sample(li, ri);
	const float k = 1.0f / float(mu2000::DAC_FULL_SCALE);
	l = std::clamp(float(li) * k, -1.0f, 1.0f);
	r = std::clamp(float(ri) * k, -1.0f, 1.0f);
}


void engine::midi(const uint8_t *bytes, size_t n, int port)
{
	port = port == 1 ? 1 : 0;
	// 受け取ったものをそのまま控える（S_MU2000_MIDI_LOG=1 のときだけ）。
	// ホストが何を寄越しているかを、音源に入る前の姿で見るためのもの。
	// 置き場は記録と同じ（砂場の中なら容器の下）
	// **環境変数は砂場の中へは届かない。** AUv3 の拡張は別プロセスで、
	// 端末で付けた変数は継がれない。だから「印のファイルがあれば控える」形にする:
	//
	//   mkdir -p ~/Library/Containers/net.smu2000.S-MU2000.AU/Data/Library/\
	//            Application\ Support/S-MU2000/midilog
	//   touch .../midilog/on
	//
	// 控えは同じところの midi-in.log に出る。端末から動かすときは
	// S_MU2000_MIDI_LOG=1 でもよい
	static std::FILE *midilog = [] () -> std::FILE * {
		const std::string dir = state_dir("midilog");
		if (dir.empty())
			return nullptr;
		const char *e = std::getenv("S_MU2000_MIDI_LOG");
		const bool by_env = e && *e && *e != '0';
		if (!by_env) {
			std::FILE *mark = std::fopen((dir + "/on").c_str(), "rb");
			if (!mark)
				return nullptr;
			std::fclose(mark);
		}
		std::FILE *f = std::fopen((dir + "/midi-in.log").c_str(), "wb");
		if (f)
			logf("MIDI の控え: %s/midi-in.log", dir.c_str());
		return f;
	}();
	if (midilog) {
		std::fprintf(midilog, "%c", port ? 'B' : 'A');
		for (size_t i = 0; i < n; i++)
			std::fprintf(midilog, " %02X", bytes[i]);
		std::fputc('\n', midilog);
		std::fflush(midilog);
	}

	const status s = state();
	if (s == status::failed)
		return;
	// **機械には直に入れない。** 自前の列へ置き、実機の線の速さに均しながら
	// drain_midi() が 1 バイトずつ渡す（列は音声スレッドしか触らない）。
	// ここで機械に触らないので、排他を取る必要もない。
	//
	// 溜まっていたもの（起動待ちや、機械を他が使っていた間の分）が残っている
	// うちは、そちらの後ろに積む。先に新しい方を入れると順番が入れ替わる
	if (s == status::ready && m_pending[port].empty()) {
		for (size_t i = 0; i < n; i++)
			push_midi(bytes[i], port);
		return;
	}
	// 起動待ちか、まだ掃け切っていない。あふれるようなら捨てる
	std::vector<uint8_t> &pending = m_pending[port];
	if (pending.size() + n > 65536)
		return;
	pending.insert(pending.end(), bytes, bytes + n);
}

void engine::all_notes_off()
{
	for (int port = 0; port < 2; port++)
		for (int ch = 0; ch < 16; ch++) {
			const uint8_t msg[6] = { uint8_t(0xb0 | ch), 120, 0,
			                         uint8_t(0xb0 | ch), 123, 0 };
			midi(msg, sizeof(msg), port);
		}
}


// ホストの周波数の入力を 44100 に直して溜める。溢れる分は捨てる
void engine::push_input(const float *in_l, const float *in_r, int n)
{
	if (!in_l || n <= 0)
		return;
	if (!in_r)
		in_r = in_l;
	for (int at = 0; at < n;) {
		const int k = std::min(1024, n - at);
		m_in_stage.resize(size_t(k) * 2);
		for (int i = 0; i < k; i++) {
			m_in_stage[size_t(i) * 2]     = s16(std::lround(std::clamp(in_l[at + i], -1.0f, 1.0f) * 32767.0f));
			m_in_stage[size_t(i) * 2 + 1] = s16(std::lround(std::clamp(in_r[at + i], -1.0f, 1.0f) * 32767.0f));
		}
		m_in_rs.push(m_in_stage.data(), k);
		at += k;
		const int out = m_in_rs.output_available();
		if (out <= 0)
			continue;
		m_in_conv.resize(size_t(out) * 2);
		m_in_rs.pull(m_in_conv.data(), out);
		for (int i = 0; i < out; i++) {
			const int next = (m_in_w + 1) & IN_MASK;
			if (next == m_in_r)
				break;
			m_in_q[m_in_w * 2]     = s16(std::lround(std::clamp(m_in_conv[size_t(i) * 2], -1.0f, 1.0f) * 32767.0f));
			m_in_q[m_in_w * 2 + 1] = s16(std::lround(std::clamp(m_in_conv[size_t(i) * 2 + 1], -1.0f, 1.0f) * 32767.0f));
			m_in_w = next;
		}
	}
}

// MIDI OUT。実機の OUT 端子。midi_out_take() は run_sample と同じ糸からしか
// 呼べないので、呼ぶ側も fill() と同じ糸であること
// pump_out() が渡してきたものを溜める。溢れたら古いほうから捨てる
void engine::tx_push(uint8_t v)
{
	const int next = (m_tx_w + 1) & TX_MASK;
	if (next == m_tx_r)
		m_tx_r = (m_tx_r + 1) & TX_MASK;
	m_tx[m_tx_w] = v;
	m_tx_w = next;
}

size_t engine::midi_out(uint8_t *dst, size_t max)
{
	if (!dst || !max)
		return 0;
	size_t n = 0;
	while (n < max && m_tx_r != m_tx_w) {
		dst[n++] = m_tx[m_tx_r];
		m_tx_r = (m_tx_r + 1) & TX_MASK;
	}
	return n;
}

void engine::fill(float *left, float *right, int n, const float *in_l, const float *in_r)
{
	if (n <= 0)
		return;
	// 音声スレッドは待たない。保存などで機械が使われていれば、この区間は無音
	std::unique_lock<std::mutex> lock(m_machine, std::try_to_lock);
	if (!lock.owns_lock() || state() != status::ready) {
		std::memset(left,  0, size_t(n) * sizeof(float));
		std::memset(right, 0, size_t(n) * sizeof(float));
		if (lock.owns_lock())
			push_input(in_l, in_r, n);
		return;
	}
	push_input(in_l, in_r, n);
	apply_deferred_state();

	m_drv.apply_buttons(*m_mu, m_bridge);
	m_drv.pump_midi(*m_mu, m_bridge);
	m_drv.pump_wheel(*m_mu, m_bridge);

	for (int port = 0; port < 2; port++) {
		for (uint8_t b : m_pending[port])
			push_midi(b, port);
		m_pending[port].clear();
	}

	if (m_direct) {
		for (int i = 0; i < n; i++)
			one_sample(left[i], right[i]);
		m_drv.pump_out(*m_mu, m_bridge, [this](u8 v) { tx_push(v); });
		m_drv.publish(*m_mu, m_bridge, u32(n), u32(NATIVE_RATE), true, nullptr);
		return;
	}

	for (int i = 0; i < n; i++) {
		const int64_t centre = int64_t(std::floor(m_pos));
		// 畳み込みに要る一番先のサンプルまで作る
		while (m_written <= centre + HALF) {
			float l, r;
			one_sample(l, r);
			m_ring_l[m_written & RMASK] = l;
			m_ring_r[m_written & RMASK] = r;
			m_written++;
		}

		double al = 0.0, ar = 0.0, sum = 0.0;
		for (int k = -HALF + 1; k <= HALF; k++) {
			const int64_t idx = centre + k;
			const double d  = std::fabs((m_pos - double(idx)) * m_cutoff);
			const double fx = d * STEPS;
			const size_t j  = size_t(fx);
			if (j + 1 >= m_tab.size())
				continue;
			const double t = fx - double(j);
			const double h = m_tab[j] + (m_tab[j + 1] - m_tab[j]) * t;
			al  += h * m_ring_l[idx & RMASK];
			ar  += h * m_ring_r[idx & RMASK];
			sum += h;
		}
		if (sum > 1e-9) { al /= sum; ar /= sum; }
		left[i]  = std::clamp(float(al), -1.0f, 1.0f);
		right[i] = std::clamp(float(ar), -1.0f, 1.0f);
		m_pos += m_step;
	}

	// firmware が MIDI OUT から送り出したもの（画面の問い合わせの返事）
	m_drv.pump_out(*m_mu, m_bridge, [this](u8 v) { tx_push(v); });
	m_drv.publish(*m_mu, m_bridge, u32(n), u32(NATIVE_RATE), true, nullptr);

	// 桁が落ちる前に原点を戻す。RING の倍数だけずらせば環の並びは変わらない
	if (m_pos > double(1 << 28)) {
		const int64_t base = (int64_t(m_pos) - HALF) & ~int64_t(RMASK);
		m_pos     -= double(base);
		m_written -= base;
	}
}



// ---- 状態の保存と復元
//
// どれも m_machine を取ってその場でやる。音声スレッドは取れない区間を無音にして待たない

void engine::apply_deferred_state()
{
	if (m_deferred_state.empty() || state() != status::ready || !m_mu)
		return;
	std::string err;
	if (!m_mu->load_state(m_deferred_state.data(), m_deferred_state.size(), err))
		log_line(("状態を読み戻せない: " + err).c_str());
	m_deferred_state.clear();
	m_deferred_state.shrink_to_fit();
}

std::vector<uint8_t> engine::save_state()
{
	std::lock_guard<std::mutex> lock(m_machine);
	if (state() != status::ready || !m_mu) {
		// 起動が終わる前に保存されたら、戻す予定だった状態をそのまま返す
		return m_deferred_state;
	}
	apply_deferred_state();
	return m_mu->save_state();
}

bool engine::load_state(const uint8_t *p, size_t n)
{
	if (!p || !n)
		return false;
	std::lock_guard<std::mutex> lock(m_machine);
	if (state() == status::failed)
		return false;
	if (state() != status::ready || !m_mu) {
		m_deferred_state.assign(p, p + n);
		return true;
	}
	m_deferred_state.clear();
	std::string err;
	const bool ok = m_mu->load_state(p, n, err);
	if (!ok)
		log_line(("状態を読み戻せない: " + err).c_str());
	return ok;
}


// ---- 機械に触る仕事（SmartMedia の差し替えなど）

bool engine::on_machine(const std::function<void(mu2000 &)> &fn)
{
	std::lock_guard<std::mutex> lock(m_machine);
	if (state() != status::ready || !m_mu)
		return false;
	fn(*m_mu);
	return true;
}

std::string engine::card_path() const
{
	std::lock_guard<std::mutex> lock(m_card_mutex);
	return m_card_path;
}

void engine::card_flush()
{
	const std::string path = card_path();
	if (path.empty() || !m_mu)
		return;
	std::vector<smartmedia::block> blocks;
	const std::function<void(mu2000 &)> take = [&](mu2000 &m) { m.card().take_dirty_blocks(blocks); };
	if (state() == status::ready)
		on_machine(take);
	else
		take(*m_mu);
	std::string err;
	if (!smartmedia::write_blocks(path, blocks, err))
		log_line(err.c_str());
}

void engine::card_eject()
{
	card_flush();
	on_machine([](mu2000 &m) { m.card().eject(); });
	std::lock_guard<std::mutex> lock(m_card_mutex);
	m_card_path.clear();
}

bool engine::card_insert(const std::string &path, std::string &err)
{
	auto card = std::make_shared<smartmedia>();
	if (!card->load(path, err))
		return false;
	if (state() != status::ready) {
		err = "まだ起動していない";
		return false;
	}
	card_flush();
	if (!on_machine([card](mu2000 &m) { m.card() = std::move(*card); })) {
		err = "カードを差せなかった（音声スレッドが応じない）";
		return false;
	}
	std::lock_guard<std::mutex> lock(m_card_mutex);
	m_card_path = path;
	return true;
}

} // namespace vst3
} // namespace smu2000
