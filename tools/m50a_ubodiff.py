#!/usr/bin/env python3
"""M50A: 从 vk_boot.log 里抽 M50A 的 pass 级 UBO dump（[m50a] ubo pass log armed/off 分段），
把两段窗口（W1=红帧 / W2=金帧）按 draw 序号 + (stage,c) 对齐做 diff，输出变化字段的字节偏移。
usage: python3 tools/m50a_ubodiff.py <vk_boot.log> [--out md] [--pass 0]
"""
import re, sys, collections

def parse(path):
    segs, cur, idx = [], None, 0
    rx = re.compile(r'ubo: present (\d+) pass (\d+) stage (\d+) c(\d+) (0x[0-9a-f]+) \+(\d+) hash ([0-9a-f]+): (.*)$')
    with open(path, 'r', errors='replace') as f:
        for line in f:
            if '[m50a] ubo pass log armed' in line:
                cur = []; segs.append(cur); idx = 0; continue
            if '[m50a] ubo pass log off' in line:
                cur = None; continue
            if cur is None: continue
            m = rx.search(line)
            if not m: continue
            floats = []
            body = m.group(8)
            for tok in re.split(r'[|\s]+', body.strip()):
                if not tok: continue
                try: floats.append(float(tok))
                except ValueError: floats.append(tok)
            cur.append({'stage': int(m.group(3)), 'creg': int(m.group(4)), 'addr': m.group(5),
                        'off': int(m.group(6)), 'hash': m.group(7), 'v': floats, 'i': idx})
            idx += 1
    return segs

def key(e):
    return (e['stage'], e['creg'])

def diff_seg(a, b, maxdiff=24):
    """a,b: list of entries in draw order. 按 (stage,creg) 分组、各自保留出现顺序，逐条比。"""
    ga, gb = collections.defaultdict(list), collections.defaultdict(list)
    for e in a: ga[key(e)].append(e)
    for e in b: gb[key(e)].append(e)
    out = []
    for k in sorted(set(ga) | set(gb)):
        la, lb = ga[k], gb[k]
        for i in range(min(len(la), len(lb))):
            ea, eb = la[i], lb[i]
            n = min(len(ea['v']), len(eb['v']))
            diffs = []
            for j in range(n):
                if ea['v'][j] != eb['v'][j]:
                    diffs.append((j, ea['v'][j], eb['v'][j]))
            if diffs:
                out.append({'stage': k[0], 'creg': k[1], 'n': i, 'ndiff': len(diffs),
                            'diffs': diffs[:maxdiff], 'size': len(ea['v']),
                            'hash_a': ea['hash'], 'hash_b': eb['hash']})
    return out

if __name__ == '__main__':
    path = sys.argv[1]
    outp = None
    if '--out' in sys.argv: outp = sys.argv[sys.argv.index('--out') + 1]
    segs = parse(path)
    print(f"# segments: {len(segs)}  lines: {[len(s) for s in segs]}")
    if len(segs) < 2:
        print("!! 不足两段窗口，无法 diff"); raise SystemExit(1)
    W1, W2 = segs[0], segs[1]
    print(f"W1(红帧) entries={len(W1)} W2(金帧) entries={len(W2)}")
    res = diff_seg(W1, W2)
    lines = []
    lines.append(f"W1 entries={len(W1)} W2 entries={len(W2)} 变化条目={len(res)}\n")
    for r in res:
        lines.append(f"- stage {r['stage']} c{r['creg']} 第{r['n']}个 UBO (hash {r['hash_a']}→{r['hash_b']}, {r['ndiff']} 个字段变):")
        for j, a, b in r['diffs']:
            lines.append(f"    field[{j}] (+{j*4} B)  W1={a:g}  →  W2={b:g}")
    txt = "\n".join(lines)
    print(txt)
    if outp:
        open(outp, 'w').write(txt)
