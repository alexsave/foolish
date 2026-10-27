/* speed.c - see speed.h. */
#include "speed.h"

int cc_speed(int base_s, int inc_s) {
    int est = base_s + 40 * inc_s;
    return est < 180 ? CC_BULLET : est < 480 ? CC_BLITZ : est < 1500 ? CC_RAPID : CC_CLASSICAL;
}
