// Event wire ("evwire" v1) — one recipient's animation sequence as packed
// bytes. This is the C port of the
// TS buildEvents (sdk/ts/wasm/engine.ts) fused with the
// per-recipient masking that convertToPersonal/PublicAnimationEvents did:
// the same engine hook snapshots + kernel logs drive it, and every event's
// game_state crosses as a viewer-masked put_state payload (view.c).
//
//   u8 EVWIRE_FORMAT_VERSION
//   u8 viewer seat (0xFF spectator)
//   u8 actor seat  (0xFF none) — lets the client reproduce the good_players
//                   insertion-order rule without the order on the wire
//   u8 n_events
//   per event:
//     u8 type       EVW_T_* (mirrors ANIMATION_EVENT_TYPE)
//     u8 seat       event player seat, 0xFF none
//     u8 msg_code   EVW_MSG_* — which message template this step carries
//                   (strings are reconstructed by the decoder; the client UI
//                   never rendered them, tests assert template parity)
//     u8 from_loc, u8 to_loc   EVW_LOC_*, 0xFF none
//     u8 flags      bit0 target_card follows, bit1 battle_index follows
//     u8 n_cards, n x u8 wire cards  (DEAL/REFILL masked to WIRE_CARD_HIDDEN
//                                     unless viewer == seat)
//     [u8 target_card] [u8 battle_index]
//     u16 snap_len, snap_len bytes: viewer-masked put_state of this step
//   trailer:
//     u16 final_len, final_len bytes: viewer-masked put_state of the final
//                                     committed state (the payload's `game`)
#ifndef CNITRO_EVWIRE_H
#define CNITRO_EVWIRE_H

#include "game.h"

#define EVWIRE_FORMAT_VERSION 1

// Event types — mirrors ANIMATION_EVENT_TYPE in _shared/core/types.ts.
#define EVW_T_MAGIC_TRANSITION 0
#define EVW_T_DEAL             1
#define EVW_T_FLIPPED          2
#define EVW_T_DEFENDER_MOVE    3
#define EVW_T_ATTACK_PASS      4
#define EVW_T_COVER            5
#define EVW_T_PICKUP           6
#define EVW_T_DISCARD          7
#define EVW_T_OUT              8
#define EVW_T_REFILL           9
#define EVW_T_CARDS_TO_TRASH   10

// Locations.
#define EVW_LOC_DECK    0
#define EVW_LOC_HAND    1
#define EVW_LOC_TABLE   2
#define EVW_LOC_DISCARD 3
#define EVW_LOC_FLIPPED 4
#define EVW_LOC_NONE    0xFF

// Message templates (buildEvents' literal strings, reconstructed TS-side).
#define EVW_MSG_NONE             0
#define EVW_MSG_ATTACKED         1
#define EVW_MSG_PASSED           2
#define EVW_MSG_OUT              3
#define EVW_MSG_COVERED          4
#define EVW_MSG_DISCARDED        5
#define EVW_MSG_DREW             6
#define EVW_MSG_DEFENDER_MOVE    7
#define EVW_MSG_PICKUP           8
#define EVW_MSG_GOOD_TRANSITION  9
#define EVW_MSG_START_MAGIC      10
#define EVW_MSG_FIRST_ATTACKER   11

#define EVW_SEAT_NONE 0xFF

// One captured engine-hook snapshot (the wasm bridges store the log-free
// Game prefix per hook; both real Games and prefix slots serialize fine
// because state_put only reads prefix fields).
typedef struct {
    const Game *g;
    int tag;   // ENGINE_HOOK_*
    int aux;   // acting/affected seat, or battle index for COVER
} EvSnap;

// One derived animation event, before anything writes it down.
//
// The kernel already knows which card flies where — it is the only thing that
// does. Exposing the derived event (rather than only the packed evwire bytes)
// lets a second destination consume it without re-deriving anything, which is
// why `BoardDiff.swift` is cancelled
// (docs/C_CORE_CONSOLIDATION.md F4). `cards`/`snap` point into the caller's
// buffers and are valid only for the duration of the sink call.
typedef struct {
    int         type;        // EVW_T_*
    int         seat;        // event player seat, or -1
    int         msg;         // EVW_MSG_*
    int         from, to;    // EVW_LOC_*
    const Card *cards;
    int         n_cards;
    int         mask_cards;  // DEAL/REFILL redaction: emit card backs
    int         has_target;
    Card        target;
    int         has_battle;
    int         battle;
    const Game *snap;        // board state at this step
} EvwEvent;

typedef void (*EvwSink)(void *ctx, const EvwEvent *ev);

