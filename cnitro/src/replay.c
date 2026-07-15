// Whole-game replay codec, format v5 — see replay.h. This is a bit-exact
// port of the TS reference (frozen as e2e/replay_ts_oracle.ts): the menus,
// weights, and probability model are WIRE FORMAT — never change them without
// bumping REPLAY_FORMAT_VERSION and keeping the old code path.
//
// Everything the projection shares with the real rules comes from the
// kernel: can_cover (game.c), the deck-size boundary min_value_for and
// CARDS_PER_PLAYER (card.h), and the LOG_* stream vocabulary (game.h). The
// public-state model itself (hand counts + known/unseen sets) is
// replay-specific by nature — the full-state engine has no notion of a
// spectator's information set — but it now lives beside the engine it
// mirrors and is difftested against real engine games (tests/
// replay_difftest.c, e2e/replay_codec.test.ts).
//
// JS-isms mirrored deliberately (wire format!):
//   - geo() masks its shift count to 5 bits, like the JS `>>` operator: a
//     player holding 32+ matching cards wraps the geometric decay.
//   - The binomial table is exact u64; every reachable call has n <= 51, so
//     the values equal the float64 reference's (all < 2^53, exact there).
//   - ID_QUANT is a power of two, so the quantization divisions are exact.
//
// Single-threaded by design (like the wasm bridge): the coder, model and
// bignum scratch are plain statics. Do not call from OMP arena workers.

#include "replay.h"
#include "wasm_overlay.h"
#include "rules_overlay.h"
#include <stddef.h>   // offsetof (the re-deal's short-log Game slot)
#include <string.h>

static int g_err_detail = 0;
int replay_last_error_detail(void) { return g_err_detail; }

/* =============================== bignum ================================== */
// Little-endian u32 limbs. rANS only ever multiplies-accumulates by, and
// divides by, small integers (M < 2^21), so two exact primitives suffice and
// the coder stays bit-identical to the BigInt reference.

// REC_CAP/BN_CAP are build parameters: the native tools keep the huge
// defaults; the wasm builds set measured caps (see the Makefile). Each
// recorded choice multiplies the integer by M < 2^21, so the integer needs
// <= ceil(21/32 x REC_CAP) limbs — BN_CAP derives from REC_CAP unless
// explicitly overridden.
#ifndef REPLAY_REC_CAP
#define REPLAY_REC_CAP 65536
#endif
#ifndef REPLAY_BN_CAP
#define REPLAY_BN_CAP ((REPLAY_REC_CAP * 21 + 31) / 32)
#endif
#define BN_CAP REPLAY_BN_CAP

// Measurement-only counters (-DREPLAY_STATS): peak recorded choices and peak
// bignum limbs across every encode/decode since process start. Compiled out
// (zero cost) everywhere the flag is absent, which is every production build.
#ifdef REPLAY_STATS
int replay_stat_max_rec = 0;
int replay_stat_max_bn = 0;
#define STAT_MAX(v, x) do { if ((x) > (v)) (v) = (x); } while (0)
#else
#define STAT_MAX(v, x) do { } while (0)
#endif

typedef struct { int n; uint32_t l[BN_CAP]; } Bn;
#ifdef CD_WASM_OVERLAY
// M8: g_bn aliases into solve_ws (see wasm_overlay.h). Written-before-read every
// call (bn_zero / bn_from_bytes_be / coder_finish set .n before any read).
_Static_assert(sizeof(Bn) <= CD_OVL_IO_OFF - CD_OVL_BN_OFF, "g_bn overflows its overlay slot");
#define g_bn (*(Bn *)(cd_overlay + CD_OVL_BN_OFF))
#elif defined(CD_RULES_OVERLAY)
// R1 (docs/RULES_GUARDS_WASM_MEMORY_PLAN.md): g_bn is the REPLAY family's
// bignum slot in the rules arena (see rules_overlay.h). Written-before-read
// every call, exactly as the M8 bots overlay above. 16-aligned arena satisfies
// Bn's 4-byte alignment at the 4-aligned RULES_OVL_BN_OFF.
_Static_assert(_Alignof(Bn) <= 16, "Bn alignment exceeds the arena's 16");
_Static_assert(RULES_OVL_BN_OFF % _Alignof(Bn) == 0, "g_bn offset misaligned");
_Static_assert(sizeof(Bn) <= RULES_OVL_REPLAY_IO_OFF - RULES_OVL_BN_OFF, "g_bn overflows its overlay slot");
#define g_bn (*(Bn *)(rules_overlay + RULES_OVL_BN_OFF))
#else
static Bn g_bn;
#endif

static void bn_zero(Bn *x) { x->n = 0; }
static bool bn_is_zero(const Bn *x) { return x->n == 0; }

// x = x * m + a (m >= 1); false when BN_CAP would overflow.
static bool bn_mul_add(Bn *x, uint32_t m, uint32_t a) {
    uint64_t carry = a;
    for (int i = 0; i < x->n; i++) {
        uint64_t t = (uint64_t)x->l[i] * m + carry;
        x->l[i] = (uint32_t)t;
        carry = t >> 32;
    }
    while (carry) {
        if (x->n >= BN_CAP) return false;
        x->l[x->n++] = (uint32_t)carry;
        carry >>= 32;
    }
    STAT_MAX(replay_stat_max_bn, x->n);
    return true;
}

// x = x / d (d >= 1); returns the remainder.
static uint32_t bn_divmod(Bn *x, uint32_t d) {
    uint64_t rem = 0;
    for (int i = x->n - 1; i >= 0; i--) {
        uint64_t cur = (rem << 32) | x->l[i];
        x->l[i] = (uint32_t)(cur / d);
        rem = cur % d;
    }
    while (x->n > 0 && x->l[x->n - 1] == 0) x->n--;
    return (uint32_t)rem;
}

static bool bn_from_bytes_be(Bn *x, const unsigned char *b, int len) {
    bn_zero(x);
    for (int i = 0; i < len; i++) {
        if (!bn_mul_add(x, 256, b[i])) return false;
    }
    return true;
}

// Minimal big-endian bytes; zero is the single byte 0x00 (bigintToBytes).
static int bn_to_bytes_be(const Bn *x, unsigned char *out, int out_cap) {
    if (x->n == 0) {
        if (out_cap < 1) return -1;
        out[0] = 0;
        return 1;
    }
    uint32_t top = x->l[x->n - 1];
    int top_bytes = top >= 0x1000000u ? 4 : top >= 0x10000u ? 3 : top >= 0x100u ? 2 : 1;
    int len = top_bytes + (x->n - 1) * 4;
    if (len > out_cap) return -1;
    int q = 0;
    for (int i = top_bytes - 1; i >= 0; i--) out[q++] = (unsigned char)((top >> (8 * i)) & 0xff);
    for (int i = x->n - 2; i >= 0; i--) {
        uint32_t v = x->l[i];
        out[q++] = (unsigned char)(v >> 24);
        out[q++] = (unsigned char)((v >> 16) & 0xff);
        out[q++] = (unsigned char)((v >> 8) & 0xff);
        out[q++] = (unsigned char)(v & 0xff);
    }
    return len;
}

/* ================================ coder ================================== */
// Exact rANS (codec.ts). Encode records (cum, w, M) per coded choice and
// finish pushes them in REVERSE; decode pops forward. A one-option menu
// codes zero bits on both sides.

typedef struct { uint32_t cum, w, M; } RecChoice;
#define REC_CAP REPLAY_REC_CAP  // build parameter; overflow is a clean error
#ifdef CD_WASM_OVERLAY
// M8: g_rec aliases into solve_ws (see wasm_overlay.h). Written-before-read
// every call — the Coder's n_rec index starts at 0, so entry [i] is stored
// before it is ever read back in the finish/decode pass.
_Static_assert((size_t)REC_CAP * sizeof(RecChoice) <= CD_OVL_BN_OFF, "g_rec overflows its overlay slot");
#define g_rec ((RecChoice *)(cd_overlay + CD_OVL_REC_OFF))
#elif defined(CD_RULES_OVERLAY)
// R1: g_rec is the REPLAY family's choice-log slot in the rules arena (offset
// 0). Written-before-read every call, exactly as the M8 bots overlay above.
_Static_assert(_Alignof(RecChoice) <= 16, "RecChoice alignment exceeds the arena's 16");
_Static_assert((size_t)REC_CAP * sizeof(RecChoice) <= RULES_OVL_BN_OFF - RULES_OVL_REC_OFF, "g_rec overflows its overlay slot");
#define g_rec ((RecChoice *)(rules_overlay + RULES_OVL_REC_OFF))
#else
static RecChoice g_rec[REC_CAP];
#endif

typedef struct {
    bool encode;
    int n_rec;  // encode: choices recorded so far
    Bn *x;      // decode: the shrinking integer
    int err;
} Coder;

static int coder_code(Coder *c, const uint32_t *w, int len, int chosen) {
    if (c->err) return 0;
    if (len <= 0) { c->err = REPLAY_EEMPTYMENU; return 0; }
    if (len == 1) return 0;  // forced move: 0 bits, both sides symmetric
    uint32_t M = 0;
    for (int j = 0; j < len; j++) M += w[j];
    if (c->encode) {
        if (chosen < 0 || chosen >= len) { c->err = REPLAY_ECHOSEN; return 0; }
        uint32_t cum = 0;
        for (int j = 0; j < chosen; j++) cum += w[j];
        if (c->n_rec >= REC_CAP) { c->err = REPLAY_ECAP; return 0; }
        g_rec[c->n_rec].cum = cum;
        g_rec[c->n_rec].w = w[chosen];
        g_rec[c->n_rec].M = M;
        c->n_rec++;
        STAT_MAX(replay_stat_max_rec, c->n_rec);
        return chosen;
    }
    uint32_t r = bn_divmod(c->x, M);
    uint32_t acc = 0;
    int k = 0;
    for (; k < len; k++) {
        if (r < acc + w[k]) break;
        acc += w[k];
    }
    if (k >= len) k = len - 1;  // defensive, mirrors ransPop
    if (!bn_mul_add(c->x, w[k], r - acc)) { c->err = REPLAY_ECAP; return 0; }
    return k;
}

