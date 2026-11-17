#!/usr/bin/env python3
"""M4j：分析回读的 PAM(P7/RGBA)——distinct 近似、top-N 直方图、与指定基线色的逐像素差、导出 PNG。

用法：python3 analyze_pam.py <a.pam> [--base 0,0,0,255] [--png out.png] [--tag name]
"""
import argparse
import collections
import struct
import sys

from PIL import Image


def read_pam(path):
    with open(path, 'rb') as f:
        hdr = {}
        magic = f.readline().strip()
        if magic != b'P7':
            sys.exit('%s: not P7 (magic %r)' % (path, magic))
        while True:
            line = f.readline()
            if not line:
                sys.exit('%s: truncated header' % path)
            line = line.strip()
            if line == b'ENDHDR':
                break
            if not line or line.startswith(b'#'):
                continue
            k, _, v = line.partition(b' ')
            hdr[k.decode()] = v.decode()
        w = int(hdr['WIDTH']); h = int(hdr['HEIGHT'])
        depth = int(hdr.get('DEPTH', '4'))
        body = f.read()
    if len(body) != w * h * depth:
        sys.exit('%s: body %d != %d' % (path, len(body), w * h * depth))
    return w, h, depth, body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pam')
    ap.add_argument('--base', default='0,0,0,255')
    ap.add_argument('--png', default='')
    ap.add_argument('--tag', default='')
    a = ap.parse_args()
    w, h, depth, body = read_pam(a.pam)
    n = w * h
    base = bytes(int(x) & 0xFF for x in a.base.split(','))
    cnt = collections.Counter()
    diff = 0
    alpha_nz = 0
    for i in range(n):
        px = body[i * depth:(i + 1) * depth]
        cnt[px] += 1
        if px[:3] != base[:3]:
            diff += 1
        if depth == 4 and px[3]:
            alpha_nz += 1
    print('[%s] %s %dx%d depth=%d' % (a.tag or a.pam, a.pam, w, h, depth))
    print('  distinct=%d' % len(cnt))
    print('  diff_vs_base(%s)=%d/%d (%.3f%%)' % (a.base, diff, n, 100.0 * diff / n))
    print('  alpha_nz=%d' % alpha_nz)
    for px, c in cnt.most_common(6):
        print('  hist rgba=%s n=%d (%.3f%%)' % (tuple(px), c, 100.0 * c / n))
    if a.png:
        img = Image.frombytes('RGBA' if depth == 4 else 'RGB', (w, h), body)
        img.save(a.png)
        print('  png -> %s' % a.png)


if __name__ == '__main__':
    main()
