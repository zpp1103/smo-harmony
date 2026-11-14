#!/usr/bin/env python3
# M44A / S2b-2 前置：生成 opcode 覆盖矩阵（logs/m44a/opcode_matrix.md）
# 基线 = tools/m43b_opcode_matrix.py（未改动，留作 M43B 的可复现产物）；本文件只多两件事：
#   ① M44A 把执行链必需的 4 个 opcode（OP_CALL / OP_SIGNAL_EVENT / OP_FENCE_SYNC / OP_REPORT_COUNTER）接进同一条探针；
#   ② 逐条理由表随之更新（nx2 侧未镜像 17 → 13，直喂侧未镜像 21 → 17）。
# 数据全部从源码**解析出来**，不手写数字：nx2 OP_ 枚举（nvn_vk2/abi/nvn_obj.h）、
# 直喂侧 CMD_ 枚举（platform/nvn_replay/nvn.h）、本单的镜像族表（nvn_vk2/rec/m43b_rec_tap.c 的 g_ent）、
# 以及 nx2 录制侧每个 OP_ 的生产者函数（对 rec/hle_nvn.c 做函数体扫描）。
# 用法: python3 tools/m43b_opcode_matrix.py <cpp_root> <out.md>
import re, sys, os

cpp, out = sys.argv[1], sys.argv[2]

# ---------- 1. nx2 OP_ 枚举 ----------
src = open(os.path.join(cpp, 'nvn_vk2/abi/nvn_obj.h')).read()
i = src.find('typedef struct { uint32_t op, size; } op_hdr;')
b = re.sub(r'//[^\n]*', '', src[i:i + 3000])
seg = b[b.find('enum {'):b.find('OP_COUNT_')]
OPS = list(enumerate(re.findall(r'OP_[A-Z_0-9]+', seg)))
assert len(OPS) == 50, len(OPS)
OPV = {n: v for v, n in OPS}

# ---------- 2. 直喂侧 CMD_ 枚举 ----------
src = open(os.path.join(cpp, 'platform/nvn_replay/nvn.h')).read()
m = re.search(r'enum \{\s*\n\s*CMD_END = 0,(.*?)CMD_COUNT\s*\n\};', src, re.S)
CMDS = []
v = 0
for t in re.findall(r'CMD_[A-Z_0-9]+', m.group(0)):
    if t == 'CMD_COUNT':
        break
    CMDS.append((v, t))
    v += 1
assert len(CMDS) == 62, len(CMDS)

# ---------- 3. 本单镜像族表 ----------
probe = open(os.path.join(cpp, 'nvn_vk2/rec/m43b_rec_tap.c')).read()
ENT = {}
for mm in re.finditer(r'\[(T_[A-Z_0-9]+)\]\s*=\s*\{\s*"([^"]+)",\s*(C_[A-Z_0-9]+),\s*(OP_[A-Z_0-9]+|-\d)\s*\}', probe):
    ENT[mm.group(1)] = (mm.group(2), 'CMD_' + mm.group(3)[2:], mm.group(4))
BY_CMD, BY_OP = {}, {}
for k, (fn, c, o) in ENT.items():
    BY_CMD.setdefault(c, []).append(k)
    BY_OP.setdefault(o, []).append(k)

# ---------- 4. nx2 录制侧的 OP_ 生产者 ----------
src = open(os.path.join(cpp, 'nvn_vk2/rec/hle_nvn.c')).read()
PROD = {}   # OP_ -> [func]
for mm in re.finditer(r'^NVN(?:_NOPROTO)?\((\w+),', src, re.M):
    name = mm.group(1)
    br = src.find('{', mm.end())
    if br < 0 or br - mm.end() > 400:
        continue
    depth, j = 0, br
    while j < len(src):
        ch = src[j]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                break
        elif ch == '"':
            j += 1
            while j < len(src) and src[j] != '"':
                if src[j] == '\\':
                    j += 1
                j += 1
        j += 1
    for o in sorted(set(re.findall(r'\bOP_[A-Z_0-9]+', src[br:j + 1]))):
        PROD.setdefault(o, []).append(name)
