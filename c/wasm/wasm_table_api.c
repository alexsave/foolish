// wasm_table_api.c - the C Table (src/table.h) as bots.wasm exports it.
//
// Linked into bots.wasm only. The table's board is the module's one resident
// Game (wasm_api.c g_game), so every table call is part of the host's single
// synchronous kernel section: load, operate, copy the products out.
//
// How things cross:
//   - Byte inputs (a state blob, a durable roster, an actor id, an action wire,
//     a game id) are written into the IO buffer back to back, with their lengths
//     as call arguments. They are opaque to the host: it moves them, it never
//     reads inside them.
//   - Structured outputs (the commit's spans and scalars, the loaded roster, a
//     decoded request, the finish order and rating changes) are C structs the
//     host reads through sdk/ts/gen/game_layout.bots.ts, generated from these
//     very headers (tools/structgen/specs/game_layout.args) and checked against
//     wasm_layout_hash at load.
//   - Byte outputs (the commit arena, a push, an envelope, a response) are
//     written into the IO buffer; a span or the return value says where.
//   - Versions, epochs and clocks cross as doubles: a JS number, no BigInt, and
//     exact for every integer a version or a millisecond clock reaches.
#include "../src/table.h"
#include "../src/client_table.h"
#include <string.h>

extern Game *wasm_game_ptr_internal(void);
extern unsigned char *wasm_io_ptr(void);
extern int wasm_io_cap(void);
extern uint32_t wasm_rng_base_internal(void);
extern void wasm_set_rng_base(uint32_t base);
extern void wasm_clearenv(void);
extern void wasm_reload_bot_flags(void);
extern void wasm_belief_probe_observe_internal(const Game *g, int seat);
extern unsigned char *wasm_replay_io_ptr(void);
extern int wasm_replay_io_cap(void);

static TableSnaps   g_table_snaps;
static Table        g_table;
static TableCommit  g_table_commit;
static TableRequest g_table_request;
static TableElo     g_table_elo;
static BotDriveOut  g_table_drive;
static int          g_table_ready;

static Table *table(void) {
    if (!g_table_ready) {
        table_init(&g_table, wasm_game_ptr_internal(), &g_table_snaps);
        table_choose_observer = wasm_belief_probe_observe_internal;
        g_table_ready = 1;
    }
    return &g_table;
}

// A game id out of the IO buffer, before the call overwrites the buffer with
// its output. 0, or TABLE_E_WIRE for an id over the trailer's cap.
static int take_gid(int gid_len, char *gid) {
    if (gid_len < 0 || gid_len > ROSTER_GAME_ID_MAX) return TABLE_E_WIRE;
    memcpy(gid, wasm_io_ptr(), (size_t)gid_len);
    return 0;
}

static uint32_t u32_of(double v) { return v < 0 ? 0u : (uint32_t)(uint64_t)v; }

Roster       *wasm_table_roster_ptr(void)  { return &table()->r; }
TableCommit  *wasm_table_commit_ptr(void)  { return &g_table_commit; }
TableRequest *wasm_table_request_ptr(void) { return &g_table_request; }
TableElo     *wasm_table_elo_ptr(void)     { return &g_table_elo; }
BotDriveOut  *wasm_table_drive_ptr(void)   { return &g_table_drive; }
int wasm_table_detail(void) { return table()->detail; }
int wasm_table_reject(void) { return table()->reject; }

// io = the request body.
int wasm_table_request_decode(int len) {
    if (len < 0 || len > wasm_io_cap()) return TABLE_E_WIRE;
    return table_request_decode(wasm_io_ptr(), len, &g_table_request);
}

// io = [state blob][durable roster].
int wasm_table_load(int state_len, int roster_len) {
    if (state_len < 0 || roster_len < 0 || state_len + roster_len > wasm_io_cap()) return TABLE_E_WIRE;
    Table *t = table();
    t->rng_base = wasm_rng_base_internal();
    const unsigned char *io = wasm_io_ptr();
    return table_load(t, io, state_len, io + state_len, roster_len);
}

// io = [actor id][action wire]. intent < 0: the request carried no intent.
int wasm_table_act(int id_len, int wire_len, double intent, double round_epoch) {
    if (id_len < 0 || wire_len < 0 || id_len + wire_len > wasm_io_cap()) return TABLE_E_WIRE;
    const unsigned char *io = wasm_io_ptr();
    return table_act(table(), (const char *)io, id_len, io + id_len, wire_len,
                     intent < 0 ? -1 : (int64_t)intent, (int64_t)round_epoch);
}

