// license:BSD-3-Clause
//
// macOS の MIDI 出力（CoreMIDI）。midi_out.cpp（Windows の WinMM）と同じ口を出す。
//
// **音声スレッドは待たせない。** 音声スレッドは輪っかにバイトを積むだけで、
// 送るのは別のスレッド。CoreMIDI の MIDISend は WinMM ほど詰まらないが、
// 相手が仮想の口（IAC や他のアプリ）だと巻き込まれることはあるので、
// Windows 側と同じ作りにしてある。
//
// バイトの列を 1 通ずつに切るのは ui::midi_split（AUv3 と同じもの）。

#include "midi_out.h"
#include "midi_split.h"

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
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

std::string endpoint_name(MIDIEndpointRef ep)
{
	std::string name;
	CFStringRef s = nullptr;
	if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &s) == noErr && s) {
		name = to_utf8(s);
		CFRelease(s);
	}
	if (name.empty() && MIDIObjectGetStringProperty(ep, kMIDIPropertyName, &s) == noErr && s) {
		name = to_utf8(s);
		CFRelease(s);
	}
	return name.empty() ? "?" : name;
}

MIDIClientRef client()
{
	static MIDIClientRef c = 0;
	if (!c && MIDIClientCreate(CFSTR("S-MU2000"), nullptr, nullptr, &c) != noErr)
		c = 0;
	return c;
}

// 送りスレッドを起こす合図。Windows のイベントの代わり。
// **閉じても捨てない**（積む側が掴み損ねないように）ので、ここに置いておく
struct waker {
	std::mutex m;
	std::condition_variable cv;
	bool flag = false;

	void poke()
	{
		{
			std::lock_guard<std::mutex> lock(m);
			flag = true;
		}
		cv.notify_one();
	}
	void wait_ms(int ms)
	{
		std::unique_lock<std::mutex> lock(m);
		cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return flag; });
		flag = false;
	}
};

// 開いている口。CoreMIDI の道具立てを隠す
struct out_port {
	MIDIPortRef     port = 0;
	MIDIEndpointRef dest = 0;
	waker           wake;
};

} // namespace


std::vector<std::string> midi_out::list()
{
	std::vector<std::string> out;
	const ItemCount n = MIDIGetNumberOfDestinations();
	for (ItemCount i = 0; i < n; i++)
		out.push_back(endpoint_name(MIDIGetDestination(i)));
	return out;
}

bool midi_out::open(int device, std::string &err)
{
	close();
	if (device < 0)
		return true;

	if (ItemCount(device) >= MIDIGetNumberOfDestinations()) {
		err = "その番号の MIDI 出力は無い";
		return false;
	}
	const MIDIClientRef c = client();
	if (!c) {
		err = "CoreMIDI を始められない";
		return false;
	}

	out_port *op = new out_port;
	op->dest = MIDIGetDestination(ItemCount(device));
	if (MIDIOutputPortCreate(c, CFSTR("S-MU2000 out"), &op->port) != noErr) {
		delete op;
		err = "MIDI の送り口を作れない";
		return false;
	}

	m_handle = op;
	m_wake   = &op->wake;
	m_name   = endpoint_name(op->dest);
	m_read.store(0);
	m_write.store(0);
	m_quit.store(false);
	m_stuck.store(false);
	m_thread_done.store(false);
	m_open.store(true);

	const unsigned gen = m_gen.load();
	m_thread = std::thread([this, gen, op] { run(gen, op); });
	return true;
}

void midi_out::close()
{
	if (!m_open.load() && !m_thread.joinable())
		return;

	m_open.store(false);
	m_gen.fetch_add(1);          // 置いていかれた古い糸が戻ってきても触らせない
	m_quit.store(true);
	if (m_wake)
		static_cast<waker *>(m_wake)->poke();

	if (m_thread.joinable()) {
		// 相手が固まっていたら置いていく（Windows 側と同じ振る舞い）
		for (int i = 0; i < 200 && !m_thread_done.load(std::memory_order_acquire); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		if (m_thread_done.load(std::memory_order_acquire))
			m_thread.join();
		else {
			m_stuck.store(true, std::memory_order_release);
			m_thread.detach();
		}
	}

	if (m_thread_done.load(std::memory_order_acquire) && m_handle) {
		out_port *op = static_cast<out_port *>(m_handle);
		MIDIPortDispose(op->port);
		delete op;
	}
	m_handle = nullptr;
	m_wake = nullptr;
	m_name.clear();
}

midi_out::~midi_out()
{
	close();
}

void midi_out::send(u8 v)
{
	if (!m_open.load(std::memory_order_acquire))
		return;
	const size_t w = m_write.load(std::memory_order_relaxed);
	const size_t next = (w + 1) & MASK;
	if (next == m_read.load(std::memory_order_acquire))
		return;                            // 溢れた。捨てる
	m_buf[w] = v;
	m_write.store(next, std::memory_order_release);
	if (m_wake)
		static_cast<waker *>(m_wake)->poke();
}

// 組み上がった 1 通を CoreMIDI へ
void midi_out::emit(void *handle, const u8 *p, size_t n)
{
	if (!handle || !p || !n)
		return;
	out_port *op = static_cast<out_port *>(handle);

	// SysEx は長いことがある。packet は 65536 バイトまで積める
	std::vector<Byte> storage(sizeof(MIDIPacketList) + n + 64);
	MIDIPacketList *list = reinterpret_cast<MIDIPacketList *>(storage.data());
	MIDIPacket *pk = MIDIPacketListInit(list);
	pk = MIDIPacketListAdd(list, storage.size(), pk, 0, n, p);
	if (pk)
		MIDISend(op->port, op->dest, list);
}

// 送りスレッド。輪っかから取り出し、1 通ずつに切って送る
void midi_out::run(unsigned gen, void *handle)
{
	out_port *op = static_cast<out_port *>(handle);
	midi_split split;
	split.reset();

	struct ctx_t { midi_out *self; void *handle; } ctx{ this, handle };
	auto emit_one = [](void *c, const u8 *bytes, size_t n) {
		ctx_t *x = static_cast<ctx_t *>(c);
		x->self->emit(x->handle, bytes, n);
	};

	for (;;) {
		op->wake.wait_ms(50);

		for (;;) {
			if (m_gen.load() != gen) {          // 置いていかれた。もう新しい口のもの
				m_thread_done.store(true, std::memory_order_release);
				return;
			}
			const size_t r = m_read.load(std::memory_order_relaxed);
			if (r == m_write.load(std::memory_order_acquire))
				break;
			const u8 b = m_buf[r];
			m_read.store((r + 1) & MASK, std::memory_order_release);
			split.feed(&b, 1, emit_one, &ctx);
		}

		if (m_quit.load(std::memory_order_acquire))
			break;
	}
	m_thread_done.store(true, std::memory_order_release);
}

} // namespace ui
