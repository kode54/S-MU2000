// license:BSD-3-Clause
//
// macOS の音声出力（CoreAudio の AUHAL）。audio_out.cpp（Windows の WASAPI）と
// 同じ口を出す。ui::audio_out を使う側（live / gui / vst3）はどちらか分からない。
//
// **時計は持たない。** CoreAudio は自分の実時間スレッドから「N フレームくれ」と
// 呼びに来るので、その中で音を作ってそのまま器へ書く。Windows 側が自前の
// スレッドと溜めを持っているのは、WASAPI が合図で起こす形だからで、
// こちらには要らない（だから「溜め」は常に 0 で、待ち時間はデバイスの
// 器の長さとドライバの分だけになる）。
//
// **標本化周波数の変換は自分でやる。** デバイスは 48000Hz で回っていることが
// 多いが、MU2000 は 44100Hz より他では動かない。AUHAL に任せると既定の
// 変換器を通されるので、窓関数付き sinc（ui::resampler）で自分で変換してから渡す。
//
// **待ち時間はデバイスの器の大きさで決まる。** latency_ms から
// kAudioDevicePropertyBufferFrameSize を決める。デバイスが受け付けた値が
// そのまま周期になる。

#include "audio_out.h"
#include "resampler.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include "compat/platform.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace ui {

namespace {

std::string to_utf8(CFStringRef s)
{
	if (!s)
		return {};
	const CFIndex len = CFStringGetLength(s);
	const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
	std::string out(size_t(max), '\0');
	if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8))
		return {};
	out.resize(std::char_traits<char>::length(out.c_str()));
	return out;
}

std::string endpoint_name(AudioDeviceID id)
{
	AudioObjectPropertyAddress a = { kAudioObjectPropertyName,
	                                 kAudioObjectPropertyScopeGlobal,
	                                 kAudioObjectPropertyElementMain };
	CFStringRef s = nullptr;
	UInt32 size = sizeof(s);
	if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &s) != noErr || !s)
		return "?";
	const std::string out = to_utf8(s);
	CFRelease(s);
	return out.empty() ? "?" : out;
}

// そのデバイスが持つ出力チャンネルの数。0 なら入力専用
u32 output_channels(AudioDeviceID id)
{
	AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration,
	                                 kAudioObjectPropertyScopeOutput,
	                                 kAudioObjectPropertyElementMain };
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(id, &a, 0, nullptr, &size) != noErr || !size)
		return 0;
	std::vector<u8> raw(size);
	AudioBufferList *bl = reinterpret_cast<AudioBufferList *>(raw.data());
	if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, bl) != noErr)
		return 0;
	u32 n = 0;
	for (UInt32 i = 0; i < bl->mNumberBuffers; i++)
		n += bl->mBuffers[i].mNumberChannels;
	return n;
}

// 出力を持つデバイスを並べる
std::vector<AudioDeviceID> output_devices()
{
	AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices,
	                                 kAudioObjectPropertyScopeGlobal,
	                                 kAudioObjectPropertyElementMain };
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr)
		return {};
	std::vector<AudioDeviceID> all(size / sizeof(AudioDeviceID));
	if (all.empty() ||
	    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, all.data()) != noErr)
		return {};
	std::vector<AudioDeviceID> out;
	for (AudioDeviceID id : all)
		if (output_channels(id))
			out.push_back(id);
	return out;
}

AudioDeviceID default_output()
{
	AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice,
	                                 kAudioObjectPropertyScopeGlobal,
	                                 kAudioObjectPropertyElementMain };
	AudioDeviceID id = kAudioObjectUnknown;
	UInt32 size = sizeof(id);
	AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &id);
	return id;
}

double nominal_rate(AudioDeviceID id)
{
	AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate,
	                                 kAudioObjectPropertyScopeGlobal,
	                                 kAudioObjectPropertyElementMain };
	Float64 r = 0.0;
	UInt32 size = sizeof(r);
	if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &r) != noErr || r <= 0.0)
		return double(AUDIO_RATE);
	return double(r);
}