// ---- lobby edits: io = [actor id][the edit's own inputs, back to back] ----

// Inputs longer than the IO buffer are refused before anything reads them.
static const unsigned char *inputs(int total) {
    return total < 0 || total > wasm_io_cap() ? 0 : wasm_io_ptr();
}

// io = [actor id][name]
int wasm_table_create(int id_len, int name_len) {
    const unsigned char *io = inputs(id_len + name_len);
    if (!io || id_len < 0 || name_len < 0) return TABLE_E_WIRE;
    return table_create(table(), (const char *)io, id_len, (const char *)io + id_len, name_len);
}

// io = [actor id][name]
int wasm_table_join(int id_len, int name_len) {
    const unsigned char *io = inputs(id_len + name_len);
    if (!io || id_len < 0 || name_len < 0) return TABLE_E_WIRE;
    return table_join(table(), (const char *)io, id_len, (const char *)io + id_len, name_len);
}

// io = [actor id][target id]
int wasm_table_leave(int id_len, int target_len) {
    const unsigned char *io = inputs(id_len + target_len);
    if (!io || id_len < 0 || target_len < 0) return TABLE_E_WIRE;
    return table_leave(table(), (const char *)io, id_len, (const char *)io + id_len, target_len);
}

// io = [actor id][bot id][nickname][brain key][deal seed, FOOLISH_SEED_LEN]
int wasm_table_add_bot(int id_len, int bot_len, int nick_len, int brain_len) {
    const int seed_at = id_len + bot_len + nick_len + brain_len;
    const unsigned char *io = inputs(seed_at + FOOLISH_SEED_LEN);
    if (!io || id_len < 0 || bot_len < 0 || nick_len < 0 || brain_len < 0) return TABLE_E_WIRE;
    const char *c = (const char *)io;
    return table_add_bot(table(), c, id_len, c + id_len, bot_len, c + id_len + bot_len, nick_len,
                         c + id_len + bot_len + nick_len, brain_len, io + seed_at);
}

// io = [actor id][bot id]
int wasm_table_remove_bot(int id_len, int bot_len) {
    const unsigned char *io = inputs(id_len + bot_len);
    if (!io || id_len < 0 || bot_len < 0) return TABLE_E_WIRE;
    return table_remove_bot(table(), (const char *)io, id_len, (const char *)io + id_len, bot_len);
}

// io = [actor id][deal seed, FOOLISH_SEED_LEN]
int wasm_table_ready(int id_len) {
    const unsigned char *io = inputs(id_len + FOOLISH_SEED_LEN);
    if (!io || id_len < 0) return TABLE_E_WIRE;
    return table_ready(table(), (const char *)io, id_len, io + id_len);
}

// io = [actor id][n x { u8 len, id }]
int wasm_table_reseat(int id_len, int ids_len) {
    const unsigned char *io = inputs(id_len + ids_len);
    if (!io || id_len < 0 || ids_len < 0) return TABLE_E_WIRE;
    return table_reseat(table(), (const char *)io, id_len, io + id_len, ids_len);
}

// io = [actor id][title]
int wasm_table_retitle(int id_len, int title_len) {
    const unsigned char *io = inputs(id_len + title_len);
    if (!io || id_len < 0 || title_len < 0) return TABLE_E_WIRE;
    return table_retitle(table(), (const char *)io, id_len, (const char *)io + id_len, title_len);
}

// io = [actor id]
int wasm_table_continue(int id_len) {
    const unsigned char *io = inputs(id_len);
    if (!io || id_len < 0) return TABLE_E_WIRE;
    return table_continue(table(), (const char *)io, id_len);
}

// io = [actor id][n hand indices, one byte each]
int wasm_table_rearrange_hand(int id_len, int n) {
    const unsigned char *io = inputs(id_len + n);
    if (!io || id_len < 0 || n < 0) return TABLE_E_WIRE;
    return table_rearrange_hand(table(), (const char *)io, id_len, io + id_len, n);
}

// io = [user id][replacement name]
int wasm_table_redact(int id_len, int name_len) {
    const unsigned char *io = inputs(id_len + name_len);
    if (!io || id_len < 0 || name_len < 0) return TABLE_E_WIRE;
    return table_redact(table(), (const char *)io, id_len, (const char *)io + id_len, name_len);
}

// io = [actor id] -> the seat, -1, or TABLE_E_NOT_LOADED.
int wasm_table_seat_of(int id_len) {
    const unsigned char *io = inputs(id_len);
    if (!io || id_len < 0) return TABLE_E_WIRE;
    return table_seat_of(table(), (const char *)io, id_len);
}

