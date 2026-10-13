#include "view.h"
#include "../wasm/wire.h"
#include "../../../shared/c/le_bytes.h"
#include <string.h>   // memset — the hidden-run fill in state_put (see below)

// Whether the hand of seat `seat` is visible to `viewer`.
static int hand_visible(int viewer, int seat) {
    return viewer == VIEW_UNMASKED || viewer == seat;
}

// The board, with `rules` as the rules it names. state_put names the game's
// own; the durable blob names none, because its flag byte is the rules' one
// owner in a row (state_blob_put_at below).
static int state_put_rules(const Game *g, int viewer, int rules, unsigned char *out) {
    const int unmasked = (viewer == VIEW_UNMASKED);
    unsigned char *q = out;
    *q++ = (unsigned char)g->status;
    *q++ = (unsigned char)g->num_players;
    *q++ = (unsigned char)g->power_suit;
    *q++ = (unsigned char)g->first_attacker;
    *q++ = (unsigned char)g->defender;
    le_put_u16(q, (uint16_t)g->discard_pile_length); q += 2;
    // THE FLAGS BYTE (STATE_FLAGS_AT): the flip in bit 0 and the game's RULES
    // above it. A board is a whole game only with its rules: the Oracle builds
    // the game it deliberates on from nothing but this board, and a board that
    // dropped them handed it the transfer game at every podkidnoy table
    // (docs/PODKIDNOY.md). Zero is the classic game, so every classic board is
    // the byte string it always was.
    *q++ = (unsigned char)((g->has_flipped ? STATE_FLAG_FLIPPED : 0)
                           | (unsigned)(uint8_t)rules << STATE_FLAG_RULES_SHIFT);
    // Canonical no-flip byte: after the flipped trump is drawn the kernel
    // keeps the stale card in g->flipped (gated by has_flipped) — writing it
    // would make byte-equal states serialize differently depending on
    // whether they came from a marshal round-trip (which normalizes to the
    // {0,0} placeholder -> WIRE_CARD_HIDDEN) or stayed kernel-resident.
    // Decoders ignore this byte when has_flipped is 0.
    *q++ = g->has_flipped ? wire_from_card(g->flipped) : (unsigned char)WIRE_CARD_HIDDEN;
    le_put_u32(q, g->good_players_mask); q += 4;
    *q++ = (unsigned char)(g->has_good_timestamp ? 1 : 0);
    le_put_u16(q, (uint16_t)g->deck_count); q += 2;
    // Hoist the loop-invariant `unmasked` test out of the per-card loop. A
    // masked view (every /ws push) hides the ENTIRE deck, so that collapses to
    // one vectorized memset of WIRE_CARD_HIDDEN instead of a branch + byte
    // store per card. Output is byte-for-byte identical either way. state_put
    // is the server's hottest own-code once bot-thread churn is gone
    // (PROFILE_HOTPATH.md's assembly capture: view.c:34-35 / 49-50).
    if (unmasked) {
        for (int i = 0; i < g->deck_count; i++) *q++ = wire_from_card(g->deck[i]);
    } else if (g->deck_count > 0) {
        memset(q, (unsigned char)WIRE_CARD_HIDDEN, (size_t)g->deck_count);
        q += g->deck_count;
    }
    *q++ = (unsigned char)g->num_battles;
    for (int i = 0; i < g->num_battles; i++) {
        const Battle *b = &g->table_battles[i];
        *q++ = wire_from_card(b->attack);
        *q++ = wire_from_card(b->defense);
    }
    for (int i = 0; i < g->num_players; i++) {
        const Player *pl = &g->players[i];
        const int visible = hand_visible(viewer, i);
        *q++ = (unsigned char)pl->status;
        *q++ = (unsigned char)((visible && pl->awaiting_attack) ? 1 : 0);
        *q++ = (unsigned char)pl->hand_count;
        // Same hoist as the deck loop: a hidden hand (every seat except the
        // viewer, on a masked push) is one memset instead of a per-card branch.
        if (visible) {
            for (int j = 0; j < pl->hand_count; j++) *q++ = wire_from_card(pl->hand[j]);
        } else if (pl->hand_count > 0) {
            memset(q, (unsigned char)WIRE_CARD_HIDDEN, (size_t)pl->hand_count);
            q += pl->hand_count;
        }
    }
    *q++ = (unsigned char)g->num_eliminated;
    for (int i = 0; i < g->num_eliminated; i++) *q++ = (unsigned char)g->elimination_order[i];
    return (int)(q - out);
}

