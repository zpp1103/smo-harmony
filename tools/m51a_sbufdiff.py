#!/usr/bin/env python3
"""M51A: 从 vk_boot.log 里抽 type 7 (storage buffer) dump（`sbuf:` 行 + 其后每 64 B 一行 hex），
按 (pass, 尺寸, stage, binding, reg, pool+off, size) 分组，对两个 arm 窗口（[m50a] ubo pass log armed/off 分段）
做「同一 buffer 两帧」的 word 级 diff，给出字段偏移（字节偏移 = 行内 word 下标 *4）、前后值（hex + float 视图）。

usage:
  python3 tools/m51a_sbufdiff.py <vk_boot.log>                 # 全窗口两两比较（W1 vs W2）
  python3 tools/m51a_sbufdiff.py <vk_boot.log> --size 3136     # 只看 3136 B 的（实例数组）
  python3 tools/m51a_sbufdiff.py <vk_boot.log> --dump-meta     # 只列分组
"""
import re, sys, collections, struct

RX_HDR = re.compile(r'sbuf: present (\d+) pass (\d+) (\d+)x(\d+) draw (\d+) stage (\d+) b(\d+) c(-?\d+) type 7 addr (0x[0-9a-f]+) pool (0x[0-9a-f]+)\+(0x[0-9a-f]+) size (\d+) hash ([0-9a-f]+) hex (\d+)')
RX_ROW = re.compile(r'sbuf\[\+([0-9a-f]+)\] i(\d+):((?: [0-9a-f]{8})+)')

def parse(path):
    segs, cur = [], None
    pending = None
    with open(path, errors='replace') as f:
        for line in f:
            if '[m50a] ubo pass log armed' in line:
                cur = []; segs.append(cur); pending = None; continue
            if '[m50a] ubo pass log off' in line:
                cur = None; pending = None; continue
            if cur is None: continue
            m = RX_HDR.search(line)
            if m:
                pending = {'present': int(m.group(1)), 'pass': int(m.group(2)), 'w': int(m.group(3)), 'h': int(m.group(4)),
                           'draw': int(m.group(5)), 'stage': int(m.group(6)), 'bind': int(m.group(7)), 'creg': int(m.group(8)),
                           'addr': m.group(9), 'pool': m.group(10), 'off': int(m.group(11), 16), 'size': int(m.group(12)),
                           'hash': m.group(13), 'hexlen': int(m.group(14)), 'rows': {}}
                cur.append(pending); continue
            if pending is not None:
                r = RX_ROW.search(line)
                if r:
                    vals = [int(w, 16) for w in r.group(3).split()]
                    pending['rows'][int(r.group(1), 16)] = vals
    return segs

def key(e):
    return (e['pass'], e['w'], e['h'], e['stage'], e['bind'], e['reg'] if 'reg' in e else e['creg'], e['pool'], e['off'], e['size'])

def f32(w):
    return struct.unpack('<f', struct.pack('<I', w & 0xffffffff))[0]

def words(e):
    out = []
    for o in sorted(e['rows']):
        out.extend(e['rows'][o])
    return out

def diff_pair(a, b, k, maxshow=40):
    wa, wb = words(a), words(b)
    n = min(len(wa), len(wb))
    if n == 0: return None
    diffs = [(i, wa[i], wb[i]) for i in range(n) if wa[i] != wb[i]]
    lines = []
    lines.append(f"### buffer pass{a['pass']} {a['w']}x{a['h']} stage{a['stage']} b{a['bind']} c{a['creg']} pool {a['pool']}+{a['off']:#x} size {a['size']}  words {n}")
    lines.append(f"- W1: present {a['present']} draw {a['draw']} hash {a['hash']}   W2: present {b['present']} draw {b['draw']} hash {b['hash']}")
    lines.append(f"- differing words: **{len(diffs)}/{n}** ({100.0*len(diffs)/n:.1f}%)")
    for i, x, y in diffs[:maxshow]:
        off = i * 4
        inst, lane = off // 64, (off % 64) // 4
        lines.append(f"  - word {i:3d} (byte +{off:#05x}, inst {inst:2d} lane {lane:2d}): {x:#010x} → {y:#010x}   float {f32(x):.6g} → {f32(y):.6g}")
    if len(diffs) > maxshow: lines.append(f"  - … {len(diffs)-maxshow} more")
    return "\n".join(lines)

def main():
    path = sys.argv[1]
    only_size = None; meta = False; pair = (0, 1)
    if '--size' in sys.argv: only_size = int(sys.argv[sys.argv.index('--size')+1])
    if '--pair' in sys.argv:
        v = sys.argv[sys.argv.index('--pair')+1]; a, b = v.split(','); pair = (int(a), int(b))
    if '--dump-meta' in sys.argv: meta = True
    segs = parse(path)
    print(f"# segments (arm windows): {len(segs)}  sizes={[len(s) for s in segs]}")
    if len(segs) < 2:
        print("!! fewer than 2 armed windows: nothing to diff"); return
    A, B = segs[pair[0]], segs[pair[1]]
    print(f'# comparing segment {pair[0]} (red?) vs {pair[1]} (gold?)')
    ka = collections.OrderedDict(); kb = collections.OrderedDict()
    for e in A:
        if only_size and e['size'] != only_size: continue
        ka.setdefault(key(e), []).append(e)
    for e in B:
        if only_size and e['size'] != only_size: continue
        kb.setdefault(key(e), []).append(e)
    print(f"# groups: W1={len(ka)} W2={len(kb)}")
    if meta:
        for k in ka: print("W1", k, "hash", [e['hash'] for e in ka[k][:4]])
        for k in kb: print("W2", k, "hash", [e['hash'] for e in kb[k][:4]])
        return
    both = [k for k in ka if k in kb]
    print(f"# groups in both windows: {len(both)}")
    for k in both:
        ea, eb = ka[k], kb[k]
        # dominant hash per window
        ha = collections.Counter(e['hash'] for e in ea).most_common(1)[0][0]
        hb = collections.Counter(e['hash'] for e in eb).most_common(1)[0][0]
        a = next(e for e in ea if e['hash'] == ha)
        b = next(e for e in eb if e['hash'] == hb)
        if ha == hb:
            print(f"### buffer pass{a['pass']} {a['w']}x{a['h']} stage{a['stage']} b{a['bind']} pool {a['pool']}+{a['off']:#x} size {a['size']}: SAME hash {ha}")
            continue
        print(diff_pair(a, b, k))

if __name__ == '__main__':
    main()
