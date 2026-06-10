// Shard format implementation: ObservableState builder, tuple
// serialization, shard writer/reader, CRC32. See grpo_format.h.

#include "grpo_format.h"
#include "grpo_encode.h"
#include "strategy.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Lock the typed enum values to the encoder's #defines so the in-memory
// constants stay in sync. If you renumber one, the other must follow.
_Static_assert(GRPO_ROLE_ATTACKER    == ROLE_ATTACKER,    "role enum drift");
_Static_assert(GRPO_ROLE_DEFENDER    == ROLE_DEFENDER,    "role enum drift");
_Static_assert(GRPO_ROLE_CO_ATTACKER == ROLE_CO_ATTACKER, "role enum drift");
_Static_assert(GRPO_ROLE_IDLE        == ROLE_IDLE,        "role enum drift");
_Static_assert(GRPO_ROLE_COUNT       == N_ROLES,          "role enum drift");

// --- CRC32 -----------------------------------------------------------------

static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
}

uint32_t grpo_crc32(uint32_t seed, const void *data, size_t n) {
    pthread_once(&crc_once, crc_init);
    uint32_t c = seed ^ 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// --- Helpers ---------------------------------------------------------------

GrpoDeckVariant grpo_deck_variant_for(int num_players) {
    return (num_players >= 6) ? GRPO_DECK_VARIANT_52 : GRPO_DECK_VARIANT_36;
}

const char *grpo_role_name(GrpoRole r) {
    switch (r) {
        case GRPO_ROLE_ATTACKER:    return "attacker";
        case GRPO_ROLE_DEFENDER:    return "defender";
        case GRPO_ROLE_CO_ATTACKER: return "co_attacker";
        case GRPO_ROLE_IDLE:        return "idle";
        default:                    return "?";
    }
}

static inline void bitset_set(uint8_t *bs, int idx) {
    bs[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

// --- Public-knowledge derivations (parallel to grpo_encode.c) --------------

static void state_compute_discard(const Game *g, uint8_t bs[GRPO_DISCARD_BITSET_BYTES]) {
    memset(bs, 0, GRPO_DISCARD_BITSET_BYTES);
    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *l = &g->logs[i];
        if (l->log_type != LOG_DISCARD) continue;
        for (int j = 0; j < l->num_pairs; j++) {
            bitset_set(bs, CARD_IDX(l->pairs[j].primary));
        }
    }
}

static void state_compute_opp_held(const Game *g, int opp_idx,
                                   uint8_t bs[GRPO_DISCARD_BITSET_BYTES]) {
    memset(bs, 0, GRPO_DISCARD_BITSET_BYTES);
    // We must clear bits when a card is played, so we walk the log in order
    // and track a transient 52-bit set, then pack it at the end.
    bool held[MAX_DECK_INDEX] = { false };
    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *l = &g->logs[i];
        if (l->player_idx != opp_idx) continue;
        switch (l->log_type) {
            case LOG_PICKUP:
                for (int j = 0; j < l->num_pairs; j++) {
                    held[CARD_IDX(l->pairs[j].primary)] = true;
                    if (l->pairs[j].has_target) {
                        held[CARD_IDX(l->pairs[j].target)] = true;
                    }
                }
                break;
            case LOG_ATTACK:
            case LOG_COVER:
            case LOG_PASS:
                for (int j = 0; j < l->num_pairs; j++) {
                    held[CARD_IDX(l->pairs[j].primary)] = false;
                }
                break;
            default: break;
        }
    }
    for (int i = 0; i < MAX_DECK_INDEX; i++) if (held[i]) bitset_set(bs, i);
}

static GrpoRole derive_role(const Game *g, int player_idx) {
    const Player *p = &g->players[player_idx];
    if (player_idx == g->defender)       return GRPO_ROLE_DEFENDER;
    if (player_idx == g->first_attacker) return GRPO_ROLE_ATTACKER;
    if (p->status == PLAYER_STATUS_OUT)  return GRPO_ROLE_IDLE;
    if (p->awaiting_attack)              return GRPO_ROLE_CO_ATTACKER;
    return GRPO_ROLE_IDLE;
}

// --- Builders --------------------------------------------------------------

void grpo_observable_state_build(const Game *g, int self_idx, ObservableState *out) {
    memset(out, 0, sizeof(*out));
    out->num_players      = g->num_players;
    out->self_idx         = (int8_t)self_idx;
    out->power_suit       = g->power_suit;
    out->defender         = g->defender;
    out->first_attacker   = g->first_attacker;
    out->num_battles      = g->num_battles;
    out->num_eliminated   = g->num_eliminated;
    out->good_players_mask = (uint8_t)(g->good_players_mask & 0xFFu);
    out->has_flipped      = g->has_flipped;
    out->flipped          = g->flipped;
    out->deck_count       = g->deck_count;

    const Player *self = &g->players[self_idx];
    out->hand_count = self->hand_count;
    for (int i = 0; i < self->hand_count; i++) out->hand[i] = self->hand[i];

    for (int i = 0; i < g->num_battles; i++) out->table_battles[i] = g->table_battles[i];

    for (int i = 0; i < g->num_players; i++) {
        out->player_status[i]         = g->players[i].status;
        out->player_hand_count[i]     = g->players[i].hand_count;
        out->player_awaiting_attack[i] = g->players[i].awaiting_attack;
    }
    for (int i = 0; i < g->num_eliminated; i++) {
        out->elimination_order[i] = g->elimination_order[i];
    }

    state_compute_discard(g, out->discard_bitset);
    // Self's slot stays zero (we have the actual hand).
    for (int i = 0; i < g->num_players; i++) {
        if (i == self_idx) continue;
        state_compute_opp_held(g, i, out->opp_held_bitset[i]);
    }
}

static inline Card card_from_idx(int idx) {
    Card c; c.value = (int8_t)(idx / 4 + 1); c.suit = (int8_t)(idx % 4); return c;
}

// Append a LOG_PICKUP/LOG_DISCARD event for every set bit in `bs`. Batches
// up to MAX_LOG_PAIRS cards per GameLog entry to keep the log count small.
static void synth_log_events(Game *g, int log_type, int8_t player_idx,
                             const uint8_t *bs, size_t nbits) {
    int j = 0;
    GameLog *cur = NULL;
    for (size_t i = 0; i < nbits; i++) {
        if (!((bs[i >> 3] >> (i & 7)) & 1u)) continue;
        if (!cur || cur->num_pairs >= MAX_LOG_PAIRS) {
            if (g->num_logs >= MAX_LOGS) return;
            cur = &g->logs[g->num_logs++];
            memset(cur, 0, sizeof(*cur));
            cur->log_type      = (int8_t)log_type;
            cur->player_idx    = player_idx;
            cur->defender_index = -1;
            cur->num_pairs     = 0;
        }
        cur->pairs[cur->num_pairs].primary    = card_from_idx((int)i);
        cur->pairs[cur->num_pairs].has_target = false;
        cur->num_pairs++;
        j++;
    }
    (void)j;
}

void grpo_state_to_game(const ObservableState *s, Game *g) {
    memset(g, 0, sizeof(*g));
    g->status               = GAME_STATUS_PLAYING;
    g->num_players          = s->num_players;
    g->power_suit           = s->power_suit;
    g->first_attacker       = s->first_attacker;
    g->defender             = s->defender;
    g->num_battles          = s->num_battles;
    g->deck_count           = s->deck_count;
    g->has_flipped          = s->has_flipped;
    g->flipped              = s->flipped;
    g->discard_pile_length  = 0;   // recomputed from bitset card count below
    g->good_players_mask    = s->good_players_mask;
    g->has_good_timestamp   = (s->good_players_mask != 0);
    g->num_eliminated       = s->num_eliminated;
    for (int i = 0; i < s->num_eliminated; i++) g->elimination_order[i] = s->elimination_order[i];

    for (int i = 0; i < s->num_battles; i++) g->table_battles[i] = s->table_battles[i];

    for (int i = 0; i < s->num_players; i++) {
        Player *p = &g->players[i];
        p->status            = s->player_status[i];
        p->hand_count        = s->player_hand_count[i];
        p->awaiting_attack   = s->player_awaiting_attack[i];
        p->strategy_key      = STRAT_HANDWRITTEN;  // placeholder; not used at this stage
        snprintf(p->player_id, sizeof(p->player_id), "p%d", i);
        // Hand contents left zero for all but self.
        memset(p->hand, 0, sizeof(p->hand));
    }
    // Self's hand from stored cards.
    {
        Player *self = &g->players[s->self_idx];
        self->hand_count = s->hand_count;
        for (int i = 0; i < s->hand_count; i++) self->hand[i] = s->hand[i];
    }

    // Discard pile length: count bits in discard_bitset.
    int discard_n = 0;
    for (int i = 0; i < MAX_DECK_INDEX; i++) {
        if ((s->discard_bitset[i >> 3] >> (i & 7)) & 1u) discard_n++;
    }
    g->discard_pile_length = (int16_t)discard_n;

    // Synthesized logs so the encoder's log-scan finds the same bits.
    synth_log_events(g, LOG_DISCARD, -1, s->discard_bitset, MAX_DECK_INDEX);
    for (int p = 0; p < s->num_players; p++) {
        if (p == s->self_idx) continue;
        synth_log_events(g, LOG_PICKUP, (int8_t)p, s->opp_held_bitset[p], MAX_DECK_INDEX);
    }
}

int grpo_legal_move_match(const LegalMoves *moves, const LegalMove *chosen) {
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *m = &moves->moves[i];
        if (m->type != chosen->type) continue;
        if (m->n_cards != chosen->n_cards) continue;
        bool match = true;
        for (int k = 0; k < m->n_cards; k++) {
            if (!card_eq(m->cards[k], chosen->cards[k])) { match = false; break; }
        }
        if (!match) continue;
        if (chosen->type == MOVE_COVER) {
            for (int k = 0; k < m->n_cards; k++) {
                if (!card_eq(m->attack_cards[k], chosen->attack_cards[k])) { match = false; break; }
            }
            if (!match) continue;
        }
        return i;
    }
    return -1;
}

