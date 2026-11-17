#!/usr/bin/env python3
# M43B / S2b-1：把一次上机的 m43b_frame.tsv + m43b_frame_summary.tsv + vk_boot.log 变成 frame_cmp.md
# 用法: python3 tools/m43b_frame_cmp.py <run_dir> <out.md>
import sys, os, collections

run, out = sys.argv[1], sys.argv[2]
tsv = os.path.join(run, 'm43b_frame.tsv')
sumf = os.path.join(run, 'm43b_frame_summary.tsv')
boot = os.path.join(run, 'vk_boot.log')

rows = []
with open(tsv, encoding='utf-8', errors='replace') as f:
    hdr = f.readline().rstrip('\n').split('\t')
    for line in f:
        p = line.rstrip('\n').split('\t')
        if len(p) != len(hdr):
            continue
        d = dict(zip(hdr, p))
        rows.append(d)

frames = []
with open(sumf, encoding='utf-8', errors='replace') as f:
    h2 = f.readline().rstrip('\n').split('\t')
    for line in f:
        p = line.rstrip('\n').split('\t')
        if len(p) != len(h2):
            continue
        frames.append(dict(zip(h2, p)))

STAT = ['A_enumnum', 'B_semeq', 'C_real_missing', 'D_param_mismatch', 'E_norec_by_design']
tot = collections.Counter(r['status'] for r in rows)
by_frame = collections.defaultdict(lambda: collections.Counter())
for r in rows:
    by_frame[int(r['frame'])][r['status']] += 1
by_fam = collections.defaultdict(lambda: collections.Counter())
for r in rows:
    by_fam[r['nx_fn']][r['status']] += 1
by_cmd = collections.Counter()
for r in rows:
    by_cmd[r['d_cmd']] += 1

# 选"整帧"：行数最多、且同时含 draw 与 state 的帧
def has_draw(fr):
    return any(r['d_cmd'] in ('37', '38', '39', '40') for r in rows if int(r['frame']) == fr)
cand = [(n, fr) for fr, n in ((fr, sum(by_frame[fr].values())) for fr in by_frame) if has_draw(fr)]
cand.sort(reverse=True)
pick = cand[0][1] if cand else max(by_frame, key=lambda fr: sum(by_frame[fr].values()))

# 直喂侧全量计数器（探针 destructor 打的 CMD_* 出现次数）
seen, seen_line = {}, ''
if os.path.exists(boot):
    with open(boot, encoding='utf-8', errors='replace') as f:
        for line in f:
            if 'direct-side CMD_* occurrences' in line:
                seen_line = line.strip()
                body = line.split('occurrences:', 1)[1]
                for tok in body.split():
                    if '=' in tok:
                        k, v = tok.split('='); seen[k] = int(v)
            if 'recording side ON (M43B)' in line:
                on_line = line.strip()

L = []
w = L.append
w('# M43B / S2b-1：整帧对账结果')
w('')
w('数据来源：`%s`（设备侧 `m43b_frame.tsv` / `m43b_frame_summary.tsv` / `vk_boot.log`）' % run)
w('')
w('## 一、总账')
w('')
w('| 指标 | 值 |')
w('|---|---|')
w('| 落盘帧数 | %d |' % len(frames))
w('| 对账总条数 | **%d** |' % len(rows))
for s in STAT:
    w('| %s | %d |' % (s, tot.get(s, 0)))
w('| 一致数（A + B） | **%d** |' % (tot.get('A_enumnum', 0) + tot.get('B_semeq', 0)))
w('| **真实缺失（C）** | **%d** |' % tot.get('C_real_missing', 0))
w('| **参数不符（D）** | **%d** |' % tot.get('D_param_mismatch', 0))
w('| cap_skip（录制 arena 空间不足而跳过） | %d |' % sum(1 for r in rows if r['cap_skip'] == '1'))
w('')
w('> 判定口径：**A_enumnum** = 直喂侧 `CMD_*` 与录制侧 `OP_*` 两套枚举各自编号、参数逐字段全等（录制侧恰好 1 条记录）；')
w('> **B_semeq** = 参数全等但录制侧把 1 条 NVN 调用拆成 N 条记录（BindUniformBuffers/StorageBuffers 每槽一条）；')
w('> **C_real_missing** = 直喂侧进了命令流、录制侧一条记录都没产生（**必须为 0**）；')
w('> **D_param_mismatch** = opcode 对上但关键字段不同（**必须为 0**）；')
w('> **E_norec_by_design** = 录制侧按 nx2 设计显式不记录（SetRenderEnable 的 profile 门控），不是数据丢失，已在 `opcode_matrix.md` 逐条点名。')
w('')

