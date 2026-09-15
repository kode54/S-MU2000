# AUv3（macOS の音源プラグイン）

実機の端子をそのまま口にしてある。

| 口 | 実機で言うと |
|---|---|
| 出力 0「Main Out」 2ch | MAIN OUT L/R。PHONES と DIGITAL OUT にも同じ信号が出ている |
| 入力 0「A/D Input」 2ch | A/D INPUT。左が AD1、右が AD2（サンプリングと A/D の系統に入る） |
| MIDI 入 ケーブル 0 | MIDI IN A（パート 1-16） |
| MIDI 入 ケーブル 1 | MIDI IN B（パート 17-32） |
| MIDI 出「MIDI Out」 | MIDI OUT（SH7043 の SCI ch0）。firmware の返事が出てくる |

VST3 は MIDI 入力 2 本と A/D INPUT までで、**MIDI OUT を出していない**。
AUv3 には MIDI 出力の口があるので、そこは実機に合わせて足した。

音源そのものは VST3 と同じ `src/vst3/engine.h` の `engine` を使う。VST3 の型は
一つも出てこないので、`smu2000::plug` という別名で呼んでいる。

```
make auv3           build/S-MU2000.app を作る（中に .appex が入る）
make install-auv3   ~/Applications へ複製する
make autest         .appex を通さずその場で試す道具
```

## 作りの地図

| | |
|---|---|
| `src/auv3/audio_unit.mm` | `AUAudioUnit` の中身。口・描き出し・状態の持ち帰り |
| `src/auv3/factory.mm` | `.appex` の入口（`NSExtensionPrincipalClass`） |
| `src/auv3/midi_split.h` | MIDI OUT のバイト列を 1 メッセージずつに切る |
| `src/auv3/main_app.mm` | 器のアプリ。音は出さない |
| `src/auv3/autest.mm` | その場で登録して口と音を確かめる |
| `src/vst3/hostpaths_mac.cpp` | ROM 置き場探しと記録（Windows 版と対） |
| `src/auv3/appex.entitlements` | **砂場の権利。これが無いと登録されない** |

## 気をつけるところ

**標本化周波数はホストに合わせる。** MU2000 は 44100Hz でしか動かないので、
`engine` が自前の窓関数付き sinc で変換する。先読みはしないので `latency` は 0。

**描き出しの中では確保も錠もしない。** 器は `allocateRenderResources` で取る。
`maximumFramesToRender` は 4096。

**MIDI は標本単位で効く。** 事象の位置で区間に割って `engine::fill()` を呼ぶ。

**MIDI OUT は音源の溜めを直に引いてはいけない。**
`engine::fill()` の中で `ui::driver::pump_out()` が先に引いてパネルの画面へ
渡してしまうので、後から `mu2000::midi_out_take()` を呼んでも空になっている。
`pump_out()` の echo からこちらの輪へ写し、`engine::midi_out()` はそれを読む。
（最初これで「MIDI OUT が何も返さない」ことになった。）

**起動は実時間で待つ。** ROM を読んで 4 秒ぶん空回しするのは `engine` の別の
スレッドで、描き出した量とは関係が無い。起動前の描き出しは無音をすぐ返すので、
回すだけでは一瞬で終わり「鳴らないプラグイン」に見える。

## 確かめたこと

`make autest` は `.appex` を通さず、`+[AUAudioUnit registerSubclass:]` で
その場に登録して鳴らす。実測:

```
口:
  出力 0  Main Out     2 ch
  入力 0  A/D Input    2 ch
  MIDI 出   MIDI Out
標本化周波数 48000 Hz / 遅れ 0.00 ms

MIDI IN A（ケーブル 0 → パート 1）  peak 0.0746  鳴った
MIDI IN B（ケーブル 1 → パート 17） peak 0.0742  鳴った
MAIN OUT   peak 0.0746  rms 0.01599（143360 フレーム / 48000 Hz）
A/D INPUT  引かれた回数 690
MIDI OUT   1 メッセージ / 15 バイト  （識別要求に返事が来た）
```