int state_put(const Game *g, int viewer, unsigned char *out) {
    return state_put_rules(g, viewer, g->rules, out);
}

// A state card byte, decoded EXACTLY: 0..51 is that card, and every other byte
// becomes the {-1,-1} not-a-card, which game_validate refuses. (It used to
// clamp onto the ace of diamonds, which turned a corrupt byte into a real card
// the state could then be played on.)
static Card card_from_wire_exact(unsigned char b) {
    if (b > 51) { Card c; c.suit = -1; c.value = -1; return c; }
    return card_of_id(b);
}

// Masked decode: WIRE_CARD_HIDDEN becomes the {0,1} placeholder the browser
// marshal always used for redacted cards; everything else decodes exactly.
static Card card_from_wire_masked(unsigned char b) {
    if (b == WIRE_CARD_HIDDEN) { Card c; c.suit = 0; c.value = 1; return c; }
    return card_from_wire_exact(b);
}

int state_get(Game *g, const unsigned char *p, int len, int masked) {
    // MEASURE FIRST, always. The decode below walks the buffer by the counts
    // the CONTENT carries, so without this it reads as far as the content says
    // and not one byte less - up to ~755 bytes past a truncated blob. Every
    // count is checked here, against the caller's real byte count, before any
    // of them is used to advance: the payload must be exactly `len` bytes.
    // `len` is a byte count, not a capacity; handing this a buffer size is the
    // original bug wearing a parameter.
    if (state_measure(p, len) != len) return GAME_INVALID_COUNT;
    const unsigned char *q = p;
    int clamped = 0;
    // No full-struct memset: the Game is ~200 KB (mostly log capacity) and
    // every read in the kernel is bounded by the counts set below. Every
    // count is also clamped to its array capacity - the same set state_measure
    // already refused, kept as defense in depth (see
    // docs/SECURITY_WASM_BOUNDARY.md). A clamp is also reported: past a clamped
    // count the bytes no longer line up with the fields, so state_import
    // refuses the whole state.
#define CLAMP(v, hi) do { if ((v) < 0) { (v) = 0; clamped = 1; } \
                          if ((v) > (hi)) { (v) = (hi); clamped = 1; } } while (0)
    g->status = (int8_t)*q++;
    g->num_players = (int8_t)*q++;
    CLAMP(g->num_players, MAX_PLAYERS);
    g->power_suit = (int8_t)*q++;
    g->first_attacker = (int8_t)*q++;
    g->defender = (int8_t)*q++;
    g->discard_pile_length = (int16_t)le_get_u16(q); q += 2;
    // The flags byte: the flip, and the rules the board names - set here, so
    // whatever the game held before never survives a board (zero is the
    // classic game). A bit this kernel does not know is game_validate's to
    // refuse (GAME_INVALID_RULES).
    g->has_flipped = (*q & STATE_FLAG_FLIPPED) != 0;
    g->rules = (int8_t)(*q++ >> STATE_FLAG_RULES_SHIFT);
    // When there is no flip (TS flipped === null), preserve the exact {0,0}
    // bytes the old 2-byte wire left in g->flipped - cordite-lineage belief
    // code reads it unguarded and {0,0} acts as a harmless never-matches pin.
    {
        unsigned char fw = *q++;
        if (g->has_flipped) g->flipped = card_from_wire_exact(fw);
        else { g->flipped.suit = 0; g->flipped.value = 0; }
    }
    g->good_players_mask = le_get_u32(q); q += 4;
    g->has_good_timestamp = (*q++ != 0);
    g->deck_count = (int16_t)le_get_u16(q); q += 2;
    CLAMP(g->deck_count, MAX_DECK);
    for (int i = 0; i < g->deck_count; i++) {
        unsigned char b = *q++;
        g->deck[i] = masked ? card_from_wire_masked(b) : card_from_wire_exact(b);
    }
    // Read as a byte, not an int8: MAX_BATTLES is 64 in wasm, so 128..255 must
    // clamp high rather than wrap negative.
    {
        int nb = *q++;
        CLAMP(nb, MAX_BATTLES);
        g->num_battles = (int8_t)nb;
    }
    for (int i = 0; i < g->num_battles; i++) {
        Battle *b = &g->table_battles[i];
        b->attack = card_from_wire_exact(*q++);
        unsigned char db = *q++;
        b->defense = (db == WIRE_CARD_NONE) ? CARD_NONE : card_from_wire_exact(db);
    }
    for (int i = 0; i < g->num_players; i++) {
        Player *pl = &g->players[i];
        pl->status = (int8_t)*q++;
        pl->awaiting_attack = (*q++ != 0);
        pl->hand_count = (int8_t)*q++;
        CLAMP(pl->hand_count, MAX_HAND_SIZE);
        for (int j = 0; j < pl->hand_count; j++) {
            unsigned char b = *q++;
            pl->hand[j] = masked ? card_from_wire_masked(b) : card_from_wire_exact(b);
        }
    }
    g->num_eliminated = (int8_t)*q++;
    CLAMP(g->num_eliminated, MAX_PLAYERS);
