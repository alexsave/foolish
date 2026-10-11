// What each of the kernel's refusal codes says, in English, keyed by the code's
// own name. Diagnostics only: a host logs these or throws them, nothing renders
// one on a board, so they are not translated and not in c/i18n.
//
// THIS FILE IS SOURCE FOR A GENERATOR AND IS LINKED BY NOTHING. tools/datagen
// reads the tables below at build time and writes sdk/ts/gen/errtext/*.ts
// (tools/structgen/gen.sh); c/errtext is outside c/src and in no *_SRC list in
// c/Makefile, for the reason c/i18n is: a kernel that returns a number has no
// use for the sentence, and bots.wasm.gz is downloaded by every visitor.
//
// WHY IT IS C AND NOT A TABLE IN THE HOST. Each of these was a `switch` or a
// `Record<number, string>` typed out in TypeScript beside a comment naming the
// header it mirrored, with the numbers copied in. A code added in C then had no
// message until somebody noticed (ENGINE_REJECT_PASS_DISABLED went a release
// with none), and nothing could say so. Here a row is written against the
// constant itself, so a renamed or removed code does not compile, and
// e2e/validation/error_text_validation.test.ts refuses a code that has no row.
//
// A row is { code, text }. The code is written exactly as the kernel returns
// it - the families that return a negated code negate it here - so a host looks
// a result up without knowing which family negates.
#ifndef FOOLISH_ERROR_TEXT_H
#define FOOLISH_ERROR_TEXT_H

#include "client_table.h"
#include "game.h"
#include "msg_wire.h"
#include "replay.h"
#include "replay_extras.h"
#include "table.h"

typedef struct { int code; const char *text; } ErrorText;

// ---- a move the action path refused ----------------------------------------------
// Everything a host can be handed back for a move it sent: the rules kernel's
// ENGINE_REJECT_* (game.h), and the three answers that are not one of those.
// A rule's text starts with its own name, pasted from the constant so the two
// cannot be spelled differently.
#define RULE(name, prose) { ENGINE_REJECT_##name, #name ": " prose }
static const ErrorText MOVE_REJECT_TEXT[] = {
    RULE(NOT_PLAYING,         "game is not in progress"),
    RULE(EMPTY,               "no cards in the move"),
    RULE(IS_DEFENDER,         "the defender cannot attack"),
    RULE(NOT_DEFENDER,        "only the defender can do this"),
    RULE(NOT_IN_HAND,         "card not in hand"),
    RULE(DUPLICATES,          "duplicate cards in the move"),
    RULE(NOT_SAME_VALUE,      "cards must share one value"),
    RULE(NOT_FIRST_ATTACKER,  "waiting for the first attacker"),
    RULE(VALUE_NOT_ON_TABLE,  "attack value not on the table"),
    RULE(DEFENDER_CAPACITY,   "defender cannot hold that many attacks"),
    RULE(NO_UNCOVERED,        "nothing uncovered to act on"),
    RULE(ATTACK_NOT_ON_TABLE, "target attack is not on the table"),
    RULE(CANNOT_COVER,        "card cannot beat that attack"),
    RULE(NO_TABLE_CARDS,      "the table is empty"),
    RULE(COVER_PRESENT,       "cannot pass over a covered battle"),
    RULE(PASS_VALUES,         "pass cards must match the table value"),
    RULE(PASS_CAPACITY,       "next defender cannot hold the passed attacks"),
    RULE(NOT_IN_STATUS,       "player is not in play"),
    RULE(ALREADY_GOOD,        "already said good"),
    RULE(FIRST_MUST_ATTACK,   "the first attacker must open"),
    RULE(PASS_OVERFLOW,       "too many cards to pass"),
    RULE(PASS_DISABLED,       "this table plays without passing"),
    // Kernel-legal, refused by an edge policy: a round closed before it landed.
    // The number sits above the ENGINE_REJECT_* space so a client can tell the
    // two apart by the code alone (table.h).
    { TABLE_REJECT_STALE_ROUND, "STALE_ROUND: a round closed before this move landed" },
    // A wire that is not a move at all, as the client slot (client_table.h) and
    // as the rules bridge (wasm_apply_action, which has no named code) say it.
    { CLIENT_E_MOVE,            "MALFORMED: unreadable action wire" },
    { -1,                       "MALFORMED: unreadable action wire" },
};
#undef RULE

