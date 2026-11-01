// BotMemory (bot_memory.h): the fold, the binding and the durable form.
#include "bot_memory.h"
#include "view.h"
#include "../wasm/wire.h"

#include <string.h>
#ifdef BOT_MEM_ASSERT
#include <stdio.h>
#include <stdlib.h>
#endif

// ---- the rows (bot_common.h bot_pin_add / bot_pin_remove, the same rules) ----

static bool row_has(const Card *row, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(row[i], c)) return true;
    return false;
}
static void row_remove(Card *row, uint8_t *n, Card c) {
    for (int q = 0; q < *n; q++) {
        if (card_eq(row[q], c)) {
            row[q] = row[*n - 1];
            (*n)--;
            return;
        }
    }
}
// ---- THE ONE OWNER of "a belief never holds a fake card" ----------------------
//
// Every card the memory pins to a seat, every void it infers and every floor it
// sets passes here: a card of THIS game's deck (a suit, a value from the deck's
// lowest for the seat count up to the Ace), never CARD_HIDDEN, CARD_NONE, the
// all-zero {0,0} or a value the deck does not have. A record that would put one
// there came from a corrupt or hostile log; in the native test builds
// (BOT_MEM_ASSERT, beside ENGINE_SNAP_ASSERT in c/Makefile) that is a failure,
// loudly, and in every shipped build the card is simply not written.
_Thread_local int bot_mem_tolerate_fakes;
_Thread_local long bot_mem_fakes_refused;

static bool admit(const BotMemory *m, Card c, const char *what) {
    if (card_in_range(c, min_value_for(m->num_players), ACE_VALUE)) return true;
    bot_mem_fakes_refused++;
#ifdef BOT_MEM_ASSERT
    if (!bot_mem_tolerate_fakes) {
        fprintf(stderr, "bot_memory: refused to %s the card {suit %d, value %d} at %d seats (a fake card in a belief)\n",
                what, c.suit, c.value, m->num_players);
        abort();
    }
#else
    (void)what;
#endif
    return false;
}

static void row_add(const BotMemory *m, Card *row, uint8_t *n, Card c) {
    if (*n >= BOT_MEM_ROW || row_has(row, *n, c) || !admit(m, c, "pin")) return;
    row[(*n)++] = c;
}

static bool void_forbids(const BotMemory *m, const Game *g, int p, Card c) {
    for (int k = 0; k < m->void_n[p]; k++)
        if (can_cover(m->voids[p][k], c, g->power_suit)) return true;
    return false;
}

static void sat_inc(uint16_t *v) { if (*v < UINT16_MAX) (*v)++; }

void bot_mem_clear(BotMemory *m) {
    memset(m, 0, sizeof(*m));
    m->cur_def = -1;
    m->flip_holder = -1;
}

static void reset_for(BotMemory *m, const Game *g) {
    bot_mem_clear(m);
    const int n = g->num_players > 0 && g->num_players <= MAX_PLAYERS ? g->num_players : 0;
    m->num_players = (int8_t)n;
    m->in_now = (int8_t)n;
    for (int i = 0; i < n; i++) m->hand_n[i] = CARDS_PER_PLAYER;
}

// Seat p played c: off its row, against its tells and its floor, onto the
// deck-alive counts (still pending until the next draw).
static void seat_play(BotMemory *m, const Game *g, int p, Card c) {
    const int trump = g->power_suit;
    sat_inc(&m->cp_pend[p]);
    if (c.suit == trump) sat_inc(&m->tp_pend[p]);
    const bool declined = c.suit != trump && c.value >= 0 && (m->decl[p] >> c.value & 1u);
    if (declined || (!row_has(m->pin[p], m->pin_n[p], c) && void_forbids(m, g, p, c))) m->flags[p] |= BOT_MEM_TELL;
    if (p == m->flip_holder && (declined || (!row_has(m->alt, m->alt_n, c) && void_forbids(m, g, p, c))))
        m->flags[p] |= BOT_MEM_ALT_TELL;
    if (m->floor_v[p] > 0 && c.suit != trump && c.value < m->floor_v[p]) {
        m->floor_v[p] = 0;
        m->flags[p] |= BOT_MEM_FLOOR_BROKEN;
    }
    row_remove(m->pin[p], &m->pin_n[p], c);
    if (p == m->flip_holder) row_remove(m->alt, &m->alt_n, c);
}

