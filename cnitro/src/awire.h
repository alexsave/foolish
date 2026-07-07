// Action wire ("awire" v1) — the packed move a client sends. The SAME bytes
// the browser validates through guards.wasm are POSTed verbatim and applied
// by rules.wasm on the server: one decoder, compiled into both modules, so
// the two sides can never disagree about what a payload means.
//
//   u8 kind    0 attack, 1 cover, 2 pass, 3 pickup, 4 good
//   u8 n       card count (0 for pickup/good)
//   n x u8     wire cards (cover: the covering cards)
//   n x u8     cover only: the attack cards being covered (positional pairs)
//
// Total length must match exactly; anything else is malformed. Card bytes
// are clamped into the representable space (hostile ids become real cards
// the kernel then rejects as not-in-hand — memory-safe by construction).
#ifndef CNITRO_AWIRE_H
#define CNITRO_AWIRE_H

#include "game.h"

#define AWIRE_ATTACK 0
#define AWIRE_COVER  1
#define AWIRE_PASS   2
#define AWIRE_PICKUP 3
#define AWIRE_GOOD   4

// No legal move exceeds 26 cards in any reachable <=52-card state (see the
// MAX_MOVE_CARDS analysis in cnitro/Makefile); 28 matches the wasm build cap.
#define AWIRE_MAX_CARDS 28

typedef struct {
    int  kind;
    int  n;
    Card cards[AWIRE_MAX_CARDS];
    Card attacks[AWIRE_MAX_CARDS]; // cover only
} AwireAction;

// Returns 1 and fills `out` on a well-formed payload, 0 on malformed
// (bad kind, n out of range, or length mismatch). Never reads past len.
int awire_decode(const unsigned char *buf, int len, AwireAction *out);

#endif