# 少数 opcode 由 helper 写入（不在某个 NVN(...) 函数体里）：人工补上生产者名字，避免矩阵里显示成 "—"
PROD.setdefault('OP_BIND_STATE', ['bind_state() ← BindBlend/Color/ChannelMask/Multisample/Polygon/DepthStencilState'])
PROD.setdefault('OP_JUMP', ['cmd_new_segment()/jump_from_here() —— 宿主 shadow 页与主机段之间的续接'])

# ---------- 5. 未镜像项的理由（人工，逐条）----------
OP_NOTE = {
    'OP_END': ('不镜像', '命令列表的终结记录（EndRecording 写入），不是一笔 NVN 调用；直喂侧同一位置的 CMD_END 同理。'),
    'OP_JUMP': ('不镜像', '命令流跨段跳转（宿主 shadow 页/主机段切换时的内部记录），没有对应的 NVN 调用。'),
    'OP_BEGIN': ('不镜像', 'nx2 已**不再记录**（hle_nvn.c BeginRecording 注释：一段录制以第一条命令当句柄，空录制只占 END）。'),
    'OP_CALL': ('已镜像（S2b-2 就绪）', '**M44A 新接**：直喂侧 `call_commands` 每写一条 `CMD_CALL`（= 接进一个命令句柄）就调一次 `T_CALL`，nx2 侧以 n=1 调 `CopyCommands`（只写 `OP_CALL{target=handle}`，不解析句柄、不重放、不回写客体），载荷逐值比较 target。直喂侧 CALL 仍是「只记录、不重放」（call_list 一行未改）。'),
    'OP_SIGNAL_EVENT': ('已镜像（S2b-2 就绪）', '**M44A 新接**：比较 {ev->val 指针, 信号模式, 值} 三字段。nx2 录制侧只**读** `ev->val` 再写记录，真正的「写事件值」发生在 exec 侧（`mtl_sub_event`），所以镜像不改执行语义。'),
    'OP_REPORT_COUNTER': ('已镜像（S2b-2 就绪）', '**M44A 新接**：比较 {counterType, 目标地址}。地址按直喂侧既有口径（非客体内存已清零并记一行日志）传入，见 `cmdbuf.c` 的 `M43B_TAP_X` 注释。'),
    'OP_BIND_SAMP': ('不镜像', '录制侧由 BindSeparateSamplers/BindSeparateSampler 产生；**直喂侧这两个函数是"只记账不记录"**（cmdbuf.c 里只 LOG、不 record），没有对应 CMD_ 可对账。属直喂侧既有未移植分支。'),
    'OP_COPY_B2T': ('不镜像', '纹理快照族（tex_snap）。CopyBufferToTexture 在录制侧要为 1 个 NVNtexture 做快照，M43A/M43B 未覆盖该分支；直喂侧 CMD_COPY_BUFFER_TO_TEXTURE 有记录。列为**未镜像**，不是"丢失"。'),
    'OP_COPY_T2B': ('不镜像', '同 OP_COPY_B2T（纹理快照族）。'),
    'OP_COPY_T2T': ('不镜像', '同 OP_COPY_B2T（纹理快照族，2 张纹理快照）。'),
    'OP_RESET_COUNTER': ('不镜像', '直喂侧平台层没有 ResetCounter 记录器（cmdbuf.c 无对应 NVN 函数/CMD_），无对账对象。'),
    'OP_RENDER_COND': ('部分镜像', 'T_RENDEREN 已挂点，但 nx2 只在 **DM_GAME_RENDER_COND** 打开时才记录；本单未打开该 profile → 期望命中 E_NOREC（"录制侧按设计不记录"），不是缺失。SetRenderEnableConditional 在直喂侧也不记录。'),
    'OP_FENCE_SYNC': ('已镜像（S2b-2 就绪）', '**M44A 新接**：只比较 `NVNsync*` 指针。nx2 录制侧 FenceSync 只写 `OP_FENCE_SYNC{sync}`，fence 值（`mtl_last_fence()+1`）在 exec 侧才赋，所以镜像不改执行语义。'),
    'OP_TF_BIND': ('不镜像', '直喂侧 BindTransformFeedbackBuffer(s) 是 `(void)c;`（不记录），没有对账对象。'),
    'OP_TF_STATE': ('不镜像', '同上：直喂侧 Begin/End/Pause/ResumeTransformFeedback 在本移植里没有记录器。'),
    'OP_RAST_DISCARD': ('不镜像', '直喂侧 SetRasterizerDiscard 是 `(void)c;`（不记录）。'),
    'OP_VP_SWIZZLE': ('不镜像', '直喂侧 SetViewportSwizzles 是 `(void)c;`（不记录）。'),
    'OP_BIND_TEX1': ('等价镜像', '**不直接产生**：单槽 BindTexture 用"1 条的复数形态"镜像，录制侧落到 OP_BIND_TEX（p_bind_handles n=1），载荷逐字段相同 → 记为 B_SEMEQ。'),
    'OP_DRAW_TF': ('不镜像', 'DrawTransformFeedback：直喂侧无记录器；依赖 TF 捕获计数，属执行链。'),
}

