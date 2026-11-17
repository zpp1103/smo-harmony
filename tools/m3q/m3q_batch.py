#!/usr/bin/env python3
"""M3q 批量驱动：翻译结果(trans.tsv) -> glslangValidator 编译 -> index.json + 统计。

成功判定严格：glslangValidator 返回 0 **且** .spv 前 4 字节为 03 02 23 07。
所有产物写在 --out 下（默认 /tmp/m3q），不放进 git 仓库。
"""
import argparse, hashlib, json, os, re, struct, subprocess, sys, time

CXX = "/opt/homebrew/bin/glslangValidator"
STAGE_EXT = {0: ("vert", "-S", "vert"), 1: ("frag", "-S", "frag"), 5: ("comp", "-S", "comp")}


def parse_tsv(path):
    rows = []
    for line in open(path):
        f = line.rstrip("\n").split("\t")
        if len(f) < 4 or f[0] != "T":
            continue
        r = {"key": f[1], "stage": int(f[2]), "t_ok": f[3] == "1",
             "err": "", "raw": line.rstrip("\n")}
        if r["t_ok"]:
            for tok in f[4].split():
                if "=" not in tok:
                    continue
                k, v = tok.split("=", 1)
                if k == "stage":
                    continue          # C 侧打的 "stage=vert" 别覆盖第 3 列的数字 stage
                r[k] = v
        else:
            err = f[4][4:] if f[4].startswith("err=") else f[4]
            r["err"] = re.sub(r"\s+stage=\S+\s+path=\S+$", "", err)
        rows.append(r)
    return rows


def fail_sites(translate_c):
    """提取 translate.c 里所有 fail(c, "fmt", ...) 的 (行号, 骨架)。"""
    sites = []
    src = open(translate_c, encoding="utf-8", errors="replace").read().splitlines()
    for i, line in enumerate(src, 1):
        m = re.search(r'fail\(c,\s*"((?:[^"\\]|\\.)*)"', line)
        if not m:
            continue
        fmt = m.group(1).replace('\\"', '"')
        skel = re.sub(r"%[-#0-9.*hljztL]*[diouxXscfeEgG]", "*", fmt)
        sites.append((i, skel, fmt))
    return sites


def match_site(err, sites):
    skel = re.sub(r"%[-#0-9.*hljztL]*[diouxXscfeEgG]", "*", err)
    # 精确骨架匹配；"unknown instruction" 这类带多个参数也能对上
    for ln, s, fmt in sites:
        if s == skel:
            return ln, fmt
    # 退一步：前缀匹配（值里可能含 %）
    for ln, s, fmt in sites:
        pre = s.split("*")[0]
        if pre and skel.startswith(pre):
            return ln, fmt
    return None, None


def key_recompute(corpus, key, stage):
    """svr: 实测重算 sha256(ctl||code)[:8] -> 16 hex，对照文件名。"""
    ctl = open(os.path.join(corpus, f"{key}.{stage}.ctl"), "rb").read()
    code = open(os.path.join(corpus, f"{key}.{stage}.code"), "rb").read()
    return hashlib.sha256(ctl + code).hexdigest()[:16]