#undef CLAMP
    for (int i = 0; i < g->num_eliminated; i++) g->elimination_order[i] = (int8_t)*q++;
    g->num_logs = 0;
    // The resident game always has a full-size log array; only sampled-world
    // slots ever set log_cap (see game.h). Re-pin defensively per marshal.
    g->log_cap = 0;
    g->log_virt = 0;
    return clamped ? GAME_INVALID_COUNT : GAME_VALID;
}

int state_measure(const unsigned char *p, int len) {
    // status, num_players, power_suit, first_attacker, defender, u16 discard,
    // has_flipped, flipped, u32 good mask, has_good_timestamp, then u16 deck_count.
    if (!p || len < 16) return -1;
    const int np = (int8_t)p[1];
    const int deck = (int16_t)le_get_u16(p + 14);
    if (np < 0 || np > MAX_PLAYERS || deck < 0 || deck > MAX_DECK) return -1;
    int q = 16 + deck;
    if (q + 1 > len || p[q] > MAX_BATTLES) return -1;
    q += 1 + 2 * p[q];
    for (int i = 0; i < np; i++) {
        if (q + 3 > len) return -1;
        const int hand = (int8_t)p[q + 2];
        if (hand < 0 || hand > MAX_HAND_SIZE) return -1;
        q += 3 + hand;
    }
    if (q + 1 > len || p[q] > MAX_PLAYERS) return -1;
    q += 1 + p[q];
    return q > len ? -1 : q;
}

int state_import(Game *g, const unsigned char *p, int len, int masked) {
    // Decode in place over a saved copy of the state part of `g` (everything
    // ahead of the log array, which is all state_get writes and game_validate
    // reads), and put the copy back if the result is refused. A byte copy
    // rather than a scratch Game: a full Game is ~136 KB at the wasm caps, and
    // this path is on every marshal.
    // The bound is state_get's: it measures `len` before it reads anything.
    unsigned char saved[offsetof(Game, logs)];
    memcpy(saved, g, sizeof saved);
    int r = state_get(g, p, len, masked);
    if (r == GAME_VALID) r = game_validate(g, masked ? GAME_VALIDATE_MASKED : 0);
    if (r != GAME_VALID) memcpy(g, saved, sizeof saved);
    return r;
}

