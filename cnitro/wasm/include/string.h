// Freestanding shim for the wasm32 build (no libc). The kernel only uses
// memcpy/memset, and clang lowers struct copies to these too — implemented
// in wasm_api.c.
#ifndef CNITRO_WASM_STRING_H
#define CNITRO_WASM_STRING_H

#ifndef CNITRO_WASM_SIZE_T
#define CNITRO_WASM_SIZE_T
typedef __SIZE_TYPE__ size_t;
#endif

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
// Needed by strategy.h's parse_strategy (inline) and cordite's getenv users;
// implemented in wasm_bots_api.c (rules.wasm never references it).
int strcmp(const char *a, const char *b);

#endif
