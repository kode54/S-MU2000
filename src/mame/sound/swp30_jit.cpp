// license:BSD-3-Clause
//
// S-MU2000: MEG のプログラムをその場で x86-64 の機械語にする（JIT）。
//
// firmware が MEG に書いたプログラムを、プログラムか番地の割り当てが変わるたびに訳し直す。
// 訳した機械語は meg_state::run_program() と**ビット単位で同じこと**をする:
//
//   * 命令ごとの判定（ALU の種類・読み先・書き先・メモリ操作）は訳すときに決める
//   * 3 命令遅れの書き込み・2 命令遅れのメモリポートと t の値は、遅れの輪のどの枠かが
//     訳すときに決まるので、反映する命令の位置に直に置く。サンプルを跨ぐ分
//     （頭の 3 命令が読む枠と、終わりの 3 命令が書く枠）だけ輪そのものを使う
//   * 定数・番地表・LFO は firmware が動かしている最中にも書くので、実行時に読む
//   * 乱数（ディザ）は同じ順に同じ回数だけ引く
//
// 分岐（bit 0x3f）を含むプログラム（LO-FI と DYNA 系）は、遅れの輪を毎命令で読み書きする形で訳す。
// 訳した物はどこにも保存しない（firmware 由来のものを配らない。実行時に作って捨てる）。
//
// x86-64（Windows・macOS・Linux）と arm64 で使う。
// そのほかでは build() が false を返し、今までどおり解釈実行する。

#include "swp30.h"

#if defined(__x86_64__) || defined(__aarch64__)
#define SMU2000_MEG_JIT 1
#include "compat/exec_mem.h"
#ifdef __aarch64__
#include "a64asm.h"
#else
#include "x64asm.h"
#endif
#else
#define SMU2000_MEG_JIT 0
#endif

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

#if defined(__x86_64__)

using namespace x64asm;
using meg_asm = assembler;
using meg_arg_t = x64_arg0_t;

// meg_state::revram_encode と同じことをする。入力 eax（u32）、出力 eax（u16）。rcx rdx r11 を壊す。
// 分岐を使わない。符号は音の値しだいで読めないので、分岐にすると予測の外れで遅くなる
void emit_revram_encode(assembler &a)
{
	a.and32i(RAX, 0x7ffffff);
	a.mov32(RDX, RAX);
	a.shl32(RDX, 5);
	a.sar32(RDX, 31);                                    // bit 26 が立っていれば -1
	a.mov32(RCX, RDX);
	a.and32i(RCX, 0x7ffffff);
	a.xor32(RAX, RCX);
	a.and32i(RDX, 1);                                    // s
	// e は bit 11〜25 のうち一番上の 1 の位置 - 10。無ければ e = 0 で m = v（v < 0x800）
	a.mov32(RCX, RAX);
	a.or32ri(RCX, 0x400);
	a.bsr32(RCX, RCX);
	a.sub32ri(RCX, 10);                                  // e
	a.xor32(R11, R11);
	a.test32(RCX, RCX);
	a.setcc(0x95, R11);                                  // e != 0
	a.sub32(RCX, R11);                                   // e ? e - 1 : 0
	a.add32(R11, RCX);                                   // e
	a.shr32cl(RAX);
	a.and32i(RAX, 0x7ff);
	a.shl32(R11, 12);
	a.or32(RAX, R11);
	a.shl32(RDX, 11);
	a.or32(RAX, RDX);
}

// meg_state::m1_expand と同じことをする。入力 eax（下の 16bit が s16）、出力 rax（0〜0x7ffc）。rcx を壊す
void emit_m1_expand(assembler &a)
{
	a.test32ri(RAX, 0x8000);
	const size_t neg = a.jcc_fwd(0x85);
	a.mov32(RCX, RAX);
	a.shr32(RCX, 12);
	a.and32i(RCX, 7);                                    // s
	a.and32i(RAX, 0xfff);
	a.or32ri(RAX, 0x1000);
	a.cmp32ri(RCX, 5);
	const size_t done1 = a.jcc_fwd(0x84);                // s == 5
	const size_t less = a.jcc_fwd(0x82);                 // s < 5（jb）
	a.sub32ri(RCX, 5);
	a.shl32cl(RAX);
	const size_t done2 = a.jmp_fwd();
	a.patch(less);
	a.neg32(RCX);
	a.add32ri(RCX, 5);
	a.shr32cl(RAX);
	const size_t done3 = a.jmp_fwd();
	a.patch(neg);
	a.xor32(RAX, RAX);
	a.patch(done1);
	a.patch(done2);
	a.patch(done3);
}

// meg_state::revram_decode と同じことをする。入力 eax（u16）、出力 eax。rcx rdx r8 を壊す。分岐を使わない
void emit_revram_decode(assembler &a)
{
	a.mov32(R8, RAX);                                    // v
	a.mov32(RCX, RAX);
	a.shr32(RCX, 12);                                    // e
	a.and32i(RAX, 0x7ff);                                // m
	a.xor32(RDX, RDX);
	a.test32(RCX, RCX);
	a.setcc(0x95, RDX);                                  // e != 0
	a.sub32(RCX, RDX);                                   // e ? e - 1 : 0
	a.shl32(RDX, 11);
	a.or32(RAX, RDX);                                    // e ? m | 0x800 : m
	a.shl32cl(RAX);
	a.imm32(RDX, 0xffffffff);
	a.shl32cl(RDX);                                      // 反転の範囲
	a.mov32(RCX, R8);
	a.shl32(RCX, 20);
	a.sar32(RCX, 31);                                    // s ? -1 : 0
	a.and32(RDX, RCX);
	a.xor32(RAX, RDX);
}

#elif defined(__aarch64__)

using namespace a64;
using meg_asm = emitter;
using meg_arg_t = u32;

// The three helpers work on the same scratch registers the JIT uses, so one can
// be dropped into a block exactly as it stands: HA carries the value in and
// out, HC/HD/HM are scratch. The W forms are used throughout because the JIT's
// x86 counterpart also works on the low 32 bits and the W writes clear the top
// half of the X register, which is what the callers expect to read back.
enum : u8 { HA = X2, HC = X3, HD = X4, HE = X5, HM = X7 };

// Same as meg_state::revram_encode. Input HA (u32), result HA (low 16 bits).
// HC and HD are clobbered.
void emit_revram_encode(emitter &a)
{
	a.and_imm(HA, HA, 0x7ffffff);
	a.eor_reg(HD, HD, HD);                               // s
	a.tst_imm(HA, 0x4000000);
	const size_t pos = a.b_cond(EQ);
	a.eor_imm(HA, HA, 0x7ffffff);
	a.mov_imm32(HD, 1);
	a.patch(pos);
	// e is the highest set bit minus 10 when the value reaches bit 11; below
	// that e = 0 and m is the value itself
	a.lsr_imm(HC, HA, 11);
	const size_t small = a.cbz_w(HC);
	a.clz(HC, HA);
	a.mov_imm32(HM, 31);
	a.sub_reg(HC, HM, HC);                               // bsr(v)
	a.sub_imm(HC, HC, 11);                               // e - 1
	a.lsrv(HA, HA, HC);
	a.and_imm(HA, HA, 0x7ff);
	a.add_imm(HC, HC, 1);                                // e
	a.lsl_imm(HC, HC, 12);
	a.orr_reg(HA, HA, HC);
	a.patch(small);
	a.lsl_imm(HD, HD, 11);
	a.orr_reg(HA, HA, HD);
}

// Same as meg_state::m1_expand. Input HA (a sign-extended s16 in the low half),
// result HA (0..0x7ffc). HC is clobbered.
void emit_m1_expand(emitter &a)
{
	a.tst_imm(HA, 0x8000);
	const size_t neg = a.b_cond(NE);
	a.lsr_imm(HC, HA, 12);
	a.and_imm(HC, HC, 7);                                // s
	a.and_imm(HA, HA, 0xfff);
	a.or_imm(HA, HA, 0x1000);
	a.cmp_imm(HC, 5);
	const size_t done1 = a.b_cond(EQ);
	const size_t less = a.b_cond(LO);
	a.sub_imm(HC, HC, 5);
	a.lslv(HA, HA, HC);
	const size_t done2 = a.b();
	a.patch(less);
	a.neg_reg(HC, HC);
	a.add_imm(HC, HC, 5);
	a.lsrv(HA, HA, HC);
	const size_t done3 = a.b();
	a.patch(neg);
	a.eor_reg(HA, HA, HA);
	a.patch(done1);
	a.patch(done2);
	a.patch(done3);
}

// Same as meg_state::revram_decode. Input HA (u16), result HA.
//
// The C++ version inverts every bit when e = 0, so that a negative value
// round-trips through revram_encode (doc/upstream.md #18). The inversion mask
// is therefore built at run time rather than being one constant per branch.
// HC, HD and HE are clobbered.
void emit_revram_decode(emitter &a)
{
	a.mov_reg(HE, HA);                                   // v
	a.lsr_imm(HC, HA, 12);                               // e
	a.and_imm(HA, HA, 0x7ff);                            // m
	a.cmp_imm(HC, 0);
	a.cset(HD, NE);                                      // e != 0
	a.sub_reg(HC, HC, HD);                               // e ? e - 1 : 0
	a.lsl_imm(HD, HD, 11);
	a.orr_reg(HA, HA, HD);                               // e ? m | 0x800 : m
	a.lslv(HA, HA, HC);
	a.mov_imm32(HD, 0xffffffff);
	a.lslv(HD, HD, HC);                                  // the inversion mask
	a.lsl_imm(HC, HE, 20);
	a.sar_imm(HC, HC, 31);                               // s ? -1 : 0
	a.and_reg(HD, HD, HC);
	a.eor_reg(HA, HA, HD);
}

#endif

} // namespace


struct swp30_device::meg_jit {
	using fn_t = void (*)(meg_state *, swp30_device *, u16 *);

	// 訳した機械語 1 本
	struct code {
		fn_t fn = nullptr;
		void *buf = nullptr;
		size_t buf_size = 0;
		u32 d3 = 0, d2 = 0;
		~code()
		{
#if SMU2000_MEG_JIT
			if (buf)
				exec_mem::free_mem(buf, buf_size);
#endif
		}
	};

