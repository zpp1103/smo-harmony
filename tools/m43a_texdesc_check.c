/* M43A / S2a：nvn_texdesc 布局双向校验器（S1 报告 §8-1 的交接风险①）。
 *
 * 用法（两个 TU 各编一次，输出必须逐字节相同）：
 *   cc -DM43A_TEXDESC_SIDE_ABI  -I nvn_vk2 [-o a] tools/m43a_texdesc_check.c && ./a > abi.txt
 *   cc -DM43A_TEXDESC_SIDE_TREE -I third_party/d2r_gfx/gfx/nvn [... 同上] && ./b > tree.txt
 *   diff abi.txt tree.txt   # 空 = 通过
 *
 * 两个头都定义 nvn_texdesc/nvn_texlayout，不能同 TU 共存，所以「双向」= 同一份源码编两次、
 * 由宏选择走哪份头，再由 diff 做终审。输出包含：sizeof、每个字段的 offsetof/sizeof、
 * 以及同一份「打包字」样本在两侧的字段解码结果。
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(M43A_TEXDESC_SIDE_ABI) && defined(M43A_TEXDESC_SIDE_TREE)
#error "pick exactly one side"
#endif
#if defined(M43A_TEXDESC_SIDE_ABI)
#include "abi/nvn_tex.h"
#define SIDE "abi(nvn_vk2, nx2android 搬入件)"
#elif defined(M43A_TEXDESC_SIDE_TREE)
#include "nvn_tex.h"
#define SIDE "tree(third_party/d2r_gfx, 直喂侧真身)"
#else
#error "define M43A_TEXDESC_SIDE_ABI or M43A_TEXDESC_SIDE_TREE"
#endif

#define OFF(T, f) printf("  %-28s off=%2zu size=%zu\n", #T "." #f, offsetof(T, f), sizeof(((T *)0)->f))

int main(void)
{
    printf("side=%s\n", SIDE);
    printf("sizeof(nvn_texdesc)=%zu align=%zu\n", sizeof(nvn_texdesc), _Alignof(nvn_texdesc));
    OFF(nvn_texdesc, target);
    OFF(nvn_texdesc, format);
    OFF(nvn_texdesc, flags);
    OFF(nvn_texdesc, samples);
    OFF(nvn_texdesc, width);
    OFF(nvn_texdesc, height);
    OFF(nvn_texdesc, depth);
    OFF(nvn_texdesc, levels);
    OFF(nvn_texdesc, stride);
    /* bitfields: offsetof is illegal in C, so derive the word position from the last plain field + total size */
    printf("  %-28s off=%2zu size=4 (packed word, derived)\n", "nvn_texdesc.<pkg/swizzle word>",
           offsetof(nvn_texdesc, stride) + sizeof(uint32_t));
    printf("sizeof(nvn_texlayout)=%zu align=%zu\n", sizeof(nvn_texlayout), _Alignof(nvn_texlayout));
    OFF(nvn_texlayout, size);
    OFF(nvn_texlayout, layer_size);
    OFF(nvn_texlayout, align);
    OFF(nvn_texlayout, layers);
    OFF(nvn_texlayout, bpb);
    OFF(nvn_texlayout, tileH);
    OFF(nvn_texlayout, tileD);

    /* 同一份「打包字」样本：byte0=4（块高 log2=3 → +1=4），byte1..3 = swizzle 2/3/4/5 的位域编码。
       两侧从**同一块内存**解出来的字段必须一致；这正是 nx2 原版（整 int 读）会读错的地方。 */
    unsigned char blob[sizeof(nvn_texdesc)];
    memset(blob, 0, sizeof blob);
    nvn_texdesc d;
    memset(&d, 0, sizeof d);
    d.target = 1; d.format = 0x25; d.flags = 0x40; d.samples = 1;
    d.width = 800; d.height = 450; d.depth = 1; d.levels = 1; d.stride = 0;
    d.pkg_block_h = 4;
    d.swizzle_r = 2; d.swizzle_g = 3; d.swizzle_b = 4; d.swizzle_a = 5; d.swizzle_pad = 0;
    memcpy(blob, &d, sizeof d);
    printf("word@36 = 0x%02x%02x%02x%02x\n", blob[36], blob[37], blob[38], blob[39]);
    nvn_texdesc r;
    memcpy(&r, blob, sizeof r);
    printf("decode pkg_block_h=%d swz=%u,%u,%u,%u pad=%u\n",
           (int)r.pkg_block_h, (unsigned)r.swizzle_r, (unsigned)r.swizzle_g,
           (unsigned)r.swizzle_b, (unsigned)r.swizzle_a, (unsigned)r.swizzle_pad);
    printf("reencode word == 0x%02x%02x%02x%02x : %s\n", blob[36], blob[37], blob[38], blob[39],
           memcmp(&r, &d, sizeof d) == 0 ? "identical" : "DIFFERS");
    return 0;
}
