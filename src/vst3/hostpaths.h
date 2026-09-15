// license:BSD-3-Clause
//
// プラグインが機種ごとに違うことをするところ。engine.cpp から切り出した。
//
//   ・ROM 置き場を探す（自分の置かれた場所を起点にする）
//   ・記録を 1 行書く（画面が無いので、うまくいかなかったときはここを見てもらう）
//
// 中身は hostpaths_win.cpp（Windows の DLL）と hostpaths_mac.cpp（macOS の
// バンドル）に分かれている。engine.cpp はどちらか知らない。

#ifndef S_MU2000_VST3_HOSTPATHS_H
#define S_MU2000_VST3_HOSTPATHS_H

#pragma once

#include <string>

namespace smu2000 {
namespace vst3 {

// ROM 置き場を探す。見つかった場所を返す。無ければ空で、探した場所が tried に入る。
//
// 探す順は機種で少し違うが、考え方は同じ:
//   1. 環境変数 S_MU2000_ROMS（一番強い）
//   2. 自分（バンドル）の中の Resources
//   3. 自分のすぐ横の roms/
//   4. 場所を書いた紙（roms.txt）の 1 行目
//   5. 利用者ごとの決め打ちの置き場
std::string find_roms(std::string &tried);

// 記録へ 1 行。時刻を添える。書けなければ黙って捨てる
void logf(const char *fmt, ...);

// 書いてよい置き場（利用者ごと）。leaf はその下に作るディレクトリの名前。
// 作れなければ空を返す。
//
// **砂場の中では容器へすり替えられる。** AUv3 の拡張は砂場に入るので、
// ここは ~/Library/Containers/<id>/Data/... の下になる。それでよい
// （書ければいい置き場であって、他所と分け合うものではない）
std::string state_dir(const char *leaf);

// 自分（バンドル）の中の Resources。読むだけ。無ければ空。
// 作るときに焼き込んだもの（起動の写しなど）を、砂場の中からでも読める
std::string resource_dir();

} // namespace vst3
} // namespace smu2000

#endif // S_MU2000_VST3_HOSTPATHS_H
