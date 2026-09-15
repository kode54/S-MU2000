// license:BSD-3-Clause
//
// macOS の音声入力（CoreAudio の AUHAL）。audio_in.cpp（Windows の WASAPI）と
// 同じ口を出す。実機の A/D INPUT に流し込む音を、機械の入口から取る。
//
// **標本化周波数は 44100 に直してから積む。** 音源は 44100 でしか動かないので、
// 44100 以外で回っているデバイスからは、間を取って（線形で）合わせる。
// ここは実機の A/D の入口で、出口の音ほど質を問われないので sinc は使わない。
//
// 入力は「取り込む側の口」なので、AUHAL では element 1 の入力を開き、
// element 0 の出力は止める。

#include "audio_in.h"

#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ui {

namespace {

constexpr double NATIVE_RATE = 44100.0;

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

std::string device_label(AudioDeviceID id)
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

u32 input_channels(AudioDeviceID id)
{
	AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration,
	                                 kAudioObjectPropertyScopeInput,
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

std::vector<AudioDeviceID> input_devices()
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
		if (input_channels(id))
			out.push_back(id);
	return out;
}

AudioDeviceID default_input()
{
	AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultInputDevice,
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
		return NATIVE_RATE;
	return double(r);
}

// 開いている口。CoreAudio の道具立てと、44100 へ直すための持ち越しを隠す
struct in_stream {
	AudioUnit unit = nullptr;
	bool      started = false;
	u32       channels = 2;
	double    rate = NATIVE_RATE;

	// AUHAL から受け取る器
	std::vector<u8>    ablmem;
	std::vector<float> chan[2];

	// 44100 へ間を取るための位置と、直前のサンプル
	double pos = 0.0;
	float  prev_l = 0.0f, prev_r = 0.0f;
	bool   have_prev = false;

	std::vector<s16> stage;
};

in_stream *g_in = nullptr;      // audio_in は 1 つしか開かない

audio_in *g_owner = nullptr;

} // namespace


std::vector<std::string> audio_in::list()
{
	std::vector<std::string> out;
	for (AudioDeviceID id : input_devices())
		out.push_back(device_label(id));
	return out;
}

// AUHAL の入力から呼ばれる。ここで引き取って 44100 に直し、輪っかへ積む
static OSStatus in_proc(void *ref, AudioUnitRenderActionFlags *flags,
                        const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                        AudioBufferList *)
{
	in_stream *st = g_in;
	audio_in  *self = static_cast<audio_in *>(ref);
	if (!st || !self || !frames)
		return noErr;

	AudioBufferList *abl = reinterpret_cast<AudioBufferList *>(st->ablmem.data());
	const u32 ch = std::min<u32>(st->channels, 2);
	abl->mNumberBuffers = ch;
	for (u32 c = 0; c < ch; c++) {
		if (st->chan[c].size() < frames)
			st->chan[c].assign(frames, 0.0f);
		abl->mBuffers[c].mNumberChannels = 1;
		abl->mBuffers[c].mDataByteSize   = frames * sizeof(float);
		abl->mBuffers[c].mData           = st->chan[c].data();
	}
	if (AudioUnitRender(st->unit, flags, ts, bus, frames, abl) != noErr)
		return noErr;

	const float *L = st->chan[0].data();
	const float *R = (ch > 1) ? st->chan[1].data() : L;

	// 44100 へ。デバイスがちょうど 44100 ならそのまま
	st->stage.clear();
	if (std::fabs(st->rate - NATIVE_RATE) < 1e-6) {
		st->stage.reserve(size_t(frames) * 2);
		for (UInt32 i = 0; i < frames; i++) {
			st->stage.push_back(s16(std::clamp(L[i], -1.0f, 1.0f) * 32767.0f));
			st->stage.push_back(s16(std::clamp(R[i], -1.0f, 1.0f) * 32767.0f));
		}
	} else {
		const double step = st->rate / NATIVE_RATE;   // 入力 何個で 出力 1 個
		double p = st->pos;
		while (p < double(frames)) {
			const int i0 = int(std::floor(p));
			const double t = p - i0;
			const float a_l = (i0 <= 0 && st->have_prev) ? st->prev_l : L[std::max(0, i0)];
			const float a_r = (i0 <= 0 && st->have_prev) ? st->prev_r : R[std::max(0, i0)];
			const int i1 = std::min<int>(i0 + 1, int(frames) - 1);
			const float l = a_l + (L[i1] - a_l) * float(t);
			const float r = a_r + (R[i1] - a_r) * float(t);
			st->stage.push_back(s16(std::clamp(l, -1.0f, 1.0f) * 32767.0f));
			st->stage.push_back(s16(std::clamp(r, -1.0f, 1.0f) * 32767.0f));
			p += step;
		}
		st->pos = p - double(frames);
		st->prev_l = L[frames - 1];
		st->prev_r = R[frames - 1];
		st->have_prev = true;
	}

	if (!st->stage.empty())
		self->push_frames(st->stage.data(), u32(st->stage.size() / 2));
	return noErr;
}

