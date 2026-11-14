// nvn_vk.h —— NVN 图形 API → OHOS Vulkan 后端（第一批：device/surface/swapchain/纹理上传）
// 移植自 infra/dontstarve-harmony/dm/gfx/nvn/nvn_vulkan.c（7402 行产品级 NVN→Vulkan 重放层，
// 宿主 d2r-mobile dm 工程，与 MacNativeGames odyssey runtime 同血统不同分支）。
// 分层对齐 dontstarve：它的 hle_nvn.c（523 名字调度）对应我们的 platform/gfx_bridge.c；本目录只做
// 「后端半边」——不碰 NVN 函数表，宿主经本头文件的 C 接口喂对象/字节进来。
//
// 第一批范围（骨架通电）：vkCreateInstance + VK_OHOS_surface + 物理设备/队列/设备 + swapchain
//（实测绿写法全套）+ BC/ASTC 能力探测 + BC 映射表 + vkCreateImage + 设备内存 + staging 上传
// + 出图冒烟（acquire+清除+present）。commandBuffer 重放/管线/着色器在第二、三批。
//
// 红线对齐：本目录不 include 任何 platform/ 引擎头；窗口句柄由宿主显式送入（nvn_vk_set_native_window），
// gfx_bridge.c 的 present 清色链已在 round23批1 接通，mtl_* 全量接线在批 3（docs/round23-nvn-wiring-plan.md）。
#pragma once
#include <stdint.h>
#include <stddef.h>

// M7k P1（2026-11-07）：小图（≤1MB）id-img LRU 让位直通。默认 1；改 0 = 精确回到 M7j 行为
// （让位仍要求 binds>=M5Z_EVICT_MIN_BINDS）。定义在共享头，nvn_vk.c 的开关回读行与
// gfx_bridge.c 的判定共用同一编译期默认值。
#ifndef M7K_SMALL_EVICT_ON
#define M7K_SMALL_EVICT_ON 1
#endif

// M7l P1（2026-11-07）：「绑上了但内容是零」的归属取证。默认 1；改 0 = 与 M7k 行为逐位相同
// （本轮全部新增探针只读：上传点源字节 nz 统计 + 场景族上传的解排布后签名 + 动态观察名单）。
// 定义在共享头：nvn_vk.c 打回读行/解排布签名，gfx_bridge.c 打 [m7l] up 与动态名单，共用同一默认。
#ifndef M7L_ON
#define M7L_ON 1
#endif

// 贴图描述符直接复用 donor 血统的 nvn_texdesc（third_party/d2r_gfx/gfx/nvn/nvn_tex.h，
// platform/nvn_tex.c 已参与 arm64 构建）——gfx_bridge.c 的 tex_h 内嵌同一结构，第二批接线零转换。
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_tex.h"   /* 迁入 platform/nvn_vk/ 深度 +1：../ 只到 cpp/platform，须 ../../ 回 cpp 根（host→platform 同级更名的 gfx_bridge/gfx_probe 五行 ../third_party 深度不变、无需改） */

