// license:BSD-3-Clause
//
// AUv3 の本体。音源そのものは src/vst3/engine.h の engine（VST3 と同じもの）で、
// ここがやるのは AUv3 の作法に合わせることだけ。
//
// **実機の端子をそのまま口にする。**
//
//   出力 0    MAIN OUT L/R。PHONES と DIGITAL OUT にも同じ信号が出ている
//   入力 0    A/D INPUT。AD1 が左、AD2 が右（サンプリングと A/D の系統に入る）
//   MIDI 入   ケーブル 0 = MIDI IN A（パート 1-16）
//             ケーブル 1 = MIDI IN B（パート 17-32）
//   MIDI 出   MIDI OUT（SH7043 の SCI ch0）。firmware が送り出したもの
//
// MU2000 の中身は 44100Hz でしか動かないので、ホストの標本化周波数への変換は
// engine が自前の sinc でやる。その遅れは latency で申告する。
//
// **描き出しの中では確保も錠もしない。** 器は allocateRenderResources で取る。

#import "audio_unit.h"

#import <AVFoundation/AVFoundation.h>

#include "ui/midi_split.h"
#include "vst3/engine.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace {

constexpr AUAudioFrameCount MAX_FRAMES = 4096;
constexpr int PORTS = 2;                     // MIDI IN A と B

// 描き出しの中で使う器。確保はここでは**しない**（allocateRenderResources で済ませる）
struct scratch {
	std::vector<float> out_l, out_r;         // ホストが器を寄越さなかったとき用
	std::vector<float> in_l, in_r;           // A/D INPUT を引くところ
	// A/D INPUT を引くための AudioBufferList（2ch ぶん場所を取っておく）
	uint8_t            in_abl_mem[sizeof(AudioBufferList) + sizeof(AudioBuffer)] = {};
	std::vector<uint8_t> tx;                 // MIDI OUT の生バイト
	ui::midi_split split;         // それをメッセージに切る

	AudioBufferList *in_abl() { return reinterpret_cast<AudioBufferList *>(in_abl_mem); }

	void allocate(AUAudioFrameCount n)
	{
		out_l.assign(n, 0.0f);  out_r.assign(n, 0.0f);
		in_l.assign(n, 0.0f);   in_r.assign(n, 0.0f);
		tx.assign(4096, 0);
		split.reset();
	}
};

// MIDIOutputEventBlock へ 1 メッセージ渡すときの持ち物
struct emit_ctx {
	AUMIDIOutputEventBlock __unsafe_unretained block;
	AUEventSampleTime when;
};

void emit_one(void *ctx, const uint8_t *bytes, size_t n)
{
	emit_ctx *e = static_cast<emit_ctx *>(ctx);
	if (e->block)
		e->block(e->when, 0, NSInteger(n), bytes);
}

} // namespace


@implementation SMU2000AudioUnit {
	std::unique_ptr<smu2000::plug::engine> _engine;
	std::unique_ptr<scratch>               _scratch;
	AUAudioUnitBusArray                   *_inputBusArray;
	AUAudioUnitBusArray                   *_outputBusArray;
	AUAudioUnitBus                        *_inputBus;
	AUAudioUnitBus                        *_outputBus;
}

- (instancetype)initWithComponentDescription:(AudioComponentDescription)desc
                                     options:(AudioComponentInstantiationOptions)options
                                       error:(NSError **)outError
{
	self = [super initWithComponentDescription:desc options:options error:outError];
	if (!self)
		return nil;

	AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0
	                                                                   channels:2];

	// MAIN OUT。PHONES と DIGITAL OUT にも同じ信号が出ている
	_outputBus = [[AUAudioUnitBus alloc] initWithFormat:fmt error:nil];
	_outputBus.maximumChannelCount = 2;
	_outputBus.name = @"Main Out";

	// A/D INPUT。実機の背面の入力。サンプリングと A/D の系統に入る。
	// 繋がなくても鳴るので、ホストが何も寄越さなければ無音として扱う
	_inputBus = [[AUAudioUnitBus alloc] initWithFormat:fmt error:nil];
	_inputBus.maximumChannelCount = 2;
	_inputBus.name = @"A/D Input";

	_outputBusArray = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
	                                                         busType:AUAudioUnitBusTypeOutput
	                                                          busses:@[_outputBus]];
	_inputBusArray  = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
	                                                         busType:AUAudioUnitBusTypeInput
	                                                          busses:@[_inputBus]];

	_engine = std::make_unique<smu2000::plug::engine>();
	_scratch = std::make_unique<scratch>();

	// ROM を読んで起動するのは別スレッド。ここではすぐ返る
	_engine->start();

	self.maximumFramesToRender = MAX_FRAMES;
	return self;
}