void grpo_tuple_build(const Game *g, int self_idx,
                      const LegalMove *chosen_move,
                      uint32_t game_seed, uint16_t game_decision_idx,
                      int n_live_at_decision,
                      TupleRecord *out) {
    memset(out, 0, sizeof(*out));
    out->game_seed = game_seed;
    out->game_decision_idx = game_decision_idx;
    out->role = derive_role(g, self_idx);
    out->deck_variant = grpo_deck_variant_for(g->num_players);
    out->n_live_at_decision = (int8_t)n_live_at_decision;
    grpo_observable_state_build(g, self_idx, &out->state);
    out->chosen_move = *chosen_move;
}

// --- Serialization ---------------------------------------------------------
//
// Packed, little-endian, no padding. Variable-length fields prefixed or
// gated by explicit counts (num_players, num_battles, etc).

static inline void buf_put_u8(uint8_t **p, uint8_t v) { **p = v; (*p)++; }
static inline void buf_put_i8(uint8_t **p, int8_t v)  { **p = (uint8_t)v; (*p)++; }
static inline void buf_put_u16(uint8_t **p, uint16_t v) { memcpy(*p, &v, 2); *p += 2; }
static inline void buf_put_i16(uint8_t **p, int16_t v)  { memcpy(*p, &v, 2); *p += 2; }
static inline void buf_put_u32(uint8_t **p, uint32_t v) { memcpy(*p, &v, 4); *p += 4; }
static inline void buf_put_card(uint8_t **p, Card c)    { buf_put_i8(p, c.suit); buf_put_i8(p, c.value); }

