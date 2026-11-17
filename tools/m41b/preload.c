// LD_PRELOAD 探针：构造期跑 Vulkan 探测，输出到文件与 stderr
#include <stdio.h>
#include <stdlib.h>
int probe_main(int argc, char **argv);
static void __attribute__((constructor)) m41b_init(void) {
    static char *argv[] = { "m41b_probe", "/data/local/tmp/m41b_out_pre.txt", NULL };
    fprintf(stderr, "[m41b] preload constructor fired\n");
    int rc = probe_main(2, argv);
    fprintf(stderr, "[m41b] probe_main rc=%d\n", rc);
}