void bot_mem_fold(BotMemory *m, const GameLog *L, const Game *g) {
    if (L->log_type == LOG_GAME_START) reset_for(m, g);
    m->n_records++;
    const int n = m->num_players;
    const int p = L->player_idx >= 0 && L->player_idx < n ? L->player_idx : -1;
    const int np = L->num_pairs > 0 ? (L->num_pairs < MAX_LOG_PAIRS ? L->num_pairs : MAX_LOG_PAIRS) : 0;
    const int trump = g->power_suit;
    switch (L->log_type) {
        case LOG_ATTACK:
        case LOG_PASS: {
            const bool first_attack = L->log_type == LOG_ATTACK && m->tbl_n == 0;
            if (p >= 0) m->hand_n[p] = (int16_t)(m->hand_n[p] - np);
            if (first_attack && m->cur_def < 0 && p >= 0) m->cur_def = (int8_t)((p + 1) % n);
            bool any_trump = false;
            for (int k = 0; k < np; k++) {
                const Card c = L->pairs[k].primary;
                if (c.suit == trump) any_trump = true;
                if (m->unc_n < BOT_MEM_UNC) m->unc[m->unc_n++] = c;
                if (m->tbl_n < BOT_MEM_TABLE) m->tbl[m->tbl_n++] = c;
                if (p >= 0) seat_play(m, g, p, c);
            }
            if (p >= 0 && L->log_type == LOG_ATTACK) {
                if (any_trump && m->viol_pend[p] < UINT16_MAX) m->viol_pend[p]++;
                if (first_attack && np == 1 && !any_trump && m->in_now > 2 && admit(m, L->pairs[0].primary, "set a floor from"))
                    m->floor_v[p] = (uint8_t)L->pairs[0].primary.value;
            }
            break;
        }
        case LOG_COVER:
            if (p >= 0) m->hand_n[p] = (int16_t)(m->hand_n[p] - np);
            for (int k = 0; k < np; k++) {
                const Card c = L->pairs[k].primary;
                if (m->tbl_n < BOT_MEM_TABLE) m->tbl[m->tbl_n++] = c;
                if (p >= 0) seat_play(m, g, p, c);
                if (!card_is_none(L->pairs[k].target)) {
                    for (int q = 0; q < m->unc_n; q++) {
                        if (card_eq(m->unc[q], L->pairs[k].target)) {
                            m->unc[q] = m->unc[--m->unc_n];
                            break;
                        }
                    }
                }
            }
            break;
        case LOG_GOOD:
            if (p >= 0 && m->in_now > 2 && m->cur_def >= 0 && m->cur_def < n
                && m->hand_n[m->cur_def] - m->unc_n >= 1) {
                for (int k = 0; k < m->tbl_n; k++) {
                    const Card c = m->tbl[k];
                    if (c.suit != trump && c.value >= 0) m->decl[p] |= (uint16_t)(1u << c.value);
                }
            }
            break;
        case LOG_DEFENDER_CHANGE:
            m->cur_def = L->defender_index;
            break;
        case LOG_PICKUP:
            if (p >= 0) {
                m->hand_n[p] = (int16_t)(m->hand_n[p] + m->tbl_n);
                m->decl[p] = 0;
                // Exactly one uncovered attack: the defender held no cover for it.
                if (m->unc_n == 1 && m->void_n[p] < BOT_MEM_VOIDS && admit(m, m->unc[0], "infer a void from"))
                    m->voids[p][m->void_n[p]++] = m->unc[0];
                for (int k = 0; k < m->tbl_n; k++) {
                    row_add(m, m->pin[p], &m->pin_n[p], m->tbl[k]);
                    if (p == m->flip_holder) row_add(m, m->alt, &m->alt_n, m->tbl[k]);
                }
                m->floor_v[p] = 0;
            }
            m->tbl_n = m->unc_n = 0;
            break;
        case LOG_DISCARD:
            for (int k = 0; k < m->tbl_n; k++) {
                const Card c = m->tbl[k];
                if (card_in_range(c, 1, ACE_VALUE)) m->discard |= 1ull << card_to_id(c);
            }
            m->tbl_n = m->unc_n = 0;
            break;
        case LOG_DRAW:
            // Every play before a draw was made with the deck alive.
            for (int s = 0; s < n; s++) {
                m->viol_conf[s] = m->viol_conf[s] + m->viol_pend[s] > UINT16_MAX ? UINT16_MAX : (uint16_t)(m->viol_conf[s] + m->viol_pend[s]);
                m->cp_conf[s] = m->cp_conf[s] + m->cp_pend[s] > UINT16_MAX ? UINT16_MAX : (uint16_t)(m->cp_conf[s] + m->cp_pend[s]);
                m->tp_conf[s] = m->tp_conf[s] + m->tp_pend[s] > UINT16_MAX ? UINT16_MAX : (uint16_t)(m->tp_conf[s] + m->tp_pend[s]);
                m->viol_pend[s] = m->cp_pend[s] = m->tp_pend[s] = 0;
            }
            if (p >= 0) {
                m->hand_n[p] = (int16_t)(m->hand_n[p] + np);
                m->decl[p] = 0;
                m->void_n[p] = 0;   // new unknown cards: the constraints expire
                m->floor_v[p] = 0;
                // This seat may have taken the face-up trump: its row as it is
                // if so (the LAST card of the record, the one revealed draw).
                if (m->flip_holder >= 0) m->flags[m->flip_holder] &= (uint8_t)~BOT_MEM_ALT_TELL;
                m->flip_holder = (int8_t)p;
                m->alt_n = m->pin_n[p];
                memcpy(m->alt, m->pin[p], sizeof m->alt);
                if (np > 0) {
                    const Card fc = L->pairs[np - 1].primary;
                    if (fc.value >= 0 && fc.suit >= 0) row_add(m, m->alt, &m->alt_n, fc);
                }
                if (m->flags[p] & BOT_MEM_TELL) m->flags[p] |= BOT_MEM_ALT_TELL;
            }
            break;
        case LOG_PLAYER_OUT:
            m->in_now--;
            break;
        default:
            break;
    }
}

