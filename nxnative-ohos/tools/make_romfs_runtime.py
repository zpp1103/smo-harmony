#!/usr/bin/env python3
"""make_romfs_runtime.py —— 档案驱动的 RomFS 运行时资产生成（game-work/make_romfs_runtime.py 泛化版）。

DMROMFS1（nxnative dm 工具链提取产物）→ 宿主 romfs.c 消费的 TOTKRFS1 索引 + 等大分卷。
两种容器的文件 offset 都是「镜像内绝对偏移」，无需搬数据，只做索引转换 + 分卷。

档案键（games/<titleID>/game.tsv，键表 ../games/README.md）：
  title_id          RomFS.img 自动搜索（--work 同级 *<titleID>/RomFS.img）
  romfs_volume_mib  单卷大小（MiB，缺省 1024；卷数 = ceil(镜像大小/单卷)）
  romfs_marker      归属校验（--verify-marker 时确认该路径在索引里）

用法（totk M3：17,045,762,015 B → 16 卷 @1GiB）：
  python tools/make_romfs_runtime.py --archive games/0100F2C0115B6000 \
      --romfs ../game-work/totk_0100F2C0115B6000/RomFS.img --verify-marker
输出（缺省 .build/<titleID>/romfs_parts/，大文件不入库红线）：
  runtime-index.bin + romfs.bin.000…00N（末卷短）
分卷幂等：已存在且字节数与该卷期望一致的卷跳过重写（断点续跑；不做逐卷哈希，改动镜像后请清空输出区重跑）。
romfs_config.h 由 tools/prepare_runtime.py 生成（吃本工具的 runtime-index.bin 算 INDEX_SHA256）。
"""
import argparse
import hashlib
import math
import struct
import sys
from pathlib import Path

NXN = Path(__file__).resolve().parents[1]

ENT = struct.Struct('<QQIHBB')  # DMROMFS1 entry: a,b,name_off,name_len,type,pad


def parse_game_tsv(path: Path) -> dict:
    keys = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) >= 2:
            keys[parts[0]] = parts[1]
    return keys


def parse_dmromfs1(path):
    """按 infra/nxnative/dm/host/romfs_image.h 的校验逻辑解析，返回 [(相对路径, offset, size), ...] 全局排序。"""
    size = path.stat().st_size
    with path.open('rb') as f:
        magic, ioff, isz, n = struct.unpack('<8sQQQ', f.read(32))
        if magic != b'DMROMFS1':
            raise SystemExit(f'魔数 {magic!r} 不是 DMROMFS1')
        if not 0 < n <= 0xffffffff or ioff < 32 or ioff > size or isz != size - ioff:
            raise SystemExit('DMROMFS1 头部不合法')
        f.seek(ioff)
        index = f.read(isz)
        if len(index) != isz:
            raise SystemExit('索引读取不完整')
    ents = [ENT.unpack_from(index, k * ENT.size) for k in range(n)]
    pool = isz - n * ENT.size

    def name_of(k):
        a, b, no, nl, t, pad = ents[k]
        if no + nl > pool:
            raise SystemExit(f'条目 {k} 名字越界')
        return index[n * ENT.size + no:n * ENT.size + no + nl].decode('utf-8')

    # 广度优先展开目录树（a=首孩子 b=孩子数；文件 a=offset b=size）——结构与已验证的诊断版一致
    files = []

    def walk(k, prefix):
        a, b, no, nl, t, pad = ents[k]
        if t:
            files.append((prefix, a, b))
            return
        if a + b > n:
            raise SystemExit(f'目录 {k}({prefix}) 孩子区间越界 a={a} b={b} n={n}')
        for j in range(a, a + b):
            ja, jb, jno, jnl, jt, jpad = ents[j]
            child = name_of(j)
            if jt:
                files.append((prefix + child, ja, jb))
            else:
                walk(j, prefix + child + '/')

    walk(0, '')
    files.sort(key=lambda x: x[0])
    if len(files) != sum(1 for e in ents if e[4]):
        raise SystemExit(f'文件数不吻合：走到 {len(files)}，索引说 {sum(1 for e in ents if e[4])}')
    return files, size