u32 device_uint(AudioDeviceID id, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
	AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
	UInt32 v = 0, size = sizeof(v);
	if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &v) != noErr)
		return 0;
	return v;
}

// 書いた音が鳴るまでの、デバイスとドライバの分（フレーム）。
// WASAPI の GetStreamLatency に当たる
u32 device_latency_frames(AudioDeviceID id)
{
	u32 n = device_uint(id, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) +
	        device_uint(id, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);

	// 流れそのものが持つ分も足す
	AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams,
	                                 kAudioObjectPropertyScopeOutput,
	                                 kAudioObjectPropertyElementMain };
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(id, &a, 0, nullptr, &size) == noErr && size >= sizeof(AudioStreamID)) {
		std::vector<AudioStreamID> st(size / sizeof(AudioStreamID));
		if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, st.data()) == noErr && !st.empty()) {
			AudioObjectPropertyAddress la = { kAudioStreamPropertyLatency,
			                                  kAudioObjectPropertyScopeGlobal,
			                                  kAudioObjectPropertyElementMain };
			UInt32 v = 0, vs = sizeof(v);
			if (AudioObjectGetPropertyData(st[0], &la, 0, nullptr, &vs, &v) == noErr)
				n += v;
		}
	}
	return n;
}

} // namespace


// 開いている口の中身。ヘッダを Windows 側と共通にしてあるので、
// CoreAudio の道具立てはここに隠す
struct mac_stream {
	AudioUnit      unit = nullptr;
	AudioDeviceID  dev  = kAudioObjectUnknown;
	bool           started = false;

	resampler          rs;
	std::vector<s16>   stage;     // 44100Hz の生の音（m_fill が書く）
	std::vector<float> mixbuf;    // デバイスの周波数に直した 2ch インタリーブ

	std::FILE *cap = nullptr;     // 切り分け用の書き出し
	u64        cap_frames = 0;
	u32        cap_rate = AUDIO_RATE;
};

std::vector<std::string> audio_out::list()
{
	std::vector<std::string> out;
	for (AudioDeviceID id : output_devices())
		out.push_back(endpoint_name(id));
	return out;
}

// Windows 側はここで自前のスレッドを回す。CoreAudio は自分でスレッドを持つので
// 使わない。ヘッダを共通にしてある都合で形だけ残す
void audio_out::run(int, bool)
{
}