48000Hz で鳴っている＝変換器が通っている。MIDI OUT の 15 バイトは GM の
Identity Request（`F0 7E 7F 06 01 F7`）に対する firmware の返事。

## 登録には **App Sandbox の権利が要る**

**macOS の app extension は砂場に入っていないと登録されない。**
権利書を付けずに署名すると、次のように「もう少しで動きそう」な状態になり、
何が悪いのか分からない:

* LaunchServices はアプリも拡張も見えている
  （`lsregister -dump` に `plugin Identifiers: net.smu2000.S-MU2000.AU` が出る）
* 署名は正しい（`codesign --verify --deep --strict` が通り、Team ID も付く）
* それでも `pluginkit` にも `auval -a` にも出てこない
* `pkd` のログには何も出ない

足りなかったのは `com.apple.security.app-sandbox` だけだった
（`src/auv3/appex.entitlements`、器のアプリにも同じものを `app.entitlements`）。
署名するときに `--entitlements` で渡す。**証明書の種類は関係がない**
（ad-hoc でも登録される）。プロビジョニングプロファイルも要らなかった。

Xcode で作ると `ENABLE_APP_SANDBOX = YES` が同じものを埋めるので、この段は
自分で Makefile を書いたときだけ踏む。

## 起動は最後まで走らせ、写しておく

**`midi_ready()` は起動の終わりではない。** 8 秒ぶんほどで立つが、そこは
「MIDI を取りこぼさなくなった」だけで、実機の起動はまだ続いている
（実機でも電源を入れてから 24-26 秒かかる）。そこで口を開けると、
まだ動いている最中の機械に書き込むことになる。

なので **`engine::boot()` は 26 秒ぶん（`S_MU2000_BOOT_SECONDS` で変えられる）
回しきってから ready にする。** 出てくる音はその場で捨てる。回している間に
届いた MIDI は `engine::midi()` が溜めておき、ready になってから順に流れるので、
**起動が終わる前に口が開くことはない**。

起動中に音は出ない（測った。`render ... --boot 32` で 32 秒ぶん丸ごと peak 0）。
捨てる音は実際には無いが、出たとしても外へは出ない。

毎回 26 秒ぶん回すのは重いので、**一度やったら姿を写しておく**。

```
~/Library/Application Support/S-MU2000/bootcache/<ROM の鍵>-<設定の鍵>.bin
```

鍵は「プログラム ROM の中身」「起動時の NVRAM の中身」「回した秒数」の 3 つ。
どれかが変われば写しは使えないので、普通に起動して写し直す。保存の形が
変わったときは `load_state()` が版を見て断るので、やはり作り直される。
`S_MU2000_NO_BOOT_CACHE=1` で切れる。

砂場の中では容器の下に置かれる（`~/Library/Containers/<id>/Data/...`）。
DAW でそのプラグインを初めて挿したときだけ待たされ、以後は待たない。

実測（砂場の中、AUv3 として）:

| | 1 回目 | 2 回目以降 |
|---|---|---|
| ROM 読み込み | 13 ms | 13 ms |
| 起動 | **7.16 秒**（26 秒ぶんを回す） | **5 ms**（写しから戻す） |

**写しから戻した機械は、起動しきった機械と 1 バイトも違わない。**
`autest --state` で両方の姿を書き出して突き合わせてある（6096745 バイト、
SHA1 一致）。音も同じ（`peak 0.0748 / rms 0.01599` がどちらも出る）。
26 秒まで回した姿は `midi_ready` で止めた姿とは**違う**ので、
回しきることには意味がある（こちらも突き合わせで確かめた）。

## ホストに「ケーブルは 2 本」と言うこと

`AUAudioUnit.virtualMIDICableCount` は既定で **1**。名乗らないと:

* ホストは MIDI IN B（ケーブル 1）を使わない
* ファイルを鳴らす種類のホストは、代わりに**口ごとに音源をもう 1 台開く**。
  MU2000 が 2 台起きて、起動も 2 回になる（Cog の `AUPlayer` は
  `virtualMIDICableCount` を見て台数を決めている）

