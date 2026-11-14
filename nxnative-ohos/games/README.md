# nxnative-ohos 游戏档案：键表（game.tsv schema，权威文档）

每款游戏一个档案目录 `games/<titleID>/`（以基础版 title ID 命名，与 nxnative `dm/games/` 同构；出处：`infra/nxnative/dm/games/README.md` 开篇、`game-work/nxnative_ohos_design.md` §1）。本文是**游戏参数面 schema 的唯一权威描述**：键的完整语义以 nxnative 键表（`infra/nxnative/dm/games/README.md` L125–165 "Keys"）为准，本文在其上做鸿蒙视角的**增删与落点**标注，不重复其长注。

- 键表增删原则：`game-work/nxnative_ohos_design.md` §2（鸿蒙版键面已定）。
- 实值示例：odyssey = `0100000000010000`（Super Mario Odyssey 1.4.1），totk = `0100F2C0115B6000`（Tears of the Kingdom；nxnative 注册 1.4.3，本机转译为 1.1.2 → derived profile，见 §derived）。
- 鸿蒙侧落点 = 当前鸿蒙工程（`mario-odessy-harmony` 泛化为 `nxnative-ohos/template/`）中消费该键的生成头 / 脚本 / setenv。

## 0. 档案目录结构

```
games/<titleID>/
├── game.tsv                      # 游戏常量键值表：每行 key<TAB>value，注释独占一行（nxnative 同格式）
├── patches.py                    # 可选：本游戏自有 build 的重写器补丁（CUTS/OBS/FRAME_STEP，按 NSO build ID 键控）；
│                                 #   odyssey 当前为空（设计 §1），totk 有 143 行手写（survey L65）
├── host_override/                # 可选：整文件覆盖逃生门——runtime 宿主分叉度 ~65%，模板不可能参数化覆盖全部
│                                 #   （设计 §7；文件相对 template/ 同名路径原样替换）
├── 着色器表 TSV 族                # shader_split.py 产物 + 手工事实（详见「着色器表」组）
│   ├── shader_map.tsv (+ .conflicts.tsv)
│   ├── shader_facts.tsv / shader_facts_manual.tsv（可选，手工事实）
│   ├── shader_pairs.tsv / pso_pairs.tsv（顶点输出 padding + 管线预热的配对表）
│   ├── pso_manifest.tsv          # 预热种子清单
│   └── bf_want.tsv
├── 测试资产
│   ├── smoke.tsv / bench.tsv / usage.tsv（可选）
│   └── expected-v<版本>.json     # “expected 钉”：已验证版本的字节级验证记录（见下）
└── README.md                     # 该游戏的笔记：档案为什么长这样、怎么到达某个场景、还有什么开着
```

**expected 钉**（MacNativeGames 血统，样例 `infra/MacNativeGames/games/totk/assets/expected-v1.4.3.json`）：每个验证过的版本一份，钉住 ExeFS 源 SHA256（main/main.npdm/rtld/sdk/subsdk0）、重写产物 SHA256（`*.patched.bin`）、RomFS size+SHA256、六槽共享字体 SHA256。鸿蒙侧 `game-work/prepare_runtime_ohos.py` 当前**跳过** expected 校验（其 docstring 自述），`build_hap_from_nsp.py`（M2）恢复该纪律；构建工作区的 `.key` 指纹缓存（输入不变即跳过阶段，设计 §3）是 expected 钉的运行时形态，不入库。

**不入库**的大文件（patched.bin / RomFS 分卷 / HAP / c1_full.tsv / cb_extent.tsv）一律走 `<titleID>_build/` 工作区（`nxnative-ohos/README.md` Layout）。

## 0.1 三种游戏差异表达（优先级，铁律）

> **Everything game-specific belongs in `games/<titleID>/`. Code reads the profile; it does not hard-code a game.**
> —— `nxnative-ohos/README.md`「Architecture rule」（继承 nxnative `dm/games/README.md`）

游戏差异一律按以下优先级表达，从上往下用，能用 1 就不用 2，能用 2 就不用 3（`nxnative-ohos/README.md`）：

1. **`game.tsv` 键值**——内存布局 / 呈现行为 / 纹理策略 / 字体模式等一切可参数化的差异（本文键表全集）。
2. **`patches.py` per-build 指令补丁**——CUTS（字节裁剪，每条断言原字节）/ OBS（观察门）/ FRAME_STEP（帧步进桩），按 NSO build ID 键控；一个 build 被两个档案声明即重写报错；Nintendo `sdk` 模块的公共裁剪在 `dm/tools/sdk_patches.py`（nxnative 语义）。
3. **`host_override/` 整文件覆盖**——逃生门：odyssey↔totk 宿主 runtime 55 个同名文件仅 19 个逐字节相同（survey L76），模板不可能纯参数化覆盖全部。

