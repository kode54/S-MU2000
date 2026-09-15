# macOS で作る

Apple Silicon / Intel の macOS で、Xcode の `clang++` だけで作れる。
MSYS2 も MinGW も要らない。

```
make
```

`Makefile` が `uname -s` を見て機種を決める。実行ファイルに `.exe` は付かない。

```
build/live     <rom ディレクトリ> [--midi 番号]     MIDI 入力を受けて鳴らす
build/live     --list                              MIDI 入力と音声の出口の一覧
build/render   <rom ディレクトリ> <MIDI> <出力 wav>  ファイルを WAV に
build/panel    <rom ディレクトリ>                   フロントパネルを文字で動かす
build/boot     <rom ディレクトリ> [サイクル数]       起動の確認
build/blocktime <rom> <MIDI> <フレーム数> [秒] [回数]  1 ブロックの所要時間を測る
```

`make test` の回帰試験も同じように走る（[testing.md](testing.md)）。
**音は Windows と同じもの**が出る。`tests/*.json` の指紋は Windows で焼いた
ものだが、macOS でもそのまま合う。

## まだ Windows だけのもの

| | なぜ |
|---|---|
| `gui` | 画面が Direct3D 11 と Win32（Dear ImGui の backend が win32/dx11 しか入っていない） |
| VST3 | 画面が上と同じ。バンドルの形も `Contents/MacOS/` に変える必要がある |
| `midisend` / `rec` | WinMM と WASAPI の取り込みを直に叩いている |

音を作るところ（`src/mame/`、`src/mu2000.cpp`）は機種で変わらない。
違うのは出入口だけなので、上の 3 つも足せる。

## 音声と MIDI

| | Windows | macOS |
|---|---|---|
| 音声出力 | WASAPI（`src/ui/audio_out.cpp`） | CoreAudio の AUHAL（`src/ui/audio_out_mac.cpp`） |
| MIDI 入力 | WinMM（`src/ui/midi_in.cpp`） | CoreMIDI（`src/ui/midi_in_mac.cpp`） |

口（`audio_out.h` / `midi_in.h`）は同じなので、呼ぶ側はどちらか知らない。
MIDI の輪っかへの積み方は機種で変わらないので `src/ui/midi_in_common.cpp` に
分けてある。

**時計は持たない。** CoreAudio は自分の実時間スレッドから「N フレームくれ」と
呼んでくるので、その中で音を作ってそのまま器へ書く。Windows 側が自前の
スレッドと溜めを持っているのは WASAPI が合図で起こす形だからで、こちらには
要らない。だから待ち時間は**デバイスの器の長さそのもの**になる。

`--latency` はその器の長さ（ミリ秒）として `kAudioDevicePropertyBufferFrameSize`
に渡す。デバイスが受け付ける範囲へ丸めるので、頼んだとおりにならないことがある
（`live` が開いた後に実際の周期を出す）。

標本化周波数の変換は自前の sinc でやる。384000Hz で回っている USB DAC でも
44100 から正しく上げる。

### AudioUnit に 1 回で渡せるフレーム数

**`kAudioUnitProperty_MaximumFramesPerSlice` を器と一緒に広げること。**
AudioUnit はこの上限を自分で持っていて（既定は 1156）、器がそれより大きいと
**描き出しの手続きが一度も呼ばれない**。`AudioUnitInitialize` も
`AudioOutputUnitStart` も `noErr` を返すので、戻り値では気付けない。
`--latency 20` で音が出ず `--latency 1` では出る、という形で出る。

`audio_out::start()` は、実際に呼ばれ始めたのを確かめてから成功を返す。
1 秒待っても鳴り出さなければ口を閉じて理由を返す（黙って待ち続けない）。

## MIDI 入力を用意する

macOS には既定で MIDI の送り口が無い。他のアプリから鳴らすには、
**Audio MIDI 設定**（`/System/Applications/Utilities/`）を開いて
ウインドウ → MIDI スタジオ → IAC ドライバ → 「装置はオンライン」に印を付ける。
`build/live --list` に出てくるようになる。

```
build/live --list                    番号と名前を見る
build/live roms --midi 0             その口から受ける
build/live roms --audio FIIO         音声の出口を名前の一部で選ぶ
```

## JIT は動かない

`swp30_jit.cpp` と `sh2_jit.cpp` は x86-64 の機械語を吐くので、Windows の
x86-64 でだけ使う。macOS では解釈実行に落ちる。**出る音は同じ**（JIT は
解釈実行とビット単位で一致するように書いてある）が、そのぶん遅い。

Apple Silicon の実測では、16 パート同時でも実時間の 35-50%（1 コア）で収まる。
`build/blocktime` で自分の機械の最悪値を測れる。

## 設定の置き場

実機の電池で保持される RAM を、`live` が終わるときに残す。

| | |
|---|---|
| Windows | `%LOCALAPPDATA%\S-MU2000\nvram\` |
| macOS | `~/Library/Application Support/S-MU2000/nvram/` |

ファイル名はプログラム ROM の FNV-1a。`--factory` を付けて起動すると
覚えている設定を捨てる。
