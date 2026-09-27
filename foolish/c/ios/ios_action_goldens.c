// ios_action_goldens.c - emits ios/Fixtures/action_goldens.bin, the awire
// action frame as the KERNEL writes it, for the Swift suite to be measured
// against (ios/FoolishTests/PackedActionTests.swift).
//
// WHY IT EXISTS. Swift used to write the action frame itself, twice
// (sdk/swift/MoveWire.swift and ios/FoolishNet/PackedAction.swift), and its
// tests asserted byte literals typed out beside those encoders - Swift pinned
// to Swift, which cannot fail for the reason that matters. Both now go through
// fio_awire_encode, so the bytes are C's; what is left to Swift is the MAPPING
// from a Move onto that door's arguments, and that is what this fixture pins:
// the type number, the cards in order, and which cover card lands on which
// attack.
//
// THE REFUSALS ARE GOLDENS TOO. Which moves have no frame at all - a wait, a
// pickup carrying cards, a cover whose runs do not pair up, a move over the
// card cap - is equally the kernel's answer, and a host that quietly wrote one
// anyway is the whole reason the door exists.
//
// NO JSON. It is bytes, like everything else that crosses between these two
// languages. Layout, all of it little-endian and nothing wider than a byte:
//
//   0  u8  format (ACTION_GOLDEN_FORMAT)
//   1  u8  vector count
//   then per vector:
//     u8  name_len, name_len bytes of ASCII  (so a failure can NAME itself)
//     u8  type                               MOVE_* index; 5 (wait) has no frame
//     u8  n_cards,   n_cards   x { i8 suit, i8 value }
//     u8  n_attacks, n_attacks x { i8 suit, i8 value }
//     u8  frame_len, frame_len bytes of awire frame
//
// frame_len 0 means THE KERNEL REFUSED, in band: the shortest frame it will
// ever write is two bytes (a kind and a zero count), so an empty one is not a
// frame that happens to be short.
//
// Build/run via `make ios-goldens` (writes the file) or run directly to stdout.

#include "ios_api.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define ACTION_GOLDEN_FORMAT 1

typedef struct {
    const char *name;
    int         type;          // MOVE_* / AWIRE_* index
    int         n_cards;
    int8_t      cards[2 * 30];
    int         n_attacks;
    int8_t      attacks[2 * 30];
} ActionVec;

// Card ids, for reading the expected bytes back: suit*13 + (value-1), so
// spades6=5, hearts6=18, hearts9=21, spades7=6, clubs6=31, clubsJ=36,
// diamonds6=44, diamonds10=48, spadesA=12, diamondsA=51, spades1=0.
static const ActionVec VECTORS[] = {
    { "attack-one",        0, 1, { 0, 6 },        0, { 0 } },
    { "attack-two",        0, 2, { 0, 6, 1, 6 },  0, { 0 } },
    { "attack-lowest-id",  0, 1, { 0, 1 },        0, { 0 } },
    { "attack-highest-id", 0, 1, { 3, 13 },       0, { 0 } },
    { "cover-one",         1, 1, { 1, 9 },        1, { 0, 7 } },
    // Two pairs, deliberately NOT in the same order as the attacks they land
    // on: the frame is cards-then-attacks and positional, so a host that
    // interleaved the two runs, or sorted either of them, fails here and
    // nowhere else.
    { "cover-two",         1, 2, { 1, 9, 2, 11 }, 2, { 0, 7, 3, 10 } },
    { "pass-one",          2, 1, { 2, 6 },        0, { 0 } },
    { "pass-two",          2, 2, { 2, 6, 3, 6 },  0, { 0 } },
    { "pickup",            3, 0, { 0 },           0, { 0 } },
    { "good",              4, 0, { 0 },           0, { 0 } },
    // ---- the refusals ----
    { "wait-has-no-frame", 5, 0, { 0 },           0, { 0 } },
    { "pickup-with-cards", 3, 1, { 0, 6 },        0, { 0 } },
    { "good-with-cards",   4, 1, { 0, 6 },        0, { 0 } },
    { "cover-short",       1, 2, { 1, 9, 2, 11 }, 1, { 0, 7 } },
    { "cover-unpaired",    1, 1, { 1, 9 },        2, { 0, 7, 3, 10 } },
};

static void put(unsigned char b) { fputc(b, stdout); }

static void put_pairs(const int8_t *p, int n) {
    put((unsigned char)n);
    for (int i = 0; i < 2 * n; i++) put((unsigned char)p[i]);
}

static int emit(const ActionVec *a) {
    unsigned char frame[128];
    const int rc = fio_awire_encode(a->type, a->cards, a->n_cards,
                                    a->attacks, a->n_attacks,
                                    (char *)frame, (int)sizeof frame);
    if (rc > 255) { fprintf(stderr, "action goldens: %s does not fit a length byte\n", a->name); return 1; }
    const size_t name_len = strlen(a->name);
    if (name_len > 255) { fprintf(stderr, "action goldens: name too long\n"); return 1; }
    put((unsigned char)name_len);
    fwrite(a->name, 1, name_len, stdout);
    put((unsigned char)a->type);
    put_pairs(a->cards, a->n_cards);
    put_pairs(a->attacks, a->n_attacks);
    if (rc < 0) { put(0); return 0; }                       // refused, in band
    put((unsigned char)rc);
    fwrite(frame, 1, (size_t)rc, stdout);
    return 0;
}

int main(void) {
    const int n = (int)(sizeof VECTORS / sizeof VECTORS[0]);
    // The over-cap vector is built rather than written out: AWIRE_MAX_CARDS + 1
    // of the same card, which is over the kernel's cap and is also where the
    // Swift encoder this replaced used to TRAP rather than refuse.
    ActionVec over = { "attack-over-cap", 0, 29, { 0 }, 0, { 0 } };
    for (int i = 0; i < over.n_cards; i++) { over.cards[2 * i] = 0; over.cards[2 * i + 1] = 6; }

    put(ACTION_GOLDEN_FORMAT);
    put((unsigned char)(n + 1));
    for (int i = 0; i < n; i++) if (emit(&VECTORS[i])) return 1;
    if (emit(&over)) return 1;
    return fflush(stdout) == 0 ? 0 : 1;
}