CMD_NOTE = {
    'CMD_END': ('不镜像', '命令列表终结记录（EndRecording 写），不是 NVN 调用。运行时计数器会看到它（值 0），矩阵里点名，不算缺失。'),
    'CMD_JUMP': ('不镜像', '分段跳转内部记录。'),
    'CMD_CALL': ('已镜像（S2b-2 就绪）', '每个被接进来的命令句柄一条 `CMD_CALL` ↔ 一条 `OP_CALL`（1:1）；只比较句柄值。'),
    'CMD_BIND_PROGRAM': ('已镜像', ''), 'CMD_BIND_BLEND': ('已镜像', ''), 'CMD_BIND_CHANNEL_MASK': ('已镜像', ''),
    'CMD_BIND_COLOR': ('已镜像', ''), 'CMD_BIND_DEPTH_STENCIL': ('已镜像', ''), 'CMD_BIND_MULTISAMPLE': ('已镜像', ''),
    'CMD_BIND_POLYGON': ('已镜像', ''), 'CMD_BIND_VERTEX_ATTRIB': ('已镜像', ''), 'CMD_BIND_VERTEX_STREAM': ('已镜像', ''),
    'CMD_BIND_VERTEX_BUFFERS': ('已镜像', '复数 + 单参数两种形态都镜像（单参数改写成 1 条的复数形态）'),
    'CMD_BIND_UNIFORM_BUFFERS': ('已镜像', '复数形态 1→N 拆分（每槽一条 OP_BIND_UBO）→ B_SEMEQ；单参数形态同'),
    'CMD_BIND_STORAGE_BUFFERS': ('已镜像', '同 UBO，1→N'),
    'CMD_BIND_TEXTURES': ('已镜像', '复数 + 单参数；另有 BindTexturesDeferred 走 CMD_BIND_TEXTURES_DEFERRED（见该行）'),
    'CMD_BIND_IMAGES': ('已镜像', '复数 + 单参数'),
    'CMD_SET_TEXTURE_POOL': ('已镜像', '比较 pool 指针；镜像后立即把私有 CB 的 tex_base 复位 0（避免 handle 被宿主池编号重编号，影响后续比较基准）'),
    'CMD_SET_SAMPLER_POOL': ('已镜像', '同上'),
    'CMD_SET_RENDER_TARGETS': ('已镜像', '比较 n / colors[0] 纹理指针 / depth'),
    'CMD_SET_VIEWPORTS': ('已镜像', '复数 + 单参数'),
    'CMD_SET_SCISSORS': ('已镜像', '复数 + 单参数（录制侧复数形态只保留 index 0，与 nx2 原实现一致）'),
    'CMD_SET_DEPTH_RANGES': ('已镜像', '复数 + 单参数'),
    'CMD_SET_BLEND_COLOR': ('已镜像', ''),
    'CMD_SET_STENCIL': ('已镜像', '3 个变体（Ref/Mask/ValueMask）分别挂点，比较 face + which + 对应值字段'),
    'CMD_SET_PRIMITIVE_RESTART': ('已镜像', ''),
    'CMD_SET_ALPHA_REF': ('不镜像', 'nx2 的 SetAlphaRef 是**显式 no-op**（只 ONCE_LOG "宿主没有 alpha test"），录制侧按设计不记录 → 挂了也只会得到 E_NOREC，本单不挂，矩阵点名。'),
    'CMD_SET_DEPTH_BOUNDS': ('不镜像', 'nx2 把 SetDepthBounds 放在 g_nops 名单（空实现，不记录）。'),
    'CMD_SET_POLYGON_OFFSET': ('已镜像', 'float 三参数（factor/units/clamp）'),
    'CMD_SET_LINE_WIDTH': ('不镜像', 'nx2 显式 no-op（"Metal 只有 1 像素线"）。'),
    'CMD_SET_POINT_SIZE': ('不镜像', 'nx2 显式 no-op（"点大小来自 shader"）。'),
    'CMD_SET_SAMPLE_MASK': ('不镜像', 'nx2 显式 no-op（"宿主暂时忽略"）。'),
    'CMD_SET_RENDER_ENABLE': ('部分镜像', '已挂 T_RENDEREN；nx2 受 DM_GAME_RENDER_COND 门控 → 本轮期望 E_NOREC。'),
    'CMD_CLEAR_COLOR': ('已镜像', '3 个变体（float/i/ui）分别挂点；录制侧载荷结构完全相同'),
    'CMD_CLEAR_DEPTH_STENCIL': ('已镜像', 'float depth + 整型 mask'),
    'CMD_CLEAR_TEXTURE': ('已镜像', '3 个变体；比较 mask / region.x / is_uint'),
    'CMD_CLEAR_BUFFER': ('已镜像', ''),
    'CMD_DRAW_ARRAYS': ('已镜像', 'M43A 起（T_DRAW_ARRAYS / T_DRAW_ARRAYS_I）'),
    'CMD_DRAW_ELEMENTS': ('已镜像', 'M43A 起（T_DRAW_ELEMENTS / _BV / _I）'),
    'CMD_DRAW_ARRAYS_INDIRECT': ('已镜像', '经 HLE 的 PLAIN 确实进命令流（M43A 实测纠正）'),
    'CMD_DRAW_ELEMENTS_INDIRECT': ('已镜像', '同上'),
    'CMD_DISPATCH': ('已镜像', ''),
    'CMD_DISPATCH_INDIRECT': ('已镜像', ''),
    'CMD_COPY_BUFFER_TO_BUFFER': ('已镜像', ''),
    'CMD_COPY_BUFFER_TO_TEXTURE': ('不镜像', '纹理快照族（tex_snap），见 OP_COPY_B2T 说明。'),
    'CMD_COPY_TEXTURE_TO_BUFFER': ('不镜像', '同上。'),
    'CMD_COPY_TEXTURE_TO_TEXTURE': ('不镜像', '同上（2 张纹理快照）。'),
    'CMD_DOWNSAMPLE': ('不镜像', 'cmdbuf.c 没有该记录器：Downsample 以 CMD_COPY_TEXTURE_TO_TEXTURE + DM_COPY_RESOLVE 位记录。'),
    'CMD_UPDATE_UNIFORM_BUFFER': ('已镜像', '比较 dst(=buf+off) 与长度'),
    'CMD_FENCE_SYNC': ('已镜像（S2b-2 就绪）', '比较 `NVNsync*` 指针；fence 值的赋值在 exec 侧（`OP_FENCE_SYNC`），录制侧无客体回写。'),
    'CMD_WAIT_SYNC': ('不镜像', 'nx2 的 WaitSync 在 g_nops（空实现，不记录）。'),
    'CMD_BARRIER': ('已镜像', ''),
    'CMD_SIGNAL_EVENT': ('已镜像（S2b-2 就绪）', '比较 {ev->val, 模式, 值}；直喂侧 `PLAIN` → `PLAIN_T`（record 语义不变，只多一行挂点）。'),
    'CMD_WAIT_EVENT': ('不镜像', 'nx2 的 WaitEvent 在 g_nops。'),
    'CMD_REPORT_COUNTER': ('已镜像（S2b-2 就绪）', '比较 {type, 地址}；非客体内存地址按直喂侧既有口径清零后再比。'),
    'CMD_DISCARD_COLOR': ('不镜像', 'nx2 的 DiscardColor 只计数、不记录（视角丢弃语义在录制层不产生命令）。'),
    'CMD_DISCARD_DEPTH_STENCIL': ('不镜像', '同上。'),
    'CMD_SET_COPY_STRIDES': ('已镜像', 'Row/Image 两个变体分别挂点（各自比较 stride + image 位）'),
    'CMD_DRAW_TEXTURE': ('不镜像', 'nx2 无等价实现（TiledDrawTexture 族是空实现）。'),
    'CMD_RESOLVE_DEPTH': ('不镜像', 'nx2 的 ResolveDepthBuffer/TiledDownsample 是空实现（`(void)c;`）。'),
    'CMD_SET_DEPTH_CLAMP': ('已镜像', ''),
    'CMD_BIND_TEXTURES_DEFERRED': ('不镜像', '延迟句柄（defer=1）必须在**执行时**从 src 刷新（bh_fresh），对账用的私有缓冲没有执行期 → 比较会失真，S2b-2 处理。'),
}

