// LEAFBOOK canonicalization (docs/L1_SPEND_PLAN.md §4, "S3").
//
// A round-boundary 2-player deck-empty endgame is fully specified by
// (attacker hand, defender hand, trump suit): num_battles==0, good_mask==0,
// deck empty, roles fixed. Its game value depends on ranks only through
// (a) trump membership, (b) relative rank order in the covering rules, and
// (c) cross-suit rank equality — exactly the RANKSYM invariant
// (cordite_sim.c sim_fingerprint_ranksym). So two positions that are
// order-isomorphic after a global monotone rank-compaction AND a relabeling
// of the three non-trump suits have identical value and share one book entry.
//
// This header is the SINGLE canonical form used by BOTH the offline enumerator
// / book-builder (tools/leafbook/*) and the in-engine probe, so a built key and
// a probed key are bit-identical by construction. It is header-only and depends
// only on <stdint.h> + GCC/clang popcount builtins, so the native tools link it
// without the engine.
//
// Canonical form of (HA, HD, power):
//   1. rank-compact: nr[r] = #present ranks below r, applied to every suit of
//      both hands (global => cross-suit rank equality preserved).
//   2. trump fixed to canonical suit 3; the 3 non-trump suits relabeled by the
//      permutation (of 6) that MINIMIZES the packed (attacker,defender)
//      encoding, both hands relabeled together.
// The result is packed losslessly (<=6 cards => <=6 distinct compacted ranks =>
// rank index 0..5 => 6 bits/suit) into a 48-bit key: (attacker24 << 24)|def24.
#ifndef CNITRO_LEAFBOOK_H
#define CNITRO_LEAFBOOK_H

#include <stdint.h>

// Max total cards (both hands) a book entry covers. Chosen by the enumeration
// feasibility gate (tools/leafbook/enumerate.c). Overridable at build time.
#ifndef LEAFBOOK_K
#define LEAFBOOK_K 6
#endif

// Pack a rank-compacted 52-bit hand (suit su => bits su*13.., each suit block
// already using only compacted rank bits 0..5) into 24 bits: 6 bits/suit.
static inline uint32_t leafbook_pack24(uint64_t H) {
    uint32_t out = 0;
    for (int su = 0; su < 4; su++)
        out |= (uint32_t)((H >> (su * 13)) & 0x3Full) << (su * 6);
    return out;
}

// Canonical 48-bit key for the round-boundary endgame (HA attacker, HD
// defender, power the trump suit). Deterministic; equal iff same value-orbit.
static inline uint64_t leafbook_key(uint64_t HA, uint64_t HD, int power) {
    // ranks present across both hands, all suits
    uint32_t R = 0;
    for (int su = 0; su < 4; su++) {
        R |= (uint32_t)((HA >> (su * 13)) & 0x1FFFu);
        R |= (uint32_t)((HD >> (su * 13)) & 0x1FFFu);
    }
    uint8_t nr[13];
    for (int r = 0; r < 13; r++) nr[r] = (uint8_t)__builtin_popcount(R & ((1u << r) - 1u));
    // per-suit rank-compacted blocks (indexed by ORIGINAL suit)
    uint32_t ca[4], cd[4];
    for (int su = 0; su < 4; su++) {
        uint32_t x = (uint32_t)((HA >> (su * 13)) & 0x1FFFu), c = 0;
        while (x) { int r = __builtin_ctz(x); c |= 1u << nr[r]; x &= x - 1; }
        ca[su] = c;
        x = (uint32_t)((HD >> (su * 13)) & 0x1FFFu); c = 0;
        while (x) { int r = __builtin_ctz(x); c |= 1u << nr[r]; x &= x - 1; }
        cd[su] = c;
    }
    // three non-trump suits (original indices), permuted; trump -> canon suit 3
    static const int P[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    int nt[3], k = 0;
    for (int su = 0; su < 4; su++) if (su != power) nt[k++] = su;
    uint64_t best = ~0ull;
    for (int p = 0; p < 6; p++) {
        // canon suit index for each original suit: trump->3, nt[i]->P[p][i]
        int cs[4];
        cs[power] = 3;
        cs[nt[0]] = P[p][0]; cs[nt[1]] = P[p][1]; cs[nt[2]] = P[p][2];
        uint32_t ha24 = 0, hd24 = 0;
        for (int su = 0; su < 4; su++) {
            ha24 |= (ca[su] & 0x3Fu) << (cs[su] * 6);
            hd24 |= (cd[su] & 0x3Fu) << (cs[su] * 6);
        }
        uint64_t enc = ((uint64_t)ha24 << 24) | (uint64_t)hd24;
        if (enc < best) best = enc;
    }
    return best;
}

#endif
