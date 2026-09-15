// license:BSD-3-Clause
//
// WASAPI の音声出力を、自分のスレッドで回す。
//
// live.exe で詰めた形をそのまま切り出したもの。要点は
// **時計を自分で持たないこと**。デバイスが「N サンプルくれ」と言った分だけ
// fill を呼ぶ。GUI がある側ではメッセージループを止められないので、
// ここはスレッドに分かれている必要がある。
//
// **標本化周波数の変換は自分でやる。** デバイスは 48000Hz の float を
// 言ってくることが多いが、MU2000 は 44100Hz より他では動かない。変換を
// Windows に任せると既定の品質の変換器を通されるので、窓関数付き sinc
// （ui::resampler）で自分で変換してから渡す。
//
// **溜めは満杯にしない。** 待ち時間は「書いた音の前に溜まっている量」で
// 決まる。昔は起きるたびに溜めを満杯まで埋めていたので、溜めの長さが
// そのまま待ち時間になっていた。いまは target だけ溜めて、それ以上は
// 書かない。target は音源の最悪値より長くないと音が切れる
// （doc/todo.md 2 番、build/blocktime.exe で測れる）。

#ifndef S_MU2000_UI_AUDIO_OUT_H
#define S_MU2000_UI_AUDIO_OUT_H

#pragma once

#include "compat/mamecompat.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace ui {

constexpr u32 AUDIO_RATE = 44100;

class audio_out
{
public:
	// 16bit 2ch インタリーブで frames サンプルぶん書く（44100Hz）
	using fill_fn = std::function<void(s16 *out, u32 frames)>;

	~audio_out() { stop(); }

	// 使える再生デバイスの名前。番号は挿し直すとずれるので、**名前で選ぶ**
	static std::vector<std::string> list();

	// latency_ms は**溜める目標の長さ**。0 以下ならデバイスの周期 2 つぶん。
	// exclusive なら Windows の混ぜ合わせを通さない（他のアプリは鳴らせない）。
	// device は名前の一部（空なら Windows の既定）。**既定は勝手に変わる**ので、
	// 聞いている口が決まっているなら指定したほうがよい
	bool start(int latency_ms, fill_fn fill, std::string &err, bool exclusive = false,
	           const std::string &device = std::string(), bool raw = false);

	// 実際に開いた口の名前
	std::string device_name() const { return m_dev_name; }
	void stop();

	// 具合。すべて音声スレッドが書き、他所から読んでよい
	u32 buffer_frames() const { return m_buffer_frames.load(); }
	u64 produced() const      { return m_produced.load(); }
	// 間に合わなかった回数。**デバイスが待たされた（音が切れた）回数**で、
	// 「起きたときに残量ゼロ」ではない。残量ゼロは溜めを詰めれば常に起きる
	u64 late() const          { return m_late.load(); }
	// 起きたときに残っていた最小の量（ミリ秒）。余裕の実測
	double slack_min_ms() const;
	// MMCSS（Pro Audio）に登録できたか。だめだと途切れやすくなる
	bool mmcss() const        { return m_mmcss.load(); }
	bool running() const      { return m_running.load(); }
	std::string error() const { return m_err; }
	double cpu_percent() const;
	double worst_ms() const;

	// デバイスが言ってきた形式。開いた後に読む
	u32 device_rate() const     { return m_dev_rate.load(); }
	u32 device_channels() const { return m_dev_channels.load(); }
	bool converting() const     { return m_converting.load(); }
	bool exclusive() const      { return m_exclusive.load(); }
	// エンジンの信号処理を飛ばせたか（共有モードのみ）
	bool raw() const            { return m_raw.load(); }
	double period_ms() const    { return m_period_ms.load(); }
	double buffer_ms() const;

	// ---- 待ち時間の内訳（すべてミリ秒）
	//
	// 書いた音が鳴るまで = 前に溜まっている量 + デバイスの分。
	// 前に溜まっている量はこちらが決められる。デバイスの分は決められない
	double queue_ms() const;        // 起きたときに溜まっていた量（平均）
	double queue_worst_ms() const;  // 同じ（最悪）
	double target_ms() const;       // 溜める目標
	double device_ms() const { return m_stream_ms.load(); }  // GetStreamLatency
	// 書いた音が鳴るまでの見込み
	double output_ms() const { return queue_ms() + device_ms(); }

	// **書いたのに、まだ鳴っていない量。** IAudioClock の再生位置との差なので、
	// こちらの溜めだけでなく**ドライバが抱えている分も入る**。
	// 溜めが 20ms なのにこれが 100ms なら、残りはドライバの中にある
	double inflight_ms() const;
	double inflight_worst_ms() const;

	// デバイスへ渡したバイト列をそのまま書き出す（切り分け用）。
	// start() の前に呼ぶ。止めるときに WAV として閉じる
	void set_capture(const std::string &path) { m_cap_path = path; }

	// 人が読む行
	std::string format_line() const;
	std::string latency_line() const;

private:
	void run(int latency_ms, bool exclusive);

#ifdef __APPLE__
	// CoreAudio（audio_out_mac.cpp）。デバイスの実時間スレッドから呼ばれ、
	// frames ぶんを iodata（AudioBufferList）へ書く。戻り値は OSStatus。
	// CoreAudio の型をここへ持ち込まないために void * と int にしてある
	int   render(u32 frames, void *iodata);
	void *m_mac = nullptr;          // mac_stream。開いていなければ nullptr
#endif

	fill_fn           m_fill;
	std::thread       m_thread;
	std::atomic<bool> m_quit{false};
	std::atomic<bool> m_running{false};
	std::string       m_err;

	std::atomic<u32> m_buffer_frames{0};
	std::atomic<u64> m_produced{0}, m_late{0};
	std::atomic<u64> m_slack_min{~u64(0)};
	std::atomic<u64> m_busy_ticks{0}, m_worst_ticks{0};
	std::atomic<bool> m_mmcss{false};
	// 立ち上がりの首尾。**メンバに置くこと。** スレッドは run() を抜けた後に
	// ここへ書くので、start() のローカルに置くと宙ぶらりんの参照になる
	std::atomic<int>  m_start_state{0};   // 0 待ち / 1 動いた / 2 だめ
	std::atomic<u32> m_dev_rate{AUDIO_RATE}, m_dev_channels{2};
	std::atomic<bool> m_converting{false};
	std::atomic<bool> m_exclusive{false};
	std::atomic<double> m_period_ms{0.0}, m_stream_ms{0.0};
	std::atomic<int>  m_dev_bits{16};
	std::atomic<bool> m_dev_float{false};
	std::atomic<u32>  m_target_frames{0};
	std::atomic<u64>  m_queue_sum{0}, m_queue_n{0}, m_queue_worst{0};
	std::atomic<u64>  m_inflight_sum{0}, m_inflight_n{0}, m_inflight_worst{0};
	std::string       m_cap_path;
	std::string       m_dev_name, m_want_dev;
	bool              m_want_raw = false;
	std::atomic<bool> m_raw{false};
	s64 m_qpc_freq = 1;
};

} // namespace ui

#endif // S_MU2000_UI_AUDIO_OUT_H