static int coder_uniform(Coder *c, int n, int chosen) {
    if (c->err) return 0;
    if (n <= 0) { c->err = REPLAY_EEMPTYMENU; return 0; }
    if (n == 1) return 0;
    if (c->encode) {
        if (chosen < 0 || chosen >= n) { c->err = REPLAY_ECHOSEN; return 0; }
        if (c->n_rec >= REC_CAP) { c->err = REPLAY_ECAP; return 0; }
        g_rec[c->n_rec].cum = (uint32_t)chosen;
        g_rec[c->n_rec].w = 1;
        g_rec[c->n_rec].M = (uint32_t)n;
        c->n_rec++;
        STAT_MAX(replay_stat_max_rec, c->n_rec);
        return chosen;
    }
    // all-ones pop: k = x % n, x /= n (the w=1 multiply is a no-op)
    return (int)bn_divmod(c->x, (uint32_t)n);
}

static bool coder_finish(Coder *c, Bn *x) {
    bn_zero(x);
    for (int i = c->n_rec - 1; i >= 0; i--) {
        uint32_t r = bn_divmod(x, g_rec[i].w);
        if (!bn_mul_add(x, g_rec[i].M, g_rec[i].cum + r)) return false;
    }
    return true;
}

/* ============================== binomials ================================ */

static uint64_t g_comb[52][52];
static bool g_comb_ready = false;

static void comb_init(void) {
    if (g_comb_ready) return;
    for (int n = 0; n < 52; n++) {
        g_comb[n][0] = 1;
        for (int k = 1; k <= n; k++)
            g_comb[n][k] = g_comb[n - 1][k - 1] + (k <= n - 1 ? g_comb[n - 1][k] : 0);
        for (int k = n + 1; k < 52; k++) g_comb[n][k] = 0;
    }
    g_comb_ready = true;
}

static uint64_t comb64(int n, int k) {
    if (n < 0 || n >= 52 || k < 0 || k > n) return 0;
    return g_comb[n][k];
}

/* ============================ weight profile ============================= */
// FROZEN wire format (core.ts V1) — bump REPLAY_FORMAT_VERSION to change.

#define V1_COVER             6
#define V1_COVER_FRESH       3
#define V1_PASS              16
#define V1_PASS_FRESH        4
#define V1_PICKUP            2
#define V1_ATTACK            2
#define V1_ATTACK_FRESH_LEAD 2
#define V1_ATTACK_FRESH      1
#define V1_ROUND_END         3
#define V1_STOP              1
#define V1_ID_QUANT          16384

// JS `>>` masks the shift count to 5 bits; a 32+ position wraps. Wire format.
static uint32_t geo(uint32_t base, int pos) {
    uint32_t w = base >> (pos & 31);
    return w >= 1 ? w : 1;
}

/* ============================= public model ============================== */

#define RMAX_BATTLES 64  // >= any reachable table (52 distinct cards)
#define MENU_CAP 2048
#define CONT_CAP 53

typedef struct { int8_t attack; int8_t defense; } RBattle;  // defense -1 = uncovered

typedef struct {
    int n;
    int trump_id;
    int power_suit;
    bool status[MAX_PLAYERS];        // true = IN
    uint64_t known[MAX_PLAYERS];     // revealed cards currently in hand
    int unknown[MAX_PLAYERS];        // count of hidden cards in hand
    uint64_t unseen;                 // never-revealed cards (stock + hidden)
    int deck_count;                  // face-down stock (excludes the flip)
    bool flipped_held;               // trump card still waiting under the stock
    RBattle battles[RMAX_BATTLES];
    int num_battles;
    int first_attacker;
    int defender;
    int elim[MAX_PLAYERS];
    int num_elim;
    int discard;
    // Decode log stream, written as it happens (NULL while encoding — the
    // stream is derived output, never read back by the run itself).
    unsigned char *out;
    int out_pos, out_cap;
    uint32_t out_logs;
    int err;
    // ---- Format 6 only (0 / NULL for v5) ----
    int format;                // 5 (default) or 6
    Coder *rev_coder;          // coder reached from draw_for to code reveals
    const unsigned char *rev;  // encode: real hidden card ids (deal + draws)
    int rev_n, rev_pos;        // reveal stream length / cursor
} RModel;

static RModel g_model;

static int id_value(int id) { return id % 13 + 1; }

static Card id_to_card(int id) {
    Card c;
    c.suit = (int8_t)(id / 13);
    c.value = (int8_t)(id % 13 + 1);
    return c;
}

/* ---------------------------- the atom sink ------------------------------ */
// Only replay_decode_atoms_v6 installs this; every other path through apply_*
// (v5, and both encode directions) leaves it null and pays nothing. It fires
// from inside the deal/draw/apply_* points because those are where a move is
// fully RESOLVED — the coder above them still holds only a menu index.

static ReplayAtomSink g_atom_sink = 0;
static void          *g_atom_ctx  = 0;

static void atom_out(int kind, int seat, const int *ids, int n, int target_id) {
    if (!g_atom_sink) return;
    ReplayAtom a;
    a.kind = kind;
    a.seat = seat;
    a.n_cards = n > REPLAY_MAX_PAIRS ? REPLAY_MAX_PAIRS : n;
    for (int i = 0; i < a.n_cards; i++) a.cards[i] = id_to_card(ids[i]);
    a.target = target_id >= 0 ? id_to_card(target_id) : CARD_NONE;
    g_atom_sink(g_atom_ctx, &a);
}

static int hand_len(const RModel *m, int s) {
    return __builtin_popcountll(m->known[s]) + m->unknown[s];
}

static int stock_total(const RModel *m) {
    return m->deck_count + (m->flipped_held ? 1 : 0);
}

static int in_count(const RModel *m) {
    int c = 0;
    for (int s = 0; s < m->n; s++) if (m->status[s]) c++;
    return c;
}

// Faithful port of get_next_player_index, including the "<= 1 player left
// returns current" guard — derived DEFENDER_CHANGE indexes at game end
// depend on it.
static int next_in(const RModel *m, int cur) {
    if (in_count(m) <= 1) return cur;
    int nx = (cur + 1) % m->n;
    while (!m->status[nx]) nx = (nx + 1) % m->n;
    return nx;
}

// defense-beats-attack — THE kernel rule (game.c), not a re-implementation.
static bool beats(const RModel *m, int def_id, int atk_id) {
    return can_cover(id_to_card(atk_id), id_to_card(def_id), m->power_suit);
}

/* Preference order: cheap cards first — non-trumps ascending by (value,
 * suit), then trumps ascending. Menu enumeration order AND the weight decay
 * both follow it, so this ordering is wire format. */
static int pref_key(const RModel *m, int id) {
    int suit = id / 13;
    int trump = (suit == m->power_suit) ? 1 : 0;
    return trump * 4096 + id_value(id) * 4 + suit;
}

// Insertion sort on (pref_key, id) — a total order, so the result is the
// unique sequence the TS comparator produced regardless of algorithm.
static void pref_sort(const RModel *m, int8_t *ids, int n) {
    for (int i = 1; i < n; i++) {
        int8_t v = ids[i];
        int kv = pref_key(m, v);
        int j = i - 1;
        while (j >= 0) {
            int kj = pref_key(m, ids[j]);
            if (kj < kv || (kj == kv && ids[j] < v)) break;
            ids[j + 1] = ids[j];
            j--;
        }
        ids[j + 1] = v;
    }
}

static int sorted_known(const RModel *m, int s, int8_t *out) {
    int n = 0;
    for (int id = 0; id < 52; id++)
        if ((m->known[s] >> id) & 1ull) out[n++] = (int8_t)id;
    pref_sort(m, out, n);
    return n;
}

#define PRED_ANY   0
#define PRED_VALUE 1
#define PRED_BEATS 2
#define PRED_TABLE 3
typedef struct { int kind; int v; int atk; const bool *tv; } Pred;

static bool pred_ok(const RModel *m, Pred p, int id) {
    switch (p.kind) {
        case PRED_ANY:   return true;
        case PRED_VALUE: return id_value(id) == p.v;
        case PRED_BEATS: return beats(m, id, p.atk);
        case PRED_TABLE: return p.tv[id_value(id)];
    }
    return false;
}

static int unseen_matching(const RModel *m, Pred p, int8_t *out) {
    int n = 0;
    for (int id = 0; id < 52; id++)
        if (((m->unseen >> id) & 1ull) && pred_ok(m, p, id)) out[n++] = (int8_t)id;
    pref_sort(m, out, n);
    return n;
}

static void check_conservation(RModel *m) {
    int u = 0;
    for (int s = 0; s < m->n; s++) u += m->unknown[s];
    if (__builtin_popcountll(m->unseen) != m->deck_count + u)
        m->err = REPLAY_ECONSERVATION;
}

/* taking a card out of a hand at the moment it is chosen */
static void take_known(RModel *m, int seat, int id) {
    uint64_t bit = 1ull << id;
    if (!(m->known[seat] & bit)) { m->err = REPLAY_EKNOWN; return; }
    m->known[seat] &= ~bit;
    m->unseen &= ~bit;  // no-op (known => already seen); keep symmetric
}

static void take_fresh(RModel *m, int seat, int id) {
    uint64_t bit = 1ull << id;
    if (!(m->unseen & bit)) { m->err = REPLAY_EFRESH; return; }
    m->unseen &= ~bit;
    if (m->unknown[seat] <= 0) { m->err = REPLAY_EHIDDEN; return; }
    m->unknown[seat]--;
}

/* ---------------------------- output stream ----------------------------- */

