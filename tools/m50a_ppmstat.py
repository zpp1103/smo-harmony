#!/usr/bin/env python3
"""M50A: PPM(P6) 统计 —— 通道均值 / nearwhite(all>=250) / ge240 / R>G+B 比例。用于 before/after 量化。
usage: python3 tools/m50a_ppmstat.py a.ppm [b.ppm ...]   (每行一帧)
"""
import sys

def stats(p):
    with open(p, 'rb') as f:
        assert f.readline().strip() == b'P6', p
        line = f.readline()
        while line.startswith(b'#'):
            line = f.readline()
        w, h = map(int, line.split())
        assert int(f.readline()) == 255
        d = f.read(w * h * 3)
    n = w * h
    sr = sg = sb = 0
    nw = ge240 = red_le = 0
    for i in range(n):
        r, g, b = d[3 * i], d[3 * i + 1], d[3 * i + 2]
        sr += r; sg += g; sb += b
        if r >= 250 and g >= 250 and b >= 250: nw += 1
        if r >= 240 or g >= 240 or b >= 240: ge240 += 1
        if r > g + b: red_le += 1
    return w, h, (sr / n, sg / n, sb / n), 100.0 * nw / n, 100.0 * ge240 / n, 100.0 * red_le / n

for p in sys.argv[1:]:
    w, h, (r, g, b), nw, g240, rr = stats(p)
    print(f"{p}\t{w}x{h}\tmean={r:.1f}/{g:.1f}/{b:.1f}\tnearwhite(all>=250)={nw:.2f}%\tge240={g240:.2f}%\tR>G+B={rr:.2f}%")