bool audio_in::start(const std::string &device, std::string &err)
{
	if (g_in)
		return true;

	m_want = device;
	m_err.clear();
	m_quit.store(false);
	m_w.store(0);
	m_r.store(0);

	auto fail = [&](const std::string &what) {
		m_err = what;
		err = what;
		return false;
	};

	AudioDeviceID dev = kAudioObjectUnknown;
	if (!device.empty()) {
		for (AudioDeviceID id : input_devices())
			if (device_label(id).find(device) != std::string::npos) {
				dev = id;
				break;
			}
		if (dev == kAudioObjectUnknown)
			return fail("その名前の録音デバイスが無い: " + device);
	}
	if (dev == kAudioObjectUnknown)
		dev = default_input();
	if (dev == kAudioObjectUnknown)
		return fail("録音デバイスが見つからない");

	AudioComponentDescription desc{};
	desc.componentType         = kAudioUnitType_Output;
	desc.componentSubType      = kAudioUnitSubType_HALOutput;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
	if (!comp)
		return fail("AUHAL が見つからない");

	in_stream *st = new in_stream;
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

	// 取り込む側を開き、出す側は止める
	UInt32 on = 1, off = 0;
	if (AudioUnitSetProperty(st->unit, kAudioOutputUnitProperty_EnableIO,
	                         kAudioUnitScope_Input, 1, &on, sizeof(on)) != noErr ||
	    AudioUnitSetProperty(st->unit, kAudioOutputUnitProperty_EnableIO,
	                         kAudioUnitScope_Output, 0, &off, sizeof(off)) != noErr)
		return bail("取り込みの口を開けない");

	if (AudioUnitSetProperty(st->unit, kAudioOutputUnitProperty_CurrentDevice,
	                         kAudioUnitScope_Global, 0, &dev, sizeof(dev)) != noErr)
		return bail("そのデバイスへ繋げない: " + device_label(dev));

	st->rate     = nominal_rate(dev);
	st->channels = std::min<u32>(input_channels(dev), 2);
	if (!st->channels)
		return bail("入力のチャンネルが無い");

	// こちらが受け取る形。float32 の非インタリーブ
	AudioStreamBasicDescription fmt{};
	fmt.mSampleRate       = st->rate;
	fmt.mFormatID         = kAudioFormatLinearPCM;
	fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
	                        kAudioFormatFlagIsNonInterleaved;
	fmt.mChannelsPerFrame = st->channels;
	fmt.mBitsPerChannel   = 32;
	fmt.mFramesPerPacket  = 1;
	fmt.mBytesPerFrame    = 4;
	fmt.mBytesPerPacket   = 4;
	if (AudioUnitSetProperty(st->unit, kAudioUnitProperty_StreamFormat,
	                         kAudioUnitScope_Output, 1, &fmt, sizeof(fmt)) != noErr)
		return bail("float 32bit を受け付けてもらえない");

	// **1 回に渡せるフレーム数を広げておくこと**（出力側と同じ落とし穴）
	UInt32 maxf = 4096, sz = sizeof(maxf);
	AudioUnitGetProperty(st->unit, kAudioDevicePropertyBufferFrameSize,
	                     kAudioUnitScope_Global, 0, &maxf, &sz);
	if (maxf < 512) maxf = 4096;
	AudioUnitSetProperty(st->unit, kAudioUnitProperty_MaximumFramesPerSlice,
	                     kAudioUnitScope_Global, 0, &maxf, sizeof(maxf));

	st->ablmem.assign(sizeof(AudioBufferList) + sizeof(AudioBuffer) * 2, 0);
	st->chan[0].assign(maxf, 0.0f);
	st->chan[1].assign(maxf, 0.0f);
	st->stage.reserve(size_t(maxf) * 2 + 16);

	AURenderCallbackStruct cbs{};
	cbs.inputProc = in_proc;
	cbs.inputProcRefCon = this;
	if (AudioUnitSetProperty(st->unit, kAudioOutputUnitProperty_SetInputCallback,
	                         kAudioUnitScope_Global, 0, &cbs, sizeof(cbs)) != noErr)
		return bail("取り込みの手続きを渡せない");

	g_in = st;
	g_owner = this;
	if (AudioUnitInitialize(st->unit) != noErr) { g_in = nullptr; return bail("初期化できない"); }
	if (AudioOutputUnitStart(st->unit) != noErr) { g_in = nullptr; return bail("録音を始められない"); }

	st->started = true;
	m_dev_name     = device_label(dev);
	m_dev_rate     = u32(st->rate);
	m_dev_channels = st->channels;
	m_dev_bits     = 32;
	m_dev_float    = true;
	m_running.store(true);
	m_start_state.store(1);
	return true;
}

void audio_in::stop()
{
	m_quit.store(true);
	in_stream *st = g_in;
	if (!st) {
		m_running.store(false);
		return;
	}
	g_in = nullptr;
	g_owner = nullptr;
	if (st->started)
		AudioOutputUnitStop(st->unit);
	AudioUnitUninitialize(st->unit);
	AudioComponentInstanceDispose(st->unit);
	delete st;
	m_running.store(false);
}

// Windows 側は自前のスレッドを回す。CoreAudio は自分で持つので使わない
void audio_in::run()
{
}

// 輪っかへ積む。積むのは取り込みの糸、取るのは音声の糸の一本ずつ
void audio_in::push_frames(const s16 *frames, u32 n)
{
	u32 wr = m_w.load(std::memory_order_relaxed);
	for (u32 i = 0; i < n; i++) {
		const u32 rd = m_r.load(std::memory_order_acquire);
		if (((wr + 1) & MASK) == rd)
			break;                       // 溢れた。取る側が追いついていない
		m_ring[wr * 2]     = frames[i * 2];
		m_ring[wr * 2 + 1] = frames[i * 2 + 1];
		wr = (wr + 1) & MASK;
		m_w.store(wr, std::memory_order_release);
	}
}

void audio_in::push(const s16 *frames, u32 n)
{
	push_frames(frames, n);
}

} // namespace ui