- (void)dealloc
{
	_engine.reset();
	_scratch.reset();
}

- (AUAudioUnitBusArray *)inputBusses  { return _inputBusArray; }
- (AUAudioUnitBusArray *)outputBusses { return _outputBusArray; }

// 実機の MIDI OUT。1 本
- (NSArray<NSString *> *)MIDIOutputNames { return @[@"MIDI Out"]; }

// MPE は実機に無い
- (BOOL)supportsMPE { return NO; }

// **実機の MIDI IN は 2 口ある。** ここで 2 と言わないとホストは 1 と思い、
// ケーブル 1（MIDI IN B）を使わない。ファイルを鳴らす種類のホストは
// 代わりに「1 口ずつ別の音源を開く」ことがあり、MU2000 が 2 台起きて
// 起動も 2 回になる（Cog の AUPlayer がこれを見て台数を決める）
- (NSInteger)virtualMIDICableCount { return PORTS; }

// 標本化周波数の変換のぶんだけ音が遅れる
- (NSTimeInterval)latency
{
	const double rate = _outputBus.format.sampleRate;
	if (!_engine || rate <= 0.0)
		return 0.0;
	return double(_engine->latency_samples()) / rate;
}

- (BOOL)allocateRenderResourcesAndReturnError:(NSError **)outError
{
	if (![super allocateRenderResourcesAndReturnError:outError])
		return NO;

	// 入力と出力で周波数が食い違っていると、引いた音の長さが合わなくなる
	if (_inputBus.format.sampleRate != _outputBus.format.sampleRate) {
		if (outError)
			*outError = [NSError errorWithDomain:NSOSStatusErrorDomain
			                                code:kAudioUnitErr_FormatNotSupported
			                            userInfo:nil];
		return NO;
	}

	_engine->set_output_rate(_outputBus.format.sampleRate);
	_scratch->allocate(self.maximumFramesToRender);

	// **鳴らし始める前に、起動が終わるのをここで待つ。**
	//
	// ここは実時間の糸ではないので待ってよい。待たずに始めると、起動が
	// 終わるまで無音を返し続け、その間の MIDI は溜まるだけになる。実時間より
	// 速く回すホストでは、その無音が曲の頭の十数秒ぶんに化けて、そこにあった
	// 音符も失われる。写しがあれば数ミリ秒で戻ってくる（doc/auv3.md）
	// 起動しなかった（ROM が無いなど）ときも器は作る。無音で鳴るだけで、
	// 理由は記録に残っている
	(void)_engine->wait_ready(120.0);

	_engine->set_processing(true);
	return YES;
}

- (void)deallocateRenderResources
{
	_engine->set_processing(false);
	[super deallocateRenderResources];
}