static void emit(RModel *m, int type, int seat, int def_idx,
                 const unsigned char (*pairs)[2], int n_pairs) {
    if (m->err || !m->out) return;
    if (m->out_pos + 4 + 2 * n_pairs > m->out_cap) { m->err = REPLAY_ECAP; return; }
    unsigned char *q = m->out + m->out_pos;
    *q++ = (unsigned char)type;
    *q++ = seat < 0 ? 0xFF : (unsigned char)seat;
    *q++ = def_idx < 0 ? 0xFF : (unsigned char)def_idx;
    *q++ = (unsigned char)n_pairs;
    for (int i = 0; i < n_pairs; i++) {
        *q++ = pairs[i][0];
        *q++ = pairs[i][1];
    }
    m->out_pos += 4 + 2 * n_pairs;
    m->out_logs++;
}

static int table_pairs(const RModel *m, unsigned char (*pairs)[2]) {
    int n = 0;
    for (int i = 0; i < m->num_battles; i++) {
        pairs[n][0] = (unsigned char)m->battles[i].attack;
        pairs[n][1] = REPLAY_CARD_NONE;
        n++;
        if (m->battles[i].defense >= 0) {
            pairs[n][0] = (unsigned char)m->battles[i].defense;
            pairs[n][1] = REPLAY_CARD_NONE;
            n++;
        }
    }
    return n;
}

/* --------------------------- derived cascades --------------------------- */

static int code_reveal(RModel *m, Coder *c, int seat);  // format 6, defined below

// Port of refillPlayerHandsWithEvents semantics (kernel refill_player_hands
// projected onto public state). Emits DRAW logs; players who end the refill
// with no cards while the stock is dry go OUT silently.
static void draw_for(RModel *m, int seat) {
    unsigned char pairs[CARDS_PER_PLAYER][2];
    int nd = 0;
    while (hand_len(m, seat) < CARDS_PER_PLAYER) {
        if (m->deck_count > 0) {
            m->deck_count--;
            if (m->format == 6) {
                // v6: reveal the real drawn card inline (unseen now == stock).
                int id = code_reveal(m, m->rev_coder, seat);
                if (m->err) return;
                pairs[nd][0] = (unsigned char)id;
            } else {
                m->unknown[seat]++;
                pairs[nd][0] = REPLAY_CARD_HIDDEN;
            }
            pairs[nd][1] = REPLAY_CARD_NONE;
            nd++;
        } else if (m->flipped_held) {
            m->flipped_held = false;
            m->known[seat] |= 1ull << m->trump_id;
            pairs[nd][0] = (unsigned char)m->trump_id;
            pairs[nd][1] = REPLAY_CARD_NONE;
            nd++;
        } else {
            break;
        }
    }
    if (nd > 0) emit(m, LOG_DRAW, seat, -1, pairs, nd);
    if (nd > 0 && g_atom_sink) {
        int wide[REPLAY_MAX_PAIRS];
        int nn = nd > REPLAY_MAX_PAIRS ? REPLAY_MAX_PAIRS : nd;
        for (int i = 0; i < nn; i++) wide[i] = pairs[i][0];
        atom_out(REPLAY_ATOM_DRAW, seat, wide, nn, -1);
    }
}

static void refill(RModel *m) {
    if (stock_total(m) == 0) {
        // engine's early-return branch: seat order, not rotation order
        for (int i = 0; i < m->n; i++) {
            if (hand_len(m, i) == 0 && m->status[i]) {
                m->status[i] = false;
                m->elim[m->num_elim++] = i;
            }
        }
        return;
    }

    // defender draws first when a clean cover emptied their hand
    if (hand_len(m, m->defender) == 0) draw_for(m, m->defender);

    int p = m->first_attacker;
    bool visited[MAX_PLAYERS] = { false };
    do {
        if (visited[p]) break;
        visited[p] = true;
        draw_for(m, p);
        if (hand_len(m, p) == 0 && m->status[p]) {
            m->status[p] = false;
            m->elim[m->num_elim++] = p;
        }
        p = next_in(m, p);
    } while (p != m->first_attacker);
}

// Shared discard+refill+rotation used by the good-transition — and, with a
// different rotation, by cover/pickup.
static void discard_table(RModel *m) {
    m->discard += m->num_battles * 2;  // engine counts 2 per battle (all covered)
    unsigned char pairs[2 * RMAX_BATTLES][2];
    int np = table_pairs(m, pairs);
    emit(m, LOG_DISCARD, -1, -1, pairs, np);
    m->num_battles = 0;
}

/* ------------------------ applying chosen atoms -------------------------- */
// Each apply_* mirrors the corresponding execute* in the server actions,
// including the exact order of emitted logs and rotation updates.

static void apply_attack(RModel *m, int seat, const int8_t *ids, int n) {
    if (g_atom_sink) {
        int wide[REPLAY_MAX_PAIRS];
        int nn = n > REPLAY_MAX_PAIRS ? REPLAY_MAX_PAIRS : n;
        for (int i = 0; i < nn; i++) wide[i] = ids[i];
        atom_out(REPLAY_ATOM_ATTACK, seat, wide, nn, -1);
    }
    unsigned char pairs[REPLAY_MAX_PAIRS][2];
    for (int i = 0; i < n; i++) {
        if (m->num_battles >= RMAX_BATTLES) { m->err = REPLAY_ECAP; return; }
        m->battles[m->num_battles].attack = ids[i];
        m->battles[m->num_battles].defense = -1;
        m->num_battles++;
        pairs[i][0] = (unsigned char)ids[i];
        pairs[i][1] = REPLAY_CARD_NONE;
    }
    emit(m, LOG_ATTACK, seat, -1, pairs, n);
    if (hand_len(m, seat) == 0 && stock_total(m) == 0) {
        // out on an emptied hand only once the stock is dry — with cards
        // still in the deck the attacker refills at round end
        m->status[seat] = false;
        m->elim[m->num_elim++] = seat;
        emit(m, LOG_PLAYER_OUT, seat, -1, 0, 0);
    }
}

static void apply_cover(RModel *m, int b, int cover_id) {
    // Read the attack card BEFORE the assignment below: it is the atom's target.
    if (g_atom_sink) {
        int one = cover_id;
        atom_out(REPLAY_ATOM_COVER, m->defender, &one, 1, m->battles[b].attack);
    }
    m->battles[b].defense = (int8_t)cover_id;
    unsigned char pair[1][2];
    pair[0][0] = (unsigned char)cover_id;
    pair[0][1] = (unsigned char)m->battles[b].attack;
    emit(m, LOG_COVER, m->defender, -1, pair, 1);

    if (hand_len(m, m->defender) == 0) {
        // executeCover's clean-sweep branch: discard, refill (defender
        // first), defender leads next bout — or goes out if the stock is dry.
        discard_table(m);
        refill(m);
        m->first_attacker = m->defender;
        if (hand_len(m, m->defender) == 0) {
            bool was_in = m->status[m->first_attacker];
            m->status[m->first_attacker] = false;
            if (was_in) m->elim[m->num_elim++] = m->first_attacker;
            // engine logs even if refill already marked OUT
            emit(m, LOG_PLAYER_OUT, m->first_attacker, -1, 0, 0);
            m->first_attacker = next_in(m, m->first_attacker);
        }
        m->defender = next_in(m, m->first_attacker);
        emit(m, LOG_DEFENDER_CHANGE, -1, m->defender, 0, 0);
    }
}

static void apply_pass(RModel *m, int seat, const int8_t *ids, int n) {
    if (g_atom_sink) {
        int wide[REPLAY_MAX_PAIRS];
        int nn = n > REPLAY_MAX_PAIRS ? REPLAY_MAX_PAIRS : n;
        for (int i = 0; i < nn; i++) wide[i] = ids[i];
        atom_out(REPLAY_ATOM_PASS, seat, wide, nn, -1);
    }
    unsigned char pairs[REPLAY_MAX_PAIRS][2];
    for (int i = 0; i < n; i++) {
        if (m->num_battles >= RMAX_BATTLES) { m->err = REPLAY_ECAP; return; }
        m->battles[m->num_battles].attack = ids[i];
        m->battles[m->num_battles].defense = -1;
        m->num_battles++;
        pairs[i][0] = (unsigned char)ids[i];
        pairs[i][1] = REPLAY_CARD_NONE;
    }
    emit(m, LOG_PASS, seat, -1, pairs, n);
    int next = next_in(m, m->defender);
    if (stock_total(m) == 0 && hand_len(m, seat) == 0) {
        m->status[seat] = false;
        m->elim[m->num_elim++] = seat;
        emit(m, LOG_PLAYER_OUT, seat, -1, 0, 0);
    }
    m->defender = next;
    emit(m, LOG_DEFENDER_CHANGE, -1, m->defender, 0, 0);
}

static void apply_pickup(RModel *m) {
    atom_out(REPLAY_ATOM_PICKUP, m->defender, 0, 0, -1);
    unsigned char pairs[2 * RMAX_BATTLES][2];
    int np = table_pairs(m, pairs);
    emit(m, LOG_PICKUP, m->defender, -1, pairs, np);
    for (int i = 0; i < np; i++) m->known[m->defender] |= 1ull << pairs[i][0];
    m->num_battles = 0;
    refill(m);
    m->first_attacker = next_in(m, m->defender);
    m->defender = next_in(m, m->first_attacker);
    emit(m, LOG_DEFENDER_CHANGE, -1, m->defender, 0, 0);
}

// executeRoundTransition: before the discard, every IN attacker says good,
// in seat order (good presses cost no wire bits — v4).
static void apply_round_end(RModel *m) {
    atom_out(REPLAY_ATOM_ROUND_END, -1, 0, 0, -1);
    for (int s = 0; s < m->n; s++) {
        if (s != m->defender && m->status[s]) emit(m, LOG_GOOD, s, -1, 0, 0);
    }
    discard_table(m);
    refill(m);
    m->first_attacker = m->defender;
    m->defender = next_in(m, m->first_attacker);
    emit(m, LOG_DEFENDER_CHANGE, -1, m->defender, 0, 0);
}

/* ------------------------------- the menus ------------------------------- */

#define OPT_COVER     0
#define OPT_PASS      1
#define OPT_PICKUP    2
#define OPT_ATTACK    3
#define OPT_ROUND_END 4

typedef struct { int8_t kind; int8_t a; int8_t id; } Opt;  // a: battle or seat; id -1 = fresh

