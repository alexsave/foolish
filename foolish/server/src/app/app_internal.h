// app_internal.h - what table_ops.c shares with bots.c and functions_http.c,
// and the seams the app's unit tests drive. Nothing outside src/app/ and its
// tests includes this.
//
// THE SECTION. One kernel section's scratch: the Game, the hook snapshots, the
// Table, the commit products and their arena, about 300 KB, heap-allocated per
// operation so any thread may run one (DESIGN.md "Kernel thread safety"). A
// section is loaded from a stored row, an operation runs on section->t, and
// cb_section_commit turns what it did into one CAS commit plus everything a
// winning commit owes: the end of the game, the fan-out.
#ifndef CB_APP_INTERNAL_H
#define CB_APP_INTERNAL_H

#include "store/rows_foolish.h"
#include "table_ops.h"

#include <stdbool.h>
#include <stdint.h>

// ---- seams ------------------------------------------------------------------------

// The host clock every kernel call is handed, and the sleep a bot drive paces
// with. cb_now_ms and cb_sleep_ms; a test points both at a fake clock so the
// kernel's pacing costs no wall time.
extern int64_t (*cb_app_now)(void);
extern void (*cb_app_sleep)(int64_t ms);
// A new game's id: cb_game_id_random (store/game_id.h, THE GAME ID RULE).
// A test points it at a fixed sequence to make an id collide.
extern void (*cb_app_game_id)(char out[CB_GAME_ID_MAX + 1]);

// ---- kept buffers ---------------------------------------------------------------

// Byte buffers a row's fields are copied into, kept and grown, never shrunk: a row copied
// again into the same buffers allocates nothing.
typedef struct {
    uint8_t *state, *roster, *logs;
    size_t   state_cap, roster_cap, logs_cap;
} cb_row_buf;
void cb_row_buf_free(cb_row_buf *b);

void cb_section_drain(void);        // frees the thread's kept section (a leak-counting test)
void cb_rows_batch_drain(void);     // frees the thread's kept commit batch (a leak-counting test)
void cb_table_lock_drain(void);     // frees the game locks no thread holds (a leak-counting test)

// ---- the section ------------------------------------------------------------------

#define CB_ARENA_BYTES (128 * 1024)

typedef struct cb_section {
    Game        g;
    TableSnaps  snaps;
    Table       t;                // zeroed by table_init, not by cb_section_free
    TableCommit c;                // zeroed by table_commit_products, not by cb_section_free
    char        gid[CB_GAME_ID_MAX + 1];
    // The humans and bots the loaded roster seated before the operation (the
    // membership diff, and the commit's prev seats).
    int         n_old;
    cb_uuid     old_humans[CB_MAX_SEATS];
    int         n_old_bots;
    cb_uuid     old_bots[CB_MAX_SEATS];
    // The moves the operation applied, as the store's journal holds them instead of writing
    // the tables (journal.h): the kernel's own list (table.h Table.moves, the delta cast's), which
    // must hold `moves_want` of them - 1 for a human's action (cb_table_run), the drive's count
    // for a bot cycle (bots.c), 0 for any other operation. Cleared with the section.
    int         moves_want;
    bool        moves_bots;
    // The game as the journal holds it for this operation (journal.h THE HOLD: cb_table_run's
    // acquire, released before its unlock), or NULL. Cleared with the section.
    struct cb_jhold *held;
    uint8_t     arena[CB_ARENA_BYTES];
    int         arena_hw;         // bytes of the arena a products call may have written (cb_section_free zeroes that prefix)
    bool        in_use, pooled;   // cb_section_new's own: the worker thread's reusable one, and whether it is handed out
    // The game end's replay scratch (finalize_game: the code, the decode gate's scratch and the
    // extras, 3.25 MB), made on first use and kept with the section: a job thread
    // that finalizes allocates it once, not per finished game.
    uint8_t    *fin;
    cb_row_buf  rb;               // the loaded row's bytes, the journal's copy (row_acquire); kept with the section, not zeroed
    // The buffer a commit decodes the new game into (table_resident) for the journal, which takes
    // it and hands its old one back (rows_foolish.h cb_game_commit.resident): made on first use,
    // kept with the section, not zeroed.
    TableResident *res;
} cb_section;

cb_section *cb_section_new(void);
void cb_section_free(cb_section *s);
bool cb_section_pooled_is_clean(void);   // test hook: the idle pooled section is all zero (Game.logs: all 0xAA when poisoned)
bool cb_section_logs_poisoned(void);     // Game.logs is filled with 0xAA, not left stale (ASan builds, CBASE_POISON_LOGS)

// table_init + table_load + table_set_deal_seed (the row's seed as lowercase
// hex, "" for none), and the old humans noted. The kernel's result (< 0 a
// stored row the kernel refuses).
int cb_section_load(cb_section *s, const cb_game_row *row);
long cb_section_loads(void);   // how many rows cb_section_load decoded (a test's count: an adopted game decodes none)

// The game read WITHOUT its lock (a bot drive's read: its commit is fenced) into the section:
// under serve the overlay's resident adopted (or its row's bytes loaded), else the store's row
// loaded. *kept: the row's bytes are the section's (s->rb, or none when adopted), so it is
// released with cb_row_release, never freed. CB_OK, CB_E_NOT_FOUND, CB_E_STATE for a row the
// kernel refuses (logged), or the store's error.
int  cb_section_read(cb_ctx *ctx, const char *id, cb_section *s, cb_game_row *row, bool *kept);
void cb_row_release(cb_game_row *row, bool kept);

// One viewer's envelope of the loaded table at `version` (viewer -1: the
// spectator), malloc'd. CB_OK or CB_E_STATE.
int cb_section_envelope(cb_section *s, int viewer, uint32_t version, uint8_t **out, size_t *out_len);

// The journal's replay of one move at start (journal.h cb_journal_apply_fn; ud is the cb_ctx).
struct cb_jmove;
int cb_table_replay_move(void *ud, const cb_game_row *row, const struct cb_jmove *m, cb_game_row *out, uint8_t **log,
                         size_t *log_len);

// What a commit attempt came to.
typedef struct {
    bool     lost;          // the CAS lost: nothing was written; reload and run again
    int64_t  version;       // the committed version (row->version + 1)
    bool     needs_bots;
    bool     ended;
    uint8_t *actor_env;     // the actor's envelope (seat, else spectator); NULL when no actor_id
    size_t   actor_env_len;
} cb_commit_out;

// Commits the operation that just ran on s->t against `row` (the version it
// was loaded at): the products (rated when the kernel wants ratings, from
// `ratings` when *have_ratings, else read from the store into it), the new row
// (logs appended or reset, round_epoch, the deal seed when `dealt_seed` and the
// operation dealt), the CAS. On a win: the snapshot when the game ended, the
// fan-out. CB_OK (look at out->lost), or CB_E_STATE / the store's
// error.
int cb_section_commit(cb_ctx *ctx, cb_section *s, const cb_game_row *row, const uint8_t *dealt_seed,
                      const char *actor_id, int32_t ratings[CB_MAX_SEATS], bool *have_ratings,
                      cb_commit_out *out);

#endif
