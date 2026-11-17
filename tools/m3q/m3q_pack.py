#!/usr/bin/env python3
"""M3w：把 index.json + out/*.spv 打成运行时容器 spv.bin，并（可选）拷进 HAP rawfile。

容器格式（必须与 nvn_vk.c r_bin_load() 逐字节一致）：
  magic "SMO1"=0x534D4F31 | u32 count | u32 hdr_size(=12+28*count)
  count × {char key[16]; u32 stage; u32 bytes; u32 off;} = 28B
  之后各 .spv 原始字节（4 字节对齐）。
"""
import argparse, json, os, struct, sys

MAGIC = 0x534D4F31


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bake", default="/tmp/m3w", help="烘焙目录（含 index.json 与 out/）")
    ap.add_argument("--rawfile", default="", help="拷贝目标目录（HAP 的 rawfile/shadercache）")
    a = ap.parse_args()

    idx = json.load(open(os.path.join(a.bake, "index.json")))["_index"]
    ents = []
    for key, node in sorted(idx.items()):
        for st in (0, 1, 5):
            e = node.get(f"stage{st}")
            if not e or e.get("status") != "ok":
                continue
            path = os.path.join(a.bake, "out", f"{key}.{st}.spv")
            if not os.path.exists(path):
                sys.exit(f"MISSING {path}")
            ents.append((key, st, path))
    if not ents:
        sys.exit("no ok entries")

    hdr = 12 + 28 * len(ents)
    off = (hdr + 3) & ~3
    body = bytearray()
    table = bytearray()
    for key, st, path in ents:
        b = open(path, "rb").read()
        assert len(b) % 4 == 0 and b[:4] == b"\x03\x02\x23\x07", (key, st, len(b))
        pad = (-off) % 4
        body += b"\0" * pad
        table += struct.pack("<16sIII", key.encode()[:16].ljust(16, b"\0"), st, len(b), off)
        body += b
        off += pad + len(b)
    out = struct.pack("<III", MAGIC, len(ents), hdr) + bytes(table) + bytes(body)

    dst = os.path.join(a.bake, "spv.bin")
    open(dst, "wb").write(out)
    print(f"packed {len(ents)} entries bytes={len(out)} hdr={hdr} -> {dst}")

    if a.rawfile:
        os.makedirs(a.rawfile, exist_ok=True)
        for name in ("index.json", "spv.bin"):
            src = os.path.join(a.bake, name)
            with open(src, "rb") as f, open(os.path.join(a.rawfile, name), "wb") as g:
                g.write(f.read())
        print(f"copied index.json + spv.bin -> {a.rawfile}")


if __name__ == "__main__":
    main()
