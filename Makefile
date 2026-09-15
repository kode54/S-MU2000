# S-MU2000
#
#   make          verify / boot / render / live を作る
#                 render は MIDI ファイルを WAV に書き出す
#                 live   は MIDI 入力を受けてその場で鳴らす
#   make test     回帰試験（ROM が無ければ verify だけ）
#   make clean    消す
#
# Windows は MSYS2 / MinGW-w64 の g++、macOS は Xcode の clang++ を想定している。
# C++20 が要る（sh.cpp が std::rotl / std::rotr を使う）。
#
# 作れるものは機種で変わる（下の all を見よ）。音を作るところは同じで、
# 違うのは音声と MIDI の出入口と画面だけ:
#
#   Windows   WASAPI / WinMM / Direct3D 11 / VST3
#   macOS     CoreAudio / CoreMIDI（gui と vst3 はまだ）
#
# The SH2 and MEG JITs run on x86-64 (Windows / macOS) and arm64 (macOS).
# Anything else falls back to the interpreter (the output is identical)

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
MACOS := 1
CXX      ?= clang++
PYTHON   ?= python3
# 実行ファイルに .exe は付けない
EXE      :=
else
CXX      ?= g++
PYTHON   ?= python
EXE      := .exe
endif
# 音を作るのは重いので最適化を上げる。-O2 より 6% 速い
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wno-unused-variable -Wno-unused-but-set-variable

# 自分の CPU に合わせるとさらに 4% ほど速いが、他の機械では動かなくなる。
#   make MARCH=native
ifdef MARCH
CXXFLAGS += -march=$(MARCH)
endif
CXXFLAGS += -I src -I src/compat
# ヘッダを直したときに .o を作り直させる
CXXFLAGS += -MMD -MP

ifdef MACOS
# macOS は静的リンクしない（Apple は libSystem の静的リンクを認めていない）。
# 音声と MIDI は OS の枠組みを使う
LDFLAGS   ?=
FW_AUDIO  := -framework CoreAudio -framework AudioToolbox -framework AudioUnit \
             -framework CoreFoundation
FW_MIDI   := -framework CoreMIDI
else
# MSYS2 の DLL に依存させない。動的リンクのままだと、MSYS2 の環境の外
# （素の PowerShell など）では起動に失敗して何も言わずに終わる
LDFLAGS ?= -static -static-libgcc -static-libstdc++
endif

BUILD := build

SRCS := \
	src/compat/compat.cpp \
	src/smartmedia.cpp \
	src/mame/sound/swp30.cpp \
	src/mame/sound/swp30_jit.cpp \
	src/mame/video/hd44780.cpp \
	src/mame/machine/sci4.cpp \
	src/mame/cpu/sh.cpp \
	src/mame/cpu/sh2.cpp 	src/mame/cpu/sh2_jit.cpp \
	src/compat/a64asm.cpp \
	src/mame/cpu/sh7042.cpp \
	src/mame/cpu/sh_adc.cpp \
	src/mame/cpu/sh_bsc.cpp \
	src/mame/cpu/sh_cmt.cpp \
	src/mame/cpu/sh_dmac.cpp \
	src/mame/cpu/sh_intc.cpp \
	src/mame/cpu/sh_mtu.cpp \
	src/mame/cpu/sh_port.cpp \
	src/mame/cpu/sh_sci.cpp

OBJS := $(SRCS:%.cpp=$(BUILD)/%.o)

ifdef MACOS
# macOS で作れるもの。gui（Direct3D 11）と vst3、midisend / rec（WinMM）は
# まだ Windows だけ。doc/macos.md を見よ
all: $(BUILD)/verify$(EXE) $(BUILD)/boot$(EXE) $(BUILD)/render$(EXE) \
     $(BUILD)/live$(EXE) $(BUILD)/panel$(EXE) \
     $(BUILD)/statetest$(EXE) $(BUILD)/blocktime$(EXE) \
     $(BUILD)/xgtest$(EXE) $(BUILD)/samptest$(EXE)
else
# vst3 と vst3probe は下で定義している。変数はまだ空なので名前で書く
all: $(BUILD)/verify$(EXE) $(BUILD)/boot$(EXE) $(BUILD)/render$(EXE) \
     $(BUILD)/live$(EXE) $(BUILD)/midisend$(EXE) $(BUILD)/panel$(EXE) $(BUILD)/gui$(EXE) \
     $(BUILD)/statetest$(EXE) $(BUILD)/rec$(EXE) $(BUILD)/blocktime$(EXE) \
     vst3 $(BUILD)/vst3probe$(EXE)