运行时开关沿用宿主现有 `TOTK_*`/`ODYSSEY_*` env 面（近期不重命名，保持宿主稳定；统一化列 M4+。出处：设计 §2 尾注、`mario-odessy-harmony/entry/src/main/cpp/game_napi.cpp` 的 setenv 块）。

---

## 1. 键表

共 **43 个 game.tsv 键** = 继承 nxnative 37（43 − 删除 6，删除清单见 §2）+ 鸿蒙新增 6（§1.16）。键序沿用 nxnative Keys 表。列：`键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注`。"—"= 档案未写该键，用默认（默认值列在备注）。

### 1.1 身份

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `title_id` | 游戏 title ID | `0100000000010000` | `0100F2C0115B6000` | 档案目录名；template 实例化：rawfile 镜像目录名、game_napi 解包子目录、CMake `GAME_{{ID}}` 宏（设计 §4） | 必填；文件夹名即此值 |
| `program_id` | `svcGetInfo(ProgramId)` 应答值 | `0100000000010000` | `0100F2C0115B6000` | 生成 `game_version.h` 的 `GAME_TITLE_ID`（当前 odyssey 值 `0x0100000000010000ULL`） | 必填；可与 title_id 不同 |
| `name` | 游戏名，仅作 `game.py list`/报错/窗口标题的标签；真正的名字 NSP 脚本从 control 数据（NACP）读 | `Super Mario Odyssey` | `The Legend of Zelda: Tears of the Kingdom` | `hap_label` 的默认值；hilog 日志文案 | 必填；玩家可见名字以 NACP 为准 |

### 1.2 版本

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `build` | 每个被带起来的版本一行：`build <ver> <display> <模块>=<build ID>…` | `v393216 1.4.1 main=b424be150a8e7d78701cbe7a439d9ebf subsdk0=93ff8252d975adf07230cb86c83301c5 sdk=fcf93a41b08c5193a1410d262602dc62` | `v720896 1.4.3 main=277178b7dba1b6d4949a84778d4abc58 subsdk0=f39f4c431234c72eb520cfdd1001bbcf sdk=574284b40c06cf1821a44857939083be` | 重写步骤按 build ID 匹配产物；`prepare_runtime`（泛化自 `game-work/prepare_runtime_ohos.py`）读 `<id>_build/patched/<模块>/`（patched.bin + fixups.tsv + gates.tsv）；生成 `game_version.h` 版本串 | build ID 是 32 位 hex；`game_version.h` 当前注释 "1.3.0" 但 main build ID `b424be15…` 即 1.4.1 的 main，以 build ID 为准 |
| `package` | 档案的表 / 预热清单 / 笔记制作时所用的版本；NSP 脚本打包文件夹里最新版（`-p` 可另选） | `v393216` | `v720896` | `build_hap_from_nsp.py` 选版本 | 必填 |
| `shader_versions` | 着色器表是哪些版本的并集（基础版在前）；不在文件夹内的版本 → derived profile | `v393216` | `v720896` | 着色器拆分步骤（shader_split.py） | 决定"注册版本 vs derived"的判据，见 §3 |

### 1.3 模块

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `modules` | 重写并加载的代码模块，按加载序 | `main subsdk0 sdk` | `main subsdk0 sdk` | `prepare_runtime` 的模块表 → `generated/runtime_tables.h` 的 `Module modules[]`（当前脚本硬编码 `["main","subsdk0","sdk"]`，泛化后读本键；`rtld` 由宿主替换） | 必填；rtld 不列 |
| `romfs_marker` | 必须存在于 RomFS 的一个路径（归属校验）；空值 = 程序无数据段时直接查目录本身 | `SoundData/SeResourceStd2nd.aras` | `Pack/ZsDic.pack.zs` | 提取步骤（nx_extract 之后）校验 RomFS 归属 | 必填；survey 取最大 RomFS 文件 |

### 1.4 内存（客机体地址空间与配额）