int wasm_table_needs_bots(void) { return table_needs_bots(table()) ? 1 : 0; }

// ---- the bot cycle and the end of a game ----------------------------------------

// io = [games.game_seed as hex text]. The bridge's own base follows, so every
// other seeded export (wasm_strategy_seed_probe) agrees with the table's draws.
int wasm_table_set_deal_seed(int len) {
    const unsigned char *io = inputs(len);
    if (!io || len < 0) return TABLE_E_WIRE;
    const int rc = table_set_deal_seed(table(), (const char *)io, len);
    wasm_set_rng_base(table()->rng_base);
    return rc;
}

// io = [games.logs_packed bytes] -> records loaded.
int wasm_table_import_session_log(int len) {
    const unsigned char *io = inputs(len);
    if (!io || len < 0) return TABLE_E_WIRE;
    return table_import_session_log(table(), io, len);
}

// io = [prefs blob from wasm_table_drive_prefs, or nothing] -> actions applied;
// the cycle is at wasm_table_drive_ptr. No env: the C roster's knob specs are
// authoritative, and a table an earlier decision left must not override them
// (the reason sdk/ts/wasm/bots.ts wasmBotDrive clears it too).
int wasm_table_bot_drive(int prefs_len, int max_actions) {
    const unsigned char *io = inputs(prefs_len);
    if (!io || prefs_len < 0) return TABLE_E_WIRE;
    wasm_clearenv();
    wasm_reload_bot_flags();
    return table_bot_drive(table(), io, prefs_len, max_actions, &g_table_drive);
}

// -> io = the preferred moves to offer a retry of the last cycle.
int wasm_table_drive_prefs(void) {
    return table_drive_prefs(table(), &g_table_drive, wasm_io_ptr(), wasm_io_cap());
}

int wasm_table_cycle_delay_ms(void) { return table_cycle_delay_ms(table(), &g_table_drive); }

// io = [deal seed][session log] -> io = the verified v6 replay code. The code is
// written into the replay buffer and decoded back into the IO buffer past the
// inputs (the gate reads the log again), then copied to the front.
int wasm_table_replay_code(int seed_len, int log_len) {
    const unsigned char *io = inputs(seed_len + log_len);
    if (!io || seed_len < 0 || log_len < 0) return TABLE_E_WIRE;
    const int used = seed_len + log_len;
    const int n = table_replay_code(table(), io, seed_len, io + seed_len, log_len,
                                    wasm_replay_io_ptr(), wasm_replay_io_cap(),
                                    wasm_io_ptr() + used, wasm_io_cap() - used);
    if (n > 0) memcpy(wasm_io_ptr(), wasm_replay_io_ptr(), (size_t)n);
    return n;
}

// io = [session log] -> io = the replay extras blob (built in the replay buffer:
// the times are read from the log while the blob is written).
int wasm_table_replay_extras(int log_len) {
    const unsigned char *io = inputs(log_len);
    if (!io || log_len < 0) return TABLE_E_WIRE;
    const int n = table_replay_extras(table(), io, log_len, wasm_replay_io_ptr(), wasm_replay_io_cap());
    if (n > 0) memcpy(wasm_io_ptr(), wasm_replay_io_ptr(), (size_t)n);
    return n;
}
int wasm_table_bots_need_logs(void) { return table_bots_need_logs(table()) ? 1 : 0; }

// io = [game id] -> io = the arena wasm_table_commit_ptr's spans point into.
int wasm_table_commit_products(int gid_len, double next_version, double now_ms) {
    char gid[ROSTER_GAME_ID_MAX];
    if (take_gid(gid_len, gid) != 0) return TABLE_E_WIRE;
    return table_commit_products(table(), gid, gid_len, u32_of(next_version), (int64_t)now_ms,
                                 &g_table_commit, wasm_io_ptr(), wasm_io_cap());
}

// io = [game id] -> io = one viewer's as3 push.
int wasm_table_push(int gid_len, int viewer) {
    char gid[ROSTER_GAME_ID_MAX];
    if (take_gid(gid_len, gid) != 0) return TABLE_E_WIRE;
    return table_push(table(), gid, gid_len, viewer, wasm_io_ptr(), wasm_io_cap());
}

// io = [game id] -> io = one viewer's response envelope.
int wasm_table_envelope(int gid_len, int viewer, double version) {
    char gid[ROSTER_GAME_ID_MAX];
    if (take_gid(gid_len, gid) != 0) return TABLE_E_WIRE;
    return table_envelope(table(), gid, gid_len, viewer, u32_of(version), wasm_io_ptr(), wasm_io_cap());
}