// The top-level menu — FIXED order (wire format!): for each seat ascending —
// defender: covers (battle asc; known cards in preference order, then
// fresh), pass (preference order, then fresh), pickup; attacker: attacks
// (preference order, then fresh). round_end appended once everything is
// covered. Mirrors the validate* rules, deliberately erring permissive.
static int build_top_menu(RModel *m, Opt *opts, uint32_t *weights) {
    int n_opts = 0;
#define ADD(K, A, ID, W) do { \
        if (n_opts >= MENU_CAP) { m->err = REPLAY_ECAP; return n_opts; } \
        opts[n_opts].kind = (K); \
        opts[n_opts].a = (int8_t)(A); \
        opts[n_opts].id = (int8_t)(ID); \
        uint32_t w_ = (W); \
        weights[n_opts] = w_ >= 1 ? w_ : 1; \
        n_opts++; \
    } while (0)

    int uncovered = 0;
    for (int i = 0; i < m->num_battles; i++)
        if (m->battles[i].defense < 0) uncovered++;
    bool all_covered = m->num_battles > 0 && uncovered == 0;
    int def_hand = hand_len(m, m->defender);
    bool tv[14] = { false };
    for (int i = 0; i < m->num_battles; i++) {
        tv[id_value(m->battles[i].attack)] = true;
        if (m->battles[i].defense >= 0) tv[id_value(m->battles[i].defense)] = true;
    }

    int8_t kn[52];
    int8_t scratch[52];

    for (int seat = 0; seat < m->n; seat++) {
        if (!m->status[seat]) continue;

        if (seat == m->defender) {
            if (m->num_battles == 0) continue;
            int nk = sorted_known(m, seat, kn);
            // covers (validateCover: any uncovered battle, any beating card)
            for (int b = 0; b < m->num_battles; b++) {
                if (m->battles[b].defense >= 0) continue;
                int atk = m->battles[b].attack;
                int pos = 0;
                for (int i = 0; i < nk; i++) {
                    if (beats(m, kn[i], atk))
                        ADD(OPT_COVER, b, kn[i], geo(V1_COVER, pos++));
                }
                if (m->unknown[seat] > 0
                    && unseen_matching(m, (Pred){ PRED_BEATS, 0, atk, 0 }, scratch) > 0) {
                    ADD(OPT_COVER, b, -1, V1_COVER_FRESH);
                }
            }
            // pass / perevod (validatePass: nothing covered, one rank on the
            // table, next player must cover everything incl. the passed card)
            if (uncovered == m->num_battles) {
                int v = id_value(m->battles[0].attack);
                bool one_rank = true;
                for (int i = 0; i < m->num_battles; i++)
                    if (id_value(m->battles[i].attack) != v) { one_rank = false; break; }
                int next = next_in(m, m->defender);
                if (one_rank && next != seat
                    && hand_len(m, next) >= m->num_battles + 1) {
                    int pos = 0;
                    for (int i = 0; i < nk; i++) {
                        if (id_value(kn[i]) == v)
                            ADD(OPT_PASS, 0, kn[i], geo(V1_PASS, pos++));
                    }
                    if (m->unknown[seat] > 0
                        && unseen_matching(m, (Pred){ PRED_VALUE, v, 0, 0 }, scratch) > 0) {
                        ADD(OPT_PASS, 0, -1, V1_PASS_FRESH);
                    }
                }
            }
            // pickup (validatePickup: any non-empty table)
            ADD(OPT_PICKUP, 0, 0, V1_PICKUP);
        } else {
            // attacks
            if (m->num_battles == 0) {
                // first attack of the bout: only the first attacker
                if (seat == m->first_attacker && def_hand >= 1) {
                    int nk = sorted_known(m, seat, kn);
                    int pos = 0;
                    for (int i = 0; i < nk; i++)
                        ADD(OPT_ATTACK, seat, kn[i], geo(V1_ATTACK, pos++));
                    if (m->unknown[seat] > 0 && m->unseen != 0)
                        ADD(OPT_ATTACK, seat, -1, V1_ATTACK_FRESH_LEAD);
                }
            } else if (uncovered + 1 <= def_hand) {
                int nk = sorted_known(m, seat, kn);
                int pos = 0;
                for (int i = 0; i < nk; i++) {
                    if (tv[id_value(kn[i])])
                        ADD(OPT_ATTACK, seat, kn[i], geo(V1_ATTACK, pos++));
                }
                if (m->unknown[seat] > 0
                    && unseen_matching(m, (Pred){ PRED_TABLE, 0, 0, tv }, scratch) > 0) {
                    ADD(OPT_ATTACK, seat, -1, V1_ATTACK_FRESH);
                }
            }
        }
    }
    // One decision ends the bout: once everything is covered, either someone
    // throws in (options above) or the round closes (good presses implied).
    if (all_covered) ADD(OPT_ROUND_END, 0, 0, V1_ROUND_END);
#undef ADD
    return n_opts;
}

// Continuation menus for multi-card ATTACK / PASS atoms: index 0 is always
// STOP, then known cards in preference order, then fresh (-1). Constraints
// are validated against the state at atom START, with `count` cards chosen.
static int build_attack_cont(const RModel *m, int seat, bool first_attack, int v0,
                             const bool *tv, int uncovered_before, int count,
                             int8_t *out) {
    if (uncovered_before + count + 1 > hand_len(m, m->defender)) return 0;
    int n = 0;
    int8_t kn[52];
    int nk = sorted_known(m, seat, kn);
    for (int i = 0; i < nk; i++) {
        bool ok = first_attack ? id_value(kn[i]) == v0 : tv[id_value(kn[i])];
        if (ok) out[n++] = kn[i];
    }
    Pred p = first_attack ? (Pred){ PRED_VALUE, v0, 0, 0 } : (Pred){ PRED_TABLE, 0, 0, tv };
    int8_t scratch[52];
    if (m->unknown[seat] > 0 && unseen_matching(m, p, scratch) > 0) out[n++] = -1;
    return n;
}

static int build_pass_cont(const RModel *m, int seat, int v0, int next_seat,
                           int battles_before, int count, int8_t *out) {
    if (battles_before + count + 1 > hand_len(m, next_seat)) return 0;
    int n = 0;
    int8_t kn[52];
    int nk = sorted_known(m, seat, kn);
    for (int i = 0; i < nk; i++)
        if (id_value(kn[i]) == v0) out[n++] = kn[i];
    int8_t scratch[52];
    if (m->unknown[seat] > 0
        && unseen_matching(m, (Pred){ PRED_VALUE, v0, 0, 0 }, scratch) > 0) out[n++] = -1;
    return n;
}

/* -------------------------- fresh-card identity -------------------------- */
// Hypergeometric "best of the player's u hidden cards out of the U unseen"
// weights C(U-1-j, u-1) over the preference-ordered feasible list, quantized
// to ID_QUANT, clamped >= 1. Uniform fallback when u < 2.

static int code_fresh(RModel *m, Coder *c, int seat, Pred pred, int actual_id) {
    int8_t feas[52];
    int nf = unseen_matching(m, pred, feas);
    if (nf == 0) { m->err = REPLAY_ENOFRESH; return -1; }
    int chosen = -1;
    if (c->encode) {
        for (int j = 0; j < nf; j++)
            if (feas[j] == actual_id) { chosen = j; break; }
        if (chosen < 0) { m->err = REPLAY_ENOTFEAS; return -1; }
    }
    int U = __builtin_popcountll(m->unseen);
    int u = m->unknown[seat];
    uint32_t w[52];
    if (u >= 2) {
        uint64_t scale = comb64(U - 1, u - 1) / V1_ID_QUANT + 1;
        for (int j = 0; j < nf; j++) {
            uint64_t cw = (U - 1 - j >= u - 1) ? comb64(U - 1 - j, u - 1) : 0;
            uint64_t q = cw / scale;
            w[j] = q >= 1 ? (uint32_t)q : 1;
        }
    } else {
        for (int j = 0; j < nf; j++) w[j] = 1;
    }
    int k = coder_code(c, w, nf, chosen);
    if (c->err) { m->err = c->err; return -1; }
    return feas[k];
}

/* --------------------------- format 6 helpers --------------------------- */
// Reveal ONE hidden card to `seat`, coded uniform over the unseen pool (a
// uniformly shuffled deck makes the next dealt/drawn card uniform over what is
// still unseen). The pool is enumerated in ascending id — a canonical order
// both directions reproduce. Encode pulls the real id from m->rev; decode reads
// it out of the integer. The card moves unseen -> known[seat]; unknown[] is
// never touched, so in v6 every hand is fully known and the v5 reveal /
// hypergeometric / complement machinery is simply never reached.
static int code_reveal(RModel *m, Coder *c, int seat) {
    int8_t pool[52];
    int U = 0;
    for (int id = 0; id < 52; id++)
        if ((m->unseen >> id) & 1ull) pool[U++] = (int8_t)id;  // ascending id
    if (U == 0) { m->err = REPLAY_ENOFRESH; return -1; }
    int chosen = -1;
    if (c->encode) {
        if (m->rev_pos >= m->rev_n) { m->err = REPLAY_EINPUT; return -1; }
        int real = m->rev[m->rev_pos++];
        for (int j = 0; j < U; j++) if (pool[j] == real) { chosen = j; break; }
        if (chosen < 0) { m->err = REPLAY_ENOTFEAS; return -1; }
    }
    int k = coder_uniform(c, U, chosen);
    if (c->err) { m->err = c->err; return -1; }
    int id = pool[k];
    m->unseen &= ~(1ull << id);
    m->known[seat] |= 1ull << id;
    return id;
}

// LEB128-style varint over the coder (8 bits/byte via coder_uniform(256)); used
// for the v6 atom count. Encode reads *val; decode builds it.
static void code_varint(Coder *c, uint32_t *val) {
    if (c->encode) {
        uint32_t v = *val;
        do {
            int byte = (int)(v & 0x7Fu);
            v >>= 7;
            if (v) byte |= 0x80;
            coder_uniform(c, 256, byte);
        } while (v && !c->err);
    } else {
        uint32_t v = 0;
        int shift = 0, byte;
        do {
            byte = coder_uniform(c, 256, -1);
            if (c->err) break;
            v |= (uint32_t)(byte & 0x7F) << shift;
            shift += 7;
        } while ((byte & 0x80) && shift < 35);
        *val = v;
    }
}