bool audio_out::start(int latency_ms, fill_fn fill, std::string &err, bool exclusive,
                      const std::string &device, bool raw)
{
	if (m_mac)
		return true;

	m_want_raw = raw;
	m_want_dev = device;
	m_fill = std::move(fill);
	m_quit.store(false);
	m_err.clear();
	m_qpc_freq = 1000000000;      // 刻みはナノ秒（smu2000::now_ns）

	auto fail = [&](const std::string &what) {
		m_err = what;
		err = what;
		return false;
	};

	// ---- デバイスを選ぶ。名前の一部で探し、無ければ既定
	AudioDeviceID dev = kAudioObjectUnknown;
	if (!m_want_dev.empty()) {
		for (AudioDeviceID id : output_devices())
			if (endpoint_name(id).find(m_want_dev) != std::string::npos) {
				dev = id;
				break;
			}
		if (dev == kAudioObjectUnknown)
			return fail("その名前の再生デバイスが無い: " + m_want_dev);
	}
	if (dev == kAudioObjectUnknown)
		dev = default_output();
	if (dev == kAudioObjectUnknown)
		return fail("音声デバイスが見つからない");

	m_dev_name = endpoint_name(dev);

	// ---- AUHAL を起こす
	AudioComponentDescription desc{};
	desc.componentType         = kAudioUnitType_Output;
	desc.componentSubType      = kAudioUnitSubType_HALOutput;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
	if (!comp)
		return fail("AUHAL が見つからない");

	mac_stream *st = new mac_stream;
	auto bail = [&](const std::string &what) {
		if (st->unit) {
			AudioUnitUninitialize(st->unit);
			AudioComponentInstanceDispose(st->unit);
		}
		delete st;
		return fail(what);
	};

	if (AudioComponentInstanceNew(comp, &st->unit) != noErr)
		return bail("AUHAL を作れない");
	st->dev = dev;

	if (AudioUnitSetProperty(st->unit, kAudioOutputUnitProperty_CurrentDevice,
	                         kAudioUnitScope_Global, 0, &dev, sizeof(dev)) != noErr)
		return bail("そのデバイスへ繋げない: " + m_dev_name);

	// ---- 器の大きさ。これがそのまま周期になり、待ち時間を決める。
	// latency_ms が 0 以下ならデバイスの今の値に任せる。
	//
	// **AudioUnit 越しに頼む。** デバイスそのものに書くと、その機械を使っている
	// 他のアプリの周期まで変わる。AUHAL 越しなら自分の口の分だけで済む
	const double rate = nominal_rate(dev);
	if (latency_ms > 0) {
		UInt32 want = UInt32(double(latency_ms) * rate / 1000.0);
		// デバイスが受け付ける範囲へ丸める
		AudioObjectPropertyAddress ra = { kAudioDevicePropertyBufferFrameSizeRange,
		                                  kAudioObjectPropertyScopeOutput,
		                                  kAudioObjectPropertyElementMain };
		AudioValueRange vr{};
		UInt32 vs = sizeof(vr);
		if (AudioObjectGetPropertyData(dev, &ra, 0, nullptr, &vs, &vr) == noErr)
			want = UInt32(std::clamp(double(want), vr.mMinimum, vr.mMaximum));
		AudioUnitSetProperty(st->unit, kAudioDevicePropertyBufferFrameSize,
		                     kAudioUnitScope_Global, 0, &want, sizeof(want));
	}
	UInt32 buf_frames = 0, bfs = sizeof(buf_frames);
	if (AudioUnitGetProperty(st->unit, kAudioDevicePropertyBufferFrameSize,
	                         kAudioUnitScope_Global, 0, &buf_frames, &bfs) != noErr || !buf_frames)
		buf_frames = 512;

	// ---- こちらが渡す形。float32 の非インタリーブ 2ch。
	// デバイスが何チャンネルでも AUHAL が振り分ける
	AudioStreamBasicDescription fmt{};
	fmt.mSampleRate       = rate;
	fmt.mFormatID         = kAudioFormatLinearPCM;
	fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
	                        kAudioFormatFlagIsNonInterleaved;
	fmt.mChannelsPerFrame = 2;
	fmt.mBitsPerChannel   = 32;
	fmt.mFramesPerPacket  = 1;
	fmt.mBytesPerFrame    = 4;    // 非インタリーブなので 1 チャンネルぶん
	fmt.mBytesPerPacket   = 4;
	if (AudioUnitSetProperty(st->unit, kAudioUnitProperty_StreamFormat,
	                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt)) != noErr)
		return bail("float 32bit 2ch を受け付けてもらえない");

	// **これを忘れると、何のしるしも無く音が止まる。**
	// AudioUnit は 1 回に渡せるフレーム数の上限を自分で持っていて（既定は 1156）、
	// 器がそれより大きいと描き出しの手続きが**一度も呼ばれない**。
	// AudioUnitInitialize も AudioOutputUnitStart も 0 を返すので気付けない。
	// 器を広げたら、必ずここも一緒に広げる
	{
		UInt32 maxf = buf_frames;
		if (AudioUnitSetProperty(st->unit, kAudioUnitProperty_MaximumFramesPerSlice,
		                         kAudioUnitScope_Global, 0, &maxf, sizeof(maxf)) != noErr)
			return bail("1 回に渡せるフレーム数を広げられない");
	}

	// ---- 変換器と作業場。**実時間の中では確保しない**ので、多めに取っておく
	st->rs.configure(double(AUDIO_RATE), rate);
	const size_t room = size_t(std::max<UInt32>(buf_frames, 4096)) * 4;
	st->mixbuf.resize(room * 2);
	st->stage.resize(room * 2);

	// ---- 切り分け用の書き出し（デバイスへ渡した float をそのまま）
	if (!m_cap_path.empty()) {
		st->cap = std::fopen(m_cap_path.c_str(), "wb");
		st->cap_rate = u32(rate);
		if (st->cap) {
			u8 hdr[44] = {};
			std::memcpy(hdr, "RIFF\0\0\0\0WAVEfmt ", 16);
			std::fwrite(hdr, 1, 44, st->cap);   // 中身は stop() で書き直す
		}
	}

	m_dev_rate.store(u32(rate));
	m_dev_channels.store(2);
	m_dev_float.store(true);
	m_dev_bits.store(32);
	m_converting.store(!st->rs.direct());
	m_exclusive.store(false);     // CoreAudio に独り占めは無い（hog mode は使わない）
	m_raw.store(false);
	m_buffer_frames.store(u32(buf_frames));
	m_target_frames.store(u32(buf_frames));
	m_period_ms.store(1000.0 * double(buf_frames) / rate);
	m_stream_ms.store(1000.0 * double(device_latency_frames(dev)) / rate);
	// CoreAudio の IO スレッドは OS が実時間で回す。こちらで上げる必要が無い
	m_mmcss.store(true);

	m_mac = st;

	AURenderCallbackStruct cbs{};
	cbs.inputProc = [](void *ref, AudioUnitRenderActionFlags *, const AudioTimeStamp *,
	                   UInt32, UInt32 frames, AudioBufferList *io) -> OSStatus {
		return OSStatus(static_cast<audio_out *>(ref)->render(u32(frames), io));
	};
	cbs.inputProcRefCon = this;
	if (AudioUnitSetProperty(st->unit, kAudioUnitProperty_SetRenderCallback,
	                         kAudioUnitScope_Input, 0, &cbs, sizeof(cbs)) != noErr) {
		m_mac = nullptr;
		return bail("描き出しの手続きを渡せない");
	}

	if (AudioUnitInitialize(st->unit) != noErr) {
		m_mac = nullptr;
		return bail("音声デバイスを初期化できない");
	}
	if (AudioOutputUnitStart(st->unit) != noErr) {
		m_mac = nullptr;
		return bail("再生を始められない");
	}
	st->started = true;

	// **本当に呼ばれ始めたかを確かめてから成功と言う。** CoreAudio は
	// 開けなかったことを戻り値で教えてくれないことがある（上の
	// MaximumFramesPerSlice がまさにそれだった）。ここで待たずに返すと、
	// 呼んだ側は音の出ない口を掴んだまま延々と待つことになる
	for (int i = 0; i < 200 && !m_produced.load(std::memory_order_relaxed); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	if (!m_produced.load(std::memory_order_relaxed)) {
		AudioOutputUnitStop(st->unit);
		st->started = false;
		m_mac = nullptr;
		return bail("音声デバイスが鳴り始めない: " + m_dev_name);
	}

	m_running.store(true);
	return true;
}