#ifdef __cplusplus
extern "C" {
#endif

// ---- 接线（尾巴①）：surface 来源 ----
// OHNativeWindow*（XComponent OnSurfaceCreated 回调给的 window）由宿主塞进来。
// 已接线（round23批1落点1）：game_napi.cpp surface_set 转发本函数（窗口销毁传 NULL），
// x86_64 壳层 game_napi.cpp 内弱符号空实现兜底。窗口到达且未起层时本函数触发懒起层。
void nvn_vk_set_native_window(void *native_window, uint32_t width, uint32_t height);

// ---- 起层 ----
// 惰性起层（幂等）：instance+surface+device+swapchain+能力探测。成功返 1。
// 失败（含没窗口/SMO_VK=0/驱动不给）返 0 并打一行日志，宿主保持 stub 模式不变。
int nvn_vk_init(void);
void nvn_vk_shutdown(void);
int  nvn_vk_is_up(void);
// 压缩格式能力位图（起层后有效）：1 BC1_RGB 2 BC1_RGBA 4 BC2 8 BC3 16 BC4 32 BC5
// 64 BC6H 128 BC7 256 ASTC4x4；0 = 未起层或全不认（Maleoon 920 实测即此）。
uint32_t nvn_vk_bc_caps(void);
// 一行状态（ASCII，日志/汇报用），例 "vk up dev=Maleoon920 fmt=BGRA8 1260x2720 bc=0x0"
const char *nvn_vk_status_line(void);

// ---- 出图冒烟（第一批验证链路；带源图的完整 present 第二批） ----
// acquire → 经典通道清色 → present，全同步。成功返 1。颜色 0-255。
int nvn_vk_present_clear(uint32_t r, uint32_t g, uint32_t b);

// ---- M7d (2026-11-07)：实验开关位（默认全 0）。bit2=fmt(清色 r/b 对调) bit4=same(强制真图路径) bit8=clear(纯绿)。
// 每 present 由 gfx_bridge 调一次；smo_m7d_pres 的记账/100s 汇总在 nvn_vk.c 内部完成。 ----
int nvn_vk_m7d_flags(void);

// ---- M3h：离屏清屏（最小 NVN→Vulkan 回放验证） ----
// 在专用离屏 RGBA8 图（1280x720、TRANSFER_DST|SAMPLED）上录 vkCmdClearColorImage 并同步提交
//（独立命令池/栅栏/图，与上传路径的 g_cp/g_ufence、出图路径的 g_pfence 零共享，避免跨线程竞争）。
// 不走交换链、不碰 present 路径（g_ohos_nb_stripped 门禁与本路径无关）。
// 返回 VkResult 值（0 = VK_SUCCESS），-1 = 后端未起，-2 = 离屏目标建不起来，-3 = 命令缓冲分配失败。
// *out_img（可 NULL）= 离屏 VkImage 句柄，取证日志用。
int nvn_vk_clear_color_offscreen(const float rgba[4], unsigned long long *out_img);
/* M4f P1：把 clear 收到「每帧一次」。frame = 调用方的帧锚点（gfx_bridge 的 WindowAcquireTexture 计数）。
   同一 frame 内对**同一目标图**只落地第一次 clear，后续不再提交（draws 不受影响、按序累积），
   返回 0 且 *suppressed（可 NULL）=1。语义：帧内首清胜出（颜色差异计入 dup_color）。 */
int nvn_vk_clear_color_offscreen_frame(unsigned long long frame, const float rgba[4],
                                       unsigned long long *out_img, int *suppressed);
/* M38F A（只读，log-only，零行为改动）：清屏调用**前一刻**由 gfx_bridge 调，打「即将被清的目标
   身份（slot/id/ext/lay/深度附件）+ 该图此刻的自有回读签名」。开关 smo_m38f 关态零输出。 */
void nvn_vk_m38f_clear_note(unsigned long long frame, const float rgba[4]);
/* 当前统计所属帧 + 该帧尝试/抑制次数 + 生效清屏色 + 累计（抑制总数/颜色差异数）。 */
void nvn_vk_clear_stats(unsigned long long *frame, unsigned long long *att, unsigned long long *supp,
                        unsigned long long *supp_total, unsigned long long *dup_color, float color[4]);

// ---- M3i（2026-11-03）：SET_RENDER_TARGETS 真绑定到离屏 RT ----
// 把离屏图当 color attachment：懒惰建 render pass + image view + framebuffer，并在「按 submit 批次」
// 的命令缓冲里记 barrier（→ COLOR_ATTACHMENT_OPTIMAL）+ viewport/scissor（尺寸取自 RT）。
// 命令缓冲在 nvn_vk_rt_flush()（清屏前 / 命令流走完时）统一提交并等栅栏——**不**每次绑定都 submit
// （实测 24.2 次 SET_RENDER_TARGETS/submit，逐次提交会把 60fps 打穿）。
// n = 记录体里的颜色 RT 数（0 也接受：仍绑离屏 RT）；返回 0 = 已记录，-1 = 后端未起，-2 = RT 建不起。
// *out_img（可 NULL）= 离屏 VkImage；*out_w/*out_h（可 NULL）= RT 尺寸（本轮回退 1280x720）。
/* M4c（2026-11-03）：key = 颜色 RT 0 的贴图句柄（SET_RENDER_TARGETS 的 tex0），rw/rh = 该纹理快照
   尺寸（宿主 tex_h->d.width/height）。按 key 取/建 per-RT VkImage（尺寸来自快照），clear/draw/回读全走它；
   key/rw/rh 缺失或建图失败 ⇒ 回落宿主 1280x720 并计 rt_fallback。SMO_KEEP_HOST_RT=1 强制旧行为。 */
/* M4e：tex_id = 该 RT 对应的游戏纹理 id（-1 = 未知）。源图替换（route②）靠它把「blit 绑的纹理」
   对回「我们画的那张 RT 图」。 */
/* M4r P1：dkey = 同一次 SET_RENDER_TARGETS 的 depth 存储身份（0 = 无深度）。
   n>0 且 dkey 能对上同尺寸深度槽 ⇒ draw 走「颜色+深度」通道（var 1/2）；
   n==0 且 dkey 对上 ⇒ 不切颜色目标，draw 走「仅深度」通道（var 3/4），不再回落宿主图。 */
/* M4t P1：fmt = 该 RT 的游戏侧 NVN 格式（0 = 未知）。route② 的反向登记（「哪张纹理 id 与这个 RT
   同尺寸同格式」）需要它，否则 1600×900 的两张 RT（fmt 0x3d 场景图 / fmt 0x25 另一张）分不开。 */
int nvn_vk_rt_bind(int n, unsigned long long key, unsigned rw, unsigned rh, int tex_id, unsigned fmt,
                   unsigned long long dkey,
                   unsigned long long *out_img, unsigned *out_w, unsigned *out_h);
/* M4q P1：深度目标留证槽。SET_RENDER_TARGETS 的 depth（NVNtexture*）此前完全没交给回放层，
   于是 n=0 的纯深度 pass 在宿主侧等于「没有目标」，1024×1024 深度图在日志里根本没有身份。
   本入口只按游戏声明的尺寸/格式建一张深度 VkImage 并打一行 [h] rt depth slot（本轮**不**接进
   renderPass——深度附件是下一个里程碑）；建不起/格式不支持只计数，不影响颜色路径。
   key 用存储身份（池数据地址），与颜色槽同口径；w/h/fmt 来自游戏 descriptor。返回 0 = 已记录。 */
int nvn_vk_rt_depth_bind(unsigned long long key, unsigned rw, unsigned rh, unsigned fmt, int tex_id);
/* M4q P1 取证：深度槽计数（任一可 NULL）：收到次数 / 建成槽数 / 建失败 / 格式不支持跳过。 */
void nvn_vk_rt_depth_stats(unsigned long long *binds, unsigned long long *slots,
                           unsigned long long *fail, unsigned long long *badfmt);
/* M4r P1：深度回读探针（拷贝当前深度槽到 host buffer 并统计 one/zero/lt1；上限 12 次）。
   返回 0 = 已统计并落一行 [t] dsig；1 = 跳过（无深度槽/超上限/尺寸过大）；负值 = 失败。 */
int nvn_vk_depth_probe(const char *tag);
/* M4r P1 验收计数（任一可 NULL）：挂上深度的颜色绑定数 / n=0 纯深度 pass 绑定数 / 挂失败数，
   以及真正录进 CB 的「颜色+深度」/「仅深度」draw 笔数、深度管线变体建成/失败数。 */
void nvn_vk_depth_use_stats(unsigned long long *attach, unsigned long long *only, unsigned long long *skip,
                            unsigned long long *dcolor, unsigned long long *donly,
                            unsigned long long *pipe_ok, unsigned long long *pipe_fail);
/* M4s P3：在 n=0 纯深度 pass 期间被 skip 掉的 draw 笔数（回答 dpass_only 里为什么没有一笔 draw）。 */
unsigned long long nvn_vk_depth_dpass_skip(void);
/* M4s P3：纯深度 pass 期间到达的 DrawArrays 笔数（该 opcode 在 gfx_bridge 里只有记账桩、从不重放）。 */
unsigned long long nvn_vk_depth_dpass_arrays(void);
// 提交当前 RT 批次。1 = 无待提交内容（空操作）；0 = 提交成功；负值 = 提交失败（-4 分配失败）。
int nvn_vk_rt_flush(void);
/* M4c 取证：已建 per-RT 目标数 / 回落宿主图次数 / 目标切换次数。 */
void nvn_vk_rt_targets(int *slots, unsigned long long *fallback, unsigned long long *switches);
// 取证计数（任一参数可 NULL）：绑定总次数 / 其中 n>0 的次数 / 批次提交成功 / 失败。
void nvn_vk_rt_stats(unsigned long long *binds, unsigned long long *nonempty,
                     unsigned long long *flush_ok, unsigned long long *flush_fail);
/* M4ar P1：最近一次 SET_RENDER_TARGETS 的口径（n=颜色目标数 / 请求尺寸 / 是否回落宿主图）。
   用途：给「新 key 的 draw 为什么落在 hostRT 1280×720」分类 —— n=0（没有颜色目标）
   与「有目标但解不出身份」是两种完全不同的病，日志必须能分开。 */
void nvn_vk_rt_last_bind(int *n, unsigned *w, unsigned *h, int *fallback);
/* M4ar P2：解不出身份的 RT 绑定改用「按请求尺寸建的名义槽」的次数。 */
unsigned long long nvn_vk_rt_nominal(void);
/* M4bn P1（只读，0 行为改动）：把「最近一次 SET_RENDER_TARGETS 的深度声明」交给回放层，供 draw 时对账
   （世界 draw 全 dk=0 的归因）。raw = 游戏传进来的深度对象指针（0 = 游戏明确说没有深度）；
   resolved = 我们是否解析出尺寸/格式（1 是 / 0 否）；n = 该次 SET_RENDER_TARGETS 的颜色目标数。
   必须在对应的 nvn_vk_rt_bind 之前调用（同线程、按序）。 */
void nvn_vk_dk_note(int n, unsigned long long raw, int resolved, unsigned w, unsigned h, unsigned fmt);
/* M4bo P1（只读，0 行为改动）：「本帧哪一笔 draw 真的写了深度 / 后处理深度从哪来」的声明侧对账。
   每次 SET_RENDER_TARGETS 推一条（与 nvn_vk_dk_note 同一时刻）；只有 depth != NULL 才记账/打印，
   并在下一条声明到来时补上 next_draws（两条声明之间的 draw 笔数）。 */
void nvn_vk_dp_decl(int n, unsigned long long raw, int resolved, unsigned w, unsigned h, unsigned fmt);

// ---- M3l（2026-11-03）：离屏 RT 上的三角形 draw 路径 + 一次性回读 ----
// M3m 起 draw 不再记在 clear 的命令缓冲里：nvn_vk_draw_tri() 用 draw 专属命令池/栅栏单独
// alloc/begin/record/end/submit/wait，调用点 = clear 返回之后（gfx_bridge 的 clear 桥），
// 因此「draw 崩」与「clear 崩」在日志上是两条互不相干的链路。
// 录制内容：renderPass(loadOp=CLEAR 深色) → bind pipeline → 动态 viewport/scissor →
// vkCmdDraw(3,1,0,0) → end。无顶点输入/无深度/blend 关，shader 由离线编译的 tri_spv.h 提供。
// 第 TRI_RB_AT（默认 3，SMO_TRI_RB_AT 可覆盖）次 draw 后回读一次，写 tri.ppm（P6）并 stat 自校验。
// 编译期开关 SMO_TRI_DRAW=0 可整体关掉（默认开）。
// 返回 0 = 本次已画并等到栅栏；1 = 本帧不画（无 RT/布局不符，不算失败）；负值 = 失败。
int nvn_vk_draw_tri(void);
// 取证计数（任一可 NULL）：已记录 draw 次数 / 记录或建管线失败次数 / 回读次数。
void nvn_vk_draw_stats(unsigned long long *draws, unsigned long long *fail, unsigned long long *readback);

// ---- M3o（2026-11-03）：离屏 RT → 屏幕（软件上屏，不经 swapchain） ----
// 把离屏 RT（1280x720 R8G8B8A8）回读到宿主内存，再经 native window 的
// RequestBuffer/mmap/memcpy/Flush 搬到 XComponent 窗口（居中 1:1，不缩放；换屏频率由
// SMO_PRESENT_EVERY 控制，默认每 8 次 present 一次）。必须在既有游戏/渲染线程调用
// （gfx_bridge 的 nvnQueuePresentTexture 同线程）。返 1 = 本帧屏幕归本路径（已上屏或主动
// 降采样跳过）；0 = 本帧没接（后端没起/还没画过/无窗口/失败），调用方可回落纯色清屏。
int nvn_vk_present_offscreen(void);
// 取证计数（任一可 NULL）：调用数 / 上屏成功 / 降采样跳过 / 失败。
void nvn_vk_present_stats(unsigned long long *calls, unsigned long long *ok,
                          unsigned long long *skipped, unsigned long long *fail);
// M4x P1：上屏链路拆解。ds=降采样跳过；small=源 <1280x720 跳过；nosrc=保留（恒 0）；
// hit=用「本帧最后一笔颜色 draw 的图」上屏；cur=该图正好是当前目标；miss=候选校验失败退回。
void nvn_vk_present_skip_split(unsigned long long *ds, unsigned long long *small,
                               unsigned long long *nosrc, unsigned long long *hit,
                               unsigned long long *cur, unsigned long long *miss);
// M4ag P1：**游戏自己说要看哪张**。gfx_bridge 在 nvnQueuePresentTexture 里把
// win->tex[idx]（nvnWindowAcquireTexture 轮转给的那张窗口纹理对象）解析成存储身份
// （池数据地址 key，拿不到时退对象指针）后调用本函数登记；下一次 nvn_vk_present_offscreen
// 优先以 key 命中的 h_rt_slot 为源。key=0 ⇒ 解析失败，present 记 texmiss 并回落旧逻辑。
void nvn_vk_present_src(unsigned long long key, unsigned w, unsigned h, int texid, int idx);
// 台账：src_tex=游戏指定槽命中次数 / miss=拿不到映射（回落） / nolay=命中了但布局不可拷。
void nvn_vk_present_tex_stats(unsigned long long *src_tex, unsigned long long *miss,
                              unsigned long long *nolay, unsigned long long *ovr);
// M4ag P3：attr「绑定区间不足但客体可读 ⇒ 放行」的属性数（与 attr_ph 对账）。
unsigned long long nvn_vk_attr_over(void);

// ---- CPU 填色兜底（e1e2 G1）：Vulkan 起不来时（模拟器无驱动 vkCreateInstance=-9），不经 GPU
// 直接把清屏色写进 native window：RequestBuffer → GetBufferHandleFromNative → mmap → 填 RGBA
// → FlushBuffer。同一 native window（尾巴①塞进来的），Vulkan 路径保留不动，等真机。成功返 1。
int nvn_vk_cpu_clear(uint32_t r, uint32_t g, uint32_t b);

// ---- 纹理（第一批：BC 映射 + vkCreateImage + 映射内存 + staging 上传） ----
typedef struct nvn_vk_tex nvn_vk_tex;

// 按描述符建 VkImage：NVN 格式 → VkFormat（BC 表 + 设备能力探测 + 未压缩回退），
// 用法位按格式能力取舍，OPTIMAL tiling，设备本地内存，含采样视图（NVN swizzle 映射）。
// 失败返 NULL（格式不认/无用法位/建图或分配失败），不打日志风暴（每类原因一行一次）。
nvn_vk_tex *nvn_vk_tex_create(const nvn_texdesc *d);
/* M40P：tex_create 成功后按 tex id 记建图次数（同 id 重复建图率，smo_m40p 关态零副作用） */
void nvn_vk_m40p_idrep(int id, unsigned w, unsigned h, unsigned fmt);

// 上传「已线性排布」的一级一层：linear_bytes 指向本级本层线性字节（压缩格式 = 线性排布的
// 压缩块行，未压缩 = 像素行），row_pitch = 行字节数（0 = 按最小行距）。
// 内部：staging 拷贝 → 屏障 → vkCmdCopyBufferToImage → 屏障转 SHADER_READ_ONLY（dontstarve
// 第 85 轮实测：传完不转可采样 = 1131 次绑定全错）。同步等栅栏后返回。成功返 1。
// 注意：若 tex_create 时格式走了 BC→未压缩回退（soft_bc），这里的字节必须是已解码的像素，
// 解码器（nvn_bcdec）第三批接。
int nvn_vk_tex_upload_level(nvn_vk_tex *t, unsigned level, unsigned layer,
                            const void *linear_bytes, uint32_t row_pitch);

// 便捷路径：直接吃池内存（我们的 tex_h->mem 就是宿主指针，dontstarve 要解址+写保护那套全省）。
// LINEAR/LINEAR_RENDER_TARGET 存储（flags & 0x50）逐级逐层直传；块线性存储返 0（要 Tegra
// 解排布，deswizzle 收编在第二批——third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c 已在树）。
// len 是整个贴图存储字节数（越界夹取按 nvn_tex_layout 实算，不信调用方）。
// dbg_id：M4a 内容签名探针的纹理 id；0xFFFFFFFF = 不打（Initialize 试传路径用）。
int nvn_vk_tex_upload_storage(nvn_vk_tex *t, const void *pool_bytes, uint64_t len, uint32_t dbg_id);

// ================= M40J（2026-11-09，纯只读探针 smo_m40j） =================
// 三问：Q1 画面里那笔大 draw 绑的纹理 id/池 storage 区间；Q2 整轮 boot 有没有拷贝 dst 落到该区间；
// Q3 reloc 的 122 MB 拷贝去了哪。关态（smo_m40j=0）零客体读、零日志、行为逐位不变。
// reg：首次 bind 时登记 (id, 池地址, 跨度)；cp：所有「写客体」拷贝喂一条（tag 1=romfs 2=fs_host
// 3=UpdateUniformBuffer 4=gmem reloc 宿主侧）；tick：每个 submit 调一次（每 600 submit 出一条 cpsum）。
void nvn_vk_m40j_reg(int id, uint64_t store, uint64_t span);
void nvn_vk_m40j_cp(unsigned tag, const void *dst, uint64_t len, uint64_t src);
void nvn_vk_m40j_tick(void);

// M7l P1：场景族 draw（vs 前缀 24702883，m7k_scenedraw 观测）是否已出现过。只读取证门：
// [m7l] up / [m7l] sig / 动态观察名单只看它出现之后的上传（id2249 类场景贴图在此之后才被准入）。
// M7L_ON=0 时恒返 0（探针全静默）。
int nvn_vk_scene_seen(void);

// 查询/销毁。view 返回 VkImageView（void* 免得本头拖 vulkan 依赖，第二批描述符用）。
void       *nvn_vk_tex_view(const nvn_vk_tex *t);   // NULL = 未建
uint32_t    nvn_vk_tex_width(const nvn_vk_tex *t);
uint32_t    nvn_vk_tex_height(const nvn_vk_tex *t);
int         nvn_vk_tex_soft_bc(const nvn_vk_tex *t); // 1 = BC 回退成未压缩，字节需先解码
uint32_t    nvn_vk_tex_vkfmt(const nvn_vk_tex *t);   // M4v P1：实际建图用的 VkFormat 值（0 = 无对象）
void        nvn_vk_tex_destroy(nvn_vk_tex *t);
/* M6a B：延迟销毁（不 vkQueueWaitIdle）。drain 只在「所有已提交工作都已等到栅栏」时调用。 */
void        nvn_vk_tex_destroy_deferred(nvn_vk_tex *t);
void        nvn_vk_tex_graveyard_drain(int queue_done);
void        nvn_vk_tex_graveyard_stats(unsigned long long *deferred, unsigned long long *freed,
                                       unsigned long long *sync_fallback, unsigned *pending);

// ---- M3r（2026-11-03）：真实 shader 管线 + 第一笔真实 DRAW_ELEMENTS ----
// 契约（全部可在后端未起时安全调用，返回值只用于计数）：
//   program_set_keys : ProgramSetShaders 里算好的两阶段 key（stage0→VS、stage1→FS），按 program 指针登记；
//                      同一 program 的两次 SetShaders 调用各自补齐一路，查不到 key 的那一路留空。
//   bind_program     : 0 = 该 program 的 VS/FS key 都在 cache index 里；1 = 缺（该 draw 跳过）；-1 = index 读不到。
//   set_vertex_*     : 录制侧的原始数组（attrib = count×4B 的 NVNvertexAttribState，stream = count×8B）。
//   bind_vertex_buffers: NVNbufferRange{addr,size} 数组（GPU 地址 == 宿主地址，可直接读）。
//   draw_elements    : 0 = 已录进 draw 批次；1 = 主动跳过（不算失败）；负值 = 失败。
//   draw_flush       : 一批结束（每个 submit 一次）：EndRenderPass/End/Submit/Wait + 释放动态缓冲；
//                      M4h P1：首笔真实游戏 draw 之后的 flush 回读 rt_first.pam，之后每 300 帧回读一次。
void nvn_vk_program_set_keys(uint64_t prog, const char *vs_key, const char *fs_key);
/* M4bd P1（只登记，不参与渲染）：shader 自带的 c1 常量段。
   NVN 控制段 +0x700/+0x704 = c1 相对指令区起点的 [offset,total)；c1 = code[offset,total)。
   硬件 c 段 k ← NVN bind 槽 k-3，所以 c1 没有对应 bind 槽：它是随 shader 编译出来的常量块。 */
void nvn_vk_shader_c1(const char *key, int stage, const void *ctl, uint64_t code_addr);
int  nvn_vk_bind_program(uint64_t prog);
/* M3u P3：program→key 缺口取证（registered=表内条目数，paired=VS+FS 齐备数，
   cap_hit=表满被丢弃的登记次数，bind/bind_miss=BindProgram 命中情况）。只读计数。 */
unsigned nvn_vk_programs_paired(void);
unsigned nvn_vk_programs_registered(void);
unsigned long long nvn_vk_program_cap_hits(void);
unsigned long long nvn_vk_program_binds(void);
unsigned long long nvn_vk_program_bind_misses(void);
unsigned long long nvn_vk_pipe_cap_hits(void);   /* M3v P1：管线表撞满次数（skip_nopipe 成因拆分） */
unsigned long long nvn_vk_pipe_skip_gmem(void);  /* M3x P1：binding 17 (GMem) 未接线而跳过的管线数 */
/* M5v P1：gmem 后台账（heap 状态 / 搬迁笔数与字节 / slot 用尽 / 跨度超限），与 [r] draw totals 同节流点 */
void nvn_vk_gm_report(unsigned sub);
/* M103A P3（2026-11-13）：`[tex] miss class` 改按**时间**触发（≥30 s 一行）——3D 段 0.2 fps 时
   旧的「每 300 submit」在整段里只能落一次，低帧率段等于仪表全盲。由 gfx_bridge 的 30 s tick 调用；
   函数内部自带同一道 30 s 门，所以与 submit 报表路径共用拍子、不会重复打印。 */
void nvn_vk_m103a_tick(unsigned sub);
/* M5w P0：[hang] 行的第二行——在飞批次成分（ndraw/verts_max/tops）、上一批实测 us、隔离表。只读。
   注意名字必须与 vk_boot.c 的弱引用一致（runtime_hang_batch_dump）。 */
void runtime_hang_batch_dump(void);
unsigned long long nvn_vk_pipe_skip_bigfs(void);  /* M4k P1③：>256KB 片元着色器按需跳过数 */
/* M4k P0-2：宿主侧阻塞计数（up=上传栅栏 >5s；destroy=纹理销毁 vkQueueWaitIdle >300ms） */
void nvn_vk_stall_stats(unsigned long long *up, unsigned long long *destroy);
/* M3v P3：skip_noprog 拆分（noentry=program 表无此项/缺一路；nokey=key 不在 shader_cache；cache_keys=索引键数） */
void nvn_vk_skip_noprog_split(unsigned long long *noentry, unsigned long long *nokey, int *cache_keys);
/* M5w：隔离闸门（M5e 黑名单 + C 默认 + 实测 learned）累计跳掉的 draw 笔数 */
unsigned long long nvn_vk_skip_heavy(void);
unsigned long long nvn_vk_skip_memveto(void);   /* M5x: memory-explosion veto skips */
unsigned long long nvn_vk_skip_cfg(void);        /* M6y: SPIR-V CFG veto skips */
void nvn_vk_bind_storage_buffers(int stage, int first, int count, const void *ranges);   /* M5x: gmem address source */
void nvn_vk_memveto_report(unsigned sub);   /* M5x: per-key veto ledger */
/* M4bw P1（只读）：skip_noprog 按目标 RT 身份（key+texid+ext，不用槽号）与「缺哪一路/包里有没有 spv」汇总；
   pair 门（(vs,fs) 对门控）顺带按目标归因。只读，不改任何判定。 */
void nvn_vk_nw_report(unsigned sub);
/* M4h P1：skip nokey 的 (vs,fs)→次数 聚合表在报告点 dump top20（含 spv.bin 有/无标记）。
   sub = 当前 submit 序号，只用于日志对齐；可在任意时刻重复调用（非破坏性）。 */
void nvn_vk_skip_nokey_dump(unsigned sub);
/* M4b P1（2026-11-03）：运行期去重累计设备实际请求的 (vs_key, fs_key) 配对，每 60s append 到
   files/pair_want.txt（行：vs fs prog nreg ndraw）。P2 的配对重烘只烘这份清单。 */
void nvn_vk_pair_want_dump(int force);
int  nvn_vk_pair_want_count(void);
void nvn_vk_set_vertex_attrib_state(int count, const void *raw);
void nvn_vk_set_vertex_stream_state(int count, const void *raw);
void nvn_vk_bind_vertex_buffers(int first, int count, const void *ranges);
/* M3s（2026-11-03）：UBO 真实字节——每次绑定把客体内存 {addr,size} 拷进 arena slot，draw 时作 dynamic offset。 */
void nvn_vk_bind_uniform_buffers(int stage, int first, int count, const void *ranges);
/* M3y P2（2026-11-03）：真实纹理槽。slot = NVN BindTextures 的 first+i，对应 GLSL binding 32+i。
   t == NULL 表示该槽回到 1x1 白占位（解析失败/未上送时行为与 M3x 一致）。 */
void nvn_vk_bind_texture_slot(int slot, nvn_vk_tex *t);
/* M4aa P1（2026-11-03）：该槽的**游戏采样器**（NVN nvnSampler 参数）。宿主在绑定纹理时按句柄的 sid
   查自己的 sampler 台账后送入；本层按 (wrapU,wrapV,wrapW,minF,magF) 组合缓存 VkSampler 并只在该槽
   采样器变化时抬描述符代次。wu < 0 = 该 sid 未登记（保持旧的 NEAREST/REPEAT/maxLod=0 行为并计数）。
   LOD 只记录不生效：上传只填 level 0，建 sampler 一律 minLod=maxLod=0。 */
void nvn_vk_texture_slot_sampler(int slot, int sid, int wu, int wv, int ww,
                                 int minf, int magf, float lod_min, float lod_max);
/* M4e（2026-11-03）route②：源图替换——把一个裸 VkImageView（= 我们自己的 RT 图/其采样影子）挂到槽上。
   view == NULL 等价 nvn_vk_bind_texture_slot(slot, NULL)。 */
void nvn_vk_bind_texture_slot_view(int slot, void *view);
int nvn_vk_texture_slot_matches_view(int slot, void *view);
/* M4n P3（取证元信息）：槽的绑定来源，由 gfx_bridge 在 bind_textures 回放时喂进来——nvn_vk.c 不反查
   游戏纹理表（快照纪律）。kind 取值见 NVN_VK_SLOT_*；只在全屏 draw 取证行里打出来，不参与任何控制流。 */
enum { NVN_VK_SLOT_NONE = 0, NVN_VK_SLOT_RT_SELF = 1, NVN_VK_SLOT_RT_OTHER = 2,
       NVN_VK_SLOT_TEX = 3, NVN_VK_SLOT_PLACEHOLDER = 4,
       NVN_VK_SLOT_DEPTH_NR = 5 };   /* M4p P2：深度纹理存在但未上送（≠「无纹理」） */
/* M4t P1：store = 该纹理快照的池数据地址（texsnap.mem）。RT 槽的 key 就是同一个池地址，
   所以「绑定的纹理是不是我们正在画的 RT」有**精确**判据，不必只靠尺寸+格式猜。 */
void nvn_vk_tex_slot_info(int slot, int tex_id, unsigned w, unsigned h, int fmt, int kind,
                          unsigned long long store, int uploaded);
/* M4bk P1（只读取证）：该槽「为什么没有真数据」的归因码 + 绑定热度/快照字节数。由 gfx_bridge 在
   bind_textures 回放时喂进来，只在 [m] sentinel 行里打出来，不参与任何控制流。
   code：1=已上送；-2..-12 = 上送失败原码（与 [t] skip 的 why= 同口径）；-20=快照缺失；
   -30=建图被热度门槛挡 / -31=建图上限 / -32=建图失败 / -33=渲染未就绪 / -34=描述不符。 */
void nvn_vk_tex_slot_why(int slot, int code, unsigned binds, unsigned long long bytes);
/* M5z P0：该槽「空，但 id 有可用快照」⇒ 挡它的只有我们自己的配额/门槛（给图就能修）。
   只喂 [tex] miss 报表，不参与任何控制流。 */
void nvn_vk_tex_slot_fixable(int slot, int ok);
/* M5z P1：该槽此刻有图、且这张图是「LRU 让位换来的」⇒ 修前口径下它是白占位（同 run 反事实）。 */
void nvn_vk_tex_slot_fixsrc(int slot, int ok);
/* M4t P1 取证：RT↔tex id 反向登记的结果（by_store=池地址精确命中 / by_desc=尺寸+格式+顺序 / links=总条数）。 */
void nvn_vk_rt_link_stats(unsigned long long *by_store, unsigned long long *by_desc,
                          unsigned long long *links, unsigned long long *ambiguous);
/* 该纹理 id 是否是我们画的某张 RT：0 = 不是；1 = 是且正是当前渲染目标（附件不能同时被采样 ⇒ 走影子副本）；
   2 = 是另一张 RT（可直接采样，转一次布局即可）。 */
int nvn_vk_rt_tex_kind(int tex_id);
/* 为 id 对应的 RT 准备一个可采样的 view（返回 void* VkImageView，失败 NULL 并写 why 负码）。
   只在该批 draw 已 flush、无在飞 CB 时调用（gfx_bridge 的切槽处）。 */
void *nvn_vk_rt_sample_prepare(int tex_id, int *why);
/* M4w P1：按**存储身份**（池数据地址 = RT 槽 key）反查 RT 槽，不依赖 tex_id。
   返回 0=未命中 / 1=命中且是当前渲染目标（调用方须走影子副本）/ 2=命中别的 RT。
   *slot_out = 槽号（未命中 -1），*tx_id_out = 该槽已知 id（-1=未知）。 */
int nvn_vk_rt_lookup_store(unsigned long long store, unsigned w, unsigned h, int fmt,
                           int *slot_out, int *tx_id_out);
/* 绑定期把 id 补登到 key==store 的 RT 槽（只写 tex_id，不改渲染状态；幂等）。 */
void nvn_vk_rt_link_id_by_store(unsigned long long store, int id);
/* M6a A：零内容被采样纹理 -> RT 身份解析（many-to-one id 别名 + store 精确 + 唯一 ext/fmt）。
   src: 1 = 已有 id 链接 / 2 = store 精确 / 3 = 唯一 ext+fmt 候选。cand_* 只作取证。
   allow_extfmt 由调用方给（只在「这张 id 上送过且首 4 KiB 全零」时为 1）；命中后登记别名，不建图。 */
int nvn_vk_rt_tex_kind_zero(int id, unsigned w, unsigned h, int fmt, unsigned long long store,
                            int allow_extfmt, int *src, int *cand_slot, int *cand_texid,
                            int *cand_img, int *cand_n);
/* M6a A 取证：该 id 是否被记录为「上送内容首 4 KiB 全零」。 */
int nvn_vk_tex_zero_id(int id);
/* M6a A 取证：别名条数/命中/溢出 + 同 store 别名 / 唯一 ext+fmt / 多候选放弃 各修掉多少。 */
void nvn_vk_rt_alias_stats(unsigned long long *n, unsigned long long *hit, unsigned long long *ovf,
                           unsigned long long *by_store, unsigned long long *by_extfmt,
                           unsigned long long *multi);
/* M6b B：同 ext+fmt 多候选时靠「本 submit 唯一写者」消歧的命中次数（src=4 / by=order+writers）。 */
void nvn_vk_rt_order_stats(unsigned long long *by_order);
/* M35A：÷2 分辨率账的归因口径（零内容回退路 ②/③ 两路的 join 数 + 其中靠 ÷2 档的笔数）。 */
void nvn_vk_m35a_stats(unsigned long long *store_hit, unsigned long long *store_div2,
                       unsigned long long *extfmt_hit, unsigned long long *extfmt_div2);
/* M6b B：某 RT 槽本 submit 内被 draw 写入的笔数（0 = 本 submit 没人写）。 */
unsigned nvn_vk_rt_slot_writers(int slot);
/* ---- M36A（2026-11-09）：rt-bind 命中处的逐槽取证（只读；只在 smo_m36a=1 时被调用）----
   writers_all = 整 boot 被 draw 写进该槽的笔数（0 ⇒ 从来没被画过）；
   last_wr_f / last_bind_f = 最后写入帧 / 最后被绑成渲染目标的帧（帧锚点 dm_frame_anchor_get）；
   lay = 槽当前记账的 VkImageLayout，s_layout = 采样声明需要的布局（恒 SHADER_READ_ONLY_OPTIMAL=5）。 */
void nvn_vk_rt_slot_m36a(int slot, unsigned long long *writers_all, unsigned *writers_sub,
                         unsigned long long *last_use, unsigned long long *last_wr_f,
                         unsigned long long *last_bind_f, int *lay, int *s_layout,
                         unsigned *w, unsigned *h, unsigned long long *key,
                         int *has_img, int *is_cur, int *shadow_lay);
/* M36A：同一块池存储上的**任意**尺寸/格式的槽（不看 h_rs_match / fmt 守卫）；1 = 命中。 */
int nvn_vk_rt_store_sibling(unsigned long long store, int *slot, int *txid);
/* M36A：命中槽的整幅内容回读（mean / nz% / colored% / distinct 上界 / 源像素 lag1×100 / px0）。 */
int nvn_vk_rt_slot_content_m36a(int slot, unsigned long long *mr, unsigned long long *mg,
                                unsigned long long *mb, unsigned *nz_pm, unsigned *colored_pm,
                                unsigned *distinct, int *lag1_x100, unsigned *px0);
/* store 版可采样 view（id 未知时用）；其余语义同 nvn_vk_rt_sample_prepare。 */
void *nvn_vk_rt_sample_prepare_store(unsigned long long store, int *why);
unsigned long long nvn_vk_rt2_rejected(void);   /* 尺寸/格式守卫拒掉的 store 命中数 */
void nvn_vk_rt2_reject_split(unsigned long long *by_size, unsigned long long *by_fmt);
void nvn_vk_rt_repl_stats(unsigned long long *copy_ok, unsigned long long *copy_fail,
                          unsigned long long *throttle, unsigned long long *direct,
                          unsigned long long *direct_fail);
void nvn_vk_rt_shadow_probe_cur(const char *tag);   /* M4e 取证：当前 RT 的影子图内容直方图 */
int nvn_vk_view_is_rt(void *view);                  /* M4e：该 view 是否我们的 RT 图/影子图 */
unsigned long long nvn_vk_rt_ds_writes(void);       /* M4e：描述符写入落到 RT 图的槽次数 */
unsigned long long nvn_vk_texture_slots_bound(void);
/* M5z P1：id 图 LRU 驱逐用的两个只读查询（在用判据 / 图字节数）。 */
int nvn_vk_tex_id_in_use(int id);
unsigned long long nvn_vk_tex_vkbytes(const nvn_vk_tex *t);
int nvn_vk_texture_slot_has(int slot);   /* 该槽当前是否挂着真实纹理（切回占位需要 flush） */
int nvn_vk_texture_slot_matches(int slot, nvn_vk_tex *t);   /* 该槽是否已是 t（重复绑定不 flush） */
int nvn_vk_tex_matches_desc(const nvn_vk_tex *t, const nvn_texdesc *d);   /* 侧表图与快照是否同源 */
void nvn_vk_texture_forget(nvn_vk_tex *t);   /* 销毁前清掉引用该 view 的槽（防悬垂句柄） */
void nvn_vk_ubo_stats(unsigned long long *binds, unsigned long long *up, unsigned long long *trunc);
unsigned long long nvn_vk_ubo_stage_count(int s);
int  nvn_vk_draw_elements(int mode, int type, int count, uint64_t idx, int base_vertex, int base_instance,
                          int instances, int dclass);
/* ---- M14B（2026-11-08）：NVN blend / color state 的回放入口 ----
   由 gfx_bridge.c 的 cmd_trace sink 在 CMD_BIND_BLEND / CMD_BIND_COLOR 上调用，
   bytes 是**录制时刻**从客体状态对象拷下的字节（blend 8B = 一个打包位域；color 4B = color_st）。
   smo_m14b=0（缺省）时两者都在第一行返回：不存状态、不打印、不动管线（B0 逐字节等于现状）。
   smo_m14b=1 时：blend 解出 (target,srcRgb,dstRgb,srcA,dstA,eqRgb,eqA)，color 解出 blend_en，
   draw 时按这对值下发动态混合状态（VK_EXT_extended_dynamic_state3）。恒返回 0（本层不否决）。 */
int nvn_vk_bind_blend_state(uint64_t guest_ptr, const void *bytes, unsigned nbytes);
int nvn_vk_bind_color_state(uint64_t guest_ptr, const void *bytes, unsigned nbytes);
/* M14B：把开关文件落盘（app 自己建，0666；hdc shell 建不了）。录制侧第一次 bind blend 时调一次。 */
void nvn_vk_m14b_ensure(void);
/* M14B：开关当前值（1 = 解码 + 混合修复；0 = 旧行为）。gfx_bridge 的 setter 真实现按它决定写不写。 */
int nvn_vk_m14b_on(void);
/* M14B 只读台账（给报告/下一轮用；smo_m14b=0 时全 0）。 */
void nvn_vk_m14b_stats(unsigned long long *binds, unsigned long long *ok, unsigned long long *bad,
                       unsigned long long *alpha_combo, unsigned long long *en_on,
                       unsigned long long *draws_blended, unsigned long long *draws_total,
                       unsigned long long *last_raw);
/* M4ap P1：实例化输入率台账（divisor 是 NVN stream state 的第二个 u32）。
   calls = SetVertexStreamState 调用数；nz = 其中含 divisor>0 的调用数；dmax = 见过的最大 divisor；
   support/limit = 设备能否表达 INSTANCE 率 + 驱动的 maxVertexAttribDivisor。 */
void nvn_vk_inst_divisor_census(unsigned long long *calls, unsigned long long *nz,
                                unsigned long long *dmax, unsigned *support, unsigned *limit);
/* M4ap P1：实例化 draw 台账（submitted/clamped/fell_back/attr_inst_bound/inst_max + 各原因计数）。 */
void nvn_vk_inst_draw_stats(unsigned long long *draws, unsigned long long *submitted_multi,
                            unsigned long long *clamped, unsigned long long *attr_bound,
                            unsigned long long *fb_nodiv, unsigned long long *fb_cap,
                            unsigned long long *fb_attr, unsigned *inst_max);
/* M4ap P1：[r] inst totals 一行（与 [r] draw totals 同节流点）。 */
void nvn_vk_inst_totals_report(unsigned sub);
/* M4at P1：顶点属性格式台账（Top8，fmt=ok/ph）。与 [r] draw totals 同节流点。 */
void nvn_vk_attr_fmt_report(unsigned sub);
/* M4ad P2：无索引 draw（NVN DrawArrays / DrawArraysInstanced）。走流端 cmd_trace.c 把
   CMD_DRAW_ARRAYS 交到这里；内部合成 u32 索引 0..count-1(+first)，复用 draw_elements 的整条
   「管线/状态/绑定/arena/取证」路径，不新造第二套状态机。返回 0 = 真提交（同 draw_elements 口径）。 */
int  nvn_vk_draw_arrays(int mode, int first, int count, int instances);
/* M4ad P1：draw 普查（verts/idx 直方图 + Top vs + 目标分布 + DrawArrays 台账）。
   sub = submit 序号（只用于日志标签）。 */
void nvn_vk_da_census_report(unsigned sub);
/* M4ad P1：当前 submit 序号（一帧清单的标签；gfx_bridge 每个 submit 开头设一次）。 */
void nvn_vk_cen_set_submit(unsigned sub);
// M4s P3：DrawArrays 类 opcode 到达时记账（gfx_bridge 里它们只有 cb_account 桩、从不重放）；
// 只有「当前是纯深度 pass」时才会累加/打日志。
void nvn_vk_note_draw_arrays(void);
int  nvn_vk_draw_flush(void);
void nvn_vk_real_draw_stats(unsigned long long *draws, unsigned long long *fail, unsigned long long *skip,
                            unsigned long long *pipes_ok, unsigned long long *pipes_fail,
                            unsigned long long *skip_noprog, unsigned long long *skip_nopipe,
                            unsigned long long *skip_attr, unsigned long long *skip_topo,
                            unsigned long long *skip_nort, unsigned long long *skip_basev,
                            unsigned long long *skip_nverts, unsigned long long *skip_alloc,
                            unsigned long long *pipe_skip_int64, unsigned long long *attr_ph);
/* M4ao P1：skip 归属审计（每个拒收 return 路径一个理由码；unattributed 必须为 0）。
   reasons/reasons_call 需由调用方提供 nvn_vk_skip_reason_count() 个元素；
   rejects[0..2]=cap/占位/ensure 失败（这三类不累加 skip，但同样让 sink fail 涨）；
   incall[0/1] = 来自 CALL inline 体的 draw 数 / 其中画成的笔数。 */
int  nvn_vk_skip_reason_count(void);
const char *nvn_vk_skip_reason_name(int i);
unsigned long long nvn_vk_skip_idx_total(void);      /* skip_idx 总数（M4an 报表漏印的那一项） */
unsigned long long nvn_vk_skip_pair_total(void);     /* (vs,fs) 对门控命中数 */
void nvn_vk_skip_detail(unsigned long long *total, unsigned long long *sum, long long *unattr,
                        unsigned long long *reasons, unsigned long long *reasons_call,
                        unsigned long long *rejects, unsigned long long *incall);

/* M25A：合并窗口。gfx_bridge 的 nvn_h_replay 在「逐命令流循环」前后各调一次；
   窗口内 nvn_vk_draw_flush 可以只收口不提交（smo_m25a_c1=1 时），窗口末统一真提交一次。
   两个开关都是 C 默认 0 的沙箱文件开关，关态行为与 M23B 之后逐字一致。 */
void nvn_vk_m25a_merge_begin(void);
void nvn_vk_m25a_merge_end(void);

#ifdef __cplusplus
}
#endif
