// M3q：离线把 m3p 落盘的真实 NVN shader 用 nvn_replay/maxwell/translate.c 翻成 Vulkan GLSL。
//
// 输入 .code 已经去掉 NVN 的 0x30 字节头（文件首字 = SPH magic 0x12345678），
// 而 maxwell_translate_bounded() 期望 blob = NVN 头(0x30) + SPH(0x50) + 指令，
// 所以这里补 0x30 字节零头，max_size = 0x30 + code_len。
//
// 输出：每行一条 TSV（字段见下），GLSL 写到 <out>/<key>.<stage>.glsl。
// 不做任何语义修改；失败原因原样带出 info->error（后续按 translate.c 的 fail() 站点归类）。
//
// usage: m3q_translate <corpus_dir> <out_dir> [key]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "platform/nvn_replay/maxwell/shader.h"

static uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n + 1);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return p;
}

// 错误串里的 %u/%d/%016llx 等替换成 *，便于和源码里的格式串对齐（失败归类用）。
static void skeleton(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        if (in[i] == '%' && in[i + 1]) {
            const char *p = in + i + 1;
            while (*p && !strchr("diouxXscfeEgG", *p)) p++;
            if (*p) p++;
            out[o++] = '*';
            i = (size_t)(p - in) - 1;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <corpus_dir> <out_dir> [key]\n", argv[0]); return 2; }
    const char *dir = argv[1], *out = argv[2], *only = argc > 3 ? argv[3] : NULL;
    char path[1024], indexPath[1100];
    snprintf(indexPath, sizeof indexPath, "%s/index.txt", dir);

    FILE *ix = fopen(indexPath, "r");
    if (!ix) { perror(indexPath); return 2; }

    maxwell_gmem_base = 0x80000000ull;
    maxwell_gmem_size = 0x40000000ull;   // 1 GiB：不触发 translate.c:1861 的 4GiB 断言
    if (getenv("M3Q_NO_INT64")) maxwell_no_int64 = true;   // 复测无 shaderInt64 设备的退路

    char key[64]; unsigned stage; unsigned ctl_claim, code_claim;
    char line[512];
    unsigned n_done = 0;
    while (fgets(line, sizeof line, ix)) {
        if (sscanf(line, "%63s %u ctl=%u code=%u", key, &stage, &ctl_claim, &code_claim) != 4) continue;
        if (only && strcmp(only, key)) continue;

        size_t ctl_len = 0, code_len = 0;
        snprintf(path, sizeof path, "%s/%s.%u.ctl", dir, key, stage);
        uint8_t *ctl = slurp(path, &ctl_len);
        snprintf(path, sizeof path, "%s/%s.%u.code", dir, key, stage);
        uint8_t *code = slurp(path, &code_len);
        if (!ctl || !code) { fprintf(stderr, "B\t%s\t%u\tread failed\n", key, stage); free(ctl); free(code); continue; }

        // 现场复核 dump 口径（与 shader_dump.c:97-105 一致），不符直接拒收而不是猜
        if (rd32(ctl) != 0x98761234u || stage > 5 || code_len < 0x80) {
            fprintf(stderr, "B\t%s\t%u\tlayout mismatch magic=%08x stage=%u code=%zu\n",
                    key, stage, rd32(ctl), stage, code_len);
            free(ctl); free(code);
            continue;
        }

        // .code 就是 NVN 二进制（0x30 NVN 头 + 0x50 SPH + 指令），首字 0x12345678；无需补头。
        // translate.c:3053-3067 取 blob+0x30=SPH、blob+0x80=指令，正好对上。
        size_t blob_len = code_len;
        uint8_t *blob = code;

        ShaderKey k;
        memset(&k, 0, sizeof k);
        k.glsl = 1;            // Vulkan GLSL 输出
        k.attr_kind[0] = 0;    // 全部按 float 顶点输入（只影响声明类型，不影响能否编译）
        // M3w：NVN 的 clip space 与 GL 同口径（y 向上、z ∈ [-1,1]），Vulkan 是 y 向下、z ∈ [0,1]。
        // 本机 Android/HarmonyOS 后端用的是**正高度** viewport（nvn_vk.c 的
        // VkViewport{0,0,H_OFF_W,H_OFF_H}），不像桌面后端那样靠负高度 viewport 抵消，
        // 所以两项都要在烘焙期补：z=(z+w)*0.5 把 [-1,1] 压进 [0,1]，y 取反。
        // 环境变量留退路用于对照烘焙（M3Q_NO_ZREMAP / M3Q_NO_FLIP）。
        k.depth_minus_one = getenv("M3Q_NO_ZREMAP") == NULL;
        k.flip_y = getenv("M3Q_NO_FLIP") == NULL;
        /* M5u P1：`M3Q_STRUCTURED=1` 打开 translate.c 的结构化控制流（if/else/for），
           关掉默认的 `switch (pc)` 状态机 + 100000 次 guard 循环。
           背景（M5u boot#1 实测）：状态机 GLSL 每个片元要"解释执行"整段 Maxwell 指令，
           1600x900 的一次全屏 FS 在 SwiftShader 上直接把 boot 卡死（guest 线程停在 op=29，
           10 分钟无进展；同样的二进制 + 旧语料 A/B 正常）。结构化后同 key 的 GLSL 里
           `switch (pc)` 与 guard 循环都消失，只剩真循环。
           两个全局都由「Android 后端」设置（translate.c 注释），离线烘焙同样适用。 */
        /* M5v P1：结构化控制流改为**默认开**（M5u 只在显式 `M3Q_STRUCTURED=1` 下烘过，
           设备上那份合并语料必须与开关一致，否则下一次重烘又会引入 park）。
           `M3Q_NO_STRUCTURED=1` 保留为回退到 `switch (pc)` 状态机的 A/B 开关。 */
        if (getenv("M3Q_NO_STRUCTURED") == NULL) {
            extern bool maxwell_static_stacks, maxwell_structured;
            maxwell_static_stacks = true;
            maxwell_structured = true;
        }

        ShaderInfo *si = maxwell_translate_bounded(blob, blob_len, 0, (int)stage, &k);
        char gpath[1200];
        const char *stage_name = stage == 0 ? "vert" : stage == 1 ? "frag" : "comp";
        snprintf(gpath, sizeof gpath, "%s/%s.%u.glsl", out, key, stage);

        char skel[256] = "";
        if (si && si->msl) {
            FILE *g = fopen(gpath, "wb");
            if (g) { fwrite(si->msl, 1, strlen(si->msl), g); fclose(g); }
            // 反射：非零 cbuf 尺寸、贴图槽（运行时要靠它建描述符布局）
            static char cbsz[512], texbuf[4096];
            size_t co = 0, to = 0;
            cbsz[0] = texbuf[0] = 0;
            for (int i = 0; i < 18; i++)
                if (si->cbuf_size[i] && co < sizeof cbsz - 32)
                    co += (size_t)snprintf(cbsz + co, sizeof cbsz - co, "%s%d:%u", co ? "," : "", i, si->cbuf_size[i]);
            for (int i = 0; i < si->ntex && i < 64 && to < sizeof texbuf - 96; i++) {
                ShaderTexture *t = &si->tex[i];
                to += (size_t)snprintf(texbuf + to, sizeof texbuf - to,
                                       "%s%d:%u/%d%d%d%d/%u/%u/%u", to ? "," : "", i, t->type, (int)t->shadow,
                                       (int)t->is_array, (int)t->is_image, (int)t->bindless, t->handle_offset,
                                       t->bindless_cbuf, t->bindless_offset);
            }
            fprintf(stdout,
                    "T\t%s\t%u\t1\tattr=%u out=%u cbuf=%u cbdyn=%u cbwin=%u ntex=%d lmem=%u ninst=%u"
                    " wdepth=%d disc=%d wps=%d gmem=%d gmemst=%d tint=%d rscale=%d glsl=%zu"
                    " cbsz=%s tex=%s stage=%s zremap=%d flipy=%d path=%s\n",
                    key, stage, si->attr_mask, si->out_mask, si->cbuf_mask, si->cbuf_dynamic, si->cbwin_mask,
                    si->ntex, si->local_mem, si->num_insts,
                    (int)si->writes_depth, (int)si->uses_discard, (int)si->writes_point_size,
                    (int)si->uses_gmem, (int)si->uses_gmem_store, (int)si->uses_tint, (int)si->uses_rscale,
                    strlen(si->msl), cbsz, texbuf, stage_name,
                    (int)k.depth_minus_one, (int)k.flip_y, gpath);
        } else {
            const char *err = (si && si->error[0]) ? si->error : "no output";
            skeleton(err, skel, sizeof skel);
            fprintf(stdout, "T\t%s\t%u\t0\terr=%s stage=%s path=-\n", key, stage, err, stage_name);
            fprintf(stderr, "F\t%s\t%u\t%s\t%s\n", key, stage, skel, err);   // 归类骨架
        }
        maxwell_free(si);   // 也负责 free(si->msl)

        free(ctl);
        free(code);
        n_done++;
    }
    fclose(ix);
    fprintf(stderr, "DONE %u entries\n", n_done);
    return 0;
}
