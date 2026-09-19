// ios_api_replay.c - REPLAYS AND THE EVWIRE STREAM THEY PRODUCE (see
// ios/include/ios_api.h).
//
// Split out of ios_api.c by domain, unchanged. The share code is taken off the
// resident game (fio_session, ios_internal.h); everything else here is a
// function of the code it is handed, so a host may ask it about a chain it
// never adopted.

#include "ios_api.h"
#include "ios_internal.h"

#include "game.h"
#include "replay.h"
#include "replay_steps.h"
#include "replay_extras.h"
#include "evwire.h"

// The last replay_* failure, kept for fio_last_replay_error.
static int g_last_replay_error = 0;

// ---------- replays (§16.C) ------------------------------------------------
//
// DECODE is implemented: base32 (RFC 4648 uppercase, no padding) → the replay
// integer bytes → replay_decode() → the packed step wire. This is byte-parity
// with the server by construction (shared replay.c), so a web-generated code
// plays natively.

// base32 lives in replay.c (replay_b32_decode / replay_b32_encode).

// The exact game, hidden state and all. One kernel call - the deal seed was kept
// at fio_new_game, the actions are this game's own logs, and the reveal stream
// is re-derived inside the kernel, so the app assembles nothing. This is the
// same replay_encode_v6_from_game the server calls through
// wasm_replay_encode_v6_from_game, so an offline share and a site share are the
// same code.
//
// Returns FIO_ENOSEED for a game dealt without a wide seed: its deal cannot be
// re-derived, and there is no second format to fall back to any more.
int fio_replay_encode_v6_b32(char *out, int cap) {
    FioSession *s = fio_session();
    if (!s->has_game) return FIO_ENOGAME;
    if (!s->has_deal_seed) return FIO_ENOSEED;
    g_last_replay_error = 0;
    static unsigned char encout[16384];
    int enclen = replay_encode_v6_from_game(&s->game, s->deal_seed, FOOLISH_SEED_LEN,
                                            1 << 30, encout, sizeof(encout));
    if (enclen < 0) { g_last_replay_error = -enclen; return FIO_EREPLAY; }
    int w = replay_b32_encode(encout, enclen, out, cap);
    if (w < 0) return FIO_ECAP;
    return w;
}

// THE call a client makes for a share code. It is a straight pass-through today
// because there is one replay format left, and it stays a separate entry point
// because WHICH FORMAT TO SHARE IS NOT AN APP DECISION: if that choice lived in
// Swift then the watch, the iMessage extension and every later client would each
// reimplement it and drift.
//
// It used to fall back to the retrodiction encoder when the deal could not be
// re-derived, and that fallback is deliberately gone rather than replaced. A
// retrodicted code is not a shorter recording of this game - it is a DIFFERENT
// game, one whose hidden hands the decoder guesses by complement - so shipping
// one under a share button trades a visible failure for an invisible lie. A
// game with no re-derivable deal now returns FIO_ENOSEED, and the caller shows
// that. (Swift always deals through fio_new_game with 32 bytes, so this is the
// unreachable branch made honest, not a capability withdrawn.)
int fio_replay_share_code_b32(char *out, int cap) {
    return fio_replay_encode_v6_b32(out, cap);
}

// The whole shareable link, prefix and all (ios_api.h). Pure function of its
// arguments - no resident game - so a caller may reach it from anywhere.
int fio_replay_share_link(const char *moves,
                          const unsigned char *names, int names_len, int n_names,
                          char *out, int cap) {
    int n = replay_extras_link(moves, names, names_len, n_names, out, cap);
    if (n == -REPLAY_EXTRAS_ECAP) return FIO_ECAP;
    if (n < 0) return FIO_EBADARG;
    return n;
}


