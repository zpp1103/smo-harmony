// CoreFoundation shim —— 宿主仅少量 CFString/CFBundle 读取（游戏路径、语言）
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef const void *CFStringRef;
typedef unsigned long CFIndex;
typedef unsigned int CFStringEncoding;
#define CFStringEncodingUTF8 0x08000100

CFStringRef CFStringCreateWithCString(void *alloc, const char *cStr, CFStringEncoding enc);
int CFStringGetCString(CFStringRef s, char *buffer, CFIndex bufferSize, CFStringEncoding enc);
CFIndex CFStringGetLength(CFStringRef s);
void CFRelease(void *cf);

// Locale（宿主 UI 语言探测）：OHOS 上返回 NULL → 英文，周末接 @ohos.i18n
typedef const void *CFLocaleRef;
#define kCFLocaleLanguageCode ((CFStringRef)1)
#define kCFLocaleCountryCode ((CFStringRef)2)
#define kCFStringEncodingUTF8 CFStringEncodingUTF8
CFLocaleRef CFLocaleCopyCurrent(void);
CFStringRef CFLocaleGetValue(CFLocaleRef locale, CFStringRef key);
