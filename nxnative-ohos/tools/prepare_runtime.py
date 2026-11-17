#!/usr/bin/env python3
"""prepare_runtime.py —— 用 nxnative 转译产物生成鸿蒙宿主的绑定头（泛化版）。

从 game-work/prepare_runtime_ohos.py 泛化：路径全走 CLI 参数 + 游戏档案，去硬编码
（nxnative-ohos 设计 §4.3）。档案（games/<titleID>/game.tsv）只消费两键：
  modules   → runtime_tables.h 的模块表（rtld 由宿主替换，不列）
  title_id  → RomFS.img 自动搜索兜底（<work> 同级 *<titleID>/RomFS.img）

输入：<work>/patched/<模块>/（patched.bin + fixups.tsv + gates.tsv）
输出：<out>/{runtime_tables.h, romfs_config.h, shared_fonts_config.h}

与 Mac 原版 prepare_runtime.py 的差异（沿袭 game-work 版）：跳过 expected 哈希校验
（nxnative 重写器产物，M2 恢复该纪律）、接受 spin 类 gate、共享字体守卫——
shared_fonts_config.h 已存在（make_shared_fonts.py 内嵌了伪造字体）则不动。
"""
import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path


def parse_game_tsv(path: Path) -> dict:
    """game.tsv：每行 key<TAB>value，# 开头注释行。返回单值键表（build/shader_groups 等原样字符串）。"""
    keys = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) >= 2:
            keys[parts[0]] = parts[1]
    return keys


def find_d2r(override: str) -> Path:
    """relr.py 所在 d2r 目录：--d2r > $D2R_PATH > 仓库内 infra/MacNativeGames/games/by3/third_party/d2r。"""
    cands = []
    if override:
        cands.append(Path(override))
    if os.environ.get("D2R_PATH"):
        cands.append(Path(os.environ["D2R_PATH"]))
    cands.append(Path(__file__).resolve().parents[2] / "infra" / "MacNativeGames" / "games" / "by3" / "third_party" / "d2r")
    for c in cands:
        if (c / "relr.py").is_file():
            return c
    sys.exit(f"prepare_runtime: relr.py not found (tried: {[str(c) for c in cands]}); pass --d2r <dir>")


def find_romfs(explicit: str, work: Path, title_id: str) -> Path:
    """RomFS.img：--romfs > <work> 同级的 *<titleID>/RomFS.img（game-work 抽取目录命名 <短名>_<titleID>）。"""
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        sys.exit(f"prepare_runtime: --romfs {explicit} not found")
    for p in sorted(work.parent.glob(f"*{title_id}/RomFS.img")):
        return p
    sys.exit(f"prepare_runtime: RomFS.img not found next to {work} (pattern *{title_id}/RomFS.img); pass --romfs")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


