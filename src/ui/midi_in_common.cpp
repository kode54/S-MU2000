// license:BSD-3-Clause
//
// MIDI 入力のうち、機種に依らないところ。
//
// 線から来たバイトを輪っかに積み、音声スレッドが 1 バイトずつ取り出す。
// 積むのは OS のコールバック（Windows なら midiInOpen、macOS なら CoreMIDI）、
// 読むのは音声スレッドの一本ずつ。**この切り分けは機種で変わらない**ので、
// midi_in.cpp（Windows）と midi_in_mac.cpp（macOS）の両方からここを使う。

#include "midi_in.h"

namespace ui {

// 短いメッセージ（Windows の MIM_DATA、CoreMIDI の 1 パケット）。
// 下位バイトからステータス・データ 1・データ 2 の順に詰まっている
void midi_in::on_short(u32 p1)
{
	const u8 status = u8(p1);
	if (status < 0x80)
		return;
	// リアルタイム（クロックなど）は SysEx の途中に挟まってもよい決まり。
	// SysEx を待っている間は、SysEx と一緒に読ませる
	if (status >= 0xf8) {
		push(status);
		if (!m_in_sysex)
			commit();
		return;
	}
	// それ以外が来たら、線の上では SysEx はそこで終わっている
	if (m_in_sysex) {
		m_in_sysex = false;
		commit();
	}
	// 長さは種別で決まる。プログラムチェンジとチャンネルプレッシャだけ 1 バイト
	int n = 3;
	const u8 kind = status & 0xf0;
	if (kind == 0xc0 || kind == 0xd0) n = 2;
	if (status == 0xf1 || status == 0xf3) n = 2;
	else if (status == 0xf2) n = 3;
	else if (status >= 0xf4 && status < 0xf8) n = 1;

	push(status);
	if (n > 1) push(u8(p1 >> 8));
	if (n > 2) push(u8(p1 >> 16));
	commit();
}

// SysEx の切れ端。長いものは何枚かに分かれて届くので、F7 まで読ませない
void midi_in::on_long(const u8 *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		const u8 v = p[i];
		if (v == 0xf0) {
			if (m_in_sysex)
				commit();                 // F7 の無いまま次が始まった。そこまでで区切る
			m_in_sysex = true;
		}
		if (!m_in_sysex)
			continue;                     // F0 より前の半端なバイト。どこにも属さない
		push(v);
		if (v == 0xf7) {
			m_in_sysex = false;
			commit();
		}
	}
}

void midi_in::push(u8 v)
{
	if (m_overflow)
		return;
	const size_t next = (m_pending + 1) & MASK;
	if (next == m_read.load(std::memory_order_acquire)) {
		m_overflow = true;            // 溢れ。実機の受信バッファ溢れと同じ。このメッセージは捨てる
		return;
	}
	m_buf[m_pending] = v;
	m_pending = next;
	m_bytes.fetch_add(1, std::memory_order_relaxed);
}

void midi_in::commit()
{
	if (m_overflow) {
		rollback();
		return;
	}
	m_write.store(m_pending, std::memory_order_release);
}

void midi_in::rollback()
{
	m_pending = m_write.load(std::memory_order_relaxed);
	m_overflow = false;
}

bool midi_in::pop(u8 &v)
{
	const size_t r = m_read.load(std::memory_order_relaxed);
	if (r == m_write.load(std::memory_order_acquire))
		return false;
	v = m_buf[r];
	m_read.store((r + 1) & MASK, std::memory_order_release);
	return true;
}

} // namespace ui
