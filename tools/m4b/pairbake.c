// M4b：按 (vs_key, fs_key) 配对重烘。与 m3q_translate.c 的唯一差别在 FS：
// 先把配对的 VS 翻出来，取它的 out_mask 填进 ShaderKey.vs_out_mask，再翻 FS。
// translate.c:313-314 的规则不改（不在 mask 里的属性发 0u），这样 FS 声明的 varying
// 才与 VS 实际输出一致 —— 原烘焙器的 vs_out_mask 恒 0，全部 FS 的 attr_mask 被折成 0。
//
// 输入 pair 文件每行：<vs_key|-> <fs_key|-> [nreg] [ndraw] [mask=0x..]
// 输出与 m3q_translate.c 完全相同的 TSV（交给 m3q_batch.py 编译）+ <out>/<key>.<stage>.glsl。
//
// usage: pairbake <corpus_dir> <out_dir> <pairs_file>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>

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

/* 已翻过的键：key+stage -> out_mask（VS）/ 用过的 vs_out_mask（FS）。表小，线性找。 */
typedef struct { char key[32]; int stage; uint32_t mask; int ok; } Done;
/* M4h P2：全量 A 类补烘的 (key,stage) 去重表会到 ~11k 条，8192 会静默丢表项
   （done_add 失败 ⇒ vs_known=0 ⇒ 后续 FS 全被 skip，表现为「烘了 VS 没烘 FS」）。 */
static Done g_done[32768];
static int g_done_n;
static Done *done_find(const char *key, int stage) {
    for (int i = 0; i < g_done_n; i++)
        if (g_done[i].stage == stage && !strcmp(g_done[i].key, key)) return &g_done[i];
    return NULL;
}
static Done *done_add(const char *key, int stage, uint32_t mask, int ok) {
    if (g_done_n >= 32768) return NULL;
    Done *d = &g_done[g_done_n++];
    snprintf(d->key, sizeof d->key, "%s", key);
    d->stage = stage; d->mask = mask; d->ok = ok;
    return d;
}

