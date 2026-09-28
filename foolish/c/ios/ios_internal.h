// ios_internal.h — what the bridge's translation units share with each other.
// NOT part of the Swift-visible contract; that is ios/include/ios_api.h.
//
// The bridge is split TWICE, for two different reasons.
//
// THE BOT SPLIT, which is about what a binary LINKS. `Foolish.xcframework` is a
// static archive and ld pulls it in per OBJECT, so one translation unit that
// names bot_drive drags the whole strategy ladder (21 brains, cordite's
// simulator, the roster and its knob table - three quarters of the native
// kernel by size) into any binary that uses ANY of the bridge. The iMessage
// extension plays people, never bots, and used to carry all of it because
// ios_api.c held both halves. ios_bots_api.c is now the only object that names
// a bot symbol, and ios/split_check.sh asserts it on every core object.
//
// THE DOMAIN SPLIT, which is about what a READER has to hold in their head. The
// core half is 123 entry points at about nine lines each - wide, not deep - and
// they sort cleanly by what they are ABOUT: the resident game (ios_api.c), the
// client's board slot (ios_api_view.c), the move menu and the board's gesture
// rules (ios_api_play.c), the animation surface (ios_api_anim.c), replays and
// evwire (ios_api_replay.c), the roster and its seat/name gates
// (ios_api_identity.c) and the iMessage envelope (ios_api_msg.c). No entry
// changed on the way; ios/include/ios_api.h is untouched by the split.
//
// Both splits need the files to share the resident game, which is what this
// header is for. Everything here is an ACCESSOR rather than an `extern` on the
// state itself, so the state stays owned by one file, stays static, and cannot
// be reseated behind that file's back - and so "is there a game" has one answer
// rather than two symbols that could disagree.
#ifndef CNITRO_IOS_INTERNAL_H
#define CNITRO_IOS_INTERNAL_H

#include <stdint.h>

#include "game.h"
#include "client_table.h"
#include "msg_wire.h"

// ---------- the resident game and the terms it was dealt under --------------
//
// One struct rather than a drawer of loose statics, because these fields are
// ONE fact: this device's current game, and the deal it came out of. They are
// established together (a fresh deal in fio_new_game, or a chain adopted in
// fio_msg_decode), they are cleared together, and a seal repeats every one of
// them - so a file that can set one of them has to be able to see the rest, or
// it will leave the session half-changed.
//
// ios_api.c owns it. The other TUs reach it through fio_session() and are
// expected to keep the discipline each field's comment there states.
typedef struct {
    Game     game;            // the one resident Game; garbage while !has_game
    int      has_game;        // 0 until fio_new_game or fio_msg_decode fills it

    // The deal seed this game was dealt from. Zero when the game came from a
    // short/absent seed, i.e. the legacy LCG deal - which cannot be re-derived,
    // so those games can never encode as v6.
    uint8_t  deal_seed[FOOLISH_SEED_LEN];
    int      has_deal_seed;

    // THE LOG MARK: the log count at the moment this game was established from
    // a chain (a decode) or from a fresh deal. -1 = unknown. See ios_api.c.
    int      msg_base_logs;
    uint16_t msg_base_sent_at;   // the adopted chain's send clock
    uint8_t  msg_opening;        // the fool's penalty, or MSG_NO_OPENING
    uint32_t msg_carry_key;      // the rematch carry a WAITING envelope holds
    uint8_t  msg_carry_fool;     // …and its fool, or MSG_NO_FOOL
    int8_t   msg_rules;          // the variant this table is played under
} FioSession;

// The session, always. Its `game` is the storage a decode replays INTO, so this
// hands back the slot whether or not a game is resident; ask has_game first.
FioSession *fio_session(void);

// The one resident game, or NULL when fio_new_game has not run. Callers in
// another TU return FIO_ENOGAME on NULL, exactly as ios_api.c's own entries do.
Game *fio_resident_game(void);

// THE ONE SCRATCH GAME, borrowed for the length of a single call. Fill it
// before you read it and do not hold it across a return; see ios_api.c for why
// that is enough.
Game *fio_scratch_game(void);

// ---------- the client's board slot (ios_api_view.c) ------------------------
//
// The slot the last adopt filled. ios_api_play.c asks for it so a host that has
// just adopted an envelope can ask for its menu in the same breath, without
// handing the bytes back down a second time.
ClientTable *fio_client(void);

// ---------- the packed roster (ios_api_identity.c) --------------------------
//
// A ROSTER, PACKED - byte for byte the tail fio_msg_decode's joins are read
// from:
//   n_joins(1), then n_joins x { seat(1), name_len(1), name[name_len] }
// One layout for the roster in both directions, so a host that can read one can
// write one. ios_api_msg.c seals through this same reader, which is what keeps
// a seal from writing a roster the gates would read differently.
//
// Every record is BOUNDED BEFORE IT IS READ, and the blob must be consumed
// EXACTLY: trailing bytes mean the caller and the kernel disagree about what a
// roster is, and a roster read short is a different table. Returns FIO_EOK or
// FIO_EPARSE.
int fio_read_joins(const uint8_t *b, int len, MsgEnvelope *e);

#endif