	// gen: 定数を実行時に読む版（いつでも使える）。spec: 今の定数を焼き込んだ版。
	// 定数は firmware がエフェクトを組むときにまとめて書き、そのあとはほとんど変わらない。
	// 変わってから STABLE サンプルのあいだ動かなければ spec を作り、次に変わった瞬間に gen へ戻す
	code gen, spec;
	const meg_state::op *ops = nullptr;
	u32 spec_const_gen = 0;
	u32 seen_const_gen = 0;
	u32 stable = 0;
	bool spec_tried = false;
	static constexpr u32 STABLE = 8192;

#if SMU2000_MEG_JIT
	static u32 call_lfo(meg_state *ms, u32 lfo) { return ms->get_lfo(int(lfo)); }
#endif

	bool build(code &c, meg_state &ms, const meg_state::op *ops, swp30_device &swp, bool bake);
};

void swp30_device::meg_jit_delete(meg_jit *j)
{
	delete j;
}

bool swp30_device::meg_jit_enabled()
{
#if SMU2000_MEG_JIT
	static const bool on = [] {
		const char *e = std::getenv("SMU2000_MEG_JIT");
		return !(e && e[0] == '0');
	}();
	return on;
#else
	return false;
#endif
}

void swp30_device::meg_jit_rebuild()
{
	if (!meg_jit_enabled()) {
		m_jit.reset();
		return;
	}
	if (!m_jit)
		m_jit.reset(new meg_jit);
	meg_jit &j = *m_jit;
	j.ops = m_meg_ops.data();
	if (!j.build(j.gen, *m_meg, j.ops, *this, false))
		j.gen.fn = nullptr;
	// プログラムか番地が変わったので、焼き込んだ版は作り直す
	j.spec.fn = nullptr;
	j.spec_tried = false;
	j.seen_const_gen = m_meg_const_gen;
	j.stable = 0;
}

// 訳した物を使えなくする（次に meg_jit_rebuild() を呼ぶまで解釈実行で回る）
void swp30_device::meg_jit_invalidate()
{
	if (m_jit) {
		m_jit->gen.fn = nullptr;
		m_jit->spec.fn = nullptr;
	}
}

bool swp30_device::meg_jit_run()
{
	meg_jit *j = m_jit.get();
	if (!j || !j->gen.fn)
		return false;

	static const bool bake_on = [] {
		const char *e = std::getenv("SMU2000_MEG_BAKE");
		return !(e && e[0] == '0');
	}();

	meg_jit::code *c = &j->gen;
	if (bake_on) {
		const u32 cg = m_meg_const_gen;
		if (cg != j->seen_const_gen) {
			j->seen_const_gen = cg;
			j->stable = 0;
			j->spec_tried = false;
		} else if (j->stable < meg_jit::STABLE)
			j->stable++;
		if (j->spec.fn && j->spec_const_gen == cg)
			c = &j->spec;
		else if (j->stable >= meg_jit::STABLE && !j->spec_tried) {
			j->spec_tried = true;
			if (j->build(j->spec, *m_meg, j->ops, *this, true)) {
				j->spec_const_gen = cg;
				c = &j->spec;
			} else
				j->spec.fn = nullptr;
		}
	}

	if (c->d3 != m_meg->m_delay_3 || c->d2 != m_meg->m_delay_2)
		return false;

	static const bool check = [] {
		const char *e = std::getenv("SMU2000_MEG_JIT_CHECK");
		return e && e[0] != '0';
	}();
	if (check) {
		// DEBUG: run the block through the JIT, then through the interpreter from
		// the same starting state, and report the first field that differs.
		static int shown = 0;
		meg_state before(*m_meg);
		std::vector<u16> ram0(m_reverb_ram);
		u32 seed0 = m_rand_seed;
		bool fn0 = m_meg_flag_n, fz0 = m_meg_flag_z;
		c->fn(m_meg, this, m_reverb_ram.data());
		const meg_state jit(*m_meg);
		const std::vector<u16> ramj(m_reverb_ram);
		const u32 seedj = m_rand_seed;
		const bool fnj = m_meg_flag_n, fzj = m_meg_flag_z;
		*m_meg = before;
		m_reverb_ram = ram0;
		m_rand_seed = seed0;
		m_meg_flag_n = fn0;
		m_meg_flag_z = fz0;
		if (const char *e = std::getenv("SMU2000_MEG_JIT_UPTO")) {
			const int upto = std::atoi(e);
			for (int i = 0; i != upto; i++)
				m_meg->step();
		} else
			m_meg->run_program(m_meg_ops.data());
		const u8 *jb = reinterpret_cast<const u8 *>(&jit);
		const u8 *ib = reinterpret_cast<const u8 *>(m_meg);
		// run_program() subtracts the 0x180 icount itself, so the icount (and the
		// retval next to it, which only the helper-call paths write) is expected
		// to differ here; stop short of it
		const size_t end = size_t(reinterpret_cast<const u8 *>(&m_meg->m_icount) - ib);
		size_t first = end;
		for (size_t i = 0; i < end; i++)
			if (jb[i] != ib[i]) { first = i; break; }
		size_t rambad = 0, ramfirst = 0;
		for (size_t i = 0; i < ramj.size(); i++)
			if (ramj[i] != m_reverb_ram[i]) { if (!rambad) ramfirst = i; rambad++; }
		if ((first != end || rambad || seedj != m_rand_seed || fnj != m_meg_flag_n || fzj != m_meg_flag_z) && shown < 40) {
			shown++;
			std::fprintf(stderr, "MEGCHECK sample %d state@%zu/%zu ram %zu (first %zu) seed %s flags %s%s\n",
				shown, first, end, rambad, ramfirst,
				seedj == m_rand_seed ? "ok" : "BAD", fnj == m_meg_flag_n ? "ok" : "BAD", fzj == m_meg_flag_z ? "ok" : "BAD");
			if (shown == 1) {
				for (int b = 0; b < 0x40; b += 8) {
					std::fprintf(stderr, "  m[%02x..] jit", b);
					for (int i = 0; i < 8; i++) std::fprintf(stderr, " %8d", jit.m_m[b + i]);
					std::fprintf(stderr, "\n         interp");
					for (int i = 0; i < 8; i++) std::fprintf(stderr, " %8d", m_meg->m_m[b + i]);
					std::fprintf(stderr, "\n");
				}
				std::fprintf(stderr, "  jit    m32 %d m33 %d m48 %d m49 %d mwv %d,%d,%d mwr %d,%d,%d\n",
					jit.m_m[32], jit.m_m[33], jit.m_m[48], jit.m_m[49],
					jit.m_mw_value[0], jit.m_mw_value[1], jit.m_mw_value[2], jit.m_mw_reg[0], jit.m_mw_reg[1], jit.m_mw_reg[2]);
				std::fprintf(stderr, "  interp m32 %d m33 %d m48 %d m49 %d mwv %d,%d,%d mwr %d,%d,%d\n",
					m_meg->m_m[32], m_meg->m_m[33], m_meg->m_m[48], m_meg->m_m[49],
					m_meg->m_mw_value[0], m_meg->m_mw_value[1], m_meg->m_mw_value[2], m_meg->m_mw_reg[0], m_meg->m_mw_reg[1], m_meg->m_mw_reg[2]);
				for (u32 k = 0; k != 0x180; k++) {
					const meg_state::op &o = m_meg_ops[k];
					if (o.dm == 32 || o.dm == 33 || o.dm == 48 || o.dm == 49)
						std::fprintf(stderr, "  ops[%u] dm=%d dm_src=%d mmode=%d asel=%d rop=%d shift=%d clamp=%d sm=%d sr=%d alu=%d\n",
							k, o.dm, o.dm_src, o.mmode, o.asel, o.rop, o.shift, o.clamp, o.sm, o.sr, o.alu);
				}
				std::fprintf(stderr, "  before m32 %d m33 %d m48 %d m49 %d p %lld mwv %d,%d,%d mwr %d,%d,%d d3 %d d2 %d\n",
					before.m_m[32], before.m_m[33], before.m_m[48], before.m_m[49], (long long)before.m_p,
					before.m_mw_value[0], before.m_mw_value[1], before.m_mw_value[2], before.m_mw_reg[0], before.m_mw_reg[1], before.m_mw_reg[2], before.m_delay_3, before.m_delay_2);
			}
			for (int i = 0; i < 0x40; i++)
				if (jit.m_m[i] != m_meg->m_m[i]) std::fprintf(stderr, "  m[%d] jit %d interp %d\n", i, jit.m_m[i], m_meg->m_m[i]);
			for (int i = 0; i < 0x80; i++)
				if (jit.m_r[i] != m_meg->m_r[i]) std::fprintf(stderr, "  r[%d] jit %d interp %d\n", i, jit.m_r[i], m_meg->m_r[i]);
			for (int i = 0; i < 8; i++)
				if (jit.m_t[i] != m_meg->m_t[i]) std::fprintf(stderr, "  t[%d] jit %d interp %d\n", i, jit.m_t[i], m_meg->m_t[i]);
			if (jit.m_p != m_meg->m_p) std::fprintf(stderr, "  p jit %lld interp %lld\n", (long long)jit.m_p, (long long)m_meg->m_p);
			if (jit.m_ram_index != m_meg->m_ram_index) std::fprintf(stderr, "  ix jit %d interp %d\n", jit.m_ram_index, m_meg->m_ram_index);
			if (jit.m_ram_read != m_meg->m_ram_read) std::fprintf(stderr, "  rr jit %d interp %d\n", jit.m_ram_read, m_meg->m_ram_read);
			if (jit.m_ram_write != m_meg->m_ram_write) std::fprintf(stderr, "  rw jit %d interp %d\n", jit.m_ram_write, m_meg->m_ram_write);
			if (jit.m_delay_3 != m_meg->m_delay_3 || jit.m_delay_2 != m_meg->m_delay_2) std::fprintf(stderr, "  d3/d2 jit %d,%d interp %d,%d\n", jit.m_delay_3, jit.m_delay_2, m_meg->m_delay_3, m_meg->m_delay_2);
			for (int i = 0; i < 3; i++) {
				if (jit.m_mw_value[i] != m_meg->m_mw_value[i] || jit.m_mw_reg[i] != m_meg->m_mw_reg[i]) std::fprintf(stderr, "  mw[%d] jit %d/%d interp %d/%d\n", i, jit.m_mw_value[i], jit.m_mw_reg[i], m_meg->m_mw_value[i], m_meg->m_mw_reg[i]);
				if (jit.m_rw_value[i] != m_meg->m_rw_value[i] || jit.m_rw_reg[i] != m_meg->m_rw_reg[i]) std::fprintf(stderr, "  rw[%d] jit %d/%d interp %d/%d\n", i, jit.m_rw_value[i], jit.m_rw_reg[i], m_meg->m_rw_value[i], m_meg->m_rw_reg[i]);
				if (jit.m_index_value[i] != m_meg->m_index_value[i] || jit.m_index_active[i] != m_meg->m_index_active[i]) std::fprintf(stderr, "  ixv[%d] jit %d/%d interp %d/%d\n", i, jit.m_index_value[i], jit.m_index_active[i], m_meg->m_index_value[i], m_meg->m_index_active[i]);
				if (jit.m_memw_value[i] != m_meg->m_memw_value[i] || jit.m_memw_active[i] != m_meg->m_memw_active[i]) std::fprintf(stderr, "  memw[%d] jit %d/%d interp %d/%d\n", i, jit.m_memw_value[i], jit.m_memw_active[i], m_meg->m_memw_value[i], m_meg->m_memw_active[i]);
				if (jit.m_memr_value[i] != m_meg->m_memr_value[i] || jit.m_memr_active[i] != m_meg->m_memr_active[i]) std::fprintf(stderr, "  memr[%d] jit %d/%d interp %d/%d\n", i, jit.m_memr_value[i], jit.m_memr_active[i], m_meg->m_memr_value[i], m_meg->m_memr_active[i]);
			}
			for (int i = 0; i < 2; i++)
				if (jit.m_t_value[i] != m_meg->m_t_value[i]) std::fprintf(stderr, "  tv[%d] jit %d interp %d\n", i, jit.m_t_value[i], m_meg->m_t_value[i]);
		}
	}
	c->fn(m_meg, this, m_reverb_ram.data());
	m_meg->m_pc = 0;
	m_meg->m_icount -= 0x180;
	return true;
}

