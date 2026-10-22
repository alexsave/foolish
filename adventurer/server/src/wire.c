// wire.c - wire.h: the layout hash the answers carry.
#include "wire_table.h"

uint32_t adv_wire_hash(void) {
    static uint32_t h;
    if (!h) h = adv_wire_table_hash();
    return h;
}