endif

$(BUILD)/verify$(EXE): $(OBJS) $(BUILD)/src/verify.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/boot$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/boot.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# 1 ブロックを作るのに何 ms かかるかを測る。音声デバイスは使わない。
# 待ち時間の下限はこの最悪値で決まる（doc/todo.md 2 番）
$(BUILD)/blocktime$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(BUILD)/src/blocktime.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# パラメータの層の定義表を firmware に確かめさせる（doc/params.md）
$(BUILD)/xgtest$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/xg/model.o $(BUILD)/src/xgtest.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# インサーションのパラメータの表（src/xg/fx_params.h）を firmware の LCD から作る（doc/pc-editor.md）。
#   build/fxsweep.exe ../MU2000/roms > fxsweep.txt
#   python tools/fxsweep/make_fx_params.py fxsweep.txt src/xg/fx_params.h
$(BUILD)/fxsweep$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/tools/fxsweep/fxsweep.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/render$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(BUILD)/src/render.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# samptest はサンプリング（録音して試聴する）が一回りするかを確かめる
$(BUILD)/samptest$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/samptest.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# statetest は状態の保存と復元が正しいかを確かめる
$(BUILD)/statetest$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(BUILD)/src/statetest.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# panel はフロントパネル（LCD とボタン）を文字だけで動かす
$(BUILD)/panel$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(BUILD)/src/panel.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# GDI の肩代わり（macOS）。パネル・エディタ・エフェクトの絵は GDI で描いてあり、
# その .cpp には手を触れずに、GDI の側を CoreGraphics と CoreText で用意する
ifdef MACOS
GDI_SRC  := src/compat/gdicompat_mac.mm
GDI_OBJ  := $(BUILD)/src/compat/gdicompat_mac.o
FW_DRAW  := -framework CoreGraphics -framework CoreText -framework ImageIO \
            -framework CoreFoundation -framework CoreServices

$(BUILD)/src/compat/%.o: src/compat/%.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -ObjC++ -c -o $@ $<

$(BUILD)/src/ui/%.o: src/ui/%.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -ObjC++ -c -o $@ $<

# 肩代わりした GDI でパネルの絵が出るかを見る。画面も音源も要らない
$(BUILD)/paneltest$(EXE): $(GDI_OBJ) $(BUILD)/src/ui/paneltest.o \
                          $(BUILD)/src/ui/panel.o $(BUILD)/src/ui/layout.o \
                          $(BUILD)/src/ui/svg.o $(BUILD)/src/ui/editor.o \
                          $(BUILD)/src/ui/effects.o $(BUILD)/src/ui/png.o \
                          $(BUILD)/src/xg/model.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(FW_DRAW)

paneltest: $(BUILD)/paneltest$(EXE)

endif

# 音声と MIDI の出入口。口（audio_out.h / midi_in.h）は同じで、中身が機種で違う
ifdef MACOS
AUDIO_OUT_SRC := src/ui/audio_out_mac.cpp
MIDI_IN_SRCS  := src/ui/midi_in_common.cpp src/ui/midi_in_mac.cpp
else
AUDIO_OUT_SRC := src/ui/audio_out.cpp
MIDI_IN_SRCS  := src/ui/midi_in_common.cpp src/ui/midi_in.cpp
endif
AUDIO_OUT_OBJ := $(AUDIO_OUT_SRC:%.cpp=$(BUILD)/%.o)
MIDI_IN_OBJS  := $(MIDI_IN_SRCS:%.cpp=$(BUILD)/%.o)


# gui は実機のフロントパネル風の画面を出す
UI_SRCS := src/ui/panel.cpp src/ui/editor.cpp src/ui/effects.cpp src/ui/png.cpp \
           $(AUDIO_OUT_SRC) src/ui/audio_in.cpp $(MIDI_IN_SRCS) src/ui/midi_out.cpp \
           src/ui/layout.cpp src/ui/svg.cpp src/ui/player.cpp src/xg/model.cpp
UI_OBJS := $(UI_SRCS:%.cpp=$(BUILD)/%.o)

