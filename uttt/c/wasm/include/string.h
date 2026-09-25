/* Freestanding string.h for the wasm32 web build: what the kernel calls, and
 * what clang lowers struct copies to. Implemented in uttt_web.c. */
#ifndef UTTT_WASM_STRING_H
#define UTTT_WASM_STRING_H

typedef __SIZE_TYPE__ size_t;

void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strncmp(const char *a, const char *b, size_t n);

#endif