/* ------------------------------ info source ------------------------------ */
// Encode-side reader over the action stream, from either of two sources:
//
//   BYTES — the marshaled action bytes (replay.h format), for replay_encode /
//     replay_encode_v6. Here the player_id -> seat mapping and the round_end
//     synthesis stay caller-side.
//   LOGS  — a played game's own GameLog array, for replay_encode_v6_from_game.
//     Seats are already seats and the round_end rule is applied here, so no
//     action ever has to be marshaled into a buffer at all.
//
// Everything downstream (find_top_index, run_replay, run_replay_v6, the coder,
// the model) reads only the loaded fields, so it never learns which source it
// is on — that is the point of keeping this behind one struct.

typedef struct {
    const unsigned char *buf;   // BYTES source; NULL on the LOGS source
    const GameLog *logs;        // LOGS source; NULL on the BYTES source
    int num_logs;
    int len;
    int pos;    // read cursor: next unloaded action — a byte offset on the
                // BYTES source, a log index on the LOGS source
    int count;  // total actions
    int idx;    // current action index
    bool loaded;
    int kind;   // LOG_* or REPLAY_ROUND_END
    int seat;
    int n_pairs;
    unsigned char pairs[REPLAY_MAX_PAIRS][2];
    int adv;    // byte length of the loaded action
} Src;

static bool src_exhausted(const Src *s) { return s->idx >= s->count; }

// A Card as a 1-byte wire id (replay.h): a real card, 0xFF for CARD_NONE
// ({-2,-2}: a log pair with no target), 0xFE for the hidden card ({-1,-1}).
// The two sentinels are kept apart on purpose — a hidden PRIMARY means a masked
// log reached an encoder that needs real cards, and must fail, not encode as
// "no card".
static unsigned char rep_wire_of(Card c) {
    if (card_is_none(c)) return REPLAY_CARD_NONE;
    if (c.suit < 0 || c.value < 0) return REPLAY_CARD_HIDDEN;
    return (unsigned char)(c.suit * 13 + (c.value - 1));
}

// Is this log an atom, and which kind? The action stream is the info logs
// (attack/cover/pass/pickup) plus a round_end marker for every DISCARD directly
// preceded by a GOOD — a rule that had four independent copies (encode.ts
// collectV6, ios_api.c build_encode_input, tests/replay_difftest.c,
// tests/replay_v6_test.c). This is the kernel's copy; `i` indexes `logs`
// because the round_end rule reads the PREVIOUS log.
static int log_atom_kind(const GameLog *logs, int i) {
    int kind = logs[i].log_type;
    if (kind == LOG_ATTACK || kind == LOG_COVER
        || kind == LOG_PASS || kind == LOG_PICKUP) return kind;
    if (kind == LOG_DISCARD && i > 0 && logs[i - 1].log_type == LOG_GOOD)
        return REPLAY_ROUND_END;
    return 0;   // not an atom (GOOD, DRAW, GAME_START, a swept DISCARD, ...)
}

int replay_first_attacker_from_logs(const GameLog *logs, int num_logs) {
    for (int i = 0; i < num_logs; i++)
        if (logs[i].log_type == LOG_ATTACK) return logs[i].player_idx;
    return -1;
}

// Atoms a game's logs would produce, capped at max_atoms. v6 codes the count
// into the header before the stream runs, so it must be known up front.
static int count_atoms_from_logs(const GameLog *logs, int num_logs, int max_atoms) {
    int n = 0;
    for (int i = 0; i < num_logs && n < max_atoms; i++)
        if (log_atom_kind(logs, i)) n++;
    return n;
}

// LOGS source: scan to the next atom and load it. Validates exactly what the
// BYTES source validates, because the logs of a game that ran in THIS process
// are not automatically trustworthy input — the server's arrive over the wire
// (wasm_import_logs) and are masked, so a hidden card here is reachable and
// must be rejected rather than coded.
static int src_load_logs(Src *s, int n_seats) {
    for (int i = s->pos; i < s->num_logs; i++) {
        int kind = log_atom_kind(s->logs, i);
        if (!kind) continue;
        const GameLog *l = &s->logs[i];
        if (kind == REPLAY_ROUND_END) {
            s->kind = REPLAY_ROUND_END;
            s->seat = -1;
            s->n_pairs = 0;
        } else {
            int np = l->num_pairs;
            if (np > REPLAY_MAX_PAIRS) return REPLAY_EINPUT;
            if (l->player_idx < 0 || l->player_idx >= n_seats) return REPLAY_EINPUT;
            if (kind != LOG_PICKUP && np == 0) return REPLAY_EINPUT;
            for (int j = 0; j < np; j++) {
                unsigned char prim = rep_wire_of(l->pairs[j].primary);
                unsigned char tgt  = rep_wire_of(l->pairs[j].target);
                if (prim > 51) return REPLAY_EINPUT;
                if (tgt > 51 && tgt != REPLAY_CARD_NONE) return REPLAY_EINPUT;
                s->pairs[j][0] = prim;
                s->pairs[j][1] = tgt;
            }
            s->kind = kind;
            s->seat = l->player_idx;
            s->n_pairs = np;
        }
        s->adv = i + 1 - s->pos;   // src_advance moves the log cursor past it
        s->loaded = true;
        return 0;
    }
    // count_atoms_from_logs promised another atom and the logs disagree — only
    // reachable if the two walks ever fall out of step.
    return REPLAY_EINPUT;
}

static int src_load(Src *s, int n_seats) {
    if (s->loaded) return 0;
    if (s->logs) return src_load_logs(s, n_seats);
    const unsigned char *p = s->buf;
    int pos = s->pos;
    if (pos + 3 > s->len) return REPLAY_EINPUT;
    int kind = p[pos], seat = p[pos + 1], np = p[pos + 2];
    if (np > REPLAY_MAX_PAIRS) return REPLAY_EINPUT;
    if (pos + 3 + 2 * np > s->len) return REPLAY_EINPUT;
    if (kind == REPLAY_ROUND_END) {
        if (np != 0) return REPLAY_EINPUT;
        s->seat = -1;
    } else {
        if (kind != LOG_ATTACK && kind != LOG_COVER
            && kind != LOG_PASS && kind != LOG_PICKUP) return REPLAY_EINPUT;
        if (seat >= n_seats) return REPLAY_EINPUT;
        // info-log primaries are real cards (the engine reveals every played
        // card); an empty ATTACK/COVER/PASS could never come from the engine
        if (kind != LOG_PICKUP && np == 0) return REPLAY_EINPUT;
        s->seat = seat;
    }
    for (int i = 0; i < np; i++) {
        unsigned char prim = p[pos + 3 + 2 * i];
        unsigned char tgt = p[pos + 3 + 2 * i + 1];
        if (prim > 51) return REPLAY_EINPUT;
        if (tgt > 51 && tgt != REPLAY_CARD_NONE) return REPLAY_EINPUT;
        s->pairs[i][0] = prim;
        s->pairs[i][1] = tgt;
    }
    s->kind = kind;
    s->n_pairs = np;
    s->adv = 3 + 2 * np;
    s->loaded = true;
    return 0;
}

static void src_advance(Src *s) {
    s->pos += s->adv;
    s->idx++;
    s->loaded = false;
}

static int find_top_index(RModel *m, const Opt *opts, int n_opts, const Src *s) {
    if (s->kind == REPLAY_ROUND_END) {
        for (int i = 0; i < n_opts; i++)
            if (opts[i].kind == OPT_ROUND_END) return i;
        m->err = REPLAY_EROUNDEND;
        return -1;
    }
    switch (s->kind) {
        case LOG_ATTACK: {
            int id0 = s->pairs[0][0];
            bool want_known = (m->known[s->seat] >> id0) & 1ull;
            for (int i = 0; i < n_opts; i++) {
                const Opt *o = &opts[i];
                if (o->kind != OPT_ATTACK || o->a != s->seat) continue;
                if (want_known ? o->id == id0 : o->id < 0) return i;
            }
            break;
        }
        case LOG_COVER: {
            if (s->pairs[0][1] == REPLAY_CARD_NONE) { m->err = REPLAY_EINPUT; return -1; }
            int target = s->pairs[0][1];
            // executeCover targets the first uncovered battle holding this card
            int b_idx = -1;
            for (int b = 0; b < m->num_battles; b++) {
                if (m->battles[b].defense < 0 && m->battles[b].attack == target) {
                    b_idx = b;
                    break;
                }
            }
            int cov = s->pairs[0][0];
            bool want_known = (m->known[m->defender] >> cov) & 1ull;
            for (int i = 0; i < n_opts; i++) {
                const Opt *o = &opts[i];
                if (o->kind != OPT_COVER || o->a != b_idx) continue;
                if (want_known ? o->id == cov : o->id < 0) return i;
            }
            break;
        }
        case LOG_PASS: {
            int id0 = s->pairs[0][0];
            bool want_known = (m->known[m->defender] >> id0) & 1ull;
            for (int i = 0; i < n_opts; i++) {
                const Opt *o = &opts[i];
                if (o->kind != OPT_PASS) continue;
                if (want_known ? o->id == id0 : o->id < 0) return i;
            }
            break;
        }
        case LOG_PICKUP:
            for (int i = 0; i < n_opts; i++)
                if (opts[i].kind == OPT_PICKUP) return i;
            break;
    }
    m->err = REPLAY_ENOTINMENU;
    g_err_detail = (s->kind << 16) | (n_opts & 0xFFFF);
    return -1;
}

/* ------------------------------- the atoms ------------------------------- */