// The animations of the chain's LAST TURN, as packed evwire frames — the "what
// just happened" an iMessage receiver sees on opening a bubble. Same packed
// evwire the website renders and live play broadcasts; Swift reads it with
// EvWire.decodeFrames.
//
// THE KERNEL decides the group; the client passes the encoded chain and the one
// fact the chain cannot hold - `atoms_before`, how many atoms were on it BEFORE
// this bubble (-1 for "cannot say"). That is a property of the BUBBLE (its
// `turn` minus the delta it carries, msg_wire.h's n_new), never "where I last
// looked": a device's cache must not decide what animates.
//
// Why the count BEFORE rather than the count added, when the wire carries the
// latter: because the two are equivalent for a receiver and only this one is
// answerable by a SENDER. A device animating its own just-played move knows
// exactly what it adopted (the parent's turn) but not how many atoms its moves
// became - the codec is not 1:1 with actions, it folds a bout's closing goods
// into one round_end atom and can expand a closing good into two. Taking the
// base lets the kernel do that arithmetic against its own step count, so
// neither side has to guess.
//
// A turn is not an action. This used to hand back the final step alone, on the
// reasoning that a v6 replay is the deal (step 0) then exactly one step per
// action, and that each step already bundles an action with ALL its
// kernel-internal consequences — a `pickup` step carries the PICKUP, every
// seat's refill draws, and the defender change, from the one handle_pickup
// call. All true, and still one action short of what a BUBBLE carries: a
// player stages as many actions as they like before sending, so a defender who
// covers two attacks sends one bubble holding two cover steps. Replaying only
// the last of them showed the first cover already sitting on the table, landed
// and rotated, while the second flew in — "if it's a double cover, the first
// cover will just already be there, and only the second one will play."
//
// So the group is a SUFFIX of the step stream, and `atoms_before` says where it
// starts. A v6 replay is the deal then exactly one step per atom (replay_steps.c's
// rs_collect keeps every atom but DEAL/DRAW, which is precisely the set
// msg_replay counts into `turn`), so a chain with B atoms behind it opens its
// bubble at step B+1 and runs to the end - the atoms this bubble put on the
// chain, and nothing that was already there. B == the atom count means the
// bubble added nothing and nothing animates, which is the honest answer for the
// one bubble that can do it (an undo-to-empty re-seal, §10).
//
// WITHOUT a base (-1: a format-2 chain sealed before round 16, or a chain whose
// delta did not fit) it falls back to the guess that shipped before the field
// existed: the trailing run of steps by ONE acting seat - walk back
// over the seatless tail (ROUND_END belongs to whoever caused it), then back
// over every immediately preceding step by that same seat. That is right only
// when the sender staged its whole run and sent once, and the owner hit both
// ways it is wrong: covering, sending, covering, sending replays BOTH covers on
// the second bubble; and a cover that ends the bout with no ROUND_END atom (the
// defender's last card - handle_cover discards inline) sits directly before
// that same seat's opening attack of the next bout, so replaying the attack
// replays the cover with it. Both are exact with a base, which is why the wire
// field was added rather than the heuristic sharpened - no walk over the steps
// can separate two bubbles that a single bubble could have produced.
//
// Every frame is masked for `viewer` exactly like live play: the viewer's own
// drawn/picked-up cards carry real identities (fixing "my own refill never
// animated on reopen"), everyone else's are hidden backs.
//
// Frames come back in the shape replay_steps_frames_v6 writes them — each
// preceded by a u16 LE length, in play order. v6 only. Returns bytes written
// (0 if the turn produced nothing to animate), or a negative error.
int fio_replay_last_events_packed(const char *code, int viewer, int atoms_before,
                                  unsigned char *out, int cap) {
    if (!code || !out) return FIO_EBADARG;
    g_last_replay_error = 0;

    static unsigned char intbuf[16384];
    int ilen = replay_b32_decode(code, intbuf, sizeof(intbuf));
    if (ilen < 0) return FIO_ECAP;

    // What each step IS (kind + acting seat), so the run can be found without
    // decoding a single frame first. The ceiling is the same one `intbuf`
    // already implies — a chain that decodes to more actions than this could
    // not have fit in the 16KB buffer above in the first place — and
    // replay_steps_index_v6 returns -REPLAY_ECAP rather than truncating.
    #define FIO_MAX_REPLAY_STEPS 2048
    static unsigned char idx[RS_INDEX_STRIDE * FIO_MAX_REPLAY_STEPS];
    int ilen_idx = replay_steps_index_v6(intbuf, ilen, 0, idx, sizeof idx);
    if (ilen_idx < 0) { g_last_replay_error = -ilen_idx; return FIO_EREPLAY; }
    const int n = ilen_idx / RS_INDEX_STRIDE;
    if (n <= 0) return 0;

    const int last = n - 1;
    int from = last;
    if (atoms_before >= 0) {
        // The bubble told us where it starts: step 0 is the DEAL, which no
        // bubble adds, so B atoms behind it means step B+1 onward…
        from = atoms_before + 1;
        if (from < 1) from = 1;
        if (from > n) from = n;          // added nothing: animate nothing
        // …except on a chain that IS only the deal (a genesis or the lobby's
        // LIVE handoff, n == 1), where the deal is the one thing to show.
        if (n == 1) from = 0;
    } else {
        // No delta: the pre-round-16 guess. See the note above for what it
        // cannot separate.
        // Back over the seatless tail (ROUND_END) to the acting step that caused it.
        int a = last;
        while (a > 0 && idx[a * RS_INDEX_STRIDE + 1] == RS_SEAT_NONE) a--;
        const unsigned char actor = idx[a * RS_INDEX_STRIDE + 1];
        if (actor != RS_SEAT_NONE) {
            from = a;
            // …then back over every step that seat played immediately before it.
            // Never across another seatless step: that is a closed bout, and the
            // run on its far side is a different turn.
            while (from > 1 && idx[(from - 1) * RS_INDEX_STRIDE + 1] == actor) from--;
        }
    }

    // The group runs to the end of the stream, so asking for [from, ...) is
    // exactly it. Length-prefixed frames, in play order, as written.
    int n_frames = 0, next_step = 0;
    int r = replay_steps_frames_v6(intbuf, ilen, viewer, from, 0,
                                   out, cap, &n_frames, &next_step);
    if (r < 0) { g_last_replay_error = -r; return FIO_EREPLAY; }
    if (n_frames <= 0) return 0;                   // nothing to animate
    // A short buffer must not silently drop the END of the turn — the newest
    // action is the one the viewer most needs to see. FIO_ECAP, not
    // FIO_EREPLAY: a turn of several frames (each carrying a masked board) can
    // outgrow the caller's first guess, and ECAP is the code that makes the
    // Swift side retry with a bigger buffer instead of giving up on the
    // animation. A real decode failure is still FIO_EREPLAY above.
    if (next_step <= last) { g_last_replay_error = REPLAY_ECAP; return FIO_ECAP; }
    return r;
}