// A clock value as the blob stores it: u48 little-endian epoch ms
// (shared/c/le_bytes.h). Negative values (never written) clamp to 0, so
// "never" has one spelling.
static void put_clock(unsigned char *q, int64_t v) { le_put_u48(q, v > 0 ? (uint64_t)v : 0); }

int game_ratings_put(const GameRatings *r, unsigned char *out) {
    memset(out, 0, GAME_RATINGS_BYTES);
    if (!r) return GAME_RATINGS_BYTES;
    out[0] = r->n;
    for (int s = 0; s < r->n && s < MAX_PLAYERS; s++) {
        unsigned char *q = out + 1 + 6 * s;
        const uint32_t b = (uint32_t)r->before[s];
        const uint16_t d = (uint16_t)r->delta[s];
        q[0] = (unsigned char)b; q[1] = (unsigned char)(b >> 8); q[2] = (unsigned char)(b >> 16); q[3] = (unsigned char)(b >> 24);
        q[4] = (unsigned char)d; q[5] = (unsigned char)(d >> 8);
    }
    return GAME_RATINGS_BYTES;
}

bool game_ratings_get(GameRatings *r, const unsigned char *p) {
    if (p[0] > MAX_PLAYERS) return false;
    for (int i = 1 + 6 * p[0]; i < GAME_RATINGS_BYTES; i++) if (p[i]) return false;
    memset(r, 0, sizeof(*r));
    r->n = p[0];
    for (int s = 0; s < r->n; s++) {
        const unsigned char *q = p + 1 + 6 * s;
        r->before[s] = (int32_t)((uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24));
        r->delta[s] = (int16_t)(uint16_t)(q[4] | (q[5] << 8));
    }
    return true;
}

int state_blob_put_at(const Game *g, const BoardClock *clk, int format, unsigned char *out) {
    return state_blob_put_rated_at(g, clk, 0, format, out);
}

int state_blob_put_rated_at(const Game *g, const BoardClock *clk, const GameRatings *ratings, int format, unsigned char *out) {
    int flag = g->deterministic_deck ? STATE_BLOB_FLAG_DETERMINISTIC : 0;
    if (format == STATE_BLOB_FORMAT_V4 || format == STATE_BLOB_FORMAT_V5) {
        // The one rule v4 carries; a rules bit beyond it has no flag to go in.
        if (g->rules & ~GAME_RULE_NO_PASS) return STATE_BLOB_E_RULES;
        if (game_pass_allowed(g)) flag |= STATE_BLOB_FLAG_PASSING;
    } else if (g->rules != 0) {
        return STATE_BLOB_E_RULES;   // before v4 a row is the classic game, and this one is not
    }
    out[0] = (unsigned char)format;
    out[1] = (unsigned char)flag;
    // The board inside names no rules: the flag above is the row's one owner of
    // them, and a row stays the bytes every v4 reader already reads.
    const int n = STATE_BLOB_HEADER + state_put_rules(g, VIEW_UNMASKED, 0, out + STATE_BLOB_HEADER);
    if (format == STATE_BLOB_FORMAT_V2) return n;
    put_clock(out + n, clk ? clk->shown_ms : 0);
    put_clock(out + n + 6, clk ? clk->settles_ms : 0);
    if (format != STATE_BLOB_FORMAT_V5) return n + STATE_BLOB_V3_CLOCK_BYTES;
    // The stamp is a finished game's: the one owner of that rule on the way in.
    const bool stamped = ratings && ratings->n > 0 && g->status == GAME_STATUS_GAME_OVER;
    return n + STATE_BLOB_V3_CLOCK_BYTES + game_ratings_put(stamped ? ratings : 0, out + n + STATE_BLOB_V3_CLOCK_BYTES);
}

