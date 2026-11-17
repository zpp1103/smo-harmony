#!/usr/bin/env python3
"""M50A: 逐 pass dump 扫描 —— 把 vk_boot.log 的 dump 索引→submission pass 映射与 PPM 统计合成一张表，
并给出「nearwhite 首次 >= 阈值」的 pass（过曝产生点候选）。
usage: python3 tools/m50a_passscan.py <dir> [<dir2> ...] [--th 0.90] [--out md]
dir 里需有 vk_boot.log 与 pass_NN.ppm
"""
import os, re, sys

def ppm_stats(p):
    with open(p, 'rb') as f:
        assert f.readline().strip() == b'P6'
        line = f.readline()
        while line.startswith(b'#'): line = f.readline()
        w, h = map(int, line.split())
        assert int(f.readline()) == 255
        d = f.read(w * h * 3)
    n = w * h
    sr = sg = sb = 0; nw = ge = 0
    for i in range(n):
        r, g, b = d[3*i], d[3*i+1], d[3*i+2]
        sr += r; sg += g; sb += b
        if r >= 250 and g >= 250 and b >= 250: nw += 1
        if r >= 240 or g >= 240 or b >= 240: ge += 1
    return w, h, (sr/n, sg/n, sb/n), 100.0*nw/n, 100.0*ge/n

def scan(d):
    log = os.path.join(d, 'vk_boot.log')
    if not os.path.exists(log): return []
    rx = re.compile(r'pass dump (\d+) is the submission\'s pass (\d+)')
    sx = re.compile(r'pass dump (\d+): (\d+)x(\d+) format (\d+)')
    pmap, smap = {}, {}
    for line in open(log, errors='replace'):
        m = rx.search(line)
        if m: pmap[int(m.group(1))] = int(m.group(2))
        m = sx.search(line)
        if m: smap[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
    rows = []
    for i in sorted(pmap):
        p = os.path.join(d, f'pass_{i:02d}.ppm')
        if not os.path.exists(p): continue
        try: w, h, mean, nw, ge = ppm_stats(p)
        except Exception: continue
        sz = smap.get(i, (w, h, -1))
        rows.append((i, pmap[i], sz[0], sz[1], sz[2], mean, nw, ge))
    return rows

if __name__ == '__main__':
    dirs = [a for a in sys.argv[1:] if not a.startswith('--')]
    th = 0.90
    if '--th' in sys.argv: th = float(sys.argv[sys.argv.index('--th')+1])
    outp = None
    if '--out' in sys.argv: outp = sys.argv[sys.argv.index('--out')+1]
    lines = []
    allrows = []
    for d in dirs:
        rows = scan(d)
        lines.append(f"## {d}  ({len(rows)} dumps)")
        lines.append("| dump | pass | size | fmt | mean R/G/B | nearwhite(all>=250) | ge240 |")
        lines.append("|---|---|---|---|---|---|---|")
        for i, p, w, h, f, mean, nw, ge in rows:
            lines.append(f"| {i} | {p} | {w}x{h} | {f} | {mean[0]:.1f}/{mean[1]:.1f}/{mean[2]:.1f} | {nw:.2f}% | {ge:.2f}% |")
        allrows += [(d,) + r for r in rows]
        first = next((r for r in rows if r[6] >= th*100), None)
        lines.append(f"- 首个 nearwhite>={th*100:.0f}% 的 pass: " + (f"dump {first[0]} = pass {first[1]} ({first[2]}x{first[3]} fmt {first[4]}, {first[6]:.2f}%)" if first else "无"))
        # 首次越过 50% 的
        first50 = next((r for r in rows if r[6] >= 50), None)
        lines.append(f"- 首个 nearwhite>=50% 的 pass: " + (f"dump {first50[0]} = pass {first50[1]} ({first50[2]}x{first50[3]} fmt {first50[4]}, {first50[6]:.2f}%)" if first50 else "无"))
        lines.append("")
    txt = "\n".join(lines)
    print(txt)
    if outp: open(outp, 'w').write(txt)
