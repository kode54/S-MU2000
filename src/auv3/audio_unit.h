// license:BSD-3-Clause
//
// AUv3 の本体。中身は src/vst3/engine.h の engine（VST3 と同じもの）。

#ifndef S_MU2000_AUV3_AUDIO_UNIT_H
#define S_MU2000_AUV3_AUDIO_UNIT_H

#pragma once

#import <AudioToolbox/AudioToolbox.h>

NS_ASSUME_NONNULL_BEGIN

/// MU2000 一台。実機の端子をそのまま口にしてある（src/auv3/README 相当は doc/auv3.md）。
///
///   出力 0   MAIN OUT L/R（PHONES と DIGITAL OUT も同じ信号）
///   入力 0   A/D INPUT（AD1 が左、AD2 が右）
///   MIDI 入  ケーブル 0 = MIDI IN A（パート 1-16）、1 = MIDI IN B（パート 17-32）
///   MIDI 出  MIDI OUT（firmware の返事。XG の問い合わせやダンプ要求への応答）
@interface SMU2000AudioUnit : AUAudioUnit
@end

NS_ASSUME_NONNULL_END

#endif // S_MU2000_AUV3_AUDIO_UNIT_H