void bot_mem_count(BotMemory *m, const GameLog *L, const Game *g) {
    if (L->log_type == LOG_GAME_START) {
        bot_mem_clear(m);
        m->num_players = (int8_t)(g->num_players > 0 && g->num_players <= MAX_PLAYERS ? g->num_players : 0);
        m->counts_only = 1;
    }
    m->n_records++;
}

void bot_mem_mask(const GameLog *rec, const Game *g, bool pre_has_flip, Card pre_flip, GameLog *l) {
    unsigned char w[4 + 2 * MAX_LOG_PAIRS];
    log_record_put(rec, 1, pre_has_flip, pre_flip, g->has_flipped, w);
    memset(l, 0, sizeof(*l));
    l->log_type = (int8_t)w[0];
    l->player_idx = (int8_t)w[1];
    l->defender_index = (int8_t)w[2];
    l->num_pairs = (int8_t)(w[3] < MAX_LOG_PAIRS ? w[3] : MAX_LOG_PAIRS);
    for (int j = 0; j < l->num_pairs; j++) {
        l->pairs[j].primary = card_from_wire_pair(w[4 + 2 * j]);
        l->pairs[j].target = card_from_wire_pair(w[5 + 2 * j]);
    }
}

void bot_mem_fold_masked(BotMemory *m, const GameLog *rec, const Game *g, bool pre_has_flip, Card pre_flip) {
    GameLog l;
    bot_mem_mask(rec, g, pre_has_flip, pre_flip, &l);
    bot_mem_fold(m, &l, g);
}

void bot_mem_from_logs(BotMemory *m, const Game *g) {
    bot_mem_clear(m);
    if (g->num_logs > 0 && g->logs[0].log_type != LOG_GAME_START) reset_for(m, g);
    for (int i = 0; i < g->num_logs; i++) bot_mem_fold(m, &g->logs[i], g);
}

// ---- the binding -----------------------------------------------------------

static _Thread_local const Game *t_bound_game;
static _Thread_local const BotMemory *t_bound_mem;