static void atom_attack(RModel *m, Coder *c, const Src *s, Opt opt) {
    int seat = opt.a;
    bool first_attack = m->num_battles == 0;
    int uncovered_before = 0;
    for (int i = 0; i < m->num_battles; i++)
        if (m->battles[i].defense < 0) uncovered_before++;
    bool tv[14] = { false };
    for (int i = 0; i < m->num_battles; i++) {
        tv[id_value(m->battles[i].attack)] = true;
        if (m->battles[i].defense >= 0) tv[id_value(m->battles[i].defense)] = true;
    }
    int8_t ids[REPLAY_MAX_PAIRS];
    int n_ids = 0;
    int id0;
    if (opt.id >= 0) {
        id0 = opt.id;
        take_known(m, seat, id0);
    } else {
        Pred p = first_attack ? (Pred){ PRED_ANY, 0, 0, 0 } : (Pred){ PRED_TABLE, 0, 0, tv };
        id0 = code_fresh(m, c, seat, p, s ? s->pairs[0][0] : -1);
        if (m->err) return;
        take_fresh(m, seat, id0);
    }
    if (m->err) return;
    ids[n_ids++] = (int8_t)id0;
    int v0 = id_value(id0);
    // continuation: 0=stop, then more cards (same rank on a first attack,
    // any table rank otherwise)
    for (;;) {
        int8_t cont[CONT_CAP];
        int n_cont = build_attack_cont(m, seat, first_attack, v0, tv,
                                       uncovered_before, n_ids, cont);
        int cont_chosen = -1;
        if (s) {
            if (n_ids < s->n_pairs) {
                int next_card = s->pairs[n_ids][0];
                bool want_known = (m->known[seat] >> next_card) & 1ull;
                int found = -1;
                for (int j = 0; j < n_cont; j++) {
                    if (want_known ? cont[j] == next_card : cont[j] < 0) { found = j; break; }
                }
                cont_chosen = 1 + found;
                if (cont_chosen == 0) { m->err = REPLAY_EATTCONT; return; }
            } else {
                cont_chosen = 0;
            }
        }
        uint32_t cw[CONT_CAP + 1];
        cw[0] = V1_STOP;
        for (int j = 0; j < n_cont; j++) cw[1 + j] = 1;
        int k = coder_code(c, cw, 1 + n_cont, cont_chosen);
        if (c->err) { m->err = c->err; return; }
        if (k == 0) break;
        int pick = cont[k - 1];
        int id;
        if (pick >= 0) {
            id = pick;
            take_known(m, seat, id);
        } else {
            Pred p = first_attack ? (Pred){ PRED_VALUE, v0, 0, 0 } : (Pred){ PRED_TABLE, 0, 0, tv };
            id = code_fresh(m, c, seat, p, s ? s->pairs[n_ids][0] : -1);
            if (m->err) return;
            take_fresh(m, seat, id);
        }
        if (m->err) return;
        if (n_ids >= REPLAY_MAX_PAIRS) { m->err = REPLAY_ECAP; return; }
        ids[n_ids++] = (int8_t)id;
    }
    apply_attack(m, seat, ids, n_ids);
}

static void atom_cover(RModel *m, Coder *c, const Src *s, Opt opt) {
    int b = opt.a;
    int atk = m->battles[b].attack;
    int cover_id;
    if (opt.id >= 0) {
        cover_id = opt.id;
        take_known(m, m->defender, cover_id);
    } else {
        cover_id = code_fresh(m, c, m->defender, (Pred){ PRED_BEATS, 0, atk, 0 },
                              s ? s->pairs[0][0] : -1);
        if (m->err) return;
        take_fresh(m, m->defender, cover_id);
    }
    if (m->err) return;
    apply_cover(m, b, cover_id);
}

static void atom_pass(RModel *m, Coder *c, const Src *s, Opt opt) {
    int seat = m->defender;
    int v0 = id_value(m->battles[0].attack);
    int battles_before = m->num_battles;
    int next_seat = next_in(m, m->defender);
    int8_t ids[REPLAY_MAX_PAIRS];
    int n_ids = 0;
    int id0;
    if (opt.id >= 0) {
        id0 = opt.id;
        take_known(m, seat, id0);
    } else {
        id0 = code_fresh(m, c, seat, (Pred){ PRED_VALUE, v0, 0, 0 },
                         s ? s->pairs[0][0] : -1);
        if (m->err) return;
        take_fresh(m, seat, id0);
    }
    if (m->err) return;
    ids[n_ids++] = (int8_t)id0;
    for (;;) {
        int8_t cont[CONT_CAP];
        int n_cont = build_pass_cont(m, seat, v0, next_seat, battles_before, n_ids, cont);
        int cont_chosen = -1;
        if (s) {
            if (n_ids < s->n_pairs) {
                int next_card = s->pairs[n_ids][0];
                bool want_known = (m->known[seat] >> next_card) & 1ull;
                int found = -1;
                for (int j = 0; j < n_cont; j++) {
                    if (want_known ? cont[j] == next_card : cont[j] < 0) { found = j; break; }
                }
                cont_chosen = 1 + found;
                if (cont_chosen == 0) { m->err = REPLAY_EPASSCONT; return; }
            } else {
                cont_chosen = 0;
            }
        }
        uint32_t cw[CONT_CAP + 1];
        cw[0] = V1_STOP;
        for (int j = 0; j < n_cont; j++) cw[1 + j] = 1;
        int k = coder_code(c, cw, 1 + n_cont, cont_chosen);
        if (c->err) { m->err = c->err; return; }
        if (k == 0) break;
        int pick = cont[k - 1];
        int id;
        if (pick >= 0) {
            id = pick;
            take_known(m, seat, id);
        } else {
            id = code_fresh(m, c, seat, (Pred){ PRED_VALUE, v0, 0, 0 },
                            s ? s->pairs[n_ids][0] : -1);
            if (m->err) return;
            take_fresh(m, seat, id);
        }
        if (m->err) return;
        if (n_ids >= REPLAY_MAX_PAIRS) { m->err = REPLAY_ECAP; return; }
        ids[n_ids++] = (int8_t)id;
    }
    apply_pass(m, seat, ids, n_ids);
}

/* --------------------------- the shared driver --------------------------- */
// One function runs both directions. In encode mode `s` supplies the actual
// game (the info-bearing actions); in decode mode choices come back out of
// the integer. This symmetry is the round-trip guarantee.

static void model_init(RModel *m, int n, int trump_id, int first_attacker,
                       unsigned char *out, int out_pos, int out_cap, int format) {
    memset(m, 0, sizeof *m);
    m->n = n;
    m->trump_id = trump_id;
    m->power_suit = trump_id / 13;
    m->format = format;
    int min_v = min_value_for(n);  // THE deck rule — card.h, shared with game.c
    int deck_size = 4 * (ACE_VALUE - min_v + 1);
    for (int s = 0; s < n; s++) {
        m->status[s] = true;
        // v6 deals the initial hands explicitly (code_reveal), so they start
        // empty and are filled before the first atom; v5 leaves them hidden.
        m->unknown[s] = (format == 6) ? 0 : CARDS_PER_PLAYER;
    }
    m->deck_count = deck_size - n * CARDS_PER_PLAYER - 1;
    m->flipped_held = true;
    m->first_attacker = first_attacker;
    m->defender = (first_attacker + 1) % n;  // set_positions()
    for (int suit = 0; suit < 4; suit++) {
        for (int v = min_v; v <= ACE_VALUE; v++) {
            int id = suit * 13 + (v - 1);
            if (id != trump_id) m->unseen |= 1ull << id;
        }
    }
    m->out = out;
    m->out_pos = out_pos;
    m->out_cap = out_cap;
    emit(m, LOG_GAME_START, -1, -1, 0, 0);
}

static Opt g_opts[MENU_CAP];
static uint32_t g_weights[MENU_CAP];

static void run_replay(RModel *m, Coder *c, Src *s) {
    int atoms = 0;
    while (in_count(m) > 1) {
        if (++atoms > REPLAY_MAX_ATOMS) { m->err = REPLAY_EATOMS; return; }
        check_conservation(m);
        if (m->err) return;

        int n_opts = build_top_menu(m, g_opts, g_weights);
        if (m->err) return;
        if (n_opts == 0) { m->err = REPLAY_ENOMOVES; return; }

        int chosen = -1;
        if (s) {
            if (src_exhausted(s)) { m->err = REPLAY_EINCOMPLETE; return; }
            int perr = src_load(s, m->n);
            if (perr) { m->err = perr; return; }
            chosen = find_top_index(m, g_opts, n_opts, s);
            if (m->err) return;
        }
        int k = coder_code(c, g_weights, n_opts, chosen);
        if (c->err) { m->err = c->err; return; }
        Opt opt = g_opts[k];

        switch (opt.kind) {
            case OPT_ATTACK:    atom_attack(m, c, s, opt); break;
            case OPT_COVER:     atom_cover(m, c, s, opt); break;
            case OPT_PASS:      atom_pass(m, c, s, opt); break;
            case OPT_PICKUP:    apply_pickup(m); break;
            case OPT_ROUND_END: apply_round_end(m); break;
        }
        if (m->err) return;
        if (c->err) { m->err = c->err; return; }

        if (s) src_advance(s);
    }
    if (s && !src_exhausted(s)) m->err = REPLAY_ELOGSAFTER;
}

