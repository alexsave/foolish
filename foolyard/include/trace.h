#ifndef FOOLYARD_TRACE_H
#define FOOLYARD_TRACE_H

#include "types.h"
#include "card.h"

struct World;

// One line per event, stamped with the sim clock. Call it through the
// trace_line macro in world.h, never directly: the macro is what keeps the
// ARGUMENTS from being evaluated when tracing is off. Guarding inside this
// function does not, and trace_cards was running snprintf per card on every
// bot action of every run.
void trace_emit(struct World *w, const char *fmt, ...);

// "7h", "Ks". The returned pointer is one of a few rotating static buffers, so
// several calls in one printf are fine and a hundred are not.
const char *trace_card(Card c);
const char *trace_cards(const Card *c, int n);

const char *trace_suit(int suit);
const char *trace_move_kind(int kind);
const char *trace_event_name(u32 type);

#endif