static inline uint8_t  buf_get_u8 (const uint8_t **p) { uint8_t v = **p; (*p)++; return v; }
static inline int8_t   buf_get_i8 (const uint8_t **p) { int8_t v = (int8_t)**p; (*p)++; return v; }
static inline uint16_t buf_get_u16(const uint8_t **p) { uint16_t v; memcpy(&v, *p, 2); *p += 2; return v; }
static inline int16_t  buf_get_i16(const uint8_t **p) { int16_t v;  memcpy(&v, *p, 2); *p += 2; return v; }
static inline uint32_t buf_get_u32(const uint8_t **p) { uint32_t v; memcpy(&v, *p, 4); *p += 4; return v; }
static inline Card     buf_get_card(const uint8_t **p) { Card c; c.suit = buf_get_i8(p); c.value = buf_get_i8(p); return c; }

// Upper bound on a single serialized tuple (used to gate flushes).
//
//   fixed metadata + counts                       ~30 bytes
//   per-player arrays  (MAX_PLAYERS=8, 3 fields)  24 bytes
//   elimination order  (MAX_PLAYERS=8)            8 bytes
//   hand               (MAX_HAND_SIZE=64 cards)   128 bytes
//   table battles      (MAX_BATTLES=32, 5 B/ea)   160 bytes
//   discard bitset                                7 bytes
//   opp_held bitsets   (MAX_PLAYERS * 7)          56 bytes
//   chosen move        (MAX_MOVE_CARDS=8 cards × 4 B + small header)  ~40 bytes
//                                                 -----
#define GRPO_TUPLE_MAX_BYTES 512

