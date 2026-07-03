// Freestanding shim for the wasm32 build (no libc). The kernel only uses
// memcpy/memset, and clang lowers struct copies to these too — implemented
// in wasm_api.c.
#ifndef CNITRO_WASM_STRING_H
#define CNITRO_WASM_STRING_H

typedef __SIZE_TYPE__ size_t;

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

#endif
