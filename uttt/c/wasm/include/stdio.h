/* Freestanding stdio.h for the wasm32 web build: snprintf alone, for the
 * kernel's sentences (uttt_say.c), with only the %s and %d they use.
 * Implemented in uttt_web.c. */
#ifndef UTTT_WASM_STDIO_H
#define UTTT_WASM_STDIO_H

typedef __SIZE_TYPE__ size_t;

int snprintf(char *out, size_t cap, const char *fmt, ...);

#endif