void bot_mem_bind(const Game *g, const BotMemory *m) {
    t_bound_game = m ? g : 0;
    t_bound_mem = m;
}

const BotMemory *bot_mem_bound(const Game *g) {
    return g && g == t_bound_game ? t_bound_mem : 0;
}

const BotMemory *bot_mem_bound_read(const Game *g) {
    const BotMemory *m = bot_mem_bound(g);
    if (!m || !m->counts_only) return m;
#ifdef BOT_MEM_ASSERT
    fprintf(stderr, "bot_memory: a brain read the memory of a table whose brains read none (bots.c uses_logs is wrong)\n");
    abort();
#endif
    return 0;
}

const BotMemory *bot_mem_for(const Game *g, BotMemory *scratch) {
    const BotMemory *m = bot_mem_bound_read(g);
    if (m) return m;
    bot_mem_from_logs(scratch, g);
    return scratch;
}

const BotMemory *bot_mem_of(const Game *g) {
    static _Thread_local BotMemory scratch;
    return bot_mem_for(g, &scratch);
}

uint32_t bot_mem_records(const Game *g) {
    const BotMemory *m = bot_mem_bound(g);
    return m ? m->n_records : (uint32_t)g->num_logs;
}

int bot_mem_discard_logs(const BotMemory *m, GameLog *dst, int cap) {
    int n = 0;
    GameLog *l = 0;
    for (int id = 0; id < 52; id++) {
        if (!(m->discard >> id & 1u)) continue;
        if (!l || l->num_pairs >= MAX_LOG_PAIRS) {
            if (n >= cap) break;
            l = &dst[n++];
            memset(l, 0, sizeof(*l));
            l->log_type = LOG_DISCARD;
            l->player_idx = -1;
            l->defender_index = -1;
        }
        l->pairs[l->num_pairs].primary = card_of_id(id);
        l->pairs[l->num_pairs].target = CARD_NONE;
        l->num_pairs++;
    }
    return n;
}

// ---- the durable form --------------------------------------------------------
//
//   varint n_records, u8 num_players, u8 in_now, i8 cur_def, i8 flip_holder,
//   u8 has (bit0 discard, bit1 table, bit2 alt, bit3 counts only: the byte is
//   then exactly 8, in_now 0, cur_def and flip_holder -1, and nothing follows),
//   [u64 LE discard], [u8 tbl_n, ids, u8 unc_n, ids], [u8 alt_n, ids],
//   then per seat: u8 present (bit0 row, bit1 voids, bit2 floor, bit3 flags,
//   bit4 decl, bit5 counts), i16 hand_n as a zigzag varint, and what present
//   says: [u8 n, ids] [u8 n, ids] [u8 floor] [u8 flags] [u16 LE decl]
//   [6 varints: viol, cp, tp, each conf then pend].
// A card is its id (card.h card_to_id); a byte past 51 is unreadable.

static int put_var(uint8_t *o, uint32_t v) {
    int n = 0;
    while (v >= 0x80) { o[n++] = (uint8_t)(v | 0x80); v >>= 7; }
    o[n++] = (uint8_t)v;
    return n;
}

typedef struct { const uint8_t *p; int len, at; bool bad; } Rd;
static uint32_t rd_u8(Rd *r) {
    if (r->at >= r->len) { r->bad = true; return 0; }
    return r->p[r->at++];
}
static uint32_t rd_var(Rd *r) {
    uint32_t v = 0;
    for (int s = 0; s < 35; s += 7) {
        const uint32_t b = rd_u8(r);
        v |= (b & 0x7F) << s;
        if (!(b & 0x80)) return v;
    }
    r->bad = true;
    return 0;
}

static bool card_byte_ok(Card c) { return card_in_range(c, 1, ACE_VALUE) && card_to_id(c) < 52; }

static int put_cards(uint8_t *o, const Card *c, int n, bool *ok) {
    o[0] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        if (!card_byte_ok(c[i])) *ok = false;
        o[1 + i] = (uint8_t)card_to_id(c[i]);
    }
    return 1 + n;
}
static int rd_cards(Rd *r, Card *c, int room) {
    const int n = (int)rd_u8(r);
    if (n > room) { r->bad = true; return 0; }
    for (int i = 0; i < n; i++) {
        const uint32_t id = rd_u8(r);
        if (id >= 52) { r->bad = true; return 0; }
        c[i] = card_of_id((int)id);
    }
    return n;
}

