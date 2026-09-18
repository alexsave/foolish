// Freestanding shim so wit-bindgen's generated kernel.c compiles with no libc.
// The generated glue calls realloc (inside cabi_realloc) and free (after
// import-state, and in export-state's post-return). component_impl.c backs
// them with a fixed static bump arena that is reset after every import - the
// kernel itself never allocates.
#ifndef CM_STDLIB_H
#define CM_STDLIB_H
#include <stddef.h>
void *realloc(void *ptr, size_t n);
void free(void *ptr);
_Noreturn void abort(void);
#endif