なので `- (NSInteger)virtualMIDICableCount { return PORTS; }` と書く。
これで 1 台が 2 口とも受け持つ。

## 起動は「器を用意するとき」に待つ

`fill()` は起動が終わるまで無音を返す。これだけだと、**実時間より速く回す
ホスト**（ファイルを鳴らすもの）では困る: ホストは待ってくれないので、
起動の数秒ぶんの壁時計の間に曲の十数秒ぶんを描き出してしまい、そこは丸ごと
無音になる。そこにあった音符は溜められたまま、遅れて一度に流れる
（＝聞こえない）。

だから **`allocateRenderResources` の中で `engine::wait_ready()` を呼んで、
起動が終わってから器を返す**。ここは実時間の糸ではないので待ってよい。
鳴り始めたときには機械はもう立ち上がっている。

## 初めて挿したときも待たせない（写しを焼いておく）

`make auv3 AUV3_ROMS=roms` は、ROM を入れたあとに**その場で一度起動して、
その姿をバンドルへ焼き込む**:

```
<appex>/Contents/Resources/bootcache/<鍵>.bin
```

`boot()` は「焼いてあるもの」→「自分で残したもの」の順に見る。
焼いてあれば、容器が空の状態で初めて挿したときでも 5 ms で立ち上がる。

**焼くときは空の HOME で走らせる。** 砂場の中のプラグインは NVRAM を持たない
（容器が空）ので、作る側に自分の設定が混ざると鍵が変わり、焼いた写しが
使われない。Makefile が `HOME=$$(mktemp -d)` でそれを避けている。

実測（容器を空にしてから、AUv3 として初めて挿す）:

```
ROM 読み込み: 14 ms
起動を飛ばした: .../Resources/bootcache/acecfc0a8ad6d49f-7e49fc62f6a837f5.bin（5 ms）
```

## 砂場の中からは自分のバンドルしか読めない

登録されるということは砂場に入るということで、`$HOME` は容器
（`~/Library/Containers/net.smu2000.S-MU2000.AU/Data`）へすり替えられる。
`~/Library/Application Support/S-MU2000/roms` も `roms.txt` の指す先も届かない。

読めるのは**自分のバンドルの中**なので、そこへ入れる:

```
make auv3 AUV3_ROMS=roms
```

`<appex>/Contents/Resources/roms` に複製され、`find_roms()` がそこを見つける。
**署名より前に入れること**（後から足すと封が破れる）。

ROM は配れないので既定では入れない。入れなければ登録も描き出しも普通に通り、
音だけが出ない（記録に「ROM が見つからない」と探した場所が残る）。

## 確かめたこと（登録された .appex で）

`make install-auv3` のあと、`build/autest --system` は
`AVAudioUnitComponentManager` で探して**別プロセスに読み込ませる**。
DAW が掴むのと同じ道:

```
（システムに登録された .appex を、別プロセスで掴んだ）
MIDI IN A（ケーブル 0 → パート 1）  peak 0.0746  鳴った
MIDI IN B（ケーブル 1 → パート 17） peak 0.0745  鳴った
MAIN OUT   peak 0.0746  rms 0.01599（143360 フレーム / 48000 Hz）
A/D INPUT  引かれた回数 690
MIDI OUT   1 メッセージ / 15 バイト  （識別要求に返事が来た）
```

容器の記録に、ROM をバンドルの中から読んだことが残る:

```
ROM: /Users/.../S-MU2000AU.appex/Contents/Resources/roms
起動: 音 7.90 秒ぶん / 実時間 1.67 秒
```

`auval -v aumu MU2k Smu2` も通る（**AU VALIDATION SUCCEEDED**）。
22050 / 44100 / 48000 / 96000 / 192000 Hz、64〜4096 フレーム、
細切れの描き出し、MIDI、どれも PASS。

なお v3 の拡張は `AudioComponentFindNext()` には出てこない。
探すときは `AVAudioUnitComponentManager` を使う。
