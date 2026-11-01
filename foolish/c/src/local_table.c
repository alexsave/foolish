// local_table.c - one human and one bot, in the browser. See local_table.h.
#include "local_table.h"
#include "bots.h"
#include "bot_roster.h"
#include "move_list.h"
#include <string.h>

#define HUMAN_LEN ((int)sizeof LOCAL_HUMAN_ID - 1)
#define BOT_LEN   ((int)sizeof LOCAL_BOT_ID - 1)

static int choose(const Game *g, int seat, const LegalMoves *moves) {
    return LOCAL_BOT_CHOOSE(g, seat, moves, NULL);
}

// The seed as the hex text table_set_deal_seed hashes (the server's games.game_seed).
static void seed_hex(const uint8_t *seed, char *out) {
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) {
        out[2 * i] = HEX[seed[i] >> 4];
        out[2 * i + 1] = HEX[seed[i] & 15];
    }
}

// The section's first step: the table as the last commit left it.
static void adopt(LocalTable *L) { table_adopt(&L->t, &L->res); }

// The section's last step, after an operation that changed the table: the
// products at the next version, as the server commits them (table_ops.c
// cb_section_commit) - the move list grows by the operation's chunk, the
// round's epoch moves with a closed round, the resident is kept with the clock
// the products advanced, and the human's cast is left for the page.
static int commit(LocalTable *L, int64_t now_ms) {
    Table *t = &L->t;
    const uint32_t next = L->version + 1;
    const int rc = table_commit_products_flags(t, L->gid, L->gid_len, next, now_ms,
                                               TABLE_PRODUCTS_NO_ROW | TABLE_PRODUCTS_NO_SPECTATOR_VIEW,
                                               &L->c, L->arena, sizeof L->arena);
    if (rc < 0) return rc;
    const TableCommit *c = &L->c;
    // The session's move list, as the server's store keeps it (table_ops.c section_row): a deal
    // or a lobby starts it again, any other operation appends its chunk.
    if (c->logs_reset || c->status == GAME_STATUS_WAITING) L->list_len = 0;
    if (c->logs.len > 0 && L->list_len < (int)sizeof L->list) {
        const int k = table_move_list_chunk(L->opened_ms, L->arena + c->logs.off, c->logs.len, L->list + L->list_len,
                                            (int)sizeof L->list - L->list_len);
        if (k >= 0) L->list_len += k;
        else L->list[L->list_len++] = MOVE_LIST_BROKEN_ITEM;   // no room: no replay, never no commit
    }
    L->round_epoch = (c->logs_reset || c->status == GAME_STATUS_WAITING) ? 0
                   : c->closed_round ? (int64_t)next : L->round_epoch;
    if (t->dealt_now) {
        char hex[2 * FOOLISH_SEED_LEN];
        seed_hex(L->seed, hex);
        table_set_deal_seed(t, hex, (int)sizeof hex);
    }
    const int n = table_cast(t, L->gid, L->gid_len, 0, next, L->cast, sizeof L->cast);
    if (n < 0) return n;
    L->cast_len = n;
    table_resident(t, &c->clock, &L->res);
    L->version = next;
    return TABLE_OK;
}

int local_table_open(LocalTable *L, Game *g, TableSnaps *snaps, const char *gid, int gid_len,
                     const uint8_t *seed, const char *you, int you_len, int64_t now_ms) {
    if (!L || !g || !snaps || !gid || !seed || gid_len <= 0 || gid_len > ROSTER_GAME_ID_MAX) return TABLE_E_WIRE;
    memset(L, 0, offsetof(LocalTable, list));
    L->opened_ms = now_ms;
    memcpy(L->gid, gid, (size_t)gid_len);
    L->gid_len = gid_len;
    memcpy(L->seed, seed, FOOLISH_SEED_LEN);
    Table *t = &L->t;
    table_init(t, g, snaps);

    // The bot is the roster's row for the brain: its key, and the nickname its
    // rung's first server account carries ("%Miami 1", bot_roster.h).
    const BotDef *d = bot_def(LOCAL_BOT_STRAT);
    if (!d || !d->key) return TABLE_E_UNKNOWN_BRAIN;
    char nick[ROSTER_NAME_MAX];
    const int nick_len = bot_nickname_write(LOCAL_BOT_STRAT, 1, nick, (int)sizeof nick);
    if (nick_len < 0) return TABLE_E_UNKNOWN_BRAIN;

    int rc = table_create(t, LOCAL_HUMAN_ID, HUMAN_LEN, you, you_len);
    if (rc == TABLE_OK)
        rc = table_add_bot(t, LOCAL_HUMAN_ID, HUMAN_LEN, LOCAL_BOT_ID, BOT_LEN, nick, nick_len,
                           d->key, (int)strlen(d->key), L->seed);
    if (rc != TABLE_OK) return rc;
    if ((rc = commit(L, now_ms)) != TABLE_OK) return rc;
    L->cast_len = 0;   // nobody was watching a table that did not exist: the page reads its envelope
    L->replay_len = L->extras_len = 0;
    L->open = true;
    return TABLE_OK;
}