// The replay decode as its RAW binary (replay.h DECODE layout: a 20-byte header
// then n_logs records of [type,seat,defIdx,n_pairs] + n_pairs*[primary,target]
// wire-card bytes). Swift parses this directly (DecodedReplay.decode): the
// stream is handed over whole rather than walked here. Card bytes: 0xFF none,
// 0xFE hidden, else id.
int fio_replay_decode_packed(const char *code, unsigned char *out, int cap) {
    if (!code) return FIO_EBADARG;
    g_last_replay_error = 0;
    static unsigned char intbuf[16384];
    int ilen = replay_b32_decode(code, intbuf, sizeof(intbuf));
    if (ilen < 0) return FIO_ECAP;
    int dlen = replay_decode(intbuf, ilen, out, cap);
    if (dlen < 0) { g_last_replay_error = -dlen; return FIO_EREPLAY; }
    if (dlen < REPLAY_DEC_HDR) { g_last_replay_error = REPLAY_EINPUT; return FIO_EREPLAY; }
    return dlen;
}

int fio_last_replay_error(void) { return g_last_replay_error; }

// Straight through to the rule in evwire.c - see ios_api.h for why the
// extension asks the kernel rather than switching on the type itself.
int fio_evw_is_settlement(int type) { return evw_is_settlement(type); }

// THE CUT, over the frame stream fio_replay_last_events_packed just handed
// back. The clients flatten those frames into one event list, so the answer is
// an index into the FLATTENED list; the walk across frames is in evwire.c and
// this is one line so it stays that way.
int fio_evw_frames_settlement_cut(const unsigned char *frames, int len) {
    return evwire_frames_settlement_cut(frames, len);
}

// WHERE THE FRAMES ARE in that same stream, so a host hands one sequence at a
// time to fio_push_open without knowing that the container is a u16 length
// prefix. Writes off[i]/len[i] per frame and returns the count (or a negative
// EVW_E*), and counts alone when both are NULL.
int fio_evw_frames(const unsigned char *frames, int len, int *off, int *flen, int cap) {
    return evwire_frames(frames, len, off, flen, cap);
}