消费方：宿主 `host/services.c` 的客体内存布局（当前为 TOTK_* env + 编译期常量；M1 参数化后由档案经 setenv / 生成头注入）。

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `mem_reserve` | 客体地址空间总预留 | `0x100000000`（4 GiB） | `0x140000000`（5 GiB） | `services.c` 预留策略（当前 `GUEST_ADDRESS_LIMIT 2^39` + iOS/搜洞逻辑，`TOTK_ALIAS_GIB` 钉大小） | 十六进制字节；totk 5 GiB = 256MB 前置 + 2.375GB alias + 256MB 栈 + 2.125GB 堆（totk game.tsv 注释） |
| `mem_alias` | alias 区大小 | `0x10000000`（256 MiB） | `0x98000000`（2.375 GiB） | `services.c` `alias_requested()`；env `TOTK_ALIAS_GIB`（默认按平台 64/16 GiB 搜缩） | totk 值来自 heap check：`SetHeapSize(0x86000000)` 逼出的手调 |
| `mem_stack` | 栈区大小 | `0x10000000`（256 MiB） | `0x10000000`（256 MiB） | `services.c` `STACK_SIZE`（当前硬编码 0x10000000，与两档案一致） | |
| `app_pool` | 应用内存池（`GetInfo` 应答） | `0xCD500000`（3.24 GiB） | `0xCD500000` | `services.c` `app_pool()`：`TOTK_APP_POOL` env，缺省 `0xcd500000` + 输出分辨率附加 | 零售 Switch 池 3.2 GiB 量级 |
| `mem_used` | `GetInfo(UsedMemorySize)` 在堆之外应答的已用量 | —（默认 `0x4000000`） | —（默认 `0x4000000`） | `services.c` `GetInfo` case 7/22 应答 | 默认 64 MiB；模块超过 64 MiB 的游戏必须调大（Age of Calamity 72MB / Cuphead 84MB，nxnative 例） |
| `sys_resource` | NPDM 声明的系统资源（main.npdm META +0x14），`GetInfo(TotalSystemResourceSize)` 应答；非 0 ⇒ `IsVirtualAddressMemoryEnabled` 为真 | —（宿主旧答案 `0x2000000`/32 MiB） | —（NPDM 实为 24 MiB，totk 注释说明；档案未写则宿主答 32 MiB） | `services.c` `SYSTEM_RESOURCE_BUDGET`（当前 32 MiB 常量）；survey 从 NPDM 自动写入 | 可省但 survey 会补；影响游戏自选内存形态（totk 因此走 alias 区） |
| `mem_map_limit` | `MapPhysicalMemory` 越过应用池时按 Horizon 报 ResultLimitReached | —（默认 off） | —（默认 off） | `services.c` `MapPhysicalMemory` 路径 | on/off；Pokémon Z-A 类"放完再收"的游戏需要 on |

### 1.5 启动与运行时行为

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `game_args` | 命令行无参时传给游戏的参数（空格切分） | —（无） | —（无） | 宿主 loader 的 argv 缺省（`dmhost <dir> -- …` 覆盖） | 可省；如 Octopath 0 的 `-SwitchVirtualAddressSpaceSize=2048` |
| `game_env` | 游戏 `getenv` 应答的 `KEY=VALUE` 对（空格分隔） | —（无） | —（无） | 宿主 hle_os 的 getenv 面 | 可省；如 Absolum 的 `DOTNET_GCRegionRange=60000000` |
| `core_preempt` | 每次 SVC 后额外检查钉在同核的高优 runnable 线程 | —（默认 off） | —（默认 off） | `host/threads.c` 调度近似 | on/off；`DM_PREEMPT` 覆盖 |
| `alloc_big_align` | `StandardAllocator::Allocate` 把 ≥64KB 的块对齐到 64KB | —（默认 off） | —（默认 off） | 宿主 hle_os 分配器行为 | on/off；Vanillaware 系必需，否则环堆基址 0 → 空指针写 |

### 1.6 图形行为（呈现 / 深度 / 回放）

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `present_interval` | `game`（默认）：按游戏设置的 present interval 配帧；`display`：每个显示 tick 一帧 | —（默认 `game`） | —（默认 `game`） | 宿主 present 层（M4 nvn_vk 实装后的呈现节奏） | `DM_PRESENT_INTERVAL` 覆盖 |
| `device_api` | `nvnDeviceGetInteger` 应答的 NVN API `major.minor` | —（按 SDK 推导：SDK≥11 → 55.15，≤10 → 53.313） | —（同左，SDK 20.5 ⇒ 55.15） | `nvn_vk` 层 API 版本应答 | 可省；`DM_NVN_API` 覆盖；不进着色器链版本 |
| `depth_mode` | 管线预构建用的深度模式：`gl`（默认，near=−w）/ `zero`（near=0） | —（默认 `gl`） | —（默认 `gl`） | M4 Vulkan 管线预构建（对齐 nxnative 离线管线档案语义） | 主机侧运行时跟随游戏设置；错值只损失预构建 |
| `render_base` | 3D 视图基准尺寸（dock 然后掌机，如 `1600x900 1280x720`）：渲染缩放所缩的基准 | —（无渲染缩放桩） | —（无渲染缩放桩） | 设置面：渲染缩放行的 `base`（像素显示）；env `TOTK_RENDER_RES` 配合 | 仅带渲染缩放桩（重写表 `RENDER_SCALE`）的 build 才有；只写档案笔记记载过的尺寸 |
| `render_cond` | 条件渲染在回放时求值；时间戳报文 counter 字段按 GPU 置 0 | —（默认 off） | `on`（不加则清屏全跑、3D 场景黑） | 回放层条件渲染路径 | on/off；动物森友会同需 on |
| `gpu_timestamps` | 时间戳报文记录什么：`submission`（默认）/ `segment`（GPU 段起止） | —（默认 `submission`） | —（默认 `submission`；BotW 需 `segment`） | 回放层 report counter 路径 | `DM_NVN_GPU_TS` 覆盖 |
| `vconv_range` | CPU 侧转换的顶点属性只按每 draw 实际读到的顶点范围转换 | —（默认 off） | —（默认 off） | 回放层顶点转换（`vconv_bind` 语义） | on/off；Age of Calamity 类整池 bind 的游戏必需 |
| `feedback_snap` | 采样自己 render pass 颜色附着 的片元键（前缀），须读快照 | —（无） | —（无） | 回放层 pass 拆分 + 拷贝（Metal 限制的起因；Vulkan 侧为 feedback loop 约束的对应决策点，M4 复核） | 8–16 位小写 hex 键前缀列表；只列实测会坏的着色器 |