int bot_mem_encode(const BotMemory *m, uint8_t *out, int cap) {
    uint8_t buf[BOT_MEM_BYTES_MAX + 64];
    int at = 0;
    bool ok = true;
    at += put_var(buf + at, m->n_records);
    buf[at++] = (uint8_t)m->num_players;
    buf[at++] = (uint8_t)m->in_now;
    buf[at++] = (uint8_t)m->cur_def;
    buf[at++] = (uint8_t)m->flip_holder;
    if (m->counts_only) {
        buf[at++] = 8;
        if (at > cap) return -1;
        memcpy(out, buf, (size_t)at);
        return at;
    }
    const bool table = m->tbl_n || m->unc_n, alt = m->flip_holder >= 0;
    buf[at++] = (uint8_t)((m->discard ? 1 : 0) | (table ? 2 : 0) | (alt ? 4 : 0));
    if (m->discard) for (int i = 0; i < 8; i++) buf[at++] = (uint8_t)(m->discard >> (8 * i));
    if (table) {
        at += put_cards(buf + at, m->tbl, m->tbl_n, &ok);
        at += put_cards(buf + at, m->unc, m->unc_n, &ok);
    }
    if (alt) at += put_cards(buf + at, m->alt, m->alt_n, &ok);
    const int n = m->num_players > 0 && m->num_players <= MAX_PLAYERS ? m->num_players : 0;
    for (int s = 0; s < n; s++) {
        const bool counts = m->viol_conf[s] || m->viol_pend[s] || m->cp_conf[s] || m->cp_pend[s]
                         || m->tp_conf[s] || m->tp_pend[s];
        const uint8_t present = (uint8_t)((m->pin_n[s] ? 1 : 0) | (m->void_n[s] ? 2 : 0) | (m->floor_v[s] ? 4 : 0)
                                        | (m->flags[s] ? 8 : 0) | (m->decl[s] ? 16 : 0) | (counts ? 32 : 0));
        buf[at++] = present;
        const int32_t h = m->hand_n[s];
        at += put_var(buf + at, (uint32_t)((h << 1) ^ (h >> 31)));
        if (present & 1) at += put_cards(buf + at, m->pin[s], m->pin_n[s], &ok);
        if (present & 2) at += put_cards(buf + at, m->voids[s], m->void_n[s], &ok);
        if (present & 4) buf[at++] = m->floor_v[s];
        if (present & 8) buf[at++] = m->flags[s];
        if (present & 16) { buf[at++] = (uint8_t)m->decl[s]; buf[at++] = (uint8_t)(m->decl[s] >> 8); }
        if (present & 32) {
            at += put_var(buf + at, m->viol_conf[s]); at += put_var(buf + at, m->viol_pend[s]);
            at += put_var(buf + at, m->cp_conf[s]);   at += put_var(buf + at, m->cp_pend[s]);
            at += put_var(buf + at, m->tp_conf[s]);   at += put_var(buf + at, m->tp_pend[s]);
        }
        if (at > BOT_MEM_BYTES_MAX) return -1;
    }
    // A block holding a card that is no card (a corrupt fold input) is not
    // written: the next reader folds the log again instead.
    if (!ok || at > cap) return -1;
    memcpy(out, buf, (size_t)at);
    return at;
}

