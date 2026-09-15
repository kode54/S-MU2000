#!/usr/bin/env python3
# license:BSD-3-Clause
"""吸い出した ROM から、S-MU2000 が読む rom ディレクトリを組み立てる。

使い方:
    python tools/make_roms.py <入力...> [-o roms] [--link] [--v1]

入力は **zip でもディレクトリでもよい**。混ぜてもよい。
出力は既定で roms/（.gitignore に入っているので、吸い出したものが commit に
紛れ込まない）。**吸い出したものは repo の外に置いてよい。**

  python tools/make_roms.py ~/mu2000-dump                # 下を全部さらう
  python tools/make_roms.py ~/mu2000.zip ~/mu2000-dump   # 混ぜてもよい

ディレクトリは再帰的に見て、中にある zip も開く。名前は何でもよい
（ic25.bin でも 1.bin でも）。**中身の SHA1 で見分ける**ので、
そのまま／バイトスワップのどちらで吸ってあっても拾う。

出来上がるもの（README の表と同じ）:

    <出力>/mu2000_flash.bin        プログラム ROM 4MB（CPU から見えるまま）
    <出力>/dump/xv364a0.ic49 ほか 3 つ   波形 ROM 8MB × 4
    <出力>/standin/sin-table.bin   MEG が使う sin 表 64KB
    <出力>/hd44780u_b04.bin        LCD の字の絵（panel と gui が使う。無くてもよい）

**プログラム ROM は EX（v2.01）を優先する。** 両方あれば EX を使う。
tests/*.json の指紋は EX で焼いてあるので、v1.01 で作ると make test が
食い違う（音そのものは ±2 程度しか違わないが、指紋は PCM の SHA1 なので
1 ビットでも外れる）。v1.01 を敢えて使うなら --v1。
"""
import argparse
import hashlib
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "dump"))
# 見分ける表は verify_roms.py が持っている。二重に書かない
import verify_roms as vr

# プログラム ROM の組。上位（IC25）と下位（IC24）。**先にあるものを優先する**
PROG_PAIRS = [
    ("EX (v2.01)", "mu2000-v2.01-h.bin", "mu2000-v2.01-l.bin"),
    ("v1.01",      "xw87020.ic25",       "xw86920.ic24"),
]

# ROM ではないが rom ディレクトリに要るもの。名前と大きさで見分ける
STANDINS = {
    "sin-table.bin":    (0x10000, "standin/sin-table.bin"),
    "hd44780u_b04.bin": (0x1000,  "hd44780u_b04.bin"),
}


def blobs(paths):
    """入力から (人に見せる場所, 名前, 中身, 元のファイル) を吐く。zip は開く。

    元のファイルは、生のファイルのときだけ。zip の中身は None"""
    for p in paths:
        p = Path(p)
        if not p.exists():
            print("無い: %s" % p)
            continue
        if p.is_file():
            files = [p]
        else:
            files = sorted(q for q in p.rglob("*") if q.is_file())
        for q in files:
            if q.suffix.lower() == ".zip":
                try:
                    with zipfile.ZipFile(q) as z:
                        for n in z.namelist():
                            if not n.endswith("/"):
                                yield "%s!%s" % (q, n), Path(n).name, z.read(n), None
                except zipfile.BadZipFile:
                    print("zip として開けない: %s" % q)
            else:
                yield str(q), q.name, q.read_bytes(), q


def collect(paths):
    """MAME の名前 -> (中身, 元のファイル) と、stand-in の名前 -> 中身。

    元のファイルは、**そのまま使えるとき**（生のファイルで、向きも直していない）
    だけ入れる。zip から出したものと直したものは実体が無いので None"""
    roms, extra, seen = {}, {}, set()
    for where, name, data, raw in blobs(paths):
        key = hashlib.sha1(data).hexdigest()
        if key in seen:
            continue
        seen.add(key)

        # ROM は中身で見分ける。バイトスワップされていれば直す
        _, sha = vr.digest(data)
        hit = vr.BY_SHA1.get(sha)
        if hit:
            roms.setdefault(hit[0], (data, raw))
            print("  %-20s <- %s" % (hit[0], where))
            continue
        swapped = vr.byteswap(data)
        _, sha = vr.digest(swapped)
        hit = vr.BY_SHA1.get(sha)
        if hit:
            roms.setdefault(hit[0], (swapped, None))
            print("  %-20s <- %s（バイトスワップを直した）" % (hit[0], where))
            continue

        # stand-in は中身が決まっていない（作り直せる）ので名前と大きさで見る
        want = STANDINS.get(name)
        if want and len(data) == want[0]:
            extra.setdefault(name, data)
            print("  %-20s <- %s" % (name, where))
    return roms, extra