/* 翻一条并打 TSV（字段与 m3q_translate.c 一致）。返回 out_mask，失败返回 ~0u。 */
static uint32_t translate_emit(const char *dir, const char *out, const char *key, unsigned stage,
                               uint32_t vs_out_mask)
{
    char path[1024], gpath[1200];
    size_t ctl_len = 0, code_len = 0;
    snprintf(path, sizeof path, "%s/%s.%u.ctl", dir, key, stage);
    uint8_t *ctl = slurp(path, &ctl_len);
    snprintf(path, sizeof path, "%s/%s.%u.code", dir, key, stage);
    uint8_t *code = slurp(path, &code_len);
    const char *stage_name = stage == 0 ? "vert" : stage == 1 ? "frag" : "comp";
    snprintf(gpath, sizeof gpath, "%s/%s.%u.glsl", out, key, stage);
    char skel[256] = "";

    if (!ctl || !code) {
        fprintf(stdout, "T\t%s\t%u\t0\terr=not in corpus stage=%s path=-\n", key, stage, stage_name);
        fprintf(stderr, "F\t%s\t%u\tnot in corpus\tnot in corpus\n", key, stage);
        free(ctl); free(code);
        return ~0u;
    }
    if (rd32(ctl) != 0x98761234u || stage > 5 || code_len < 0x80) {
        fprintf(stdout, "T\t%s\t%u\t0\terr=layout mismatch stage=%s path=-\n", key, stage, stage_name);
        fprintf(stderr, "F\t%s\t%u\tlayout mismatch\tlayout mismatch\n", key, stage);
        free(ctl); free(code);
        return ~0u;
    }

    ShaderKey k;
    memset(&k, 0, sizeof k);
    k.glsl = 1;
    k.attr_kind[0] = 0;
    k.depth_minus_one = getenv("M3Q_NO_ZREMAP") == NULL;
    k.flip_y = getenv("M3Q_NO_FLIP") == NULL;
    k.vs_out_mask = vs_out_mask;

    ShaderInfo *si = maxwell_translate_bounded(code, code_len, 0, (int)stage, &k);
    uint32_t out_mask = ~0u;
    if (si && si->msl) {
        FILE *g = fopen(gpath, "wb");
        if (g) { fwrite(si->msl, 1, strlen(si->msl), g); fclose(g); }
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
        out_mask = si->out_mask;
    } else {
        const char *err = (si && si->error[0]) ? si->error : "no output";
        skeleton(err, skel, sizeof skel);
        fprintf(stdout, "T\t%s\t%u\t0\terr=%s stage=%s path=-\n", key, stage, err, stage_name);
        fprintf(stderr, "F\t%s\t%u\t%s\t%s\n", key, stage, skel, err);
    }
    maxwell_free(si);
    free(ctl);
    free(code);
    return out_mask;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s <corpus_dir> <out_dir> <pairs_file>\n", argv[0]); return 2; }
    const char *dir = argv[1], *out = argv[2];
    FILE *pf = fopen(argv[3], "r");
    if (!pf) { perror(argv[3]); return 2; }

    maxwell_gmem_base = 0x80000000ull;
    maxwell_gmem_size = 0x40000000ull;
    if (getenv("M3Q_NO_INT64")) maxwell_no_int64 = true;

    unsigned n_pair = 0, n_vs = 0, n_fs = 0, n_fs_skip_nomask = 0, n_conflict = 0, n_fail = 0;
    char line[512];
    while (fgets(line, sizeof line, pf)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char vs[64] = "", fs[64] = "", tok[64];
        unsigned long nreg = 0, ndraw = 0, mask_env = 0; int have_mask = 0;
        int nf = sscanf(line, "%63s %63s %lu %lu %63s", vs, fs, &nreg, &ndraw, tok);
        if (nf < 2) continue;
        if (nf >= 5 && !strncmp(tok, "mask=0x", 7)) { mask_env = (unsigned)strtoul(tok + 7, NULL, 16); have_mask = 1; }
        if (!strcmp(vs, "-")) vs[0] = 0;
        if (!strcmp(fs, "-")) fs[0] = 0;
        if (!vs[0] && !fs[0]) continue;
        n_pair++;

        uint32_t vs_mask = 0; int vs_known = 0;
        if (vs[0]) {
            Done *d = done_find(vs, 0);
            if (d && d->ok) { vs_mask = d->mask; vs_known = 1; }
            else if (!d) {
                uint32_t m = translate_emit(dir, out, vs, 0, 0);
                d = done_add(vs, 0, m, m != ~0u);
                n_vs++;
                if (d && d->ok) { vs_mask = d->mask; vs_known = 1; } else n_fail++;
            }
        }
        if (!fs[0]) continue;
        if (!vs_known && have_mask) { vs_mask = mask_env; vs_known = 1; }
        if (!vs_known) {
            /* VS 未知/翻转失败：不能猜 mask（猜错等于又回到 attr_mask=0 或建管线失败），留缺口报出来 */
            n_fs_skip_nomask++;
            fprintf(stderr, "S\t%s\tvs=%s\tno vs_out_mask (skipped)\n", fs, vs[0] ? vs : "-");
            continue;
        }
        Done *d = done_find(fs, 1);
        if (d) {
            if (d->mask != vs_mask) {
                n_conflict++;
                fprintf(stderr, "C\t%s\tmask %#x != %#x (kept first)\n", fs, d->mask, vs_mask);
            }
            continue;
        }
        uint32_t m = translate_emit(dir, out, fs, 1, vs_mask);
        done_add(fs, 1, vs_mask, m != ~0u);
        n_fs++;
        if (m == ~0u) n_fail++;
    }
    fclose(pf);
    fprintf(stderr, "PAIRBAKE pairs=%u vs=%u fs=%u fs_skip_nomask=%u conflict=%u fail=%u\n",
            n_pair, n_vs, n_fs, n_fs_skip_nomask, n_conflict, n_fail);
    return 0;
}