// Format 6 driver. Deals every seat's real initial hand (coded reveals, emitted
// as one LOG_DRAW per seat), then runs exactly `n_atoms` top-level decisions —
// so the stream may stop MID-GAME. Hidden cards are coded as they are dealt or
// drawn (draw_for reaches the coder via m->rev_coder), so no reveal is ever
// deferred and the decoded stream is fully identity-resolved. Shares every menu,
// weight, atom and cascade with v5 — only card *revelation* moves earlier.
// Deal one seat's CARDS_PER_PLAYER-card initial hand. A hand is a SET, so we
// code it as an ASCENDING combination — each card uniform over the unseen cards
// with a strictly larger id than the last — which spends no bits on within-hand
// order (v5's ~log2(6!) per hand of slack). Draws stay single-card ordered
// reveals (a draw is genuinely time-ordered). Emitted as one LOG_DRAW per seat.
static void deal_hand_v6(RModel *m, Coder *c, int seat) {
    int real[CARDS_PER_PLAYER];
    if (c->encode) {
        for (int k = 0; k < CARDS_PER_PLAYER; k++) {
            if (m->rev_pos >= m->rev_n) { m->err = REPLAY_EINPUT; return; }
            real[k] = m->rev[m->rev_pos++];
        }
        for (int i = 1; i < CARDS_PER_PLAYER; i++) {  // insertion sort ascending
            int v = real[i], j = i - 1;
            while (j >= 0 && real[j] > v) { real[j + 1] = real[j]; j--; }
            real[j + 1] = v;
        }
    }
    unsigned char pairs[CARDS_PER_PLAYER][2];
    int lo = -1;
    for (int k = 0; k < CARDS_PER_PLAYER; k++) {
        int8_t pool[52];
        int U = 0;
        for (int id = lo + 1; id < 52; id++)
            if ((m->unseen >> id) & 1ull) pool[U++] = (int8_t)id;  // ascending, > lo
        if (U == 0) { m->err = REPLAY_ENOFRESH; return; }
        int chosen = -1;
        if (c->encode) {
            for (int j = 0; j < U; j++) if (pool[j] == real[k]) { chosen = j; break; }
            if (chosen < 0) { m->err = REPLAY_ENOTFEAS; return; }
        }
        int kk = coder_uniform(c, U, chosen);
        if (c->err) { m->err = c->err; return; }
        int id = pool[kk];
        m->unseen &= ~(1ull << id);
        m->known[seat] |= 1ull << id;
        lo = id;
        pairs[k][0] = (unsigned char)id;
        pairs[k][1] = REPLAY_CARD_NONE;
    }
    emit(m, LOG_DRAW, seat, -1, pairs, CARDS_PER_PLAYER);
    if (g_atom_sink) {
        int wide[CARDS_PER_PLAYER];
        for (int k = 0; k < CARDS_PER_PLAYER; k++) wide[k] = pairs[k][0];
        atom_out(REPLAY_ATOM_DEAL, seat, wide, CARDS_PER_PLAYER, -1);
    }
}

static void run_replay_v6(RModel *m, Coder *c, Src *s, uint32_t n_atoms) {
    m->rev_coder = c;
    // Initial deal: seat-major, CARDS_PER_PLAYER real cards each.
    for (int seat = 0; seat < m->n; seat++) {
        deal_hand_v6(m, c, seat);
        if (m->err) return;
    }

    for (uint32_t a = 0; a < n_atoms; a++) {
        if (in_count(m) <= 1) { m->err = REPLAY_ELOGSAFTER; return; }
        check_conservation(m);
        if (m->err) return;

        int n_opts = build_top_menu(m, g_opts, g_weights);
        if (m->err) return;
        if (n_opts == 0) { m->err = REPLAY_ENOMOVES; return; }

        int chosen = -1;
        if (s) {
            if (src_exhausted(s)) { m->err = REPLAY_EINCOMPLETE; return; }
            int perr = src_load(s, m->n);
            if (perr) { m->err = perr; return; }
            chosen = find_top_index(m, g_opts, n_opts, s);
            if (m->err) return;
        }
        int k = coder_code(c, g_weights, n_opts, chosen);
        if (c->err) { m->err = c->err; return; }
        Opt opt = g_opts[k];

        switch (opt.kind) {
            case OPT_ATTACK:    atom_attack(m, c, s, opt); break;
            case OPT_COVER:     atom_cover(m, c, s, opt); break;
            case OPT_PASS:      atom_pass(m, c, s, opt); break;
            case OPT_PICKUP:    apply_pickup(m); break;
            case OPT_ROUND_END: apply_round_end(m); break;
        }
        if (m->err) return;
        if (c->err) { m->err = c->err; return; }

        if (s) src_advance(s);
    }
    if (s && !src_exhausted(s)) m->err = REPLAY_ELOGSAFTER;
}

/* ------------------------- header & entry points ------------------------- */

// The flipped card is redrawn while it is an ace (start_game), so aces are
// not in the trump alphabet.
static int trump_alphabet(int n, int8_t *out) {
    int min_v = min_value_for(n);
    int cnt = 0;
    for (int suit = 0; suit < 4; suit++)
        for (int v = min_v; v < ACE_VALUE; v++) out[cnt++] = (int8_t)(suit * 13 + (v - 1));
    return cnt;
}

int replay_encode(const unsigned char *in, int in_len,
                  unsigned char *out, int out_cap) {
    g_err_detail = 0;
    comb_init();
    if (in_len < 5) return -REPLAY_EINPUT;
    int n = in[0], trump_id = in[1], fa = in[2];
    if (n < 2 || n > MAX_PLAYERS) return -REPLAY_EINPUT;
    if (trump_id > 51) return -REPLAY_EINPUT;
    if (fa >= n) return -REPLAY_EINPUT;
    int n_actions = in[3] | (in[4] << 8);

    Src s;
    memset(&s, 0, sizeof s);
    s.buf = in;
    s.len = in_len;
    s.pos = 5;
    s.count = n_actions;

    Coder c;
    memset(&c, 0, sizeof c);
    c.encode = true;

    coder_uniform(&c, REPLAY_VERSION_ALPHABET, REPLAY_FORMAT_VERSION);
    coder_uniform(&c, 7, n - 2);
    int8_t alpha[48];
    int alen = trump_alphabet(n, alpha);
    int t = -1;
    for (int i = 0; i < alen; i++)
        if (alpha[i] == trump_id) { t = i; break; }
    if (t < 0) return -REPLAY_EHEADER;  // trump not in alphabet (incl. aces)
    coder_uniform(&c, alen, t);
    coder_uniform(&c, n, fa);
    if (c.err) return -c.err;

    RModel *m = &g_model;
    model_init(m, n, trump_id, fa, 0, 0, 0, REPLAY_FORMAT_VERSION);
    run_replay(m, &c, &s);
    if (m->err) return -m->err;
    if (c.err) return -c.err;

    if (!coder_finish(&c, &g_bn)) return -REPLAY_ECAP;
    int len = bn_to_bytes_be(&g_bn, out, out_cap);
    if (len < 0) return -REPLAY_ECAP;
    return len;
}

// The v6 header + run, shared by both producers. Everything above this differs
// only in where the actions and the reveals came from; from here down there is
// one v6 encoder, so the two entry points cannot drift on the wire format.
static int encode_v6_run(int n, int trump_id, int fa, int n_actions,
                         const unsigned char *reveals, int n_reveals,
                         Src *s, unsigned char *out, int out_cap) {
    Coder c;
    memset(&c, 0, sizeof c);
    c.encode = true;

    coder_uniform(&c, REPLAY_VERSION_ALPHABET, REPLAY_FORMAT_VERSION_V6);
    coder_uniform(&c, 7, n - 2);
    int8_t alpha[48];
    int alen = trump_alphabet(n, alpha);
    int t = -1;
    for (int i = 0; i < alen; i++)
        if (alpha[i] == trump_id) { t = i; break; }
    if (t < 0) return -REPLAY_EHEADER;  // trump not in alphabet (incl. aces)
    coder_uniform(&c, alen, t);
    coder_uniform(&c, n, fa);
    uint32_t atoms = (uint32_t)n_actions;
    code_varint(&c, &atoms);
    if (c.err) return -c.err;

    RModel *m = &g_model;
    model_init(m, n, trump_id, fa, 0, 0, 0, REPLAY_FORMAT_VERSION_V6);
    m->rev = reveals;
    m->rev_n = n_reveals;
    m->rev_pos = 0;
    run_replay_v6(m, &c, s, atoms);
    if (m->err) return -m->err;
    if (c.err) return -c.err;

    if (!coder_finish(&c, &g_bn)) return -REPLAY_ECAP;
    int len = bn_to_bytes_be(&g_bn, out, out_cap);
    if (len < 0) return -REPLAY_ECAP;
    return len;
}

int replay_encode_v6(const unsigned char *in, int in_len,
                     unsigned char *out, int out_cap) {
    g_err_detail = 0;
    comb_init();
    if (in_len < 7) return -REPLAY_EINPUT;
    int n = in[0], trump_id = in[1], fa = in[2];
    if (n < 2 || n > MAX_PLAYERS) return -REPLAY_EINPUT;
    if (trump_id > 51) return -REPLAY_EINPUT;
    if (fa >= n) return -REPLAY_EINPUT;
    int n_actions = in[3] | (in[4] << 8);
    int n_reveals = in[5] | (in[6] << 8);
    int rev_off = 7;
    if (rev_off + n_reveals > in_len) return -REPLAY_EINPUT;
    for (int i = 0; i < n_reveals; i++)
        if (in[rev_off + i] > 51) return -REPLAY_EINPUT;

    Src s;
    memset(&s, 0, sizeof s);
    s.buf = in;
    s.len = in_len;
    s.pos = rev_off + n_reveals;
    s.count = n_actions;

    return encode_v6_run(n, trump_id, fa, n_actions,
                         in + rev_off, n_reveals, &s, out, out_cap);
}

/* ------------------- v6 from a played game: the re-deal ------------------- */

// The re-deal needs a Game to deal INTO, but only its hands, deck and flip —
// never its logs. A full Game is ~130 KB in the production build (MAX_LOGS x
// MAX_LOG_PAIRS), which is not worth a second static in a wasm module that
// already carries one, so this slot stops just past the start of the logs
// array: the same short-log trick cordite_sim.c plays for its sampled worlds
// (WORLD_SLOT_BYTES). log_cap = 1 then makes log_alloc route every non-DISCARD
// append — during a deal, all of them — to its own sink instead of the array.
// Plain statics, like the coder and model above: this codec is single-threaded
// by design.
#define DEAL_SLOT_BYTES (offsetof(Game, logs) + sizeof(GameLog))
typedef struct { _Alignas(16) unsigned char bytes[DEAL_SLOT_BYTES]; } DealSlot;
static DealSlot g_deal_slot;