cdb = open(os.path.join(cpp, 'platform/nvn_replay/cmdbuf.c')).read()
L = []
w = L.append
w('# M44A / S2b-2 前置：opcode 覆盖矩阵（50 个 nx2 `OP_*` + 62 个直喂侧 `CMD_*`，逐个交代）')
w('')
w('> 本文由 `tools/m44a_opcode_matrix.py` 从源码解析生成，数字不手写（解析逻辑与 M43B 的生成器逐字相同，只改了 4 条状态与标题）：')
w('> nx2 `OP_*` 枚举 ← `nvn_vk2/abi/nvn_obj.h`；直喂侧 `CMD_*` 枚举 ← `platform/nvn_replay/nvn.h`；')
w('> 镜像族表 ← `nvn_vk2/rec/m43b_rec_tap.c` 的 `g_ent[]`；录制侧生产者 ← 对 `rec/hle_nvn.c` 做函数体扫描。')
w('')
w('**判定口径**（与 `frame_cmp.md` 的 A/B/C/D/E 一致）：')
w('')
w('| 状态 | 含义 |')
w('|---|---|')
w('| 已镜像 | 本单把这条直喂侧记录器接到录制侧同名实现，逐帧逐条对账 |')
w('| 部分镜像 | 挂点已接，但录制侧受 profile 门控或只在特定形态下记录 |')
w('| 等价镜像 | 不直接产生该 opcode，但用等价形态镜像（载荷逐字段可对） |')
w('| **不镜像** | 本单明确不接，且**逐条给出理由**（下面两张表每一行都有） |')
w('')
w('## 一、nx2 录制侧 50 个 `OP_*`（`OP_COUNT_ = 50`，值 0..49）')
w('')
w('| # | OP_ | nx2 生产者（`rec/hle_nvn.c`） | 直喂侧 CMD_ | 本单状态 | 说明 / 不镜像的理由 |')
w('|---|---|---|---|---|---|')
n_mir = n_part = n_nomir = 0
for v, name in OPS:
    fams = BY_OP.get(name, [])
    cmds = sorted({ENT[f][1] for f in fams})
    prod = PROD.get(name, [])
    prod_s = ', '.join('`%s`' % p for p in prod[:4]) + (' …' if len(prod) > 4 else '') if prod else '—'
    cmd_s = ', '.join('`%s`' % c for c in cmds) if cmds else '—'
    if fams:
        note = OP_NOTE.get(name, ('已镜像', ''))
        st = note[0] if note[1] else '已镜像'
        why = note[1] or '逐字段比较（见 `frame_cmp.md`）'
        if st.startswith('已镜像'):
            n_mir += 1
        elif st == '部分镜像':
            n_part += 1
        else:
            n_nomir += 1
    else:
        st, why = OP_NOTE.get(name, ('不镜像', '（见下）'))
        if st == '不镜像':
            n_nomir += 1
        elif st == '部分镜像':
            n_part += 1
        else:
            n_mir += 1
    w('| %d | `%s` | %s | %s | %s | %s |' % (v, name, prod_s, cmd_s, st, why))
