#!/usr/bin/env python3
"""build_hap_from_profile.py —— M1 一键：游戏档案 + 既有 patched 产物 → 签名 HAP。

M1 范围（nxnative-ohos 设计 §3 七步流的 4→7 子集）：不做 NSP 提取/重写（--work 指向
既有 <titleID>_build 工作区），从 template/ 实例化参数化工程并出包。

流程：
  1 profile      读 games/<titleID>/game.tsv（键表：../games/README.md）
  2 instantiate  template/ → 构建区：{{...}} 占位替换 + game_patches.tmpl.c 启用 + host_override 覆盖
                 + rawfile 镜像硬链（patched.bin 不入库，从 work 区链入）+ game_version.h 生成兜底
                 + shared_fonts_{config,data}.h 拷入（fonts_mode=embedded，M1 用现成内嵌字体对）
  3 prepare      tools/prepare_runtime.py → generated/runtime_tables.h + romfs_config.h
                 （shared_fonts_config.h 守卫：字体头已存在则不动）
  4 build        hvigorw --mode module -p product=default assembleHap（命令复用 game-work/run_cycle.sh）
  5 report       HAP 路径 + 字节数

--progress：每阶段一行 JSON 事件（{"event":"stage"|"step"|...}，对齐设计 §3 / docs/frontends.md）。
用法（M1 验收）：
  python scripts/build_hap_from_profile.py games/0100000000010000 --work ../game-work/smo_build --progress
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]          # switch2hos/
NXN = REPO / "nxnative-ohos"

# DevEco 工具链（与 game-work/run_cycle.sh 同源；--node/--hvigorw/--sdk 可覆盖）。
# 注意：不读环境 DEVECO_SDK_HOME——本机该 env 指向 D: 盘 command-line-tools 的残缺 SDK，
# run_cycle.sh 的做法就是无条件 export Studio 自带 SDK 路径（PITFALLS 同源）。
NODE = os.environ.get("DEVECO_NODE", "C:/Program Files/Huawei/DevEco Studio/tools/node/node.exe")
HVIGORW = os.environ.get("DEVECO_HVIGORW", "C:/Program Files/Huawei/DevEco Studio/tools/hvigor/bin/hvigorw.js")
DEVECO_SDK_HOME = "C:/Program Files/Huawei/DevEco Studio/sdk"

PROGRESS = False


def ev(kind, **kw):
    """--progress json 事件：一行一个。"""
    print(json.dumps({"event": kind, "ts": round(time.time(), 3), **kw}, ensure_ascii=False), flush=True)


def log(stage, step, detail):
    if PROGRESS:
        ev("step", stage=stage, step=step, detail=detail)
    else:
        print(f"[{stage}] {step}: {detail}")


def stage(name, status):
    if PROGRESS:
        ev("stage", stage=name, status=status)


def parse_game_tsv(path: Path) -> dict:
    keys = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) >= 2:
            keys[parts[0]] = parts[1]
    return keys


def link_or_copy(src: Path, dst: Path):
    """大文件优先硬链（同盘零拷贝），失败退拷贝。红线：大文件不进 nxnative-ohos 档案/模板，只进构建区。"""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.unlink()
    try:
        dst.hardlink_to(src)
        return "link"
    except OSError:
        shutil.copy2(src, dst)
        return "copy"


def rmtree_retry(path: Path, tries: int = 5):
    """Windows：hvigor daemon/杀毒/索引器可能短暂握住构建区句柄或延迟删目录
    （WinError 32/145），重试兜底；仍失败则改名隔离后尽力删。"""
    for i in range(tries):
        try:
            shutil.rmtree(path)
            return
        except OSError:
            if i == tries - 1:
                alt = path.with_name(f"{path.name}.stale-{int(time.time())}")
                try:
                    path.rename(alt)
                    shutil.rmtree(alt, ignore_errors=True)
                except OSError:
                    pass
                shutil.rmtree(path, ignore_errors=True)
                return
            time.sleep(2)


def substitute(text: str, subs: dict) -> str:
    out = text
    for k, v in subs.items():
        out = out.replace("{{" + k + "}}", str(v))
    left = re.findall(r"\{\{[A-Z0-9_]+\}\}", out)
    if left:
        sys.exit(f"build_hap: unresolved placeholders {sorted(set(left))}")
    return out


def main():
    global PROGRESS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive", help="档案目录 games/<titleID>/（或其 game.tsv 路径）")
    ap.add_argument("--work", required=True, help="既有 <titleID>_build 工作区根（含 patched/<模块>/）")
    ap.add_argument("--out", help="构建区（实例化工程根），缺省 nxnative-ohos/.build/<titleID>/")
    ap.add_argument("--romfs", help="RomFS.img 路径（缺省 work 同级 *<titleID>/RomFS.img 搜索）")
    ap.add_argument("--fonts-src", help="现成 shared_fonts_{config,data}.h 所在目录（embedded 模式；"
                                        "缺省生产线 mario-odessy-harmony/…/generated，只读）")
    ap.add_argument("--node", default=NODE)
    ap.add_argument("--hvigorw", default=HVIGORW)
    ap.add_argument("--sdk", default=DEVECO_SDK_HOME)
    ap.add_argument("--keep", action="store_true", help="构建区已存在时复用（默认整体重建）")
    ap.add_argument("--progress", action="store_true", help="每阶段一行 JSON 事件")
    args = ap.parse_args()
    PROGRESS = args.progress

    archive = Path(args.archive).resolve()
    if archive.name == "game.tsv":
        archive = archive.parent
    work = Path(args.work).resolve()
    t0 = time.time()

    # ---- 1 profile：读档案 ----
    stage("profile", "start")
    tsv = parse_game_tsv(archive / "game.tsv")
    for need in ("title_id", "program_id", "name", "modules", "hap_bundle", "hap_label",
                 "romfs_volume_mib", "fonts_mode", "nvn_backend", "package"):
        if need not in tsv:
            sys.exit(f"build_hap: game.tsv missing key {need}")
    title_id = tsv["title_id"]
    if archive.name != title_id:
        sys.exit(f"build_hap: 档案目录名 {archive.name} != title_id {title_id}（nxnative 铁律：目录名即 title ID）")
    package = tsv["package"]
    display = next((ln.split("\t")[2] for ln in (archive / "game.tsv").read_text(encoding="utf-8").splitlines()
                    if ln.startswith("build\t") and ln.split("\t")[1] == package), "1.0.0")
    modules = tsv["modules"].split()                       # 加载序 main subsdk0 sdk
    rawfile_modules = ["rtld"] + modules[::-1]             # rawfile 分发序：rtld + 逆加载序（smo: rtld sdk subsdk0 main）
    # hilog 标签：hap_bundle 尾段 PascalCase + "Game"（com.vibecc.smo → SmoGame，兼容 run_cycle.sh 验尸 grep）
    short = tsv["hap_bundle"].rsplit(".", 1)[-1]
    log_tag = short[0].upper() + short[1:] + "Game"
    log("profile", "game.tsv", f"{title_id} {tsv['name']} {display} tag={log_tag}")

    # RomFS.img：分卷数由实际镜像大小算
    romfs_img = Path(args.romfs) if args.romfs else None
    if romfs_img is None:
        for p in sorted(work.parent.glob(f"*{title_id}/RomFS.img")):
            romfs_img = p
            break
    if not romfs_img or not romfs_img.is_file():
        sys.exit(f"build_hap: RomFS.img not found near {work} (or --romfs)")
    vol_mib = int(tsv["romfs_volume_mib"])
    romfs_volumes = max(1, math.ceil(romfs_img.stat().st_size / (vol_mib * 1024 * 1024)))
    out_dir = Path(args.out).resolve() if args.out else NXN / ".build" / title_id
    log("profile", "romfs", f"{romfs_img.name} {romfs_img.stat().st_size:,}B → {romfs_volumes} 卷 × {vol_mib}MiB")
    stage("profile", "done")

    # ---- 2 instantiate：模板 → 构建区 ----
    stage("instantiate", "start")
    # romfs_parts 是本目录里唯一的大产物（tools/make_romfs_runtime.py 输出，16GB 级）：
    # 整体重建时把它改名隔离、清完再放回，避免重跑一次构建就重写十几 GB 分卷
    parts = out_dir / "romfs_parts"
    parts_aside = None
    if out_dir.exists() and not args.keep and parts.is_dir():
        parts_aside = out_dir.with_name(out_dir.name + ".romfs_parts-aside")
        rmtree_retry(parts_aside)
        parts.rename(parts_aside)
    if out_dir.exists() and not args.keep:
        rmtree_retry(out_dir)
    if parts_aside:
        parts.parent.mkdir(parents=True, exist_ok=True)   # rmtree 已删 out_dir，先立父目录再放回
        parts_aside.rename(parts)
    shutil.copytree(NXN / "template", out_dir, dirs_exist_ok=True)
    subs = {
        "GAME_ID_DIR": title_id,
        "GAME_TITLE_ID": "0x" + tsv["program_id"],
        "GAME_LOG_TAG": log_tag,
        "RAWFILE_MODULES": ", ".join(f"'{m}'" for m in rawfile_modules),
        "ROMFS_VOLUMES": romfs_volumes,
        "HAP_BUNDLE": tsv["hap_bundle"],
        "HAP_LABEL": tsv["hap_label"],
        "HAP_VERSION": display,
    }
    # 占位替换白名单（不碰 third_party 大树）
    (out_dir / "AppScope" / "app.json5.tmpl").rename(out_dir / "AppScope" / "app.json5")
    for rel in ("AppScope/app.json5",
                "entry/src/main/cpp/game_id.h",
                "entry/src/main/ets/entryability/EntryAbility.ets",
                "entry/src/main/ets/pages/Index.ets",
                "entry/src/main/resources/base/element/string.json",
                "AppScope/resources/base/element/string.json"):
        p = out_dir / rel
        p.write_text(substitute(p.read_text(encoding="utf-8"), subs), encoding="utf-8")
    log("instantiate", "placeholders", f"{len(subs)} vars, romfs volumes={romfs_volumes}")
    # 签名配置与 bundle 绑定：模板 build-profile.json5 的调试签名材料（DevEco auto-sign，p7b 内嵌
    # bundle-name）只对本机生成它的那个 bundle 有效。档案 hap_bundle 与 p7b 不符时剥离 signingConfigs，
    # 出未签名 HAP（装机需在 DevEco Project Structure → Signing Configs 点一次 Fix 生成新材料后重跑）。
    bp = out_dir / "build-profile.json5"
    bp_text = bp.read_text(encoding="utf-8")
    m = re.search(r'"profile"\s*:\s*"([^"]+\.p7b)"', bp_text)
    p7b_bundle = None
    if m and Path(m.group(1)).is_file():
        p7b_bundles = set(re.findall(rb"com\.[a-z0-9_.]+", Path(m.group(1)).read_bytes()))
        p7b_bundle = next((b.decode() for b in p7b_bundles if b.decode().count(".") >= 2), None)
    signed_ok = p7b_bundle == tsv["hap_bundle"]
    if not signed_ok:
        bp_text = re.sub(r'"signingConfigs"\s*:\s*\[.*?\]\s*,\s*\n\s*"products"',
                         '"signingConfigs": [],\n    "products"', bp_text, flags=re.S)
        bp_text = re.sub(r',?\s*"signingConfig"\s*:\s*"default"', "", bp_text)
        bp.write_text(bp_text, encoding="utf-8")
        log("instantiate", "signing", f"p7b 绑定 {p7b_bundle} != {tsv['hap_bundle']}：剥离签名配置，出未签名 HAP"
                                       f"（装机前需 DevEco Signing Configs → Fix 再重跑）")
    # 游戏钩子：模板空实现启用；档案 host_override 整文件覆盖（逃生门，设计 §7）
    (out_dir / "entry/src/main/cpp/host/game_patches.tmpl.c").rename(
        out_dir / "entry/src/main/cpp/host/game_patches.c")
    override = archive / "host_override"
    n_over = 0
    if override.is_dir():
        for f in override.rglob("*"):
            if f.is_file():
                dst = out_dir / f.relative_to(override)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, dst)
                n_over += 1
    log("instantiate", "host_override", f"{n_over} files overridden")
    # game_version.h 兜底：host_override 没带就按档案生成（GAME_ODYSSEY 类分叉宏由 override 表达）
    gv = out_dir / "entry/src/main/cpp/host/game_version.h"
    if not gv.exists():
        gv.write_text(f'/* Generated by build_hap_from_profile.py from {archive.name}/game.tsv. */\n'
                      f'#pragma once\n#define TOTK_GAME_VERSION "{display}"\n'
                      f'#define TOTK_GAME_DIR "{title_id}"\n#define GAME_TITLE_ID 0x{tsv["program_id"]}ULL\n',
                      encoding="utf-8")
        log("instantiate", "game_version.h", "generated from profile")
    # rawfile 游戏镜像：从 work 区硬链（不入库红线）
    raw_dir = out_dir / "entry/src/main/resources/rawfile" / title_id
    raw_dir.mkdir(parents=True, exist_ok=True)
    for m in rawfile_modules:
        src = work / "patched" / m / f"{m}.patched.bin"
        if not src.is_file():
            sys.exit(f"build_hap: {src} not found（--work 需指向既有 patched 产物）")
        link_or_copy(src, raw_dir / f"{m}.patched.bin")
    log("instantiate", "rawfile", f"{len(rawfile_modules)} patched.bin linked from {work}")
    # 内嵌字体对（fonts_mode=embedded）：先拷入，prepare 的守卫就不会踩掉
    if tsv["fonts_mode"] != "embedded":
        sys.exit(f"build_hap: fonts_mode={tsv['fonts_mode']} 未支持（M1 仅 embedded）")
    fonts_src = Path(args.fonts_src).resolve() if args.fonts_src else \
        REPO / "mario-odessy-harmony" / "entry" / "src" / "main" / "cpp" / "generated"
    gen = out_dir / "entry" / "src" / "main" / "cpp" / "generated"
    gen.mkdir(parents=True, exist_ok=True)
    for h in ("shared_fonts_config.h", "shared_fonts_data.h"):
        if not (fonts_src / h).is_file():
            sys.exit(f"build_hap: {h} not found in {fonts_src}（embedded 模式需现成内嵌字体对，--fonts-src 指定）")
        shutil.copy2(fonts_src / h, gen / h)
    log("instantiate", "shared_fonts", f"copied pair from {fonts_src}")
    stage("instantiate", "done")

    # ---- 3 prepare：生成头（runtime_tables + romfs_config） ----
    stage("prepare", "start")
    # 构建区若有 make_romfs_runtime 的分卷产物，把 runtime-index.bin 喂给 prepare（真 INDEX SHA）；
    # 没有则 prepare 只记 "0"（运行时 romfs 校验会失败——先跑 tools/make_romfs_runtime.py）
    romfs_index = out_dir / "romfs_parts" / "runtime-index.bin"
    prep_argv = [sys.executable, str(NXN / "tools" / "prepare_runtime.py"),
                 "--archive", str(archive), "--work", str(work), "--out", str(gen),
                 "--romfs", str(romfs_img)]
    if romfs_index.is_file():
        prep_argv += ["--romfs-index", str(romfs_index)]
    prep = subprocess.run(prep_argv, capture_output=True, text=True)
    if prep.returncode != 0:
        sys.exit("build_hap: prepare_runtime failed\n" + prep.stdout + prep.stderr)
    log("prepare", "prepare_runtime", prep.stdout.strip().replace("\n", " | "))
    stage("prepare", "done")

    # ---- 4 build：hvigor ----
    stage("build", "start")
    env = dict(os.environ, DEVECO_SDK_HOME=args.sdk)
    build_log = out_dir / "build-log.txt"
    with build_log.open("w", encoding="utf-8", errors="replace") as lf:
        # --no-daemon：本脚本每次重建构建区，常驻 daemon 会握住已删目录的句柄（WinError 32）。
        # --no-incremental：构建区虽每次重建，hvigor 全局 project_caches 仍会把旧任务的产物原样搬回来
        #（实测 totk 构建把 smo 时代的 icon 中间件从缓存恢复进 HAP）——关掉增量保证档案输入即产物。
        rc = subprocess.run([args.node, args.hvigorw, "--no-daemon", "--no-incremental", "--mode", "module",
                             "-p", "product=default", "assembleHap"],
                            cwd=out_dir, env=env, stdout=lf, stderr=subprocess.STDOUT).returncode
    hap_signed = out_dir / "entry/build/default/outputs/default/entry-default-signed.hap"
    hap = hap_signed if hap_signed.is_file() else out_dir / "entry/build/default/outputs/default/entry-default-unsigned.hap"
    text = build_log.read_text(encoding="utf-8", errors="replace")
    if rc != 0 or "BUILD SUCCESSFUL" not in text or not hap.is_file():
        tail = "\n".join(text.splitlines()[-25:])
        sys.exit(f"build_hap: hvigor FAILED (rc={rc})，尾部日志：\n{tail}\n完整日志：{build_log}")
    log("build", "hvigor", f"BUILD SUCCESSFUL ({'signed' if hap.name.endswith('-signed.hap') else 'UNSIGNED'})")
    stage("build", "done")

    # ---- 5 report ----
    stage("report", "start")
    size = hap.stat().st_size
    log("report", "hap", f"{hap} ({size:,} bytes)")
    stage("report", "done")
    if PROGRESS:
        ev("result", hap=str(hap), bytes=size, secs=round(time.time() - t0, 1))
    print(f"HAP: {hap}")
    print(f"BYTES: {size:,}")


if __name__ == "__main__":
    main()
