#!/usr/bin/env python3
# M44A：整帧对账报告（抽 N 帧）。输入一次上机的 m43b_frame.tsv + m43b_frame_summary.tsv (+ vk_boot.log)。
# 用法: python3 tools/m44a_frame_cmp.py <run_dir> <out.md> [sample_n=200]
# 判据沿用 M43B：C_real_missing 与 D_param_mismatch 必须为 0（A=枚举编号差、B=语义等价拆分、E=按设计不记录）。
import sys, os, re, collections

run, out = sys.argv[1], sys.argv[2]
NS = int(sys.argv[3]) if len(sys.argv) > 3 else 200
tsv = os.path.join(run, 'm43b_frame.tsv')
sumf = os.path.join(run, 'm43b_frame_summary.tsv')
boot = os.path.join(run, 'vk_boot.log')
STAT = ['A_enumnum', 'B_semeq', 'C_real_missing', 'D_param_mismatch', 'E_norec_by_design']

if not os.path.exists(tsv) or not os.path.exists(sumf):
    print('missing tsv/summary in', run); sys.exit(1)

# ---- 帧窗口 ----
frames = []
with open(sumf, encoding='utf-8', errors='replace') as f:
    h = f.readline().rstrip('\n').split('\t')
    for line in f:
        p = line.rstrip('\n').split('\t')
        if len(p) != len(h): continue
        d = dict(zip(h, p)); frames.append(d)
nums = sorted(int(d['frame']) for d in frames)
if not nums:
    print('no frames'); sys.exit(1)
span = nums[-1] - nums[0] + 1
# 等距抽 NS 帧（含首尾）
if len(nums) <= NS:
    sample = set(nums)
else:
    idx = [round(i * (len(nums) - 1) / (NS - 1)) for i in range(NS)]
    sample = {nums[i] for i in idx}
sample = sorted(sample)

# ---- 逐行 ----
tot = collections.Counter()
bysample = collections.Counter()
per = collections.defaultdict(collections.Counter)   # id -> status count
names = {}
bad = []          # 所有 C/D 行（最多记 200 条原文）
nrows = 0
with open(tsv, encoding='utf-8', errors='replace') as f:
    h = f.readline().rstrip('\n').split('\t')
    ix = {k: i for i, k in enumerate(h)}
    for line in f:
        p = line.rstrip('\n').split('\t')
        if len(p) != len(h): continue
        nrows += 1
        st = p[ix['status']]; fr = int(p[ix['frame']]); fid = int(p[ix['id']])
        names[fid] = p[ix['nx_fn']]
        tot[st] += 1
        per[fid][st] += 1
        per[fid]['rows'] += 1
        if fr in sample: bysample[st] += 1
        if st in ('C_real_missing', 'D_param_mismatch') and len(bad) < 200:
            bad.append((fr, p[ix['seq']], fid, p[ix['nx_fn']], st, p[ix['ncmp']], p[ix['nmis']],
                        p[ix['d0']], p[ix['d1']], p[ix['r0']], p[ix['r1']], p[ix['cap_skip']]))

# ---- 交叉核对（直喂侧 CMD_* 出现次数 vs 对账行数）----
cmdline = ''
cmd = {}
if os.path.exists(boot):
    with open(boot, 'rb') as f:
        for bl in f.read().decode('utf-8', 'replace').splitlines():
            if 'direct-side CMD_* occurrences' in bl:
                cmdline = bl.strip()
                for k, v in re.findall(r'(\d+)=(\d+)', bl.split(':', 1)[1] if ':' in bl else bl):
                    cmd[k] = int(v)