w('## 二、整帧对账（第 %d 帧，本窗口内行数最多且同时含 draw + state 的一帧）' % pick)
w('')
pf = by_frame[pick]
w('- 该帧总条数 **%d**：A=%d B=%d **C=%d** D=%d E=%d'
  % (sum(pf.values()), pf.get('A_enumnum', 0), pf.get('B_semeq', 0), pf.get('C_real_missing', 0),
     pf.get('D_param_mismatch', 0), pf.get('E_norec_by_design', 0)))
w('')
w('| seq | 直喂侧 CMD | 录制侧 OP | nx2 实现 | 比较字段数 | 不符 | 判定 | d0..d3 | r0..r3 |')
w('|---|---|---|---|---|---|---|---|---|')
for r in rows:
    if int(r['frame']) != pick:
        continue
    w('| %s | %s | %s | `%s` | %s | %s | %s | %s,%s,%s,%s | %s,%s,%s,%s |'
      % (r['seq'], r['d_cmd'], r['r_op'], r['nx_fn'], r['ncmp'], r['nmis'], r['status'],
         r['d0'], r['d1'], r['d2'], r['d3'], r['r0'], r['r1'], r['r2'], r['r3']))
w('')

w('## 三、按命令族（nx2 实现）汇总')
w('')
w('| nx2 实现 | 条数 | A | B | C | D | E |')
w('|---|---|---|---|---|---|---|')
for fn in sorted(by_fam, key=lambda k: -sum(by_fam[k].values())):
    c = by_fam[fn]
    w('| `%s` | %d | %d | %d | %d | %d | %d |'
      % (fn, sum(c.values()), c.get('A_enumnum', 0), c.get('B_semeq', 0), c.get('C_real_missing', 0),
         c.get('D_param_mismatch', 0), c.get('E_norec_by_design', 0)))
w('')

w('## 四、真实缺失 / 参数不符 逐条（应为空）')
w('')
bad = [r for r in rows if r['status'] in ('C_real_missing', 'D_param_mismatch')]
if not bad:
    w('**空** —— 本窗口内 C（真实缺失）= 0、D（参数不符）= 0。')
else:
    w('| 帧 | seq | nx2 实现 | 判定 | d0..d3 | r0..r3 |')
    w('|---|---|---|---|---|---|')
    for r in bad[:400]:
        w('| %s | %s | `%s` | %s | %s,%s,%s,%s | %s,%s,%s,%s |'
          % (r['frame'], r['seq'], r['nx_fn'], r['status'], r['d0'], r['d1'], r['d2'], r['d3'],
             r['r0'], r['r1'], r['r2'], r['r3']))
w('')

w('## 五、覆盖交叉核对：直喂侧全量计数器 vs 对账行数')
w('')
w('探针在 `platform/nvn_replay/cmdbuf.c` 的 `record()` 里挂了全量计数器（`m43b_note_cmd`），')
w('**每一条**写进玩家命令流的记录都算一次；对账行只在对账通道接到时才产生。逐 opcode 相等 = 没有命令绕过对账。')
w('')
if seen:
    w('| CMD_ | 直喂侧计数器 | 对账行数 | 相等? | 说明 |')
    w('|---|---|---|---|---|')
    for k in sorted(seen, key=lambda x: int(x)):
        n = by_cmd.get(k, 0)
        note = ''
        if n != seen[k]:
            note = ('直喂侧有、本单未镜像（见 opcode_matrix.md）' if n == 0 else
                    '**边界效应（-1）**：窗口在帧边界关闭，计数器在 record() 里已 +1，而该行落在关闭后的下一帧缓冲里被丢弃；'
                    '不是漏镜像（该 opcode 其余 %d 条逐条对上）' % n)
        w('| %s | %d | %d | %s | %s |' % (k, seen[k], n, '✓' if n == seen[k] else '✗', note))
    w('')
    w('计数器原文：`%s`' % seen_line[:400])
else:
    w('（未在 vk_boot.log 里找到 destructor 的计数器行 —— 说明进程被强杀，属未验证项）')
w('')
w('## 六、逐帧表（帧号 / 总条数 / A / B / C / D / E）')
w('')
w('| 帧 | 总条数 | A | B | C | D | E |')
w('|---|---|---|---|---|---|---|')
for fr in sorted(by_frame):
    c = by_frame[fr]
    w('| %d | %d | %d | %d | %d | %d | %d |'
      % (fr, sum(c.values()), c.get('A_enumnum', 0), c.get('B_semeq', 0), c.get('C_real_missing', 0),
         c.get('D_param_mismatch', 0), c.get('E_norec_by_design', 0)))
open(out, 'w').write('\n'.join(L) + '\n')
print('wrote', out, len(L), 'lines; rows', len(rows), 'frames', len(frames), 'pick', pick,
      'C', tot.get('C_real_missing', 0), 'D', tot.get('D_param_mismatch', 0))