- (AUInternalRenderBlock)internalRenderBlock
{
	// 描き出しの中で Objective-C を触らなくて済むよう、素のポインタで捕まえる
	smu2000::plug::engine *eng = _engine.get();
	scratch               *sc  = _scratch.get();
	// ARC の輪を作らないよう、弱い参照で捕まえる（この塊は自分が持つ）
	__unsafe_unretained SMU2000AudioUnit *unowned_self = self;

	return ^AUAudioUnitStatus(AudioUnitRenderActionFlags *actionFlags,
	                          const AudioTimeStamp       *timestamp,
	                          AUAudioFrameCount           frameCount,
	                          NSInteger                   outputBusNumber,
	                          AudioBufferList            *outputData,
	                          const AURenderEvent        *realtimeEventListHead,
	                          AURenderPullInputBlock      pullInputBlock)
	{
		(void)actionFlags; (void)outputBusNumber;
		if (frameCount > MAX_FRAMES)
			return kAudioUnitErr_TooManyFramesToProcess;

		// ---- 出力の器。ホストが mData を寄越さないことがある（その場合はこちらが出す）
		float *out_l = nullptr, *out_r = nullptr;
		if (outputData->mNumberBuffers >= 1) {
			if (!outputData->mBuffers[0].mData) {
				outputData->mBuffers[0].mData = sc->out_l.data();
				outputData->mBuffers[0].mDataByteSize = frameCount * sizeof(float);
			}
			out_l = static_cast<float *>(outputData->mBuffers[0].mData);
		}
		if (outputData->mNumberBuffers >= 2) {
			if (!outputData->mBuffers[1].mData) {
				outputData->mBuffers[1].mData = sc->out_r.data();
				outputData->mBuffers[1].mDataByteSize = frameCount * sizeof(float);
			}
			out_r = static_cast<float *>(outputData->mBuffers[1].mData);
		} else {
			out_r = sc->out_r.data();     // モノラルで頼まれたら右は捨てる
		}
		if (!out_l)
			return kAudioUnitErr_InvalidParameter;

		// ---- A/D INPUT。繋がっていなければ無音のまま
		const float *in_l = nullptr, *in_r = nullptr;
		if (pullInputBlock) {
			AudioBufferList *abl = sc->in_abl();
			abl->mNumberBuffers = 2;
			abl->mBuffers[0].mNumberChannels = 1;
			abl->mBuffers[0].mDataByteSize   = frameCount * sizeof(float);
			abl->mBuffers[0].mData           = sc->in_l.data();
			abl->mBuffers[1].mNumberChannels = 1;
			abl->mBuffers[1].mDataByteSize   = frameCount * sizeof(float);
			abl->mBuffers[1].mData           = sc->in_r.data();

			AudioUnitRenderActionFlags f = 0;
			if (pullInputBlock(&f, timestamp, frameCount, 0, abl) == noErr) {
				in_l = static_cast<const float *>(abl->mBuffers[0].mData);
				in_r = abl->mNumberBuffers >= 2
				           ? static_cast<const float *>(abl->mBuffers[1].mData)
				           : in_l;
			}
		}

		// ---- MIDI を挟みながら区間ごとに作る。事象の位置は標本単位で正しく効く
		const AURenderEvent *e = realtimeEventListHead;
		AUAudioFrameCount done = 0;
		while (done < frameCount) {
			// いま（done）までに来ている事象を流す
			while (e) {
				AUEventSampleTime off = e->head.eventSampleTime - timestamp->mSampleTime;
				if (off < 0)
					off = 0;                       // 遅れて届いたものは今すぐ
				if (AUAudioFrameCount(off) > done)
					break;
				if (e->head.eventType == AURenderEventMIDI ||
				    e->head.eventType == AURenderEventMIDISysEx) {
					const AUMIDIEvent *m = &e->MIDI;
					// ケーブル 0 が MIDI IN A、1 が B。それ以外は A に寄せる
					const int port = (m->cable < PORTS) ? int(m->cable) : 0;
					if (m->length)
						eng->midi(m->data, m->length, port);
				}
				e = e->head.next;
			}

			// 次の事象までを一息に作る
			AUAudioFrameCount upto = frameCount;
			if (e) {
				AUEventSampleTime off = e->head.eventSampleTime - timestamp->mSampleTime;
				if (off < 0)
					off = 0;
				if (AUAudioFrameCount(off) < upto)
					upto = AUAudioFrameCount(off);
			}
			if (upto <= done)
				upto = done + 1;                   // 同じ所で止まらないように
			if (upto > frameCount)
				upto = frameCount;

			const int n = int(upto - done);
			eng->fill(out_l + done, out_r + done, n,
			          in_l ? in_l + done : nullptr,
			          in_r ? in_r + done : nullptr);
			done = upto;
		}

		// ---- MIDI OUT。firmware が送り出したものをメッセージに切って渡す
		AUMIDIOutputEventBlock outBlock = unowned_self.MIDIOutputEventBlock;
		if (outBlock) {
			const size_t got = eng->midi_out(sc->tx.data(), sc->tx.size());
			if (got) {
				emit_ctx ctx{ outBlock, AUEventSampleTime(timestamp->mSampleTime) };
				sc->split.feed(sc->tx.data(), got, emit_one, &ctx);
			}
		}
		return noErr;
	};
}

// ---- 音色などの持ち帰り。DAW のプロジェクトに覚えさせる

static NSString *const kStateKey = @"S-MU2000.nvram";

- (NSDictionary<NSString *, id> *)fullState
{
	NSMutableDictionary *d = [[super fullState] mutableCopy] ?: [NSMutableDictionary dictionary];
	if (_engine) {
		std::vector<uint8_t> blob = _engine->save_state();
		if (!blob.empty())
			d[kStateKey] = [NSData dataWithBytes:blob.data() length:blob.size()];
	}
	return d;
}

- (void)setFullState:(NSDictionary<NSString *, id> *)state
{
	[super setFullState:state];
	NSData *d = state[kStateKey];
	if (_engine && [d isKindOfClass:[NSData class]] && d.length)
		_engine->load_state(static_cast<const uint8_t *>(d.bytes), d.length);
}

// 鳴らしっぱなしを消す（ホストが止めたとき）
- (void)reset
{
	if (_engine) {
		_engine->all_notes_off();
		_engine->flush_resampler();
	}
	if (_scratch)
		_scratch->split.reset();
}

@end