def interleave(hi, lo):
    """上位（IC25）と下位（IC24）から、CPU から見えるままの 4MB を作る。

    tools/dump/ydl_extract.py が 4MB を h/l に割るのと逆の手順。
    16bit 語ごとに、h が上位・l が下位で、それぞれ中で逆順に入っている"""
    if len(hi) != len(lo):
        raise ValueError("上位と下位の大きさが違う")
    img = bytearray(len(hi) * 2)
    for k in range(len(hi) // 2):
        img[4 * k + 0:4 * k + 2] = hi[2 * k:2 * k + 2][::-1]
        img[4 * k + 2:4 * k + 4] = lo[2 * k:2 * k + 2][::-1]
    return bytes(img)


def check_image(img):
    """並べ方を間違えていないか。SH-2 は 0x000000 に初期 PC、0x000004 に初期 SP を置く。

    **向きを間違えても「何か」は起動しようとする**ので、ここで見ておく"""
    be = lambda o: int.from_bytes(img[o:o + 4], "big")
    pc, sp = be(0), be(4)
    ok = 0 < pc < 0x400000 and (pc & 1) == 0 and 0xffff0000 <= sp <= 0xffffffff
    return ok, pc, sp


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="zip かディレクトリ（混ぜてよい）")
    ap.add_argument("-o", "--out", type=Path, default=ROOT / "roms")
    ap.add_argument("--link", action="store_true",
                    help="波形 ROM を複製せず symlink にする（32MB 節約）")
    ap.add_argument("--v1", action="store_true",
                    help="EX があっても v1.01 のプログラム ROM を使う")
    a = ap.parse_args()

    print("見つけたもの:")
    roms, extra = collect(a.inputs)
    if not roms and not extra:
        print("何も見つからなかった")
        return 1

    # ---- プログラム ROM。既定は EX（表の順）。--v1 なら v1.01 を先に見る
    pairs = sorted(PROG_PAIRS, key=lambda p: p[0] != "v1.01") if a.v1 else PROG_PAIRS
    chosen = next((p for p in pairs if p[1] in roms and p[2] in roms), None)
    have_ex = PROG_PAIRS[0][1] in roms and PROG_PAIRS[0][2] in roms

    print()
    lack_wave = sorted(n for n in vr.WAVE if n not in roms)
    missing = []
    if not chosen:
        missing.append("プログラム ROM の上位と下位が揃っていない")
    for n in lack_wave:
        missing.append("波形 ROM %s" % n)
    if "sin-table.bin" not in extra:
        missing.append("sin-table.bin（tools/dump/make_standins.py で作れる）")

    if missing:
        print("足りないもの:")
        for m in missing:
            print("  - %s" % m)
        # プログラムと波形は無いと鳴らない。sin 表だけなら警告で済ませる
        if not chosen or lack_wave:
            print("\nrom ディレクトリを作れない")
            return 1
        print()

    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "dump").mkdir(exist_ok=True)
    (a.out / "standin").mkdir(exist_ok=True)

    label, hi, lo = chosen[0], roms[chosen[1]][0], roms[chosen[2]][0]
    img = interleave(hi, lo)
    ok, pc, sp = check_image(img)
    (a.out / "mu2000_flash.bin").write_bytes(img)

    print("書いた:")
    print("  mu2000_flash.bin      %s  %.1fMB  SHA1 %s"
          % (label, len(img) / 1048576.0, hashlib.sha1(img).hexdigest()))
    print("     リセット後 PC=%08x SP=%08x  %s"
          % (pc, sp, "妥当" if ok else "**おかしい。並べ方を疑え**"))

    for n in sorted(vr.WAVE):
        data, src = roms[n]
        dst = a.out / "dump" / n
        if dst.exists() or dst.is_symlink():
            dst.unlink()
        # symlink にできるのは、**元のファイルがそのまま使えるとき**だけ。
        # zip から出したものと向きを直したものは実体が無いので書き出す
        if a.link and src is not None:
            dst.symlink_to(src.resolve())
            how = "-> %s" % src
        else:
            dst.write_bytes(data)
            how = "%.0fMB" % (len(data) / 1048576.0)
        print("  dump/%-16s %s" % (n, how))

    for n, (_, rel) in STANDINS.items():
        if n in extra:
            (a.out / rel).write_bytes(extra[n])
            print("  %-21s %d バイト" % (rel, len(extra[n])))

    print("\n出来た: %s" % a.out)
    if a.v1 and label == PROG_PAIRS[0][0]:
        print("（--v1 と言われたが v1.01 が見つからないので EX を使った）")
    if label != PROG_PAIRS[0][0]:
        print("**注意: プログラム ROM は %s。** tests/*.json の指紋は EX (v2.01) で" % label)
        print("焼いてあるので make test は食い違う。")
        if have_ex:
            print("（EX も見つかっている。--v1 を外せば EX を使う）")
        else:
            print("（EX の吸い出しが手元に無い。doc/dump/ の手順で updater から取れる）")
    print("使い方:  build/render %s <MIDI> out.wav" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
