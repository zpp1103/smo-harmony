#!/usr/bin/env python3
"""M43A / S2a：把 m43a_opcode_cmp.tsv（运行时双流）生成 logs/m43a/opcode_cmp.md。

每一行 = 游戏的一次 draw 调用：
  · 直喂侧（d_*）：gfx_bridge.c 的 NVN 符号表指向的原函数写进**玩家命令流**的 opcode
    （platform/nvn_replay/nvn.h:125-141 的 CMD_* 枚举），随后由 nvn_vk.c 直喂出图。
  · 录制侧（r_*）：同一笔调用旁路喂给 nx2android 录制层（nvn_vk2/rec/hle_nvn.c）后，
    **从它刚写下的记录里读回来**的 opcode（nvn_vk2/abi/nvn_obj.h 的 OP_* 枚举）+ 参数。

分类规则（每条差异都必须落进一类，不留"未知"）：
  A 枚举编号差   两套枚举本来不同名不同号（CMD_* vs OP_*），只要语义对得上就归这一类
  B 语义等价     形态/参数一致，但记录的字段表达不同（如 elements 的 index buffer 地址、
                 instance 缺省 0→1 的归一化）
  C 真实缺失     直喂侧根本没把这一笔写进命令流（只记账桩），或录制侧漏记 —— 这类才是要修的
"""
import sys, collections

D_NAMES = {
    -1: "（未进命令流）",
    19: "CMD_SET_RENDER_TARGETS", 33: "CMD_CLEAR_COLOR",
    37: "CMD_DRAW_ARRAYS", 38: "CMD_DRAW_ELEMENTS",
    39: "CMD_DRAW_ARRAYS_INDIRECT", 40: "CMD_DRAW_ELEMENTS_INDIRECT",
}
R_NAMES = {25: "OP_DRAW", 26: "OP_DRAW_IDX", -1: "（未录到）"}
OP_DRAW, OP_DRAW_IDX = 25, 26


