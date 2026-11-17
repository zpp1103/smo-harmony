#!/usr/bin/env python3
# M51A：解析 vk_boot.log 里 M51A 新加的 type-7 dump（sbuf: ... + sbuf[+off] 行），
# 按 (pool,off,size) 分组，对同一 buffer 的**前后两次 dump** 做字段级 diff（每 64 B 一个实例 = 16 个 u32 字段）。
# usage: python3 m51a_sbufdiff.py <vk_boot.log> [out.md]
import re, sys, struct, collections

HDR = re.compile(r'sbuf: present (\d+) pass (-?\d+) draw (\d+) stage (\d+) b(\d+) c(-?\d+) type 7 addr (0x[0-9a-f]+) '
                 r'pool (0x[0-9a-f]+)\+(0x[0-9a-f]+) size (\d+) hash ([0-9a-f]{16}) hex (\d+)')
ROW = re.compile(r'sbuf\[\+([0-9a-f]{4})\] i(\d+):((?: [0-9a-f]{8})+)\s*$')

def f2i(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]

def parse(path):
    dumps = []   # list of dict
    cur = None
    with open(path, 'rb') as f:
        for raw in f:
            try: line = raw.decode('utf-8', 'replace')
            except Exception: continue
            m = HDR.search(line)
            if m:
                cur = dict(present=int(m.group(1)), passn=int(m.group(2)), draw=int(m.group(3)), stage=int(m.group(4)),
                           binding=int(m.group(5)), reg=int(m.group(6)), addr=m.group(7), pool=m.group(8), off=m.group(9),
                           size=int(m.group(10)), hash=m.group(11), nhex=int(m.group(12)), bytes=bytearray())
                dumps.append(cur); continue
            if cur is not None:
                r = ROW.search(line)
                if r:
                    o = int(r.group(1), 16)
                    words = [int(w, 16) for w in r.group(3).split()]
                    if len(cur['bytes']) < o: cur['bytes'].extend(b'\0' * (o - len(cur['bytes'])))
                    for i, w in enumerate(words):
                        p = o + 4 * i
                        if p + 4 > len(cur['bytes']): cur['bytes'].extend(b'\0' * (p + 4 - len(cur['bytes'])))
                        cur['bytes'][p:p+4] = struct.pack('<I', w)
                    continue
                if line.startswith('sbuf[') or 'sbuf:' in line: continue
                cur = None
    return dumps

def u32s(b):
    n = len(b) // 4
    return list(struct.unpack('<%dI' % n, b[:4*n]))

def main():
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    dumps = parse(path)
    L = []
    L.append("# M51A / type 7 (storage buffer) dump 汇总\n")
    L.append("总 dump 数 = %d\n" % len(dumps))
    key = lambda d: (d['pool'], d['off'], d['size'])
    groups = collections.OrderedDict()
    for d in dumps: groups.setdefault(key(d), []).append(d)
    L.append("\n| # | pool+off | size | dumps | presents | pass | binding/reg | stage | hashes(去重) |")
    L.append("|---|---|---|---|---|---|---|---|---|")
    for i, (k, ds) in enumerate(groups.items(), 1):
        hs = sorted({d['hash'] for d in ds})
        L.append("| %d | %s+%s | %d | %d | %s | %s | b%d/c%d | %s | %s |" % (
            i, k[0], k[1], k[2], len(ds), ",".join(str(d['present']) for d in ds[:6]) + ("…" if len(ds) > 6 else ""),
            ",".join(str(d['passn']) for d in sorted({d['passn'] for d in ds})), ds[0]['binding'], ds[0]['reg'],
            ",".join(str(s) for s in sorted({d['stage'] for d in ds})), ", ".join(h[:8] for h in hs[:4]) + ("…" if len(hs) > 4 else "")))
    L.append("")
    # 关键：对每个 buffer，第一次 dump vs 最后一次 dump 的字段级 diff
    for i, (k, ds) in enumerate(groups.items(), 1):
        a = ds[0]; b = ds[-1]
        wa = u32s(a['bytes']); wb = u32s(b['bytes'])
        n = min(len(wa), len(wb))
        L.append("\n## buffer #%d  pool %s+%s  size %d" % (i, k[0], k[1], k[2]))
        L.append("- dump 次数 %d；first: present %d pass %d draw %d hash %s" % (len(ds), a['present'], a['passn'], a['draw'], a['hash']))
        L.append("- last : present %d pass %d draw %d hash %s" % (b['present'], b['passn'], b['draw'], b['hash']))
        if len(ds) > 1:
            same = sum(1 for d in ds if bytes(d['bytes']) == bytes(a['bytes']))
            L.append("- 与 first 逐字节相同的 dump 数 = %d / %d" % (same, len(ds)))
        if a['hash'] == b['hash']:
            L.append("- **两次 dump 内容完全相同（hash 一致）⇒ 该 buffer 里没有字段在变**")
            continue
        inst = 64
        nfield = 0
        rows = []
        n_inst = k[2] // inst if k[2] else n * 4 // inst
        for ii in range(n_inst):
            for f in range(16):
                idx = ii * 16 + f
                if idx >= n: break
                if wa[idx] != wb[idx]:
                    nfield += 1
                    rows.append((ii, f * 4, idx, wa[idx], wb[idx]))
        L.append("- 变化字段数 = %d / %d（%d 个实例 × 16 字段）" % (nfield, n_inst * 16, n_inst))
        if rows:
            L.append("\n| 实例 | 字段偏移 | word# | first | last | first(float) | last(float) |")
            L.append("|---|---|---|---|---|---|---|")
            for ii, off, idx, ua, ub in rows[:200]:
                L.append("| %d | +0x%02x | %d | 0x%08x | 0x%08x | %g | %g |" % (ii, off, idx, ua, ub, f2i(ua), f2i(ub)))
            if len(rows) > 200: L.append("| … | | | | | | （共 %d 行，前 200 行） |" % len(rows))
        # 常量字段概览（跨实例补零/异常值）
        if n >= 16:
            zerosets = [f for f in range(16) if all(wa[ii*16+f] == 0 for ii in range(min(n_inst, n//16)))]
            L.append("- 全部实例都为 0 的字段偏移: %s" % (", ".join("+0x%02x" % (f*4) for f in zerosets) if zerosets else "（无）"))
    txt = "\n".join(L) + "\n"
    if out:
        open(out, 'w').write(txt); print("wrote", out, len(txt), "bytes")
    else:
        print(txt)

if __name__ == '__main__':
    main()