### 1.7 纹理

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `tex_flush_upload` | CPU 原地写入、用 `nvnMemoryPoolFlushMappedRange` 发布的纹理按打包纹理上传，页变重传 | —（默认 off） | `on`（流送纹理全走此路，不加 3D 黑屏） | `host/nvn_tex.c` 上传路径 | on/off |
| `tex_dirty_upload` | 需 `tex_flush_upload on`：init 后 CPU 无 flush 改写页的注册纹理也上传（写保护 + 首用检查） | —（默认 off） | `on`（ASTC 地形材质阵列；不加洞里品红） | `host/nvn_tex.c` 脏页路径 | on/off；依赖 `tex_flush_upload` |
| `tex_dyn_slots` | 句柄追不到单槽位的着色器走 fallback 数组 + c2 槽位表绑定 | —（默认 off） | —（默认 off） | 回放层纹理/采样器绑定表 | on/off；仅对句柄确实来自 c2 表的游戏正确；不进链版本 |

### 1.8 帧率调度

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `fps_target` | 帧步进上限：重写器 FRAME_STEP 桩的上限（有桩的 build）或引擎自身帧限（宿主写入游戏内存） | —（默认 `0` = 游戏自身步进） | `60`（配 patches.py 的 FRAME_STEP；Switch 原生 30） | 宿主帧步进（fstep 语义）；env `DM_FPS_TARGET`/`TOTK_UNCAPPED*` 面 | 取值 0/30/60 |
| `fps_hold` | 连续 3 帧迟到时把 hold 提到 cap 的 vsync 数之上、稳了再降 | —（默认 on） | —（默认 on） | 宿主 fstep hold 逻辑 | on/off；纸马 TTYD 类"帧等 3 个渲染工作线程"的游戏要 off |

### 1.9 输入壳与运行模式

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `op_mode` | 启动时游戏看到的运行模式：`console`（默认，dock）/ `handheld` | —（默认 `console`） | —（默认 `console`） | `host/applet_host.c`；env `TOTK_OPERATION_MODE` 已存在 | 银河 1/2、Captain Toad 用 `handheld`（触屏指针）；handheld 也改 720p 渲染 |
| `perf_mode` | 性能模式：`follow`（默认，随 op_mode）/ `boost`（恒 Boost） | —（默认 `follow`） | —（默认 `follow`） | `host/applet_host.c` 性能模式应答 | `boost` 是 Switch 1 不会报告的组合，纯档案选择 |

（nxnative 的 `pad_layout`/iPhone 虚拟手柄键已删；鸿蒙触壳布局由宿主 `gamepad_layout.h` 的 env 面 `TOTK_FACE_BUTTONS`/`TOTK_MINIMAL_LAYOUT` 承担，见 §2。）

### 1.10 字体

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `shared_font` | 共享字体地址指向什么：`bfttf`（8 字节 BFTTF 头 + TTF，SDK≥11）/ `ttf`（裸 TTF，SDK≤10） | —（按 SDK 推导，SDK 7.3.2 本应为 ttf 类；nxnative 语义里 Odyssey 归入 ttf 组） | —（SDK 20.5 ⇒ bfttf） | `host/shared_font.c` 加载器（sha256/XOR/魔数校验）+ `make_shared_fonts.py` 的伪造策略；env `TOTK_SHARED_FONTS=<dir>` 换真字体目录 | 无档案设置它（nxnative 同）；鸿蒙新增的 `fonts_mode`（§1.16）管"伪造内嵌 vs 固件真字体"这一鸿蒙特有维度 |