// THE SETTLEMENT: is this step a CONSEQUENCE of the action rather than the
// action itself?
//
// Three moves close a bout - a good that was the last one owed, a cover that
// empties the defender's hand, a pickup - and each of them runs, inside the
// same handle_*, the discard, the refill and the rotation that follow
// (game.c's execute_round_transition, apply_cover's clean sweep,
// handle_pickup). Those steps are what this names. Everything before the
// first of them is the acting seat's own play: the card they laid down, the
// table they took, the good they declared.
//
// It is a rules question, so it is answered here rather than by each client
// re-listing the types: a settlement is the transition marker, the discard,
// the refill, and the trash sweep. The OUT and DEFENDER_MOVE steps are NOT
// listed - they occur without a settlement too (a pass moves the defender; an
// attack can put its attacker out) - but they always trail one when there is
// one, and a caller that cuts at the FIRST settlement step keeps them on the
// settlement's side, which is where they belong.
//
// Why the kernel is asked at all: an iMessage move is staged before it is
// sent, and a staged bout-ender that dealt its sender a new hand on the spot
// would let them read the deal and then undo (or delete the bubble) - so the
// extension plays the action, holds the settlement, and releases it on Send.
int evw_is_settlement(int type);

// Derive the event sequence for `viewer` from the hook snapshots + this
// action's logs, handing each event to `sink`. THE one derivation; both
// evwire_serialize and the settlement cut drive it.
void evwire_walk(const EvSnap *snaps, int n_snaps,
                 const GameLog *logs, int n_logs, int viewer,
                 EvwSink sink, void *ctx);

// ---------- reading the format back -----------------------------------------
//
// The writer above is only half a format. Everything the kernel does with an
// event stream downstream of serialization - ask where a turn settles, whatever
// a later stage needs - has to walk these bytes, and a walk
// inlined at each of those sites is the same duplication the wire exists to
// end, just moved indoors. So the reader lives here, beside the writer, and the
// byte offsets are written down exactly once.
//
// Strictness is part of the contract: a sequence that does not decode WHOLE is
// unreadable, never a prefix. Half a sequence rendered as a whole one is worse
// than none.

#define EVW_EBADARG -1   // null/short buffer
#define EVW_EPARSE  -2   // truncated, or a format version that is not ours

// The four header bytes.
typedef struct {
    int version;
    int viewer;    // seat, or -1 for a spectator sequence
    int actor;     // seat, or -1
    int n_events;
} EvwHeader;

// One event as it sits ON THE WIRE - undecoded, borrowed. Cards stay wire bytes
// so a reader can still tell a redacted card (WIRE_CARD_HIDDEN) from a real one,
// which is a distinction card_from_wire_state throws away; the snapshot stays
// packed because decoding it costs a Game-sized slot the caller may not want.
// Every pointer is into the caller's buffer and lives only for the sink call.
typedef struct {
    int type;                        // EVW_T_*
    int seat;                        // seat, or -1
    int msg;                         // EVW_MSG_*
    int from, to;                    // EVW_LOC_*
    int has_target, has_battle;
    unsigned char target_wire;       // valid iff has_target
    int battle;                      // valid iff has_battle
    const unsigned char *cards_wire; // n_cards wire bytes
    int n_cards;
    const unsigned char *snap;       // this step's packed masked board
    int snap_len;
} EvwRead;

typedef void (*EvwReadSink)(void *ctx, int index, const EvwRead *ev);

// The header alone, without walking the events - what a caller needs BEFORE the
// first event, when a reader must announce `viewer` and `actor` up front).
// Returns 0, or EVW_EBADARG / EVW_EPARSE.
int evwire_read_header(const unsigned char *buf, int len, EvwHeader *out);

// Walk one packed sequence, handing every event to `sink` in wire order, and
// report the trailer (the committed final board) through `out_final`. `out_hdr`
// and `out_final` are optional. Nothing is allocated and nothing is copied.
//
// Returns the event count, or EVW_EBADARG / EVW_EPARSE. On an error the sink may
// already have seen the events that decoded cleanly before it - the caller is
// expected to discard the lot, which is what "never a partial parse" means.
int evwire_read(const unsigned char *buf, int len,
                EvwHeader *out_hdr,
                const unsigned char **out_final, int *out_final_len,
                EvwReadSink sink, void *ctx);

// ---------- the settlement cut ----------------------------------------------

// Where a TURN settles, over the length-prefixed FRAME stream a turn arrives as
// (u16 LE length then one packed sequence, repeated - replay_steps_frames_v6).
//
// A turn is several frames because it is several actions: an iMessage bubble
// carries everything its sender staged. The clients flatten those frames into
// one event list, so the cut is an index into the FLATTENED list, counted across
// frames in order.
//
// Returns that index, -1 when the turn ended no bout, or EVW_EPARSE for a stream
// that is not whole. See evw_is_settlement for what is being cut and why.
int evwire_frames_settlement_cut(const unsigned char *frames, int len);

// Serialize the full sequence for `viewer` (seat index, or VIEW_SPECTATOR).
// `logs`/`n_logs` are THIS action's kernel logs (the resident game's fresh
// log buffer). `final_g` is the post-action (post-finalize) state for the
// trailer. `append_final_transition` adds the game-over MAGIC_TRANSITION
// event executeWithGameLock used to push after finalizeEndedGame.
// Returns bytes written, or -1 if `cap` would be exceeded (nothing usable
// is left in `out`).
int evwire_serialize(const EvSnap *snaps, int n_snaps,
                     const GameLog *logs, int n_logs,
                     const Game *final_g, int viewer, int actor,
                     int append_final_transition,
                     unsigned char *out, int cap);

#endif
