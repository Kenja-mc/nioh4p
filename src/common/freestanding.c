#include "freestanding.h"

void *memset(void *destination, int value, size_t size)
{
    unsigned char *p = destination;
    while (size--) *p++ = (unsigned char)value;
    return destination;
}

void *memcpy(void *destination, const void *source, size_t size)
{
    unsigned char *d = destination;
    const unsigned char *s = source;
    while (size--) *d++ = *s++;
    return destination;
}

size_t strlen(const char *text)
{
    const char *end = text;
    while (*end) end++;
    return (size_t)(end - text);
}

int strcmp(const char *left, const char *right)
{
    while (*left && *left == *right) { left++; right++; }
    return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t size)
{
    while (size-- && *left && *left == *right) { left++; right++; }
    return size == (size_t)-1 ? 0 : (unsigned char)*left - (unsigned char)*right;
}

int memcmp(const void *left, const void *right, size_t size)
{
    const unsigned char *a = left, *b = right;
    while (size--) {
        if (*a != *b) return *a - *b;
        a++; b++;
    }
    return 0;
}