// CoreAudio の実時間スレッドから呼ばれる。ここで音を作って器へ書く。
// **確保も錠も待ちもしない。** m_fill の中の音源だけが仕事
int audio_out::render(u32 frames, void *iodata)
{
	mac_stream *st = static_cast<mac_stream *>(m_mac);
	AudioBufferList *io = static_cast<AudioBufferList *>(iodata);
	if (!st || m_quit.load(std::memory_order_relaxed)) {
		for (UInt32 b = 0; b < io->mNumberBuffers; b++)
			std::memset(io->mBuffers[b].mData, 0, io->mBuffers[b].mDataByteSize);
		return noErr;
	}

	const u64 t0 = smu2000::now_ns();

	// 見込みより多く頼まれたら、そのときだけ広げる（起こらないはずだが落とさない）
	if (st->mixbuf.size() < size_t(frames) * 2)
		st->mixbuf.resize(size_t(frames) * 2);

	const int need = st->rs.input_needed(int(frames));
	if (need > 0) {
		if (st->stage.size() < size_t(need) * 2)
			st->stage.resize(size_t(need) * 2);
		m_fill(st->stage.data(), u32(need));
		st->rs.push(st->stage.data(), need);
	}
	st->rs.pull(st->mixbuf.data(), int(frames));

	// 非インタリーブなので左右を別の器へ
	float *l = io->mNumberBuffers > 0 ? static_cast<float *>(io->mBuffers[0].mData) : nullptr;
	float *r = io->mNumberBuffers > 1 ? static_cast<float *>(io->mBuffers[1].mData) : nullptr;
	for (u32 i = 0; i < frames; i++) {
		if (l) l[i] = st->mixbuf[i * 2 + 0];
		if (r) r[i] = st->mixbuf[i * 2 + 1];
	}
	// 3ch 以上を渡されたら残りは黙らせる
	for (UInt32 b = 2; b < io->mNumberBuffers; b++)
		std::memset(io->mBuffers[b].mData, 0, io->mBuffers[b].mDataByteSize);

	if (st->cap) {
		std::fwrite(st->mixbuf.data(), 4, size_t(frames) * 2, st->cap);
		st->cap_frames += frames;
	}

	const u64 one = smu2000::now_ns() - t0;
	m_busy_ticks.fetch_add(one, std::memory_order_relaxed);
	for (u64 w = m_worst_ticks.load(std::memory_order_relaxed); one > w;)
		if (m_worst_ticks.compare_exchange_weak(w, one, std::memory_order_relaxed))
			break;
	m_produced.fetch_add(frames, std::memory_order_relaxed);

	// 器 1 杯ぶんの時間を使い切ったら、次の回に間に合っていない
	const u32 rate = m_dev_rate.load(std::memory_order_relaxed);
	const u64 span = rate ? u64(1000000000.0 * double(frames) / double(rate)) : 0;
	if (span && one > span)
		m_late.fetch_add(1, std::memory_order_relaxed);

	// 余裕の最小。器 1 杯から、音を作るのに使った分を引いたもの（フレーム）
	if (span) {
		const u64 slack = one >= span ? 0 : u64(double(frames) * double(span - one) / double(span));
		for (u64 v = m_slack_min.load(std::memory_order_relaxed); slack < v;)
			if (m_slack_min.compare_exchange_weak(v, slack, std::memory_order_relaxed))
				break;
	}

	// 溜めは持たない（デバイスの器へ直に書く）。まだ鳴っていない量は
	// 「いま書いた 1 杯 + ドライバの分」
	m_queue_sum.fetch_add(0, std::memory_order_relaxed);
	m_queue_n.fetch_add(1, std::memory_order_relaxed);
	const u64 inflight = frames + u64(m_stream_ms.load(std::memory_order_relaxed) * rate / 1000.0);
	m_inflight_sum.fetch_add(inflight, std::memory_order_relaxed);
	m_inflight_n.fetch_add(1, std::memory_order_relaxed);
	for (u64 v = m_inflight_worst.load(std::memory_order_relaxed); inflight > v;)
		if (m_inflight_worst.compare_exchange_weak(v, inflight, std::memory_order_relaxed))
			break;

	return noErr;
}

