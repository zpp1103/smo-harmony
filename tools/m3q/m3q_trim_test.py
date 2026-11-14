#!/usr/bin/env python3
"""M3q 验证实验：5 个 "unknown instruction" 失败是否只是尾部浮点常量池被当成指令。

做法：把 .code 截到最后一条 EXIT（含）+8 字节，重跑翻译器 + glslang。
不改 translate.c；只改喂给它的字节范围（运行时按 code 指令区长度喂同样可以）。
"""
import json, os, shutil, struct, subprocess, sys

CORPUS = "/Users/zpp/VibeCoding/switch2hos/logs/m3p_shaderdump/shaderdump"
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "m3q_translate")
BASE = "/tmp/m3q"
TRIM = os.path.join(BASE, "trim")
GLSL = os.path.join(TRIM, "glsl")
SPV = os.path.join(TRIM, "out")

res = json.load(open(os.path.join(BASE, "results.json")))
fails = [k for k, e in res.items() if not e["translate_ok"] and "unknown instruction" in e["translate_error"]]

os.makedirs(GLSL, exist_ok=True)
os.makedirs(SPV, exist_ok=True)
index = []
report = {}
for ent in fails:
    key, stage = ent.rsplit(".", 1)
    stage = int(stage)
    src = os.path.join(CORPUS, f"{key}.{stage}.code")
    data = open(src, "rb").read()
    code = data[0x80:]
    last = None
    for i in range(len(code) // 8):
        w = struct.unpack_from("<Q", code, i * 8)[0]
        if (w >> 52) == 0xE30:
            last = i * 8
    cut = 0x80 + last + 8
    dst = os.path.join(TRIM, f"{key}.{stage}.code")
    open(dst, "wb").write(data[:cut])
    shutil.copyfile(os.path.join(CORPUS, f"{key}.{stage}.ctl"), os.path.join(TRIM, f"{key}.{stage}.ctl"))
    index.append(f"{key} {stage} ctl={0} code={cut} magic=98761234 hdr=12345678")
    report[ent] = {"orig_bytes": len(data), "trim_bytes": cut,
                   "last_exit_rel": hex(last), "dropped_bytes": len(data) - cut}

open(os.path.join(TRIM, "index.txt"), "w").write("\n".join(index) + "\n")

p = subprocess.run([TOOL, TRIM, GLSL], capture_output=True, text=True)
print("translate rc", p.returncode)
print(p.stdout)
for ent in fails:
    key, stage = ent.rsplit(".", 1)
    stage = int(stage)
    g = os.path.join(GLSL, f"{key}.{stage}.glsl")
    if not os.path.exists(g):
        report[ent]["spv_ok"] = False
        report[ent]["why"] = "translate failed again"
        continue
    spv = os.path.join(SPV, f"{key}.{stage}.spv")
    name = "vert" if stage == 0 else "frag"
    c = subprocess.run(["/opt/homebrew/bin/glslangValidator", "-V", "--target-env", "vulkan1.3",
                        "-S", name, "-o", spv, g], capture_output=True, text=True)
    b = open(spv, "rb").read() if os.path.exists(spv) else b""
    report[ent]["spv_ok"] = c.returncode == 0 and len(b) >= 4 and struct.unpack_from("<I", b)[0] == 0x07230203
    report[ent]["spv_bytes"] = len(b)
    report[ent]["spv_path"] = spv
    report[ent]["diag"] = [l for l in (c.stdout + c.stderr).splitlines() if "ERROR" in l][:3]

print(json.dumps(report, indent=1))
json.dump(report, open(os.path.join(BASE, "trim_report.json"), "w"), indent=1)