// Re-derive a seeded game's deal: the true initial hands (seat-major) followed
// by the whole remaining stock in draw order, which is the reveal stream v6
// wants, plus the trump. The flip is deliberately absent from the stock —
// start_game draws it out — and v6 never lists it, because it IS the header
// trump. Passing the whole stock rather than only the cards this game went on
// to draw is what the TS producer did too, and costs nothing: run_replay_v6
// pops reveals as draws happen and never reads the tail.
static int deal_reveals_from_seed(const unsigned char *seed, int seed_len, int n,
                                  unsigned char *reveals, int *out_nr, int *out_trump) {
    Game *d = (Game *)g_deal_slot.bytes;
    memset(d, 0, sizeof g_deal_slot.bytes);
    d->num_players = (int8_t)n;
    d->log_cap = 1;   // short-log slot: this deal's logs are never read

    // A deal fires the snapshot hook (ENGINE_HOOK_DEAL per seat, plus
    // START_MAGIC and FLIPPED) and engine_snap_hook is GLOBAL, so a re-deal on
    // a host that has one installed would splice a whole imaginary deal into
    // the animation plan it is building — the exact bug the bot_drive harness
    // found in the choose path (bot_drive.c choose_move). And start_game
    // consumes the deal RNG and leaves wide mode set, which draw_index reads
    // for every game in this thread. Both are global, so both get put back.
    unsigned char saved_rng[GAME_DEAL_RNG_STATE_MAX];
    void (*saved_hook)(const Game *, int, int) = engine_snap_hook;
    game_deal_rng_get(saved_rng);
    engine_snap_hook = 0;

    game_set_deal_seed_bytes(seed, seed_len);
    start_game(d);

    engine_snap_hook = saved_hook;
    game_deal_rng_set(saved_rng);

    if (!d->has_flipped) return REPLAY_EHEADER;   // deck too small to flip
    *out_trump = d->flipped.suit * 13 + (d->flipped.value - 1);

    int nr = 0;
    for (int s = 0; s < n; s++) {
        // v6 slices the reveal stream CARDS_PER_PLAYER per seat, so a short
        // hand would silently shift every later seat's deal.
        if (d->players[s].hand_count != CARDS_PER_PLAYER) return REPLAY_EINPUT;
        for (int k = 0; k < CARDS_PER_PLAYER; k++)
            reveals[nr++] = rep_wire_of(d->players[s].hand[k]);
    }
    for (int i = 0; i < d->deck_count; i++)
        reveals[nr++] = rep_wire_of(d->deck[i]);
    for (int i = 0; i < nr; i++)
        if (reveals[i] > 51) return REPLAY_EINPUT;   // a real deal has real cards
    *out_nr = nr;
    return 0;
}

int replay_encode_v6_from_game(const Game *g, const unsigned char *seed, int seed_len,
                               int max_atoms, unsigned char *out, int out_cap) {
    g_err_detail = 0;
    comb_init();
    if (!g || !seed || seed_len < FOOLISH_SEED_LEN) return -REPLAY_EINPUT;
    if (g->num_logs >= MAX_LOGS) return -REPLAY_EINPUT;  // overflowed → untrusted
    int n = g->num_players;
    if (n < 2 || n > MAX_PLAYERS) return -REPLAY_EINPUT;
    if (max_atoms <= 0) return -REPLAY_EINPUT;

    int fa = replay_first_attacker_from_logs(g->logs, g->num_logs);
    if (fa < 0 || fa >= n) return -REPLAY_EINPUT;   // no attack logged → nothing to encode

    unsigned char reveals[MAX_PLAYERS * CARDS_PER_PLAYER + MAX_DECK];
    int n_reveals = 0, trump_id = 0;
    int rc = deal_reveals_from_seed(seed, seed_len, n, reveals, &n_reveals, &trump_id);
    if (rc) return -rc;

    // Did this seed actually deal this game? Check it against the trump while
    // the game still carries one. ONLY while has_flipped: once the trump has
    // been drawn, `flipped` is not a reliable witness. This kernel keeps the
    // card and only clears the flag, but a game that has round-tripped through
    // the server's state blob does NOT — engine.ts marshals a drawn-out trump as
    // wire card 0 (marshalGame: `game.flipped ? wireStateCard(...) : 0`), which
    // reads back as a real 6, not a sentinel. There is no way to tell that apart
    // from a genuine 6 here, so trust the flag, not the card.
    //
    // This is a cheap early guard, not the whole defense: a seed that did not
    // deal `g` mainly fails downstream, where the logged opening attack is not
    // in the menu the wrong deal produces (REPLAY_ENOTINMENU — the v6 test's
    // wrong-seed case takes exactly that path on a dry-stock game).
    if (g->has_flipped && rep_wire_of(g->flipped) != (unsigned char)trump_id)
        return -REPLAY_EHEADER;

    int n_actions = count_atoms_from_logs(g->logs, g->num_logs, max_atoms);
    if (n_actions <= 0) return -REPLAY_EINPUT;

    Src s;
    memset(&s, 0, sizeof s);
    s.logs = g->logs;
    s.num_logs = g->num_logs;
    s.count = n_actions;

    return encode_v6_run(n, trump_id, fa, n_actions,
                         reveals, n_reveals, &s, out, out_cap);
}

// The shared decode. `out` NULL = decode for the atoms alone: the RModel's
// emit() already no-ops without a buffer, so the atom path costs no log
// memory (a 2 MB scratch, the size decode's callers really pass, is not
// something to hand a phone or spend a wasm page budget on).
// `hdr` optional. Returns bytes written to `out` (0 when out is NULL) or
// -REPLAY_E*.
static int decode_impl(const unsigned char *in, int in_len,
                       unsigned char *out, int out_cap, ReplayHeader *hdr) {
    g_err_detail = 0;
    comb_init();
    if (in_len < 0 || in_len > REPLAY_MAX_INT_BYTES) return -REPLAY_ECAP;
    if (out && out_cap < REPLAY_DEC_HDR) return -REPLAY_ECAP;
    if (!bn_from_bytes_be(&g_bn, in, in_len)) return -REPLAY_ECAP;

    Coder c;
    memset(&c, 0, sizeof c);
    c.encode = false;
    c.x = &g_bn;

    int version = coder_uniform(&c, REPLAY_VERSION_ALPHABET, -1);
    if (version != REPLAY_FORMAT_VERSION && version != REPLAY_FORMAT_VERSION_V6) {
        g_err_detail = version;
        return -REPLAY_EVERSION;
    }
    int n = coder_uniform(&c, 7, -1) + 2;
    int8_t alpha[48];
    int alen = trump_alphabet(n, alpha);
    int trump_id = alpha[coder_uniform(&c, alen, -1)];
    int first_attacker = coder_uniform(&c, n, -1);
    if (c.err) return -c.err;

    RModel *m = &g_model;
    model_init(m, n, trump_id, first_attacker, out, REPLAY_DEC_HDR,
               out ? out_cap : 0, version);
    if (version == REPLAY_FORMAT_VERSION_V6) {
        uint32_t atoms = 0;
        code_varint(&c, &atoms);
        if (c.err) return -c.err;
        run_replay_v6(m, &c, 0, atoms);
    } else {
        run_replay(m, &c, 0);
    }
    if (m->err) return -m->err;
    if (!bn_is_zero(&g_bn)) return -REPLAY_ELEFTOVER;
    // v5 requires a single fool; v6 may legitimately be a mid-game cut where
    // >1 players are still IN — then there is no fool yet (out[4] = 0xFF).
    int fool = -1;
    if (in_count(m) == 1) {
        for (int seat = 0; seat < n; seat++)
            if (m->status[seat]) fool = seat;
    } else if (version == REPLAY_FORMAT_VERSION) {
        return -REPLAY_ENOFOOL;
    }

    if (hdr) {
        hdr->version = version;
        hdr->n = n;
        hdr->trump_id = trump_id;
        hdr->first_attacker = first_attacker;
        hdr->fool = fool;
        hdr->discard_count = m->discard;
        hdr->num_eliminated = m->num_elim;
        for (int i = 0; i < MAX_PLAYERS; i++)
            hdr->elim[i] = i < m->num_elim ? m->elim[i] : -1;
    }
    if (!out) return 0;

    out[0] = (unsigned char)version;
    out[1] = (unsigned char)n;
    out[2] = (unsigned char)trump_id;
    out[3] = (unsigned char)first_attacker;
    out[4] = (unsigned char)fool;
    out[5] = (unsigned char)(m->discard & 0xff);
    out[6] = (unsigned char)((m->discard >> 8) & 0xff);
    out[7] = (unsigned char)m->num_elim;
    for (int i = 0; i < MAX_PLAYERS; i++)
        out[8 + i] = i < m->num_elim ? (unsigned char)m->elim[i] : 0xFF;
    out[16] = (unsigned char)(m->out_logs & 0xff);
    out[17] = (unsigned char)((m->out_logs >> 8) & 0xff);
    out[18] = (unsigned char)((m->out_logs >> 16) & 0xff);
    out[19] = (unsigned char)((m->out_logs >> 24) & 0xff);
    return m->out_pos;
}

int replay_decode(const unsigned char *in, int in_len,
                  unsigned char *out, int out_cap) {
    return decode_impl(in, in_len, out, out_cap, 0);
}

// The atoms, not the logs — see replay.h. Same decode, same model (the menus
// ARE the coder's probability model, so the walk is unavoidable either way);
// only the reporting differs.
int replay_decode_atoms_v6(const unsigned char *in, int in_len,
                           ReplayHeader *hdr, ReplayAtomSink sink, void *ctx) {
    ReplayHeader local;
    if (!hdr) hdr = &local;
    g_atom_sink = sink;
    g_atom_ctx  = ctx;
    int r = decode_impl(in, in_len, 0, 0, hdr);
    g_atom_sink = 0;
    g_atom_ctx  = 0;
    if (r < 0) return r;
    // v5 hides the deal, so its atoms are not a deck and cannot rebuild a Game.
    if (hdr->version != REPLAY_FORMAT_VERSION_V6) {
        g_err_detail = hdr->version;
        return -REPLAY_EVERSION;
    }
    return REPLAY_EOK;
}
