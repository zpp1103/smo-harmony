#!/usr/bin/env python3
"""M42F 判据③：相邻截图 diff8%。

定义（本单口径，写死避免歧义）：
  diff8%(a,b) = 100 × #{px | max_c |a_c - b_c| ≥ 8} / #px
  "真实帧"     = 非纯色图（**5bit 量化后**主色占比 dom5 < 98%，且 uniq_q5 ≥ 4）
                 —— 用量化占比而不是原始占比：≤200 KB 的 JPEG 压缩噪声会把纯色图
                 撑出上百个原始色（M42E 基线实测 uniq_q5=156 但全是同一桶）。
判据：相邻**真实帧**对里 diff8% ≥ 2% 的对数 ≥3，且分母（相邻真实帧对）≥5。

用法:
  python3 tools/m42f_diff8.py <截图目录> [--out <markdown 路径>]
输出：markdown 表格 + 末行汇总（real_pairs / real_pairs_with_diff8>=2% / all_pairs）。
"""
import sys, os, glob
import numpy as np
from PIL import Image

DIFF_LEVEL = 8          # 每通道阈值
TARGET = 2.0            # 判据阈值 %
DOMSHARE_PURE = 98.0    # 纯色图门槛（**5bit 量化后**主色占比）


def stats(path):
    im = Image.open(path).convert('RGB')
    a = np.asarray(im, dtype=np.uint8)
    packed = (a[:, :, 0].astype(np.uint32) << 16) | (a[:, :, 1].astype(np.uint32) << 8) | a[:, :, 2].astype(np.uint32)
    uniq, cnt = np.unique(packed.ravel(), return_counts=True)
    dom = 100.0 * cnt.max() / packed.size
    q = (a[:, :, 0] >> 3).astype(np.uint32) << 10 | (a[:, :, 1] >> 3).astype(np.uint32) << 5 | (a[:, :, 2] >> 3).astype(np.uint32)
    uq, ucnt = np.unique(q, return_counts=True)
    dom5 = 100.0 * ucnt.max() / q.size
    return a, uniq.size, uq.size, dom, dom5


def diff8(a, b):
    n = min(a.shape[0], b.shape[0])
    w = min(a.shape[1], b.shape[1])
    d = np.abs(a[:n, :w].astype(np.int16) - b[:n, :w].astype(np.int16)).max(axis=2)
    return 100.0 * float((d >= DIFF_LEVEL).sum()) / float(d.size)


def main():
    d = sys.argv[1]
    out = None
    if '--out' in sys.argv:
        out = sys.argv[sys.argv.index('--out') + 1]
    files = sorted(glob.glob(os.path.join(d, 's_*.jpeg')) + glob.glob(os.path.join(d, 's_*.png')))
    if not files:
        print('no screenshots in', d)
        return 2
    rows, prev, prevf = [], None, None
    real_pairs = hit = 0
    all_pairs = 0
    for f in files:
        a, uniq, uq5, dom, dom5 = stats(f)
        tag = os.path.basename(f).replace('s_', '').rsplit('.', 1)[0]
        if prev is not None:
            d8 = diff8(prev, a)
            rp = 1 if (uq5 >= 4 and dom5 < DOMSHARE_PURE and prev_uq5 >= 4 and prev_dom5 < DOMSHARE_PURE) else 0
            all_pairs += 1
            if rp:
                real_pairs += 1
                if d8 >= TARGET:
                    hit += 1
            rows.append((prevf + '->' + tag, d8, rp, uniq, prev_uniq, dom5, prev_dom5))
        prev, prevf, prev_uq5, prev_dom5, prev_uniq = a, tag, uq5, dom5, uniq
    lines = ['# 相邻截图 diff8%%（阈值：任一通道 |Δ|≥%d；real_pair = 两边 uniq_q5≥4 且 dom5<%.1f%%）' % (DIFF_LEVEL, DOMSHARE_PURE),
             '', '| pair(t) | diff8% | real_pair | uniq a/b | dom5 a/b |', '|---|---|---|---|---|']
    for name, d8, rp, ua, ub, da, db in rows:
        lines.append('| %s | %.2f | %s | %d/%d | %.2f/%.2f |' % (name, d8, 'yes' if rp else 'no', ua, ub, db, da))
    lines += ['',
              'real_pairs=%d (分母)  real_pairs_with_diff8>=%.0f%%=%d' % (real_pairs, TARGET, hit),
              'real diff8%% list: ' + ', '.join('%s:%.2f' % (r[0], r[1]) for r in rows if r[2]),
              'all_pairs=%d  all_with_diff8>=%.0f%%=%d' % (all_pairs, TARGET, sum(1 for r in rows if r[1] >= TARGET))]
    txt = '\n'.join(lines) + '\n'
    if out:
        open(out, 'w', encoding='utf-8').write(txt)
    print(txt)
    return 0


if __name__ == '__main__':
    sys.exit(main())