class Runtime:  # ---- 以下逻辑与 game-work/prepare_runtime_ohos.py 逐行同源（只参数化路径/模块表） ----
    def __init__(self, patched: Path, out: Path, d2r: Path):
        self.PATCHED = patched
        self.OUT = out
        sys.path.insert(0, str(d2r))
        global decode_relr
        from relr import decode_relr  # noqa: F401  (infra/MacNativeGames …/d2r/relr.py)
        self.OUT.mkdir(parents=True, exist_ok=True)

    def layout_of(self, name):
        lay = {}
        for line in (self.PATCHED / name / f"{name}.fixups.tsv").read_text().splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1].isupper():
                try:
                    lay[parts[1]] = int(parts[2], 0)
                except ValueError:
                    pass  # BUILD_ID 等非数值行
        need = ["PATCH_OFF", "KEY_PAGE_OFF", "TEXT_END", "RO_OFF", "DATA_OFF"]
        missing = [k for k in need if k not in lay]
        assert not missing, f"{name} fixups missing {missing}: {list(lay)}"
        return lay

    def module_table(self, name):
        path = self.PATCHED / name / f"{name}.patched.bin"
        image = path.read_bytes()
        digest = hashlib.sha256(image).hexdigest()
        layout = self.layout_of(name)
        mod0 = struct.unpack_from("<I", image, 4)[0]
        assert image[mod0:mod0 + 4] == b"MOD0"
        dyn = mod0 + struct.unpack_from("<i", image, mod0 + 4)[0]
        tags = {}
        for off in range(dyn, dyn + 4096 * 16, 16):
            tag, value = struct.unpack_from("<qQ", image, off)
            if tag == 0:
                break
            tags[tag] = value
        else:
            raise ValueError(f"{name}: unterminated dynamic table")
        assert tags[11] == 24 and tags[9] == 24 and tags[20] == 7, f"{name}: dynamic layout {tags}"
        nsym = struct.unpack_from("<I", image, tags[4] + 4)[0] if 4 in tags else 0
        symbols = []
        for i in range(nsym):
            no, info, other, section, value, size = struct.unpack_from("<IBBHQQ", image, tags[6] + 24 * i)
            end = image.index(0, tags[5] + no, tags[5] + tags[10])
            symbols.append(dict(name=image[tags[5] + no:end].decode(), value=value, size=size,
                                defined=bool(section), binding=info >> 4, type=info & 15))
        relocs = [dict(where=w, kind=1027, symbol=0, addend=a)
                  for w, a in decode_relr(image, tags, layout["DATA_OFF"], layout["KEY_PAGE_OFF"])]
        relocated = {r["where"] for r in relocs}
        for address, size in [(tags[7], tags[8]), (tags[23], tags[2])]:
            if not size:
                continue
            assert size % 24 == 0
            for off in range(address, address + size, 24):
                where, info, addend = struct.unpack_from("<QQq", image, off)
                kind, si = info & 0xFFFFFFFF, info >> 32
                assert kind in (257, 1025, 1026, 1027) and si < nsym, f"{name}: reloc kind {kind} at {where:#x}"
                assert layout["DATA_OFF"] <= where <= layout["KEY_PAGE_OFF"] - 8
                assert where not in relocated, f"{name}: duplicate RELR/RELA at {where:#x}"
                relocated.add(where)
                relocs.append(dict(where=where, kind=kind, symbol=si, addend=addend))
        gates = []
        for line in (self.PATCHED / name / f"{name}.gates.tsv").read_text().splitlines():
            ret, kind, arg, original = line.split()
            assert kind in ("cntpct", "cntvct", "svc", "spin"), f"{name}: gate kind {kind}"
            gates.append(dict(ret=int(ret, 0), timer=kind != "svc", arg=int(arg), original=int(original, 0)))
        # ctor state word：DT_INIT 里 adrp+ldr 对（照抄原版逻辑）
        init = tags[12]
        words = [struct.unpack_from("<I", image, init + 4 * i)[0] for i in range(16)]
        ctor_flag = 0
        for i, a in enumerate(words):
            if a & 0x9F000000 != 0x90000000:
                continue
            reg = a & 31
            for j in range(i + 1, len(words)):
                w = words[j]
                if w & 0xFFC00000 == 0xB9400000 and (w >> 5) & 31 == reg:
                    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7FFFF) << 2)
                    imm = (imm ^ (1 << 20)) - (1 << 20)
                    ctor_flag = ((init + 4 * i) & ~4095) + (imm << 12) + ((w >> 10) & 0xFFF) * 4
                    break
            if ctor_flag:
                break
        assert ctor_flag, f"{name}: no constructor-state load in DT_INIT"
        return dict(name=name, sha256=digest, size=len(image), layout=layout, tags=tags, symbols=symbols,
                    relocs=relocs, gates=gates, ctor_flag=ctor_flag)

    def runtime_tables(self, module_names):
        modules = [self.module_table(n) for n in module_names]
        main = modules[0]
        target = "_ZN2nn2os13GetSystemTickEv"
        sym_index = {s["name"]: i for i, s in enumerate(main["symbols"])}
        slot = next(r["where"] for r in main["relocs"] if r["kind"] == 1026 and r["symbol"] == sym_index[target])
        image = (self.PATCHED / module_names[0] / f"{module_names[0]}.patched.bin").read_bytes()
        plt = []
        for address in range(main["layout"]["TEXT_END"] - 0x10000, main["layout"]["TEXT_END"] - 12, 4):
            a, b, c, d = struct.unpack_from("<IIII", image, address)
            if a & 0x9F00001F != 0x90000010 or b & 0xFFC003FF != 0xF9400211 or d != 0xD61F0220:
                continue
            imm = ((a >> 29) & 3) | (((a >> 5) & 0x7FFFF) << 2)
            imm = (imm ^ (1 << 20)) - (1 << 20)
            if (address & ~4095) + (imm << 12) + ((b >> 10) & 0xFFF) * 8 == slot:
                plt.append(address)
        assert len(plt) == 1, f"PLT matches {plt}"
        lines = ["/* Generated by nxnative-ohos/tools/prepare_runtime.py from nxnative rewrite products. */"]
        for n, m in enumerate(modules):
            lines.append(f"static const Reloc reloc_{n}[] = {{")
            lines.extend(f"{{{r['where']}ULL,{r['kind']},{r['symbol']},{r['addend']}LL}}," for r in m["relocs"])
            lines.append("};")
            lines.append(f"static const Symbol symbol_{n}[] = {{")
            lines.extend("{" + f"{json.dumps(s['name'])},{s['value']}ULL,{s['size']}ULL,{int(s['defined'])},{s['binding']},{s['type']}" + "}," for s in m["symbols"])
            lines.append("};")
            lines.append(f"static const Gate gate_{n}[] = {{")
            lines.extend(f"{{{g['ret']}ULL,{g['original']}ULL,{int(g['timer'])},{g['arg']}}}," for g in m["gates"])
            if not m["gates"]:
                lines.append("{0,0,0,0},")
            lines.append("};")
        lines.append("static Module modules[] = {")
        for n, m in enumerate(modules):
            l = m["layout"]
            lines.append("{" + f".name={json.dumps(m['name'])},.digest={json.dumps(m['sha256'])},.file_size={m['size']}ULL,.text_end={l['TEXT_END']}ULL,.ro_off={l['RO_OFF']}ULL,.data_off={l['DATA_OFF']}ULL,.patch_off={l['PATCH_OFF']}ULL,.key_off={l['KEY_PAGE_OFF']}ULL,.symbols=symbol_{n},.nsym={len(m['symbols'])},.relocs=reloc_{n},.nreloc={len(m['relocs'])},.gates=gate_{n},.ngate={len(m['gates'])},.init={m['tags'].get(12,0)}ULL,.ctor_flag={m['ctor_flag']}ULL,.init_array={m['tags'].get(25,0)}ULL,.init_size={m['tags'].get(27,0)}ULL" + "},")
        lines.append("};")
        lines.append(f"#define PROBE_PLT {plt[0]}ULL\n#define PROBE_SLOT {slot}ULL")
        (self.OUT / "runtime_tables.h").write_text("\n".join(lines) + "\n")
        return dict(modules=len(modules), relocations=sum(len(m["relocs"]) for m in modules))

    def romfs_config(self, img: Path, index_hint: Path | None):
        size = img.stat().st_size
        image_sha = sha256_file(img)
        # runtime-index.bin：make_romfs_runtime 的分卷输出区（game-work 布局在 <work> 同级 romfs_parts/），
        # 找到才算真 INDEX SHA，找不到记 "0"（运行时侧 romfs 校验会失败，应尽早补分卷区）
        index_sha = "0"
        cands = ([index_hint] if index_hint else
                 [self.PATCHED.parent.parent / "romfs_parts" / "runtime-index.bin",
                  self.PATCHED.parent / "romfs_parts" / "runtime-index.bin",
                  self.PATCHED.parent / "runtime-index.bin",
                  img.parent / "runtime-index.bin"])
        for c in cands:
            if c and c.is_file():
                index_sha = sha256_file(c)
                break
        (self.OUT / "romfs_config.h").write_text(
            f"#define ROMFS_IMAGE_SIZE {size}ULL\n"
            f'#define ROMFS_IMAGE_SHA256 "{image_sha}"\n'
            f'#define ROMFS_INDEX_SHA256 "{index_sha}"\n')
        return dict(size=size, image_sha=image_sha, index_sha=index_sha)

    def shared_fonts_config(self):
        out = self.OUT / "shared_fonts_config.h"
        if out.exists():  # make_shared_fonts.py 内嵌了伪造字体，别踩掉（守卫逻辑沿袭 game-work 版）
            return "kept"
        rows = ['{"",0ULL,0ULL,""}'] * 6
        out.write_text(
            "#define SHARED_FONT_COUNT 6\n#define SHARED_FONT_ROWS " + ",".join(rows) + "\n")
        return "empty"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--archive", required=True, help="游戏档案目录 games/<titleID>/（含 game.tsv）")
    ap.add_argument("--work", required=True, help="<titleID>_build 工作区根（含 patched/<模块>/）")
    ap.add_argument("--out", required=True, help="生成头输出目录（实例化工程的 entry/src/main/cpp/generated）")
    ap.add_argument("--romfs", help="RomFS.img 路径（缺省在 work 同级按 *<titleID>/RomFS.img 搜索）")
    ap.add_argument("--romfs-index", help="runtime-index.bin 路径（缺省在 work 区搜索，找不到 INDEX SHA 记 \"0\"）")
    ap.add_argument("--d2r", help="d2r 目录（含 relr.py；缺省 $D2R_PATH 或仓库 infra/MacNativeGames/…/d2r）")
    args = ap.parse_args()

    archive = Path(args.archive).resolve()
    work = Path(args.work).resolve()
    out = Path(args.out).resolve()
    tsv = parse_game_tsv(archive / "game.tsv")
    for need in ("title_id", "modules"):
        if need not in tsv:
            sys.exit(f"prepare_runtime: game.tsv missing key {need}")
    modules = tsv["modules"].split()          # 加载序（rtld 不列，宿主替换）
    img = find_romfs(args.romfs, work, tsv["title_id"])
    rt = Runtime(work / "patched", out, find_d2r(args.d2r))
    print("runtime_tables:", rt.runtime_tables(modules))
    print("romfs:", rt.romfs_config(img, Path(args.romfs_index) if args.romfs_index else None))
    print("shared_fonts:", rt.shared_fonts_config())


if __name__ == "__main__":
    main()
