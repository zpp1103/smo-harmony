#!/usr/bin/env python3
# M44A：把一次上机目录压成"崩溃归因"一行 + 细节（run.log / crash_summary.txt / vk_boot.log）。
# 用法: python3 tools/m44a_run_stats.py <run_dir> [--tsv]
# 输出（TSV 一行，供拼表）:
#   tag  probe  alive_s  dead  sig  crash_mod  fault_addr  frames  rows  A  B  C  D  E  cap  bigstack
import sys, os, re, json

run = sys.argv[1]
tsv_mode = '--tsv' in sys.argv
boot = os.path.join(run, 'vk_boot.log')
logp = os.path.join(run, 'run.log')
tag = os.path.basename(run.rstrip('/')).replace('run_', '')

txt = ''
for p in (boot, os.path.join(run, 'crash_summary.txt'), logp):
    if os.path.exists(p):
        with open(p, 'rb') as f:
            txt += f.read().decode('utf-8', 'replace')

# 存活时长：run.log 的 "alive=Ns" 行；否则 vk_boot.log 的 [beat] t=
alive = None
m = re.search(r'alive=(\d+)s', txt)
if m: alive = int(m.group(1))
if alive is None:
    beats = re.findall(r'\[beat\] t=(\d+)', txt)
    if beats: alive = max(int(x) for x in beats)

dead = 1 if re.search(r'pid=<dead>|alive=\d+s dead=1', txt) else 0

# crash: [fx] FAULT sig=.. addr=.. pc=name+off lr=name+off / [fx] FAULT mod pc=.. lr=..
sig = ''
mod_s = ''
addr = ''
for line in txt.splitlines():
    if '[fx] FAULT sig=' in line:
        m = re.search(r'sig=(\d+)', line); sig = m.group(1) if m else ''
        m = re.search(r'addr=(0x[0-9a-fA-F]+)', line); addr = m.group(1) if m else ''
        m = re.search(r'pc=([^\s]+) lr=([^\s]+)', line)
        if m: mod_s = 'pc=%s lr=%s' % (m.group(1), m.group(2))
    if '[fx] FAULT mod pc=' in line:
        m = re.search(r'\[fx\] FAULT mod pc=(\S+)', line)
        if m: mod_s = 'pc=%s' % m.group(1) + ((' ' + mod_s.split(' ', 1)[1]) if mod_s and ' ' in mod_s else '')

# probe 是否真的开：以**对账帧行**为准（constructor 的 "recording side ON" 行在 vk_boot.log
# 还没打开时就被丢弃了 —— 这是直喂侧日志通道的既有行为，M43B 的 boot.log 同样没有该行）
probe = 'unknown'
nboot = 0
if os.path.exists(boot):
    with open(boot, 'rb') as fb:
        btxt = fb.read().decode('utf-8', 'replace')
    nboot = len(re.findall(r'\[m43b\] frame \d+:', btxt))
else:
    btxt = ''
probe = 'ON' if nboot else 'OFF'
if re.search(r'-> recording OFF|swnvn section missing|lookup failed', txt): probe = 'OFF(fallback)'

# 对账收尾行：[m43b] done: frames=N rows=M A=.. B=.. C_MISSING=.. D_PARAM=.. E_NOREC=..
frames = rows = ''
cnt = {}
m = re.search(r'\[m43b\] done: frames=(\d+) rows=(\d+) A=(\d+) B=(\d+) C_MISSING=(\d+) D_PARAM=(\d+) E_NOREC=(\d+)', btxt or txt)
if m:
    frames, rows = m.group(1), m.group(2)
    cnt = dict(A=m.group(3), B=m.group(4), C=m.group(5), D=m.group(6), E=m.group(7))
cap = ''
m = re.search(r'cap_skip=(\d+)', txt)
if m: cap = m.group(1)
# 若崩溃发生在收尾行之前，用 frame summary 兜底统计
sumf = os.path.join(run, 'm43b_frame_summary.tsv')
if os.path.exists(sumf):
    with open(sumf, encoding='utf-8', errors='replace') as f:
        hdr = f.readline().rstrip('\n').split('\t')
        idx = {k: i for i, k in enumerate(hdr)}
        tf = 0; tot = [0] * 7
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) != len(hdr): continue
            tf += 1
            for j, k in enumerate(['total', 'A_enumnum', 'B_semeq', 'C_real_missing', 'D_param_mismatch', 'E_norec', 'cap_skip']):
                tot[j] += int(p[idx[k]] or 0)
        if not frames: frames, rows = str(tf), str(tot[0])
        cnt = dict(A=tot[1], B=tot[2], C=tot[3], D=tot[4], E=tot[5])
        if not cap: cap = str(tot[6])

bs = ''
m = re.search(r'\[m44a\] bigstack selftest verdict=(\w+) fails=(\d+)', btxt)
if m: bs = '%s(fails=%s)' % (m.group(1), m.group(2))
elif re.search(r'\[m44a\] bigstack (init|enter|selftest start)', btxt): bs = 'ran(no verdict)'
else: bs = 'not-run'

row = [str(tag), str(probe), str(alive if alive is not None else '?'), str(dead), str(sig or '-'),
       str(mod_s or '-'), str(addr or '-'), str(frames or '?'), str(rows or '?'),
       str(cnt.get('A', '?')), str(cnt.get('B', '?')), str(cnt.get('C', '?')),
       str(cnt.get('D', '?')), str(cnt.get('E', '?')), str(cap or '?'), str(bs)]
if tsv_mode:
    print('\t'.join(row))
else:
    ks = ['tag', 'probe', 'alive_s', 'dead', 'sig', 'crash', 'fault_addr', 'frames', 'rows', 'A', 'B', 'C', 'D', 'E', 'cap', 'bigstack']
    for k, v in zip(ks, row):
        print('%-11s %s' % (k + ':', v))
