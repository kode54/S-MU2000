// license:BSD-3-Clause
//
// macOS の MIDI 入力（CoreMIDI）。midi_in.cpp（Windows の midiInOpen）と同じ口を出す。
// 輪っかへの積み方は midi_in_common.cpp が持っている。
//
// CoreMIDI は Windows と違って「短い」と「SysEx」を分けない。MIDIPacket の中に
// 生のバイト列がそのまま入ってくる（複数のメッセージが 1 つの packet に
// 詰まっていることもある）ので、こちらでステータスを見て切り分け、
// on_short() と on_long() へ振り分ける。
//
// **入力の口は自分で作る。** CoreMIDI では「送り口（source）」に
// 「受け口（input port）」を繋いで受ける。source は挿し直すと番号がずれるので、
// 呼び出し側は名前で選ぶ（list() の順と open() の番号は同じ）。

#include "midi_in.h"

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#include <string>
#include <vector>

namespace ui {

namespace {

// CFString を UTF-8 へ
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

// 人が読む名前。装置名とエンドポイント名を繋げる（「IAC ドライバ バス 1」など）
std::string endpoint_name(MIDIEndpointRef ep)
{
	std::string name;
	CFStringRef s = nullptr;
	// kMIDIPropertyDisplayName は装置名まで込みで返す
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

// このプロセスの CoreMIDI クライアント。一度だけ作る
MIDIClientRef client()
{
	static MIDIClientRef c = 0;
	if (!c) {
		CFStringRef n = CFSTR("S-MU2000");
		if (MIDIClientCreate(n, nullptr, nullptr, &c) != noErr)
			c = 0;
	}
	return c;
}

// 受け口に届いた packet を、メッセージの切れ目で分けて midi_in へ渡す。
//
// CoreMIDI は 1 つの packet に複数のメッセージを詰めてよいことになっている。
// ランニングステータス（ステータスを省いた続き）も来うるので、直前のステータスを覚えておく。
// SysEx は packet を跨いで続くので、F0 を見たら F7 まで on_long() に流し続ける
struct splitter {
	midi_in *owner = nullptr;
	u8   running = 0;       // 覚えているステータス（0 なら無し）
	bool in_sysex = false;

	// そのステータスに続くデータバイトの数
	static int data_bytes(u8 status)
	{
		switch (status & 0xf0) {
		case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xe0: return 2;
		case 0xc0: case 0xd0: return 1;
		default: break;
		}
		switch (status) {
		case 0xf1: case 0xf3: return 1;   // MTC クォーターフレーム、ソングセレクト
		case 0xf2: return 2;              // ソングポジション
		default: return 0;
		}
	}

	void feed(const u8 *p, size_t n)
	{
		size_t i = 0;
		while (i < n) {
			const u8 b = p[i];

			// リアルタイム（F8-FF）はどこに挟まってもよい。単独で流す
			if (b >= 0xf8) {
				owner->on_short(b);
				i++;
				continue;
			}

			if (in_sysex) {
				// F7 までを丸ごと on_long() へ。途中に現れたステータスでも終わる
				size_t j = i;
				while (j < n && p[j] < 0x80)
					j++;
				const bool end = j < n && p[j] == 0xf7;
				if (end)
					j++;
				owner->on_long(p + i, j - i);
				i = j;
				if (end || (i < n && p[i] >= 0x80))
					in_sysex = false;
				continue;
			}

			if (b == 0xf0) {
				in_sysex = true;
				running = 0;
				// F0 も含めて on_long() へ渡す（区切りの判定に使っている）
				size_t j = i + 1;
				while (j < n && p[j] < 0x80)
					j++;
				const bool end = j < n && p[j] == 0xf7;
				if (end)
					j++;
				owner->on_long(p + i, j - i);
				i = j;
				if (end)
					in_sysex = false;
				continue;
			}

			// ここからは通常のメッセージ
			u8 status;
			if (b >= 0x80) {
				status = b;
				i++;
				// F1-F7 はランニングステータスを持たない
				running = (status < 0xf0) ? status : 0;
			} else if (running) {
				status = running;       // ランニングステータス。ステータスは前のまま
			} else {
				i++;                    // 宙ぶらりんのデータバイト。捨てる
				continue;
			}

			const int want = data_bytes(status);
			if (i + size_t(want) > n)
				return;                 // 途中で切れている。次の packet を待たずに捨てる

			u32 msg = status;
			for (int k = 0; k < want; k++)
				msg |= u32(p[i + size_t(k)]) << (8 * (k + 1));
			i += size_t(want);
			owner->on_short(msg);
		}
	}
};

// 開いている口ごとの状態。midi_in は 1 本しか開かないので 1 つでよいが、
// コールバックへ渡す入れ物が要る
struct port_state {
	splitter      split;
	MIDIPortRef   port = 0;
	MIDIEndpointRef src = 0;
};

void read_proc(const MIDIPacketList *pkts, void *ref, void *)
{
	port_state *st = static_cast<port_state *>(ref);
	if (!st || !st->split.owner)
		return;
	const MIDIPacket *p = &pkts->packet[0];
	for (UInt32 i = 0; i < pkts->numPackets; i++) {
		st->split.feed(p->data, p->length);
		p = MIDIPacketNext(p);
	}
}

} // namespace


std::vector<std::string> midi_in::list()
{
	std::vector<std::string> out;
	const ItemCount n = MIDIGetNumberOfSources();
	for (ItemCount i = 0; i < n; i++)
		out.push_back(endpoint_name(MIDIGetSource(i)));
	return out;
}

bool midi_in::open(int device, std::string &err)
{
	close();
	if (device < 0)
		return true;

	const ItemCount n = MIDIGetNumberOfSources();
	if (ItemCount(device) >= n) {
		err = "その番号の MIDI 入力は無い";
		return false;
	}
	const MIDIClientRef c = client();
	if (!c) {
		err = "CoreMIDI を始められない";
		return false;
	}

	port_state *st = new port_state;
	st->split.owner = this;
	st->src = MIDIGetSource(ItemCount(device));

	if (MIDIInputPortCreate(c, CFSTR("S-MU2000 in"), read_proc, st, &st->port) != noErr) {
		delete st;
		err = "MIDI の受け口を作れない";
		return false;
	}
	if (MIDIPortConnectSource(st->port, st->src, nullptr) != noErr) {
		MIDIPortDispose(st->port);
		delete st;
		err = "MIDI 入力に繋げない";
		return false;
	}

	m_handle = st;
	m_name = endpoint_name(st->src);
	m_closing.store(false, std::memory_order_release);
	return true;
}

// Windows だけの仕掛け（SysEx の入れ物を OS へ返す）。CoreMIDI には要らない
void midi_in::requeue(void *)
{
}

void midi_in::close()
{
	if (!m_handle)
		return;
	m_closing.store(true, std::memory_order_release);

	port_state *st = static_cast<port_state *>(m_handle);
	MIDIPortDisconnectSource(st->port, st->src);
	MIDIPortDispose(st->port);
	delete st;

	m_handle = nullptr;
	m_name.clear();
}

} // namespace ui