def read_tsv(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        head = f.readline().rstrip("\n").split("\t")
        for ln in f:
            p = ln.rstrip("\n").split("\t")
            if len(p) != len(head):
                continue
            r = dict(zip(head, p))
            for k in head:
                if k == "d_kind":
                    continue
                try:
                    r[k] = int(r[k])
                except ValueError:
                    r[k] = 0
            rows.append(r)
    return rows


def classify(r):
    """返回 (verdict, category, explanation)。verdict: 一致 / 等价 / 差异。"""
    kind = r["d_kind"]
    d_op, r_op = r["d_op"], r["r_op"]
    r_ok = r["r_ok"]

    if not r_ok:
        return ("差异", "C 真实缺失",
                f"录制侧没写出记录（r_op={r_op}）；swnvn 查表或录制缓冲异常，属实现缺陷")

    if kind.endswith("_unrecorded"):
        return ("差异", "C 真实缺失",
                f"直喂侧 opcode = {D_NAMES.get(d_op, d_op)}：gfx_bridge.c 里这条路是"
                f"『只记账、不写命令流』的桩（M4ad P2① 已知盲区），几何被丢；"
                f"录制侧记了 {R_NAMES.get(r_op, r_op)}（参数 prim={r['r_prim']} count={r['r_count']}）")

    # 正常路：两侧参数应逐字段相等
    if kind == "arrays":
        same = (r["r_prim"], r["r_first"], r["r_count"]) == (r["d_prim"], r["d_first"], r["d_count"])
        if r_op == OP_DRAW and same:
            return ("一致", "A 枚举编号差",
                    f"同一笔 DrawArrays：直喂写 CMD_DRAW_ARRAYS({d_op})，录制写 OP_DRAW({r_op})；"
                    "两套枚举各自编号，语义同一")
        return ("差异", "B 语义等价" if r_op == OP_DRAW else "C 真实缺失",
                f"DrawArrays 参数不一致：直喂 (prim={r['d_prim']},first={r['d_first']},count={r['d_count']}) "
                f"vs 录制 (prim={r['r_prim']},first={r['r_first']},count={r['r_count']})")

    if kind == "arrays_inst":
        if r_op == OP_DRAW and (r["r_prim"], r["r_first"], r["r_count"], r["r_inst"], r["r_base_inst"]) == \
                (r["d_prim"], r["d_first"], r["d_count"], r["d_inst"], r["d_base_inst"]):
            return ("一致", "A 枚举编号差",
                    f"同一笔 DrawArraysInstanced：直喂 CMD_DRAW_ARRAYS({d_op})（同一个 opcode，靠记录体区分 instanced），"
                    f"录制 OP_DRAW({r_op}) 带 instances/base_instance 字段")
        return ("差异", "C 真实缺失",
                f"instanced 参数不一致：直喂 (inst={r['d_inst']},bi={r['d_base_inst']}) vs 录制 (inst={r['r_inst']},bi={r['r_base_inst']})")

    if kind in ("elements", "elements_bv", "elements_inst"):
        if r_op == OP_DRAW_IDX and (r["r_prim"], r["r_count"], r["r_itype"]) == (r["d_prim"], r["d_count"], r["d_itype"]) \
                and r["r_inst"] == r["d_inst"] and r["r_base_inst"] == r["d_base_inst"]:
            return ("一致", "A 枚举编号差" if kind == "elements" else "B 语义等价",
                    f"同一笔 {kind}：直喂 CMD_DRAW_ELEMENTS({d_op})，录制 OP_DRAW_IDX({r_op})；"
                    + ("逐字段一致" if kind == "elements" else
                       f"base_vertex/base_instance 由直喂侧记录体携带（本次 base_inst={r['d_base_inst']}）"))
        return ("差异", "C 真实缺失",
                f"{kind} 参数不一致：直喂 (prim={r['d_prim']},count={r['d_count']},itype={r['d_itype']}) "
                f"vs 录制 (prim={r['r_prim']},count={r['r_count']},itype={r['r_itype']})")

    return ("差异", "C 真实缺失", f"未分类形态 {kind}（这是脚本漏洞，必须补规则，不许留未知）")


def main():
    tsv, out = sys.argv[1], sys.argv[2]
    rows = read_tsv(tsv)
    cat = collections.Counter()
    verdicts = collections.Counter()
    frames = sorted({r["frame"] for r in rows})
    lines = []
    lines.append("# M43A / S2a 直喂 ↔ 录制 opcode 逐条对账\n")
    lines.append("运行时来源：`m43a_opcode_cmp.tsv`（模拟器 127.0.0.1:5555 单次 boot 的前 200 笔 draw，"
                 "旁路录制，执行路径未切换）。\n")
    lines.append(f"- 行数：**{len(rows)}**；覆盖帧号 `g_beat_frames` = {frames[0]}…{frames[-1]}（{len(frames)} 帧）\n"
                 "- 直喂 opcode = `platform/nvn_replay/nvn.h` 的 CMD_*（`cmdbuf.c:752/775/782/792/796/797` 写入，随后 nvn_vk.c 直喂）\n"
                 "- 录制 opcode = `nvn_vk2/abi/nvn_obj.h` 的 OP_*（`rec/hle_nvn.c` 写进本单私有 arena，**从记录里读回**，非推导）\n")
    lines.append("\n## 分类小计\n")
    for r in rows:
        v, c, _ = classify(r)
        verdicts[v] += 1
        cat[c] += 1
    lines.append("| 分类 | 条数 |\n|---|---|")
    for k in sorted(cat):
        lines.append(f"| {k} | {cat[k]} |")
    lines.append(f"| **合计** | **{sum(cat.values())}** |")
    lines.append("\n判定小计：" + "，".join(f"{k} {v}" for k, v in verdicts.items()) + "\n")

    lines.append("\n## 逐条对齐表（前 200 笔）\n")
    lines.append("| # | 帧 | 形态 | 直喂 opcode | 直喂参数 (prim/first/count/inst/base_inst/itype) | "
                 "录制 opcode | 录制参数 (prim/first/count/inst/base_inst/itype) | 判定 | 分类 | 解释 |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        v, c, why = classify(r)
        dp = f"{r['d_prim']}/{r['d_first']}/{r['d_count']}/{r['d_inst']}/{r['d_base_inst']}/{r['d_itype']}"
        rp = f"{r['r_prim']}/{r['r_first']}/{r['r_count']}/{r['r_inst']}/{r['r_base_inst']}/{r['r_itype']}"
        lines.append(f"| {r['seq']} | {r['frame']} | {r['d_kind']} | {D_NAMES.get(r['d_op'], r['d_op'])} "
                     f"| {dp} | {R_NAMES.get(r['r_op'], r['r_op'])} | {rp} | {v} | {c} | {why} |")

    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"rows={len(rows)} cat={dict(cat)} verdicts={dict(verdicts)} -> {out}")


if __name__ == "__main__":
    main()