int state_blob_put(const Game *g, const BoardClock *clk, unsigned char *out) {
    return state_blob_put_at(g, clk, STATE_BLOB_FORMAT, out);
}

int state_blob_load(Game *g, const unsigned char *p, int len, BoardClock *clk) {
    return state_blob_load_rated(g, p, len, clk, 0);
}

int state_blob_load_rated(Game *g, const unsigned char *p, int len, BoardClock *clk, GameRatings *ratings) {
    if (len < STATE_BLOB_HEADER) return 0;
    // `len` counts the header bytes too; the state is the rest, less what the
    // format carries behind it: the clock from v3, the rating stamp from v5.
    const int tail = state_blob_tail_bytes(p[0]);
    if (tail < 0) return 0;
    const int v5 = p[0] == STATE_BLOB_FORMAT_V5;
    const int v4 = v5 || p[0] == STATE_BLOB_FORMAT_V4;   // the flag byte is v4's from v4 on
    if (v4 && (p[1] & ~STATE_BLOB_FLAGS_V4)) return 0;   // a rule this kernel cannot honour
    if (len < STATE_BLOB_HEADER + tail) return GAME_INVALID_COUNT;
    // Nor does any row's board name rules (state_blob_put_at): the flag owns
    // them, so a board that names some is not a row this kernel wrote.
    if (len > STATE_BLOB_HEADER + STATE_FLAGS_AT
        && (p[STATE_BLOB_HEADER + STATE_FLAGS_AT] & ~STATE_FLAG_FLIPPED)) return 0;
    const int state_len = len - STATE_BLOB_HEADER - tail;
    // The stamp is judged BEFORE the board is adopted, so a refusal adopts
    // nothing: it must read, and count the board's seats (the state's second
    // byte) on a board that is over (its first), or count none.
    GameRatings stamp = { 0 };
    if (v5) {
        if (!game_ratings_get(&stamp, p + len - GAME_RATINGS_BYTES)) return 0;
        if (stamp.n && (state_len < 2 || stamp.n != p[STATE_BLOB_HEADER + 1]
                        || p[STATE_BLOB_HEADER] != GAME_STATUS_GAME_OVER)) return 0;
    }
    const int r = state_import(g, p + STATE_BLOB_HEADER, state_len, 0);
    if (r != GAME_VALID) return r;
    if (ratings) *ratings = stamp;
    // v2 and v3 read the whole flag byte as the deck bit, as they always have.
    g->deterministic_deck = v4 ? (p[1] & STATE_BLOB_FLAG_DETERMINISTIC) != 0 : p[1] != 0;
    // THE one place a row sets the rules: before v4 every row is the classic
    // game, whatever this Game held before it.
    g->rules = (int8_t)(v4 && !(p[1] & STATE_BLOB_FLAG_PASSING) ? GAME_RULE_NO_PASS : 0);
    if (clk) {
        const unsigned char *c = p + STATE_BLOB_HEADER + state_len;
        const int clocked = state_blob_clock_bytes(p[0]) > 0;
        clk->shown_ms = clocked ? (int64_t)le_get_u48(c) : 0;
        clk->settles_ms = clocked ? (int64_t)le_get_u48(c + 6) : 0;
    }
    return 1;
}

// ---------- the response envelope header ----------------------------------

_Static_assert(ENV_VIEW_AT == ENV_ISLAND_AT + 2 + 2,
               "the header is the island's length prefix, an empty island, and the view's length prefix");