### 1.11 着色器拆分

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `shader_groups` | shader_split.py 的源分组：`/RomFS前缀/=组名` 列表 | `/ObjectData/=objectdata /StageData/=stagedata /SoundData/=sounddata /LayoutData/=layoutdata /LocalizedData/=localizeddata /EventData/=eventdata /MovieData/=moviedata /SystemData/=systemdata /ShaderData/=shaderdata /EffectData/=effectdata` | `/Shader/ApplicationPackage=layout /Shader/=shader /Effect/=effect /UI/=layout` | 着色器拆分步骤（复用 nxnative shader_split.py；M4 起接 Vulkan 翻译） | `shader_*` 键进着色器链版本 |
| `shader_note` | map 头部的备注串 | `Super Mario Odyssey shaders` | `Tears of the Kingdom shaders` | 同上（写进表头注释） | 自由文本 |

### 1.12 着色器表 TSV 族（档案内文件，非 game.tsv 键）

| 文件 | 语义 | odyssey | totk | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `shader_map.tsv`（+ `.conflicts.tsv`） | 着色器映射主表（+ 冲突表） | 有（约 3.4MB，survey L63） | 有 | M4 着色器翻译链输入 | 拆分产物 |
| `shader_facts.tsv` | 机器翻译事实（每着色器的输入输出类型等） | 有 | 有 | 同上 | 拆分产物 |
| `shader_facts_manual.tsv` | 可选：手工翻译事实（机器看不出的：TotK/BotW 的整型片元输出） | 无 | 有 | 同上；合并覆盖生成行，改动只影响本档案链版本 | 手写、入库 |
| `shader_pairs.tsv` | 顶点/片元配对表（顶点输出 padding 依据） | 有 | 有 | 同上 | 拆分产物 |
| `pso_pairs.tsv` | agl BAHS 程序变体配对（管线预热） | 无 | 有（pso_manifest 约 2.2MB，survey L63） | 同上；无此表的档案 NSP 脚本把生成表放包旁 | agl 引擎特有 |
| `pso_manifest.tsv` | 预热种子清单 | 有（拷头部） | 有 | 管线预热（M4） | |
| `bf_want.tsv` | bf 想要的表 | 有（`# generated` 头） | 有 | 拆分校验 | |
| `c1_full.tsv` / `cb_extent.tsv` | **不入库**（git ignore）：c1 常量分段满表 / uniform 槽读取范围 | 构建工作区生成 | 构建工作区生成 | 每次 dmsh 构建产物（鸿蒙 M4 起同语义） | 没有它：每着色器 c1 各占 64KiB / pass 内部分更新的 uniform 块整块拷贝 |

### 1.13 补丁（档案内文件，非 game.tsv 键）

| 文件 | 语义 | odyssey | totk | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `patches.py` | 本游戏自有 build 的重写补丁，按 NSO build ID 键控：`CUTS`（每条断言原字节）、`OBS`（观察门）、`FRAME_STEP`（帧步进桩） | 空（无文件或空文件均可，设计 §1） | 有（FRAME_STEP：main+0x2db18c 桩替代 present interval 逻辑，60fps；143 行） | `patch_nso` 步骤（复用 nxnative）产出 patched.bin + cuts/obs/fixups/gates/sites TSV；`prepare_runtime` 吃其中 3 样 | 一个 build 被声明两次即报错；derived profile 沿用仓库档案的规则 |
| `sdk_patches.py`（工具级，非档案内） | Nintendo `sdk` 模块公共裁剪（多游戏共享，如 totk sdk node area 1GB→62MB） | — | — | `patch_nso` 步骤 | 位于 `dm/tools/`，不在 games/<id>/ |
| `MATCH_CUTS`（opt-in 机制） | 按函数指纹在未注册 build 里定位已验证裁剪 | 未用 | 未用（1.1.2 需要它或手工 enrol） | `patch_nso` 步骤 | 未注册 build 的 cut 接入通道，见 §3 |

### 1.14 测试资产

| 文件 | 语义 | odyssey | totk | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `smoke.tsv` | boot 冒烟路线（键值：save/keys/secs/reach/window/shots） | 无 | 有（title→地下洞窟 95s 路线） | `tools/run_cycle.sh` 的验尸口径 | 回归 harness 消费 |
| `bench.tsv` | 可选：基准路线（名 / 脚本输入 DM_KEYS / 测量窗口 / 描述） | 无 | 无 | M5 性能口径 | 每行一条路线 |
| `usage.tsv` | 档案键/接口使用统计（`regress.py usage` 生成，名字+计数） | 无 | 有 | 文档自证（哪些键真被读） | 生成物 |
| `expected-v<ver>.json` | expected 钉（§0） | 无（当前仅 prepare 跳过校验） | nxnative/Mac 侧有 1.0.0/1.1.2/1.2.1/1.4.3 四份 | M2 `build_hap_from_nsp.py` 校验步骤 | 鸿蒙档案按版本补钉 |

### 1.15 设置（玩家面）

