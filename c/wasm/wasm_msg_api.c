// The kernel bridge shared by every host that is NOT a bot runner: the share
// link (base32, both link styles, and reading one back), the replay code's
// extras blob, and the one-tap cover resolver.
//
// Split out of wasm_bots_api.c, which had grown two halves with nothing in
// common. The bot half (roster, strategy dispatch, the drive cycle, the belief
// probe, the knob table) is bots.wasm's alone; this half is what an iMessage
// extension needs, and it needs none of that. Both modules link this file; the
// msg module links only this one.
//
// The allocator, the env table and the log import stay with the bots: malloc is
// there for the Monte-Carlo scratch, and the env table for their knobs.

#include "game.h"
#include "wire.h"
#include "legal.h"
#include "view.h"
#include "replay.h"
#include "replay_extras.h"
#include <string.h>

extern unsigned char *wasm_io_ptr(void);
extern int wasm_io_cap(void);
extern unsigned char *wasm_replay_io_ptr(void);
extern int wasm_replay_io_cap(void);
extern Game *wasm_game_ptr_internal(void);

// ---------- the replay code's extras blob (#113) -----------------------------
//
// The nicknames and per-move timing behind the dash in a share link. One
// encoder, reached from here by the web and the server and from ios_api.c by
// the phone; there is no second one to drift from.
//
// Same buffer discipline as every other blob-in/blob-out export: the packed
// argument goes in the REPLAY io buffer, the answer comes back in the MAIN one.
// `player_count` and `move_count` on the way back are the decoded moves', not
// the blob's - it carries neither.

// The extras' names and times cross as ONE struct (replay_extras.h
// ReplayExtras), read and written through the generated reader and writer, in
// BOTH directions and for the link's roster below. The packed argument blob the
// codec itself speaks stays where it belongs: this bridge packs and unpacks it
// (replay_extras_pack / _unpack, beside the format), so no host writes those
// bytes. One instance, since no two of these calls overlap.
static ReplayExtras g_extras;
void *wasm_replay_extras_ptr(void) { return &g_extras; }

// The struct at wasm_replay_extras_ptr -> the extras blob in the main IO buffer.
// The packed argument blob is built in the REPLAY buffer on the way, which is
// where a caller used to have to build it itself.
int wasm_replay_extras_encode(void) {
    unsigned char *args = wasm_replay_io_ptr();
    const int n = replay_extras_pack(&g_extras, args, wasm_replay_io_cap());
    if (n < 0) return n;
    return replay_extras_encode(args, n, wasm_io_ptr(), wasm_io_cap());
}

// The extras blob in the REPLAY buffer -> the struct at wasm_replay_extras_ptr.
// Returns the answer's name count and gap count as one non-negative number is
// not enough, so it returns 0 and the struct carries both.
int wasm_replay_extras_decode(int blob_len, int player_count, int move_count) {
    const int n = replay_extras_decode(wasm_replay_io_ptr(), blob_len,
                                       player_count, move_count,
                                       wasm_io_ptr(), wasm_io_cap());
    if (n < 0) return n;
    return replay_extras_unpack(wasm_io_ptr(), n, &g_extras) < 0
        ? -REPLAY_EXTRAS_EINPUT : REPLAY_EXTRAS_EOK;
}

// base32, the way a replay integer travels as text (replay.h). Both directions
// read the REPLAY io buffer and write the MAIN one, so the two never alias.
//
// replay.h has always described this alphabet as "the web's codec.ts alphabet",
// and replay.c's own comment said a code made on the web "reads here byte for
// byte" - which is a mirror admitting it is a mirror. The web asks for it now,
// and there is one alphabet.
int wasm_replay_b32_encode(int in_len) {
    return replay_b32_encode(wasm_replay_io_ptr(), in_len,
                             (char *)wasm_io_ptr(), wasm_io_cap());
}

// `in_len` bytes of ASCII in the replay buffer; NUL-terminated in place, since
// the decoder wants a C string. Stops at '-' (a share link's extras suffix).
int wasm_replay_b32_decode(int in_len) {
    unsigned char *in = wasm_replay_io_ptr();
    if (in_len < 0 || in_len >= wasm_replay_io_cap()) return -1;
    in[in_len] = 0;
    return replay_b32_decode((const char *)in, wasm_io_ptr(), wasm_io_cap());
}

// Reading a link back (replay_extras.h replay_link_parse): the pasted string
// goes in the REPLAY buffer, the bare code comes back in the MAIN one.
int wasm_replay_link_parse(int in_len) {
    unsigned char *in = wasm_replay_io_ptr();
    if (in_len < 0 || in_len >= wasm_replay_io_cap()) return -REPLAY_EXTRAS_EINPUT;
    in[in_len] = 0;
    return replay_link_parse((const char *)in, (char *)wasm_io_ptr(), wasm_io_cap());
}

