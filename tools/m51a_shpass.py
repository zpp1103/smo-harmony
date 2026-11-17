#!/usr/bin/env python3
"""M51A: shadow 链证据抽取。
1) `shpass:` 行 → 每个 shadow 候选 pass（1024²/256²）的 draw 状态表：draw 条数、cull/front/mode/clamp/bias_en/pofs、dt/dw/df、
   viewport、scissor、depth range、深度附件 pool+off；pass END 行的清屏值 + scissor bbox。
2) `pass dump NN is the submission's pass P` + 对应 `NN.f32` 深度 dump → 该 pass 深度的远平面占比（>=0.999 / exactly 1.0 / min / mean）。
usage:
  python3 tools/m51a_shpass.py <vk_boot.log> [--dumps DIR]
"""
import re, sys, os, struct, collections

RX_DRAW = re.compile(r'shpass: present (\d+) pass (\d+) (\d+)x(\d+) draw (\d+) prim (\d+) count (\d+) inst (\d+) '
                     r'cull (\d+) front (\d+) mode (\d+) clamp (\d+) bias_en (\d+) pofs civ (\S+) fac (\S+) clamp (\S+) '
                     r'dt (\d+) dw (\d+) df (\d+) vp (\S+) (\S+) (\S+) (\S+) sc (-?\d+) (-?\d+) (-?\d+) (-?\d+) '
                     r'drange (\S+) (\S+) dep pool (0x[0-9a-f]+)\+(0x[0-9a-f]+) serial (\d+) fmt (\d+) ncol (\d+)')
RX_END = re.compile(r'shpass: pass (\d+) END (\d+)x(\d+) draws (\d+) clears (\d+) clear_depth (\S+) clear_stencil (\d+); '
                    r'scissor bbox (-?\d+) (-?\d+) (-?\d+) (-?\d+); dep pool (0x[0-9a-f]+)\+(0x[0-9a-f]+)')
RX_MAP = re.compile(r'pass dump (\d+) is the submission\'s pass (-?\d+) \(present (\d+)\)')
RX_DUMPHDR = re.compile(r'pass dump (\d+): depth (\d+)x(\d+) format (\d+), values (\S+) \.\. (\S+)')

def f32_stats(path, cutoff=0.999):
    d = open(path, 'rb').read()
    n = len(d) // 4
    if not n: return None
    v = struct.unpack('%df' % n, d)
    far = sum(1 for x in v if x >= cutoff)
    exactly1 = sum(1 for x in v if x == 1.0)
    return dict(n=n, far=100.0*far/n, one=100.0*exactly1/n, mn=min(v), mx=max(v), mean=sum(v)/n)

def main():
    log = sys.argv[1]
    dumps = None
    if '--dumps' in sys.argv: dumps = sys.argv[sys.argv.index('--dumps')+1]
    draws = collections.defaultdict(list); ends = collections.defaultdict(list)
    dmap = {}
    for line in open(log, errors='replace'):
        line = ' '.join(line.replace(';', ' ').split())
        m = RX_DRAW.search(line)
        if m:
            g = m.groups()
            draws[int(g[1])].append(dict(present=int(g[0]), w=int(g[2]), h=int(g[3]), draw=int(g[4]), prim=int(g[5]), count=int(g[6]),
                                         inst=int(g[7]), cull=int(g[8]), front=int(g[9]), mode=int(g[10]), clamp=int(g[11]), bias=int(g[12]),
                                         civ=float(g[13]), fac=float(g[14]), pclamp=float(g[15]), dt=int(g[16]), dw=int(g[17]), df=int(g[18]),
                                         vp=g[19:23], sc=tuple(int(x) for x in g[23:27]), dn=float(g[27]), dfar=float(g[28]),
                                         pool=g[29], off=g[30], serial=int(g[31]), fmt=int(g[32]), ncol=int(g[33])))
            continue
        m = RX_END.search(line)
        if m:
            g = m.groups()
            ends[int(g[0])].append(dict(w=int(g[1]), h=int(g[2]), n=int(g[3]), clears=int(g[4]), cd=g[5], cs=int(g[6]),
                                        bbox=tuple(int(x) for x in g[7:11]), pool=g[11], off=g[12]))
            continue
        m = RX_MAP.search(line)
        if m: dmap[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))

    print("## 1) shadow 候选 pass 状态表（shpass）")
    for p in sorted(draws):
        ds = draws[p]
        sizes = collections.Counter((d['w'], d['h']) for d in ds)
        sc = collections.Counter(d['sc'] for d in ds)
        cull = collections.Counter((d['cull'], d['front'], d['mode'], d['clamp']) for d in ds)
        bias = collections.Counter((d['bias'], d['civ'], d['fac']) for d in ds)
        dw = collections.Counter((d['dt'], d['dw'], d['df']) for d in ds)
        dn = collections.Counter((d['dn'], d['dfar']) for d in ds)
        deps = collections.Counter((d['pool'], d['off'], d['serial']) for d in ds)
        print(f"\n### pass {p}  sizes={dict(sizes)}  draws/frame={[e['n'] for e in ends.get(p, [])][:20]}  frames={len(ends.get(p, []))}")
        print(f"- depth targets: {list(deps.items())[:4]}")
        print(f"- scissor: {list(sc.items())[:4]}")
        print(f"- cull/front/mode/clamp: {list(cull.items())[:4]}")
        print(f"- bias_en + pofs(civ/fac): {list(bias.items())[:4]}")
        print(f"- depth test/write/func: {list(dw.items())[:3]}   depth range: {list(dn.items())[:3]}")
        print(f"- draws: {[(d['draw'], d['prim'], d['count'], d['inst']) for d in ds[:12]]}")
        for e in ends.get(p, [])[:3]:
            print(f"- END {e['w']}x{e['h']} draws {e['n']} clear_depth {e['cd']} (clears {e['clears']}) scissor bbox {e['bbox']} dep {e['pool']}+{e['off']}")

    print("\n## 2) passdump 深度（远平面占比）")
    if dumps and os.path.isdir(dumps):
        hdrs = {}
        for line in open(log, errors='replace'):
            m = RX_DUMPHDR.search(line)
            if m: hdrs[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
        rows = []
        for fn in sorted(os.listdir(dumps)):
            if not fn.endswith('.f32'): continue
            try: idx = int(re.search(r'(\d+)\.f32$', fn).group(1))
            except Exception: continue
            st = f32_stats(os.path.join(dumps, fn))
            if not st: continue
            rows.append((idx, dmap.get(idx, (None, None))[0], hdrs.get(idx), st))
        print("| dump | pass | size | n | far>=0.999 | ==1.0 | min | mean |")
        print("|---|---|---|---|---|---|---|---|")
        for idx, p, hdr, st in rows:
            sz = f"{hdr[0]}x{hdr[1]}" if hdr else "?"
            print(f"| {idx} | {p} | {sz} | {st['n']} | {st['far']:.2f}% | {st['one']:.2f}% | {st['mn']:.6g} | {st['mean']:.6g} |")
    else:
        print("(no --dumps dir given)")

if __name__ == '__main__':
    main()
