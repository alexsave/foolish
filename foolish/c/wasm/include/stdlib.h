// Freestanding shim for the wasm32 build. The rules kernel needs nothing
// from here; the bot module
// (bots.wasm) provides malloc/calloc/free/getenv in wasm_bots_api.c for the
// Monte-Carlo bots' one-time scratch buffers and CD_* tuning knobs.
#ifndef CNITRO_WASM_STDLIB_H
#define CNITRO_WASM_STDLIB_H

#ifndef CNITRO_WASM_SIZE_T
#define CNITRO_WASM_SIZE_T
typedef __SIZE_TYPE__ size_t;
#endif

_Noreturn void abort(void);
void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void free(void *p);
char *getenv(const char *name);
int atoi(const char *s);
int atexit(void (*fn)(void));

#endif
