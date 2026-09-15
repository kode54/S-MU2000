// license:BSD-3-Clause
//
// S-MU2000: 小さな x86-64 の組み立て器。MEG の JIT（swp30_jit.cpp）と SH2 の JIT（sh2_jit.cpp）で使う。
// x86-64 で使う。呼び出し規約は Windows x64 と SysV（macOS・Linux）の両方。生成したコードから
// 関数を呼ぶときの引数は ARG0〜ARG2 に置く。どちらの規約でも影 32 バイトと 16 の倍数の
// スタック合わせは呼ぶ側で済ませてあるので、call_abs は規約の違いを知らない。

#ifndef S_MU2000_X64ASM_H
#define S_MU2000_X64ASM_H

#pragma once

#include "mamecompat.h"

#include <cstring>
#include <initializer_list>
#include <vector>

namespace x64asm {

enum : u8 { RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15, NOREG = 0xff };

// Callee argument registers 1-4. Windows x64: rcx/rdx/r8/r9; SysV: rdi/rsi/rdx/rcx
#ifdef _WIN32
constexpr u8 ARG0 = RCX, ARG1 = RDX, ARG2 = R8, ARG3 = R9;
constexpr bool sysv_abi = false;
#else
constexpr u8 ARG0 = RDI, ARG1 = RSI, ARG2 = RDX, ARG3 = RCX;
constexpr bool sysv_abi = true;
#endif

// Type to receive the first argument on the C++ side (for the selftest's function
// pointers; the return value is rax under either ABI, so it does not differ)
#ifdef _WIN32
using x64_arg0_t = u32;   // ecx; the caller knows the callee ignores the upper 32 bits
#else
using x64_arg0_t = u64;   // rdi
#endif

struct mem {
	u8 base;
	u8 index = NOREG;
	u8 scale = 1;
	s32 disp = 0;
};

class assembler
{
public:
	std::vector<u8> code;

	void byte(u8 b) { code.push_back(b); }
	void d32(u32 v) { for (int i = 0; i < 4; i++) byte(u8(v >> (8 * i))); }
	void d64(u64 v) { for (int i = 0; i < 8; i++) byte(u8(v >> (8 * i))); }

	// 命令の本体。prefix（0 なら無し）、REX.W、命令バイト列、ModRM の reg 欄、相手
	void rr(u8 prefix, bool w, std::initializer_list<u8> opc, u8 reg, u8 rm)
	{
		if (prefix) byte(prefix);
		u8 rex = 0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | ((rm >> 3) & 1);
		if (rex != 0x40) byte(rex);
		for (u8 o : opc) byte(o);
		byte(u8(0xc0 | ((reg & 7) << 3) | (rm & 7)));
	}
	void rm(u8 prefix, bool w, std::initializer_list<u8> opc, u8 reg, const mem &m)
	{
		if (prefix) byte(prefix);
		const u8 x = m.index == NOREG ? 0 : (m.index >> 3) & 1;
		u8 rex = 0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | x << 1 | ((m.base >> 3) & 1);
		if (rex != 0x40) byte(rex);
		for (u8 o : opc) byte(o);
		// いつも disp32 の形にする（長さが決まっていて楽）
		if (m.index == NOREG && (m.base & 7) != RSP) {
			byte(u8(0x80 | ((reg & 7) << 3) | (m.base & 7)));
		} else {
			byte(u8(0x80 | ((reg & 7) << 3) | 4));
			const u8 ss = m.scale == 1 ? 0 : m.scale == 2 ? 1 : m.scale == 4 ? 2 : 3;
			const u8 idx = m.index == NOREG ? 4 : (m.index & 7);
			byte(u8((ss << 6) | (idx << 3) | (m.base & 7)));
		}
		d32(u32(m.disp));
	}

