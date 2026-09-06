// The kernel bridge shared by every host that is NOT a bot runner: the replay
// codec, the share link, the packed->JSON decoders and the one-tap cover
// resolver.
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
#include "replay_steps.h"
#include "view.h"
#include "json_out.h"
#include "replay_extras.h"
#include <string.h>

extern unsigned char *wasm_io_ptr(void);
extern int wasm_io_cap(void);
extern unsigned char *wasm_replay_io_ptr(void);
extern int wasm_replay_io_cap(void);
extern Game *wasm_game_ptr_internal(void);

// ---------- replay steps (docs/C_CORE_CONSOLIDATION.md F4.2 / A5) -----------
// A v6 code, replayed through the REAL engine, serialized as the SAME packed
// evwire frames live play broadcasts — so a replay screen decodes and renders
// what it already decodes and renders, with no replay-side projection.
//
// Lives in bots.wasm, not rules.wasm, and not by preference: replay_steps.c's
// decoded-action buffer alone is ~272 KB, while rules.wasm's whole linear
// memory is PINNED at 196,608 B (--initial-memory == --max-memory). It is 1.4x
// the module before anything else. Per the standing steer, the fix is one big
// module everywhere rather than shrinking the replay to fit the small one
// (docs/C_CORE_CONSOLIDATION.md A10 / RULES_GUARDS_WASM_MEMORY_PLAN.md).
//
// The code goes in the REPLAY io buffer (it is a replay input, like every other
// wasm_replay_* call); the frames come back in the MAIN io buffer, so the two
// never alias.
extern unsigned char *wasm_replay_io_ptr(void);
extern int wasm_replay_io_cap(void);
extern int wasm_io_cap(void);

static int g_rs_n_frames, g_rs_next_step;

int wasm_replay_events(int viewer, int from, int code_len) {
    g_rs_n_frames = 0;
    g_rs_next_step = from;
    return replay_steps_frames_v6(wasm_replay_io_ptr(), code_len,
                                  viewer < 0 ? VIEW_SPECTATOR : viewer, from, 0,
                                  wasm_io_ptr(), wasm_io_cap(),
                                  &g_rs_n_frames, &g_rs_next_step);
}
int wasm_replay_events_n(void)    { return g_rs_n_frames; }
int wasm_replay_events_next(void) { return g_rs_next_step; }

// Steps the code replays to (the deal + one per action). Sizes the scrubber.
int wasm_replay_step_count(int code_len) {
    return replay_steps_count_v6(wasm_replay_io_ptr(), code_len, 0);
}

// What each step is: 4 bytes per step into the MAIN io buffer (the code is in
// the replay buffer, as for every wasm_replay_* call, so the two never alias).
// A whole game's index is steps*4 bytes — a couple of KB — so unlike the frames
// this needs no chunking.
int wasm_replay_step_index(int code_len) {
    return replay_steps_index_v6(wasm_replay_io_ptr(), code_len, 0,
                                 wasm_io_ptr(), wasm_io_cap());
}

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

int wasm_replay_extras_encode(int in_len) {
    return replay_extras_encode(wasm_replay_io_ptr(), in_len,
                                wasm_io_ptr(), wasm_io_cap());
}

int wasm_replay_extras_decode(int blob_len, int player_count, int move_count) {
    return replay_extras_decode(wasm_replay_io_ptr(), blob_len,
                                player_count, move_count,
                                wasm_io_ptr(), wasm_io_cap());
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
// The REPLAY buffer holds
// [u8 n_names][u16 roster_len][roster bytes][moves bytes], the link comes back
// in the MAIN one.
//
// The moves code goes LAST so it can be NUL-terminated in place - it is the one
// argument replay_extras_link wants as a C string, and a long v6 game's code
// runs to tens of KB, which is not a thing to copy through a fixed buffer. One
// spare byte at the end of the input is what that costs.
int wasm_replay_link(int in_len, int style) {
    unsigned char *in = wasm_replay_io_ptr();
    int n_names, roster_len, p;
    if (in_len < 3 || in_len >= wasm_replay_io_cap()) return -REPLAY_EXTRAS_EINPUT;
    n_names = in[0];
    roster_len = in[1] | (in[2] << 8);
    p = 3;
    if (roster_len < 0 || p + roster_len > in_len) return -REPLAY_EXTRAS_EINPUT;
    in[in_len] = 0;                            // terminate the moves code in place
    return replay_extras_link_styled((const char *)(in + p + roster_len),
                                     in + p, roster_len, n_names, style,
                                     (char *)wasm_io_ptr(), wasm_io_cap());
}

// ---------- packed bytes -> JSON (A8/F7) ------------------------------------
//
// The browser's way into the kernel's decoders. The web used to read these two
// formats with hand-written TypeScript that shadowed view.c and evwire.c byte
// for byte, kept true by a parity test — i.e. the layout existed twice and a
// change meant editing both. Now the layout exists once, here, and the client
// asks for objects. iOS already worked this way (ios_api.c); this is the same
// C, reached through a different door.
//
// Buffer discipline matches every other blob-in/blob-out export on this module:
// the packed input goes in the REPLAY io buffer, the JSON comes back in the MAIN
// one, so the two never alias. Both return bytes written (the caller reads that
// many from wasm_io_ptr) or a negative JSON_E* code.

int wasm_view_json(int len, int viewer) {
    return json_view_from_packed(wasm_replay_io_ptr(), len, viewer,
                                 (char *)wasm_io_ptr(), wasm_io_cap());
}

int wasm_events_json(int len) {
    return json_events_from_packed(wasm_replay_io_ptr(), len,
                                   (char *)wasm_io_ptr(), wasm_io_cap());
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
