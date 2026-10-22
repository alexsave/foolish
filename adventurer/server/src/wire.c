// wire.c - wire.h: the layout hash the answers carry, structgen's for this build (the Makefile
// compiles this file alone with -DADV_WIRE_HASH=<the hash it printed>).
#include "wire.h"

#ifndef ADV_WIRE_HASH
#error "ADV_WIRE_HASH is the Makefile's to pass: build through it (make -C adventurer/server)"
#endif

uint32_t adv_wire_hash(void) { return ADV_WIRE_HASH; }
