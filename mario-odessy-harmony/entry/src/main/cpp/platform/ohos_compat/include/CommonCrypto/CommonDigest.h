// CommonCrypto/CommonDigest.h shim —— SHA-256 有真实实现（ohos_stub.c），loader 哈希校验用
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint32_t CC_LONG;
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    uint32_t datalen;
} CC_SHA256_CTX;

int CC_SHA256_Init(CC_SHA256_CTX *c);
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, size_t len);
int CC_SHA256_Final(uint8_t *md, CC_SHA256_CTX *c);

// 一次性接口：platform/loader.c 的模块完整性校验用它（ohos_stub.c 真实现）
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md);
