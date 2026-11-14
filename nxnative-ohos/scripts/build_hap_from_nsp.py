#!/usr/bin/env python3
"""build_hap_from_nsp.py —— M2 总入口：NSP + prod.keys → 签名 HAP（纯 PC 侧，绝不碰设备/hdc）。

七步流（nxnative-ohos 设计 §3，对齐 nxnative scripts/build_ipa_from_nsp.py 的端到端语义）：

  NSP(+update) + prod.keys
   1 identify  读 NSP 票据/文件名 titleID → games/<id>/ 档案；
              无档案 → survey 摘要（模块 build ID 表）并退出码 2（提示 new_game.py 起草）
   2 extract   nx_extract（infra/nxnative dm/tools，LibHac）→ ExeFS/ + RomFS.img（--romfs-image）
   3 rewrite   patch_nso.py 逐模块 → patched/<模块>/<模块>.patched.bin + TSV 五件套
              （游戏模块补丁 = 档案 patches.py，经 DM_GAMES_DIR 注入；sdk = nxnative sdk_patches）
   4-7 hap     转调 M1 build_hap_from_profile.py（instantiate/prepare/build/report，档案驱动出包）

指纹缓存（对齐 nxnative development.md:153-157 机制）：extract / rewrite_<模块> / hap 三类
.key 文件放 <work>/keys/，内容 = 输入指纹 sha256 + 完成时写的 note 行；输入不变 → 该步
跳过并报 reused。删 .key 即强制重跑（--force 全部无视）。

退出码：0 成功；1 错误（progress 协议带 reason）；2 未知游戏（survey 已写，需 new_game.py）。

用法（M2 验收）：
  python scripts/build_hap_from_nsp.py "E:/Yuzu/游戏/马里奥奥德赛 [版本1.4.1]/nsp主体" \
      --update ../game-work/nsp/[UPD][v1.4.1][0100000000010800][v393216].nsp --progress
  （nsp 参数可以是装 NSP 的文件夹，也可以是单个 NSP 文件；update 缺省取文件夹里版本号最高的）
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]              # switch2hos/
NXN = REPO / "nxnative-ohos"
NXNATIVE = REPO / "infra" / "nxnative"                  # 只读：提取/重写工具链
DM_TOOLS = NXNATIVE / "dm" / "tools"

DEFAULT_KEYS = "E:/Yuzu/user/keys/prod.keys"            # PROJECT_ENV.md 记载的本机 keys 位置

PROGRESS = False
FAILED = None                                           # fail 事件的 stage，供 finally 兜底


# ---------------------------------------------------------------- progress ----
def ev(kind, **kw):
    """--progress json：stdout 每行一个 JSON 事件（对齐 M1 build_hap_from_profile.py 协议）。"""
    if PROGRESS:
        print(json.dumps({"event": kind, "ts": round(time.time(), 3), **kw}, ensure_ascii=False), flush=True)


def log(stage, step, detail):
    if PROGRESS:
        ev("step", stage=stage, step=step, detail=detail)
    else:
        print(f"[{stage}] {step}: {detail}", flush=True)


class Stage:
    """一个阶段的计时与 stage/stage_end 事件；with 块异常退出时发 fail。"""
    def __init__(self, name, note=""):
        self.name, self.note, self.t0 = name, note, time.time()

    def __enter__(self):
        ev("stage", stage=self.name, status="start")
        if not PROGRESS:
            print(f"[{self.name}] …{(' ' + self.note) if self.note else ''}", flush=True)
        return self

    def __exit__(self, et, e, tb):
        global FAILED
        if et is None:
            ev("stage_end", stage=self.name, ok=True, secs=round(time.time() - self.t0, 1))
            if not PROGRESS:
                print(f"[{self.name}] done ({time.time() - self.t0:.1f}s)", flush=True)
        else:
            FAILED = self.name
        return False


def die(reason, code=1):
    """失败出口：progress 模式发 fail 事件（带 reason），普通模式打印后退出。"""
    if PROGRESS:
        ev("fail", stage=FAILED or "-", reason=reason)
    else:
        print(f"FATAL: {reason}", file=sys.stderr, flush=True)
    sys.exit(code)


# ------------------------------------------------------------- 指纹缓存 ----
def file_digest(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fingerprint(*items):
    """sha256 over 输入。item 形如：
       ('files', [paths])  文件名+内容（缺失记 'missing'）
       ('stat',  [paths])  文件名+大小+mtime（大文件：NSP/镜像/工具二进制）
       ('repr',  obj)      任意可 repr 值（flag、路径布局）
    对齐 nxnative build_ipa_from_nsp.py fingerprint() 的分形。"""
    h = hashlib.sha256()
    for kind, v in items:
        h.update(f"\0{kind}\0".encode())
        if kind == "files":
            for p in v:
                p = Path(p)
                h.update(p.name.encode() + b"\0")
                try:
                    h.update(file_digest(p).encode())
                except OSError:
                    h.update(b"missing")
        elif kind == "stat":
            for p in v:
                p = Path(p)
                try:
                    st = p.stat()
                    h.update(f"{p.name}\0{st.st_size}\0{st.st_mtime_ns}\n".encode())
                except OSError:
                    h.update(f"{p.name}\0missing\n".encode())
        else:
            h.update(repr(v).encode())
    return h.hexdigest()


def reused(keyfile: Path, key: str):
    """指纹匹配 → 返回上次成功时写的 note（无 note 则 ''）；不匹配/缺文件 → None。"""
    try:
        k, _, note = keyfile.read_text(encoding="utf-8").partition("\n")
    except OSError:
        return None
    return note.strip() if k == key else None


def mark_done(keyfile: Path, key: str, note=""):
    keyfile.parent.mkdir(parents=True, exist_ok=True)
    keyfile.write_text(f"{key}\n{note}\n", encoding="utf-8")


# ------------------------------------------------------------ NSP 识别 ----
def pfs0_names(path: Path):
    """NSP = PFS0：列出内部文件名（照抄 nxnative build_ipa_from_nsp.pfs0_names）。"""
    with open(path, "rb") as f:
        h = f.read(16)
        if h[:4] != b"PFS0":
            return []
        n, st = struct.unpack_from("<II", h, 4)
        ent, strs = f.read(n * 0x18), f.read(st)
    return [strs[o:strs.index(b"\0", o)].decode(errors="replace")
            for o in (struct.unpack_from("<I", ent, i * 0x18 + 0x10)[0] for i in range(n))]


def title_of(path: Path):
    """titleID：票据名前 16 位十六进制（rights ID 的一半），否则文件名里的 [titleID]。"""
    for n in pfs0_names(path):
        if re.match(r"[0-9A-Fa-f]{32}\.tik$", n):
            return int(n[:16], 16)
    m = re.search(r"\[([0-9A-Fa-f]{16})\]", path.name)
    return int(m.group(1), 16) if m else None


def version_of(path: Path):
    m = re.search(r"\[v(\d+)\]", path.name)
    return int(m.group(1)) if m else None


def scan_nsps(nsp_arg: Path, update_arg: Path):
    """归组一个游戏：返回 (app_titleID, base_path, update_path_or_None)。
    规则对齐 nxnative scan_nsps：低位 …000=本体、…800=更新、其余=DLC（M2 不打 DLC，仅提示）。
    nsp_arg 是文件时视作本体；--update 显式给更新（可与本体不同目录）。"""
    fam = {"base": None, "updates": [], "dlc": []}
    cands = sorted([*nsp_arg.glob("*.nsp"), *nsp_arg.glob("*.xci")]) if nsp_arg.is_dir() else [nsp_arg]
    for p in cands:
        if p.stat().st_size == 0:
            log("identify", "skip", f"空文件 {p.name}")
            continue
        tid = title_of(p)
        if tid is None:
            log("identify", "skip", f"{p.name} 读不出 titleID（无票据、文件名无 [id]）")
            continue
        low = tid & 0xFFF
        if low == 0:
            fam["base"] = p
        elif low == 0x800:
            fam["updates"].append((version_of(p), p))
        else:
            fam["dlc"].append(p)
    if update_arg is not None:
        fam["updates"].append((version_of(update_arg), update_arg))
    if fam["base"] is None:
        die(f"未找到本体 NSP（…000）：{nsp_arg}（更新 NSP 不能单独提取，ExeFS 层叠需要本体）")
    if len(fam["dlc"]):
        log("identify", "dlc", f"{len(fam['dlc'])} 个 DLC NSP 暂不打包（M2 范围外），已忽略")
    app = title_of(fam["base"])
    ups = sorted(fam["updates"], key=lambda u: u[0] or 0)
    upd = ups[-1][1] if ups else None                     # 文件夹里版本号最高的更新
    return app, fam["base"], upd


def build_id(nso: Path) -> str:
    with open(nso, "rb") as f:
        f.seek(0x40)
        return f.read(16).hex()


def exefs_modules(exefs: Path):
    """ExeFS 模块清单：按 nxnative 加载语义排序（main, subsdk*, sdk, rtld），排除 *.npdm。"""
    mods = sorted(p.name for p in exefs.iterdir() if p.is_file() and not p.name.endswith(".npdm"))
    def rank(n):
        if n == "main":
            return (0, n)
        if n.startswith("subsdk"):
            return (1, n)
        if n == "sdk":
            return (2, n)
        return (3, n)
    return sorted(mods, key=rank)


def parse_game_tsv(path: Path) -> dict:
    keys = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) >= 2:
            keys[parts[0]] = parts[1]
    return keys


def build_rows(path: Path) -> dict:
    """game.tsv 的 build 行 → {package: {module: build_id, '_display': 显示版本}}。"""
    rows = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("build\t"):
            continue
        c = line.split("\t")
        row = {"_display": c[2] if len(c) > 2 else ""}
        for kv in c[3:]:
            if "=" in kv:
                m, b = kv.split("=", 1)
                row[m] = b
        rows[c[1]] = row
    return rows


# ------------------------------------------------------------ 工具准备 ----
def ensure_nx_extract(work: Path, logs: Path) -> Path:
    """nx_extract 可执行：优先仓库预编译产物（bin/Release/net10.0），缺则 dotnet build 到 work/tools。
    对齐 nxnative build_nx_extract 的产物布局（nx_extract.exe + LibHac.dll 同目录）。"""
    pre = DM_TOOLS / "nx_extract" / "bin" / "Release" / "net10.0"
    dst = work / "tools" / "nx_extract"
    if (pre / "nx_extract.exe").is_file():
        if not (dst / "nx_extract.exe").is_file():
            shutil.copytree(pre, dst, dirs_exist_ok=True)
            log("extract", "nx_extract", f"使用仓库预编译 {pre.name}")
        return dst / "nx_extract.exe"
    if not (dst / "nx_extract.exe").is_file():
        log("extract", "nx_extract", "dotnet build（首次，约 10-30s）")
        import tempfile
        with tempfile.TemporaryDirectory(prefix="nx_extract_") as t:
            r = subprocess.run(["dotnet", "build", str(DM_TOOLS / "nx_extract"), "-c", "Release",
                                "-o", t, "-v", "quiet", "-nologo"],
                               capture_output=True, text=True)
            (logs / "nx_extract_build.log").write_text(r.stdout + r.stderr, encoding="utf-8")
            if r.returncode != 0 or not (Path(t) / "nx_extract.exe").is_file():
                die(f"dotnet build nx_extract 失败，见 {logs / 'nx_extract_build.log'}")
            shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(t, dst)
    return dst / "nx_extract.exe"


def run_logged(cmd, log_path: Path, on_line=None):
    """跑子进程，全量输出写 log；非零退出返回 returncode（调用方决定 die）。"""
    cmd = [str(c) for c in cmd]
    log_path.parent.mkdir(parents=True, exist_ok=True)
    tail = []
    with open(log_path, "w", encoding="utf-8", errors="replace") as lf, \
            subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, errors="replace", bufsize=1) as p:
        for line in p.stdout:
            lf.write(line)
            tail.append(line)
            if on_line:
                try:
                    on_line(line.rstrip("\n"))
                except Exception:
                    pass
    if p.returncode:
        ev("say", msg="---- 工具输出尾部 ----\n" + "".join(tail[-15:]).rstrip())
    return p.returncode


def poll_tree(root: Path, total: int, stop: threading.Event, stage_name: str):
    """后台线程：每 2s 统计 root 已写字节数 → progress 事件（总量为 NSP 大小估计，标 est）。"""
    while not stop.wait(2):
        done = 0
        todo = [root]
        while todo:
            try:
                it = os.scandir(todo.pop())
            except OSError:
                continue
            with it:
                for e in it:
                    try:
                        if e.is_dir(follow_symlinks=False):
                            todo.append(e.path)
                        else:
                            done += e.stat(follow_symlinks=False).st_size
                    except OSError:
                        pass
        ev("progress", stage=stage_name, done=done, total=total, unit="B", est=True,
           frac=min(done / total, 0.99) if total else None)


# ------------------------------------------------------------ 步骤实现 ----
def step_identify(args):
    """步骤1：NSP → (app, base, upd)；查档案；校验 build 行。返回档案目录。"""
    nsp_arg = Path(args.nsp).resolve()
    if not nsp_arg.exists():
        die(f"NSP 路径不存在：{nsp_arg}")
    app, base, upd = scan_nsps(nsp_arg, Path(args.update).resolve() if args.update else None)
    tid = f"{app:016X}"
    log("identify", "nsp", f"本体 {base.name}" + (f" + 更新 {upd.name}" if upd else "（无更新，v0）"))

    archive = Path(args.archive_root).resolve() / tid
    if not (archive / "game.tsv").is_file():
        return app, base, upd, None                       # 未知游戏 → survey（由 main 处理）

    tsv = parse_game_tsv(archive / "game.tsv")
    if tsv.get("title_id") != tid:
        die(f"档案 {archive} 的 title_id={tsv.get('title_id')} 与 NSP {tid} 不一致")
    rows = build_rows(archive / "game.tsv")
    pkg = tsv.get("package")
    if upd is not None and pkg and pkg != "v0" and rows and version_of(upd) is not None \
            and f"v{version_of(upd)}" != pkg:
        log("identify", "package", f"警告：档案 package={pkg}，文件夹内更新为 v{version_of(upd)}，按文件夹版本继续")
    log("identify", "profile", f"{tid} {tsv.get('name', '?')}（档案 {archive.name}）")
    return app, base, upd, archive


def survey(app, base, upd, keys, work, logs, nx_exe, allow_damaged):
    """未知游戏：manifest 模式提取（不写游戏文件）→ survey.md（模块 build ID 表）。"""
    tid = f"{app:016X}"
    ex = work / "survey_extract"
    tmp = work / "survey_extract.partial"
    shutil.rmtree(tmp, ignore_errors=True)
    log("survey", "extract", "ExeFS + RomFS 清单（--manifest，不写游戏文件）")
    rc = run_logged([nx_exe, keys, base, upd or "-", tmp, "--manifest"]
                    + (["--allow-damaged"] if allow_damaged else []), logs / "survey_extract.log")
    if rc != 0:
        die(f"nx_extract survey 提取失败 rc={rc}，见 {logs / 'survey_extract.log'}")
    shutil.rmtree(ex, ignore_errors=True)
    tmp.rename(ex)

    mods, disp, name = [], "?", tid
    nacp = ex / "control.nacp"
    if nacp.is_file():
        sys.path.insert(0, str(DM_TOOLS))
        import nacp as nacp_mod                              # nxnative 只读复用
        name, disp = nacp_mod.name(nacp) or tid, nacp_mod.version(nacp) or "?"
    for m in exefs_modules(ex / "ExeFS"):
        p = ex / "ExeFS" / m
        mods.append((m, build_id(p) if p.stat().st_size >= 0x50 else "?",
                     f"{p.stat().st_size / 2**20:.1f}"))

    lines = [f"# Survey：{name}（{tid}，display {disp}）",
             "", f"- 本体：`{base.name}`（{base.stat().st_size / 2**30:.2f} GB）",
             f"- 更新：`{upd.name}`" if upd else "- 更新：无（v0）",
             "", "## 模块 build ID（供 games/<id>/game.tsv 的 build 行）", "",
             "| 模块 | build ID | 大小 (MB) |", "|---|---|---|"]
    lines += [f"| {m} | `{b}` | {s} |" for m, b, s in mods]
    lines += ["", "下一步：`python scripts/new_game.py <NSP> -k <prod.keys>` 起草档案（内存/图形键人工补）。"]
    (work / "survey.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    log("survey", "report", str(work / "survey.md"))
    return work / "survey.md"


def step_extract(app, base, upd, keys, work, logs, args) -> Path:
    """步骤2：nx_extract --romfs-image → work/extract/{ExeFS, RomFS.img, control.nacp, icon.jpg}。"""
    ex, tmp = work / "extract", work / "extract.partial"
    nx_exe = ensure_nx_extract(work, logs)
    # 指纹：NSP（名字+大小+mtime，避免整包哈希）+ 提取器二进制 + keys 文件 stat + 损坏豁免开关
    key = fingerprint(("stat", [base] + ([upd] if upd else []) +
                       sorted(nx_exe.parent.glob("*.dll")) + [nx_exe, Path(keys)]),
                      ("repr", ["romfs-image", args.allow_damaged]))
    kf = work / "keys" / "extract.key"
    note = f"{base.name}" + (f" + {upd.name}" if upd else "")
    if not args.force and reused(kf, key) is not None and (ex / "ExeFS" / "main").is_file() \
            and (ex / "RomFS.img").is_file():
        log("extract", "reuse", f"输入未变，沿用 {ex}")
        return ex
    kf.unlink(missing_ok=True)
    shutil.rmtree(tmp, ignore_errors=True)
    total = base.stat().st_size + (upd.stat().st_size if upd else 0)
    stop = threading.Event()
    th = threading.Thread(target=poll_tree, args=(tmp, total, stop, "extract"), daemon=True)
    if PROGRESS:
        th.start()
    try:
        rc = run_logged([nx_exe, keys, base, upd or "-", tmp, "--romfs-image"]
                        + (["--allow-damaged"] if args.allow_damaged else []),
                        logs / "extract.log",
                        on_line=lambda l: log("extract", "nx_extract", l) if not PROGRESS else None)
    finally:
        stop.set()
    if rc != 0:
        die(f"nx_extract 失败 rc={rc}，见 {logs / 'extract.log'}")
    for need in ("ExeFS/main", "RomFS.img"):
        if not (tmp / need).is_file():
            die(f"nx_extract 未产出 {need}，见 {logs / 'extract.log'}")
    shutil.rmtree(ex, ignore_errors=True)
    tmp.rename(ex)
    size = (ex / "RomFS.img").stat().st_size
    mark_done(kf, key, f"{note}: RomFS.img {size:,}B")
    log("extract", "done", f"ExeFS {len(exefs_modules(ex / 'ExeFS'))} 模块 + RomFS.img {size:,}B → {ex}")
    return ex


def step_rewrite(ex: Path, archive: Path, work: Path, logs: Path, args):
    """步骤3：逐模块 patch_nso.py → work/patched/<模块>/；build 行校验 + 逐模块指纹缓存。"""
    # 先决依赖（与既有 smo 构建同解释器语义：lz4 + capstone）
    try:
        import lz4.block  # noqa: F401
        import capstone  # noqa: F401
    except ImportError as e:
        die(f"patch_nso 需要 lz4/capstone（{sys.executable} 缺 {e.name}；pip install lz4 capstone）")

    tsv = parse_game_tsv(archive / "game.tsv")
    pkg = tsv.get("package", "v0")
    row = build_rows(archive / "game.tsv").get(pkg, {})
    modules = exefs_modules(ex / "ExeFS")
    bad = [(m, build_id(ex / "ExeFS" / m), row.get(m)) for m in modules
           if row.get(m) not in (None, build_id(ex / "ExeFS" / m))]
    if bad:
        detail = "; ".join(f"{m}: 提取={got} 档案={want}" for m, got, want in bad)
        die(f"模块 build ID 与档案 build 行不符（package {pkg}）：{detail}。"
            f"需派生档案（derived profile）或换对应版本 NSP——M2 不自动派生")
    log("rewrite", "verify", f"{len(modules)} 模块 build ID 与档案 {pkg} 行一致")

    # 重写器输入面（对齐既有产物 build_src.sha256 的记录面）+ 档案 patches.py 注入
    tool_files = [DM_TOOLS / f for f in ("patch_nso.py", "nso_asm.py", "nso_code.py",
                                         "nso_match.py", "sdk_patches.py")]
    patch_glob = sorted((DM_TOOLS / ".." / "games").glob("*/patches.py")) + \
                 sorted((NXN / "games").glob("*/patches.py"))
    env = dict(os.environ,
               DM_GAMES_DIR=str(NXN / "games"),     # 档案 patches.py 参与（nxnative os.pathsep 语义）
               DM_GAME=archive.name)                # MATCH_CUTS 的档案选择键
    for m in modules:
        nso = ex / "ExeFS" / m
        out = work / "patched" / m
        key = fingerprint(("files", [nso] + tool_files + patch_glob),
                          ("repr", [str(NXN / "games"), archive.name]))
        kf = work / "keys" / f"rewrite_{m}.key"
        if not args.force and reused(kf, key) is not None and (out / f"{m}.patched.bin").is_file():
            log("rewrite", m, "输入未变，跳过（reused）")
            continue
        kf.unlink(missing_ok=True)
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        t0 = time.time()
        rc = run_logged([sys.executable, str(DM_TOOLS / "patch_nso.py"), nso, out, m],
                        logs / f"rewrite_{m}.log")
        if rc != 0 or not (out / f"{m}.patched.bin").is_file():
            die(f"patch_nso {m} 失败 rc={rc}，见 {logs / f'rewrite_{m}.log'}")
        # 产物来源记录（对齐既有 build_src.sha256 语义：工具链哈希随产物留档）
        (out / "build_src.sha256").write_text(
            "".join(f"{file_digest(p)}  {p}\n" for p in tool_files + patch_glob), encoding="utf-8")
        mark_done(kf, key, f"{m} {build_id(nso)}")
        log("rewrite", m, f"{m}.patched.bin {(out / f'{m}.patched.bin').stat().st_size:,}B"
                          f"（{time.time() - t0:.0f}s）")
    return work / "patched"


def step_hap(archive: Path, work: Path, ex: Path, args):
    """步骤4-7：转调 M1 build_hap_from_profile.py（instantiate/prepare/build/report）。"""
    hap_area = Path(args.out).resolve() if args.out else NXN / ".build" / archive.name
    hap = hap_area / "entry/build/default/outputs/default/entry-default-signed.hap"
    # M1 调用指纹：档案全文件 + patched.bin 内容 + RomFS stat + M1 脚本/工具/模板 stat
    fonts_src = Path(args.fonts_src).resolve() if args.fonts_src else \
        REPO / "mario-odessy-harmony" / "entry/src/main/cpp/generated"
    key = fingerprint(("files", sorted(p for p in archive.rglob("*") if p.is_file())),
                      ("files", sorted(work.glob("patched/*/*.patched.bin"))),
                      ("stat", [ex / "RomFS.img", fonts_src / "shared_fonts_config.h",
                                fonts_src / "shared_fonts_data.h",
                                NXN / "scripts/build_hap_from_profile.py",
                                NXN / "tools/prepare_runtime.py"]
                               + [p for p in (NXN / "template").rglob("*") if p.is_file()]),
                      ("repr", [str(hap_area), args.keep]))
    kf = work / "keys" / "hap.key"
    if not args.force and reused(kf, key) is not None and hap.is_file():
        log("hap", "reuse", f"输入未变，沿用 {hap}（{hap.stat().st_size:,}B）")
        return hap
    kf.unlink(missing_ok=True)
    cmd = [sys.executable, str(NXN / "scripts/build_hap_from_profile.py"), str(archive),
           "--work", str(work), "--romfs", str(ex / "RomFS.img"), "--out", str(hap_area)]
    if args.fonts_src:
        cmd += ["--fonts-src", str(fonts_src)]
    if args.keep:
        cmd += ["--keep"]
    if PROGRESS:
        cmd += ["--progress"]                              # M1 事件同协议直通 stdout
    t0 = time.time()
    rc = subprocess.run(cmd).returncode
    if rc != 0 or not hap.is_file():
        die(f"M1 build_hap_from_profile 失败 rc={rc}（HAP 未产出；上方有其日志）")
    mark_done(kf, key, f"{hap.name} {hap.stat().st_size:,}B")
    log("hap", "done", f"{hap}（{hap.stat().st_size:,}B，{time.time() - t0:.0f}s）")
    return hap


# ---------------------------------------------------------------- main ----
def main():
    global PROGRESS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nsp", help="NSP 文件夹（一个游戏）或本体 NSP 文件路径")
    ap.add_argument("-k", "--keys", default=os.environ.get("NX_PROD_KEYS", DEFAULT_KEYS),
                    help=f"prod.keys（缺省 $NX_PROD_KEYS 或 {DEFAULT_KEYS}）")
    ap.add_argument("--update", help="更新 NSP 显式路径（缺省取文件夹内版本号最高者）")
    ap.add_argument("--archive-root", default=str(NXN / "games"),
                    help="档案根目录（缺省 nxnative-ohos/games；测试 survey/new_game 时指到空目录）")
    ap.add_argument("--work", help="工作区根，缺省 nxnative-ohos/.build/<titleID>_nsp/")
    ap.add_argument("--out", help="HAP 构建区（转传 M1 --out），缺省 .build/<titleID>/")
    ap.add_argument("--fonts-src", help="转传 M1 --fonts-src（embedded 字体对目录）")
    ap.add_argument("--survey-only", action="store_true", help="只做 survey（等价于未知游戏路径），不出 HAP")
    ap.add_argument("--no-hap", action="store_true", help="步骤 1-3 后停（不出 HAP，快速验证提取/重写）")
    ap.add_argument("--keep", action="store_true", help="转传 M1 --keep（HAP 构建区复用而非整体重建）")
    ap.add_argument("--allow-damaged", action="store_true", help="NSP 哈希不过的文件按只读继续（转传 nx_extract）")
    ap.add_argument("--force", action="store_true", help="无视指纹缓存全部重跑")
    ap.add_argument("--progress", action="store_true", help="每阶段一行 JSON 事件")
    args = ap.parse_args()
    PROGRESS = args.progress

    keys = Path(args.keys)
    if not keys.is_file():
        die(f"prod.keys 不存在：{keys}（-k 指定）")
    t0 = time.time()
    global FAILED

    # ---- 步骤1 identify ----
    with Stage("identify", "titleID → 档案"):
        app, base, upd, archive = step_identify(args)
        tid = f"{app:016X}"
        work = Path(args.work).resolve() if args.work else NXN / ".build" / f"{tid}_nsp"
        logs = work / "logs"
        logs.mkdir(parents=True, exist_ok=True)
        nx_exe = ensure_nx_extract(work, logs)              # survey 与正式提取共用
        if archive is None or args.survey_only:
            survey(app, base, upd, keys, work, logs, nx_exe, args.allow_damaged)
            ev("result", outcome="survey", title_id=tid, survey=str(work / "survey.md"))
            print(f"SURVEY: {work / 'survey.md'}")
            sys.exit(2 if archive is None else 0)           # 2 = 无档案，需 new_game.py

    # ---- 步骤2 extract ----
    with Stage("extract", "nx_extract → ExeFS + RomFS.img"):
        ex = step_extract(app, base, upd, keys, work, logs, args)

    # ---- 步骤3 rewrite ----
    with Stage("rewrite", "patch_nso → patched.bin + TSV"):
        patched = step_rewrite(ex, archive, work, logs, args)

    if args.no_hap:
        ev("result", outcome="rewritten", title_id=tid, patched=str(patched), secs=round(time.time() - t0, 1))
        print(f"PATCHED: {patched}")
        return

    # ---- 步骤4-7 hap（转调 M1）----
    with Stage("hap", "M1 build_hap_from_profile（instantiate/prepare/build/report）"):
        hap = step_hap(archive, work, ex, args)

    ev("result", outcome="hap", title_id=tid, hap=str(hap), bytes=hap.stat().st_size,
       secs=round(time.time() - t0, 1))
    print(f"HAP: {hap}")
    print(f"BYTES: {hap.stat().st_size:,}")


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except KeyboardInterrupt:
        die("中断", 130)
    except Exception as e:                                  # 兜底：意外异常也走 fail 事件
        import traceback
        die(f"{type(e).__name__}: {e}\n{traceback.format_exc(limit=5)}")