// -> io = the action response body.
int wasm_table_action_response(int result, int reject, double version) {
    return table_action_response(result, reject, u32_of(version), wasm_io_ptr(), wasm_io_cap());
}

// -> TableElo.order, best first.
int wasm_table_rankings(void) {
    return table_rankings(table(), g_table_elo.order);
}

// TableElo.ratings (by seat) and .order (best first) -> TableElo.deltas (by seat).
int wasm_elo_deltas(int n) {
    return elo_deltas(g_table_elo.ratings, g_table_elo.order, n, g_table_elo.deltas);
}

// ---- test fixtures (e2e/helpers/table_fixture.ts) -----------------------------
//
// Test-only, like wasm_roster_*: a fixture composes a board field by field
// through the generated Game setters on the resident game, seats a roster
// here, and seals both into a row the kernel has already loaded (table_seal).
// Card text is read by card.h's parser, never by the host.

static Roster g_fixture_roster;

// A zeroed resident game (no face-up trump) and an empty roster; the table
// holds nothing loaded. -> the Game the host's setters write.
Game *wasm_fixture_begin(void) {
    Game *g = wasm_game_ptr_internal();
    memset(g, 0, sizeof(*g));
    g->flipped = CARD_NONE;
    memset(&g_fixture_roster, 0, sizeof(g_fixture_roster));
    table()->loaded = false;
    return g;
}

// io = [title]. ROSTER_OK or ROSTER_E_*.
int wasm_fixture_title(int len) {
    const unsigned char *io = inputs(len);
    if (!io) return ROSTER_E_TITLE;
    return roster_set_title(&g_fixture_roster, (const char *)io, len);
}

// io = [id][name][brain key, empty for a human]. The seat, or ROSTER_E_*.
int wasm_fixture_seat(int id_len, int name_len, int brain_len) {
    const unsigned char *io = inputs(id_len + name_len + brain_len);
    if (!io || id_len < 0 || name_len < 0 || brain_len < 0) return ROSTER_E_ID;
    const char *c = (const char *)io;
    return roster_seat_add(&g_fixture_roster, c, id_len, c + id_len, name_len, c + id_len + name_len, brain_len);
}

// -> io = [state blob][durable roster]. The state blob's length, or the refusal
// (table.h table_seal; wasm_table_detail after TABLE_E_ROSTER).
int wasm_fixture_seal(void) {
    Table *t = table();
    return table_seal(t, t->g, &g_fixture_roster, wasm_io_ptr(), wasm_io_cap());
}

// io = card text -> io = n Cards, one byte each, then (battles != 0) their n
// covers. At most `cap` items (the host passes the array it fills). n, or
// CARD_PARSE_E_*.
#define FIXTURE_CARDS_MAX 128
int wasm_card_list_parse(int len, int cap, int battles) {
    static Card cards[FIXTURE_CARDS_MAX], covers[FIXTURE_CARDS_MAX];
    const unsigned char *io = inputs(len);
    if (!io) return CARD_PARSE_E_CAP;
    if (cap < 0 || cap > FIXTURE_CARDS_MAX) cap = FIXTURE_CARDS_MAX;
    const int n = card_list_parse((const char *)io, len, cards, battles ? covers : 0, cap);
    if (n <= 0) return n;
    memcpy(wasm_io_ptr(), cards, (size_t)n);
    if (battles) memcpy(wasm_io_ptr() + n, covers, (size_t)n);
    return n;
}

// ---- the web client's slot (src/client_table.h) ---------------------------------
//
// The browser's reader of every envelope and push. Its board is a slot of its
// own, not the resident game an FMSG decode or a replay adopts, so neither can
// clobber what a player's screen was built from. The host reads the view and
// the event through sdk/ts/gen/view_layout.bots.ts snapshots.

static ClientSlot  g_client_slot;
static ClientTable g_client;
static int         g_client_ready;

static ClientTable *client(void) {
    if (!g_client_ready) {
        client_init(&g_client, &g_client_slot);
        g_client_ready = 1;
    }
    return &g_client;
}

TableView *wasm_client_view_ptr(void)  { return &client()->view; }
PushEvent *wasm_client_event_ptr(void) { return &client()->event; }
int wasm_client_detail(void) { return client()->detail; }

