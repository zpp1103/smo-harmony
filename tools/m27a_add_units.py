#!/usr/bin/env python3
"""M28A 前置工具（M27A 报告 §P4-1）：给 index.json 的**键层**补 "units" 字段。

背景（M27A 已钉死，不要重新论证）：
  语料里 `binding = 32 + 稠密序号`，而 guest 真正的 NVN texture unit 是
      unit = (handle_offset - 0x20) / 8
  该字段 `_index[<key>][stageN].tex[].handle_offset` 每张图都有，运行时从来没读。
  地表那 10 个 FS 键的真实 unit 集合是 {0,1,2,3,4,9,13,22}（不含 5、6），我们按稠密序号
  去读 unit 5/6 ⇒ 永远没绑过 ⇒ 1x1 占位 ⇒ 整片平涂。

产出（**原语料只读**，绝不原地改）：
  <out>/index.json      = 原 index.json 每个键层多一个 "units":"1,9,2,22,3,0,13"（按 slot 升序，
                          第 i 个元素 = 稠密序号 i 的真实 unit），紧凑 JSON。
用法：
  python3 tools/m27a_add_units.py --src <原始 index.json> --out <工作副本 index.json> [--check key]
"""
import argparse
import json
import os
import sys

HANDLE_BASE = 0x20
HANDLE_STRIDE = 8


def units_for(entry):
    """返回 (units 列表, 说明)。取 entry['stage'] 指向的 stageN.tex[]（无则另一阶段）。"""
    st = entry.get("stage")
    order = []
    if st == 1:
        order = ["stage1", "stage0"]
    elif st == 0:
        order = ["stage0", "stage1"]
    else:
        order = ["stage1", "stage0"]
    for name in order:
        sub = entry.get(name)
        if not isinstance(sub, dict):
            continue
        tex = sub.get("tex")
        if not isinstance(tex, list):
            continue
        pairs = []
        for t in tex:
            ho = t.get("handle_offset")
            sl = t.get("slot")
            if ho is None or sl is None:
                continue
            pairs.append((int(sl), (int(ho) - HANDLE_BASE) // HANDLE_STRIDE))
        pairs.sort()
        return [u for _, u in pairs], order[0]
    return [], None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--check", default=None, help="打印该 key 的 units 供人工核对")
    a = ap.parse_args()

    with open(a.src, "r") as f:
        doc = json.load(f)
    idx = doc["_index"]

    n_keys = n_units = 0
    n_pairs = n_same = 0
    n_empty_ctx = n_empty_slots = 0
    umax = -1
    slot_gap = []
    stage_pick = {"stage1": 0, "stage0": 0, None: 0}
    for k, e in idx.items():
        n_keys += 1
        us, which = units_for(e)
        stage_pick[which] = stage_pick.get(which, 0) + 1
        if not us:
            # 无 unit 信息：写空串。两种情形必须分开计数（否则统计会骗人）：
            #   (a) ntex==0 —— 本键不采样纹理，空串合理；
            #   (b) ntex>0 但语料的 stageN.tex[] 是空数组 —— 该键**没有** handle_offset 可用，
            #       运行时只能回落到"稠密序号当 unit"的旧行为（属本轮覆盖不到的残余）。
            e["units"] = ""
            n_units += 1
            if int(e.get("ntex") or 0) > 0:
                n_empty_ctx += 1
                n_empty_slots += int(e.get("ntex") or 0)
            continue
        e["units"] = ",".join(str(u) for u in us)
        n_units += 1
        for i, u in enumerate(us):
            n_pairs += 1
            if u == i:
                n_same += 1
            if u > umax:
                umax = u
        if len(us) != int(e.get("ntex") or 0):
            slot_gap.append((k, len(us), e.get("ntex")))

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w") as f:
        json.dump(doc, f, separators=(",", ":"))

    print("[m27a] keys=%d with_units_field=%d" % (n_keys, n_units))
    print("[m27a] pairs=%d dense_eq_unit=%d (%.1f%%) mismatched=%d"
          % (n_pairs, n_same, (100.0 * n_same / n_pairs) if n_pairs else 0.0, n_pairs - n_same))
    print("[m27a] unit_max=%d stage_pick=%s" % (umax, stage_pick))
    print("[m27a] len(units)!=ntex keys=%d" % len(slot_gap))
    for k, a1, b1 in slot_gap[:8]:
        print("        %s units=%d ntex=%s" % (k, a1, b1))
    print("[m27a] keys_with_ntex_but_no_units=%d of %d (%.1f%%); uncovered (key,slot)=%d (%.1f%% of %d)"
          % (n_empty_ctx, n_keys,
             100.0 * n_empty_ctx / n_keys, n_empty_slots,
             100.0 * n_empty_slots / max(1, n_pairs + n_empty_slots), n_pairs + n_empty_slots))
    print("[m27a] src=%s (%d B) -> out=%s (%d B)"
          % (a.src, os.path.getsize(a.src), a.out, os.path.getsize(a.out)))
    if a.check:
        e = idx.get(a.check)
        print("[m27a] check %s stage=%s ntex=%s units=%r"
              % (a.check, e and e.get("stage"), e and e.get("ntex"), e and e.get("units")))
        if e:
            for name in ("stage1", "stage0"):
                sub = e.get(name) or {}
                for t in sub.get("tex", []) or []:
                    print("        %s slot=%s handle_offset=%s -> unit=%s type=%s"
                          % (name, t.get("slot"), t.get("handle_offset"),
                             (t.get("handle_offset") - HANDLE_BASE) // HANDLE_STRIDE, t.get("type")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
