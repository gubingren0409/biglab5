#include "mod.h"

// 从begin开始对连续n个字节赋值data
void memset(void *begin, uint8 data, uint32 n)
{
    uint8 *list = (uint8 *)begin;
    for (uint32 i = 0; i < n; i++)
        list[i] = data;
}

// 从src向dst拷贝n个字节的数据
void memmove(void *dst, const void *src, uint32 n)
{
    char *d = dst;
    const char *s = src;
    while (n--)
    {
        *d = *s;
        d++;
        s++;
    }
}

// 字符串p的前n个字符与q做比较
// 按照ASCII码大小逐个比较
// 相同返回0 大于或小于返回正数或负数
int strncmp(const char *p, const char *q, uint32 n)
{
    while (n > 0 && *p && *p == *q)
        n--, p++, q++;
    if (n == 0)
        return 0;
    return (uint8)*p - (uint8)*q;
}

// 返回字符串长度
int strlen(const char *str)
{
  int i = 0;
  for (i = 0; str[i] != '\0'; i++)
    ;
  return i;
}

/* --- append to src/kernel/lib/utils.c --- */

int strcmp(const char *p, const char *q)
{
    while (*p && *p == *q)
        p++, q++;
    return (unsigned char)*p - (unsigned char)*q;
}

char* strcpy(char *s, const char *t)
{
    char *os;

    os = s;
    while ((*s++ = *t++) != 0)
        ;
    return os;
}

char* strncpy(char *s, const char *t, int n)
{
    char *os;

    os = s;
    while (n-- > 0 && (*s++ = *t++) != 0)
        ;
    while (n-- > 0)
        *s++ = 0;
    return os;
}

/* --- append to src/kernel/lib/utils.c --- */

void* memcpy(void *dst, const void *src, uint32 n)
{
    // memcpy 通常不处理重叠，但为了安全和简单，我们这里可以直接复用 memmove 逻辑
    // 或者直接写一个简单的前向拷贝
    char *s = (char *)src;
    char *d = (char *)dst;
    
    // 简单的从前往后拷贝
    while (n-- > 0)
        *d++ = *s++;
        
    return dst;
}