// 機械語にしたリバーブ RAM の詰め方・戻し方を、meg_state の関数と全部の入力で突き合わせる。
// 食い違った入力の数を返す（JIT が無い環境では 0）。make test の verify から呼ぶ
u64 swp30_device::meg_jit_selftest()
{
#if SMU2000_MEG_JIT
	u64 bad = 0;
	for (int which = 0; which < 3; which++) {
		meg_asm a;
#ifdef __aarch64__
		a.mov_reg(HA, W0);                              // the value arrives in w0
		if (which == 0) emit_revram_encode(a); else if (which == 1) emit_revram_decode(a); else emit_m1_expand(a);
		a.mov_reg(W0, HA);                              // and the result goes back in w0
#else
		a.mov32(RAX, ARG0);
		if (which == 0) emit_revram_encode(a); else if (which == 1) emit_revram_decode(a); else emit_m1_expand(a);
#endif
		a.ret();
		// RW buffer, made executable after the copy (the code is bytes on x86
		// and 32-bit instructions on arm64, hence code[0] rather than a literal)
		const size_t bytes = a.code.size() * sizeof(a.code[0]);
		void *buf = exec_mem::alloc_rw(bytes);
		if (!buf)
			return ~u64(0);
		std::memcpy(buf, a.code.data(), bytes);
		if (!exec_mem::make_executable(buf, bytes)) {
			exec_mem::free_mem(buf, bytes);
			return ~u64(0);
		}
		const auto fn = reinterpret_cast<u32 (*)(meg_arg_t)>(buf);
		if (which == 0) {
			for (u32 v = 0; v < 0x8000000; v++)        // encode は下の 27bit しか見ない
				if ((fn(v) & 0xffff) != meg_state::revram_encode(v))
					bad++;
			for (u32 v : { 0xffffffffu, 0x80000000u, 0xf8000001u })
				if ((fn(v) & 0xffff) != meg_state::revram_encode(v))
					bad++;
		} else if (which == 1) {
			for (u32 v = 0; v < 0x10000; v++)
				if (fn(v) != meg_state::revram_decode(u16(v)))
					bad++;
		} else {
			// 呼ぶ側は loads16 で 64bit に符号拡張した値を渡す。出力は 64bit のまま使う
			const auto fn64 = reinterpret_cast<s64 (*)(s64)>(buf);
			for (s32 v = -0x8000; v < 0x8000; v++)
				if (fn64(v) != s64(meg_state::m1_expand(s16(v))))
					bad++;
		}
		exec_mem::free_mem(buf, bytes);
	}
	return bad;
#else
	return 0;
#endif
}

#if !SMU2000_MEG_JIT

bool swp30_device::meg_jit::build(code &, meg_state &, const meg_state::op *, swp30_device &, bool)
{
	return false;
}

#elif defined(__x86_64__)

