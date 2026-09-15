// license:BSD-3-Clause
//
// MIDI ファイルを食わせて WAV に書き出す。
//
//   render <rom ディレクトリ> <MIDI ファイル> <出力 wav> [秒数] [--adc-in 入力 wav] [--card カード img]
//
// --adc-in は A/D INPUT に流す音（16bit PCM、1 か 2 チャンネル、44.1kHz）。MIDI の 0 秒から流す。
// 左が AD1、右が AD2（1 チャンネルなら両方に同じもの）
// --card は SmartMedia の中身のファイル（gui で作ったもの）を差す。firmware が書いたブロックは終わりに書き戻す
//
// 実機と同じく、MIDI は 31250bps の直列で MIDI IN A に流し込む。
// 出来た WAV は MAME の録音と突き合わせるためのもの。

#include "mu2000.h"
#include "smf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void write_wav(const std::string &path, const std::vector<s16> &pcm, u32 rate)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	const u32 bytes = u32(pcm.size() * 2);
	auto u32w = [&](u32 v) { u8 b[4] = { u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24) };
	                         std::fwrite(b, 1, 4, f); };
	auto u16w = [&](u16 v) { u8 b[2] = { u8(v), u8(v >> 8) }; std::fwrite(b, 1, 2, f); };
	std::fwrite("RIFF", 1, 4, f); u32w(36 + bytes); std::fwrite("WAVE", 1, 4, f);
	std::fwrite("fmt ", 1, 4, f); u32w(16); u16w(1); u16w(2);
	u32w(rate); u32w(rate * 4); u16w(4); u16w(16);
	std::fwrite("data", 1, 4, f); u32w(bytes);
	std::fwrite(pcm.data(), 1, bytes, f);
	std::fclose(f);
}

// 16bit PCM の WAV を読む。左右に分けて返す。読めなければ false
bool read_wav16(const std::string &path, std::vector<s16> &l, std::vector<s16> &r, std::string &err)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) { err = "開けない: " + path; return false; }
	std::vector<u8> d;
	u8 buf[65536];
	size_t got;
	while ((got = std::fread(buf, 1, sizeof buf, f)) > 0)
		d.insert(d.end(), buf, buf + got);
	std::fclose(f);
	auto u16at = [&](size_t o) { return u16(d[o] | (d[o + 1] << 8)); };
	auto u32at = [&](size_t o) { return u32(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (u32(d[o + 3]) << 24)); };
	if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) { err = "WAV ではない: " + path; return false; }
	int ch = 0, bits = 0;
	u32 rate = 0;
	for (size_t o = 12; o + 8 <= d.size();) {
		const u32 len = u32at(o + 4);
		const size_t body = o + 8;
		if (!std::memcmp(d.data() + o, "fmt ", 4) && body + 16 <= d.size()) {
			ch = u16at(body + 2); rate = u32at(body + 4); bits = u16at(body + 14);
		} else if (!std::memcmp(d.data() + o, "data", 4)) {
			if (bits != 16 || (ch != 1 && ch != 2)) { err = "16bit の 1 か 2 チャンネルだけ読める: " + path; return false; }
			if (rate != 44100) std::fprintf(stderr, "警告: %s は %u Hz（44100 Hz として流す）\n", path.c_str(), rate);
			const size_t n = std::min<size_t>(len, d.size() - body) / (2 * ch);
			l.resize(n); r.resize(n);
			for (size_t i = 0; i < n; i++) {
				l[i] = s16(u16at(body + i * 2 * ch));
				r[i] = s16(u16at(body + i * 2 * ch + (ch == 2 ? 2 : 0)));
			}
			return true;
		}
		o = body + len + (len & 1);
	}
	err = "data が無い: " + path;
	return false;
}

} // namespace


