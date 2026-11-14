#!/usr/bin/env python3
"""M4j P1：从既有容器里裁掉 Int64 条目，产出瘦身 spv.bin + index.json。

用法：python3 slim_bin.py <in_spv.bin> <in_index.json> <out_dir> [--drop-bigfs=BYTES]

- 逐条扫 OpCapability(17)/id=11 → SwiftShader 永远建不出管线（shaderInt64=0），直接删。
- 索引同步：删除被裁条目的 stageN；只剩 failed/空的 key 整个删掉（无 SPIR-V 可建）。
- 输出 magic 固定 0x534D4F31（LE '1OMS'），上机前必须再跑 check_bin.py。
"""
import argparse
import collections
import json
import os
import struct
import sys

MAGIC = 0x534D4F31
SPV_MAGIC = 0x07230203
ENT = 28
STAGES = (0, 1, 5, 2, 3, 4, 6, 7)


def op_caps(data, off, n):
    """返回该 SPIR-V 声明的 OpCapability 值集合。"""
    words = n // 4
    if words < 6:
        return set()
    w = struct.unpack_from('<%dI' % words, data, off)
    caps = set()
    i = 5                                # 跳过 magic/version/generator/bound/schema
    while i < words:
        op = w[i] & 0xFFFF
        wc = w[i] >> 16
        if wc == 0:
            break
        if op == 17 and wc >= 2 and i + 1 < words:
            caps.add(w[i + 1])
        i += wc
    return caps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binp')
    ap.add_argument('idxp')
    ap.add_argument('outdir')
    ap.add_argument('--drop-bigfs', type=int, default=0,
                    help='额外裁掉 >N 字节的非 Int64 条目（0=不裁）')
    a = ap.parse_args()

    data = open(a.binp, 'rb').read()
    magic, cnt, hdr = struct.unpack_from('<III', data, 0)
    if magic != MAGIC:
        sys.exit('input bad magic 0x%08X (expect 0x534D4F31)' % magic)
    if hdr != 12 + ENT * cnt:
        sys.exit('input bad header cnt=%d hdr=%d' % (cnt, hdr))

    drop = {}                            # (key, stage) -> reason
    keep = []
    in_bytes = 0
    for i in range(cnt):
        kb, st, b, off = struct.unpack_from('<16sIII', data, 12 + i * ENT)
        key = kb.split(b'\0')[0].decode()
        in_bytes += b
        caps = op_caps(data, off, b)
        if 11 in caps:
            drop[(key, st)] = 'int64'
        elif a.drop_bigfs and b > a.drop_bigfs:
            drop[(key, st)] = 'bigfs'
        else:
            keep.append((key, st, b, off))

    # 重新打包：条目紧接表头，payload 按原顺序原字节拼接
    newhdr = 12 + ENT * len(keep)
    out = bytearray(struct.pack('<III', MAGIC, len(keep), newhdr))
    blob = bytearray()
    cur = newhdr
    for key, st, b, off in keep:
        kb = key.encode()[:16].ljust(16, b'\0')
        out += struct.pack('<16sIII', kb, st, b, cur)
        blob += data[off:off + b]
        cur += b
    out += blob
    os.makedirs(a.outdir, exist_ok=True)
    bpath = os.path.join(a.outdir, 'spv.bin')
    with open(bpath, 'wb') as f:
        f.write(bytes(out))

    # 索引同步
    idx_all = json.load(open(a.idxp))
    idx = idx_all.get('_index', idx_all)
    outidx = {}
    nkey_in = nkey_out = 0
    dropped_keys = []
    for key, v in idx.items():
        nkey_in += 1
        if not isinstance(v, dict):
            outidx[key] = v
            nkey_out += 1
            continue
        nv = dict(v)
        had_stage = False
        kept_stage = False
        for s in STAGES:
            name = 'stage%d' % s
            if name not in nv:
                continue
            had_stage = True
            st = nv[name]
            status = st.get('status') if isinstance(st, dict) else None
            if (key, s) in drop or status != 'ok':
                del nv[name]
                continue
            kept_stage = True
        if had_stage and not kept_stage:
            dropped_keys.append(key)
            continue
        outidx[key] = nv
        nkey_out += 1
    with open(os.path.join(a.outdir, 'index.json'), 'w') as f:
        json.dump({'_index': outidx}, f, separators=(',', ':'))
    with open(os.path.join(a.outdir, 'excluded.tsv'), 'w') as f:
        for (key, st), why in sorted(drop.items()):
            f.write('%s\t%d\t%s\n' % (key, st, why))

    out_bytes = sum(b for _, _, b, _ in keep)
    print('IN : entries=%d bytes=%d keys=%d' % (cnt, in_bytes, len(idx)))
    print('DROP: %d entries (%s), bytes=%d' % (
        len(drop), dict(collections.Counter(drop.values())),
        in_bytes - out_bytes))
    print('OUT: entries=%d bytes=%d keys=%d (dropped keys=%d, incl. failed-only)' % (
        len(keep), out_bytes, len(outidx), nkey_in - nkey_out))
    print('FILE: %s %d B (%.1f MB)' % (bpath, len(out), len(out) / 1048576.0))


if __name__ == '__main__':
    main()
