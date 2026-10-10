// Freestanding shim for the wasm32 build. malloc, calloc, free, atoi and atol
// are the shared ones (shared/c/wasm/include/stdlib.h, reached by
// #include_next because this directory is first on the -isystem path). What
// is added here is this product's own: getenv over the CD_* knob table and
// the no-op atexit, both in wasm_bots_api.c, and abort.
#ifndef CNITRO_WASM_STDLIB_H
#define CNITRO_WASM_STDLIB_H

#include_next <stdlib.h>

_Noreturn void abort(void);
char *getenv(const char *name);
int atexit(void (*fn)(void));

#endif
