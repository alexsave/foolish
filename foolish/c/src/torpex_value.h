// Torpex value net — see torpex_value.c.
#ifndef CNITRO_TORPEX_VALUE_H
#define CNITRO_TORPEX_VALUE_H

#include "cordite_sim.h"

// 1 if weights are loaded (lazy; $TORPEX_WEIGHTS or ./torpex_weights.bin).
int tx_value_ready(void);
// Predicted normalized finish of seat p in full-info state s: 0=win, 1=durak.
float tx_value(const SimState *s, int p);

#endif