bool swp30_device::meg_jit::build(code &cd, meg_state &ms, const meg_state::op *ops, swp30_device &swp, bool bake)
{
	fn_t &fn = cd.fn;
	void *&buf = cd.buf;
	size_t &buf_size = cd.buf_size;
	u32 &d3 = cd.d3, &d2 = cd.d2;
	fn = nullptr;
	// 分岐（前へ飛ばすだけ）のあるプログラムは、遅れの輪を解釈実行と同じく毎命令で読み書きする形で訳す。
	// 飛ばされた命令は、その命令が輪に入れるはずだった書き込みを消し、t の値だけを入れる（run_program と同じ）
	bool branchy = false;
	for (u32 pc = 0; pc != 0x180; pc++)
		if (ops[pc].jump)
			branchy = true;
	if (swp.m_reverb_ram.size() < 0x40000)
		return false;

	d3 = ms.m_delay_3;
	d2 = ms.m_delay_2;
	const auto slot3 = [&](u32 k) { return (d3 + k) % 3; };
	const auto slot2 = [&](u32 k) { return (d2 + k) % 2; };

	// 要素の位置（meg_state と swp30_device の中）
	const auto off = [](const void *base, const void *field) { return s32(intptr_t(field) - intptr_t(base)); };
	const s32 o_m        = off(&ms, ms.m_m.data());
	const s32 o_r        = off(&ms, ms.m_r.data());
	const s32 o_t        = off(&ms, ms.m_t.data());
	const s32 o_p        = off(&ms, &ms.m_p);
	const s32 o_const    = off(&ms, ms.m_const.data());
	const s32 o_offset   = off(&ms, ms.m_offset.data());
	const s32 o_mw_value = off(&ms, ms.m_mw_value.data());
	const s32 o_mw_reg   = off(&ms, ms.m_mw_reg.data());
	const s32 o_rw_value = off(&ms, ms.m_rw_value.data());
	const s32 o_rw_reg   = off(&ms, ms.m_rw_reg.data());
	const s32 o_ix_value = off(&ms, ms.m_index_value.data());
	const s32 o_ix_act   = off(&ms, ms.m_index_active.data());
	const s32 o_memw_val = off(&ms, ms.m_memw_value.data());
	const s32 o_memr_val = off(&ms, ms.m_memr_value.data());
	const s32 o_t_value  = off(&ms, ms.m_t_value.data());
	const s32 o_memw_act = off(&ms, ms.m_memw_active.data());
	const s32 o_memr_act = off(&ms, ms.m_memr_active.data());
	const s32 o_ram_read = off(&ms, &ms.m_ram_read);
	const s32 o_ram_write = off(&ms, &ms.m_ram_write);
	const s32 o_ram_index = off(&ms, &ms.m_ram_index);
	const s32 o_sample   = off(&ms, &ms.m_sample_counter);
	const s32 o_lfo      = off(&ms, ms.m_lfo.data());
	const s32 o_lfo_counter = off(&ms, ms.m_lfo_counter.data());
	// get_lfo を機械語にするのは、sin 表が 1/4 周期ぶん（0x8000 個）そろっているときだけ
	const u16 *sintab = swp.m_sintab.count() >= 0x8000 ? swp.m_sintab.target() : nullptr;
	const s32 o_seed     = off(&swp, &swp.m_rand_seed);
	const s32 o_flag_n   = off(&swp, &swp.m_meg_flag_n);
	const s32 o_flag_z   = off(&swp, &swp.m_meg_flag_z);
	const s32 o_ix2_value = off(&swp, swp.m_meg_ix2_value.data());   // 2 つ目の idx（doc/upstream.md の 32）
	const s32 o_ix2_act   = off(&swp, swp.m_meg_ix2_act.data());
	const s32 o_ram_index2 = off(&swp, &swp.m_meg_ram_index2);
	const s32 o_skip     = off(&swp, &swp.m_meg_jit_skip);

	if (sizeof(ms.m_mw_reg[0]) != 1 || sizeof(ms.m_index_active[0]) != 1 || sizeof(ms.m_memw_active[0]) != 1 ||
	    sizeof(swp.m_meg_flag_n) != 1 || sizeof(ms.m_t_value[0]) != 2 || sizeof(ms.m_const[0]) != 2 ||
	    sizeof(ms.m_offset[0]) != 2 || sizeof(ms.m_m[0]) != 4 || sizeof(ms.m_r[0]) != 4)
		return false;

	// t の値（2 命令遅れ）を書いておく必要がある命令: 2 つ後に t を p から書く命令があるか、終わりの 2 つ
	bool need_tval[0x180] = {};
	for (u32 k = 0; k != 0x180; k++) {
		if (k >= 0x17e)
			need_tval[k] = true;
		if (k + 2 < 0x180 && ops[k + 2].t_write && ops[k + 2].t_from_p)
			need_tval[k] = true;
	}

	// 3 命令遅れの書き込みを、書いた命令の場所で直に入れてよいレジスタを調べる。
	// 読む命令の 1 つ前か 2 つ前（サンプルを跨いでも）に同じレジスタへの書き込みが無ければ、
	// どの時点で読んでも見える値は変わらない。終わりの 3 命令（0x17d-0x17f）が書くレジスタは、
	// 次のサンプルの頭で輪から入るので対象にしない
	bool early_r[128] = {}, early_m[128] = {};
	{
		bool bad_r[128] = {}, bad_m[128] = {};
		const auto reads = [&](const meg_state::op &o, bool m, u32 x) {
			if (!x)
				return false;
			bool rd = false;
			if (o.alu && (o.mmode == 2 || o.mmode == 3) && (o.m2_from_m != 0) == m && (m ? o.sm : o.sr) == x)
				rd = true;
			if (o.alu && !m && o.asel == 1 && o.sr == x)
				rd = true;
			if (o.alu && m && o.asel == 2 && o.sm == x)
				rd = true;
			if (!m && o.dr && o.dr_from_r && o.sr == x)
				rd = true;
			if (m && o.dm && o.dm_src == 7 && o.sm == x)
				rd = true;
			return rd;
		};
		for (u32 j = 0; j != 0x180; j++)
			for (u32 back = 1; back <= 2; back++) {
				const meg_state::op &w = ops[(j + 0x180 - back) % 0x180];
				if (w.dr && reads(ops[j], false, w.dr))
					bad_r[w.dr] = true;
				if (w.dm && reads(ops[j], true, w.dm))
					bad_m[w.dm] = true;
			}
		for (u32 k = 0x17d; k != 0x180; k++) {
			if (ops[k].dr) bad_r[ops[k].dr] = true;
			if (ops[k].dm) bad_m[ops[k].dm] = true;
		}
		static const bool early_on = [] {
			const char *e = std::getenv("SMU2000_MEG_EARLY");
			return !(e && e[0] == '0');
		}();
		for (u32 x = 1; x != 128; x++) {
			early_r[x] = early_on && !branchy && !bad_r[x];
			early_m[x] = early_on && !branchy && !bad_m[x];
		}
	}

	// 前倒しで書いた書き込みでも、輪の枠の最後の値になるもの（同じ枠へ後で書く命令が無いもの）は枠にも書く。
	// 読まれはしないが、状態の保存の中身を解釈実行と同じにしておく
	bool last_slot_r[0x180] = {}, last_slot_m[0x180] = {};
	for (u32 c = 0; c != 3; c++) {
		for (int k = 0x17f; k >= 0; k--)
			if (u32(k) % 3 == c && ops[k].dr) { last_slot_r[k] = true; break; }
		for (int k = 0x17f; k >= 0; k--)
			if (u32(k) % 3 == c && ops[k].dm) { last_slot_m[k] = true; break; }
	}

	assembler a;
	const u8 MS = RBX, SWP = R12, P = R13, SC = R14, RAM = R15, SEED = RSI, K_MAX = RDI, K_MIN = RBP;
	const u8 P_MAX = R9, P_MIN = R10;                    // p の飽和の限界。r9 r10 は呼ぶ先で壊れるので、呼んだあと積み直す
	const auto load_p_limits = [&]() {
		a.imm64(P_MAX, 0x3fffffffff);
		a.imm64(P_MIN, u64(s64(-0x4000000000)));
	};   // SEED: 乱数の種を回しているあいだ持つ
	const auto M = [&](s32 disp) { return mem{MS, NOREG, 1, disp}; };

	// 入口（引数は ARG0 = ms、ARG1 = swp、ARG2 = リバーブ RAM。Windows x64 でも SysVでも）
	a.push(RBX); a.push(R12); a.push(R13); a.push(R14); a.push(R15); a.push(RSI); a.push(RDI); a.push(RBP);
	a.subrsp(56);                                    // 呼ぶ先の影 32 + 自分の置き場 16 + 揃え 8。rsp は 16 の倍数
	a.mov64(MS, ARG0);
	a.mov64(SWP, ARG1);
	a.mov64(RAM, ARG2);
	a.load64(P, M(o_p));
	a.load32(SC, M(o_sample));
	a.load32(SEED, mem{SWP, NOREG, 1, o_seed});
	a.imm64(K_MAX, 0x7fffff);                            // pack24 の限界（即値を毎回積まないため）
	a.imm64(K_MIN, u64(s64(-0x800000)));
	load_p_limits();
	if (branchy)
		a.store32i(mem{SWP, NOREG, 1, o_skip}, 0);

	// p を 24bit に詰める（meg_pack24）。入力 rax、出力 eax
	const auto pack24 = [&]() {
		// meg_pack24 と同じく 0 の側へ切り捨てる（負なら 0x7fff を足してから右へ）
		a.mov64(RCX, RAX);
		a.sar64(RCX, 63);
		a.and32i(RCX, 0x7fff);
		a.add64(RAX, RCX);
		a.sar64(RAX, 15);
		// 1 つだけはみ出したときは限界に止め、ほかは 24bit で折り返す（meg_pack24 と同じ）
		a.cmp64ri(RAX, 0x800000);
		a.cmove64(RAX, K_MAX);
		a.cmp64ri(RAX, u32(s32(-0x800001)));
		a.cmove64(RAX, K_MIN);
		a.shl32(RAX, 8);
		a.sar32(RAX, 8);
	};
	// 乱数を 1 つ引く（swp30_device::rand）。出力 eax
	const auto rnd = [&]() {
		a.imul32i(RAX, SEED, 1664525);
		a.add32i(RAX, 1013904223);
		a.mov32(SEED, RAX);
		a.rol32(RAX, 16);
	};
	// p に雑音を足して詰める（dm の 6 番、dr の p）。出力 eax
	const auto p_packed = [&](bool noise) {
		if (noise) {
			rnd();
			a.and32i(RAX, 0x07e0);
			a.mov64(RDX, RAX);
			a.mov64(RAX, P);
			a.add64(RAX, RDX);
		} else
			a.mov64(RAX, P);
		pack24();
	};

	for (u32 k = 0; k != 0x180; k++) {
		const meg_state::op &o = ops[k];

		// ---- 反映（遅れて入る書き込み）----
		if (k < 3 || branchy) {
			const u32 s = slot3(k);
			// m
			a.loadu8(RAX, M(o_mw_reg + s));
			a.test32(RAX, RAX);
			size_t j1 = a.jz_fwd();
			a.load32(RCX, M(o_mw_value + 4 * s));
			a.store32(mem{MS, RAX, 4, o_m}, RCX);
			a.patch(j1);
			// r
			a.loadu8(RAX, M(o_rw_reg + s));
			a.test32(RAX, RAX);
			size_t j2 = a.jz_fwd();
			a.load32(RCX, M(o_rw_value + 4 * s));
			a.store32(mem{MS, RAX, 4, o_r}, RCX);
			a.patch(j2);
			// index
			a.loadu8(RAX, M(o_ix_act + s));
			a.test32(RAX, RAX);
			size_t j3 = a.jz_fwd();
			a.load32(RCX, M(o_ix_value + 4 * s));
			a.store32(M(o_ram_index), RCX);
			a.patch(j3);
			// 2 つ目の index
			a.loadu8(RAX, mem{SWP, NOREG, 1, o_ix2_act + s32(s)});
			a.test32(RAX, RAX);
			size_t j4 = a.jz_fwd();
			a.load32(RCX, mem{SWP, NOREG, 1, o_ix2_value + 4 * s32(s)});
			a.store32(mem{SWP, NOREG, 1, o_ram_index2}, RCX);
			a.patch(j4);
		} else {
			const meg_state::op &w = ops[k - 3];
			const u32 s = slot3(k);
			if (w.dm && !early_m[w.dm]) {
				a.load32(RCX, M(o_mw_value + 4 * s));
				a.store32(M(o_m + 4 * w.dm), RCX);
			}
			if (w.dr && !early_r[w.dr]) {
				a.load32(RCX, M(o_rw_value + 4 * s));
				a.store32(M(o_r + 4 * w.dr), RCX);
			}
			if (w.index) {
				a.load32(RCX, M(o_ix_value + 4 * s));
				a.store32(M(o_ram_index), RCX);
			}
			if (w.index2) {
				a.load32(RCX, mem{SWP, NOREG, 1, o_ix2_value + 4 * s32(s)});
				a.store32(mem{SWP, NOREG, 1, o_ram_index2}, RCX);
			}
		}
		if (k < 2 || branchy) {
			const u32 s = slot2(k);
			a.loadu8(RAX, M(o_memw_act + s));
			a.test32(RAX, RAX);
			size_t j1 = a.jz_fwd();
			a.load32(RCX, M(o_memw_val + 4 * s));
			a.store32(M(o_ram_write), RCX);
			a.store8i(M(o_memw_act + s), 0);
			a.patch(j1);
			a.loadu8(RAX, M(o_memr_act + s));
			a.test32(RAX, RAX);
			size_t j2 = a.jz_fwd();
			a.load32(RCX, M(o_memr_val + 4 * s));
			a.store32(M(o_ram_read), RCX);
			a.store8i(M(o_memr_act + s), 0);
			a.patch(j2);
		} else {
			const meg_state::op &w = ops[k - 2];
			const u32 s = slot2(k);
			if (w.memw) {
				a.load32(RCX, M(o_memw_val + 4 * s));
				a.store32(M(o_ram_write), RCX);
			}
			if (w.memop == 2 || w.memop == 3) {
				a.load32(RCX, M(o_memr_val + 4 * s));
				a.store32(M(o_ram_read), RCX);
			}
		}

		// ---- 分岐と、飛ばされた命令 ----
		size_t skip_jump = 0, jump_done = 0;
		if (branchy) {
			a.cmp32i_mem(mem{SWP, NOREG, 1, o_skip}, k);
			skip_jump = a.jcc_fwd(0x87);                             // ja: 飛ばす位置がこの命令より後
			if (o.jump) {
				// meg_cond を機械語で
				size_t no_jump = 0;
				if (o.cond & 8) {
					a.loadu8(RAX, mem{SWP, NOREG, 1, o_flag_n});
					if (!(o.cond & 4))
						a.xor32ri(RAX, 1);
					if (o.cond & 2) {
						a.loadu8(RCX, mem{SWP, NOREG, 1, o_flag_z});
						a.or32(RAX, RCX);
					}
					a.test32(RAX, RAX);
					no_jump = a.jz_fwd();
				}
				if (o.target > k)
					a.store32i(mem{SWP, NOREG, 1, o_skip}, o.target);
				if (no_jump)
					a.patch(no_jump);
				jump_done = a.jmp_fwd();                             // 分岐の命令そのものも、飛ばされた命令と同じ後始末をする
			}
		}
		if (!(branchy && o.jump)) {

		// ---- ALU ----
		// bake のときは定数（と、そこから決まる m1）を焼き込む。係数 0 の掛け算は省き、
		// 「p = 0 * x + p」のように p が変わらない命令は丸ごと省く（結果はビット単位で同じ）
		bool alu_skip = false;
		if (o.alu && bake && !o.m1_from_t && o.mmode != 3) {
			s64 c = ms.m_const[k];
			if (o.m1_expand)
				c = meg_state::m1_expand(s16(c));
			const bool m_zero = o.mmode == 0 || c == 0;
			if (m_zero && o.asel == 0 && o.rop == 0 && o.shift == 0 && o.clamp == 0 && !o.latch)
				alu_skip = true;                                 // p はもう 42bit に収まっている
			else if (m_zero)
				a.xor32(RAX, RAX);
			else if (o.mmode == 1)
				a.imm64(RAX, u64(c << (8 + 15)));
			else {
				a.loads32(RAX, o.m2_from_m ? M(o_m + 4 * o.sm) : M(o_r + 4 * o.sr));
				a.imul64i(RAX, RAX, u32(s32(c)));
			}
		}
		if (o.alu && !alu_skip) {
			if (!(bake && !o.m1_from_t && o.mmode != 3)) {
			if (o.m1_from_t == 2) {
				// 印（負）が立っていれば t、そうでなければ定数（meg_state::step と同じ、doc/upstream.md の 29）
				a.loads16(RAX, M(o_t + 2 * o.t));
				a.loads16(RCX, M(o_const + 2 * s32(k)));
				a.loadu8(RDX, mem{SWP, NOREG, 1, o_flag_n});
				a.test32(RDX, RDX);
				a.cmove64(RAX, RCX);
			} else if (o.m1_from_t)
				a.loads16(RAX, M(o_t + 2 * o.t));
			else
				a.loads16(RAX, M(o_const + 2 * s32(k)));
			if (o.m1_expand)
				emit_m1_expand(a);                           // meg_state::m1_expand を機械語で
			switch (o.mmode) {
			case 0:
				a.xor32(RAX, RAX);
				break;
			case 1:
				a.shl64(RAX, 8 + 15);
				break;
			case 2:
				a.loads32(RCX, o.m2_from_m ? M(o_m + 4 * o.sm) : M(o_r + 4 * o.sr));
				a.imul64(RAX, RCX);
				break;
			default:
				a.loads32(RAX, o.m2_from_m ? M(o_m + 4 * o.sm) : M(o_r + 4 * o.sr));
				a.shl64(RAX, 15);
				break;
			}
			}
			switch (o.asel) {
			case 0: a.mov64(RCX, P); break;
			case 1: a.loads32(RCX, M(o_r + 4 * o.sr)); a.shl64(RCX, 15); break;
			case 2: a.loads32(RCX, M(o_m + 4 * o.sm)); a.shl64(RCX, 15); break;
			case 3: a.mov64(RCX, P); a.sar64(RCX, 15); break;
			default: a.xor32(RCX, RCX); break;
			}
			switch (o.rop) {
			case 0: a.add64(RAX, RCX); break;
			case 1: a.sub64(RAX, RCX); break;
			case 2:
				a.mov64(RDX, RCX);
				a.neg64(RDX);
				a.cmovs64(RDX, RCX);
				a.add64(RAX, RDX);
				break;
			default: a.and64(RAX, RCX); break;
			}
			if (o.shift)
				a.shl64(RAX, o.shift);
			if (o.clamp == 0) {
				a.shl64(RAX, 22);
				a.sar64(RAX, 22);
			}
			switch (o.clamp) {
			case 0: break;
			case 1:
				a.cmp64(RAX, P_MIN);
				a.cmovl64(RAX, P_MIN);
				a.cmp64(RAX, P_MAX);
				a.cmovg64(RAX, P_MAX);
				break;
			case 2:
				a.xor32(RCX, RCX);
				a.cmp64(RAX, RCX);
				a.cmovl64(RAX, RCX);
				a.cmp64(RAX, P_MAX);
				a.cmovg64(RAX, P_MAX);
				break;
			default:
				a.mov64(RDX, RAX);
				a.neg64(RDX);
				a.cmovs64(RDX, RAX);
				a.mov64(RAX, RDX);
				a.cmp64(RAX, P_MAX);
				a.cmovg64(RAX, P_MAX);
				break;
			}
			a.mov64(P, RAX);
			if (o.latch) {
				a.test64(P, P);
				a.setl_mem(mem{SWP, NOREG, 1, o_flag_n});
				a.test64(P, P);
				a.sete_mem(mem{SWP, NOREG, 1, o_flag_z});
			}
		}

		// ---- dm ----
		if (o.dm) {
			switch (o.dm_src) {
			case 0: case 1: case 2: case 3:
				if (sintab && o.lfo < 0x18) {
					// meg_state::get_lfo を機械語で（関数は呼ばない）。出力 eax。rcx rdx r8 を壊す
					static constexpr u32 lfo_offsets[16] = {
						0x00000, 0x02aaa, 0x04000, 0x05555, 0x08000, 0x0aaaa, 0x0c000, 0x0d555,
						0x10000, 0x12aaa, 0x14000, 0x15555, 0x18000, 0x1aaaa, 0x1c000, 0x1d555,
					};
					a.load32(RAX, M(o_lfo_counter + 4 * s32(o.lfo)));
					a.shr32(RAX, 5);
					a.loadu16(RDX, M(o_lfo + 2 * s32(o.lfo)));
					a.mov32(RCX, RDX);
					a.shr32(RCX, 8);
					a.and32i(RCX, 3);
					a.shl32cl(RAX);
					a.mov32(RCX, RDX);
					a.shr32(RCX, 12);
					a.imm64(R8, u64(uintptr_t(lfo_offsets)));
					a.load32(RCX, mem{R8, RCX, 4, 0});
					a.add32(RAX, RCX);
					a.and32i(RAX, 0x1ffff);
					a.shr32(RDX, 10);
					a.and32i(RDX, 3);
					std::vector<size_t> done;
					a.test32(RDX, RDX);
					const size_t not_sine = a.jcc_fwd(0x85);
					{   // sine
						a.mov32(RCX, RAX);
						a.and32i(RCX, 0x7fff);
						a.test32ri(RAX, 0x8000);
						const size_t no_rev = a.jcc_fwd(0x84);
						a.xor32ri(RCX, 0x7fff);
						a.patch(no_rev);
						a.imm64(R8, u64(uintptr_t(sintab)));
						a.mov32(RDX, RAX);
						a.loadu16(RAX, mem{R8, RCX, 2, 0});
						a.test32ri(RDX, 0x10000);
						const size_t no_neg = a.jcc_fwd(0x84);
						a.xor32ri(RAX, 0xffff);
						a.patch(no_neg);
						done.push_back(a.jmp_fwd());
					}
					a.patch(not_sine);
					a.cmp32ri(RDX, 1);
					const size_t not_tri = a.jcc_fwd(0x85);
					{   // tri
						a.add32ri(RAX, 0x8000);
						a.and32i(RAX, 0x1ffff);
						a.test32ri(RAX, 0x10000);
						const size_t no_fold = a.jcc_fwd(0x84);
						a.xor32ri(RAX, 0x1ffff);
						a.patch(no_fold);
						done.push_back(a.jmp_fwd());
					}
					a.patch(not_tri);
					a.cmp32ri(RDX, 2);
					const size_t not_up = a.jcc_fwd(0x85);
					a.shr32(RAX, 1);                                     // saw up
					done.push_back(a.jmp_fwd());
					a.patch(not_up);
					a.xor32ri(RAX, 0x1ffff);                             // saw down
					a.shr32(RAX, 1);
					for (size_t d : done) a.patch(d);
					a.shl32(RAX, 7);
				} else {
					a.mov64(RCX, MS);
					a.imm32(RDX, o.lfo);
					a.call_abs(reinterpret_cast<void *>(&meg_jit::call_lfo));
					load_p_limits();
				}
				break;
			case 4:
				a.load32(RAX, M(o_ram_read));
				break;
			case 5:
				rnd();
				a.shl32(RAX, 8);
				a.sar32(RAX, 8);
				break;
			case 6:
				p_packed(!o.no_noise);
				break;
			default:
				a.load32(RAX, M(o_m + 4 * o.sm));
				break;
			}
			if (k < 0x17d && early_m[o.dm]) {
				a.store32(M(o_m + 4 * o.dm), RAX);
				if (last_slot_m[k])
					a.store32(M(o_mw_value + 4 * slot3(k)), RAX);
			} else
				a.store32(M(o_mw_value + 4 * slot3(k)), RAX);
		}
		if (k >= 0x17d || branchy)
			a.store8i(M(o_mw_reg + slot3(k)), o.dm);

		// ---- dr ----
		if (o.dr) {
			if (o.dr_from_r)
				a.load32(RAX, M(o_r + 4 * o.sr));
			else
				p_packed(!o.no_noise);
			if (k < 0x17d && early_r[o.dr]) {
				a.store32(M(o_r + 4 * o.dr), RAX);
				if (last_slot_r[k])
					a.store32(M(o_rw_value + 4 * slot3(k)), RAX);
			} else
				a.store32(M(o_rw_value + 4 * slot3(k)), RAX);
		}
		if (k >= 0x17d || branchy)
			a.store8i(M(o_rw_reg + slot3(k)), o.dr);

		// ---- メモリへの書き値 ----
		if (o.memw) {
			a.mov64(RAX, P);
			a.sar64(RAX, 15);
			a.store32(M(o_memw_val + 4 * slot2(k)), RAX);
		}
		if (k >= 0x17e || branchy)
			a.store8i(M(o_memw_act + slot2(k)), o.memw ? 1 : 0);

		// ---- index ----
		if (o.index) {
			a.mov64(RAX, P);
			a.sar64(RAX, 15 + 8);
			a.store32(M(o_ix_value + 4 * slot3(k)), RAX);
		}
		if (k >= 0x17d || branchy)
			a.store8i(M(o_ix_act + slot3(k)), o.index ? 1 : 0);
		if (o.index2) {
			a.mov64(RAX, P);
			a.sar64(RAX, 15 + 8);
			a.store32(mem{SWP, NOREG, 1, o_ix2_value + 4 * s32(slot3(k))}, RAX);
		}
		if (k >= 0x17d || branchy)
			a.store8i(mem{SWP, NOREG, 1, o_ix2_act + s32(slot3(k))}, o.index2 ? 1 : 0);

		// ---- t ----
		if (o.t_write) {
			if (o.t_from_p)
				a.loadu16(RAX, M(o_t_value + 2 * slot2(k)));
			else if (bake)
				a.imm32(RAX, u16(ms.m_const[k]));
			else
				a.loadu16(RAX, M(o_const + 2 * s32(k)));
			a.store16(M(o_t + 2 * o.t), RAX);
		}
		if (need_tval[k]) {
			a.mov64(RAX, P);
			if (o.index || o.index2) {
				a.sar64(RAX, 8);
				a.and32i(RAX, 0x7fff);
			} else {
				a.sar64(RAX, 15 + 8);
				a.imm64(RCX, u64(s64(-0x8000)));
				a.cmp64(RAX, RCX);
				a.cmovl64(RAX, RCX);
				a.imm64(RCX, 0x7fff);
				a.cmp64(RAX, RCX);
				a.cmovg64(RAX, RCX);
			}
			a.store16(M(o_t_value + 2 * slot2(k)), RAX);
		}

		// ---- メモリ操作 ----
		size_t table_done = 0;
		if (o.memop >= 2 && o.mem_table) {
			// bit 0x23 の読み出しはリバーブ RAM の絶対番地（meg_state::step、doc/upstream.md の 24）
			a.loadu16(RAX, M(o_offset + 2 * s32(o.offset_index)));
			if (o.mem_use_index) {
				a.load32(RCX, M(o_ram_index));
				a.add32(RAX, RCX);
			}
			if (o.mem_use_index2) {
				a.load32(RCX, mem{SWP, NOREG, 1, o_ram_index2});
				a.add32(RAX, RCX);
			}
			if (o.memop == 3)
				a.add32i(RAX, 1);
			a.and32i(RAX, 0x3ffff);
			a.loadu16(RAX, mem{RAM, RAX, 2, 0});
			emit_revram_decode(a);
			a.store32(M(o_memr_val + 4 * slot2(k)), RAX);
			table_done = a.jmp_fwd();
		}
		if (o.memop) {
			a.loadu16(RAX, M(o_offset + 2 * s32(o.offset_index)));
			if (o.mem_use_index) {
				a.load32(RCX, M(o_ram_index));
				a.add32(RAX, RCX);
			}
			if (o.mem_use_index2) {
				a.load32(RCX, mem{SWP, NOREG, 1, o_ram_index2});
				a.add32(RAX, RCX);
			}
			a.sub32(RAX, SC);
			if (o.memop == 3)
				a.add32i(RAX, 1);
			a.and32i(RAX, o.addr_mask);
			a.add32i(RAX, o.addr_base);
			a.and32i(RAX, 0x3ffff);
			if (o.memop == 1) {
				// meg_state::revram_encode を機械語で（関数は呼ばない）。番地は r8 に取っておく
				a.mov64(R8, RAX);
				a.load32(RAX, M(o_ram_write));
				emit_revram_encode(a);
				a.store16(mem{RAM, R8, 2, 0}, RAX);
			} else {
				// meg_state::revram_decode を機械語で（関数は呼ばない）
				a.loadu16(RAX, mem{RAM, RAX, 2, 0});
				emit_revram_decode(a);
				a.store32(M(o_memr_val + 4 * slot2(k)), RAX);
			}
		}
		if (table_done)
			a.patch(table_done);
		if (k >= 0x17e || branchy)
			a.store8i(M(o_memr_act + slot2(k)), (o.memop == 2 || o.memop == 3) ? 1 : 0);
		}   // !(branchy && o.jump)

		if (branchy) {
			const size_t normal_done = o.jump ? 0 : a.jmp_fwd();
			a.patch(skip_jump);
			if (jump_done)
				a.patch(jump_done);
			// 飛ばされた命令（と分岐の命令）: 輪に入れる書き込みを消し、t の値を入れる
			if (o.jump && o.t_write) {
				// 分岐の命令も t を書く（meg_state::step と同じ、doc/upstream.md の 31）
				if (o.t_from_p)
					a.loadu16(RAX, M(o_t_value + 2 * slot2(k)));
				else
					a.loadu16(RAX, M(o_const + 2 * s32(k)));
				a.store16(M(o_t + 2 * o.t), RAX);
			}
			a.store8i(M(o_mw_reg + slot3(k)), 0);
			a.store8i(M(o_rw_reg + slot3(k)), 0);
			a.store8i(M(o_memw_act + slot2(k)), 0);
			a.store8i(M(o_ix_act + slot3(k)), 0);
			a.store8i(mem{SWP, NOREG, 1, o_ix2_act + s32(slot3(k))}, 0);
			if (need_tval[k]) {
				a.mov64(RAX, P);
				a.sar64(RAX, 15 + 8);
				a.imm64(RCX, u64(s64(-0x8000)));
				a.cmp64(RAX, RCX);
				a.cmovl64(RAX, RCX);
				a.imm64(RCX, 0x7fff);
				a.cmp64(RAX, RCX);
				a.cmovg64(RAX, RCX);
				a.store16(M(o_t_value + 2 * slot2(k)), RAX);
			}
			if (normal_done)
				a.patch(normal_done);
		}
	}

	// 出口
	a.store64(M(o_p), P);
	a.store32(mem{SWP, NOREG, 1, o_seed}, SEED);
	a.addrsp(56);
	a.pop(RBP); a.pop(RDI); a.pop(RSI); a.pop(R15); a.pop(R14); a.pop(R13); a.pop(R12); a.pop(RBX);
	a.ret();

	if (a.code.size() > buf_size) {
		if (buf)
			exec_mem::free_mem(buf, buf_size);
		buf_size = (a.code.size() + 0xffff) & ~size_t(0xffff);
		buf = exec_mem::alloc_rw(buf_size);
		if (!buf) {
			buf_size = 0;
			return false;
		}
	}
	// make writable, copy, make executable again. Needed on rebuilds, which arrive still executable
	if (!exec_mem::make_writable(buf, buf_size))
		return false;
	std::memcpy(buf, a.code.data(), a.code.size());
	if (!exec_mem::make_executable(buf, buf_size))
		return false;
	fn = reinterpret_cast<fn_t>(buf);
	return true;
}

