/*
 * string.c - 内核自用字符串/内存函数
 * 裸机内核没有 libc，这里手写最常用的几个；实现朴素（逐字节），
 * 对初始化阶段够用。
 */

#include "../include/lib/string.h"

void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

/* 拷贝 n 字节。⚠️ 不处理重叠（重叠该用 memmove），目前调用方都不会重叠 */
void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* a = s1;
    const unsigned char* b = s2;
    while (n--) {
        if (*a != *b) return *a - *b;
        a++; b++;
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

/* strncmp 的不补全比较是 FAT 文件名查找依赖的，别乱改语义 */
int strcmp(const char* s1, const char* s2) {
    while (*s1 && *s2 && *s1 == *s2) { s1++; s2++; }
    return *s1 - *s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n-- && *s1 && *s2 && *s1 == *s2) { s1++; s2++; }
    if (n == (size_t)-1) return 0;
    return *s1 - *s2;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (n-- && *src) *d++ = *src++;
    while (n--) *d++ = '\0';
    return dest;
}
