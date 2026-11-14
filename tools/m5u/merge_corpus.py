#!/usr/bin/env python3
"""M5u: merge newly baked (key,stage) entries into the existing corpus
(index.json + spv.bin). Existing entries are preserved byte-for-byte; new
entries are appended after them. No entry is removed or rewritten.
ASCII only."""
import json, os, struct, sys, collections

MAGIC = 0x534D4F31
ENT = 28
SPV_MAGIC = 0x07230203

def load_bin(path):
    d = open(path, 'rb').read()
    magic, cnt, hdr = struct.unpack_from('<III', d, 0)
    assert magic == MAGIC, hex(magic)
    assert cnt > 0 and hdr == 12 + cnt * ENT and hdr <= len(d), (cnt, hdr, len(d))
    ents = []
    for i in range(cnt):
        k, st, by, off = struct.unpack_from('<16sIII', d, 12 + i * ENT)
        k = k.split(b'\0')[0].decode()
        assert off + by <= len(d), (k, st, off, by)
        ents.append((k, st, d[off:off + by]))
    return ents

def main():
    old_bin, old_index, new_index, new_spv_dir, out_dir = sys.argv[1:6]
    ents = load_bin(old_bin)
    print("old entries:", len(ents))
    have = {(k, st) for k, st, _ in ents}
    old_idx = json.load(open(old_index))
    new_idx = json.load(open(new_index))['_index']

    # collision check: a key with both stages cannot be represented by the app's
    # per-key R_Key table (r_key() ignores stage), so drop such keys entirely.
    key_stages = collections.defaultdict(set)
    for (k, st) in have:
        key_stages[k].add(st)
    for k, node in new_idx.items():
        for st in (0, 1, 5):
            e = node.get('stage%d' % st)
            if e and e.get('status') == 'ok':
                key_stages[k].add(st)
    bad_keys = {k for k, s in key_stages.items() if len(s) > 1}
    if bad_keys:
        print("keys with >1 stage (skipped):", len(bad_keys), sorted(bad_keys)[:5])

    added = 0
    skipped = collections.Counter()
    for k in sorted(new_idx):
        node = new_idx[k]
        if k in bad_keys:
            skipped['multi_stage'] += 1
            continue
        for st in (0, 1, 5):
            e = node.get('stage%d' % st)
            if not e or e.get('status') != 'ok':
                continue
            if (k, st) in have:
                skipped['already'] += 1
                continue
            if st == 5:
                skipped['compute'] += 1   # no compute path in this backend
                continue
            p = os.path.join(new_spv_dir, '%s.%d.spv' % (k, st))
            if not os.path.exists(p):
                skipped['missing_spv'] += 1
                continue
            b = open(p, 'rb').read()
            if len(b) % 4 or struct.unpack_from('<I', b, 0)[0] != SPV_MAGIC:
                skipped['bad_spv'] += 1
                continue
            ents.append((k, st, b))
            have.add((k, st))
            added += 1
    print("added:", added, "skipped:", dict(skipped))

    # ---- write container ----
    cnt = len(ents)
    hdr = 12 + cnt * ENT
    off = (hdr + 3) & ~3
    table = bytearray()
    body = bytearray()
    cur = off
    for k, st, b in ents:
        table += struct.pack('<16sIII', k.encode(), st, len(b), cur)
        body += b
        pad = (-len(b)) % 4
        if pad:
            body += b'\0' * pad
        cur += len(b) + pad
    os.makedirs(out_dir, exist_ok=True)
    outp = os.path.join(out_dir, 'spv.bin')
    with open(outp, 'wb') as f:
        f.write(struct.pack('<III', MAGIC, cnt, hdr))
        f.write(table)
        f.write(b'\0' * (((hdr + 3) & ~3) - hdr))
        f.write(body)
    print("wrote", outp, os.path.getsize(outp), "entries", cnt)

    # ---- merge index.json ----
    idx = old_idx['_index']
    n2 = 0
    for k in sorted(new_idx):
        if k in bad_keys or k in idx:
            continue
        node = new_idx[k]
        keep = {kk: vv for kk, vv in node.items() if kk in ('key', 'stage', 'attr_mask', 'ntex', 'cbuf_mask')}
        for st in (0, 1):
            e = node.get('stage%d' % st)
            if e and e.get('status') == 'ok' and (k, st) in have:
                keep['stage%d' % st] = e
                keep['stage'] = st
                keep['attr_mask'] = e.get('attr_mask', 0)
                keep['ntex'] = e.get('ntex', 0)
                keep['cbuf_mask'] = e.get('cbuf_mask', 0)
        if any(('stage%d' % st) in keep for st in (0, 1)):
            idx[k] = keep
            n2 += 1
    old_idx['meta'] = dict(old_idx.get('meta') or {})
    old_idx['meta']['m5u_corpus_completion'] = {
        'what': 'bake every (key,stage) present in device shaderdump but absent from corpus; '
                'source = /data/app/el2/100/base/com.vibecc.smo/files/shaderdump (15158 pairs)',
        'added_entries': added, 'added_keys': n2,
        'translator': 'tools/m3q/m3q_translate (in-tree maxwell->GLSL) with M3Q_NO_INT64=1',
        'compiler': 'glslangValidator --target-env vulkan1.3',
        'skipped': dict(skipped),
    }
    outp = os.path.join(out_dir, 'index.json')
    with open(outp, 'w') as f:
        json.dump(old_idx, f, indent=1, sort_keys=True)
    print("wrote", outp, os.path.getsize(outp), "keys", len(idx))
    print("M5U_MERGE_OK added=%d keys=%d" % (added, len(idx)))

main()
