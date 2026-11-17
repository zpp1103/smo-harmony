#!/usr/bin/env python3
"""M132A 语法门：用工程自带的 compile_commands.json（OHOS clang）对指定源文件跑 -fsyntax-only。
用法: syntax_gate.py <repo> <tree> <file...>
输出每个文件 rc / errors / warnings；nvn_vk.c 的 warnings 必须 == 20（build #7/#8 基线）。
（照抄 logs/m126a/build.sh 第 8 段的做法：把命令里的 repo 前缀替换成 tree 前缀，去掉 -o/-c 再加 -fsyntax-only。）
"""
import json, os, shlex, subprocess, sys

repo, tree = sys.argv[1], sys.argv[2]
want = sys.argv[3:]
db = json.load(open(os.path.join(
    repo, "mario-odessy-harmony/entry/.cxx/default/default/debug/arm64-v8a/compile_commands.json")))

bad = 0
nvkwarn = None
for w in want:
    ent = None
    for e in db:
        if e["file"].endswith("/" + w):
            ent = e
            break
    if ent is None:
        print("NO-ENTRY", w)
        bad += 1
        continue
    toks = shlex.split(ent["command"].replace(repo, tree))
    out, i = [], 0
    while i < len(toks):
        t = toks[i]
        if t == "-o":
            i += 2
            continue
        if t == "-c":
            i += 1
            continue
        out.append(t)
        i += 1
    out.append("-fsyntax-only")
    r = subprocess.run(out, cwd=ent["directory"].replace(repo, tree), capture_output=True, text=True)
    errs = [l for l in r.stderr.splitlines() if "error:" in l]
    warns = [l for l in r.stderr.splitlines() if "warning:" in l]
    print("syntax %-40s rc=%d errors=%d warnings=%d" % (w, r.returncode, len(errs), len(warns)))
    for l in errs[:10]:
        print("   ", l)
    if errs or r.returncode != 0:
        bad += 1
    if w.endswith("nvn_vk.c"):
        nvkwarn = len(warns)
print("SYNTAX-GATE", "PASS" if bad == 0 else "FAIL(%d)" % bad,
      "| nvn_vk.c warnings=%s (baseline 20)" % nvkwarn)
sys.exit(0 if bad == 0 and nvkwarn == 20 else 1)
