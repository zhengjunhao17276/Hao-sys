/**
 * =========================================================================
 * string.c - 标准 C 库字符串与内存操作函数（内核自用版）
 *
 * HaoOS 是一个裸机内核，不能链接 libc，所以自己实现了最常用的几个
 * 字符串和内存操作函数。这些实现都非常朴素——逐字节操作，没有做任何
 * 性能优化（如 SSE/MMX 或字长对齐拷贝），但对于内核初始化阶段已经够用。
 *
 * 为什么需要自己实现？
 *   - 内核运行在 ring 0，没有操作系统为我们提供 libc
 *   - 即使有交叉编译器的 libc，做了系统调用依赖，在内核中也不能用
 *   - 这些函数在 PMM/VMM/FAT 等模块中被广泛使用
 * =========================================================================
 */

#include "../include/lib/string.h"

/**
 * memset - 将内存块的前 n 个字节设置为指定的值
 *
 * 典型的 memset 实现，注意 s 和 c 都是 int（接口兼容标准 C），
 * 但实际赋值时转换为 unsigned char。常用于清零页表或初始化结构体。
 */
void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

/**
 * memcpy - 从 src 拷贝 n 个字节到 dest
 *
 * 注意：这个实现没有处理 dest 和 src 重叠的情况（应该用 memmove）。
 * 内核中目前的使用场景（结构体拷贝、缓冲区读取等）都不会重叠，所以够用。
 * 如果将来需要重叠拷贝，需要换成 memmove 或加判断。
 */
void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dest;
}

/**
 * memcmp - 比较两个内存块的前 n 个字节
 *
 * 返回值遵循标准 C 语义：
 *   - 0    表示相等
 *   - 正数 表示 s1 > s2（以第一个不等的字节为准）
 *   - 负数 表示 s1 < s2
 * FAT 文件系统中用此函数比较文件名和文件系统类型字符串。
 */
int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* a = s1;
    const unsigned char* b = s2;
    while (n--) {
        if (*a != *b) return *a - *b;
        a++; b++;
    }
    return 0;
}

/**
 * strlen - 计算字符串长度（不含结尾的 '\0'）
 */
size_t strlen(const char* s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

/**
 * strcmp - 比较两个字符串
 * strncmp - 比较两个字符串的前 n 个字符
 * strcpy - 复制字符串
 * strncpy - 复制字符串，最多 n 个字符，不足补 '\0'
 *
 * FAT 文件系统的文件名查找依赖 strncmp 进行不补全的比较。
 */
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