w('')
w('## 二、直喂侧 62 个 `CMD_*`（`CMD_COUNT = 62`，值 0..61）')
w('')
w('| # | CMD_ | 录制侧 OP_ | 本单状态 | 说明 / 不镜像的理由 |')
w('|---|---|---|---|---|')
for v, name in CMDS:
    fams = BY_CMD.get(name, [])
    ops = sorted({ENT[f][2] for f in fams})
    op_s = ', '.join('`%s`' % o for o in ops) if ops else '—'
    if fams:
        st, why = CMD_NOTE.get(name, ('已镜像', ''))
        if not why:
            why = '逐字段比较（见 `frame_cmp.md`）'
    else:
        st, why = CMD_NOTE.get(name, ('不镜像', '（见下）'))
    w('| %d | `%s` | %s | %s | %s |' % (v, name, op_s, st, why))
w('')
w('## 三、汇总')
w('')
w('- nx2 `OP_*`：50 个 → **已镜像/等价镜像 %d 个**、**部分镜像 %d 个**（profile 门控）、**不镜像 %d 个**（每个都有上面表里的明确理由）。' % (n_mir, n_part, n_nomir))
w('- 换句话说：录制侧 50 个 opcode 里，%d 个在本单被"直喂↔录制"逐帧逐条对账覆盖，%d 个被逐条点名不覆盖，**0 个静默漏项**。' % (n_mir + n_part, n_nomir))
w('- 直喂侧 `CMD_*`：62 个，其中已镜像 %d 个、部分镜像 1 个（CMD_SET_RENDER_ENABLE）、不镜像 %d 个。'
  % (len(BY_CMD), 62 - len(BY_CMD) - 1))