static size_t serialize_tuple(const TupleRecord *t, uint8_t *buf) {
    uint8_t *p = buf;

    buf_put_u32(&p, t->game_seed);
    buf_put_u16(&p, t->game_decision_idx);
    buf_put_u8(&p,  (uint8_t)t->role);
    buf_put_u8(&p,  (uint8_t)t->deck_variant);
    buf_put_i8(&p,  t->n_live_at_decision);

    const ObservableState *s = &t->state;
    buf_put_i8(&p, s->num_players);
    buf_put_i8(&p, s->self_idx);
    buf_put_i8(&p, s->power_suit);
    buf_put_i8(&p, s->defender);
    buf_put_i8(&p, s->first_attacker);
    buf_put_i8(&p, s->num_battles);
    buf_put_i8(&p, s->num_eliminated);
    buf_put_i8(&p, s->hand_count);
    buf_put_u8(&p, s->has_flipped ? 1u : 0u);
    buf_put_u8(&p, s->good_players_mask);
    buf_put_card(&p, s->flipped);              // written unconditionally; 2 bytes
    buf_put_i16(&p, s->deck_count);

    int N = s->num_players;
    for (int i = 0; i < N; i++) buf_put_i8(&p, s->player_status[i]);
    for (int i = 0; i < N; i++) buf_put_i8(&p, s->player_hand_count[i]);
    for (int i = 0; i < N; i++) buf_put_u8(&p, s->player_awaiting_attack[i] ? 1u : 0u);
    for (int i = 0; i < s->num_eliminated; i++) buf_put_i8(&p, s->elimination_order[i]);

    for (int i = 0; i < s->hand_count; i++) buf_put_card(&p, s->hand[i]);

    for (int i = 0; i < s->num_battles; i++) {
        const Battle *b = &s->table_battles[i];
        buf_put_card(&p, b->attack);
        buf_put_card(&p, b->defense);
        buf_put_u8(&p, b->has_defense ? 1u : 0u);
    }

    memcpy(p, s->discard_bitset, GRPO_DISCARD_BITSET_BYTES); p += GRPO_DISCARD_BITSET_BYTES;
    for (int i = 0; i < N; i++) {
        memcpy(p, s->opp_held_bitset[i], GRPO_DISCARD_BITSET_BYTES);
        p += GRPO_DISCARD_BITSET_BYTES;
    }

    const LegalMove *m = &t->chosen_move;
    buf_put_u8(&p, (uint8_t)m->type);
    buf_put_u8(&p, (uint8_t)m->n_cards);
    for (int i = 0; i < m->n_cards; i++) buf_put_card(&p, m->cards[i]);
    if (m->type == MOVE_COVER) {
        for (int i = 0; i < m->n_cards; i++) buf_put_card(&p, m->attack_cards[i]);
    }

    return (size_t)(p - buf);
}

