// Freestanding shim for the wasm32 build. game.c includes <stdlib.h> but only
// calls abort under -DGRPO_RNG_DEBUG, which the wasm build never defines.
#ifndef CNITRO_WASM_STDLIB_H
#define CNITRO_WASM_STDLIB_H

_Noreturn void abort(void);

#endif