int local_table_envelope(LocalTable *L, uint8_t *out, int cap) {
    if (!L || !L->open) return TABLE_E_NOT_LOADED;
    adopt(L);
    return table_envelope(&L->t, L->gid, L->gid_len, 0, L->version, out, cap);
}

int local_table_start(LocalTable *L, int64_t now_ms) {
    if (!L || !L->open) return TABLE_E_NOT_LOADED;
    adopt(L);
    const int rc = table_ready(&L->t, LOCAL_HUMAN_ID, HUMAN_LEN, L->seed);
    if (rc != TABLE_OK) return rc;
    const int cr = commit(L, now_ms);
    return cr != TABLE_OK ? cr : rc;
}

int local_table_action(LocalTable *L, const uint8_t *req, int req_len, int64_t now_ms, uint8_t *out, int cap) {
    if (!L || !L->open) return TABLE_E_NOT_LOADED;
    TableRequest q;
    if (!req || table_request_decode(req, req_len, &q) != 0) return TABLE_E_WIRE;
    if (q.gid_len != L->gid_len || memcmp(req + q.gid_off, L->gid, (size_t)q.gid_len) != 0) return TABLE_E_NOT_SEATED;
    adopt(L);
    const int rc = table_act(&L->t, LOCAL_HUMAN_ID, HUMAN_LEN, req + q.wire_off, q.wire_len,
                             q.has_intent ? (int64_t)q.intent : -1, L->round_epoch);
    if (rc < 0) return rc;
    if (rc == TABLE_OK) {
        const int cr = commit(L, now_ms);
        if (cr != TABLE_OK) return cr;
    }
    return table_action_response(rc, L->t.reject, L->version, out, cap);
}

int local_table_ask(LocalTable *L, const uint8_t *ask, int ask_len, int64_t now_ms) {
    if (!L || !L->open) return TABLE_E_NOT_LOADED;
    TableAsk a;
    if (!ask || ask_decode(ask, ask_len, &a) != ASK_OK) return TABLE_E_WIRE;
    if (a.gid_len != L->gid_len || memcmp(a.gid, L->gid, (size_t)a.gid_len) != 0) return TABLE_E_NOT_SEATED;
    adopt(L);
    const int rc = table_ask(&L->t, LOCAL_HUMAN_ID, HUMAN_LEN, "", 0, &a, NULL, L->seed);
    if (rc != TABLE_OK) return rc;
    const int cr = commit(L, now_ms);
    return cr != TABLE_OK ? cr : rc;
}

int local_table_bot_wait_ms(LocalTable *L, int64_t now_ms) {
    if (!L || !L->open) return -1;
    adopt(L);
    if (!table_bots_due(&L->t)) return -1;
    const int ms = table_bot_wait_ms(&L->t, now_ms);
    return ms < 0 ? -1 : ms;
}

int local_table_bot(LocalTable *L, int64_t now_ms) {
    if (!L || !L->open) return TABLE_E_NOT_LOADED;
    adopt(L);
    if (!table_bots_due(&L->t)) return 0;
    // No session log: the resident carries the bot memory (bot_memory.h), whose
    // record count is the progress every decision is seeded from.
    int rc;
    BotDriveOut drv;
    const int n = table_bot_drive_with(&L->t, choose, NULL, 0, 0, &drv);
    if (n <= 0) return n;
    if ((rc = commit(L, now_ms)) != TABLE_OK) return rc;
    return n;
}

int local_table_cast(LocalTable *L, uint8_t *out, int cap) {
    if (!L || L->cast_len <= 0) return 0;
    const int n = L->cast_len;
    if (!out || n > cap) return TABLE_E_CAP;
    memcpy(out, L->cast, (size_t)n);
    L->cast_len = 0;
    return n;
}

int local_table_replay(LocalTable *L, uint8_t *scratch, int scratch_cap) {
    if (!L || !L->open) return TABLE_E_NOT_LOADED;
    if (L->replay_len > 0) return TABLE_OK;
    adopt(L);
    if (L->t.g->status != GAME_STATUS_GAME_OVER) return TABLE_E_NOT_OVER;
    // From the move list, as the server's finalize_game cuts it.
    const int ne = table_replay_extras_list(&L->t, L->opened_ms, L->list, L->list_len, L->extras, sizeof L->extras);
    if (ne < 0) return ne;
    const int nc = table_replay_code_list(&L->t, L->seed, FOOLISH_SEED_LEN, L->list, L->list_len,
                                          L->replay, sizeof L->replay, scratch, scratch_cap);
    if (nc < 0) return nc;
    L->extras_len = ne;
    L->replay_len = nc;
    return TABLE_OK;
}

static int copy_out(const uint8_t *src, int n, uint8_t *out, int cap) {
    if (n <= 0) return TABLE_E_NOT_OVER;
    if (!out || n > cap) return TABLE_E_CAP;
    memcpy(out, src, (size_t)n);
    return n;
}

int local_table_replay_moves(const LocalTable *L, uint8_t *out, int cap) {
    return L ? copy_out(L->replay, L->replay_len, out, cap) : TABLE_E_NOT_LOADED;
}

int local_table_replay_extras(const LocalTable *L, uint8_t *out, int cap) {
    return L ? copy_out(L->extras, L->extras_len, out, cap) : TABLE_E_NOT_LOADED;
}