void audio_out::stop()
{
	m_quit.store(true);
	mac_stream *st = static_cast<mac_stream *>(m_mac);
	if (!st) {
		m_running.store(false);
		return;
	}
	m_mac = nullptr;

	if (st->started)
		AudioOutputUnitStop(st->unit);
	AudioUnitUninitialize(st->unit);
	AudioComponentInstanceDispose(st->unit);

	if (st->cap) {
		// WAV の頭を書き直す（float 32bit 2ch）
		const u32 bytes = u32(st->cap_frames * 2 * 4);
		std::fseek(st->cap, 0, SEEK_SET);
		auto w32 = [&](u32 v) { u8 b[4] = { u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24) };
		                        std::fwrite(b, 1, 4, st->cap); };
		auto w16 = [&](u16 v) { u8 b[2] = { u8(v), u8(v >> 8) }; std::fwrite(b, 1, 2, st->cap); };
		std::fwrite("RIFF", 1, 4, st->cap); w32(36 + bytes); std::fwrite("WAVE", 1, 4, st->cap);
		std::fwrite("fmt ", 1, 4, st->cap); w32(16); w16(3); w16(2);
		w32(st->cap_rate); w32(st->cap_rate * 8); w16(8); w16(32);
		std::fwrite("data", 1, 4, st->cap); w32(bytes);
		std::fclose(st->cap);
	}
	delete st;
	m_running.store(false);
}