	void mov64(u8 d, u8 s)            { rr(0, true, {0x8b}, d, s); }
	void load64(u8 d, const mem &m)   { rm(0, true, {0x8b}, d, m); }
	void store64(const mem &m, u8 s)  { rm(0, true, {0x89}, s, m); }
	void load32(u8 d, const mem &m)   { rm(0, false, {0x8b}, d, m); }
	void store32(const mem &m, u8 s)  { rm(0, false, {0x89}, s, m); }
	void loads32(u8 d, const mem &m)  { rm(0, true, {0x63}, d, m); }          // movsxd
	void loads16(u8 d, const mem &m)  { rm(0, true, {0x0f, 0xbf}, d, m); }    // movsx r64, m16
	void loadu16(u8 d, const mem &m)  { rm(0, false, {0x0f, 0xb7}, d, m); }   // movzx r32, m16
	void loadu8(u8 d, const mem &m)   { rm(0, false, {0x0f, 0xb6}, d, m); }   // movzx r32, m8
	void store16(const mem &m, u8 s)  { rm(0x66, false, {0x89}, s, m); }
	void store8i(const mem &m, u8 v)  { rm(0, false, {0xc6}, 0, m); byte(v); }
	void imm64(u8 d, u64 v)
	{
		byte(u8(0x48 | ((d >> 3) & 1)));
		byte(u8(0xb8 | (d & 7)));
		d64(v);
	}
	void imm32(u8 d, u32 v)
	{
		if (d >= 8) byte(0x41);
		byte(u8(0xb8 | (d & 7)));
		d32(v);
	}
	void add64(u8 d, u8 s) { rr(0, true, {0x01}, s, d); }
	void sub64(u8 d, u8 s) { rr(0, true, {0x29}, s, d); }
	void and64(u8 d, u8 s) { rr(0, true, {0x21}, s, d); }
	void cmp64(u8 a, u8 b) { rr(0, true, {0x39}, b, a); }          // cmp a, b
	void test64(u8 a, u8 b) { rr(0, true, {0x85}, b, a); }
	void test32(u8 a, u8 b) { rr(0, false, {0x85}, b, a); }
	void xor32(u8 d, u8 s) { rr(0, false, {0x31}, s, d); }
	void add32(u8 d, u8 s) { rr(0, false, {0x01}, s, d); }
	void sub32(u8 d, u8 s) { rr(0, false, {0x29}, s, d); }
	void and32(u8 d, u8 s) { rr(0, false, {0x21}, s, d); }
	void imul64(u8 d, u8 s) { rr(0, true, {0x0f, 0xaf}, d, s); }
	void imul32i(u8 d, u8 s, u32 v) { rr(0, false, {0x69}, d, s); d32(v); }
	void imul64i(u8 d, u8 s, u32 v) { rr(0, true, {0x69}, d, s); d32(v); }
	void add32i(u8 d, u32 v) { rr(0, false, {0x81}, 0, d); d32(v); }
	void and32i(u8 d, u32 v) { rr(0, false, {0x81}, 4, d); d32(v); }
	void shl64(u8 d, u8 n) { rr(0, true, {0xc1}, 4, d); byte(n); }
	void sar64(u8 d, u8 n) { rr(0, true, {0xc1}, 7, d); byte(n); }
	void shl32(u8 d, u8 n) { rr(0, false, {0xc1}, 4, d); byte(n); }
	void sar32(u8 d, u8 n) { rr(0, false, {0xc1}, 7, d); byte(n); }
	void rol32(u8 d, u8 n) { rr(0, false, {0xc1}, 0, d); byte(n); }
	void neg64(u8 d) { rr(0, true, {0xf7}, 3, d); }
	void cmovl64(u8 d, u8 s) { rr(0, true, {0x0f, 0x4c}, d, s); }
	void cmovg64(u8 d, u8 s) { rr(0, true, {0x0f, 0x4f}, d, s); }
	void cmovs64(u8 d, u8 s) { rr(0, true, {0x0f, 0x48}, d, s); }
	void cmove64(u8 d, u8 s) { rr(0, true, {0x0f, 0x44}, d, s); }
	void cmp64ri(u8 d, u32 v) { rr(0, true, {0x81}, 7, d); d32(v); }
	void setl_mem(const mem &m) { rm(0, false, {0x0f, 0x9c}, 0, m); }
	void sete_mem(const mem &m) { rm(0, false, {0x0f, 0x94}, 0, m); }
	void call_reg(u8 r) { rr(0, false, {0xff}, 2, r); }
	void push(u8 r) { if (r >= 8) byte(0x41); byte(u8(0x50 | (r & 7))); }
	void pop(u8 r)  { if (r >= 8) byte(0x41); byte(u8(0x58 | (r & 7))); }
	void subrsp(u32 v) { rr(0, true, {0x81}, 5, RSP); d32(v); }
	void addrsp(u32 v) { rr(0, true, {0x81}, 0, RSP); d32(v); }
	void ret() { byte(0xc3); }
	// jz で先へ飛ぶ。飛び先は後で patch() で埋める
	size_t jz_fwd() { byte(0x0f); byte(0x84); d32(0); return code.size(); }
	void patch(size_t at) { const u32 rel = u32(code.size() - at); std::memcpy(&code[at - 4], &rel, 4); }
	void call_abs(void *fn) { imm64(RAX, u64(uintptr_t(fn))); call_reg(RAX); }