def sha256_file(path):
    d = hashlib.sha256()
    with path.open('rb') as f:
        while True:
            b = f.read(8 << 20)
            if not b:
                break
            d.update(b)
    return d.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--archive", required=True, help="游戏档案目录 games/<titleID>/（含 game.tsv）")
    ap.add_argument("--romfs", help="RomFS.img 路径（缺省在 --work 同级按 *<titleID>/RomFS.img 搜索）")
    ap.add_argument("--work", help="<titleID>_build 工作区根（仅供缺省搜索 RomFS.img；本工具不写 work 区）")
    ap.add_argument("--out", help="分卷输出目录（缺省 nxnative-ohos/.build/<titleID>/romfs_parts/）")
    ap.add_argument("--volume-mib", type=int, help="覆盖 romfs_volume_mib（调试用）")
    ap.add_argument("--verify-marker", action="store_true", help="确认 game.tsv 的 romfs_marker 在索引内（归属校验）")
    args = ap.parse_args()

    archive = Path(args.archive).resolve()
    if archive.name == "game.tsv":
        archive = archive.parent
    tsv = parse_game_tsv(archive / "game.tsv")
    for need in ("title_id", "romfs_volume_mib"):
        if need not in tsv:
            sys.exit(f"make_romfs_runtime: game.tsv missing key {need}")
    title_id = tsv["title_id"]
    if archive.name != title_id:
        sys.exit(f"make_romfs_runtime: 档案目录名 {archive.name} != title_id {title_id}")

    img = Path(args.romfs) if args.romfs else None
    if img is None and args.work:
        work = Path(args.work).resolve()
        for p in sorted(work.parent.glob(f"*{title_id}/RomFS.img")):
            img = p
            break
    if not img or not img.is_file():
        sys.exit("make_romfs_runtime: RomFS.img not found (--romfs 或 --work 供搜索)")

    vol_mib = args.volume_mib or int(tsv["romfs_volume_mib"])
    chunk = vol_mib * 1024 * 1024
    image_size = img.stat().st_size
    volumes = max(1, math.ceil(image_size / chunk))
    out = Path(args.out).resolve() if args.out else NXN / ".build" / title_id / "romfs_parts"
    out.mkdir(parents=True, exist_ok=True)
    print(f"[1/4] 解析 DMROMFS1 {img.name} …")
    files, _ = parse_dmromfs1(img)
    data_bytes = sum(f[2] for f in files)
    print(f"    文件 {len(files)} 个，镜像 {image_size:,} 字节，文件数据 {data_bytes:,} 字节")
    if args.verify_marker:
        marker = tsv.get("romfs_marker", "")
        if marker and marker not in {f[0] for f in files}:
            sys.exit(f"make_romfs_runtime: romfs_marker {marker} 不在镜像索引内（归属校验失败）")
        print(f"    romfs_marker {marker} in index OK")

    print('[2/4] 生成 TOTKRFS1 runtime-index.bin …')
    strings, rows = bytearray(), bytearray()
    for path_s, off, size in files:
        nb = path_s.encode() + b'\0'
        rows.extend(struct.pack('<QQII', off, size, len(strings), len(nb)))
        strings.extend(nb)
    index = struct.pack('<8sQQ', b'TOTKRFS1', len(files), 24 + len(rows)) + bytes(rows) + bytes(strings)
    (out / 'runtime-index.bin').write_bytes(index)
    index_sha = hashlib.sha256(index).hexdigest()
    print(f'    index {len(index):,} 字节 sha256={index_sha}')

    print('[3/4] 全镜像 SHA256 …')
    image_sha = sha256_file(img)
    print(f'    image sha256={image_sha}')

    print(f'[4/4] 分卷 {vol_mib} MiB × {volumes}（末卷短）→ {out} …')
    written = skipped = 0
    with img.open('rb') as f:
        k = 0
        while True:
            b = f.read(chunk)
            if not b:
                break
            vol = out / f'romfs.bin.{k:03d}'
            if vol.is_file() and vol.stat().st_size == len(b):
                skipped += 1                       # 幂等：同大小分卷跳过（断点续跑）
            else:
                vol.write_bytes(b)
                written += 1
            if (k + 1) % 4 == 0 or k + 1 == volumes:
                print(f'    卷 {k + 1}/{volumes}')
            k += 1
    print(f'    {k} 卷（重写 {written}，跳过 {skipped}）')
    print('DONE')
    print(f'parts_dir: {out}')


if __name__ == '__main__':
    main()