int env_header_write(unsigned char *out, int cap, int viewer, uint32_t version, const GameRatings *ratings) {
    const int island = ratings && ratings->n > 0 ? 1 + GAME_RATINGS_BYTES : 0;
    if (!out || cap < ENV_VIEW_AT + island + 2) return -1;
    out[0] = ENV_FORMAT;
    out[1] = (unsigned char)((viewer >= 0 ? ENV_FLAG_SEATED : 0) | ENV_FLAG_ROSTER);
    out[2] = viewer >= 0 ? (unsigned char)viewer : 0xFF;
    out[3] = (unsigned char)(version & 0xff);
    out[4] = (unsigned char)((version >> 8) & 0xff);
    out[5] = (unsigned char)((version >> 16) & 0xff);
    out[6] = (unsigned char)((version >> 24) & 0xff);
    out[ENV_ISLAND_AT] = (unsigned char)island; out[ENV_ISLAND_AT + 1] = 0;
    if (island) {
        out[ENV_ISLAND_AT + 2] = ENV_ISLAND_RATINGS;
        game_ratings_put(ratings, out + ENV_ISLAND_AT + 3);
    }
    const int view_at = ENV_VIEW_AT + island;
    out[view_at] = VIEW_FORMAT_VERSION;
    out[view_at + 1] = out[2];
    return view_at;
}

void env_header_set_view_len(unsigned char *out, int view_at, int view_len) {
    // The two bytes immediately ahead of the view, wherever the island put it.
    out[view_at - 2] = (unsigned char)view_len;
    out[view_at - 1] = (unsigned char)(view_len >> 8);
}

bool env_island_ratings(const unsigned char *p, const EnvHeader *h, GameRatings *r) {
    memset(r, 0, sizeof(*r));
    if (h->island_len == 0 || p[h->island_at] != ENV_ISLAND_RATINGS) return true;
    return h->island_len == 1 + GAME_RATINGS_BYTES && game_ratings_get(r, p + h->island_at + 1);
}

bool env_header_read(const unsigned char *p, int len, EnvHeader *h) {
    if (!p || len < ENV_VIEW_AT + 2 || p[0] != ENV_FORMAT) return false;
    const int flags = p[1];
    if ((flags & ~(ENV_FLAG_SEATED | ENV_FLAG_ROSTER)) || !(flags & ENV_FLAG_ROSTER)) return false;
    const int seated = (flags & ENV_FLAG_SEATED) != 0;
    const int seat = seated ? p[2] : -1;
    if (seated ? seat >= MAX_PLAYERS : p[2] != 0xFF) return false;
    // The island: skipped by its length, whatever it holds (view.h).
    const int island_len = p[ENV_ISLAND_AT] | (p[ENV_ISLAND_AT + 1] << 8);
    int q = ENV_ISLAND_AT + 2 + island_len;
    if (q + 2 > len) return false;
    const int view_len = p[q] | (p[q + 1] << 8);
    q += 2;
    if (view_len < 2 || q + view_len > len) return false;
    if (p[q] != VIEW_FORMAT_VERSION || p[q + 1] != (seated ? (unsigned char)seat : 0xFF)) return false;
    h->seat = seat;
    h->version = (uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 24);
    h->state_at = q + 2;
    h->state_len = view_len - 2;
    h->trailer_at = q + view_len;
    h->island_at = ENV_ISLAND_AT + 2;
    h->island_len = island_len;
    return true;
}

int log_record_put(const GameLog *l, int mask_draws, int pre_has_flip, Card pre_flip,
                   int has_flipped_now, unsigned char *out) {
    const int flip_drawn = pre_has_flip && !has_flipped_now;
    const int hide = mask_draws && l->log_type == LOG_DRAW;
    unsigned char *q = out;
    *q++ = (unsigned char)l->log_type;
    *q++ = (unsigned char)l->player_idx;
    *q++ = (unsigned char)l->defender_index;
    *q++ = (unsigned char)l->num_pairs;
    for (int j = 0; j < l->num_pairs; j++) {
        const LogPair *pr = &l->pairs[j];
        *q++ = (hide && !(flip_drawn && card_eq(pr->primary, pre_flip)))
            ? (unsigned char)WIRE_CARD_HIDDEN : wire_from_card(pr->primary);
        *q++ = wire_from_card(pr->target);
    }
    return (int)(q - out);
}