static bool deserialize_tuple(const uint8_t *buf, size_t avail,
                              TupleRecord *out, size_t *consumed) {
    if (avail < 30) return false;
    const uint8_t *p0 = buf;
    const uint8_t *p = buf;

    out->game_seed         = buf_get_u32(&p);
    out->game_decision_idx = buf_get_u16(&p);
    out->role              = (GrpoRole)buf_get_u8(&p);
    out->deck_variant      = (GrpoDeckVariant)buf_get_u8(&p);
    out->n_live_at_decision = buf_get_i8(&p);

    ObservableState *s = &out->state;
    memset(s, 0, sizeof(*s));
    s->num_players     = buf_get_i8(&p);
    s->self_idx        = buf_get_i8(&p);
    s->power_suit      = buf_get_i8(&p);
    s->defender        = buf_get_i8(&p);
    s->first_attacker  = buf_get_i8(&p);
    s->num_battles     = buf_get_i8(&p);
    s->num_eliminated  = buf_get_i8(&p);
    s->hand_count      = buf_get_i8(&p);
    s->has_flipped     = buf_get_u8(&p) != 0;
    s->good_players_mask = buf_get_u8(&p);
    s->flipped         = buf_get_card(&p);
    s->deck_count      = buf_get_i16(&p);

    int N = s->num_players;
    if (N < 2 || N > MAX_PLAYERS) return false;
    for (int i = 0; i < N; i++) s->player_status[i] = buf_get_i8(&p);
    for (int i = 0; i < N; i++) s->player_hand_count[i] = buf_get_i8(&p);
    for (int i = 0; i < N; i++) s->player_awaiting_attack[i] = buf_get_u8(&p) != 0;
    for (int i = 0; i < s->num_eliminated; i++) s->elimination_order[i] = buf_get_i8(&p);

    for (int i = 0; i < s->hand_count; i++) s->hand[i] = buf_get_card(&p);
    for (int i = 0; i < s->num_battles; i++) {
        Battle *b = &s->table_battles[i];
        b->attack  = buf_get_card(&p);
        b->defense = buf_get_card(&p);
        b->has_defense = buf_get_u8(&p) != 0;
    }
    memcpy(s->discard_bitset, p, GRPO_DISCARD_BITSET_BYTES); p += GRPO_DISCARD_BITSET_BYTES;
    for (int i = 0; i < N; i++) {
        memcpy(s->opp_held_bitset[i], p, GRPO_DISCARD_BITSET_BYTES);
        p += GRPO_DISCARD_BITSET_BYTES;
    }

    LegalMove *m = &out->chosen_move;
    memset(m, 0, sizeof(*m));
    m->type    = buf_get_u8(&p);
    m->n_cards = buf_get_u8(&p);
    if (m->n_cards < 0 || m->n_cards > MAX_MOVE_CARDS) return false;
    for (int i = 0; i < m->n_cards; i++) m->cards[i] = buf_get_card(&p);
    if (m->type == MOVE_COVER) {
        for (int i = 0; i < m->n_cards; i++) m->attack_cards[i] = buf_get_card(&p);
    }

    *consumed = (size_t)(p - p0);
    return *consumed <= avail;
}

// --- Shard writer ----------------------------------------------------------

static void writer_flush(GrpoShardWriter *w) {
    if (w->buf_n == 0) return;
    w->crc = grpo_crc32(w->crc, w->buf, w->buf_n);
    fwrite(w->buf, 1, w->buf_n, w->fp);
    w->buf_n = 0;
}

bool grpo_shard_open(GrpoShardWriter *w, const char *path,
                     uint32_t worker_id, uint32_t base_seed) {
    memset(w, 0, sizeof(*w));
    w->fp = fopen(path, "wb");
    if (!w->fp) return false;
    w->buf = (uint8_t *)malloc(GRPO_SHARD_BUF_BYTES);
    if (!w->buf) { fclose(w->fp); w->fp = NULL; return false; }
    w->worker_id = worker_id;
    w->base_seed = base_seed;
    w->crc = 0;

    GrpoShardHeader h;
    memset(&h, 0, sizeof(h));
    h.magic         = GRPO_SHARD_MAGIC;
    h.version       = GRPO_SHARD_VERSION;
    h.worker_id     = worker_id;
    h.wall_time_unix = (uint32_t)time(NULL);
    h.base_seed     = base_seed;
    if (fwrite(&h, sizeof(h), 1, w->fp) != 1) { fclose(w->fp); w->fp = NULL; return false; }
    return true;
}