#else // __aarch64__

// The arm64 MEG JIT. Same shape as the x86-64 version above (which carries the
// comments explaining what each section computes); only the notes where the
// two differ are repeated here.
bool swp30_device::meg_jit::build(code &cd, meg_state &ms, const meg_state::op *ops, swp30_device &swp, bool bake)
{
	using namespace a64;

	// The baked-constant variant of the x86-64 backend has no arm64 equivalent
	// yet, so asking for it here just fails and the caller keeps the general
	// version (see meg_jit_run)
	if (bake)
		return false;

	cd.fn = nullptr;
	// Programs with jumps (LO-FI and DYNA types) fall back to the interpreter:
	// the emitted block would have to read and write the delay ring on every op.
	// The output is bit-identical either way
	for (u32 pc = 0; pc != 0x180; pc++)
		if (ops[pc].jump)
			return false;
	if (swp.m_reverb_ram.size() < 0x40000)
		return false;

	cd.d3 = ms.m_delay_3;
	cd.d2 = ms.m_delay_2;
	const auto slot3 = [&](u32 k) { return (cd.d3 + k) % 3; };
	const auto slot2 = [&](u32 k) { return (cd.d2 + k) % 2; };

	// element positions (inside meg_state and swp30_device)
	const auto off = [](const void *base, const void *field) { return s32(intptr_t(field) - intptr_t(base)); };
	const s32 o_m        = off(&ms, ms.m_m.data());
	const s32 o_r        = off(&ms, ms.m_r.data());
	const s32 o_t        = off(&ms, ms.m_t.data());
	const s32 o_p        = off(&ms, &ms.m_p);
	const s32 o_const    = off(&ms, ms.m_const.data());
	const s32 o_offset   = off(&ms, ms.m_offset.data());
	const s32 o_mw_value = off(&ms, ms.m_mw_value.data());
	const s32 o_mw_reg   = off(&ms, ms.m_mw_reg.data());
	const s32 o_rw_value = off(&ms, ms.m_rw_value.data());
	const s32 o_rw_reg   = off(&ms, ms.m_rw_reg.data());
	const s32 o_ix_value = off(&ms, ms.m_index_value.data());
	const s32 o_ix_act   = off(&ms, ms.m_index_active.data());
	const s32 o_memw_val = off(&ms, ms.m_memw_value.data());
	const s32 o_memr_val = off(&ms, ms.m_memr_value.data());
	const s32 o_t_value  = off(&ms, ms.m_t_value.data());
	const s32 o_memw_act = off(&ms, ms.m_memw_active.data());
	const s32 o_memr_act = off(&ms, ms.m_memr_active.data());
	const s32 o_ram_read = off(&ms, &ms.m_ram_read);
	const s32 o_ram_write = off(&ms, &ms.m_ram_write);
	const s32 o_ram_index = off(&ms, &ms.m_ram_index);
	const s32 o_sample   = off(&ms, &ms.m_sample_counter);
	const s32 o_seed     = off(&swp, &swp.m_rand_seed);
	const s32 o_flag_n   = off(&swp, &swp.m_meg_flag_n);
	const s32 o_flag_z   = off(&swp, &swp.m_meg_flag_z);
	// The second idx (doc/upstream.md #32) lives on swp30_device rather than in
	// meg_state, so that the layout the saved state is packed from does not move
	const s32 o_ix2_value  = off(&swp, swp.m_meg_ix2_value.data());
	const s32 o_ix2_act    = off(&swp, swp.m_meg_ix2_act.data());
	const s32 o_ram_index2 = off(&swp, &swp.m_meg_ram_index2);

	if (sizeof(ms.m_mw_reg[0]) != 1 || sizeof(ms.m_index_active[0]) != 1 || sizeof(ms.m_memw_active[0]) != 1 ||
	    sizeof(swp.m_meg_flag_n) != 1 || sizeof(ms.m_t_value[0]) != 2 || sizeof(ms.m_const[0]) != 2 ||
	    sizeof(ms.m_offset[0]) != 2 || sizeof(ms.m_m[0]) != 4 || sizeof(ms.m_r[0]) != 4)
		return false;

	// instructions that must leave a t value in the ring (2 later one is read), plus the tail
	bool need_tval[0x180] = {};
	for (u32 k = 0; k != 0x180; k++) {
		if (k >= 0x17e)
			need_tval[k] = true;
		if (k + 2 < 0x180 && ops[k + 2].t_write && ops[k + 2].t_from_p)
			need_tval[k] = true;
	}

	emitter a;
	// Block state in callee-saved registers (rbx/r12-r15 in the x86 version).
	// A carries the value a program instruction computes, E holds an address
	// across the revram helpers, T takes a materialized struct address; A, C, D,
	// E and T are all caller-saved and hold nothing across a helper call.
	const u8 MS = X19, SWP = X20, P = X21, SC = X22, RAM = X23;
	const u8 A = HA, C = HC, D = HD, E = HE, T = X6;

	// [base + disp] with disp known at build time. Each size reaches further
	// than the plain imm12 (the field is scaled by the access size), so a disp
	// past that has its address materialized in T first.
	const auto ldb   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 4095)  a.ldrb(rt, base, d);  else { a.add_off(T, base, d); a.ldrb(rt, T, 0); } };
	const auto stb   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 4095)  a.strb(rt, base, d);  else { a.add_off(T, base, d); a.strb(rt, T, 0); } };
	const auto ldh   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 8190)  a.ldrh(rt, base, d);  else { a.add_off(T, base, d); a.ldrh(rt, T, 0); } };
	const auto ldh_s = [&](u32 rt, u32 base, s32 d) {
		if (d >= 0 && d <= 8190)
			a.ldrsh(rt, base, d);
		else {
			a.add_off(T, base, d);
			a.ldrsh(rt, T, 0);
		}
		// LDRSH Wt sign-extends to 32 bits; the MEG ALU consumes a signed
		// 64-bit m1 value, so extend the result through the top half of Xt.
		a.sxtw64(rt, rt);
	};
	const auto sth   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 8190)  a.strh(rt, base, d);  else { a.add_off(T, base, d); a.strh(rt, T, 0); } };
	const auto ldw   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 16380) a.ldr_w(rt, base, d); else { a.add_off(T, base, d); a.ldr_w(rt, T, 0); } };
	const auto ldw_s = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 16380) a.ldrsw(rt, base, d); else { a.add_off(T, base, d); a.ldrsw(rt, T, 0); } };
	const auto stw   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 16380) a.str_w(rt, base, d); else { a.add_off(T, base, d); a.str_w(rt, T, 0); } };
	const auto ldx   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 32760) a.ldr_x(rt, base, d); else { a.add_off(T, base, d); a.ldr_x(rt, T, 0); } };
	const auto stx   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 32760) a.str_x(rt, base, d); else { a.add_off(T, base, d); a.str_x(rt, T, 0); } };

	// entry: x0 = ms, x1 = swp, x2 = the reverb RAM. x30 is saved because the
	// LFO is fetched with BLR, and x19-x23 because they carry the block state.
	a.stp_x(X19, X20, X31, -16, true);
	a.stp_x(X21, X22, X31, -16, true);
	a.stp_x(X23, X30, X31, -16, true);
	a.mov_x(MS, X0);
	a.mov_x(SWP, X1);
	a.mov_x(RAM, X2);
	ldx(P, MS, o_p);
	ldw(SC, MS, o_sample);

	// pack p into 24 bits (meg_state::meg_pack24). Input A, output A
	const auto pack24 = [&]() {
		// Same rounding as meg_pack24: toward zero, so a negative value has
		// 0x7fff added before the shift
		a.asr_imm_x(C, A, 63);
		a.and_imm(C, C, 0x7fff);
		a.add_x(A, A, C);
		a.asr_imm_x(A, A, 15);
		// One past either limit stops **at** the limit; everything else wraps
		// into 24 bits (the interpreter does the same, doc/upstream.md #23)
		a.mov_imm64(C, 0x800000);
		a.cmp_x(A, C);
		a.mov_imm64(D, 0x7fffff);
		a.csel_x(A, D, A, EQ);
		a.mov_imm64(C, u64(s64(-0x800001)));
		a.cmp_x(A, C);
		a.mov_imm64(D, u64(s64(-0x800000)));
		a.csel_x(A, D, A, EQ);
		a.lsl_imm(A, A, 8);
		a.sar_imm(A, A, 8);
	};
	// one step of swp30_device::rand. Output A
	const auto rnd = [&]() {
		ldw(A, SWP, o_seed);
		a.mov_imm32(T, 1664525);
		a.mul(A, A, T);
		a.mov_imm32(D, 1013904223);
		a.add_reg(A, A, D);
		stw(A, SWP, o_seed);
		a.ror_imm(A, A, 16);
	};
	// p plus noise, packed (dm 6, and dr's p). Output A
	const auto p_packed = [&](bool noise) {
		if (noise) {
			rnd();
			a.and_imm(A, A, 0x07e0);
			a.mov_x(D, A);
			a.mov_x(A, P);
			a.add_x(A, A, D);
		} else
			a.mov_x(A, P);
		pack24();
	};

	// DEBUG: stop the block early, to bisect against the interpreter
	u32 upto = 0x180;
	if (const char *e = std::getenv("SMU2000_MEG_JIT_UPTO"))
		upto = u32(std::atoi(e));
	for (u32 k = 0; k != upto; k++) {
		const meg_state::op &o = ops[k];

		// ---- delayed writes, applied ----
		if (k < 3) {
			const u32 s = slot3(k);
			// m: the slot's register number is the index, and it may be 0 (= none)
			ldb(A, MS, o_mw_reg + s);
			a.cmp_reg(A, WZR);
			const size_t j1 = a.b_cond(EQ);
			ldw(C, MS, o_mw_value + 4 * s);
			a.add_off(T, MS, o_m);
			a.str_w_sr(C, T, A);
			a.patch(j1);
			// r
			ldb(A, MS, o_rw_reg + s);
			a.cmp_reg(A, WZR);
			const size_t j2 = a.b_cond(EQ);
			ldw(C, MS, o_rw_value + 4 * s);
			a.add_off(T, MS, o_r);
			a.str_w_sr(C, T, A);
			a.patch(j2);
			// index
			ldb(A, MS, o_ix_act + s);
			a.cmp_reg(A, WZR);
			const size_t j3 = a.b_cond(EQ);
			ldw(C, MS, o_ix_value + 4 * s);
			stw(C, MS, o_ram_index);
			a.patch(j3);
			// the second index, which gets its own carry (doc/upstream.md #32)
			ldb(A, SWP, o_ix2_act + s);
			a.cmp_reg(A, WZR);
			const size_t j4 = a.b_cond(EQ);
			ldw(C, SWP, o_ix2_value + 4 * s);
			stw(C, SWP, o_ram_index2);
			a.patch(j4);
		} else {
			const meg_state::op &w = ops[k - 3];
			const u32 s = slot3(k);
			if (w.dm) {
				ldw(C, MS, o_mw_value + 4 * s);
				stw(C, MS, o_m + 4 * w.dm);
			}
			if (w.dr) {
				ldw(C, MS, o_rw_value + 4 * s);
				stw(C, MS, o_r + 4 * w.dr);
			}
			if (w.index) {
				ldw(C, MS, o_ix_value + 4 * s);
				stw(C, MS, o_ram_index);
			}
			if (w.index2) {
				ldw(C, SWP, o_ix2_value + 4 * s);
				stw(C, SWP, o_ram_index2);
			}
		}
		if (k < 2) {
			const u32 s = slot2(k);
			ldb(A, MS, o_memw_act + s);
			a.cmp_reg(A, WZR);
			const size_t j1 = a.b_cond(EQ);
			ldw(C, MS, o_memw_val + 4 * s);
			stw(C, MS, o_ram_write);
			stb(WZR, MS, o_memw_act + s);
			a.patch(j1);
			ldb(A, MS, o_memr_act + s);
			a.cmp_reg(A, WZR);
			const size_t j2 = a.b_cond(EQ);
			ldw(C, MS, o_memr_val + 4 * s);
			stw(C, MS, o_ram_read);
			stb(WZR, MS, o_memr_act + s);
			a.patch(j2);
		} else {
			const meg_state::op &w = ops[k - 2];
			const u32 s = slot2(k);
			if (w.memw) {
				ldw(C, MS, o_memw_val + 4 * s);
				stw(C, MS, o_ram_write);
			}
			if (w.memop == 2 || w.memop == 3) {
				ldw(C, MS, o_memr_val + 4 * s);
				stw(C, MS, o_ram_read);
			}
		}

		// ---- ALU ----
		if (o.alu) {
			// the interpreter widens the s16 constant (or t) to s64, so the load
			// has to sign-extend all the way
			if (o.m1_from_t == 2) {
				// MULTI COMP follows its envelope: t when the last latch was
				// negative, the constant otherwise (meg_state::step, doc/upstream.md #29)
				ldh_s(A, MS, o_t + 2 * o.t);
				ldh_s(C, MS, o_const + 2 * s32(k));
				ldb(D, SWP, o_flag_n);
				a.cmp_reg(D, WZR);
				a.csel_x(A, A, C, NE);
			} else if (o.m1_from_t)
				ldh_s(A, MS, o_t + 2 * o.t);
			else
				ldh_s(A, MS, o_const + 2 * s32(k));
			if (o.m1_expand)
				emit_m1_expand(a);
			switch (o.mmode) {
			case 0:
				// mmode 0 (no multiplier) still goes through the shift and the
				// saturation below, with p as the only source: V DT HARD/SOFT
				// distort that way (doc/upstream.md #21)
				a.eor_reg(A, A, A);
				break;
			case 1:
				a.lsl_imm_x(A, A, 8 + 15);
				break;
			case 2:
				ldw_s(C, MS, o.m2_from_m ? o_m + 4 * o.sm : o_r + 4 * o.sr);
				a.mul_x(A, A, C);
				break;
			default:
				ldw_s(A, MS, o.m2_from_m ? o_m + 4 * o.sm : o_r + 4 * o.sr);
				a.lsl_imm_x(A, A, 15);
				break;
			}
			switch (o.asel) {
			case 0: a.mov_x(C, P); break;
			case 1: ldw_s(C, MS, o_r + 4 * o.sr); a.lsl_imm_x(C, C, 15); break;
			case 2: ldw_s(C, MS, o_m + 4 * o.sm); a.lsl_imm_x(C, C, 15); break;
			case 3: a.mov_x(C, P); a.asr_imm_x(C, C, 15); break;
			default: a.eor_reg(C, C, C); break;
			}
			switch (o.rop) {
			case 0: a.add_x(A, A, C); break;
			case 1: a.sub_x(A, A, C); break;
			case 2:
				// |a|: NEG does not set the flags here (unlike x86), so compare
				a.mov_x(D, C);
				a.neg_x(D, D);
				a.cmp_x(D, X31);
				a.csel_x(D, C, D, MI);
				a.add_x(A, A, D);
				break;
			default: a.and_x(A, A, C); break;
			}
			if (o.shift)
				a.lsl_imm_x(A, A, o.shift);
			// Only an unstaturated operation wraps at 42 bits: one that clamps
			// has to reach the limit first, or a strong distortion flips sign and
			// gains harmonics the machine does not have (doc/upstream.md #20)
			if (o.clamp == 0) {
				a.lsl_imm_x(A, A, 22);
				a.asr_imm_x(A, A, 22);
			}
			switch (o.clamp) {
			case 0: break;
			case 1:
				a.mov_imm64(C, u64(s64(-0x4000000000)));
				a.cmp_x(A, C);
				a.csel_x(A, C, A, LT);
				a.mov_imm64(C, 0x3fffffffff);
				a.cmp_x(A, C);
				a.csel_x(A, C, A, GT);
				break;
			case 2:
				a.eor_reg(C, C, C);
				a.cmp_x(A, C);
				a.csel_x(A, C, A, LT);
				a.mov_imm64(C, 0x3fffffffff);
				a.cmp_x(A, C);
				a.csel_x(A, C, A, GT);
				break;
			default:
				a.mov_x(D, A);
				a.neg_x(D, D);
				a.cmp_x(D, X31);
				a.csel_x(D, A, D, MI);
				a.mov_x(A, D);
				a.mov_imm64(C, 0x3fffffffff);
				a.cmp_x(A, C);
				a.csel_x(A, C, A, GT);
				break;
			}
			a.mov_x(P, A);
			if (o.latch) {
				a.cmp_x(P, X31);
				a.cset(C, MI);
				stb(C, SWP, o_flag_n);
				a.cmp_x(P, X31);
				a.cset(C, EQ);
				stb(C, SWP, o_flag_z);
			}
		}

		// ---- dm ----
		if (o.dm) {
			switch (o.dm_src) {
			case 0: case 1: case 2: case 3:
				a.mov_x(X0, MS);
				a.mov_imm32(X1, o.lfo);
				a.mov_imm64(X17, u64(uintptr_t(&meg_jit::call_lfo)));
				a.blr(X17);
				a.mov_reg(A, W0);                        // the result comes back in w0
				break;
			case 4:
				ldw(A, MS, o_ram_read);
				break;
			case 5:
				rnd();
				a.lsl_imm(A, A, 8);
				a.sar_imm(A, A, 8);
				break;
			case 6:
				p_packed(!o.no_noise);
				break;
			default:
				ldw(A, MS, o_m + 4 * o.sm);
				break;
			}
			stw(A, MS, o_mw_value + 4 * slot3(k));
		}
		if (k >= 0x17d) {
			a.mov_imm32(C, o.dm);
			stb(C, MS, o_mw_reg + slot3(k));
		}

		// ---- dr ----
		if (o.dr) {
			if (o.dr_from_r)
				ldw(A, MS, o_r + 4 * o.sr);
			else
				p_packed(!o.no_noise);
			stw(A, MS, o_rw_value + 4 * slot3(k));
		}
		if (k >= 0x17d) {
			a.mov_imm32(C, o.dr);
			stb(C, MS, o_rw_reg + slot3(k));
		}

		// ---- value to write to memory ----
		if (o.memw) {
			a.mov_x(A, P);
			a.asr_imm_x(A, A, 15);
			stw(A, MS, o_memw_val + 4 * slot2(k));
		}
		if (k >= 0x17e) {
			a.mov_imm32(C, o.memw ? 1 : 0);
			stb(C, MS, o_memw_act + slot2(k));
		}

		// ---- index ----
		if (o.index) {
			a.mov_x(A, P);
			a.asr_imm_x(A, A, 15 + 8);
			stw(A, MS, o_ix_value + 4 * slot3(k));
		}
		// An instruction that sets both idx and a memory write carries the second
		// index too; a read marked with bit 0x22 adds it
		if (o.index2) {
			a.mov_x(A, P);
			a.asr_imm_x(A, A, 15 + 8);
			stw(A, SWP, o_ix2_value + 4 * slot3(k));
		}
		if (k >= 0x17d) {
			a.mov_imm32(C, o.index ? 1 : 0);
			stb(C, MS, o_ix_act + slot3(k));
			a.mov_imm32(C, o.index2 ? 1 : 0);
			stb(C, SWP, o_ix2_act + slot3(k));
		}

		// ---- t ----
		if (o.t_write) {
			if (o.t_from_p)
				ldh(A, MS, o_t_value + 2 * slot2(k));
			else
				ldh(A, MS, o_const + 2 * s32(k));
			sth(A, MS, o_t + 2 * o.t);
		}
		if (need_tval[k]) {
			a.mov_x(A, P);
			if (o.index || o.index2) {
				a.asr_imm_x(A, A, 8);
				a.and_imm(A, A, 0x7fff);
			} else {
				a.asr_imm_x(A, A, 15 + 8);
				a.mov_imm64(C, u64(s64(-0x8000)));
				a.cmp_x(A, C);
				a.csel_x(A, C, A, LT);
				a.mov_imm64(C, 0x7fff);
				a.cmp_x(A, C);
				a.csel_x(A, C, A, GT);
			}
			sth(A, MS, o_t_value + 2 * slot2(k));
		}

		// ---- memory operation ----
		size_t table_done = 0;
		if (o.memop >= 2 && o.mem_table) {
			// A read marked with bit 0x23 addresses the reverb RAM absolutely: the
			// firmware writes the wave or curve table straight into it, so the
			// sample counter must not be subtracted (meg_state::step, doc/upstream.md #24)
			ldh(A, MS, o_offset + 2 * s32(o.offset_index));
			if (o.mem_use_index) {
				ldw(C, MS, o_ram_index);
				a.add_reg(A, A, C);
			}
			if (o.mem_use_index2) {
				ldw(C, SWP, o_ram_index2);
				a.add_reg(A, A, C);
			}
			if (o.memop == 3)
				a.add_imm(A, A, 1);
			a.and_imm_any(A, A, 0x3ffff);
			a.ldrh_sr(A, RAM, A);
			emit_revram_decode(a);
			stw(A, MS, o_memr_val + 4 * slot2(k));
			table_done = a.b();
		}
		if (o.memop) {
			ldh(A, MS, o_offset + 2 * s32(o.offset_index));
			if (o.mem_use_index) {
				ldw(C, MS, o_ram_index);
				a.add_reg(A, A, C);
			}
			if (o.mem_use_index2) {
				ldw(C, SWP, o_ram_index2);
				a.add_reg(A, A, C);
			}
			a.sub_reg(A, A, SC);
			if (o.memop == 3)
				a.add_imm(A, A, 1);
			a.and_imm_any(A, A, o.addr_mask);
			a.add_imm_any(A, A, o.addr_base);
			a.and_imm(A, A, 0x3ffff);
			if (o.memop == 1) {
				// meg_state::revram_encode written out in machine code, with the
				// address parked in E (the helper clobbers C and D)
				a.mov_x(E, A);
				ldw(A, MS, o_ram_write);
				emit_revram_encode(a);
				a.strh_sr(A, RAM, E);
			} else {
				// meg_state::revram_decode written out in machine code
				a.ldrh_sr(A, RAM, A);
				emit_revram_decode(a);
				stw(A, MS, o_memr_val + 4 * slot2(k));
			}
		}
		if (table_done)
			a.patch(table_done);
		if (k >= 0x17e) {
			a.mov_imm32(C, (o.memop == 2 || o.memop == 3) ? 1 : 0);
			stb(C, MS, o_memr_act + slot2(k));
		}
	}

	// exit
	stx(P, MS, o_p);
	a.ldp_x(X23, X30, X31, 16, true);
	a.ldp_x(X21, X22, X31, 16, true);
	a.ldp_x(X19, X20, X31, 16, true);
	a.ret();

	// a.code is a vector of 32-bit instructions, so every size here has to
	// count four bytes each (the x86 version's vector is bytes)
	const size_t bytes = a.code.size() * sizeof(a.code[0]);
	if (bytes > cd.buf_size) {
		if (cd.buf)
			exec_mem::free_mem(cd.buf, cd.buf_size);
		cd.buf_size = (bytes + 0xffff) & ~size_t(0xffff);
		cd.buf = exec_mem::alloc_rw(cd.buf_size);
		if (!cd.buf) {
			cd.buf_size = 0;
			return false;
		}
	}
	// make writable, copy, make executable again. Needed on rebuilds, which arrive still executable
	if (!exec_mem::make_writable(cd.buf, cd.buf_size))
		return false;
	std::memcpy(cd.buf, a.code.data(), bytes);
	if (!exec_mem::make_executable(cd.buf, cd.buf_size))
		return false;
	cd.fn = reinterpret_cast<fn_t>(cd.buf);
	return true;
}

#endif