// The whole shareable link, in one of two styles (REPLAY_LINK_STYLE_*): the
// https link a person copies, or the uppercase scheme-less form a QR wants,
// which stays in QR alphanumeric mode and so fits a smaller version. Same link.
//
// The ROSTER is the struct at wasm_replay_extras_ptr (its names; the times are
// not part of a link). The MOVES CODE is `moves_len` bytes of base32 at the
// front of the REPLAY buffer - an opaque string, not a struct, which a caller
// only forwards: a long v6 game's code runs to tens of KB, so it crosses as
// itself and is NUL-terminated in place here. The link comes back in the MAIN
// buffer. A caller used to have to pack the roster in front of it by hand.
int wasm_replay_link(int moves_len, int style) {
    static unsigned char roster[MAX_PLAYERS * (2 + REPLAY_EXTRAS_NAME_SLOT)];
    unsigned char *in = wasm_replay_io_ptr();
    int w = 0;
    if (moves_len < 0 || moves_len >= wasm_replay_io_cap()) return -REPLAY_EXTRAS_EINPUT;
    if (g_extras.n_names < 0 || g_extras.n_names > MAX_PLAYERS) return -REPLAY_EXTRAS_EINPUT;
    for (int i = 0; i < g_extras.n_names; i++) {
        const int n = g_extras.names[i].len;
        if (n > REPLAY_EXTRAS_NAME_SLOT) return -REPLAY_EXTRAS_EINPUT;
        roster[w++] = (unsigned char)(n & 0xff);
        roster[w++] = (unsigned char)(n >> 8);
        for (int j = 0; j < n; j++) roster[w++] = (unsigned char)g_extras.names[i].text[j];
    }
    in[moves_len] = 0;                         // terminate the moves code in place
    return replay_extras_link_styled((const char *)in, roster, w, g_extras.n_names,
                                     style, (char *)wasm_io_ptr(), wasm_io_cap());
}

// ---------- one-tap cover resolution (A7/F9) --------------------------------
//
// The one-gesture cover affordance, decided in the kernel beside legal.c so the
// web drag, phone tap-commit, watch chooser and iMessage share one resolver
// (docs/C_CORE_CONSOLIDATION.md F9). Self-contained — reads neither the resident
// game nor any marshal, only the handful of cards the caller passes, so it is
// cheap enough to call on a hover:
//   cards_a  = the selected cover cards (n_cover wire bytes)
//   cards_b  = the table battles, 2 wire bytes each (attack, then defense or
//              WIRE_CARD_NONE) — the resolver picks out the uncovered ones
//   power_suit is the argument
// On an unambiguous cover, io_ptr receives n_cover attack wire bytes aligned to
// the cover cards and the return is n_cover; otherwise 0 (caller places manually).
extern unsigned char *wasm_cards_a_ptr(void);
extern unsigned char *wasm_cards_b_ptr(void);
// The shared card buffers (g_in_raw_a/b) are MAX_IN_CARDS=128 wide in wasm_api.c;
// this local cap must not exceed that. cards_a holds cover cards, cards_b holds
// 2 bytes per battle, so battles cap at half.
#define UC_WIRE_MAX 128

int wasm_unambiguous_cover(int n_cover, int n_battles, int power_suit) {
    if (n_cover <= 0 || n_cover > UC_WIRE_MAX) return 0;
    if (n_battles < 0 || n_battles > UC_WIRE_MAX / 2) return 0;

    const unsigned char *cov = wasm_cards_a_ptr();
    const unsigned char *bat = wasm_cards_b_ptr();

    Card cover[UC_WIRE_MAX];
    for (int i = 0; i < n_cover; i++) cover[i] = card_from_wire_state(cov[i]);

    Battle battles[UC_WIRE_MAX / 2];
    for (int i = 0; i < n_battles; i++) {
        battles[i].attack = card_from_wire_state(bat[2 * i]);
        unsigned char d = bat[2 * i + 1];
        battles[i].defense = (d == WIRE_CARD_NONE) ? CARD_NONE : card_from_wire_state(d);
    }

    Card out[UC_WIRE_MAX];
    int r = unambiguous_cover(cover, n_cover, battles, n_battles, power_suit, out);
    if (r <= 0) return 0;

    unsigned char *io = wasm_io_ptr();
    for (int i = 0; i < n_cover; i++) io[i] = wire_from_card(out[i]);
    return n_cover;
}