# PC エディタ（doc/pc-editor.md）。Dear ImGui（MIT）を third_party/imgui に取り込んである。
# 描画は Direct3D 11。ゲームパッドは使わないので XInput は外す
IMGUI_DIR  := third_party/imgui
IMGUI_SRCS := $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp               $(IMGUI_DIR)/imgui_widgets.cpp $(IMGUI_DIR)/backends/imgui_impl_win32.cpp               $(IMGUI_DIR)/backends/imgui_impl_dx11.cpp
PC_SRCS    := src/ui/pc_editor.cpp src/ui/pc_window.cpp src/ui/xg_ui.cpp src/ui/overview.cpp src/ui/fx_editor.cpp src/ui/fx_help.cpp
PC_OBJS    := $(IMGUI_SRCS:%.cpp=$(BUILD)/imgui/%.o) $(PC_SRCS:%.cpp=$(BUILD)/imgui/%.o)
ifdef MACOS
IMGUI_FLAGS := -I $(IMGUI_DIR)
else
IMGUI_FLAGS := -I $(IMGUI_DIR) -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD
endif

ifdef MACOS
# macOS の画面。実機のフロントパネルを出して、その場で鳴らす。
# 絵は Windows 版と同じ ui::panel が描き、GDI は gdicompat_mac が肩代わりする
# PC エディタの画面。ImGui の中核と、macOS 用の backend（osx と metal）
IMGUI_MAC_SRCS := $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp \
                  $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp \
                  $(IMGUI_DIR)/backends/imgui_impl_osx.mm \
                  $(IMGUI_DIR)/backends/imgui_impl_metal.mm
PC_MAC_SRCS    := src/ui/pc_window_mac.mm src/ui/pc_editor.cpp src/ui/xg_ui.cpp \
                  src/ui/overview.cpp src/ui/fx_editor.cpp src/ui/fx_help.cpp
IMGUI_MAC_OBJS := $(IMGUI_MAC_SRCS:%.cpp=$(BUILD)/imgui/%.o)
IMGUI_MAC_OBJS := $(IMGUI_MAC_OBJS:%.mm=$(BUILD)/imgui/%.o)
PC_MAC_OBJS    := $(PC_MAC_SRCS:%.cpp=$(BUILD)/imgui/%.o)
PC_MAC_OBJS    := $(PC_MAC_OBJS:%.mm=$(BUILD)/imgui/%.o)

GUI_MAC_SRCS := src/gui_mac.mm src/compat/gdicompat_mac.mm \
                src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/editor.cpp \
                src/ui/effects.cpp src/ui/png.cpp src/xg/model.cpp \
                $(AUDIO_OUT_SRC) $(MIDI_IN_SRCS)
GUI_MAC_OBJS := $(GUI_MAC_SRCS:%.cpp=$(BUILD)/%.o)
GUI_MAC_OBJS := $(GUI_MAC_OBJS:%.mm=$(BUILD)/%.o)

$(BUILD)/src/%.o: src/%.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -ObjC++ -fobjc-arc -c -o $@ $<

$(BUILD)/imgui/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) -ObjC++ -fobjc-arc -c -o $@ $<

# gui_mac.mm は ImGui も触るので、そちらの取り込み先も要る
$(BUILD)/src/gui_mac.o: CXXFLAGS += $(IMGUI_FLAGS)

$(BUILD)/gui_mac$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(GUI_MAC_OBJS) \
                        $(IMGUI_MAC_OBJS) $(PC_MAC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -framework Cocoa -framework Metal \
	       -framework MetalKit -framework QuartzCore -framework GameController \
	       $(FW_DRAW) $(FW_AUDIO) $(FW_MIDI)

gui_mac: $(BUILD)/gui_mac$(EXE)
endif

$(BUILD)/imgui/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) -c -o $@ $<

$(BUILD)/src/gui.o: CXXFLAGS += $(IMGUI_FLAGS)

ifndef MACOS
$(BUILD)/gui$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(UI_OBJS) $(PC_OBJS) $(BUILD)/src/gui.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lgdi32 -luser32 -lavrt -lcomdlg32 -lshell32 	       -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32

# midisend は MIDI ファイルを実時間で MIDI 出力へ流す（live の試験用）
$(BUILD)/midisend$(EXE): $(BUILD)/src/smf.o $(BUILD)/src/midisend.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lavrt

# rec は音声入力を WAV に録る。実機の音（S/PDIF 入力）と突き合わせるため。
# 録りながら MIDI を実機へ流せるので、同じ譜面の実機とこちらを 1 回で並べられる
$(BUILD)/rec$(EXE): $(BUILD)/src/smf.o $(BUILD)/src/rec.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -luuid
endif