int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr,
			"使い方: render <rom ディレクトリ> <MIDI ファイル> <出力 wav> [秒数]\n");
		return 1;
	}
	const std::string dir = argv[1], mid = argv[2], wav = argv[3];
	double seconds = 0.0;
	const char *swptrace = nullptr;
	bool single = false;   // スレーブを別スレッドにしない
	double boot = -1.0;     // 負なら firmware が受信を有効にするまで待つ
	const char *mu_dac_path = nullptr;
	u32 mu_dac_from = 0, mu_dac_count = 0;
	const char *meg_path = nullptr;    // MEG の中身を書き出す先
	const char *meg_trace = nullptr;   // MEG を 1 命令ずつ追う
	u32 meg_tr_from = 0, meg_tr_count = 0, meg_tr_pc0 = 0, meg_tr_pc1 = 0x180;
	const char *adc_path = nullptr;    // A/D INPUT に流す WAV
	const char *card_path = nullptr;   // 差す SmartMedia
	for (int i = 4; i < argc; i++) {
		if (!std::strcmp(argv[i], "--trace-swp") && i + 1 < argc)
			swptrace = argv[++i];
		else if (!std::strcmp(argv[i], "--boot") && i + 1 < argc)
			boot = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--dump-dac") && i + 3 < argc) {
			mu_dac_path = argv[++i];
			mu_dac_from = u32(std::strtoul(argv[++i], nullptr, 0));
			mu_dac_count = u32(std::strtoul(argv[++i], nullptr, 0));
		}
		else if (!std::strcmp(argv[i], "--dump-meg") && i + 1 < argc)
			meg_path = argv[++i];
		else if (!std::strcmp(argv[i], "--trace-meg") && i + 5 < argc) {
			meg_trace    = argv[++i];
			meg_tr_from  = u32(std::strtoul(argv[++i], nullptr, 0));
			meg_tr_count = u32(std::strtoul(argv[++i], nullptr, 0));
			meg_tr_pc0   = u32(std::strtoul(argv[++i], nullptr, 0));
			meg_tr_pc1   = u32(std::strtoul(argv[++i], nullptr, 0));
		}
		else if (!std::strcmp(argv[i], "--single"))
			single = true;
		else if (!std::strcmp(argv[i], "--adc-in") && i + 1 < argc)
			adc_path = argv[++i];
		else if (!std::strcmp(argv[i], "--card") && i + 1 < argc)
			card_path = argv[++i];
		else if (!std::strcmp(argv[i], "-v"))
			smu2000::g_verbose = true;
		else
			seconds = std::atof(argv[i]);
	}

	std::vector<smf::event> events;
	std::string err;
	if (!smf::load(mid, events, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
	std::printf("MIDI: %zu イベント、最後は %.2f 秒\n",
	            events.size(), events.empty() ? 0.0 : events.back().time);

	std::vector<s16> adc_l, adc_r;
	if (adc_path && !read_wav16(adc_path, adc_l, adc_r, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
		std::fprintf(stderr, "警告: %s\n", mu.error().c_str());

	if (card_path && !mu.card().load(card_path, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

	std::FILE *tf = swptrace ? std::fopen(swptrace, "w") : nullptr;
	if (tf)
		mu.set_swp_trace(tf, true);

	if (mu_dac_path) {
		mu.swpm().m_dbg_dac = std::fopen(mu_dac_path, "w");
		mu.swpm().m_dbg_dac_from = mu_dac_from;
		mu.swpm().m_dbg_dac_count = mu_dac_count;
		if (const char *c = std::getenv("SWP30_CHAN"))
			mu.swpm().m_dbg_chan = int(std::strtol(c, nullptr, 0));
	}

	if (meg_trace) {
		mu.swpm().m_dbg_meg = std::fopen(meg_trace, "w");
		mu.swpm().m_dbg_meg_from = meg_tr_from;
		mu.swpm().m_dbg_meg_count = meg_tr_count;
		mu.swpm().m_dbg_meg_pc0 = u16(meg_tr_pc0);
		mu.swpm().m_dbg_meg_pc1 = u16(meg_tr_pc1);
	}

	mu.set_threaded(!single);
	mu.reset();

	if (seconds <= 0.0)
		seconds = (events.empty() ? 0.0 : events.back().time) + 3.0;

	const u32 rate = 44100;
	std::vector<s16> pcm;

	// 起動を待つ。実機も電源投入から数秒は MIDI を受け付けない。
	// 待たずに流すと曲頭のリセットや音色指定が捨てられ、全パートが
	// 初期音色（ピアノ）で鳴り、発音数も足りなくなって音が抜ける。
	// firmware が受信を有効にした時点を印にする
	if (boot < 0.0) {
		const size_t limit = size_t(30.0 * rate);
		size_t i = 0;
		for (; i < limit && !mu.midi_ready(); i++) {
			s32 l = 0, r = 0;
			mu.run_sample(l, r);
			pcm.push_back(s16(std::clamp(l * 32768 / mu2000::DAC_FULL_SCALE, -32768, 32767)));
			pcm.push_back(s16(std::clamp(r * 32768 / mu2000::DAC_FULL_SCALE, -32768, 32767)));
		}
		boot = double(i) / rate;
		if (i >= limit) {
			std::fprintf(stderr, "起動を待ったが MIDI 受信が有効にならなかった\n");
			return 1;
		}
		std::printf("起動に %.2f 秒。ここから MIDI を流す\n", boot);
	}

	const size_t total = size_t((boot + seconds) * rate);
	pcm.reserve(total * 2);

	// 出し先は SMF のポート指定（`FF 21`）に従う。口 0 = MIDI IN A、
	// 口 1 = MIDI IN B。加えて、ファイルの中に `F5 nn`（1=A / 2=B）を
	// 入れておけばそこから切り替わる（F7 エスケープで埋める）。
	// 実機の firmware は F5 を見ていないので、**振り分けるのはこちら側の役目**
	int port = -1;                     // -1 なら SMF の指定に従う
	size_t next = 0;
	for (size_t i = pcm.size() / 2; i < total; i++) {
		const double t = double(i) / rate - boot;
		while (next < events.size() && events[next].time <= t) {
			const std::vector<u8> &ev = events[next].bytes;
			if (ev.size() == 2 && ev[0] == 0xf5)
				port = std::clamp(int(ev[1]) - 1, 0, mu2000::MIDI_PORTS - 1);
			else {
				// ファイルの口 3・4 は gui の既定と同じく A・B に重ねる
				const int to = port >= 0 ? port : smf::mu_port(events[next].port, true);
				for (u8 b : ev)
					mu.midi_in(b, to);
			}
			next++;
		}

		if (!adc_l.empty()) {
			const double tin = t * rate;
			const size_t k = tin < 0 ? adc_l.size() : size_t(tin);
			mu.set_audio_input(k < adc_l.size() ? adc_l[k] : 0, k < adc_r.size() ? adc_r[k] : 0);
		}
		s32 l = 0, r = 0;
		mu.run_sample(l, r);
		// DAC の全振幅は 1<<17。16bit に落とす（MAME の 1<<17 目盛りと同じ）
		l = l * 32768 / mu2000::DAC_FULL_SCALE;
		r = r * 32768 / mu2000::DAC_FULL_SCALE;
		pcm.push_back(s16(std::clamp(l, -32768, 32767)));
		pcm.push_back(s16(std::clamp(r, -32768, 32767)));

		if (!(i % (rate * 5)))
			std::printf("  %5.1f 秒  PC=%08x\n", double(i) / rate - boot, mu.cpu().pc());
	}

	if (tf)
		std::fclose(tf);

	// MEG の中身。最後の姿（＝最後に設定したエフェクト）を書き出す
	if (meg_path) {
		mu.swpm().dump_meg((std::string(meg_path) + ".m").c_str());
		mu.swps().dump_meg((std::string(meg_path) + ".s").c_str());
		std::printf("MEG を書き出した: %s.m / %s.s\n", meg_path, meg_path);
	}

	if (smu2000::g_verbose)
		std::printf("最大値  AWM2=%d  MEG=%d  DAC=%d\n",
		            mu.swpm().m_dbg_awm_max, mu.swpm().m_dbg_meg_max, mu.swpm().m_dbg_adc_max),
		std::printf("        MEG入力=%d  MELO=%d\n",
		            mu.swpm().m_dbg_megin_max, mu.swpm().m_dbg_melo_max);

	std::printf("CPU %llu サイクル / %zu サンプル = %.3f（あるべき値 %.3f）\n",
	            (unsigned long long)mu.cpu().total_cycles(), total,
	            double(mu.cpu().total_cycles()) / total, 28000000.0 / rate);

	std::printf("実行ループ %llu 周（1 サンプルあたり %.2f 周）、"
	            "タイマ %llu 回、周辺イベント %llu 回\n",
	            (unsigned long long)mu.m_loops, double(mu.m_loops) / total,
	            (unsigned long long)mu.m_timer_fires, (unsigned long long)mu.m_event_fires);

	mu.print_swp_widths();

	if (smu2000::g_verbose) {
		auto report = [](const char *name, swp30_device &d) {
			int silent = 0, weak = 0;
			for (auto [e, l] : d.m_dbg_notes) {
				if (!l || e == 0) silent++;
				else if (e / l < 50) weak++;
			}
			std::printf("%s: 発音 %zu 件 無音 %d 件 ごく小さい %d 件\n",
			            name, d.m_dbg_notes.size(), silent, weak);
			int bucket[8] = {};
			for (auto [e, l] : d.m_dbg_notes) {
				if (!l) continue;
				const u64 avg = e / l;
				int k = 0;
				while (k < 7 && avg >= (u64(20) << k)) k++;
				bucket[k]++;
			}
			std::printf("   平均振幅の分布 <20:%d <40:%d <80:%d <160:%d <320:%d <640:%d <1280:%d それ以上:%d\n",
			            bucket[0],bucket[1],bucket[2],bucket[3],bucket[4],bucket[5],bucket[6],bucket[7]);
		};
		report("マスタ", mu.swpm());
		report("スレーブ", mu.swps());
	}

	if (card_path) {
		std::vector<smu2000::smartmedia::block> blocks;
		mu.card().take_dirty_blocks(blocks);
		if (!smu2000::smartmedia::write_blocks(card_path, blocks, err))
			std::fprintf(stderr, "%s\n", err.c_str());
		else if (!blocks.empty())
			std::printf("カードに書き戻した: %s（%zu ブロック）\n", card_path, blocks.size());
	}

	write_wav(wav, pcm, rate);
	std::printf("書き出した: %s（%.1f 秒）\n", wav.c_str(), double(total) / rate);
	return 0;
}