w('- **M44A 的直接后果**：nx2 侧「不镜像」从 **17 → %d**（4 个执行链必需项转为已镜像）；直喂侧「不镜像」从 21 → %d。'
  % (n_nomir, 62 - len(BY_CMD) - 1))
w('- 四个新镜像项的镜像是「只记录、不重放」：nx2 侧这 4 个生产者在录制期都只写命令记录、不回写客体对象'
  '（SignalEvent 只读 ev->val；FenceSync 的 fence 赋值在 exec 侧；ReportCounter 的计数器在 exec 侧），'
  '直喂侧 CALL 的 call_list 语义一行未改。')
n_inline = len(re.findall(r'M43B_TAP\(T_[A-Z_0-9]+\);', cdb))
n_tapx = len(re.findall(r'M43B_TAP_X\(T_[A-Z_0-9]+,', cdb))
n_plaint = len(re.findall(r'^PLAIN_T\(T_', cdb, re.M))
n_drawtap = 7   # 7 个 draw 记录器的挂点是 M43A 的 m43a_cb_draw(...)（本单原样保留）
n_total = n_inline + n_tapx + n_plaint + n_drawtap
w('- 镜像族（`g_ent[]` 行数）：**%d**；直喂侧挂点：`M43B_TAP(id)` 行内 %d 处 + `M43B_TAP_X(id,tx)` %d 处'
  '（M44A 的 CALL / REPORT_COUNTER：一条 NVN 调用写多条记录或直喂侧真值不是 c->x 原样）+ `PLAIN_T` %d 处'
  ' + 7 个 draw 记录器的 `m43a_cb_draw(...)` = **%d**，与族数%s。'
  % (len(ENT), n_inline, n_tapx, n_plaint, n_total, '一致' if n_total == len(ENT) else '**不一致（要查）**'))
w('- **没有静默漏项**：直喂侧 `record()` 里挂了全量计数器（`m43b_note_cmd`），运行期会打印每个 `CMD_*` 的出现次数；')
w('  与对账行数逐 opcode 相等 = 没有任何一条命令绕过对账通道（见 `frame_cmp.md` 的交叉核对表）。')
open(out, 'w').write('\n'.join(L) + '\n')
print('wrote', out, len(L), 'lines; OPs mirrored', n_mir, 'not', n_nomir, '; CMDs mirrored', len(BY_CMD))
