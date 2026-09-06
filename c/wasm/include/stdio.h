// Freestanding shim for the wasm32 build. The shipped modules never print;
// the declarations exist so the sources that DO print under a research flag
// (octogen under OG_EXPLAIN_BUILD, legal.c under LEGAL_STATS) still compile
// for wasm without the host libc.
#ifndef CNITRO_WASM_STDIO_H
#define CNITRO_WASM_STDIO_H

typedef struct FILE FILE;
extern FILE *stderr;
int fprintf(FILE *stream, const char *fmt, ...);

#endif
