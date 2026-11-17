#!/usr/bin/env python3
"""new_game.py —— 从 NSP 起草新游戏档案（M2：survey → games/<titleID>/ 草稿）。

做什么：
  1 扫 NSP（复用 build_hap_from_nsp 的识别逻辑）→ titleID / 本体 / 最新更新
  2 nx_extract --manifest 轻量提取（ExeFS + RomFS 清单，不写游戏文件）
  3 读 control.nacp（名称/显示版本）+ 逐模块 build ID + RomFS 规模统计
  4 落草稿：games/<titleID>/{game.tsv, patches.py}
     - build 行自动填识别到的模块 build ID；内存/图形/字体等键留 TODO 由人补
     - 已有档案目录则拒绝（--force 覆盖），不误伤既有档案

用法：
  python scripts/new_game.py "<NSP 文件夹>" -k E:/Yuzu/user/keys/prod.keys [--update <upd.nsp>]
"""
import argparse
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_hap_from_nsp import (NXN, DEFAULT_KEYS, DM_TOOLS, exefs_modules, build_id,
                                scan_nsps, run_logged, ensure_nx_extract)  # noqa: E402

ROMFS_TOPS = 12                                          # survey 里列出的顶层目录数


def draft_game_tsv(tid, name, disp, ver, mods, bid, marker, vol_gib):
    """档案草稿：自动值尽量填实，人工键 TODO（键面见 ../games/README.md）。"""
    modules = [m for m in mods if m != "rtld"]           # modules 不含 rtld（宿主替换）
    load = " ".join(modules)
    short = "g" + tid[-6:].lower()
    L = [f"# {name}（{tid}，display {disp}）—— new_game.py 起草草稿，内存/图形键人工补齐后生效。",
         "# 键表权威：../games/README.md；参考既有档案 ../0100000000010000/game.tsv（smo）。",
         "",
         "# ---- 身份 ----",
         f"title_id\t{tid}",
         f"program_id\t{tid}\t# TODO 人工确认（本体 main.npdm 里的 program ID，多与 title_id 相同，需核对）",
         "",
         "# ---- 应用（鸿蒙组）----",
         f"name\t{name}",
         f"hap_bundle\tcom.vibecc.{short}\t# TODO 人工确认",
         f"hap_label\t{name}\t# TODO 人工确认（中文应用名）",
         "hap_icon\t$media:icon",
         "",
         "# ---- 版本 ----",
         f"build\t{ver}\t{disp}\t" + "\t".join(f"{m}={bid[m]}" for m in mods),
         f"package\t{ver}",
         f"shader_versions\t{ver}",
         "",
         "# ---- 模块 ----",
         f"modules\t{load}",
         f"romfs_marker\t{marker}",
         "",
         "# ---- 内存（TODO：对照宿主 services.c 与 nxnative 同档案键；smo 参考值见注释）----",
         "mem_reserve\t0x100000000\t# TODO（smo 参考值）",
         "mem_alias\t0x10000000\t# TODO（smo 参考值 256MiB）",
         "mem_stack\t0x10000000\t# TODO（smo 参考值）",
         "app_pool\t0xCD500000\t# TODO（smo 参考值）",
         "",
         "# ---- 着色器拆分（TODO：按 RomFS 顶层目录人工分组；smo 样例见其档案）----",
         "shader_groups\t/=romfs\t# TODO",
         f"shader_note\t{name} shaders",
         "",
         "# ---- 鸿蒙组 ----",
         f"romfs_volume_mib\t1024\t# RomFS {vol_gib:.2f} GiB → {max(1, int(vol_gib / 1.024) + 1)} 卷 × 1024MiB（四舍五入可调）",
         "fonts_mode\tembedded\t# TODO：embedded（Windows TTF 伪造）/ firmware（真 BFTTF，需固件提取）",
         "nvn_backend\tvulkan",
         "",
    ]
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nsp", help="NSP 文件夹（一个游戏）或本体 NSP 文件路径")
    ap.add_argument("-k", "--keys", default=DEFAULT_KEYS, help=f"prod.keys（缺省 {DEFAULT_KEYS}）")
    ap.add_argument("--update", help="更新 NSP 显式路径（缺省取文件夹内版本号最高者）")
    ap.add_argument("--out", default=str(NXN / "games"), help="档案根目录（缺省 nxnative-ohos/games）")
    ap.add_argument("--force", action="store_true", help="档案目录已存在时覆盖重建")
    args = ap.parse_args()

    keys = Path(args.keys)
    if not keys.is_file():
        sys.exit(f"prod.keys 不存在：{keys}")

    app, base, upd = scan_nsps(Path(args.nsp).resolve(), Path(args.update).resolve() if args.update else None)
    tid = f"{app:016X}"
    out_root = Path(args.out).resolve()
    archive = out_root / tid
    if (archive / "game.tsv").is_file() and not args.force:
        sys.exit(f"档案已存在：{archive}（--force 覆盖重建）")
    work = NXN / ".build" / f"{tid}_nsp"
    logs = work / "logs"
    logs.mkdir(parents=True, exist_ok=True)

    # manifest 模式轻量提取（已做过则沿用）
    ex = work / "survey_extract"
    if args.force or not (ex / "ExeFS" / "main").is_file():
        nx_exe = ensure_nx_extract(work, logs)
        tmp = work / "survey_extract.partial"
        shutil.rmtree(tmp, ignore_errors=True)
        print(f"[survey] nx_extract --manifest → {ex}")
        rc = run_logged([nx_exe, keys, base, upd or "-", tmp, "--manifest"], logs / "survey_extract.log")
        if rc != 0:
            sys.exit(f"nx_extract 失败 rc={rc}，见 {logs / 'survey_extract.log'}")
        shutil.rmtree(ex, ignore_errors=True)
        tmp.rename(ex)
    else:
        print(f"[survey] 沿用 {ex}")

    # 事实收集
    sys.path.insert(0, str(DM_TOOLS))
    import nacp as nacp_mod
    nc = ex / "control.nacp"
    name = nacp_mod.name(nc) if nc.is_file() else tid
    disp = nacp_mod.version(nc) if nc.is_file() else "?"
    m = re.search(r"\[v(\d+)\]", upd.name) if upd else None
    ver = f"v{m.group(1)}" if m else "v0"
    mods = exefs_modules(ex / "ExeFS")
    bid = {m: build_id(ex / "ExeFS" / m) for m in mods}

    rom_n, rom_b, biggest, tops = 0, 0, (0, ""), {}
    man = ex / "romfs_manifest.tsv"
    if man.is_file():
        for line in man.read_text(encoding="utf-8", errors="replace").splitlines():
            c = line.split("\t")
            if len(c) < 2 or not c[1].isdigit():
                continue
            sz = int(c[1])
            rom_n += 1
            rom_b += sz
            top = c[0].strip("/").split("/", 1)[0]
            tops[top] = tops.get(top, [0, 0])
            tops[top][0] += 1
            tops[top][1] += sz
            if sz > biggest[0]:
                biggest = (sz, c[0])
    vol_gib = rom_b / 2**30
    marker = biggest[1] or "TODO"

    # 落草稿
    if args.force:
        shutil.rmtree(archive, ignore_errors=True)
    archive.mkdir(parents=True, exist_ok=True)
    (archive / "game.tsv").write_text(
        draft_game_tsv(tid, name, disp, ver, mods, bid, marker, vol_gib),
        encoding="utf-8")
    (archive / "patches.py").write_text(
        f"# patches.py —— {name}（{ver} / {disp}）per-build 指令补丁（new_game.py 起草）。\n"
        "# 格式对齐 nxnative：按 NSO build ID 键控的 CUTS / OBS / FRAME_STEP；一个 build 被两个档案声明即重写报错。\n"
        "# sdk 模块公共裁剪走 dm/tools/sdk_patches.py（nxnative 语义），此处只写本游戏自有模块。\n"
        "# 本档案各模块 build ID：\n" + "".join(f"#   {m} = {bid[m]}\n" for m in mods), encoding="utf-8")

    print(f"[draft] {archive / 'game.tsv'}")
    print(f"[draft] {archive / 'patches.py'}")
    print(f"[facts] 模块 {len(mods)}：{' '.join(mods)}；RomFS {rom_n:,} 文件 / {vol_gib:.2f} GiB；最大文件 {marker}")
    print("[next ] 人工补：program_id / 内存四键 / shader_groups / hap_bundle&label / fonts_mode，"
          "然后 build_hap_from_nsp.py 走七步流")


if __name__ == "__main__":
    main()