double audio_out::cpu_percent() const
{
	const u64 done = m_produced.load();
	if (!done)
		return 0.0;
	const double audio = double(done) / double(m_dev_rate.load());
	const double busy  = double(m_busy_ticks.load()) / double(m_qpc_freq);
	return 100.0 * busy / audio;
}

double audio_out::worst_ms() const
{
	return 1000.0 * double(m_worst_ticks.load()) / double(m_qpc_freq);
}

double audio_out::buffer_ms() const
{
	const u32 r = m_dev_rate.load();
	return r ? 1000.0 * double(m_buffer_frames.load()) / double(r) : 0.0;
}

double audio_out::queue_ms() const
{
	const u64 n = m_queue_n.load();
	const u32 r = m_dev_rate.load();
	if (!n || !r)
		return 0.0;
	return 1000.0 * (double(m_queue_sum.load()) / double(n)) / double(r);
}

double audio_out::queue_worst_ms() const
{
	const u32 r = m_dev_rate.load();
	return r ? 1000.0 * double(m_queue_worst.load()) / double(r) : 0.0;
}

double audio_out::target_ms() const
{
	const u32 r = m_dev_rate.load();
	return r ? 1000.0 * double(m_target_frames.load()) / double(r) : 0.0;
}

double audio_out::slack_min_ms() const
{
	const u64 v = m_slack_min.load();
	const u32 r = m_dev_rate.load();
	return (v == ~u64(0) || !r) ? 0.0 : 1000.0 * double(v) / double(r);
}

double audio_out::inflight_ms() const
{
	const u64 n = m_inflight_n.load();
	const u32 r = m_dev_rate.load();
	if (!n || !r)
		return 0.0;
	return 1000.0 * (double(m_inflight_sum.load()) / double(n)) / double(r);
}

double audio_out::inflight_worst_ms() const
{
	const u32 r = m_dev_rate.load();
	return r ? 1000.0 * double(m_inflight_worst.load()) / double(r) : 0.0;
}

std::string audio_out::format_line() const
{
	char buf[200];
	std::snprintf(buf, sizeof(buf),
	              "CoreAudio / %u Hz 2 ch float / 周期 %.1f ms / 変換 %s",
	              m_dev_rate.load(), m_period_ms.load(),
	              m_converting.load() ? "自前 sinc" : "無し（44100 のまま）");
	return buf;
}

std::string audio_out::latency_line() const
{
	char buf[220];
	std::snprintf(buf, sizeof(buf),
	              "器 %.1f ms。**まだ鳴っていない量 平均 %.1f 最悪 %.1f ms**"
	              "（デバイスとドライバ %.1f ms）",
	              buffer_ms(), inflight_ms(), inflight_worst_ms(), device_ms());
	return buf;
}

} // namespace ui
