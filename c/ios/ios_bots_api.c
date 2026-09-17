// ios_bots_api.c — the bridge's bot half, and the ONLY object in it that names
// a bot symbol.
//
// Split out of ios_api.c so the strategy ladder stops riding into binaries that
// never asked for it. Foolish.xcframework is a static archive, so ld links it
// per OBJECT: one translation unit that references bot_drive pulls in the whole
// ladder behind it (21 strategy brains, cordite's rollout simulator, the roster
// table and its knobs - about three quarters of the native kernel by size).
// While ios_api.c held both halves, the iMessage extension - which plays people
// and never drives a seat - carried every byte of that.
//
// Everything here is offline single-player: the local game's bot opponents and
// the strategy picker behind them. An iMessage game reaches none of it.
//
// The resident game is ios_api.c's (fio_resident_game, ios_internal.h). This
// file owns no state.

#include "ios_api.h"
#include "ios_internal.h"

#include "game.h"
#include "legal.h"
#include "strategy.h"
#include "bot_roster.h"
#include "bot_drive.h"

#include <string.h>

// ---------- strategy roster (offline bots, §7.2) --------------------------
//
// The roster itself lives in the kernel (src/bot_roster.c) — one table shared
// with the server and every future client, so a bot named "cordite" here is
// the same brain at the same tuning as the website's cordite. This file used
// to carry its own copy, which mapped the player-facing rungs at the arena
// variants of handwritten/espresso and applied no tuning knobs at all; both
// bugs are gone with the table (docs/C_CORE_CONSOLIDATION.md §3.1, §4.1).
//
// A FIO strategy id is an index into the OFFLINE projection of the roster (the
// picker's rungs in strength order, docs/IOS_BOT_NAMING.md §1) — not a raw
// roster index and not a STRAT_* id, so the ids stay stable as unshipped
// research brains come and go from the table.
static int fio_roster_idx(int strategy_id) {
    return bot_roster_offline_at(strategy_id);
}


int fio_set_seat_strategy(int seat, int strategy_id) {
    Game *g = fio_resident_game();
    if (!g) return FIO_ENOGAME;
    if (seat < 0 || seat >= g->num_players) return FIO_EBADARG;
    const BotRosterEntry *e = bot_roster_at(fio_roster_idx(strategy_id));
    if (!e) return FIO_ENOSTRAT;
    // strategy_key IS the assignment: it holds a STRAT_* id by kernel-wide
    // convention and the kernel reads it (espresso_prod_strategy.c checks
    // strategy_key == STRAT_RANDOM to mirror the TS bot's random-opponent
    // case). The per-seat roster-index mirror this used to keep alongside it
    // was written in three places and read in none.
    g->players[seat].strategy_key = (int8_t)e->strat;
    return FIO_EOK;
}

// PACKED bot-drive — one kernel cycle, packed:
//   u32 n_actions, per action {seat, pace, type, n_cards, cards[], attacks[]},
//   then i32 stop, i32 ended, i32 delayMs (LE).
// Events are NOT carried (the app doesn't consume them until B4 animation; they
// come back as packed evwire then).
static void le_i32(unsigned char **q, int v) {
    unsigned int u = (unsigned int)v;
    *(*q)++ = u & 0xff; *(*q)++ = (u >> 8) & 0xff; *(*q)++ = (u >> 16) & 0xff; *(*q)++ = (u >> 24) & 0xff;
}
// THE CYCLE'S OUTPUT, AS ITSELF (bot_drive.h BotDriveOut), where it lies: the
// actions applied with their pacing and their moves, why the drive stopped and
// whether the game ended. It used to be flattened into a packed block here and
// unpacked in BotDriveWire.swift - a u32 count, a four-byte action head, two
// card runs and three little-endian ints - which was the same layout written
// down in two languages.
//
// The cycle's DELAY is not in the struct (it is a question about the drive, not
// a field of it), so it is the return value: milliseconds to wait before the
// next cycle, or a negative FIO_E*.
static BotDriveOut g_drive;

const void *fio_bot_drive_ptr(void) { return &g_drive; }

int fio_bot_drive(int human_mask) {
    Game *g = fio_resident_game();
    if (!g) return FIO_ENOGAME;
    bot_drive(g, (uint32_t)human_mask, BOT_DRIVE_MAX_ACTIONS, 0, 0, &g_drive);
    return bot_cycle_delay_ms(g, (uint32_t)human_mask, &g_drive);
}

// ---------- strategies -----------------------------------------------------

int fio_strategy_count(void) { return bot_roster_offline_count(); }

int fio_strategy_name(int id, char *out, int cap) {
    const BotRosterEntry *e = bot_roster_at(fio_roster_idx(id));
    if (!e) return FIO_ENOSTRAT;
    if (!out || cap <= 0) return FIO_EBADARG;
    // A bare NUL-terminated key: the picker shows it, nothing parses it.
    const int n = (int)strlen(e->key);
    if (n + 1 > cap) return FIO_ECAP;
    memcpy(out, e->key, (size_t)n + 1);
    return n;
}