nxnative 的玩家设置是 `dm/games/settings.tsv` 共享表（一行一设置：env 变量 / 类型 / 默认 / 显示条件 / 分区 Game|Controls|Performance；默认可用 `DM_GAME_<NAME>` 宏解析到档案键，`<ios>|<mac>` 选平台值），由 `settings.py` 按游戏裁进 app 的 `launcher.json`。鸿蒙侧 **M5 前不建 launcher**：玩家可调项由 `game_napi.cpp` 的 setenv 块 + `run_cycle.sh` 顶替，诊断开关一律 env-only（对齐 nxnative "diagnostic switches stay environment-only" 语义）。

| 设置面（nxnative settings.tsv 行） | 解析到的档案键 | odyssey 默认 | totk 默认 | 鸿蒙当前落点 | 备注 |
|---|---|---|---|---|---|
| `DM_FPS_TARGET`（Frame rate） | `fps_target` | 0 | 60 | env 面（`TOTK_UNCAPPED*` 系） | 30/60/0 |
| `DM_OPMODE`（Console mode） | `op_mode` | 1（dock） | 1 | env `TOTK_OPERATION_MODE` | |
| `DM_RENDER_SCALE`（Render scale，base= `render_base`） | `render_base` | —（无渲染缩放桩） | — | env `TOTK_RENDER_RES` | `render_base` 是 3D 视图基准尺寸（如 `1600x900 1280x720`），只有带渲染缩放桩的 build 才有 |
| `DM_LANG`（Game language） | 无（来自 control 数据语言表） | 多语 | 多语 | env `TOTK_LANG/TOTK_LOCALE` | languages>1 才显示 |

（nxnative 设置表中的 `DM_PRESENT_SNAP`/`DM_PSO_MTL4`/`DM_PSO_PREBUILD`/`DM_NVN_ENC_THREAD` 等 Metal 侧行随其键一并删除，见 §2。）

### 1.16 鸿蒙新增组（hap_* / romfs_* / fonts_mode / nvn_backend）

出处：设计 §2 新增键表。这一组是 `game.tsv` 键（非文件）。

| 键 | 语义 | odyssey 实值 | totk 实值 | 鸿蒙侧落点 | 备注 |
|---|---|---|---|---|---|
| `hap_bundle` | 应用 bundleName | `com.vibecc.smo`（`AppScope/app.json5` 现值） | 待 M3 起草（模式 `com.vibecc.<id>`） | template `AppScope/app.json5.tmpl` 的 `{{GAME_ID}}` 占位（设计 §4.1） | 默认按游戏 |
| `hap_label` | 应用名（桌面图标下与Ability 标签） | `马里奥奥德赛`（`string.json` `ability_label`；AppScope label 走 `$string:app_name`） | 待 M3 | `app.json5` label / `entry/src/main/module.json5` ability_label + `resources/.../element/string.json` | 默认取 NACP 游戏名 |
| `hap_icon` | 应用图标资源 | `$media:icon`（module.json5/icon.jpg 于 NSP control） | 待 M3（NSP icon.jpg 可复用） | `module.json5` icon / `resources/base/media/` | 默认取 NSP control 图 |
| `romfs_volume_mib` | RomFS 分卷单卷大小 | `1024`（`make_romfs_runtime.py` `CHUNK=1<<30`；5,678,065,309 B → 6 卷） | `1024`（17,052,625,232 B → 16 卷，M3 验收口径） | `tools/make_romfs_runtime.py` 的分卷粒度；宿主 `host/romfs.c` `MAX_CHUNKS=64`（上限 64 GiB） | 单位 MiB，默认 1024 |
| `fonts_mode` | 共享字体来源：`embedded`（Windows TTF 伪造 BFTTF 内嵌）/ `firmware`（真机固件 BFTTF，校验后外置） | `embedded`（`make_shared_fonts.py`：6 槽位、3,440,036 B 唯一数据、自校验通过） | 待定（`TOTK_SHARED_FONTS=<dir>` 调试通道已留） | `tools/make_shared_fonts.py`（embedded 生成 `shared_fonts_data.h`/`shared_fonts_config.h`；firmware 校验真 BFTTF，设计 §3 步骤 5） | 默认 embedded；与 `shared_font` 键正交（那个管格式语义，这个管来源） |
| `nvn_backend` | 图形后端选择（预留） | `vulkan`（`nvn_vk/` 目录在位，M4 实装） | `vulkan` | M4：宿主图形层后端选择；当前唯一合法值 `vulkan` | 默认 vulkan |

运行时开关不新增键：沿用 `TOTK_*`/`ODYSSEY_*` env 面（设计 §2 尾注）。

---

## 2. 鸿蒙删除的 nxnative 键

设计 §2：删除"pso_mtl4、ipa、zip_store、pad_layout(Apple) **等 Metal/iOS 专属**"键。逐键清单与原因：