# live は MIDI 入力を受けてその場で鳴らす。
#   Windows  WinMM の MIDI 入力 + WASAPI
#   macOS    CoreMIDI + CoreAudio
$(BUILD)/live$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(MIDI_IN_OBJS) $(AUDIO_OUT_OBJ) $(BUILD)/src/live.o
	@mkdir -p $(dir $@)
ifdef MACOS
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(FW_AUDIO) $(FW_MIDI)
else
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lavrt
endif

# ---- AUv3 プラグイン（macOS）
#
# 実機の端子をそのまま口にしてある（doc/auv3.md）:
#   出力 MAIN OUT L/R / 入力 A/D INPUT / MIDI 入 ケーブル 0=IN A, 1=IN B / MIDI 出 MIDI OUT
#
# **AUv3 はアプリの中の .appex でないとシステムが認めない。** だから音を出さない
# 器のアプリを一緒に作る。一度起動すると DAW の一覧に出る。
#
#   make auv3           build/S-MU2000.app を作る（中に .appex が入る）
#   make install-auv3   ~/Applications へ複製して一度起動する（登録される）
#   make auval          auval でプラグインを検査する

ifdef MACOS

AUV3_APP   := $(BUILD)/S-MU2000.app
AUV3_APPEX := $(AUV3_APP)/Contents/PlugIns/S-MU2000AU.appex
AUV3_BIN   := $(AUV3_APPEX)/Contents/MacOS/S-MU2000AU
AUV3_HOST  := $(AUV3_APP)/Contents/MacOS/S-MU2000

# 音源の中身は VST3 と同じ engine を使う（VST3 の型は一つも出てこない）
AUV3_SRCS := src/auv3/audio_unit.mm src/auv3/factory.mm \
             src/vst3/engine.cpp src/vst3/hostpaths_mac.cpp
AUV3_OBJS := $(AUV3_SRCS:%.cpp=$(BUILD)/auv3obj/%.o)
AUV3_OBJS := $(AUV3_OBJS:%.mm=$(BUILD)/auv3obj/%.o)

# 署名に使う証明書。ad-hoc（-）でも登録される（要るのは砂場の権利のほう）。
# 配るときは Developer ID で
#   security find-identity -v -p codesigning   で手元の証明書が見られる
CODESIGN_ID ?= -

# ROM をバンドルの中へ入れる。
#
# **砂場の中からは自分のバンドルの中しか読めない。** AUv3 の拡張は砂場に
# 入るので（入らないとそもそも登録されない）、$HOME は容器へすり替えられ、
# ~/Library/Application Support も roms.txt の指す先も届かない。
# AUv3 として鳴らすには、ここに ROM を入れておくしかない。
#
#   make auv3 AUV3_ROMS=roms
#
# ROM は配れないので、既定では入れない（入れなければ無音のまま動く）
AUV3_ROMS ?=

AUV3_FLAGS := -fobjc-arc
AUV3_FW    := -framework Foundation -framework AudioToolbox -framework AVFoundation \
              -framework CoreAudio -framework CoreMIDI

$(BUILD)/auv3obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/auv3obj/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(AUV3_FLAGS) -ObjC++ -c -o $@ $<

# バンドルの仕上げ（ROM を入れて署名する）は**毎回やる**。
# 実行ファイルを作り直したときだけにすると、後から AUV3_ROMS を付け足しても
# 何も起きない（一度これで「無音のまま」になった）
auv3: $(AUV3_HOST) $(BUILD)/autest$(EXE)
	# ROM をバンドルへ。**署名より前に**入れること（後から足すと封が破れる）
ifneq ($(AUV3_ROMS),)
	@rm -rf $(AUV3_APPEX)/Contents/Resources/roms
	@mkdir -p $(AUV3_APPEX)/Contents/Resources
	@cp -R $(AUV3_ROMS) $(AUV3_APPEX)/Contents/Resources/roms
	@echo "ROM を入れた: $(AUV3_ROMS)"
	# 起動の写しをここで作って焼き込む。**初めて挿したときに待たせない。**
	#
	# 砂場の中のプラグインは NVRAM を持たない（容器が空）ので、鍵が合うように
	# **こちらも空の HOME で作る**。そうしないと自分の設定が混ざって鍵が変わり、
	# 焼いた写しが使われない
	@rm -rf $(AUV3_APPEX)/Contents/Resources/bootcache
	@tmp=$$(mktemp -d); 	 HOME=$$tmp S_MU2000_ROMS=$(AUV3_ROMS) $(BUILD)/autest$(EXE) --state /dev/null >/dev/null 2>&1; 	 if [ -d "$$tmp/Library/Application Support/S-MU2000/bootcache" ]; then 	   mkdir -p $(AUV3_APPEX)/Contents/Resources/bootcache; 	   cp "$$tmp/Library/Application Support/S-MU2000/bootcache/"*.bin 	      $(AUV3_APPEX)/Contents/Resources/bootcache/ 2>/dev/null; 	   echo "起動の写しを焼いた: $$(ls $(AUV3_APPEX)/Contents/Resources/bootcache | head -1)"; 	 else echo "起動の写しを作れなかった（初回は待たされる）"; fi; 	 rm -rf "$$tmp"