def compile_one(glsl, spv, stage, timeout=180):
    ext, flag, name = STAGE_EXT[stage]
    t0 = time.perf_counter()
    p = subprocess.run([CXX, "-V", "--target-env", "vulkan1.3", flag, name, "-o", spv, glsl],
                       capture_output=True, text=True, timeout=timeout)
    dt = time.perf_counter() - t0
    out = (p.stdout + p.stderr).strip()
    ok_magic, nbytes, sha = False, 0, ""
    if os.path.exists(spv):
        b = open(spv, "rb").read()
        nbytes = len(b)
        ok_magic = len(b) >= 4 and struct.unpack_from("<I", b, 0)[0] == 0x07230203
        sha = hashlib.sha256(b).hexdigest()[:16]
    diagnostics = [l for l in out.splitlines() if "ERROR" in l or "error" in l][:6]
    return {
        "rc": p.returncode, "t": round(dt, 3),
        "ok": p.returncode == 0 and ok_magic,
        "spv_bytes": nbytes, "spv_sha256_16": sha if ok_magic else "",
        "diagnostics": diagnostics, "raw": out[-2000:],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default="/Users/zpp/VibeCoding/switch2hos/logs/m3p_shaderdump/shaderdump")
    ap.add_argument("--out", default="/tmp/m3q")
    ap.add_argument("--tsv", default="/tmp/m3q/trans.tsv")
    ap.add_argument("--translate-c", default="/Users/zpp/VibeCoding/switch2hos/mario-odessy-harmony/"
                    "entry/src/main/cpp/platform/nvn_replay/maxwell/translate.c")
    ap.add_argument("--no-compile", action="store_true")
    a = ap.parse_args()

    os.makedirs(os.path.join(a.out, "out"), exist_ok=True)
    rows = parse_tsv(a.tsv)
    sites = fail_sites(a.translate_c)
    results = {}
    for r in rows:
        k, st = r["key"], r["stage"]
        ent = {"key": k, "stage": st, "translate_ok": r["t_ok"], "translate_error": r["err"],
               "t_translate": None, "compile": None}
        if r["t_ok"]:
            for fld in ("attr", "out", "cbuf", "cbdyn", "cbwin", "ntex", "lmem", "ninst", "wdepth",
                        "disc", "wps", "gmem", "gmemst", "tint", "rscale", "glsl", "cbsz", "tex", "path"):
                ent[fld] = r.get(fld, "")
            if not a.no_compile:
                glsl = r.get("path", "")
                spv = os.path.join(a.out, "out", f"{k}.{st}.spv")
                ent["compile"] = compile_one(glsl, spv, st)
                ent["spv_path"] = spv
        else:
            ln, fmt = match_site(r["err"], sites)
            ent["fail_line"] = ln
            ent["fail_fmt"] = fmt
        results[f"{k}.{st}"] = ent

    # ---- 统计
    n = len(results)
    t_ok = sum(1 for e in results.values() if e["translate_ok"])
    c_ok = sum(1 for e in results.values() if e.get("compile", {}) and e["compile"]["ok"])
    per_stage = {}
    for e in results.values():
        s = per_stage.setdefault(e["stage"], {"n": 0, "translate_ok": 0, "spv_ok": 0})
        s["n"] += 1
        s["translate_ok"] += e["translate_ok"]
        s["spv_ok"] += bool(e.get("compile", {}) and e["compile"]["ok"])

    tax = {}
    for e in results.values():
        if e.get("compile", {}) and e["compile"]["ok"]:
            continue
        if not e["translate_ok"]:
            k = f"translate: {e['translate_error']}"
            k = re.sub(r"at 0x[0-9a-f]+", "at <pc>", k)
            k = re.sub(r"instruction [0-9a-f]+", "instruction <word>", k)
            src = f"translate.c:{e['fail_line']}" if e["fail_line"] else "translate.c:?"
        else:
            diags = e["compile"]["diagnostics"] or ["exit %d" % e["compile"]["rc"]]
            k = "glslang: " + diags[0][:160]
            src = "glsl 生成/编译期"
        d = tax.setdefault(k, {"n": 0, "line": src, "keys": [], "diag": e["compile"]["diagnostics"] if e.get("compile") else []})
        d["n"] += 1
        if len(d["keys"]) < 3:
            d["keys"].append(f"{e['key']}.{e['stage']}")

    # ---- 查表口径实测：全部 200 条重算 sha256(ctl||code)[:16] 对照文件名
    keycheck_all = []
    for e in results.values():
        recomputed = key_recompute(a.corpus, e["key"], e["stage"])
        keycheck_all.append({"key": e["key"], "stage": e["stage"], "recomputed": recomputed,
                             "filename": e["key"], "match": recomputed == e["key"]})
    keycheck = keycheck_all[:3]          # 报告里给 3 个例子（派活单要求）
    keycheck_summary = {"checked": len(keycheck_all),
                        "match": sum(1 for x in keycheck_all if x["match"])}

    # ---- index.json（key 口径 = sha256(ctl||code)[:16]，与 shader_dump.c 一致）
    index = {}
    for e in results.values():
        k = e["key"]
        node = index.setdefault(k, {"key": k})
        st = f"stage{e['stage']}"
        if e["translate_ok"] and e.get("compile", {}) and e["compile"]["ok"]:
            node[st] = {
                "status": "ok",
                "spv": e["spv_path"], "spv_bytes": e["compile"]["spv_bytes"],
                "spv_sha256_16": e["compile"]["spv_sha256_16"],
                "glsl": e.get("path", ""), "glsl_bytes": int(e.get("glsl", 0)),
                "stage": e["stage"],
                "attr_mask": int(e.get("attr", 0), 0), "out_mask": int(e.get("out", 0), 0),
                "cbuf_mask": int(e.get("cbuf", 0), 0), "cbuf_dynamic": int(e.get("cbdyn", 0), 0),
                "cbwin_mask": int(e.get("cbwin", 0), 0), "ntex": int(e.get("ntex", 0)),
                "local_mem": int(e.get("lmem", 0)), "num_insts": int(e.get("ninst", 0)),
                "writes_depth": bool(int(e.get("wdepth", 0))), "uses_discard": bool(int(e.get("disc", 0))),
                "writes_point_size": bool(int(e.get("wps", 0))), "uses_gmem": bool(int(e.get("gmem", 0))),
                "uses_gmem_store": bool(int(e.get("gmemst", 0))), "uses_tint": bool(int(e.get("tint", 0))),
                "uses_rscale": bool(int(e.get("rscale", 0))),
                "cbuf_size": {int(x.split(":")[0]): int(x.split(":")[1])
                              for x in e.get("cbsz", "").split(",") if ":" in x},
                "tex": parse_tex(e.get("tex", "")),
            }
            # 便捷镜像（派活单口径：顶层 attr_mask/ntex/cbuf_mask）
            node["stage"] = e["stage"]
            node["attr_mask"] = node[st]["attr_mask"]
            node["ntex"] = node[st]["ntex"]
            node["cbuf_mask"] = node[st]["cbuf_mask"]
        else:
            node[st] = {"status": "failed", "stage": e["stage"],
                        "translate_ok": e["translate_ok"],
                        "error": e["translate_error"] or (e["compile"]["diagnostics"][:2] if e.get("compile") else ""),
                        "fail_line": e.get("fail_line")}

    meta = {
        "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
        "corpus": a.corpus, "out_dir": a.out,
        "key_scheme": "sha256(ctl_bytes || code_bytes)[:16] (16 hex) - same as shader_dump.c sd_one()",
        "entries": n, "translate_ok": t_ok, "spv_ok": c_ok,
        "per_stage": per_stage,
        "compile_flags": f"{CXX} -V --target-env vulkan1.3 -S <stage>",
        "key_verification": keycheck, "key_verification_summary": keycheck_summary,
        "baked_with": {
            "glsl": 1, "attr_kind": "all float (0) - corpus carries no NVN vertex format",
            "depth_minus_one": not os.environ.get("M3Q_NO_ZREMAP"),
            "flip_y": not os.environ.get("M3Q_NO_FLIP"), "flip_x": False, "no_ds_write": False,
            "maxwell_no_int64": False, "maxwell_gmem_base": "0x80000000", "maxwell_gmem_size": "0x40000000",
            "note": "runtime must use the same ShaderKey flags for these .spv to match the baked clip/I-O convention",
        },
        "note": "corpus keys are unique per shader; no key has both stage0 and stage1",
    }

    # 可选：把“截掉尾部常量池”实验的成功结果作为 alt 挂上（不影响严格统计）
    tr = os.path.join(a.out, "trim_report.json")
    if os.path.exists(tr):
        trim = json.load(open(tr))
        meta["trim_experiment"] = {"cases": len(trim),
                                   "ok_after_trim": sum(1 for v in trim.values() if v.get("spv_ok")),
                                   "what": "cut .code at last EXIT+8 (trailing float constant pool is not code)"}
        for ent, v in trim.items():
            k, st = ent.rsplit(".", 1)
            st = f"stage{st}"
            if k in index and st in index[k] and v.get("spv_ok"):
                index[k][st]["alt"] = {
                    "status": "ok_with_trim", "spv": v.get("spv_path"),
                    "spv_bytes": v.get("spv_bytes"),
                    "dropped_bytes": v.get("dropped_bytes"),
                }
    with open(os.path.join(a.out, "index.json"), "w") as f:
        json.dump({"meta": meta, "_index": index}, f, indent=1, sort_keys=True)
    with open(os.path.join(a.out, "results.json"), "w") as f:
        json.dump(results, f, indent=1)

    print(json.dumps({"entries": n, "translate_ok": t_ok, "spv_ok": c_ok,
                      "per_stage": per_stage, "taxonomy": tax,
                      "key_verification": keycheck}, indent=2))


def parse_tex(s):
    out = []
    for x in s.split(","):
        if not x:
            continue
        idx, rest = x.split(":", 1)
        t, flags, hoff, bcb, boff = rest.split("/")
        out.append({"slot": int(idx), "type": int(t),
                    "shadow": flags[0] == "1", "array": flags[1] == "1",
                    "image": flags[2] == "1", "bindless": flags[3] == "1",
                    "handle_offset": int(hoff), "bindless_cbuf": int(bcb),
                    "bindless_offset": int(boff)})
    return out


if __name__ == "__main__":
    main()