// ---- an FMSG envelope that did not decode or seal (msg_wire.h MSG_E*) --------------
static const ErrorText MSG_ERROR_TEXT[] = {
    { MSG_ESHORT,   "iMessage payload: truncated" },
    { MSG_EMAGIC,   "iMessage payload: not an FMSG envelope" },
    { MSG_EFORMAT,  "iMessage payload: unsupported format" },
    { MSG_EFLAGS,   "iMessage payload: unsupported flags" },
    { MSG_EPHASE,   "iMessage payload: bad phase" },
    { MSG_EPLAYERS, "iMessage payload: bad player count" },
    { MSG_EVARIANT, "iMessage payload: unknown variant" },
    { MSG_ESEAT,    "iMessage payload: bad seat" },
    { MSG_ENAME,    "iMessage payload: bad nickname" },
    { MSG_ESEED,    "iMessage payload: dead deal seed" },
    { MSG_EACTION,  "iMessage payload: malformed action" },
    { MSG_ETURN,    "iMessage payload: turn does not match the chain" },
    { MSG_EROUND,   "iMessage payload: round does not match the chain" },
    { MSG_ECAP,     "iMessage payload: capacity exceeded" },
    { MSG_ETRAIL,   "iMessage payload: trailing bytes" },
    { MSG_ECHAIN,   "iMessage payload: illegal chain" },
    { MSG_EJOINS,   "iMessage payload: bad joins" },
    { MSG_EBODY,    "iMessage payload: body is not a replay code for this game" },
};

// ---- a replay's extras blob (replay_extras.h REPLAY_EXTRAS_E*, returned negated) ---
static const ErrorText REPLAY_EXTRAS_ERROR_TEXT[] = {
    { -REPLAY_EXTRAS_EHEADER,  "extras: truncated header" },
    { -REPLAY_EXTRAS_EVERSION, "extras: unsupported version" },
    { -REPLAY_EXTRAS_ENAME,    "extras: unterminated name" },
    { -REPLAY_EXTRAS_ETIMES,   "extras: truncated time header" },
    { -REPLAY_EXTRAS_EGAPS,    "extras: too few gaps for the moves they describe" },
    { -REPLAY_EXTRAS_EINPUT,   "extras: malformed argument blob" },
    { -REPLAY_EXTRAS_ECAP,     "extras: exceeds the kernel IO buffer" },
};

// ---- a replay code (replay.h REPLAY_E*, returned negated) --------------------------
// A {name} is one of the refusal's own parameters (replay.h ReplayError), which
// the host fills in: {version} and {menu} are its numbers, and {log_type} is
// the session log record's name (game.h LOG_*), in lower case.
static const ErrorText REPLAY_ERROR_TEXT[] = {
    { -REPLAY_EVERSION,      "unsupported replay format version {version}" },
    { -REPLAY_ELEFTOVER,     "invalid replay: leftover data after game end" },
    { -REPLAY_EATOMS,        "replay guard: too many events" },
    { -REPLAY_ENOMOVES,      "replay desync: no legal moves" },
    { -REPLAY_ECONSERVATION, "replay desync: conservation" },
    { -REPLAY_EKNOWN,        "replay desync: played card not in hand" },
    { -REPLAY_ENOFRESH,      "replay desync: the unseen pool is empty" },
    { -REPLAY_ENOTFEAS,      "replay desync: a supplied reveal is not unseen" },
    { -REPLAY_ENOTINMENU,    "replay desync: logged {log_type} not in menu of {menu}" },
    { -REPLAY_EROUNDEND,     "replay desync: round end not in menu" },
    { -REPLAY_EATTCONT,      "replay desync: attack continuation" },
    { -REPLAY_EPASSCONT,     "replay desync: pass continuation" },
    { -REPLAY_EINCOMPLETE,   "incomplete replay: the actions ran out before the coded atom count" },
    { -REPLAY_ELOGSAFTER,    "replay desync: logs continue after the game ended" },
    { -REPLAY_EEMPTYMENU,    "empty menu" },
    { -REPLAY_ECHOSEN,       "encode: chosen index out of range" },
    // Every "the header does not fit the game" fault, including a rebuilt deal
    // that disagrees with the header's opening seat.
    { -REPLAY_EHEADER,       "invalid replay header (trump not in alphabet, or the rebuilt deal contradicts it)" },
    { -REPLAY_EINPUT,        "replay: malformed encode input" },
    { -REPLAY_ECAP,          "replay: capacity exceeded" },
    // A refusal, not a fault - see REPLAY_ETOOLONG in replay.h.
    { -REPLAY_ETOOLONG,      "replay: game too long to encode (session log overflowed)" },
};

#endif