void grpo_shard_append(GrpoShardWriter *w, const TupleRecord *t) {
    uint8_t scratch[GRPO_TUPLE_MAX_BYTES];
    size_t n = serialize_tuple(t, scratch);
    if (w->buf_n + n > GRPO_SHARD_BUF_BYTES) writer_flush(w);
    memcpy(w->buf + w->buf_n, scratch, n);
    w->buf_n += n;
    w->tuple_count++;
    w->stream_bytes += n;
}

bool grpo_shard_close(GrpoShardWriter *w) {
    writer_flush(w);
    GrpoShardFooter f;
    f.magic        = GRPO_FOOTER_MAGIC;
    f.crc32        = w->crc;
    f.tuple_count  = w->tuple_count;
    f.stream_bytes = w->stream_bytes;
    bool ok = (fwrite(&f, sizeof(f), 1, w->fp) == 1);
    fclose(w->fp);
    free(w->buf);
    w->fp = NULL; w->buf = NULL;
    return ok;
}

// --- Shard reader ----------------------------------------------------------

bool grpo_shard_reader_open(GrpoShardReader *r, const char *path) {
    memset(r, 0, sizeof(*r));
    r->fp = fopen(path, "rb");
    if (!r->fp) return false;
    if (fread(&r->header, sizeof(r->header), 1, r->fp) != 1) {
        fclose(r->fp); r->fp = NULL; return false;
    }
    if (r->header.magic != GRPO_SHARD_MAGIC || r->header.version != GRPO_SHARD_VERSION) {
        fclose(r->fp); r->fp = NULL; return false;
    }
    // Footer at end of file.
    if (fseek(r->fp, -(long)sizeof(r->footer), SEEK_END) != 0) {
        fclose(r->fp); r->fp = NULL; return false;
    }
    if (fread(&r->footer, sizeof(r->footer), 1, r->fp) != 1) {
        fclose(r->fp); r->fp = NULL; return false;
    }
    if (r->footer.magic != GRPO_FOOTER_MAGIC) {
        fclose(r->fp); r->fp = NULL; return false;
    }
    // Seek back to start of records.
    if (fseek(r->fp, (long)sizeof(r->header), SEEK_SET) != 0) {
        fclose(r->fp); r->fp = NULL; return false;
    }
    r->crc_running = 0;
    return true;
}

bool grpo_shard_reader_next(GrpoShardReader *r, TupleRecord *out) {
    if (r->tuples_read >= r->footer.tuple_count) return false;

    // Read one tuple's worth — at most GRPO_TUPLE_MAX_BYTES; we peek and
    // parse, then rewind unused bytes.
    uint8_t scratch[GRPO_TUPLE_MAX_BYTES];
    long pos = ftell(r->fp);
    size_t avail = fread(scratch, 1, GRPO_TUPLE_MAX_BYTES, r->fp);
    if (avail == 0) return false;
    size_t consumed = 0;
    if (!deserialize_tuple(scratch, avail, out, &consumed)) return false;
    if (consumed < avail) {
        // Rewind the unconsumed portion.
        if (fseek(r->fp, pos + (long)consumed, SEEK_SET) != 0) return false;
    }
    r->crc_running = grpo_crc32(r->crc_running, scratch, consumed);
    r->tuples_read++;
    return true;
}

bool grpo_shard_reader_close(GrpoShardReader *r) {
    bool ok = (r->tuples_read == r->footer.tuple_count)
           && (r->crc_running == r->footer.crc32);
    if (r->fp) fclose(r->fp);
    r->fp = NULL;
    return ok;
}
