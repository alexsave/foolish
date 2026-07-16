// Freestanding shim for the wasm32 build. game.c includes <stdio.h> but only
// calls fprintf under -DGRPO_RNG_DEBUG, which the wasm build never defines.
#ifndef CNITRO_WASM_STDIO_H
#define CNITRO_WASM_STDIO_H

typedef struct FILE FILE;
extern FILE *stderr;
int fprintf(FILE *stream, const char *fmt, ...);

#endif
