// local_table.h - a game played entirely in the browser: one human against one
// bot, with no server, no account and no row anywhere.
//
// THE HOST OF ONE TABLE. It is what the server (c_base's foolish app) is to a
// game, cut down to the one table a promo visitor plays: it keeps the game as a
// resident between operations (table.h TableResident, as the server's journal
// does), keeps its session's move list, numbers its versions, and after every operation
// hands out what a server would push to the human's seat - the seat's envelope
// and its cast. So the page reads a local game through exactly the code it reads
// a served one with (client_table.h): the envelope and the cast are the same
// bytes, made by the same Table.
//
// What crosses to the page is MASKED, always: the human seat's envelope, that
// seat's cast, and the action response. Nothing here hands out the state blob or
// the bot's hand. (The whole game is in the module's memory, as it has to be for
// a bot to play it in the tab; that is fine for an unrated game against a bot,
// and is why a local game never reaches a server or a rating.)
//
// THE BOT IS ONE CONSTANT. LOCAL_BOT_STRAT names the brain (strategy.h) and
// LOCAL_BOT_CHOOSE its function; the drive is the kernel's own
// (table_bot_drive_with), so only the choice differs from a served game. The
// chooser is named directly rather than through the roster's dispatch because a
// function pointer is a link-time reference: the roster's reaches every named
// brain, and the browser's module would carry all of them. Changing the bot is
// these two lines (and the brain's size in web.wasm); local_table_test.c holds
// them to the roster's own row.
//
// One operation is one synchronous section, as the server's are: adopt the
// resident, operate, commit, keep the resident. Between operations the Game the
// table runs on may be used by anything else (in wasm it is the module's one
// resident Game).
#ifndef CNITRO_LOCAL_TABLE_H
#define CNITRO_LOCAL_TABLE_H

#include "table.h"
#include "strategy.h"
#include <stdint.h>

#define LOCAL_BOT_STRAT  STRAT_RANDOM
#define LOCAL_BOT_CHOOSE random_strategy_choose

// The seats' ids. Nothing outside this table ever sees them: the page names its
// own seat by the envelope's viewer, not by an id.
#define LOCAL_HUMAN_ID "local-you"
#define LOCAL_BOT_ID   "local-bot"

// The session's move list a game can keep (move_list.h), as the server keeps it:
// a whole two-seat game is a few hundred bytes; a list that outgrows the cap is
// broken (the game keeps going and has no replay).
#define LOCAL_LIST_CAP  (16 * 1024)
// The products of one operation (TABLE_PRODUCTS_NO_ROW and no spectator: the
// human's envelope, the logs, the clock), and the human's cast.
#define LOCAL_ARENA_CAP (24 * 1024)
#define LOCAL_CAST_CAP  (48 * 1024)
// The finished game's replay (table_replay_code_list, table_replay_extras_list): a two-seat
// game's moves code is tens of bytes and its extras a few hundred.
#define LOCAL_REPLAY_CAP (8 * 1024)

typedef struct {
    Table         t;
    TableResident res;
    TableCommit   c;
    char          gid[ROSTER_GAME_ID_MAX];
    int           gid_len;
    uint8_t       seed[FOOLISH_SEED_LEN];
    uint32_t      version;
    int64_t       round_epoch;   // the version the current round began at (the server's column)
    bool          open;
    int64_t       opened_ms;     // the table's creation: the base the move list is timed from (the server's created_at)
    int           list_len;
    int           cast_len;      // the last operation's cast, 0 for none
    uint8_t       list[LOCAL_LIST_CAP];
    uint8_t       arena[LOCAL_ARENA_CAP];
    uint8_t       cast[LOCAL_CAST_CAP];
    int           replay_len, extras_len;   // 0 until the finished game's replay is made
    uint8_t       replay[LOCAL_REPLAY_CAP];
    uint8_t       extras[LOCAL_REPLAY_CAP];
} LocalTable;

// A new table `gid` on `g` and `snaps` (host storage, as table_init's): the
// human `you` (its display name) in seat 0 and the bot beside it, READY, in a
// lobby. `seed` is FOOLISH_SEED_LEN bytes the host drew from crypto, the deal's.
// Nothing is dealt yet (local_table_start). TABLE_OK, or the table's refusal.
int local_table_open(LocalTable *L, Game *g, TableSnaps *snaps, const char *gid, int gid_len,
                     const uint8_t *seed, const char *you, int you_len, int64_t now_ms);

// The human's envelope of the table as it stands: its length in `out`, or a refusal.
int local_table_envelope(LocalTable *L, uint8_t *out, int cap);

// The human readies, and the table deals (the server's ASK_START). TABLE_MOOT
// once dealt. The deal's cast is the table's (local_table_cast).
int local_table_start(LocalTable *L, int64_t now_ms);

// A move, as the action endpoint receives it (table.h table_request_decode) and
// answers it (table_action_response): the response body's length in `out`, or a
// refusal of the request as a whole. An applied move's cast is the table's.
// `out` may be `req`: the request is read whole before the response is written.
int local_table_action(LocalTable *L, const uint8_t *req, int req_len, int64_t now_ms, uint8_t *out, int cap);

// An ask (ask.h), as the meta endpoint receives it, run by table_ask as the human.
// The table's result: TABLE_OK (committed; its cast is the table's), TABLE_MOOT
// (nothing to commit: a bump), or a refusal. A local table has no bot rows to
// seat and no second human, so an ask that would need one is refused.
int local_table_ask(LocalTable *L, const uint8_t *ask, int ask_len, int64_t now_ms);

// How long until the bot should act, on the board's clock (table_bot_wait_ms):
// 0 now, -1 when it has nothing to do (the human's move is owed, or the game is over).
int local_table_bot_wait_ms(LocalTable *L, int64_t now_ms);

// One bot cycle (table_bot_drive_with, the kernel's drive with LOCAL_BOT_CHOOSE):
// the actions it applied (0: nothing to do, nothing committed), or a refusal.
// A cycle that applied any is committed, and its cast is the table's.
int local_table_bot(LocalTable *L, int64_t now_ms);

// THE FINISHED GAME'S REPLAY, made exactly as the server's finalize_game makes
// it (server/src/app/table_ops.c): table_replay_extras_list over the roster and the
// move list, then table_replay_code_list from the deal seed and the same list, with
// its round-trip gate. Made once, on the first ask after the game ends, and kept;
// `scratch` is the decode gate's workspace. TABLE_E_NOT_OVER before the end, or
// the kernel's refusal. The moves code and the extras are then
// local_table_replay_moves and local_table_replay_extras.
int local_table_replay(LocalTable *L, uint8_t *scratch, int scratch_cap);
// The made replay's moves code (the v6 code, raw bytes) / extras blob, copied into `out`: the length.
int local_table_replay_moves(const LocalTable *L, uint8_t *out, int cap);
int local_table_replay_extras(const LocalTable *L, uint8_t *out, int cap);

// The cast the last committed operation left for the human (evwire.h), once:
// copied into `out`, its length; 0 when there is none or it was taken.
int local_table_cast(LocalTable *L, uint8_t *out, int cap);

#endif