bool bot_mem_decode(BotMemory *m, const uint8_t *p, int len) {
    bot_mem_clear(m);
    Rd r = { p, len, 0, false };
    m->n_records = rd_var(&r);
    const int n = (int)rd_u8(&r);
    m->num_players = (int8_t)n;
    m->in_now = (int8_t)rd_u8(&r);
    m->cur_def = (int8_t)rd_u8(&r);
    m->flip_holder = (int8_t)rd_u8(&r);
    const uint32_t has = rd_u8(&r);
    if (has == 8) {
        // Counts only: the record count and the seats, nothing remembered.
        m->counts_only = 1;
        if (n > MAX_PLAYERS || m->in_now != 0 || m->cur_def != -1 || m->flip_holder != -1) r.bad = true;
    } else if (n > MAX_PLAYERS || m->in_now < 0 || m->in_now > n || m->cur_def < -1 || m->cur_def >= MAX_PLAYERS
        || m->flip_holder < -1 || m->flip_holder >= n || (has & ~7u) || ((has & 4) != 0) != (m->flip_holder >= 0))
        r.bad = true;
    if (!r.bad && (has & 1)) {
        for (int i = 0; i < 8; i++) m->discard |= (uint64_t)rd_u8(&r) << (8 * i);
        if (!m->discard || m->discard >> 52) r.bad = true;
    }
    if (!r.bad && (has & 2)) {
        m->tbl_n = (uint8_t)rd_cards(&r, m->tbl, BOT_MEM_TABLE);
        m->unc_n = (uint8_t)rd_cards(&r, m->unc, BOT_MEM_UNC);
        if (!m->tbl_n && !m->unc_n) r.bad = true;
    }
    if (!r.bad && (has & 4)) m->alt_n = (uint8_t)rd_cards(&r, m->alt, BOT_MEM_ROW);
    for (int s = 0; s < n && !r.bad && !m->counts_only; s++) {
        const uint32_t present = rd_u8(&r);
        if (present & ~63u) { r.bad = true; break; }
        const uint32_t z = rd_var(&r);
        const int32_t h = (int32_t)(z >> 1) ^ -(int32_t)(z & 1);
        if (h < INT16_MIN || h > INT16_MAX) { r.bad = true; break; }
        m->hand_n[s] = (int16_t)h;
        if (present & 1) { m->pin_n[s] = (uint8_t)rd_cards(&r, m->pin[s], BOT_MEM_ROW); if (!m->pin_n[s]) r.bad = true; }
        if (present & 2) { m->void_n[s] = (uint8_t)rd_cards(&r, m->voids[s], BOT_MEM_VOIDS); if (!m->void_n[s]) r.bad = true; }
        if (present & 4) { m->floor_v[s] = (uint8_t)rd_u8(&r); if (!m->floor_v[s] || m->floor_v[s] > ACE_VALUE) r.bad = true; }
        if (present & 8) {
            m->flags[s] = (uint8_t)rd_u8(&r);
            if (!m->flags[s] || (m->flags[s] & ~7u)) r.bad = true;
        }
        if (present & 16) {
            m->decl[s] = (uint16_t)(rd_u8(&r) | rd_u8(&r) << 8);
            if (!m->decl[s]) r.bad = true;
        }
        if (present & 32) {
            uint32_t v[6];
            for (int i = 0; i < 6; i++) { v[i] = rd_var(&r); if (v[i] > UINT16_MAX) r.bad = true; }
            m->viol_conf[s] = (uint16_t)v[0]; m->viol_pend[s] = (uint16_t)v[1];
            m->cp_conf[s] = (uint16_t)v[2];   m->cp_pend[s] = (uint16_t)v[3];
            m->tp_conf[s] = (uint16_t)v[4];   m->tp_pend[s] = (uint16_t)v[5];
            if (!(v[0] | v[1] | v[2] | v[3] | v[4] | v[5])) r.bad = true;
        }
    }
    // Every byte is the block's, and the block re-encodes to exactly them: one
    // spelling per block, so two writers of the same game write the same bytes.
    if (!r.bad && r.at != len) r.bad = true;
    if (!r.bad) {
        uint8_t again[BOT_MEM_BYTES_MAX];
        const int k = bot_mem_encode(m, again, (int)sizeof again);
        if (k != len || memcmp(again, p, (size_t)len) != 0) r.bad = true;
    }
    if (r.bad) { bot_mem_clear(m); return false; }
    return true;
}

bool bot_mem_equal(const BotMemory *a, const BotMemory *b) {
    uint8_t x[BOT_MEM_BYTES_MAX], y[BOT_MEM_BYTES_MAX];
    const int nx = bot_mem_encode(a, x, (int)sizeof x), ny = bot_mem_encode(b, y, (int)sizeof y);
    return nx == ny && (nx < 0 || memcmp(x, y, (size_t)nx) == 0);
}