| 删除键 | nxnative 语义 | 删除原因 |
|---|---|---|
| `ipa` | iOS 应用默认 IPA 文件名（`org.nxnative.<titleID>` bundle ID 等） | iOS 打包面；鸿蒙出包是 HAP，由 `hap_bundle`/`hap_label`/`hap_icon` 取代 |
| `app_category` | App Store 分类 | Apple 商店元数据，鸿蒙无对应面 |
| `zip_store` | IPA 内不压缩存储的扩展名（odyssey `.aras:.bfsar:.bfstm:.mp4:.szs:.metallib`；totk `.zs:.bwav:.mc:.webm:.metallib`） | IPA 打包选项；鸿蒙 RomFS 走外置分卷（不进 HAP，4GB 上限，设计 §3 步骤 6），无 zip 面 |
| `pso_mtl4` | iOS 上经 MTL4Compiler 的管线编译 + 离线管线档案 + 拉取库 | Metal 4 专属编译器面；鸿蒙 Vulkan 侧管线策略待 M4 另立新键 |
| `present_snap` | present 快照环（`nvn_mtl.m`，拷贝每帧 present 让游戏 swapchain 纹理立即可复用，消除 GPU 帧间空转；iOS launcher Frame buffering 设置） | Metal 交换链机制；Vulkan 呈现模型不同。M4 若 Vulkan 路径需要同语义（present 后立即释放游戏 swapchain 纹理），以新键回流 |
| `pad_layout` | iPhone 虚拟手柄首发布局（simple/full/touch；`DmGamepad.m`） | Apple 触壳专属；鸿蒙触壳是 ArkTS/XComponent 面，布局由宿主 `gamepad_layout.h` 的 env（`TOTK_FACE_BUTTONS`/`TOTK_MINIMAL_LAYOUT`）承担 |

共删 6 键（43 → 37）。`feedback_snap` **保留**：其动因（同 render pass 写+采样同一附着）在 Vulkan 侧同样有 feedback loop 约束，是回放层的真实决策点，M4 复核。

---

## 3. derived profile（未注册版本）

对齐 `infra/nxnative/docs/development.md` 第 143 步语义：

- 档案未注册的版本（档案只注册了 update 而只给基础版、另一个 update、或文件 build ID 不同），或着色器表需要的版本不全在文件夹里，**照常可构建**：从该档案派生一个 profile 到 `<id>_build/profile/<id>/`——同样的键 + 该版本自己的 build 行 + 该版本自己文件做出的着色器表、链头、`c1_full.tsv`、`cb_extent.tsv`（同 provisional profile，**不做 heap check**）。
- 红线：注册 build 的重写裁剪与宿主修复**触及不到**另一个 build 的模块；脚本会点名哪些模块未注册。
- build 查找链：无档案的 title ID 先按 main build ID 反查（同 build 的区售版 → `same_build` 派生，用文件夹的 title ID 与 npdm 的 program ID），查不到才进 survey。
- 鸿蒙侧的现成案例：**totk 1.1.2**。nxnative 档案注册 1.4.3（`v720896`），本机转译产物是 1.1.2（`game-work/totk_0100F2C0115B6000/`，survey L118/127）→ M3 走 derived profile；1.4.3 的 patches.py（FRAME_STEP 等）不落 1.1.2 的 main，需 `MATCH_CUTS` 指纹登记或为 1.1.2 补 build 行与补丁（设计 §7 风险第 2 条）。

---

## 4. 新游戏接入 checklist（NSP → HAP，按序）

> 每步产物有 `.key` 指纹缓存（sha256 of 输入），输入不变即跳过（设计 §3；对齐 nxnative 阶段复用）。

