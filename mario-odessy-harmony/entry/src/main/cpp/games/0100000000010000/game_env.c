/* game_env.c —— SMO（Super Mario Odyssey 1.3.0/1.4.1）专属环境与 NVN 应答表。
 * 平台侧只留两个弱钩子，本文件提供强符号：
 *   1) game_configure_env(dir)（platform/game_napi.cpp 声明/兜底/调用）：游戏专属 env 配置
 *      （TOTK_DUMP_AT 的 0x131f318 族 donor 对拍地址等 SMO 专属项）。TOTK 落地时新建
 *      games/<新tid>/game_env.c 提供同签名实现，platform 零改动。
 *   2) game_nvn_devinfo(&count)（platform/gfx_bridge.c 弱 getter）：per-game NVN GetInteger
 *      应答表（TotK 是 55/15 ABI，gfx_probe.c:103 注释）。
 * 本文件随 games/<tid>/*.c glob 进入 arm64 构建；x86_64 壳不编游戏侧，走平台弱默认。 */
#include <stdlib.h>

/* donor hle_nvn.c g_devinfo 的 Odyssey 全表（SMO 1.3.0 实测值，Mac 上验证过）。
   历史：曾只答 0/1 两项（GLSLC 四元组 0x2E-0x31 被撤，当时语境下撤除是对的）；终局二段定谳
   全 0 把异步装载池短路成零容量（TexturePoolRegister×412 后绝对静默、enqueue 静默丢 work），
   且 H1 页隔离+F2 提交族已修、验尸链齐备，恢复全表。每个 what 首查打点，崩了即回滚单点。 */
static const int nvn_devinfo_odyssey[]={
 [0x00]=53,[0x01]=311,[0x02]=14,[0x03]=65536,[0x04]=256,[0x05]=8,[0x06]=16,[0x07]=4,[0x08]=16,[0x09]=32,
 [0x0A]=16,[0x0B]=4,[0x0C]=4,[0x0D]=4,[0x0E]=16,[0x0F]=32,[0x10]=32,[0x11]=256,[0x12]=256,
 [0x13]=4,[0x14]=8,[0x15]=512,[0x16]=256,[0x17]=2,[0x18]=256,[0x19]=4096,[0x1A]=131072,
 [0x1B]=16,[0x1C]=1536,[0x1D]=1024,[0x1E]=64,[0x1F]=1536,[0x20]=65535,[0x21]=65535,[0x22]=65535,
 [0x23]=8,[0x24]=1048576,[0x25]=4096,[0x26]=16,[0x27]=4096,[0x28]=1,[0x29]=1,[0x2A]=1,
 [0x2B]=262144,[0x2C]=15,[0x2D]=2048,[0x2E]=1,[0x2F]=1,[0x30]=14,[0x31]=5,[0x32]=1,[0x33]=8,
 [0x34]=8,[0x35]=4,[0x36]=32,[0x37]=128,[0x38]=32,[0x39]=128,[0x3A]=65536,[0x3B]=1,[0x3C]=4,
 [0x3D]=16384,[0x3E]=134217728,[0x3F]=2048,[0x40]=16384,[0x41]=16384,[0x42]=1,[0x43]=1,[0x44]=1,
 [0x45]=1,[0x46]=5,[0x47]=1,[0x48]=1,[0x49]=1,[0x4A]=1,[0x4B]=1,[0x4C]=1,[0x4D]=16,
 [0x4E]=1024,[0x4F]=32,[0x50]=4096,[0x51]=65536,[0x52]=65536,[0x53]=256,[0x54]=16384,
 [0x55]=262144,[0x56]=4096,[0x57]=1,[0x58]=4,[0x59]=2,[0x5A]=0,[0x5B]=4096,[0x5C]=16384,
 [0x5D]=4096,[0x5E]=128,[0x5F]=4096};

const int *game_nvn_devinfo(unsigned *out_count)
{
    if (out_count) *out_count = (unsigned)(sizeof nvn_devinfo_odyssey / sizeof *nvn_devinfo_odyssey);
    return nvn_devinfo_odyssey;
}

void game_configure_env(const char *dir)
{
    (void)dir;
    // r22 donor 黄金清单对拍静态窗（logs/donor_golden.txt）：C 类 vtable 模板(0x131f318 族，
    // +0x10 槽 0x131f330=base+0x8b5c70→bl 0x8b5ca0 注册体)+运行期 .bss vtable(0x12dc760-7d8，
    // donor C 的 vptr=base+0x12dc7a0 落此窗)+outer 的 vtable(0x12dd510，donor [outer+0])
    if (getenv("SMO_DUMP"))
        setenv("TOTK_DUMP_AT", "0x131f318,0x131f338,0x12dc760,0x12dc780,0x12dc7a0,0x12dc7c0,0x12dd510", 0);
}
