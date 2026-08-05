/**
 * =========================================================================
 * string.h - 内核自用字符串/内存操作函数声明
 *
 * 裸机内核不能依赖 libc 的声明，所以自行声明常用的字符串和内存函数。
 * 这些函数的实现见 lib/string.c。
 * =========================================================================
 */

#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <stdint.h>

/* 内存操作 */
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
int   memcmp(const void* s1, const void* s2, size_t n);

/* 字符串操作 */
size_t strlen(const char* s);
int    strcmp(const char* s1, const char* s2);
int    strncmp(const char* s1, const char* s2, size_t n);
char*  strcpy(char* dest, const char* src);
char*  strncpy(char* dest, const char* src, size_t n);

#endif
