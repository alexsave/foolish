// Event wire ("evwire" v1) — one recipient's animation sequence as packed
// bytes, replacing the JSON AnimationEvent stream. This is the C port of the
// TS buildEvents (supabase/functions/_shared/wasm/engine.ts) fused with the
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
//     u8 msg_code   EVW_MSG_* — which message template the JSON path attached
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

// Event types — mirrors ANIMATION_EVENT_TYPE in _shared/types.ts.
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
// lets a second destination consume it without re-deriving anything: the iOS
// bridge emits these as JSON, which is why `BoardDiff.swift` is cancelled
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

// Derive the event sequence for `viewer` from the hook snapshots + this
// action's logs, handing each event to `sink`. THE one derivation; both
// evwire_serialize (packed, for the web) and the iOS JSON emitter drive it.
void evwire_walk(const EvSnap *snaps, int n_snaps,
                 const GameLog *logs, int n_logs, int viewer,
                 EvwSink sink, void *ctx);

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
