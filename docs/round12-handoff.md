# round12 移交：MemoryPoolMap 洪流 = 装载任务指派断链的症状（vpool 层无杠杆）

> 移交方：vpool 师傅（MemoryPoolMap 语义对齐 + 最小 vpool 层）。
> 移交对象：threads.c svc 同步域 / game_patches.c（E1 门）/ 游戏任务系统装载管线负责人。
> 结论先行：**洪流骤降的修复点不在 Map 语义层**（证据⑤），本文档给出断链的完整坐标与复用探针。

## 一句话根因

SMO 装载管线在 init 96% 处断供：主线程无限入队任务并对任务对象 condvar 空打信号，
15 个装载工作线程停泊在自己的 condvar 上从未被指派；装载进度监视器因此以 12-25k/s
无限测量（每次量=2 次 nvnMemoryPoolMap），这就是 Map 洪流的全部本体。

## 证据链（按因果顺序）

1. **洪流=单一调用点、纯症状**
   - 调用点 `main+0x9b9ba8`（`blr x8`，经游戏 NVN proc 表双间接 cell 0x1363dc8 调 Map）。
   - 位于 GetLoadProgress（`main+0x9b97e0`）：`f(poolA=x22+0x40) - f(poolB=x22+0x28)` 双 Map
     指针差 → ×C1×C2/C3 → float 进度；触发门 `x22+0xb8 & 3 == 3`，游戏在 0x9b9b98 有
     `cbz pool` 的 NULL 检查（round12b 的 donor :1986 NULL 语义与其兼容，未崩）。
   - 12c/12d 各 150s 探针：10/10 周期采样同一 LR（两轮 ASLR 各自换算均落 0x9b9ba8）。
   - 四轮速率 26.4 / 27.1 / 25.1 / 34.9 k/s——漂移但从不下降。

2. **donor 基准=洪流是万倍病态**
   - donor（Mac 直跑，SMO 可玩）函数普查：**整个 init 阶段 MemoryPoolMap=409 次、每帧 2 次**；
     init 尾部 TexturePoolRegisterTexture=770、MapVirtual=1。
   - 本宿主 init 尾部 TexturePoolRegister≈740-743、MapVirtual=1（整池 voff=0 别名，donor
     hle_nvn.c:2115 原案）——形状几乎重合，**卡在 donor 会完成前的 ~4%**。

3. **主线程空转入队+空打信号**
   - `[totk-dump] obs E1 0x57f950 hits=1,875,300`（game_patches.c E1 门）。
   - `[totk-dump] pkey overflow (beyond 64+16 keys): sig=2,887,872`（12d 末次 dump）：
     主线程 h1001 对任务对象 condvar（`obj+0x58`；pkey3 表键区 0x6e35acxxxx-0x6e35aexxxxx，
     键间距 0x570=定长结构数组）空打信号 289 万次，**全部零等待者、零转醒**。
   - E1 快照：`el[i] → this=[el+0x10]`，`sig-key=this+0x58`，`[this+0x18]=MQ`、`mq-key=MQ+0x40`
     ——但没有任何线程等在 MQ 键上。

4. **15 个工作线程停泊未被指派**
   - 线程名册（12d dump）：h1006-h1029 等 15 个 `wait=2`（condvar 停泊），键=`worker+0x78`
     （0x6e45/0x6e81/0x6e7e/0x7f764c 等区），nsvc=2..167（早期装载阶段曾工作过），
     `[worker+0x20]`（作业函数槽）大多为 0。
   - `[totk-dump] E4 verdict: key-confluence(sig-key==wait-key)=1 e1-binding(this==worker+0x20)=0`
     ——信号族（obj+0x58）与等待族（worker+0x78）**绑定断裂**。
   - 监视线程 h1004：nsvc=3.38-4.14M ≈ Map 数（每轮 wait+signal 乒乓），
     SignalProcessWideKey 峰值 47k/s（两次 dump 差分）；主线程 h1001 等 WaitForAddress
     （装载完成屏障）永不满。

5. **Map 语义层无杠杆（移交依据）**
   - 洪流池 flags=0x12（CPU_UNCACHED|GPU_UNCACHED）/0x24（CPU_CACHED|GPU_CACHED），
     **无 CPU_NO_ACCESS 位**——donor hle_nvn.c:1986 对它们同样返 mem；返回值语义与 donor
     完全一致，改返回值=反 donor 投机。
   - round12b 已对齐 donor：CPU_NO_ACCESS(0x1) 池 Map 返 NULL（0x21/0x61/0xa1/0x189/0x2a1），
     MapVirtual 整池别名正常，150s 零 crash——本层工作已收口。

6. **排除项**
   - 无 crash（12a-12d 四轮 150s SIGSEGV/SIGILL/SIGABRT/fault 全 0）。
   - 无 FS 错误、RomFS 7 件就位；CreateThread ledger 19/19 全建全启动。
   - 洪流池对象 ledger 反查全 hit（ctor 行缺失=hilog 丢弃，非 foreign 池）。
   - 注意：emu_round10.log 曾被孤儿 hilog 流（已杀，pid 4428）追加污染，其统计不可用。

## 下一步建议（接手方）

- 断链问题：**谁应该把 el（任务）指派给 worker 并 signal worker+0x78**。
  候选方向：main+0x57f950 所在入队路径的下游派发器；或 worker 池管理器（main+0x97fac0 池
  管理器同簇）；对照 donor（native 跑法）同点行为。
- 复用探针（**已全部保留在树内，勿回滚**）：E1 门（game_patches.c）、pkey/pkey2/pkey3
  per-key 普查（threads.c，60s 周期 totk-dump）、Map LR 采样/ledger 反查
  （gfx_bridge.c nvn_pool_map，round12c 起）、relocate import-cell 打点（loader.c，12d 实测
  0x1363dc8/0x136deb8 均非直接 reloc 目标=双间接指针表，静态定名需改在运行时读 cell 链）。
- 未定名项：GetLoadProgress 里 f() 的第二表调用（cell 0x136deb8，返回值=进度分子）——
  若运行时查实为 NVN 宿主实现需回报 vpool 层另议。

## 证据文件

- 日志：`logs/emu_round12a.log`（基线）、`emu_round12b.log`（NULL 语义）、`emu_round12c.log`
  （LR 探针）、`emu_round12d.log`（+loader 探针；E4/pkey/E1 普查最全）。
- donor 基准：`logs/donor_round2_thr.log`（init 0 / frame N 函数普查）。
- 游戏镜像（反汇编用）：`switch2mac/switch2hos/game-work/smo_build/patched/main/main.patched.bin`
  （main+X=文件偏移；12c base=0x7f75b28000、12d 各轮 ASLR 以 log 内 "(main+0x…)" 对推）。
- 补丁：`patches/round12b-pool-map-null-gfx_bridge.c.patch`（NULL 语义）、
  `patches/round12c-map-flood-diagnosis-gfx_bridge_loader.patch`（观测探针）。