# ---- md ----
w = out and open(out, 'w', encoding='utf-8') or sys.stdout
def P(*a): print(*a, file=w)
P('# M44A：整帧对账（%s）' % os.path.basename(run.rstrip('/')))
P('')
P('判据（与 M43B 同口径）：**C_real_missing = 0**（直喂侧进了命令流、录制侧一条都没产生/opcode 对不上）、')
P('**D_param_mismatch = 0**（opcode 对上了但关键字段不等）。A=枚举编号差（两套枚举各自编号）、')
P('B=语义等价（一条 NVN 调用被拆成 N 条记录或字段换了编码位置）、E=录制侧按设计不记录。')
P('')
P('## 一、窗口总量')
P('')
P('| 项 | 值 |')
P('|---|---|')
P('| 帧数（summary 行数） | %d |' % len(frames))
P('| 帧号范围 | %d … %d（跨度 %d） |' % (nums[0], nums[-1], span))
P('| 对账行数 | %d |' % nrows)
for s in STAT:
    P('| %s | %d |' % (s, tot[s]))
P('| 合计 | %d |' % sum(tot.values()))
P('')
P('## 二、抽帧样本（等距 %d 帧，含首尾）' % len(sample))
P('')
P('| 项 | 值 |')
P('|---|---|')
P('| 抽样帧 | %d（%s … %s） |' % (len(sample), sample[0], sample[-1]))
for s in STAT:
    P('| %s | %d |' % (s, bysample[s]))
P('| **C/D 判定** | **%s**（C=%d, D=%d） |' % ('PASS' if bysample['C_real_missing'] == 0 and bysample['D_param_mismatch'] == 0 else 'FAIL',
                                        bysample['C_real_missing'], bysample['D_param_mismatch']))
P('')
P('## 三、逐命令族（全程）')
P('')
P('| id | nx 记录器 | 行数 | A | B | C | D | E |')
P('|---|---|---|---|---|---|---|---|')
for fid in sorted(per):
    c = per[fid]
    P('| %d | `%s` | %d | %d | %d | %d | %d | %d |' % (fid, names.get(fid, '?'), c['rows'], c['A_enumnum'],
      c['B_semeq'], c['C_real_missing'], c['D_param_mismatch'], c['E_norec_by_design']))
P('')
P('## 四、M44A 新镜像的 4 个执行链 opcode')
P('')
P('| nx 记录器 | 行数 | A | B | C | D | E |')
P('|---|---|---|---|---|---|---|')
for fn in ('nvnCommandBufferCopyCommands', 'nvnCommandBufferSignalEvent', 'nvnCommandBufferFenceSync', 'nvnCommandBufferReportCounter'):
    fid = next((k for k, v in names.items() if v == fn), None)
    if fid is None:
        P('| `%s` | 0 | 0 | 0 | 0 | 0 | 0 |' % fn); continue
    c = per[fid]
    P('| `%s` | %d | %d | %d | %d | %d | %d |' % (fn, c['rows'], c['A_enumnum'], c['B_semeq'],
      c['C_real_missing'], c['D_param_mismatch'], c['E_norec_by_design']))
P('')
if cmdline:
    P('## 五、覆盖交叉核对（直喂侧 `record()` 全量计数器）')
    P('')
    P('```')
    P(cmdline[:1500])
    P('```')
    P('')
P('## 六、C/D 行原文（最多 200 条）')
P('')
if not bad:
    P('无（C=0, D=0）。')
else:
    P('| frame | seq | id | nx 记录器 | 状态 | ncmp | nmis | d0 | d1 | r0 | r1 | cap |')
    P('|---|---|---|---|---|---|---|---|---|---|---|---|')
    for b in bad:
        P('| %s | %s | %s | `%s` | %s | %s | %s | %s | %s | %s | %s | %s |' % b)
P('')
P('**结论**：抽样 %d 帧 C=%d、D=%d → 真实缺失 %d、参数不符 %d。' % (
    len(sample), bysample['C_real_missing'], bysample['D_param_mismatch'],
    bysample['C_real_missing'], bysample['D_param_mismatch']))
if out: w.close()
print('wrote', out, 'rows=%d frames=%d sample=%d C=%d D=%d' % (nrows, len(frames), len(sample),
      bysample['C_real_missing'], bysample['D_param_mismatch']))
