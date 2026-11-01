#ifndef NIOH4P_FREESTANDING_H
#define NIOH4P_FREESTANDING_H
#include <stddef.h>
void *memset(void *destination, int value, size_t size);
void *memcpy(void *destination, const void *source, size_t size);
size_t strlen(const char *text);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t size);
int memcmp(const void *left, const void *right, size_t size);
#endif