	// ---- SH2 の JIT で足したもの ----
	void store32i(const mem &m, u32 v) { rm(0, false, {0xc7}, 0, m); d32(v); }   // mov dword [m], imm32
	void cmp32i_mem(const mem &m, u32 v) { rm(0, false, {0x81}, 7, m); d32(v); } // cmp dword [m], imm32
	void sub32i_mem(const mem &m, u32 v) { rm(0, false, {0x81}, 5, m); d32(v); } // sub dword [m], imm32
	// 条件分岐（cc は 0x84 = je、0x85 = jne、0x8e = jle など、0F の後ろのバイト）。飛び先は後で patch() で埋める
	size_t jcc_fwd(u8 cc) { byte(0x0f); byte(cc); d32(0); return code.size(); }
	size_t jmp_fwd() { byte(0xe9); d32(0); return code.size(); }
	// at（jcc_fwd / jmp_fwd の戻り値）の飛び先を target にする
	void patch_to(size_t at, size_t target) { const u32 rel = u32(target - at); std::memcpy(&code[at - 4], &rel, 4); }
	void mov32(u8 d, u8 s) { rr(0, false, {0x8b}, d, s); }
	// r32 と [m] の演算（結果は r32 か [m]）
	void add32rm(u8 d, const mem &m) { rm(0, false, {0x03}, d, m); }
	void cmp32rm(u8 d, const mem &m) { rm(0, false, {0x3b}, d, m); }     // cmp r, [m]
	void test32rm(u8 d, const mem &m) { rm(0, false, {0x85}, d, m); }
	void add32mr(const mem &m, u8 s) { rm(0, false, {0x01}, s, m); }     // add [m], r
	void sub32mr(const mem &m, u8 s) { rm(0, false, {0x29}, s, m); }
	void and32mr(const mem &m, u8 s) { rm(0, false, {0x21}, s, m); }
	void or32mr(const mem &m, u8 s)  { rm(0, false, {0x09}, s, m); }
	void xor32mr(const mem &m, u8 s) { rm(0, false, {0x31}, s, m); }
	void add32i_mem(const mem &m, u32 v) { rm(0, false, {0x81}, 0, m); d32(v); }
	void or32i_mem(const mem &m, u32 v)  { rm(0, false, {0x81}, 1, m); d32(v); }
	void and32i_mem(const mem &m, u32 v) { rm(0, false, {0x81}, 4, m); d32(v); }
	void xor32i_mem(const mem &m, u32 v) { rm(0, false, {0x81}, 6, m); d32(v); }
	void test32i_mem(const mem &m, u32 v) { rm(0, false, {0xf7}, 0, m); d32(v); }
	void shl32i_mem(const mem &m, u8 n) { rm(0, false, {0xc1}, 4, m); byte(n); }
	void shr32i_mem(const mem &m, u8 n) { rm(0, false, {0xc1}, 5, m); byte(n); }
	void loads8_32(u8 d, const mem &m)  { rm(0, false, {0x0f, 0xbe}, d, m); }  // movsx r32, byte [m]
	void loads16_32(u8 d, const mem &m) { rm(0, false, {0x0f, 0xbf}, d, m); }  // movsx r32, word [m]
	void store8(const mem &m, u8 s) { rm(0, false, {0x88}, s, m); }            // s は AL/CL/DL/BL か R8B 以上
	void movzx8(u8 d, u8 s)  { rr(0, false, {0x0f, 0xb6}, d, s); }
	void movzx16(u8 d, u8 s) { rr(0, false, {0x0f, 0xb7}, d, s); }
	void movsx8(u8 d, u8 s)  { rr(0, false, {0x0f, 0xbe}, d, s); }
	void movsx16(u8 d, u8 s) { rr(0, false, {0x0f, 0xbf}, d, s); }
	void imul32(u8 d, u8 s) { rr(0, false, {0x0f, 0xaf}, d, s); }
	void add32ri(u8 d, u32 v) { rr(0, false, {0x81}, 0, d); d32(v); }
	void sub32ri(u8 d, u32 v) { rr(0, false, {0x81}, 5, d); d32(v); }
	void cmp32ri(u8 d, u32 v) { rr(0, false, {0x81}, 7, d); d32(v); }
	void test32ri(u8 d, u32 v) { rr(0, false, {0xf7}, 0, d); d32(v); }
	void shr32(u8 d, u8 n) { rr(0, false, {0xc1}, 5, d); byte(n); }
	void neg32(u8 d) { rr(0, false, {0xf7}, 3, d); }
	void not32(u8 d) { rr(0, false, {0xf7}, 2, d); }
	void bswap32(u8 r) { if (r >= 8) byte(0x41); byte(0x0f); byte(u8(0xc8 | (r & 7))); }
	// setcc al など（cc は 0x94 = sete、0x9d = setge、0x9f = setg、0x93 = setae、0x97 = seta）
	void setcc(u8 cc, u8 r) { rr(0, false, {0x0f, cc}, 0, r); }
	void lea32(u8 d, const mem &m) { rm(0, false, {0x8d}, d, m); }
	void or32(u8 d, u8 s) { rr(0, false, {0x09}, s, d); }
	void or32ri(u8 d, u32 v)  { rr(0, false, {0x81}, 1, d); d32(v); }
	void xor32ri(u8 d, u32 v) { rr(0, false, {0x81}, 6, d); d32(v); }
	void shl32cl(u8 d) { rr(0, false, {0xd3}, 4, d); }
	void shr32cl(u8 d) { rr(0, false, {0xd3}, 5, d); }
	void bsr32(u8 d, u8 s) { rr(0, false, {0x0f, 0xbd}, d, s); }   // s が 0 のときは使わないこと
};

} // namespace x64asm

#endif
