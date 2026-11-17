#!/usr/bin/env python3
"""M4j P0：打包自检门禁 —— spv.bin + index.json 上机前必过。

用法：python3 check_bin.py <spv.bin> <index.json>
退出码 0=过，1=不过（任何一个字断言不成立即失败）。

容器格式（与 nvn_vk.c:2139 一致）：
  magic '1OMS' LE 0x534D4F31 | u32 count | u32 hdr_size(=12+28*count)
  count × { char key[16]; u32 stage; u32 bytes; u32 off; }   // 28B，spv 紧随其后
"""
import json
import struct
import sys

MAGIC = 0x534D4F31           # LE 字节 '1OMS'（写反成 'SMO1' 曾在 M4i 烧掉一次 boot）
SPV_MAGIC = 0x07230203
ENT = 28
HDR0 = 12

errors = []
warns = []


def fail(msg):
    errors.append(msg)


def main(binp, idxp):
    data = open(binp, 'rb').read()
    size = len(data)
    print('bin   = %s' % binp)
    print('index = %s' % idxp)
    print('file  = %d B (%.1f MB)' % (size, size / 1048576.0))

    if size < HDR0:
        fail('文件小于 12 字节')
        return finish()

    raw_magic = data[:4]
    magic, cnt, hdr = struct.unpack('<III', data[:12])
    print('magic = 0x%08X (bytes %r)' % (magic, raw_magic))
    if magic != MAGIC:
        fail('magic 不是 0x534D4F31（应为字节 \'1OMS\'，实际 %r%s）'
             % (raw_magic, ' —— 写反成了 SMO1' if raw_magic == b'SMO1' else ''))
        return finish()

    if cnt == 0 or cnt > 65536:
        fail('count=%d 越界（1..65536）' % cnt)
    if hdr != HDR0 + ENT * cnt:
        fail('hdr_size=%d != 12+28*count=%d' % (hdr, HDR0 + ENT * cnt))
    if hdr > size:
        fail('hdr_size=%d > 文件 %d' % (hdr, size))
    print('count = %d' % cnt)
    print('hdr   = %d' % hdr)
    print('payload = %d B (%.1f MB)' % (size - hdr, (size - hdr) / 1048576.0))
    if errors:
        return finish()

    seen = set()
    prefix15 = {}
    badspv = []
    expect_off = hdr
    binset = set()
    total_bytes = 0
    maxb = 0
    for i in range(cnt):
        p = HDR0 + i * ENT
        kb, st, b, off = struct.unpack('<16sIII', data[p:p + ENT])
        key = kb.split(b'\0')[0].decode('ascii', 'replace')
        if len(key) == 0:
            fail('entry#%d 空 key' % i)
        if len(key) > 16:
            fail('entry#%d key 长度 %d > 16' % (i, len(key)))
        if any(c not in '0123456789abcdef' for c in key):
            fail('entry#%d key 非 16 进制小写：%r' % (i, key))
        if (key, st) in seen:
            fail('entry#%d 重复 (key=%s,stage=%d)' % (i, key, st))
        seen.add((key, st))
        # 读取端 strncmp(key, key, 15)：只用前 15 字符匹配，前 15 字符相同即歧义
        p15 = (key[:15], st)
        if p15 in prefix15:
            warn('key 前 15 字符冲突 (stage=%d)：%s vs %s（读取端会取到错模块）'
                 % (st, prefix15[p15], key))
        prefix15[p15] = key
        binset.add((key, st))
        if b < 20 or b % 4:
            fail('entry#%d (key=%s,stage=%d) bytes=%d 不合法（<20 或非 4 对齐）' % (i, key, st, b))
        if off != expect_off:
            fail('entry#%d (key=%s,stage=%d) off=%d != 期望 %d（载荷不连续）' % (i, key, st, off, expect_off))
        if off + b > size:
            fail('entry#%d (key=%s,stage=%d) 越界 off=%d bytes=%d' % (i, key, st, off, b))
            expect_off = off + b
            continue
        if struct.unpack('<I', data[off:off + 4])[0] != SPV_MAGIC:
            badspv.append((key, st, '0x%08X' % struct.unpack('<I', data[off:off + 4])[0]))
        expect_off = off + b
        total_bytes += b
        maxb = max(maxb, b)
    if badspv:
        fail('%d 条 SPIR-V magic 不是 0x07230203，例：%s' % (len(badspv), badspv[:5]))
    if expect_off != size:
        fail('载荷结束于 %d != 文件 %d（尾部多 %d 字节 / 截断 %d 字节）'
             % (expect_off, size, size - expect_off, expect_off - size))
    print('payload_sum = %d B, max_entry = %d B' % (total_bytes, maxb))

    # index ↔ bin 双向键差（只算 status=ok；failed 不入容器）
    try:
        idx = json.load(open(idxp))
        idx = idx.get('_index', idx)
    except Exception as ex:
        fail('index.json 解析失败: %s' % ex)
        return finish()
    idxset = set()
    failed = 0
    for key, v in idx.items():
        if not isinstance(v, dict):
            continue
        for s in range(8):
            name = 'stage%d' % s
            if name not in v:
                continue
            st = v[name]
            status = st.get('status') if isinstance(st, dict) else None
            if status == 'ok':
                idxset.add((key, s))
            else:
                failed += 1
    only_bin = binset - idxset
    only_idx = idxset - binset
    print('index keys=%d, ok stages=%d, failed(不计)=%d' % (len(idx), len(idxset), failed))
    if only_bin:
        fail('bin 有 index 无：%d 条，例 %s' % (len(only_bin), sorted(only_bin)[:5]))
    if only_idx:
        fail('index 有 bin 无：%d 条，例 %s' % (len(only_idx), sorted(only_idx)[:5]))
    print('键差 bin-index=%d index-bin=%d' % (len(only_bin), len(only_idx)))
    return finish()


def finish():
    if warns:
        for w in warns[:10]:
            print('WARN: ' + w)
        if len(warns) > 10:
            print('WARN: ... 共 %d 条' % len(warns))
    if errors:
        print('\n'.join('FAIL: ' + e for e in errors))
        print('GATE: FAIL (%d 项)' % len(errors))
        return 1
    print('GATE: PASS')
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