else
	@rm -rf $(AUV3_APPEX)/Contents/Resources/roms
	@rm -rf $(AUV3_APPEX)/Contents/Resources/bootcache
endif
	#
	# **砂場（App Sandbox）の権利が要る。** macOS の app extension は砂場に
	# 入っていないとシステムが登録しない。権利書を付けずに署名すると、
	# LaunchServices までは見えているのに pluginkit には出てこない、という
	# 分かりにくい止まり方をする。**証明書の種類は関係ない**（ad-hoc でも通る）
	@codesign --force --sign "$(CODESIGN_ID)" --timestamp=none \
	          --entitlements src/auv3/appex.entitlements $(AUV3_APPEX)
	@codesign --force --sign "$(CODESIGN_ID)" --timestamp=none \
	          --entitlements src/auv3/app.entitlements $(AUV3_APP)
	@echo "出来た: $(AUV3_APP)"

# .appex の本体。入口は NSExtensionMain（main() は持たない）
$(AUV3_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(AUV3_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(AUV3_FW) \
	       -e _NSExtensionMain -fapplication-extension
	@cp -f src/auv3/Info-appex.plist $(AUV3_APPEX)/Contents/Info.plist

# 器のアプリ。音は出さない。.appex を抱えて登録させるだけ
$(AUV3_HOST): $(AUV3_BIN) $(BUILD)/auv3obj/src/auv3/main_app.o \
              $(BUILD)/auv3obj/src/vst3/hostpaths_mac.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(BUILD)/auv3obj/src/auv3/main_app.o \
	       $(BUILD)/auv3obj/src/vst3/hostpaths_mac.o $(LDFLAGS) -framework Cocoa
	@cp -f src/auv3/Info-app.plist $(AUV3_APP)/Contents/Info.plist
	@mkdir -p $(AUV3_APP)/Contents/Resources
	@cp -f LICENSE $(AUV3_APP)/Contents/Resources/LICENSE.txt
	@cp -f NOTICE.txt $(AUV3_APP)/Contents/Resources/NOTICE.txt

# 登録させる。~/Applications に置いて一度起動する
install-auv3: auv3
	rm -rf "$(HOME)/Applications/S-MU2000.app"
	@mkdir -p "$(HOME)/Applications"
	cp -R $(AUV3_APP) "$(HOME)/Applications/"
	@echo "入れた: $(HOME)/Applications/S-MU2000.app"
	@echo "一度起動すると DAW の一覧に出る（open してよいか聞かれたら許可する）"

# .appex にせず、その場で登録して口と音を確かめる（doc/auv3.md）
$(BUILD)/autest$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(AUV3_OBJS) \
                       $(BUILD)/auv3obj/src/auv3/autest.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(AUV3_FW)

autest: $(BUILD)/autest$(EXE)

auval: install-auv3
	@sleep 2
	auval -v aumu MU2k Smu2

endif

# ---- VST3 プラグイン
#
# Steinberg の SDK は使わず、インターフェース定義（MIT）だけを取り込んである。
# third_party/vst3/README.md を見よ。
#
#   make vst3      build/S-MU2000.vst3/ にバンドルを作る
#   make install-vst3   それを VST3 の置き場へ複製する

ifndef MACOS
VST3_DIR  := $(BUILD)/S-MU2000.vst3
VST3_BIN  := $(VST3_DIR)/Contents/x86_64-win/S-MU2000.vst3
VST3_INC  := -I third_party/vst3

VST3_SDK_SRCS := 	third_party/vst3/pluginterfaces/base/funknown.cpp 	third_party/vst3/pluginterfaces/base/coreiids.cpp 	third_party/vst3/pluginterfaces/base/conststringtable.cpp 	third_party/vst3/pluginterfaces/base/ustring.cpp

VST3_SRCS := src/vst3/plugin.cpp src/vst3/engine.cpp src/vst3/iids.cpp \
             src/vst3/view.cpp src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/editor.cpp \
             src/ui/effects.cpp src/xg/model.cpp $(VST3_SDK_SRCS)
VST3_OBJS := $(VST3_SRCS:%.cpp=$(BUILD)/vst3obj/%.o)

$(BUILD)/vst3obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) -c -o $@ $<

vst3: $(VST3_BIN)

$(VST3_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(VST3_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lgdi32 -luser32 -lavrt -lcomdlg32
	@mkdir -p $(VST3_DIR)/Contents/Resources
	@cp -f doc/vst3-readme.txt $(VST3_DIR)/Contents/Resources/README.txt 2>/dev/null || true
	# 取り込んだものの著作権表示。BSD-3 はバイナリで配るときも添えろと言っている
	@cp -f LICENSE $(VST3_DIR)/Contents/Resources/LICENSE.txt
	@cp -f NOTICE.txt $(VST3_DIR)/Contents/Resources/NOTICE.txt

# 既定の置き場へ入れる。管理者権限が要ることがある
VST3_INSTALL ?= $(PROGRAMFILES)/Common Files/VST3

install-vst3: $(VST3_BIN)
	rm -rf "$(VST3_INSTALL)/S-MU2000.vst3"
	cp -r $(VST3_DIR) "$(VST3_INSTALL)/"
	@echo "入れた: $(VST3_INSTALL)/S-MU2000.vst3"

# 工場が名乗るかどうかだけを確かめる小さな道具
$(BUILD)/vst3probe$(EXE): $(BUILD)/vst3obj/src/vst3/probe.o $(BUILD)/vst3obj/src/vst3/iids.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/funknown.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/coreiids.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/conststringtable.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/ustring.o                         $(BUILD)/src/smf.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lole32

probe: $(BUILD)/vst3probe$(EXE) $(VST3_BIN)
	$(BUILD)/vst3probe$(EXE) $(VST3_BIN)
endif

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 内蔵周辺のレジスタ振り分けは MAME の map() から起こす。
# MAME のソースの場所は MAME_SH7042 で渡す
MAME_SH7042 ?= ../MU2000/mame-src/src/devices/cpu/sh/sh7042.cpp

regen:
	python tools/gen_sh7042_map.py $(MAME_SH7042)

ifdef MACOS
# まだ移していないもの。黙って「ルールが無い」と言われるより、理由を出す
gui vst3 install-vst3 probe:
	@echo "$@ はまだ Windows だけ（画面が Direct3D 11、音声と MIDI が WinMM）。"
	@echo "doc/macos.md を見よ。macOS では live / render / panel が使える"
	@false
endif

# 回帰試験。直したことで音が変わっていないかを見る。
#
# ROM は同梱できないので、ROM が無い機械では verify だけが走る（それが正しい）。
# ROM の置き場は SMU2000_ROMS で渡せる。既定は roms/ か ../MU2000/roms。
#   make test                     全部
#   make test T=piano             1 件だけ
#   make test-update              指紋を焼き直す（意図して音を変えたとき）
TEST_EXES := $(BUILD)/verify$(EXE) $(BUILD)/statetest$(EXE) $(BUILD)/render$(EXE) \
             $(BUILD)/xgtest$(EXE) $(BUILD)/samptest$(EXE)

test: $(TEST_EXES)
	$(PYTHON) tools/run_tests.py $(if $(T),--only $(T),)

test-update: $(TEST_EXES)
	$(PYTHON) tools/run_tests.py --update $(if $(T),--only $(T),)

clean:
	rm -rf $(BUILD)

# ヘッダを直したときに .o を作り直させる仕掛け（-MMD -MP が置く .d）。
#
# **1 つずつ並べてはいけない。** 並べ忘れた .o はヘッダを直しても作り直されず、
# 型の大きさが食い違ったまま繋がって落ちる（statetest がこれで落ちていた。
# swp30.h に変数を 1 つ足したら、古い大きさのまま繋がった mu2000.o が
# 別の場所を触りに行っていた）。だから build の下にある .d を全部拾う
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

.PHONY: all clean regen gui vst3 install-vst3 probe test test-update \
        auv3 install-auv3 auval autest paneltest gui_mac