1. **备料**：游戏的 NSP（基础版 + 可选 update）+ `prod.keys`。放入工作区。
2. **识别**：跑 `scripts/build_hap_from_nsp.py`（M2）。有档案 → 直接用；无档案 → survey（`<NSP>/<titleID>_survey/survey.md`：模块与 build ID / SDK / 引擎 / 图形 API 标记 / RomFS 容器 / 重写点位普查 / 与宿主钩子的导入对比）+ `new_game.py` 起草档案（身份、build 行、`modules` 按导入猜、内存分区猜测、`shader_groups` 按 RomFS 顶层、Hagi 类引擎预设 `op_mode handheld`）。版本不在注册内 → derived profile（§3）。
3. **game.tsv 定稿**：按本文键表分组填键——身份 3 / 版本 3 / 模块 2 / 内存 7 / 启动 4 / 图形行为 8 / 纹理 3 / 帧率 2 / 输入壳 2 / 字体 1 / 着色器拆分 2（继承 37）+ 鸿蒙组 6 = 43；带起时实测修正内存键（heap check 产物），短期跑不定的键（render_cond、tex_flush_upload、tex_dirty_upload、core_preempt、gpu_timestamps、feedback_snap、vconv_range、present_interval 及预设外的 op_mode）保默认。
4. **patches.py**：本游戏 build 的 CUTS/OBS/FRAME_STEP（先跑不加补丁的重写，按 boot 尸检逐个补）；sdk 模块裁剪走 sdk_patches；未注册 build 用 MATCH_CUTS 指纹登记。
5. **重写**：`patch_nso`（复用 nxnative）→ `<id>_build/patched/<模块>/`（patched.bin + cuts/obs/fixups/gates/sites TSV）。
6. **着色器表**：`DM_GAME=<id> shader_split.py` 生成 shader_map/facts/pairs/pso_pairs；补空 `shader_map.tsv.conflicts.tsv`、`bf_want.tsv`、`pso_manifest.tsv`（拷 Mario 头）；机器看不出的手工事实进 `shader_facts_manual.tsv`。（Vulkan/GL/GLSLC/Unreal pak/Trinity pack 类运行时生成着色器的捕获流程按 nxnative "What the game files decide at every build" 节走。）
7. **生成头三件**：`tools/prepare_runtime.py`（档案驱动）→ `generated/runtime_tables.h`（模块表/符号/重定位/gates，吃 §1.2/§1.3 键）+ `romfs_config.h`（占位）+ `shared_fonts_config.h`。
8. **字体**：`fonts_mode` 决定——`embedded`：`tools/make_shared_fonts.py` 用 Windows TTF 伪造 BFTTF 内嵌（生成 `shared_fonts_data.h` + 六行 rows，幂等可复现）；`firmware`：校验真 BFTTF（不可分发，外置）。
9. **RomFS**：`tools/make_romfs_runtime.py` → DMROMFS1 → TOTKRFS1 `runtime-index.bin` + `romfs_volume_mib` 等大分卷（末卷短），写实 `romfs_config.h`（SIZE/IMAGE_SHA256/INDEX_SHA256）。分卷外置导入，不进 HAP（4GB 上限）。
10. **出包**：template 实例化——`app.json5`（hap_bundle/label/versionName）、module.json5（hap_icon/ability_label）、CMake `GAME_{{ID}}`、`game_hooks.tmpl`（game_patches.c / game_aspect.c / game_version.h；`host_override/` 有文件则原样覆盖）→ `hvigor assembleHap` → 签名 HAP。
11. **装机验证**：`tools/run_cycle.sh` 真机构建-装机-点火-验尸循环；boot 推进的坑记 `PITFALLS.md`。
12. **固化**：跑通的版本补 `expected-v<ver>.json` 钉（ExeFS 源/重写产物/RomFS/字体 SHA256）+ `smoke.tsv` 冒烟路线 + 档案 README 笔记；`usage.tsv` 由 harness 生成自证键面。

---

### 出处索引

| 断言域 | 出处 |
|---|---|
| 键语义全集 / 档案结构 / Adding a game | `infra/nxnative/dm/games/README.md`（Keys L125–165、L3–20、L181–201） |
| 键解析与默认值校验 | `infra/nxnative/dm/games/game.py`（`header()`） |
| odyssey / totk 实值 | `infra/nxnative/dm/games/0100000000010000/game.tsv`、`.../0100F2C0115B6000/game.tsv` |
| 鸿蒙增删原则 / 七步流 / 模板参数化点 / 里程碑 | `game-work/nxnative_ohos_design.md` §2/§3/§4/§5 |
| 铁律与三种差异表达 / Layout / 状态 | `nxnative-ohos/README.md` |
| derived profile | `infra/nxnative/docs/development.md` L143 |
| runtime_tables 生成 | `game-work/prepare_runtime_ohos.py` |
| 字体伪造与内嵌 | `game-work/make_shared_fonts.py` |
| RomFS 分卷与 romfs_config | `game-work/make_romfs_runtime.py`；宿主 `mario-odessy-harmony/entry/src/main/cpp/host/romfs.c` |
| 字体加载器 / TOTK_SHARED_FONTS | `mario-odessy-harmony/entry/src/main/cpp/host/shared_font.c` |
| 内存布局 env 面 | `mario-odessy-harmony/entry/src/main/cpp/host/services.c` |
| setenv 面 / 解包子目录 | `mario-odessy-harmony/entry/src/main/cpp/game_napi.cpp` |
| hap_* 现值 | `mario-odessy-harmony/AppScope/app.json5`、`entry/src/main/module.json5`、`entry/src/main/resources/base/element/string.json` |
| expected 钉样例 | `infra/MacNativeGames/games/totk/assets/expected-v1.4.3.json` |
| 设置表 | `infra/nxnative/dm/games/settings.tsv` |
| totk 1.1.2 不匹配 / 着色器表规模 / TOTK_* 计数 | `game-work/logs/nxnative_ohos_survey.md` |