// The display rules of a board the host holds (client_view_rules): the host
// writes the view at wasm_client_rules_view_ptr through the generated writer,
// then reads the rules at wasm_client_rules_ptr. Separate from the slot's view,
// so asking about a changed board never disturbs what the slot last read.
static TableView g_rules_view;
static ViewRules g_rules;
TableView *wasm_client_rules_view_ptr(void) { return &g_rules_view; }
ViewRules *wasm_client_rules_ptr(void)      { return &g_rules; }
int wasm_client_view_rules(int from_deck, int to_flipped) {
    return client_view_rules(&g_rules_view, from_deck, to_flipped, &g_rules);
}

// io = [envelope]
int wasm_client_adopt_envelope(int len) {
    const unsigned char *io = inputs(len);
    if (!io || len < 0) return CLIENT_E_FORMAT;
    return client_adopt_envelope(client(), io, len);
}

// io = [push][identity]. The push must stay in the IO buffer until push_final.
int wasm_client_push_open(int len, int identity_len, int as3, double version) {
    const unsigned char *io = inputs(len + identity_len);
    if (!io || len < 0 || identity_len < 0) return CLIENT_E_PUSH;
    return client_push_open(client(), io, len, as3, io + len, identity_len, u32_of(version));
}

// The resident game (what an FMSG decode adopted) as `viewer` sees it.
int wasm_client_adopt_resident(int viewer) {
    return client_adopt_board(client(), wasm_game_ptr_internal(), viewer);
}

int wasm_client_push_next(void)  { return client_push_next(client()); }
int wasm_client_push_final(void) { return client_push_final(client()); }

int wasm_client_identity_at(void) { return client_identity_at(client()); }

// -> io = the identity to keep (0 bytes: none).
int wasm_client_identity(void) { return client_identity(client(), wasm_io_ptr(), wasm_io_cap()); }

// io = [game id][title]
int wasm_client_identity_begin(int gid_len, int title_len) {
    const unsigned char *io = inputs(gid_len + title_len);
    if (!io || gid_len < 0 || title_len < 0) return CLIENT_E_IDENTITY;
    return client_identity_begin(client(), (const char *)io, gid_len, (const char *)io + gid_len, title_len);
}

// io = [id][name]
int wasm_client_identity_seat(int id_len, int name_len, int is_ai) {
    const unsigned char *io = inputs(id_len + name_len);
    if (!io || id_len < 0 || name_len < 0) return CLIENT_E_IDENTITY;
    return client_identity_seat(client(), (const char *)io, id_len, (const char *)io + id_len, name_len, is_ai);
}

// ---- the boards a client makes (client_table.h, Phase 6b) ----
//
// A gate reads the board the rules view holds (wasm_client_rules_view_ptr, the
// same board client_view_rules reads, and neither changes it). An edit changes
// the board at wasm_client_edit_view_ptr in place, and the host reads it back.
// A conflict question reads the edit view as the push's last board and the
// final view as its final board. Everything is written through the generated
// writers (sdk/ts/gen/view_layout.bots.ts).
static TableView        g_edit_view, g_final_view;
static BoardEdit        g_board_edit;
static ClientConflict   g_conflict;
static ConflictVerdicts g_verdicts;
TableView *wasm_client_edit_view_ptr(void)        { return &g_edit_view; }
TableView *wasm_client_final_view_ptr(void)       { return &g_final_view; }
BoardEdit *wasm_client_board_edit_ptr(void)       { return &g_board_edit; }
ClientConflict *wasm_client_conflict_ptr(void)    { return &g_conflict; }
ConflictVerdicts *wasm_client_verdicts_ptr(void)  { return &g_verdicts; }

// io = [action wire]; the board at the rules view.
int wasm_client_validate(int len) {
    const unsigned char *io = inputs(len);
    if (!io || len < 0) return CLIENT_E_MOVE;
    return client_validate(client(), &g_rules_view, io, len);
}

// io = [action wire]; the board at the edit view.
int wasm_client_optimistic_apply(int len) {
    const unsigned char *io = inputs(len);
    if (!io || len < 0) return CLIENT_E_MOVE;
    return client_optimistic_apply(client(), &g_edit_view, io, len);
}

int wasm_client_board_edit(void) { return client_board_edit(client(), &g_edit_view, &g_board_edit); }

// io = [one byte per hand index]; the board at the edit view.
int wasm_client_rearrange_hand(int n) {
    const unsigned char *io = inputs(n);
    if (!io || n < 0) return CLIENT_E_MOVE;
    return client_rearrange_hand(client(), &g_edit_view, io, n);
}

int wasm_client_conflict_verdicts(void) {
    return client_conflict_verdicts(&g_edit_view, &g_final_view, &g_conflict, &g_verdicts);
}
