// ios_table_goldens.c - emits ios/Fixtures/table_goldens.bin, the one table
// layout as the KERNEL writes it, for the Swift suite to be measured against
// (ios/FoolishTests/TableWireTests.swift).
//
// WHY IT EXISTS. Swift wrote the 2-bytes-per-battle table FOUR times
// (PlayWire, ConflictWire, PreTableWire twice), and the four disagreed about
// the one case none was built for - a card on the table the viewer may not
// see. Two spelled it "no card", which opens a battle that is closed; one
// refused the board; one had the right byte. All four now go through
// fio_table_encode, so the bytes and both sentinels are C's; what is left to
// Swift is the MAPPING from a BattleView onto the door's pairs - a card's own
// suit and value, a missing cover as the kernel's bare pair, a masked back as
// the (-1, -1) it already is - and that is what this fixture pins.
//
// THE MASKED VECTORS ARE THE POINT. A fixture of well-formed tables would pass
// against an encoder that still spelled the masked back as "no card"; the
// vectors below make the unnameable byte a golden, in every slot.
//
// NO JSON. It is bytes, like everything else that crosses between these two
// languages. Layout, all of it little-endian and nothing wider than a byte:
//
//   0  u8  format (TABLE_GOLDEN_FORMAT)
//   1  u8  vector count
//   then per vector:
//     u8  name_len, name_len bytes of ASCII  (so a failure can NAME itself)
//     u8  n_battles, n_battles x { i8 attack suit, i8 attack value,
//                                  i8 cover suit,  i8 cover value }
//     u8  wire_len, wire_len bytes of table wire (2 x n_battles)
//
// A cover pair of (FIO_CARD_NONE, FIO_CARD_NONE) is a bare attack, which the
// Swift reader turns back into `defense: nil`. There are no refusal vectors:
// the encoder is total over content (ios_api.h), and its only refusals - a
// null pointer, a short buffer - are not shapes a fixture can carry. The one
// pair no host can build, a bare ATTACK, is pinned in ios_api_smoke.c instead.
//
// Build/run via `make ios-goldens` (writes the file) or run directly to stdout.

#include "ios_api.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define TABLE_GOLDEN_FORMAT 1
#define MAX_VEC_BATTLES 4

typedef struct {
    const char *name;
    int         n_battles;
    int8_t      pairs[4 * MAX_VEC_BATTLES];
} TableVec;

#define N FIO_CARD_NONE     // a bare cover
#define H (-1)              // a masked back, in either slot

// Card ids, for reading the expected bytes back: suit*13 + (value-1), so
// spades1=0, spades6=5, hearts9=21, clubsJ=36, diamonds10=48, diamondsK=50,
// diamondsA=51.
static const TableVec VECTORS[] = {
    { "empty",              0, { 0 } },
    { "bare",               1, { 0, 6, N, N } },
    { "covered",            1, { 0, 6, 1, 9 } },
    { "two-battles",        2, { 0, 6, N, N,  2, 11, 3, 10 } },
    { "lowest-id",          1, { 0, 1, N, N } },
    { "highest-id",         1, { 3, 13, 3, 12 } },
    // ---- the masked cases ----
    { "masked-attack",      1, { H, H, N, N } },
    { "masked-cover",       1, { 0, 6, H, H } },
    { "masked-both",        1, { H, H, H, H } },
    // A masked cell beside named ones, so a host that dropped or refused the
    // battle instead of writing the byte fails here on the OTHER cells' shift.
    { "masked-among-named", 3, { 0, 6, 1, 9,  H, H, N, N,  2, 11, H, H } },
    // Off the deck without being the masked pair: still a card nobody can
    // name, never a card the arithmetic would invent.
    { "suit-off-the-deck",  1, { 4, 6, N, N } },
    { "value-off-the-deck", 1, { 0, 14, 0, 0 } },
};

static void put(unsigned char b) { fputc(b, stdout); }

static int emit(const TableVec *v) {
    unsigned char wire[2 * MAX_VEC_BATTLES];
    const int rc = fio_table_encode(v->pairs, v->n_battles, (char *)wire, (int)sizeof wire);
    if (rc < 0 || rc != 2 * v->n_battles) {
        fprintf(stderr, "table goldens: %s rc=%d\n", v->name, rc);
        return 1;
    }
    const size_t name_len = strlen(v->name);
    if (name_len > 255) { fprintf(stderr, "table goldens: name too long\n"); return 1; }
    put((unsigned char)name_len);
    fwrite(v->name, 1, name_len, stdout);
    put((unsigned char)v->n_battles);
    for (int i = 0; i < 4 * v->n_battles; i++) put((unsigned char)v->pairs[i]);
    put((unsigned char)rc);
    fwrite(wire, 1, (size_t)rc, stdout);
    return 0;
}

int main(void) {
    const int n = (int)(sizeof VECTORS / sizeof VECTORS[0]);
    put(TABLE_GOLDEN_FORMAT);
    put((unsigned char)n);
    for (int i = 0; i < n; i++) if (emit(&VECTORS[i])) return 1;
    return fflush(stdout) == 0 ? 0 : 1;
}
