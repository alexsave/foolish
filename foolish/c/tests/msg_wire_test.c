// FMSG envelope test — the native proof of the iMessage payload
// (docs/IMESSAGE_GAME_DESIGN.md §4/§7, docs/IMESSAGE_BODY_CODEC.md, msg_wire.h).
//
// What it asserts, in the order the risks matter:
//
//   1. SHA-256 KATs — parent8 and Rule P's tiebreak are only "deterministic
//      across devices" if this is the real FIPS 180-4 function.
//   2. Round-trip: seal -> encode -> decode -> re-encode is BYTE-IDENTICAL, at
//      2/3/4/8 players, in WAITING/LIVE/FINISHED, over real played games.
//   3. Replay fidelity: a decoded envelope reconstructs the SAME game its body
//      was sealed from — same hands, same table, same deck. This is the whole
//      protocol: two devices must land on identical state or the game forks.
//   4. Tamper matrix: every single-bit flip is refused or CANONICAL, and never
//      crashes. The payload arrives from a URL, so this is the hostile surface.
//   5. Hostile bodies are refused (validation = replay, §7.3).
//   6. Size guardrail: P95 of a full 4-player game < 1,000 base32 chars (§4.4).
//   7. Name-length boundary (round-5 B1, docs/APP_REVIEW_NOTES.md): a 13- and
//      a 64-byte nickname round-trip byte-for-byte; 65 is refused as MSG_ENAME,
//      at both the struct-level API and the hostile wire.
//
// Reported, not asserted: the size distribution per player count and driver, and
// `v6mid` — the mid-game-cut oracle that caught the codec's missing `good` atom
// (docs/IMESSAGE_BODY_CODEC.md §3). It must keep reading `good_mask lost 0`.
//
// Usage: msg_wire_test [games_per_pc] [seed0]   (defaults 20, 20260716)

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/awire.h"
#include "../src/bot_roster.h"
#include "../src/msg_wire.h"
#include "../../../shared/c/sha256.h"
#include "../src/replay.h"
#include "../src/replay_steps.h"
#include "../wasm/wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ENV_CAP 8192

static int g_fails = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fails++; } \
} while (0)

static uint32_t g_rng = 1;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }

// ---------- 1. SHA-256 known-answer tests --------------------------------

static void hex(const uint8_t *b, int n, char *out) {
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[i * 2] = H[b[i] >> 4]; out[i * 2 + 1] = H[b[i] & 15]; }
    out[n * 2] = 0;
}

static void test_sha256_kat(void) {
    struct { const char *in; const char *want; } v[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    for (int i = 0; i < 3; i++) {
        uint8_t d[SHA256_DIGEST_LEN];
        char got[65];
        sha256(v[i].in, strlen(v[i].in), d);
        hex(d, SHA256_DIGEST_LEN, got);
        CHECK(!strcmp(got, v[i].want), "sha256(\"%.20s\") = %s want %s", v[i].in, got, v[i].want);
    }
    // Multi-block + update-boundary coverage: a million 'a' is the standard
    // long vector, and it exercises the buffered path this codebase actually
    // uses (envelopes cross the 64-byte block boundary constantly).
    Sha256 c;
    sha256_init(&c);
    for (int i = 0; i < 1000000; i++) sha256_update(&c, "a", 1);
    uint8_t d[SHA256_DIGEST_LEN];
    char got[65];
    sha256_final(&c, d);
    hex(d, SHA256_DIGEST_LEN, got);
    CHECK(!strcmp(got, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
          "sha256(1e6 x 'a') = %s", got);
}

// ---------- game driving --------------------------------------------------

// A chain under construction: the packed action bytes plus the count.
typedef struct {
    unsigned char buf[MSG_MAX_ACTION_BYTES];
    int len;
    int n;
} Chain;

static int chain_append(Chain *ch, int seat, const AwireAction *a) {
    if (ch->len + 1 >= (int)sizeof(ch->buf)) return 0;
    ch->buf[ch->len] = (unsigned char)seat;
    const int w = awire_encode(a, ch->buf + ch->len + 1, (int)sizeof(ch->buf) - ch->len - 1);
    if (w == 0) return 0;
    ch->len += 1 + w;
    ch->n++;
    return 1;
}

static void seed_fill(uint8_t *seed, uint32_t s) {
    g_rng = s ? s : 1;
    for (int i = 0; i < MSG_SEED_LEN; i++) seed[i] = (uint8_t)(rnd() >> 13);
}

// How long a format's fixed header is - the test's own copy, so a tamper case
// can find n_joins on whatever format the seal chose. Deliberately spelled out
// here rather than exported from msg_wire.c: if the two ever disagree, that is
// a wire bug this file is supposed to catch, not share.
static int env_hdr_len(uint8_t format) {
    if (format == MSG_FORMAT_GENERATION) return MSG_HEADER_LEN_GENERATION;
    if (format == MSG_FORMAT_REMATCH || format == MSG_FORMAT_RULES_REMATCH)
        return MSG_HEADER_LEN_REMATCH;
    if (format == MSG_FORMAT_CLOCK || format == MSG_FORMAT_RULES)
        return MSG_HEADER_LEN_CLOCK;
    return MSG_HEADER_LEN;
}

// Seat names for the fixtures. These are display strings only - the wire
// round-trips any name - but they are what a seeded board SHOWS, so they are
// also what QA and store photography read off the screen. A cast of ordinary
// first names beats "Ann0..Ann7": a table of numbered clones hides exactly the
// defects a name is there to expose (badge overflow, ellipsis, collision with
// the seat chrome), because every label is the same four characters wide.
//
// FOOLISH_NAMES overrides it with a comma-separated list, so a lane that needs
// a specific seat to carry a specific name - the local player, a longest-legal
// name at 8 seats - can say so without a rebuild. Short names stay well inside
// MSG_MAX_NAME (64 bytes) and MSG_MAX_NAME_CHARS (16).
static const char *fixture_name(int seat) {
    static char slots[MSG_MAX_JOINS][MSG_MAX_NAME + 1];
    static int parsed = 0;
    static const char *fallback[MSG_MAX_JOINS] = {
        "Alex", "Mira", "Jonas", "Priya", "Tomas", "Nadia", "Felix", "Sana"
    };
    if (!parsed) {
        parsed = 1;
        const char *env = getenv("FOOLISH_NAMES");
        for (int i = 0; i < MSG_MAX_JOINS; i++)
            snprintf(slots[i], sizeof(slots[i]), "%s", fallback[i]);
        if (env && *env) {
            int i = 0;
            const char *p = env;
            while (*p && i < MSG_MAX_JOINS) {
                const char *c = strchr(p, ',');
                size_t n = c ? (size_t)(c - p) : strlen(p);
                if (n > MSG_MAX_NAME) n = MSG_MAX_NAME;
                if (n > 0) { memcpy(slots[i], p, n); slots[i][n] = 0; i++; }
                if (!c) break;
                p = c + 1;
            }
        }
    }
    return slots[seat % MSG_MAX_JOINS];
}

static void env_init(MsgEnvelope *e, const uint8_t *seed, int n_players) {
    msg_envelope_init(e);   // NOT memset: the rematch fields have sentinels
    e->format = MSG_FORMAT_V6;
    e->flags = 0;
    e->phase = MSG_PHASE_LIVE;
    e->game_id = 0x0123456789abcdefULL;
    e->n_players = (uint8_t)n_players;
    e->variant = 0;
    e->last_actor_seat = 0;
    memcpy(e->seed, seed, MSG_SEED_LEN);
    e->n_joins = n_players;
    for (int i = 0; i < n_players; i++) {
        e->joins[i].seat = (uint8_t)i;
        const char *nm = fixture_name(i);
        const int len = (int)strlen(nm);
        e->joins[i].name_len = (uint8_t)len;
        memcpy(e->joins[i].name, nm, (size_t)len);
    }
}

// Converts a kernel LegalMove into the awire action the chain stores.
static void move_to_awire(const LegalMove *m, AwireAction *a) {
    switch (m->type) {
        case MOVE_ATTACK: a->kind = AWIRE_ATTACK; break;
        case MOVE_COVER:  a->kind = AWIRE_COVER;  break;
        case MOVE_PASS:   a->kind = AWIRE_PASS;   break;
        case MOVE_PICKUP: a->kind = AWIRE_PICKUP; break;
        default:          a->kind = AWIRE_GOOD;   break;
    }
    a->n = (a->kind == AWIRE_PICKUP || a->kind == AWIRE_GOOD) ? 0 : m->n_cards;
    for (int i = 0; i < a->n; i++) {
        a->cards[i] = m->cards[i];
        a->attacks[i] = m->attack_cards[i];
    }
}

// Plays a legal game, recording the chain. `bot` is a bot_roster index, or -1
// to pick uniformly at random among legal moves. Returns the round count the
// replay should derive; fills `end` with the final game.
//
// Random play is the right driver for the CODEC tests (it reaches shapes a good
// bot never would), but the wrong one for the size budget — see test_size_budget.
static int play_game_rules(const uint8_t *seed, int n_players, int max_actions,
                           Chain *ch, Game *end, int bot, int8_t rules);

static int play_game(const uint8_t *seed, int n_players, int max_actions,
                     Chain *ch, Game *end, int bot) {
    return play_game_rules(seed, n_players, max_actions, ch, end, bot, 0);
}

// `rules` is the table's variant (game.h GAME_RULE_*); 0 is the classic game
// every caller above wants.
static int play_game_rules(const uint8_t *seed, int n_players, int max_actions,
                           Chain *ch, Game *end, int bot, int8_t rules) {
    // Pin the strategies' RNG per game: it is process-global, so without this a
    // measurement would depend on what ran before it (the 4p size moved by ~10%
    // just from adding an 8p sample ahead of it).
    random_strategy_set_seed(((uint32_t)seed[0] << 24) | ((uint32_t)seed[1] << 16) |
                             ((uint32_t)seed[2] << 8) | (uint32_t)seed[3] | 1u);
    game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
    Game g;
    memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)n_players;
    g.rules = rules;
    for (int i = 0; i < n_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = 0;
    }
    start_game(&g);

    int rounds = 0;
    static LegalMoves ml;
    for (int step = 0; step < max_actions; step++) {
        if (game_done(&g) >= 0 || g.status != GAME_STATUS_PLAYING) break;
        // Pick a seat that may act, then a legal move for it. The scan starts at
        // a random seat but visits ALL of them: sampling at random gave up ~1
        // game in 80 (at 3p only the opener can act, so 12 random draws miss it
        // 0.8% of the time) and produced empty chains that looked like codec
        // failures.
        int seat = -1;
        const int start = (int)(rnd() % (uint32_t)n_players);
        for (int t = 0; t < n_players && seat < 0; t++) {
            const int s = (start + t) % n_players;
            if (g.players[s].status != PLAYER_STATUS_IN) continue;
            calculate_legal_moves(&g, s, &ml);
            for (int i = 0; i < ml.n; i++) {
                if (ml.moves[i].type != MOVE_WAIT) { seat = s; break; }
            }
        }
        if (seat < 0) break;
        calculate_legal_moves(&g, seat, &ml);
        if (ml.n == 0) break;
        int pick;
        if (bot < 0) {
            pick = (int)(rnd() % (uint32_t)ml.n);
        } else {
            pick = bot_roster_choose(bot, &g, seat, &ml);
            if (pick < 0 || pick >= ml.n) pick = 0;
        }
        const LegalMove *m = &ml.moves[pick];
        if (m->type == MOVE_WAIT) continue;

        AwireAction a;
        move_to_awire(m, &a);
        const int battles_before = g.num_battles;

        bool ok;
        switch (a.kind) {
            case AWIRE_ATTACK: ok = handle_attack(&g, seat, a.cards, a.n); break;
            case AWIRE_COVER:  ok = handle_cover(&g, seat, a.cards, a.attacks, a.n); break;
            case AWIRE_PASS:   ok = handle_pass(&g, seat, a.cards, a.n); break;
            case AWIRE_PICKUP: ok = handle_pickup(&g, seat); break;
            default:           ok = handle_good(&g, seat); break;
        }
        if (!ok) continue; // menu/handler disagreement is another suite's problem
        if (battles_before > 0 && g.num_battles == 0) rounds++;
        if (!chain_append(ch, seat, &a)) break;
    }
    *end = g;
    return rounds;
}

// ---------- 2+3. round-trip and replay fidelity ---------------------------

static int g_finished_replays;   // test_roundtrip's finished games, so the status check is not vacuous

static void test_roundtrip(int games, uint32_t seed0) {
    const int pcs[] = { 2, 3, 4, 8 };
    for (int pi = 0; pi < 4; pi++) {
        const int np = pcs[pi];
        for (int gi = 0; gi < games; gi++) {
            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, seed0 + (uint32_t)(gi * 977 + np * 31));
            g_rng = seed0 + (uint32_t)(gi * 13 + np);

            Chain ch; memset(&ch, 0, sizeof(ch));
            Game played;
            const int rounds = play_game(seed, np, 400, &ch, &played, -1);

            MsgEnvelope e;
            env_init(&e, seed, np);
        // A NONZERO stamp, because a 0 here (what every other seal in this file
        // uses) is a format-2 chain and this loop is meant to cover the
        // clock-bearing formats. It is derived from the game's own seed rather
        // than read from time(NULL): nothing below asks about the pickup hold -
        // the compared field list a few lines down excludes sent_at, and the
        // roundtrip is byte-for-byte against what this same value encoded - so
        // the clock only made the test's INPUTS unreproducible. The payload
        // printers further down (--twocover, --lastdefense, --fatboard) still
        // stamp real time, and should: a human opening those on a phone is
        // exactly how the 15-second hold gets exercised.
        e.sent_at = (uint16_t)((seed0 + (uint32_t)(gi * 31 + np) * 7u) | 1u);
            const int over = game_done(&played) >= 0 || played.status == GAME_STATUS_GAME_OVER;
            e.phase = over ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE;
            static unsigned char body[1024];
            static Game scratch;
            const int src = msg_seal(&e, &played, MSG_NO_BASE, body, sizeof(body), &scratch);
            CHECK(src == MSG_EOK, "np=%d game=%d seal failed: %d (num_logs %d/%d, moves %d, replay_detail %d)", np, gi, src, played.num_logs, MAX_LOGS, ch.n, replay_last_error_detail());
            if (src != MSG_EOK) continue;
            // The codec folds a bout's closing goods into one round_end atom, so
            // the sealed atom count is <= the moves played. Rounds must agree.
            CHECK(e.round == (uint8_t)rounds, "np=%d game=%d sealed round %d != played %d",
                  np, gi, e.round, rounds);
            CHECK(e.n_actions <= ch.n, "np=%d game=%d atoms %d > moves %d",
                  np, gi, e.n_actions, ch.n);

            unsigned char wire[ENV_CAP];
            const int n = msg_encode(&e, wire, sizeof(wire));
            CHECK(n > 0, "np=%d game=%d encode failed: %d", np, gi, n);
            if (n <= 0) continue;

            MsgEnvelope d;
            const int rc = msg_decode(wire, n, &d);
            CHECK(rc == MSG_EOK, "np=%d game=%d decode failed: %d", np, gi, rc);
            if (rc != MSG_EOK) continue;

            CHECK(d.n_actions == e.n_actions && d.turn == e.turn && d.round == e.round &&
                  d.n_players == e.n_players && d.phase == e.phase && d.game_id == e.game_id,
                  "np=%d game=%d field mismatch after decode", np, gi);
            CHECK(!memcmp(d.seed, e.seed, MSG_SEED_LEN), "np=%d game=%d seed mismatch", np, gi);

            // Re-encode must be byte-identical: the format has no slack, no
            // optional ordering, no padding a second encoder could choose
            // differently. (A device rebases and re-sends chains constantly; if
            // re-encode drifted, parent8 would break for everyone downstream.)
            unsigned char wire2[ENV_CAP];
            const int n2 = msg_encode(&d, wire2, sizeof(wire2));
            CHECK(n2 == n && !memcmp(wire, wire2, (size_t)n),
                  "np=%d game=%d re-encode not byte-identical (%d vs %d)", np, gi, n2, n);

            // Replay must rebuild the very game the chain was recorded from.
            Game rg;
            const int rrc = msg_replay(&d, &rg);
            CHECK(rrc == MSG_EOK, "np=%d game=%d replay failed: %d", np, gi, rrc);
            if (rrc != MSG_EOK) continue;
            CHECK(rg.num_battles == played.num_battles && rg.defender == played.defender &&
                  rg.deck_count == played.deck_count && rg.discard_pile_length == played.discard_pile_length &&
                  rg.power_suit == played.power_suit,
                  "np=%d game=%d replayed state diverged", np, gi);
            // A FINISHED CHAIN REPLAYS AS A FINISHED GAME. The kernel records
            // its own end (game.c game_settle_status) on every apply path, and
            // the board a receiver opens is this replay: left PLAYING, the
            // fool's board still offered Take on a game that was over (filmed
            // on the rig, 2p and 4p, under the end screen).
            const int want = game_done(&played) >= 0 ? GAME_STATUS_GAME_OVER : GAME_STATUS_PLAYING;
            CHECK(rg.status == want, "np=%d game=%d replayed status %d, want %d",
                  np, gi, rg.status, want);
            if (want == GAME_STATUS_GAME_OVER) g_finished_replays++;
            for (int s = 0; s < np; s++) {
                CHECK(rg.players[s].hand_count == played.players[s].hand_count,
                      "np=%d game=%d seat %d hand %d vs %d", np, gi, s,
                      rg.players[s].hand_count, played.players[s].hand_count);
                for (int c = 0; c < rg.players[s].hand_count; c++) {
                    CHECK(card_eq(rg.players[s].hand[c], played.players[s].hand[c]),
                          "np=%d game=%d seat %d card %d differs", np, gi, s, c);
                }
            }
        }
    }
}

static void test_waiting_phase(void) {
    uint8_t seed[MSG_SEED_LEN];
    seed_fill(seed, 42);
    MsgEnvelope e;
    env_init(&e, seed, 4);
    e.phase = MSG_PHASE_WAITING;
    e.n_joins = 1;              // only the creator has claimed a seat
    e.turn = 0; e.round = 0; e.n_actions = 0; e.actions_len = 0; e.actions = 0;

    unsigned char wire[ENV_CAP];
    const int n = msg_encode(&e, wire, sizeof(wire));
    CHECK(n > 0, "WAITING encode failed: %d", n);
    MsgEnvelope d;
    CHECK(msg_decode(wire, n, &d) == MSG_EOK, "WAITING decode failed");
    CHECK(d.n_joins == 1 && d.n_actions == 0, "WAITING fields wrong");
}

// ---------- 7. name length boundary (round-5 B1: 12 -> 64 bytes) ----------
//
// The App Store review's B1 (docs/APP_REVIEW_NOTES.md): a name over 12 UTF-8
// bytes failed to seal, silently, and the client blamed the link — an 8-letter
// Cyrillic name like "Владимир" is 16 bytes and never fit. Owner's round-5
// call: raise MSG_MAX_NAME to 64 (the Swift UI separately caps at 16
// characters; not this layer's job). This pins the NEW boundary the same way
// test_tamper pins the header fields: AT the cap must round-trip byte-for-byte,
// and one byte OVER must be refused as MSG_ENAME — never truncated, never
// silently widened past it.
static void test_name_length_boundary(void) {
    uint8_t seed[MSG_SEED_LEN];
    seed_fill(seed, 1213);
    g_rng = 1213;
    Chain ch; memset(&ch, 0, sizeof(ch));
    Game played;
    play_game(seed, 2, 40, &ch, &played, -1);
    const int over = game_done(&played) >= 0 || played.status == GAME_STATUS_GAME_OVER;
    const uint8_t phase = (uint8_t)(over ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE);

    // 13 bytes: one past the OLD cap (the exact boundary the review's table
    // found broken). MSG_MAX_NAME: the NEW cap, exactly.
    const int lens[] = { 13, MSG_MAX_NAME };
    for (int li = 0; li < (int)(sizeof(lens) / sizeof(lens[0])); li++) {
        const int len = lens[li];
        MsgEnvelope e;
        env_init(&e, seed, 2);
        e.phase = phase;
        for (int i = 0; i < len; i++) e.joins[0].name[i] = (char)('A' + (i % 26));
        e.joins[0].name_len = (uint8_t)len;

        static unsigned char body[1024];
        static Game scratch;
        CHECK(msg_seal(&e, &played, MSG_NO_BASE, body, sizeof(body), &scratch) == MSG_EOK,
              "name_len=%d seal failed", len);

        unsigned char wire[ENV_CAP];
        const int n = msg_encode(&e, wire, sizeof(wire));
        CHECK(n > 0, "name_len=%d encode failed: %d", len, n);
        if (n <= 0) continue;

        MsgEnvelope d;
        CHECK(msg_decode(wire, n, &d) == MSG_EOK, "name_len=%d decode failed", len);
        CHECK(d.joins[0].name_len == (uint8_t)len &&
              !memcmp(d.joins[0].name, e.joins[0].name, (size_t)len),
              "name_len=%d round-trip mismatch", len);

        // Re-encode must be byte-identical, same property test_roundtrip pins.
        unsigned char wire2[ENV_CAP];
        const int n2 = msg_encode(&d, wire2, sizeof(wire2));
        CHECK(n2 == n && !memcmp(wire, wire2, (size_t)n),
              "name_len=%d re-encode not byte-identical", len);
    }

    // MSG_MAX_NAME + 1 (65): one past the NEW cap. Refused at the struct-level
    // API (encode) — content is irrelevant, the length check fires first, so
    // leaving the rest of the fixed-size name[] array unwritten is safe.
    {
        MsgEnvelope e;
        env_init(&e, seed, 2);
        e.phase = phase;
        e.joins[0].name_len = (uint8_t)(MSG_MAX_NAME + 1);
        unsigned char wire[ENV_CAP];
        CHECK(msg_encode(&e, wire, sizeof(wire)) == MSG_ENAME,
              "name_len=%d should have been refused encoding", MSG_MAX_NAME + 1);
    }

    // Same boundary at the actual hostile surface: a wire whose join name_len
    // byte was tampered past the cap (the payload arrives from a URL, never a
    // trusted struct — §7.1). Build a valid at-cap envelope, then flip the one
    // byte that claims the name's length.
    {
        MsgEnvelope e;
        env_init(&e, seed, 2);
        e.phase = phase;
        for (int i = 0; i < MSG_MAX_NAME; i++) e.joins[0].name[i] = 'A';
        e.joins[0].name_len = MSG_MAX_NAME;
        static unsigned char body[1024];
        static Game scratch;
        CHECK(msg_seal(&e, &played, MSG_NO_BASE, body, sizeof(body), &scratch) == MSG_EOK,
              "name boundary tamper fixture seal failed");
        unsigned char wire[ENV_CAP];
        const int n = msg_encode(&e, wire, sizeof(wire));
        CHECK(n > 0, "name boundary tamper fixture encode failed");
        if (n > 0) {
            wire[env_hdr_len(e.format) + 1] = MSG_MAX_NAME + 1;   // the first join's name_len byte
            MsgEnvelope d;
            CHECK(msg_decode(wire, n, &d) == MSG_ENAME,
                  "wire name_len=%d should have been refused decoding", MSG_MAX_NAME + 1);
        }
    }
}

// Rule P, rule 0: a STARTED chain beats the lobby it grew out of.
//
// The regression this pins is a fork, not a cosmetic preference. A WAITING
// lobby and the LIVE handoff that starts it both sit at round 0 / turn 0, so
// before rule 0 existed the comparison fell through to the digest tiebreak —
// a coin flip. Roughly half of all games therefore had every device that had
// cached the lobby REJECT the started game and keep the invite, which the
// client then renders as a board dealt at the lobby's CAPACITY (8) instead of
// the real player count. Two deals, two different first attackers, deadlock.
//
// So the loop below is not just "assert live wins": it counts the cases where
// the lobby's digest sorts FIRST (exactly the ones the old rule got wrong) and
// fails if there were none, because a run without them would pass against the
// broken rule too.
static void test_rule_p_started_beats_lobby(void) {
    unsigned char lobby_wire[ENV_CAP], live_wire[ENV_CAP];
    int lobby_digest_first = 0, cases = 0;

    for (uint32_t g = 1; g <= 60; g++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, g * 7919u);

        // The invite: dealt at the lobby's capacity, only the creator joined.
        MsgEnvelope lob;
        env_init(&lob, seed, 8);
        lob.phase = MSG_PHASE_WAITING;
        lob.game_id = 0x1000ULL + g;
        lob.n_joins = 1;
        lob.turn = 0; lob.round = 0; lob.n_actions = 0; lob.actions_len = 0; lob.actions = 0;
        const int nl = msg_encode(&lob, lobby_wire, sizeof(lobby_wire));
        CHECK(nl > 0, "lobby encode failed: %d", nl);

        // The handoff Start seals from the SAME locked seed: same game, real
        // player count, no action applied yet — round 0 / turn 0, like the lobby.
        MsgEnvelope live;
        env_init(&live, seed, 5);
        live.phase = MSG_PHASE_LIVE;
        live.game_id = lob.game_id;
        live.n_joins = 5;
        live.turn = 0; live.round = 0; live.n_actions = 0; live.actions_len = 0; live.actions = 0;
        const int nv = msg_encode(&live, live_wire, sizeof(live_wire));
        CHECK(nv > 0, "live handoff encode failed: %d", nv);

        MsgChainKey kl, kv;
        CHECK(msg_chain_key(lobby_wire, nl, &kl) == MSG_EOK, "lobby chain key failed");
        CHECK(msg_chain_key(live_wire, nv, &kv) == MSG_EOK, "live chain key failed");
        CHECK(kl.round == kv.round && kl.turn == kv.turn,
              "fixture no longer poses the tie (round/turn differ)");

        cases++;
        if (memcmp(kl.digest, kv.digest, SHA256_DIGEST_LEN) < 0) lobby_digest_first++;

        CHECK(msg_rule_p(&kl, &kv) > 0, "game %u: lobby preferred over the started game", g);
        CHECK(msg_rule_p(&kv, &kl) < 0, "game %u: rule P is not symmetric", g);
    }
    CHECK(cases > 0, "rule P lobby/live fixture built nothing");
    CHECK(lobby_digest_first > 0,
          "no fixture had the lobby digest sorting first — this run could not "
          "have caught the digest-coin-flip bug");
}

// 1.1(56) - WHAT AN ARRIVING CHAIN DOES TO AN OPEN LOBBY (msg_surface_delta).
//
// The owner: "LOBBY DID NOT UPDATE LIVE! I was in lobby, got a start game text,
// and it was stuck on lobby!" - and then the shape of the fix: one bubble can
// carry SEVERAL actions, because `conversation.insert` replaces an unsent draft
// rather than queueing a second one, so Join + rules + Start go out as one
// envelope. This is the half that reads the two envelopes; anim_surface_plan
// (anim_plan_test.c) is the half that lays the answer out as beats.
//
// MUTATION-CHECKED. Comparing rosters by COUNT instead of row-by-row makes the
// same-size swap below read as "nothing changed"; comparing in WIRE order rather
// than seat order makes the shuffled roster read as a change that never
// happened; ignoring the SHOWING chain's phase makes every arrival a start.
static void test_surface_delta(void) {
    uint8_t seed[MSG_SEED_LEN];
    seed_fill(seed, 4242u);

    MsgEnvelope lob, arr;
    MsgSurfaceDelta d;

    // A lobby with one seat, and the same lobby with a second player in it.
    env_init(&lob, seed, 8);
    lob.phase = MSG_PHASE_WAITING;
    lob.n_joins = 1;
    lob.turn = 0; lob.round = 0; lob.n_actions = 0; lob.actions_len = 0; lob.actions = 0;

    arr = lob;
    arr.n_joins = 2;

    msg_surface_delta(&lob, &arr, &d);
    CHECK(d.on_a_lobby == 1, "a WAITING chain on screen IS a lobby");
    CHECK(d.roster_moved == 1, "somebody sat down");
    CHECK(d.started == 0, "a lobby arriving on a lobby has not started anything");

    // The SAME bubble, with the game dealt on it - a join and a Start in one
    // text, which is the report.
    MsgEnvelope live = arr;
    live.phase = MSG_PHASE_LIVE;
    live.n_players = 2;
    msg_surface_delta(&lob, &live, &d);
    CHECK(d.started == 1, "the arriving chain is dealt, so the lobby is over");
    CHECK(d.roster_moved == 1, "and it still carries the join that came with it");

    // A BOARD is not a lobby, whatever arrives on it.
    msg_surface_delta(&live, &live, &d);
    CHECK(d.on_a_lobby == 0, "a LIVE chain on screen is a board");
    CHECK(d.started == 0, "a board was never a lobby, so nothing 'started' on it - "
                          "this is a fact about BOTH chains, not just the arriving one");

    // THE ROSTER IS COMPARED ROW BY ROW, not by size. Same count, different
    // people: a device that counted would call this "nothing changed" and never
    // redraw the names.
    MsgEnvelope swapped = arr;
    swapped.joins[1].name[3] = 'Z';
    msg_surface_delta(&arr, &swapped, &d);
    CHECK(d.roster_moved == 1, "a seat that changed hands is not the seat that was there");

    // …AND IN SEAT ORDER, not wire order. Nothing on the wire sorts the joins,
    // and "who joined next" is a fact about seats.
    MsgEnvelope shuffled = arr;
    shuffled.joins[0] = arr.joins[1];
    shuffled.joins[1] = arr.joins[0];
    msg_surface_delta(&arr, &shuffled, &d);
    CHECK(d.roster_moved == 0, "the same two people, listed the other way round, "
                               "are the same roster");

    // A LEAVE compacts the seats below it, so the rosters diverge in the middle
    // and there is no run of joins to walk.
    MsgEnvelope three = lob;
    three.n_joins = 3;
    MsgEnvelope left = lob;
    left.n_joins = 2;
    left.joins[1] = three.joins[2];
    left.joins[1].seat = 1;
    msg_surface_delta(&three, &left, &d);
    CHECK(d.roster_moved == 1, "somebody got up, and that is a roster change too");

    // THE RULES, read through msg_pass_allowed so a format that predates the
    // rules byte reads as the passing game it always was.
    MsgEnvelope podk = arr;
    podk.format = MSG_FORMAT_RULES;
    podk.variant = 0;
    MsgEnvelope pass = arr;
    pass.format = MSG_FORMAT_RULES;
    pass.variant = MSG_VARIANT_PASS;
    msg_surface_delta(&pass, &podk, &d);
    CHECK(d.passing_before == 1 && d.passing_after == 0,
          "the table turned the transfer off (got %d -> %d)",
          d.passing_before, d.passing_after);
    msg_surface_delta(&arr, &podk, &d);
    CHECK(d.passing_before == 1,
          "a pre-rules format is the passing game, not 'no rule'");

    // A STALE SURFACE SPANS EVERYTHING SINCE WHAT IT IS SHOWING. The extension
    // was closed while two texts arrived - somebody joined, then somebody
    // started - and the one chain that gets adopted differs from the lobby on
    // screen by BOTH. Owner, asked directly: "OK yeah that's the correct
    // behavior." One sequence, not a jump and not a bare fade.
    msg_surface_delta(&lob, &live, &d);
    CHECK(d.roster_moved == 1 && d.started == 1,
          "a two-message gap is reported as both actions, not just the newer one");

    // A RE-SEND of the chain already on screen differs by nothing, so nothing
    // animates. A no-op that played a fade would be very visible.
    msg_surface_delta(&arr, &arr, &d);
    CHECK(d.roster_moved == 0 && d.started == 0 && d.passing_before == d.passing_after,
          "the same chain twice is not a change");

    // …and neither is a NET-ZERO one: the rule toggled off and back on across
    // two texts while nobody was looking. Two bubbles arrived and the surface
    // does nothing, because nothing differs.
    msg_surface_delta(&pass, &pass, &d);
    CHECK(d.passing_before == d.passing_after, "a rule that came back is not a change");

    // A COLD OPEN PAINTS, three ways, and none of them is a sequence.
    //
    // Nothing was on screen: there is no `showing` to hand this at all.
    msg_surface_delta(0, &arr, &d);
    CHECK(d.on_a_lobby == 0 && d.started == 0, "a missing side stages nothing");
    // A DIFFERENT GAME's lobby, tapped while one was showing. A switch, not a
    // continuation - and caught on the game id, because "something was
    // showing" is true here and is not the question.
    MsgEnvelope other = live;
    other.game_id = lob.game_id + 1;
    msg_surface_delta(&lob, &other, &d);
    CHECK(d.on_a_lobby == 0 && d.started == 0 && d.roster_moved == 0,
          "another game's bubble is a switch, and a switch has no beats");
    // Re-opening the SAME lobby after the extension was closed is the third,
    // and it never reaches here: the extension's cold load adopts straight
    // (GameSurface.load), and only `maybeAdoptIncoming` asks for a plan. Pinned
    // on the near side instead - the same chain differs by nothing anyway.
    msg_surface_delta(&lob, &lob, &d);
    CHECK(d.roster_moved == 0 && d.started == 0,
          "re-opening the chain you were already showing changes nothing");
}

// Rule P, rule 3: at an equal (round, turn), the fuller roster wins.
//
// The fork this pins is lobby v3's double Start: any joined player may Start,
// and Start deals at the tapped bubble's join count — so one player starting
// off the full 4-join lobby and another off a stale 3-join view seal TWO LIVE
// handoffs, both round 0 / turn 0, from the same locked seed at DIFFERENT
// player counts. Different deals: different trump, different first attacker.
// Under the digest tiebreak the 3-player fork won half the time, and when the
// 4-player game's first attacker was the player stranded on the 3-player
// board, nobody anywhere could act (the shipped 4p incident). Like the rule-0
// test above, this counts the fixtures the OLD rule got wrong (smaller-roster
// digest sorting first) and fails if the run produced none.
static void test_rule_p_fuller_start_wins(void) {
    unsigned char full_wire[ENV_CAP], stale_wire[ENV_CAP];
    int small_digest_first = 0, cases = 0;

    for (uint32_t g = 1; g <= 60; g++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, g * 6271u);

        // Start from the full lobby: 4 joined, dealt at 4, no action yet.
        MsgEnvelope full;
        env_init(&full, seed, 4);
        full.game_id = 0x2000ULL + g;
        full.turn = 0; full.round = 0; full.n_actions = 0; full.actions_len = 0; full.actions = 0;
        const int nf = msg_encode(&full, full_wire, sizeof(full_wire));
        CHECK(nf > 0, "full start encode failed: %d", nf);

        // The racing Start off a stale 3-join view of the SAME lobby chain.
        MsgEnvelope stale;
        env_init(&stale, seed, 3);
        stale.game_id = full.game_id;
        stale.last_actor_seat = 2;
        stale.turn = 0; stale.round = 0; stale.n_actions = 0; stale.actions_len = 0; stale.actions = 0;
        const int ns = msg_encode(&stale, stale_wire, sizeof(stale_wire));
        CHECK(ns > 0, "stale start encode failed: %d", ns);

        MsgChainKey kf, ks;
        CHECK(msg_chain_key(full_wire, nf, &kf) == MSG_EOK, "full chain key failed");
        CHECK(msg_chain_key(stale_wire, ns, &ks) == MSG_EOK, "stale chain key failed");
        CHECK(kf.round == ks.round && kf.turn == ks.turn,
              "fixture no longer poses the tie (round/turn differ)");
        CHECK(kf.n_joins == 4 && ks.n_joins == 3, "chain key lost the join count");

        cases++;
        if (memcmp(ks.digest, kf.digest, SHA256_DIGEST_LEN) < 0) small_digest_first++;

        CHECK(msg_rule_p(&kf, &ks) < 0, "game %u: the stale 3p start beat the full 4p game", g);
        CHECK(msg_rule_p(&ks, &kf) > 0, "game %u: rule P is not symmetric", g);
    }
    CHECK(cases > 0, "rule P fuller-start fixture built nothing");
    CHECK(small_digest_first > 0,
          "no fixture had the small roster's digest sorting first — this run "
          "could not have caught the digest-coin-flip bug");

    // The ordering around rule 3, pinned on synthetic keys: turn STILL
    // dominates joins (a chain someone played on is never clobbered by a stale
    // wider Start), and joins order WAITING chains too (a 3-join lobby beats
    // the 2-join lobby it grew from — that is what lets an open lobby screen
    // adopt an incoming join instead of coin-flipping against its own invite).
    {
        MsgChainKey played = {0}, wide = {0};
        played.phase = MSG_PHASE_LIVE; played.turn = 1; played.n_joins = 3;
        wide.phase   = MSG_PHASE_LIVE; wide.turn   = 0; wide.n_joins   = 4;
        memset(played.digest, 0xFF, SHA256_DIGEST_LEN);   // digest would pick `wide`
        CHECK(msg_rule_p(&played, &wide) < 0, "a played-on chain lost to a stale wider start");

        MsgChainKey lob2 = {0}, lob3 = {0};
        lob2.phase = MSG_PHASE_WAITING; lob2.n_joins = 2;
        lob3.phase = MSG_PHASE_WAITING; lob3.n_joins = 3;
        memset(lob3.digest, 0xFF, SHA256_DIGEST_LEN);     // digest would pick lob2
        CHECK(msg_rule_p(&lob3, &lob2) < 0, "the fuller lobby lost to the invite it grew from");
    }
}

// Rule P, rule 4: a chain's own CHILD outranks it at an equal (round, turn).
//
// The tie is manufactured exactly the way live play manufactures it: an
// attacker says GOOD while the bout cannot close yet (a pending good — one
// more atom on the chain), and the next player acts on top of it. The atom
// stream is re-derived on every seal and a pending good stops being an atom
// the moment anything follows it, so the child seals back to its parent's own
// turn — and before rule 4 the comparison fell through to the digest coin
// flip. Half of those flips kept the PARENT: a live drawer silently refusing
// the very move that had just been played on it (the 1.0(17) "board is a bit
// behind until I close and re-tap the bubble" report, reproduced end-to-end by
// the FoolishHarness `arrival` scenario before this rule existed).
//
// Like the other two rule-P regression tests, this one counts the fixtures the
// OLD rule got wrong (parent digest sorting first) and fails if the sweep
// produced none — a run without them would pass against the broken rule too.
static void test_rule_p_child_beats_parent(void) {
    int cases = 0, parent_digest_first = 0, posed_ties = 0;
    static Game gm, parent_g, scratch;
    static LegalMoves ml;
    static unsigned char body[2048];

    for (uint32_t g = 1; g <= 400 && cases < 40; g++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, g * 4801u);
        random_strategy_set_seed(g * 97u + 1u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        memset(&gm, 0, sizeof(gm));
        gm.num_players = 4;
        for (int i = 0; i < 4; i++) {
            gm.players[i].status = PLAYER_STATUS_READY;
            gm.players[i].strategy_key = 0;
        }
        start_game(&gm);

        // Random play until a NON-CLOSING good lands: the pending good whose
        // atom the follow-up will fold.
        int have_parent = 0;
        for (int step = 0; step < 200 && !have_parent; step++) {
            if (game_done(&gm) >= 0 || gm.status != GAME_STATUS_PLAYING) break;
            int seat = -1;
            const int start = (int)(rnd() % 4u);
            for (int t = 0; t < 4 && seat < 0; t++) {
                const int s = (start + t) % 4;
                if (gm.players[s].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&gm, s, &ml);
                for (int i = 0; i < ml.n; i++)
                    if (ml.moves[i].type != MOVE_WAIT) { seat = s; break; }
            }
            if (seat < 0) break;
            calculate_legal_moves(&gm, seat, &ml);
            const int pick = (int)(rnd() % (uint32_t)ml.n);
            const LegalMove *m = &ml.moves[pick];
            if (m->type == MOVE_WAIT) continue;
            AwireAction a;
            move_to_awire(m, &a);
            bool ok;
            switch (a.kind) {
                case AWIRE_ATTACK: ok = handle_attack(&gm, seat, a.cards, a.n); break;
                case AWIRE_COVER:  ok = handle_cover(&gm, seat, a.cards, a.attacks, a.n); break;
                case AWIRE_PASS:   ok = handle_pass(&gm, seat, a.cards, a.n); break;
                case AWIRE_PICKUP: ok = handle_pickup(&gm, seat); break;
                default:           ok = handle_good(&gm, seat); break;
            }
            if (!ok) continue;
            if (a.kind == AWIRE_GOOD && gm.num_battles > 0) have_parent = 1;
        }
        if (!have_parent) continue;
        game_clone(&parent_g, &gm);

        // A follower that keeps the bout open (attack / cover / pass), so the
        // child stays in the parent's round. A pickup would close the bout and
        // bump `round`, which rules 1..3 already order correctly.
        int fs = -1, fp = -1;
        for (int s = 0; s < 4 && fs < 0; s++) {
            if (gm.players[s].status != PLAYER_STATUS_IN) continue;
            calculate_legal_moves(&gm, s, &ml);
            for (int i = 0; i < ml.n; i++) {
                const int t = ml.moves[i].type;
                if (t == MOVE_ATTACK || t == MOVE_COVER || t == MOVE_PASS) {
                    fs = s; fp = i; break;
                }
            }
        }
        if (fs < 0) continue;
        calculate_legal_moves(&gm, fs, &ml);
        AwireAction fa;
        move_to_awire(&ml.moves[fp], &fa);
        bool fok;
        switch (fa.kind) {
            case AWIRE_ATTACK: fok = handle_attack(&gm, fs, fa.cards, fa.n); break;
            case AWIRE_COVER:  fok = handle_cover(&gm, fs, fa.cards, fa.attacks, fa.n); break;
            default:           fok = handle_pass(&gm, fs, fa.cards, fa.n); break;
        }
        if (!fok) continue;

        // Seal parent and child; the child's parent8 names the parent's digest,
        // exactly as MessageTurnController.seal does on a phone.
        MsgEnvelope ea;
        env_init(&ea, seed, 4);
        ea.game_id = 0x4000ULL + g;
        if (msg_seal(&ea, &parent_g, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK)
            continue;
        unsigned char wa[ENV_CAP];
        const int na = msg_encode(&ea, wa, sizeof(wa));
        if (na <= 0) continue;
        uint8_t da[SHA256_DIGEST_LEN];
        msg_digest(wa, na, da);

        MsgEnvelope eb;
        env_init(&eb, seed, 4);
        eb.game_id = ea.game_id;
        eb.last_actor_seat = (uint8_t)fs;
        memcpy(eb.parent8, da, MSG_PARENT_LEN);
        if (msg_seal(&eb, &gm, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK)
            continue;
        unsigned char wb[ENV_CAP];
        const int nb = msg_encode(&eb, wb, sizeof(wb));
        if (nb <= 0) continue;

        MsgChainKey ka, kb;
        CHECK(msg_chain_key(wa, na, &ka) == MSG_EOK, "game %u: parent chain key failed", g);
        CHECK(msg_chain_key(wb, nb, &kb) == MSG_EOK, "game %u: child chain key failed", g);
        // The premise itself: the pending good folded, so the child re-encoded
        // to the parent's own (round, turn) - the tie rules 1..3 cannot break.
        if (ka.round != kb.round || ka.turn != kb.turn) continue;
        posed_ties++;

        cases++;
        if (memcmp(ka.digest, kb.digest, SHA256_DIGEST_LEN) < 0) parent_digest_first++;
        CHECK(msg_rule_p(&ka, &kb) > 0, "game %u: the parent beat its own child", g);
        CHECK(msg_rule_p(&kb, &ka) < 0, "game %u: rule P is not symmetric", g);
    }
    CHECK(cases > 0, "rule P child/parent fixture built nothing");
    CHECK(parent_digest_first > 0,
          "no fixture had the parent digest sorting first — this run could not "
          "have caught the digest-coin-flip bug");
    printf("  rule_p child-vs-parent: %d posed ties, %d the old rule would have "
           "refused\n", posed_ties, parent_digest_first);

    // The INVERSION: TWO pending goods are two atoms, and the non-good that
    // follows supersedes both, so the child seals to a turn LOWER than its
    // parent's. Here the old rule did not even need the coin flip - the turn
    // comparison preferred the parent every time, so the arriving move was
    // refused deterministically. This is why rule 4 ranks ABOVE round/turn
    // rather than sitting at the digest tiebreak.
    int inv_cases = 0;
    for (uint32_t g = 1; g <= 600 && inv_cases < 10; g++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, g * 9013u);
        random_strategy_set_seed(g * 53u + 7u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        memset(&gm, 0, sizeof(gm));
        gm.num_players = 4;
        for (int i = 0; i < 4; i++) {
            gm.players[i].status = PLAYER_STATUS_READY;
            gm.players[i].strategy_key = 0;
        }
        start_game(&gm);

        // Random play until TWO goods are pending back to back (the second
        // good's log directly follows the first's, bout still open).
        int goods_run = 0, have_parent = 0;
        for (int step = 0; step < 240 && !have_parent; step++) {
            if (game_done(&gm) >= 0 || gm.status != GAME_STATUS_PLAYING) break;
            int seat = -1;
            const int start = (int)(rnd() % 4u);
            for (int t = 0; t < 4 && seat < 0; t++) {
                const int s = (start + t) % 4;
                if (gm.players[s].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&gm, s, &ml);
                for (int i = 0; i < ml.n; i++)
                    if (ml.moves[i].type != MOVE_WAIT) { seat = s; break; }
            }
            if (seat < 0) break;
            calculate_legal_moves(&gm, seat, &ml);
            const int pick = (int)(rnd() % (uint32_t)ml.n);
            const LegalMove *m = &ml.moves[pick];
            if (m->type == MOVE_WAIT) continue;
            AwireAction a;
            move_to_awire(m, &a);
            bool ok;
            switch (a.kind) {
                case AWIRE_ATTACK: ok = handle_attack(&gm, seat, a.cards, a.n); break;
                case AWIRE_COVER:  ok = handle_cover(&gm, seat, a.cards, a.attacks, a.n); break;
                case AWIRE_PASS:   ok = handle_pass(&gm, seat, a.cards, a.n); break;
                case AWIRE_PICKUP: ok = handle_pickup(&gm, seat); break;
                default:           ok = handle_good(&gm, seat); break;
            }
            if (!ok) continue;
            if (a.kind == AWIRE_GOOD && gm.num_battles > 0) {
                if (++goods_run >= 2) have_parent = 1;
            } else {
                goods_run = 0;
            }
        }
        if (!have_parent) continue;
        game_clone(&parent_g, &gm);

        int fs = -1, fp = -1;
        for (int s = 0; s < 4 && fs < 0; s++) {
            if (gm.players[s].status != PLAYER_STATUS_IN) continue;
            calculate_legal_moves(&gm, s, &ml);
            for (int i = 0; i < ml.n; i++) {
                const int t = ml.moves[i].type;
                if (t == MOVE_ATTACK || t == MOVE_COVER || t == MOVE_PASS) {
                    fs = s; fp = i; break;
                }
            }
        }
        if (fs < 0) continue;
        calculate_legal_moves(&gm, fs, &ml);
        AwireAction fa;
        move_to_awire(&ml.moves[fp], &fa);
        bool fok;
        switch (fa.kind) {
            case AWIRE_ATTACK: fok = handle_attack(&gm, fs, fa.cards, fa.n); break;
            case AWIRE_COVER:  fok = handle_cover(&gm, fs, fa.cards, fa.attacks, fa.n); break;
            default:           fok = handle_pass(&gm, fs, fa.cards, fa.n); break;
        }
        if (!fok) continue;

        MsgEnvelope ea;
        env_init(&ea, seed, 4);
        ea.game_id = 0x5000ULL + g;
        if (msg_seal(&ea, &parent_g, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK)
            continue;
        unsigned char wa[ENV_CAP];
        const int na = msg_encode(&ea, wa, sizeof(wa));
        if (na <= 0) continue;
        uint8_t da[SHA256_DIGEST_LEN];
        msg_digest(wa, na, da);

        MsgEnvelope eb;
        env_init(&eb, seed, 4);
        eb.game_id = ea.game_id;
        eb.last_actor_seat = (uint8_t)fs;
        memcpy(eb.parent8, da, MSG_PARENT_LEN);
        if (msg_seal(&eb, &gm, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK)
            continue;
        unsigned char wb[ENV_CAP];
        const int nb = msg_encode(&eb, wb, sizeof(wb));
        if (nb <= 0) continue;

        MsgChainKey ka, kb;
        CHECK(msg_chain_key(wa, na, &ka) == MSG_EOK, "inv %u: parent chain key failed", g);
        CHECK(msg_chain_key(wb, nb, &kb) == MSG_EOK, "inv %u: child chain key failed", g);
        // The premise: same round, child turn strictly BELOW the parent's (the
        // two folded goods minus the one follower atom).
        if (ka.round != kb.round || kb.turn >= ka.turn) continue;
        inv_cases++;
        CHECK(msg_rule_p(&ka, &kb) > 0,
              "inv %u: the parent (t%u) beat its own lower-turn child (t%u)",
              g, ka.turn, kb.turn);
        CHECK(msg_rule_p(&kb, &ka) < 0, "inv %u: rule P is not symmetric", g);
    }
    CHECK(inv_cases > 0, "rule P turn-inversion fixture built nothing");
    printf("  rule_p turn-inversion: %d posed inversions, all won by the child\n",
           inv_cases);
}

// ---------- 4. tamper matrix ---------------------------------------------

static void test_tamper(void) {
    uint8_t seed[MSG_SEED_LEN];
    seed_fill(seed, 7);
    g_rng = 7;
    Chain ch; memset(&ch, 0, sizeof(ch));
    Game played;
    const int rounds = play_game(seed, 4, 60, &ch, &played, -1);

    MsgEnvelope e;
    env_init(&e, seed, 4);
    const int over = game_done(&played) >= 0 || played.status == GAME_STATUS_GAME_OVER;
    e.phase = over ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE;
    static unsigned char body[1024];
    static Game scratch;
    (void)rounds;
    CHECK(msg_seal(&e, &played, MSG_NO_BASE, body, sizeof(body), &scratch) == MSG_EOK, "tamper seal failed");

    unsigned char wire[ENV_CAP];
    const int n = msg_encode(&e, wire, sizeof(wire));
    CHECK(n > 0, "tamper base encode failed");
    if (n <= 0) return;

    // (a) Every truncation must fail cleanly, never read past the buffer.
    //
    // The verdict is decode+replay, not decode alone. Cutting into the BODY
    // leaves a structurally perfect envelope — the body is the rest of the
    // buffer, and an entropy-coded integer has no framing for msg_decode to find
    // a hole in. A short code is simply a different (shorter) code. What catches
    // it is the header it no longer matches: `turn` and `round` are the chain's
    // claims, and msg_replay holds the body to them. That is the two-layer split
    // doing its job, so the test asserts the pair.
    for (int cut = 0; cut < n; cut++) {
        MsgEnvelope d;
        const int rc = msg_decode(wire, cut, &d);
        if (rc != MSG_EOK) continue;
        Game g;
        CHECK(msg_replay(&d, &g) != MSG_EOK,
              "truncation to %d/%d survived decode AND replay", cut, n);
    }

    // (b) Every single-byte flip must fail cleanly, or be CANONICAL: whatever
    //     survives decode must re-encode to exactly the bytes it came from.
    //
    //     That is the property worth pinning here. A surviving flip is not a
    //     break — flipping e.g. last_actor_seat or parent8 yields a valid, and
    //     merely different, envelope; game integrity is protected by the digest
    //     the receiver checks against parent8, not by decode refusing to read.
    //     What WOULD be a break is a byte the decoder silently ignores or
    //     normalizes, because then two distinct payloads share one digest and
    //     `parent8` stops identifying a unique parent — Rule P's tiebreak and
    //     the whole chain link rest on that. Re-encode is how a dropped field
    //     shows itself: the encoder would put the ORIGINAL byte back.
    int accepted = 0;
    for (int pos = 0; pos < n; pos++) {
        for (int bit = 0; bit < 8; bit++) {
            unsigned char t[ENV_CAP];
            memcpy(t, wire, (size_t)n);
            t[pos] ^= (unsigned char)(1 << bit);

            MsgEnvelope d;
            if (msg_decode(t, n, &d) != MSG_EOK) continue;
            accepted++;

            unsigned char again[ENV_CAP];
            const int an = msg_encode(&d, again, sizeof(again));
            CHECK(an == n && !memcmp(again, t, (size_t)n),
                  "flip at byte %d bit %d decoded but did not re-encode to itself "
                  "(a byte the decoder ignores)", pos, bit);

            // And replay must never crash on it, whatever it decides.
            Game g;
            (void)msg_replay(&d, &g);
        }
    }
    printf("  tamper: %d of %d single-bit flips decoded; all canonical, none crashed\n",
           accepted, n * 8);

    // (c) Header lies about the chain it carries: turn, round and phase are
    //     Rule P's inputs and are read BEFORE replay, so each must be pinned to
    //     the chain by validation.
    {
        MsgEnvelope bad = e; bad.turn = (uint16_t)(e.turn + 1);
        unsigned char w[ENV_CAP];
        CHECK(msg_encode(&bad, w, sizeof(w)) == MSG_ETURN, "inflated turn was encodable");
    }
    {
        MsgEnvelope bad = e; bad.round = (uint8_t)(e.round + 1);
        unsigned char w[ENV_CAP];
        const int wn = msg_encode(&bad, w, sizeof(w));
        CHECK(wn > 0, "round-lie encode should pass structure");
        MsgEnvelope d; Game g;
        CHECK(msg_decode(w, wn, &d) == MSG_EOK, "round-lie should decode");
        CHECK(msg_replay(&d, &g) == MSG_EROUND, "inflated round survived replay");
    }
    {
        MsgEnvelope bad = e;
        bad.phase = (e.phase == MSG_PHASE_FINISHED) ? MSG_PHASE_LIVE : MSG_PHASE_FINISHED;
        unsigned char w[ENV_CAP];
        const int wn = msg_encode(&bad, w, sizeof(w));
        MsgEnvelope d; Game g;
        if (wn > 0 && msg_decode(w, wn, &d) == MSG_EOK) {
            CHECK(msg_replay(&d, &g) == MSG_EPHASE, "lying phase survived replay");
        }
    }

    // (d) Field-level rejects, one per rule.
    //
    // The offsets that follow the header are taken from the format this seal
    // actually wrote, not from a constant: five live formats share one prefix
    // and differ in length, so a hardcoded n_joins offset would silently start
    // tampering with a name instead.
    const int hl = env_hdr_len(e.format);
    struct { const char *what; int off; unsigned char val; int want; } cases[] = {
        { "magic",        0,  0xF6, MSG_EMAGIC },
        // 3 is the CLOCK format, 4 the REMATCH format, 5/6 are those two
        // with the variant byte spent on the rules, and 7 is 6 plus the
        // rematch generation - so the first unknown byte above them is 8.
        // (2 through 7 are the whole wire.) Before format 7 existed this case
        // stamped 7 and got MSG_EFORMAT: that IS how a shipped build reads a
        // rematch bubble.
        { "format",       1,  MSG_FORMAT_GENERATION + 1, MSG_EFORMAT },
        { "format:raw",   1,  1,    MSG_EFORMAT },
        { "flags:fair",   2,  MSG_FLAG_FAIR_DEAL, MSG_EFLAGS },
        { "flags:gzip",   2,  MSG_FLAG_GZIP, MSG_EFLAGS },
        // bit2 (0x04) is the LEGACY passing-allowed marker (1.0(3)); no longer
        // set, but still TOLERATED on decode so a 1.0(3) bubble still opens. bit3
        // (0x08) is still reserved and still rejected.
        { "flags:legacy", 2,  0x04, MSG_EOK },
        { "flags:rsvd",   2,  0x08, MSG_EFLAGS },
        { "phase:oob",    3,  4,    MSG_EPHASE },
        { "phase:accept", 3,  MSG_PHASE_ACCEPT, MSG_EPHASE },
        { "n_players:1",  15, 1,    MSG_EPLAYERS },
        { "n_players:9",  15, 9,    MSG_EPLAYERS },
        // The variant byte's known bit (passing) is legal on this format and
        // means the game this chain really is; a bit this build does not
        // implement is refused rather than masked off, because honouring only
        // the half we understand deals a different game from the sender's.
        { "variant:rsvd", 16, 0x02, MSG_EVARIANT },
        { "actor seat",   14, 4,    MSG_ESEAT },
        { "n_joins:0",    hl - 1, 0, MSG_EJOINS },
        { "n_joins:9",    hl - 1, 9, MSG_EJOINS },
    };
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        unsigned char t[ENV_CAP];
        memcpy(t, wire, (size_t)n);
        t[cases[i].off] = cases[i].val;
        MsgEnvelope d;
        const int rc = msg_decode(t, n, &d);
        CHECK(rc == cases[i].want, "%s: got %d want %d", cases[i].what, rc, cases[i].want);
    }

    // (e) An all-zero seed is a dead deal, not a game.
    {
        unsigned char t[ENV_CAP];
        memcpy(t, wire, (size_t)n);
        memset(t + 26, 0, MSG_SEED_LEN);
        MsgEnvelope d;
        CHECK(msg_decode(t, n, &d) == MSG_ESEED, "all-zero seed accepted");
    }

    // (f) A non-printable nickname byte.
    {
        unsigned char t[ENV_CAP];
        memcpy(t, wire, (size_t)n);
        t[hl + 2] = 0x01;   // first byte of the first join's name
        MsgEnvelope d;
        CHECK(msg_decode(t, n, &d) == MSG_ENAME, "control byte in name accepted");
    }
}

// ---------- 8. the send clock and the pickup hold (round 16) -------------
//
// The wire half (format 3 carries two bytes and format 2 still does not) and
// the rule half (who is held, for how long, and every way the hold is waived).
// One section because the two only mean anything together: a clock nothing
// reads is dead weight, and a rule with no clock can never fire.

// Drive a game to a state whose LAST log is `want`, so the hold has something
// to hold (LOG_ATTACK) or deliberately nothing (LOG_COVER). Returns 0 if no
// seed in the search produced one, which would itself be a bug worth failing on.
static int drive_to_last_log(uint32_t seed0, int np, int want, Game *end) {
    for (uint32_t gi = 0; gi < 300; gi++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, seed0 + gi * 131u);
        for (int cut = 1; cut <= 24; cut++) {
            Chain ch; memset(&ch, 0, sizeof(ch));
            g_rng = seed0 + gi;
            play_game(seed, np, cut, &ch, end, -1);
            if (end->num_logs > 0 && end->logs[end->num_logs - 1].log_type == want) return 1;
        }
    }
    return 0;
}

static int uncovered_count(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_battles; i++) {
        if (card_is_none(g->table_battles[i].defense)) n++;
    }
    return n;
}

static void test_pickup_hold(void) {
    Game g;
    if (!drive_to_last_log(9001u, 4, LOG_ATTACK, &g)) {
        CHECK(0, "no seed in the search reached a fresh attack");
        return;
    }
    const int def = g.defender;
    // The search can land on a table the defender cannot be thrown at (the
    // capacity waiver's own case); give it room so the timing cases below are
    // testing the timing and not the waiver.
    if (g.players[def].hand_count <= uncovered_count(&g)) {
        g.players[def].hand_count = (int8_t)(uncovered_count(&g) + 1);
    }

    // (a) The clock runs. 15 seconds owed at the instant it was sent, one
    //     second at 14, and nothing from 15 on - including long after.
    CHECK(msg_pickup_hold_remaining(&g, def, 1000, 1000) == MSG_PICKUP_HOLD_S,
          "no hold at the moment of the attack");
    CHECK(msg_pickup_hold_remaining(&g, def, 1000, 1001) == MSG_PICKUP_HOLD_S - 1,
          "hold did not tick down");
    CHECK(msg_pickup_hold_remaining(&g, def, 1000, 1014) == 1, "hold ended early");
    CHECK(msg_pickup_hold_remaining(&g, def, 1000, 1015) == 0, "hold outlasted 15s");
    CHECK(msg_pickup_hold_remaining(&g, def, 1000, 9999) == 0, "hold outlasted the bout");

    // (b) NO CLOCK, NO HOLD. This is the whole of backward compatibility: a
    //     format-2 chain from a shipped build decodes to sent_at 0, and a
    //     defender reading it may pick up exactly as they always could.
    // `now` is deliberately SMALL here: a stale clock of 0 against a clock of
    // 1000 is 1000 seconds elapsed, which the timing arithmetic would release
    // on its own, so that pair proves nothing about the guard. Three seconds
    // after a zero stamp is the case where only the guard can answer.
    CHECK(msg_pickup_hold_remaining(&g, def, 0, 3) == 0, "a clockless chain held");
    CHECK(msg_pickup_hold_remaining(&g, def, 0, 1000) == 0, "a clockless chain held");

    // (c) Only the defender is held - nobody else can pick up at all.
    for (int s = 0; s < g.num_players; s++) {
        if (s == def) continue;
        CHECK(msg_pickup_hold_remaining(&g, s, 1000, 1000) == 0, "seat %d held, not defending", s);
    }
    CHECK(msg_pickup_hold_remaining(&g, -1, 1000, 1000) == 0, "a bogus seat was held");
    CHECK(msg_pickup_hold_remaining(&g, g.num_players, 1000, 1000) == 0, "an out-of-range seat was held");

    // (d) THE CAPACITY WAIVER (owner): as many uncovered cards on the table as
    //     the defender has cards, and no throw-in is possible from anyone - so
    //     holding the defender protects nothing.
    {
        Game c = g;
        c.players[def].hand_count = (int8_t)uncovered_count(&c);
        CHECK(msg_pickup_hold_remaining(&c, def, 1000, 1000) == 0,
              "held a defender at capacity (%d uncovered, %d in hand)",
              uncovered_count(&c), c.players[def].hand_count);
        // One card of slack is the boundary: now a throw-in IS possible.
        c.players[def].hand_count = (int8_t)(uncovered_count(&c) + 1);
        CHECK(msg_pickup_hold_remaining(&c, def, 1000, 1000) == MSG_PICKUP_HOLD_S,
              "no hold with a card of capacity to spare");
    }

    // (e) THE CLOCK WRAPS, and unsigned subtraction is why that costs nothing.
    //     65534 -> 1 is three seconds across the rollover, not 65,533 backwards.
    CHECK(msg_pickup_hold_remaining(&g, def, 65534, 1) == MSG_PICKUP_HOLD_S - 3,
          "the hold did not survive a clock rollover");

    // (f) A stamp from the FUTURE (a sender whose clock runs fast) releases the
    //     hold instead of maxing it - the safe direction, and the only one that
    //     cannot wedge a defender behind a stranger's bad clock.
    CHECK(msg_pickup_hold_remaining(&g, def, 1000, 995) == 0, "a future stamp held the defender");

    // (g) A defender who has already COVERED is not facing anything new.
    {
        Game c;
        if (drive_to_last_log(4242u, 4, LOG_COVER, &c)) {
            CHECK(msg_pickup_hold_remaining(&c, c.defender, 1000, 1000) == 0,
                  "held after a cover, with no new attack to wait on");
        }
    }
}

// ---------- round 16: the bubble delta -------------------------------------
//
// n_new (msg_wire.h) is how many atoms ONE bubble added to the chain, and it is
// the whole reason a receiver can animate the move it just opened instead of
// that move plus the one before it. The owner's report: "a defender covers a
// single card, sends it, then covers a second card, and sends that. If anyone
// opens the bubble for the second cover, they will see BOTH covers animate."
//
// This plays a game ONE ACTION PER BUBBLE - the exact shape that used to be
// indistinguishable from one bubble holding the lot - and pins three things per
// bubble: the delta counts what THIS seal added and nothing earlier, it
// survives the wire, and the step stream it indexes into really is "the deal,
// then one step per atom" (`replay_steps_count_v6 == turn + 1`). That last one
// is load-bearing and invisible: the reader takes the last n_new STEPS, so if
// steps ever stopped being 1:1 with atoms the group would silently slide onto
// the wrong moves.
// One bubble's atoms, as (kind, seat) pairs - enough to say WHICH action each
// one is without re-implementing the codec.
typedef struct { int kind, seat; } DAtom;
typedef struct { DAtom a[MSG_MAX_ACTIONS]; int n; } DAtoms;

static void datom_sink(void *ctx, const ReplayAtom *a) {
    DAtoms *d = (DAtoms *)ctx;
    if (a->kind == REPLAY_ATOM_DEAL || a->kind == REPLAY_ATOM_DRAW) return;
    if (d->n >= MSG_MAX_ACTIONS) return;
    d->a[d->n].kind = a->kind;
    d->a[d->n].seat = a->seat;
    d->n++;
}

static int datoms_of(const unsigned char *body, int len, DAtoms *out) {
    out->n = 0;
    ReplayHeader hdr;
    return replay_decode_atoms_v6(body, len, &hdr, datom_sink, out);
}

static void test_bubble_delta(void) {
    int bubbles = 0, multi = 0, superseded = 0, expanded = 0;
    for (int np = 2; np <= 4; np++) {
        for (int gi = 0; gi < 8; gi++) {
            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, 4100u + (uint32_t)(gi * 733 + np * 17));
            g_rng = 991u + (uint32_t)(gi * 37 + np);
            random_strategy_set_seed(g_rng);
            game_set_deal_seed_bytes(seed, MSG_SEED_LEN);

            Game g;
            memset(&g, 0, sizeof(g));
            g.num_players = (int8_t)np;
            for (int i = 0; i < np; i++) {
                g.players[i].status = PLAYER_STATUS_READY;
                g.players[i].strategy_key = 0;
            }
            start_game(&g);

            // A genesis continues nothing, so its mark is its own log count -
            // not MSG_NO_BASE, which means "cannot say".
            int base_logs = g.num_logs;
            DAtoms parent;   // the atoms the bubble before this one carried
            parent.n = 0;
            int have_parent = 0;
            static LegalMoves ml;
            for (int step = 0; step < 60; step++) {
                if (game_done(&g) >= 0 || g.status != GAME_STATUS_PLAYING) break;

                // ONE TO THREE actions, then SEND: a turn is what one human
                // staged before pressing send, which is not always one action
                // (a defender covers twice; an attacker throws in and says
                // good). The single-action bubble is the shape that first
                // needed a delta at all, and the multi-action one is the shape
                // that says whether the delta is MEASURED or merely subtracted.
                const int want_actions = 1 + (int)(rnd() % 3u);
                int staged = 0;
                for (int k = 0; k < want_actions; k++) {
                    if (game_done(&g) >= 0 || g.status != GAME_STATUS_PLAYING) break;
                    int seat = -1;
                    const int start = (int)(rnd() % (uint32_t)np);
                    for (int t = 0; t < np && seat < 0; t++) {
                        const int s = (start + t) % np;
                        if (g.players[s].status != PLAYER_STATUS_IN) continue;
                        calculate_legal_moves(&g, s, &ml);
                        for (int i = 0; i < ml.n; i++)
                            if (ml.moves[i].type != MOVE_WAIT) { seat = s; break; }
                    }
                    if (seat < 0) break;
                    calculate_legal_moves(&g, seat, &ml);
                    int pick = -1;
                    for (int t = 0; t < ml.n && pick < 0; t++) {
                        const int i = (int)((rnd() + (uint32_t)t) % (uint32_t)ml.n);
                        if (ml.moves[i].type != MOVE_WAIT) pick = i;
                    }
                    if (pick < 0) break;

                    AwireAction a;
                    move_to_awire(&ml.moves[pick], &a);
                    bool ok;
                    switch (a.kind) {
                        case AWIRE_ATTACK: ok = handle_attack(&g, seat, a.cards, a.n); break;
                        case AWIRE_COVER:  ok = handle_cover(&g, seat, a.cards, a.attacks, a.n); break;
                        case AWIRE_PASS:   ok = handle_pass(&g, seat, a.cards, a.n); break;
                        case AWIRE_PICKUP: ok = handle_pickup(&g, seat); break;
                        default:           ok = handle_good(&g, seat); break;
                    }
                    if (ok) staged++;
                }
                if (staged == 0) break;

                // …and SEND.
                MsgEnvelope e;
                env_init(&e, seed, np);
                const int over = game_done(&g) >= 0 || g.status == GAME_STATUS_GAME_OVER;
                e.phase = over ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE;
                static unsigned char body[1024];
                static Game scratch;
                if (msg_seal(&e, &g, msg_seal_base(&g, base_logs),
                             body, sizeof(body), &scratch) != MSG_EOK) break;
                if (e.n_actions == 0) continue;   // nothing sealed yet (pre-opening)

                bubbles++;
                if (staged > 1) multi++;
                if (e.n_new > staged) expanded++;
                // Every bubble on a known base claims SOMETHING: a bubble that
                // said nothing would be animated by the fallback guess, which
                // is what this whole field exists to stop doing.
                CHECK(e.n_new >= 1 && e.n_new != MSG_NEW_NOTHING,
                      "np=%d game=%d: a real move sealed a delta of %d", np, gi, e.n_new);
                // A delta alone is enough to need the clock header, and every
                // seal now writes the RULES format (5), which is that header.
                CHECK(e.format == MSG_FORMAT_RULES,
                      "np=%d game=%d: a delta sealed as format %d", np, gi, e.format);

                // THE INVARIANT the reader indexes on: deal + one step per atom.
                const int steps = replay_steps_count_v6(e.actions, e.actions_len, NULL);
                CHECK(steps == (int)e.turn + 1,
                      "np=%d game=%d: %d steps for %d atoms (the group would slide)",
                      np, gi, steps, e.turn);

                // AND WHAT THE DELTA MEANS. `atoms_before` is where the reader
                // opens the animation, so everything before it must be history
                // the PARENT bubble already carried, and everything from it on
                // must be this turn. The two are pinned against the parent's
                // own atoms rather than against arithmetic:
                //
                //   - the shared prefix is IDENTICAL (a chain re-encodes its
                //     past the same way), and
                //   - the only atoms of the parent that may fall outside the
                //     prefix are GOODs, which stop being atoms as soon as
                //     anything follows them (replay_atoms_before_log). An
                //     attack, cover, pickup or round_end that fell off the
                //     front would be a move nobody ever animates.
                DAtoms child;
                CHECK(datoms_of(e.actions, e.actions_len, &child) >= 0,
                      "np=%d game=%d: the sealed body did not decode", np, gi);
                CHECK(child.n == (int)e.turn,
                      "np=%d game=%d: %d atoms for turn %d", np, gi, child.n, e.turn);
                const int atoms_before = (int)e.turn - (int)e.n_new;
                CHECK(atoms_before >= 0,
                      "np=%d game=%d: a delta of %d past turn %d", np, gi, e.n_new, e.turn);
                if (have_parent) {
                    CHECK(atoms_before <= parent.n,
                          "np=%d game=%d: the bubble starts at %d, past the parent's %d atoms",
                          np, gi, atoms_before, parent.n);
                    for (int i = 0; i < atoms_before; i++)
                        CHECK(child.a[i].kind == parent.a[i].kind
                              && child.a[i].seat == parent.a[i].seat,
                              "np=%d game=%d: atom %d of the history changed (%d/%d -> %d/%d)",
                              np, gi, i, parent.a[i].kind, parent.a[i].seat,
                              child.a[i].kind, child.a[i].seat);
                    for (int i = atoms_before; i < parent.n; i++) {
                        CHECK(parent.a[i].kind == REPLAY_ATOM_GOOD,
                              "np=%d game=%d: this bubble claims the parent's atom %d, "
                              "a %d - only a superseded good may be re-claimed",
                              np, gi, i, parent.a[i].kind);
                        superseded++;
                    }
                } else {
                    CHECK(atoms_before == 0,
                          "np=%d game=%d: the first bubble of a chain disowned %d atoms",
                          np, gi, atoms_before);
                }

                unsigned char wire[ENV_CAP];
                const int n = msg_encode(&e, wire, sizeof(wire));
                CHECK(n > 0, "np=%d game=%d: delta encode failed %d", np, gi, n);
                MsgEnvelope d;
                CHECK(msg_decode(wire, n, &d) == MSG_EOK, "np=%d game=%d: delta decode failed", np, gi);
                CHECK(d.n_new == e.n_new, "np=%d game=%d: the delta did not survive the wire (%d -> %d)",
                      np, gi, e.n_new, d.n_new);

                parent = child;
                have_parent = 1;
                base_logs = g.num_logs;   // what adopting my own bubble leaves
            }
        }
    }
    CHECK(bubbles > 100, "only %d bubbles built; this pinned little", bubbles);
    CHECK(multi > 20, "only %d bubbles staged more than one action", multi);
    // The case the log mark exists for: a bubble whose turn superseded a good
    // the parent had counted as an atom. Subtracting atom counts loses exactly
    // these, and the front of the turn goes with them.
    CHECK(superseded > 0,
          "no bubble ever superseded a pending good - the case the mark exists for never happened");
    printf("  bubble delta: %d bubbles (%d multi-action), %d goods superseded, "
           "%d turns the codec expanded\n", bubbles, multi, superseded, expanded);

    // A delta the chain cannot back is not an envelope: it would point the
    // animation group at steps before the deal.
    uint8_t seed[MSG_SEED_LEN];
    seed_fill(seed, 4242);
    Chain ch; memset(&ch, 0, sizeof(ch));
    Game played;
    g_rng = 4242;
    play_game(seed, 2, 30, &ch, &played, -1);
    MsgEnvelope e;
    env_init(&e, seed, 2);
    static unsigned char body[1024];
    static Game scratch;
    CHECK(msg_seal(&e, &played, 0, body, sizeof(body), &scratch) == MSG_EOK, "delta-cap seal failed");
    CHECK(e.n_new == (uint8_t)e.turn || e.turn > MSG_MAX_NEW,
          "a genesis seal did not claim the whole chain");
    unsigned char w[ENV_CAP];
    e.n_new = (uint8_t)(e.turn + 1);
    CHECK(msg_encode(&e, w, sizeof(w)) == MSG_ETURN, "a delta past the chain encoded");
}

// ---------- round 16: the bubble that adds NOTHING -------------------------
//
// The owner: "if you stage a move then undo, you can still send a message and
// it will look weird for the other players. Sometimes even play a weird undo
// animation I think." Messages has no API to REMOVE a staged bubble, so §10
// cancels a staged move by overwriting it with a re-seal of the board the chain
// was already in. That bubble is real, it is sendable, and it carries no move -
// and until now it claimed a delta of 1, so every recipient replayed the
// PREVIOUS player's move as if it had just arrived.
//
// What makes it hard is that "the chain did not grow" is NOT the tell: 311 of
// the 1440 bubbles above are real moves the codec folded into a round_end atom,
// and they hand back their parent's atom count too. So the fact comes from the
// host, through msg_seal_base, which reads it off the GAME (its log count is
// where adoption left it), and lands on the wire as its own value.
//
// Pinned here: the three-way discrimination (a move, a folded move, nothing),
// that the sentinel survives the wire, and - the part that is the actual bug -
// that the suffix a receiver animates from such a bubble is EMPTY.
static void test_nothing_bubble(void) {
    int nothings = 0, folds_kept = 0;
    for (int np = 2; np <= 4; np++) {
        for (int gi = 0; gi < 6; gi++) {
            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, 7700u + (uint32_t)(gi * 911 + np * 23));
            Chain ch; memset(&ch, 0, sizeof(ch));
            Game g;
            g_rng = 313u + (uint32_t)(gi * 41 + np);
            play_game(seed, np, 12 + gi * 5, &ch, &g, -1);

            MsgEnvelope e;
            env_init(&e, seed, np);
            static unsigned char body[1024];
            static Game scratch;
            if (msg_seal(&e, &g, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK) continue;
            if (e.n_actions == 0) continue;

            // THE ADOPTION. A receiver replayed this chain into its own game;
            // the atom count and the log count are what it remembers of that
            // moment. (Here the game IS the one that was played, which is the
            // same thing a replay would produce - msg_replay is how the phone
            // gets one.)
            const int base_turn = (int)e.turn;
            const int base_logs = g.num_logs;

            // …and then the human staged a move and undid it, so nothing was
            // applied. This is the seal that used to lie.
            CHECK(msg_seal_base(&g, base_logs) == MSG_BASE_NOTHING,
                  "np=%d game=%d: an untouched game did not read as empty", np, gi);
            MsgEnvelope z;
            env_init(&z, seed, np);
            z.sent_at = 0x1111;
            CHECK(msg_seal(&z, &g, MSG_BASE_NOTHING, body, sizeof(body), &scratch) == MSG_EOK,
                  "np=%d game=%d: the empty re-seal failed", np, gi);
            CHECK(z.n_new == MSG_NEW_NOTHING,
                  "np=%d game=%d: an empty bubble claimed a delta of %d", np, gi, z.n_new);
            CHECK(z.turn == (uint16_t)base_turn,
                  "np=%d game=%d: an empty bubble moved the chain (%d -> %d)",
                  np, gi, base_turn, z.turn);
            nothings++;

            // IT IS AN ENVELOPE. The sentinel is above `turn` on any short
            // chain, so the bound `turn` puts on a real delta has to exempt it
            // or the bubble would not decode at all - which would be a worse
            // bug than the one being fixed.
            unsigned char wire[ENV_CAP];
            const int n = msg_encode(&z, wire, sizeof(wire));
            CHECK(n > 0, "np=%d game=%d: an empty bubble did not encode (%d)", np, gi, n);
            MsgEnvelope d;
            CHECK(msg_decode(wire, n, &d) == MSG_EOK,
                  "np=%d game=%d: an empty bubble did not decode", np, gi);
            CHECK(d.n_new == MSG_NEW_NOTHING,
                  "np=%d game=%d: the sentinel did not survive the wire (%d)", np, gi, d.n_new);

            // THE POINT OF ALL OF IT: nothing animates. The reader opens the
            // step stream at `atoms_before + 1` (fio_replay_last_events_packed),
            // and for this bubble atoms_before IS the atom count - one past the
            // last step, so the suffix is empty. Asked the way the phone asks
            // it, against the frame writer itself, rather than by re-deriving
            // the arithmetic and agreeing with myself.
            const int steps = replay_steps_count_v6(d.actions, d.actions_len, NULL);
            CHECK(steps == (int)d.turn + 1,
                  "np=%d game=%d: %d steps for %d atoms", np, gi, steps, d.turn);
            const int atoms_before = (int)d.turn;   // what MessageEnvelope.atomsBefore yields
            static unsigned char frames[65536];
            int n_frames = -1, next_step = 0;
            const int fr = replay_steps_frames_v6(d.actions, d.actions_len, -1,
                                                  atoms_before + 1, 0,
                                                  frames, sizeof(frames), &n_frames, &next_step);
            CHECK(fr >= 0, "np=%d game=%d: the empty suffix errored (%d)", np, gi, fr);
            CHECK(n_frames == 0,
                  "np=%d game=%d: an empty bubble animated %d frames", np, gi, n_frames);

            // AND THE OTHER SIDE OF THE DISCRIMINATION. Play ONE more action
            // and the same host reads the same game as having moved - including
            // when the codec folds it and the atom count does not change, which
            // is the case that makes this fact unmeasurable from the wire.
            static LegalMoves ml;
            int seat = -1, pick = -1;
            for (int s = 0; s < np && seat < 0; s++) {
                if (g.players[s].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, s, &ml);
                for (int i = 0; i < ml.n; i++)
                    if (ml.moves[i].type != MOVE_WAIT) { seat = s; pick = i; break; }
            }
            if (seat < 0) continue;
            AwireAction a;
            move_to_awire(&ml.moves[pick], &a);
            bool ok;
            switch (a.kind) {
                case AWIRE_ATTACK: ok = handle_attack(&g, seat, a.cards, a.n); break;
                case AWIRE_COVER:  ok = handle_cover(&g, seat, a.cards, a.attacks, a.n); break;
                case AWIRE_PASS:   ok = handle_pass(&g, seat, a.cards, a.n); break;
                case AWIRE_PICKUP: ok = handle_pickup(&g, seat); break;
                default:           ok = handle_good(&g, seat); break;
            }
            if (!ok) continue;
            CHECK(msg_seal_base(&g, base_logs) == base_logs,
                  "np=%d game=%d: a played move still read as empty", np, gi);
            MsgEnvelope m2;
            env_init(&m2, seed, np);
            const int over = game_done(&g) >= 0 || g.status == GAME_STATUS_GAME_OVER;
            m2.phase = over ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE;
            m2.last_actor_seat = (uint8_t)seat;
            if (msg_seal(&m2, &g, msg_seal_base(&g, base_logs),
                         body, sizeof(body), &scratch) != MSG_EOK) continue;
            CHECK(m2.n_new >= 1 && m2.n_new != MSG_NEW_NOTHING,
                  "np=%d game=%d: a real move sealed as %d", np, gi, m2.n_new);
            if (m2.turn <= (uint16_t)base_turn) folds_kept++;
        }
    }
    CHECK(nothings >= 10, "only %d empty bubbles built; this pinned little", nothings);
    // The discrimination is only worth anything if the ambiguous case actually
    // occurred: a real move whose chain did not grow, still claiming its atom.
    CHECK(folds_kept >= 1,
          "no folded move was ever sealed - the case this fix has to tell apart never happened");
    printf("  nothing bubble: %d empty re-seals, %d folded real moves kept their delta\n",
           nothings, folds_kept);

    // A HOST THAT NEVER LOOKED cannot claim emptiness: no log mark (-1) means
    // the ordinary base, and no base at all still means "cannot say". Both
    // matter because every path that makes a game resident without adopting a
    // chain leaves one of them unset.
    uint8_t seed[MSG_SEED_LEN];
    seed_fill(seed, 8801);
    Chain ch; memset(&ch, 0, sizeof(ch));
    Game g;
    g_rng = 8801;
    play_game(seed, 2, 20, &ch, &g, -1);
    CHECK(msg_seal_base(&g, MSG_NO_BASE) == MSG_NO_BASE,
          "a game with no mark claimed to be empty");
    CHECK(msg_seal_base(&g, g.num_logs - 1) == g.num_logs - 1,
          "a game that moved past its mark claimed to be empty");
}

// ---------- Rule F: the fool's penalty ------------------------------------

// Build a joins array from a list of names, seated 0..n-1 in the order given.
static void joins_of(MsgJoin *j, const char *const *names, int n) {
    for (int i = 0; i < n; i++) {
        j[i].seat = (uint8_t)i;
        int k = 0;
        while (names[i][k] && k < MSG_MAX_NAME) { j[i].name[k] = names[i][k]; k++; }
        j[i].name_len = (uint8_t)k;
    }
}

static uint32_t key_of(const char *const *names, int n) {
    MsgJoin j[MSG_MAX_JOINS];
    joins_of(j, names, n);
    uint32_t k = 0;
    CHECK(msg_roster_key(j, n, &k, 0) == MSG_EOK, "roster key failed");
    return k;
}

// The key is a property of the CYCLE, not of the seating: every rotation of one
// table keys the same, and any order a rotation cannot produce keys different.
static void test_roster_key(void) {
    const char *abc[] = { "Alex", "Bob", "Cindy" };
    const char *bca[] = { "Bob", "Cindy", "Alex" };
    const char *cab[] = { "Cindy", "Alex", "Bob" };
    const char *acb[] = { "Alex", "Cindy", "Bob" };   // NOT a rotation of abc

    const uint32_t k = key_of(abc, 3);
    CHECK(k != 0, "a roster key may never be 0 (the wire's 'no carry')");
    CHECK(key_of(bca, 3) == k, "a rotation changed the key");
    CHECK(key_of(cab, 3) == k, "a rotation changed the key");
    CHECK(key_of(acb, 3) != k, "a reordering kept the key");

    // A different table, and a different size, are different rosters.
    const char *abd[] = { "Alex", "Bob", "Dina" };
    const char *abcd[] = { "Alex", "Bob", "Cindy", "Dina" };
    CHECK(key_of(abd, 3) != k, "a renamed player kept the key");
    CHECK(key_of(abcd, 4) != k, "a joiner kept the key");

    // Arrival order is not seating order: joins may be appended in any order
    // and must still key by where people SIT.
    MsgJoin shuffled[3];
    joins_of(shuffled, abc, 3);
    MsgJoin tmp = shuffled[0]; shuffled[0] = shuffled[2]; shuffled[2] = tmp;
    uint32_t ks = 0;
    CHECK(msg_roster_key(shuffled, 3, &ks, 0) == MSG_EOK, "shuffled joins failed");
    CHECK(ks == k, "arrival order changed the key");

    // The rotation offset is the mapping the carry rides on:
    // canonical[k] == seated[(k + rot) % n]. What has to hold is that ONE
    // PERSON lands on ONE canonical index no matter how the table is rotated -
    // that invariant, not any particular ordering of names, is what lets
    // carry_fool name the same human across a re-seating.
    MsgJoin j[MSG_MAX_JOINS];
    int rot_abc = -1, rot_bca = -1;
    uint32_t ignore = 0;
    joins_of(j, abc, 3); msg_roster_key(j, 3, &ignore, &rot_abc);
    joins_of(j, bca, 3); msg_roster_key(j, 3, &ignore, &rot_bca);
    CHECK(rot_abc >= 0 && rot_bca >= 0, "no rotation reported");
    for (int seat = 0; seat < 3; seat++) {
        // The same person, found by name in each seating.
        const char *who = abc[seat];
        int seat_in_bca = -1;
        for (int t = 0; t < 3; t++) if (!strcmp(bca[t], who)) seat_in_bca = t;
        CHECK(seat_in_bca >= 0, "%s vanished from the rotated roster", who);
        const int idx_abc = ((seat - rot_abc) % 3 + 3) % 3;
        const int idx_bca = ((seat_in_bca - rot_bca) % 3 + 3) % 3;
        CHECK(idx_abc == idx_bca,
              "%s is canonical %d seated one way and %d the other", who, idx_abc, idx_bca);
    }
}

// The verdict: right of the fool, through any rotation, and off entirely the
// moment the table is not the same table.
static void test_rematch_opening(void) {
    const char *abc[] = { "Alex", "Bob", "Cindy" };
    MsgJoin j[MSG_MAX_JOINS];
    joins_of(j, abc, 3);

    uint32_t key = 0;
    int rot = 0;
    msg_roster_key(j, 3, &key, &rot);

    // Bob (seat 1) was the fool. Canonical index of seat 1 is 1 - rot.
    for (int fool_seat = 0; fool_seat < 3; fool_seat++) {
        const uint8_t fool_idx = (uint8_t)(((fool_seat - rot) % 3 + 3) % 3);
        const int want = (fool_seat - 1 + 3) % 3;
        const int got = msg_rematch_opening(j, 3, key, fool_idx);
        CHECK(got == want, "fool at %d: opened %d, want %d (right of the fool)",
              fool_seat, got, want);
        // …and the fool is therefore the first DEFENDER, which is the whole
        // point of the rule.
        CHECK((got + 1) % 3 == fool_seat, "the fool is not the first defender");
    }

    // The same cycle, rotated (Bob's device created the lobby, so Bob sits 0):
    // the same person must still be punished.
    const char *bca[] = { "Bob", "Cindy", "Alex" };
    MsgJoin jr[MSG_MAX_JOINS];
    joins_of(jr, bca, 3);
    {
        // Bob was the fool; in the ORIGINAL seating that was seat 1.
        const uint8_t fool_idx = (uint8_t)(((1 - rot) % 3 + 3) % 3);
        const int got = msg_rematch_opening(jr, 3, key, fool_idx);
        CHECK(got >= 0, "a rotated roster lost the penalty");
        // Bob now sits at 0, so the opener must be seat 2 (Alex), and Bob is
        // the defender.
        CHECK(got == 2, "rotated: opened %d, want 2", got);
        CHECK((got + 1) % 3 == 0, "rotated: the fool is not the first defender");
    }

    // The guard. Each of these is "the players changed", and each must switch
    // the rule off rather than punish the wrong person.
    {
        const char *acb[] = { "Alex", "Cindy", "Bob" };      // reordered
        const char *abd[] = { "Alex", "Bob", "Dina" };       // renamed / replaced
        const char *abcd[] = { "Alex", "Bob", "Cindy", "D" };// joined
        MsgJoin t[MSG_MAX_JOINS];
        joins_of(t, acb, 3);
        CHECK(msg_rematch_opening(t, 3, key, 0) == -1, "a reorder kept the penalty");
        joins_of(t, abd, 3);
        CHECK(msg_rematch_opening(t, 3, key, 0) == -1, "a rename kept the penalty");
        joins_of(t, abcd, 4);
        CHECK(msg_rematch_opening(t, 4, key, 0) == -1, "a joiner kept the penalty");
    }

    // No carry is no penalty, in both of its shapes.
    CHECK(msg_rematch_opening(j, 3, 0, 0) == -1, "key 0 applied a penalty");
    CHECK(msg_rematch_opening(j, 3, key, MSG_NO_FOOL) == -1, "no fool applied a penalty");
}

// The wire half: a pinned opening survives seal -> bytes -> decode -> re-deal,
// and a chain that lies about it does not replay.
static void test_fool_penalty_wire(void) {
    for (int np = 2; np <= 5; np++) {
        for (int fool = 0; fool < np; fool++) {
            const int opening = (fool - 1 + np) % np;

            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, 7700u + (uint32_t)(np * 31 + fool));

            // Deal the rematch the way msg_replay will: pinned.
            game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
            Game g;
            memset(&g, 0, sizeof(g));
            g.num_players = (int8_t)np;
            for (int i = 0; i < np; i++) {
                g.players[i].status = PLAYER_STATUS_READY;
                g.players[i].strategy_key = 0;
            }
            game_open_at_seat(opening);
            start_game(&g);
            game_open_at_seat(-1);

            CHECK(g.first_attacker == (int8_t)opening,
                  "np=%d: the pin did not take (%d, want %d)",
                  np, g.first_attacker, opening);
            CHECK(g.defender == (int8_t)fool,
                  "np=%d: the fool is not the first defender (%d, want %d)",
                  np, g.defender, fool);

            // One real move, so the chain has a body to check the opener
            // against, and it must come from the SEAT THE PENALTY NAMED.
            static LegalMoves ml;
            calculate_legal_moves(&g, opening, &ml);
            int pick = -1;
            for (int i = 0; i < ml.n && pick < 0; i++)
                if (ml.moves[i].type == MOVE_ATTACK) pick = i;
            CHECK(pick >= 0, "np=%d: the pinned opener has no attack", np);
            if (pick < 0) continue;
            AwireAction a;
            move_to_awire(&ml.moves[pick], &a);
            CHECK(handle_attack(&g, opening, a.cards, a.n), "np=%d: opener refused", np);

            MsgEnvelope e;
            env_init(&e, seed, np);
            e.last_actor_seat = (uint8_t)opening;
            e.phase = MSG_PHASE_LIVE;
            e.opening = (uint8_t)opening;
            static unsigned char body[1024];
            static Game scratch;
            CHECK(msg_seal(&e, &g, 0, body, sizeof(body), &scratch) == MSG_EOK,
                  "np=%d: rematch seal failed", np);
            CHECK(e.format == MSG_FORMAT_RULES_REMATCH,
                  "np=%d: a pinned opening did not seal the rematch format (got %d)",
                  np, e.format);

            unsigned char wire[ENV_CAP];
            const int n = msg_encode(&e, wire, sizeof(wire));
            CHECK(n > 0, "np=%d: rematch encode failed (%d)", np, n);
            if (n <= 0) continue;

            MsgEnvelope d;
            CHECK(msg_decode(wire, n, &d) == MSG_EOK, "np=%d: rematch decode failed", np);
            CHECK(d.opening == (uint8_t)opening,
                  "np=%d: the opening seat did not survive the wire (%d)", np, d.opening);
            CHECK(d.carry_key == 0 && d.carry_fool == MSG_NO_FOOL,
                  "np=%d: a live bubble carried a lobby's question", np);

            Game rebuilt;
            CHECK(msg_replay(&d, &rebuilt) == MSG_EOK, "np=%d: rematch replay failed", np);

            // THE POINT: a device holding only these bytes deals the same board.
            for (int s = 0; s < np; s++)
                CHECK(rebuilt.players[s].hand_count == g.players[s].hand_count,
                      "np=%d seat %d: the re-dealt hand differs", np, s);

            // A chain that drops the penalty deals a DIFFERENT game, and its own
            // body no longer fits. Only meaningful when the lowest trump would
            // have opened somewhere else, which is the interesting case anyway.
            {
                MsgEnvelope t = d;
                t.opening = MSG_NO_OPENING;
                t.format  = MSG_FORMAT_CLOCK;
                Game junk;
                const int rc = msg_replay(&t, &junk);
                Game probe;
                game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
                memset(&probe, 0, sizeof(probe));
                probe.num_players = (int8_t)np;
                for (int i = 0; i < np; i++) probe.players[i].status = PLAYER_STATUS_READY;
                start_game(&probe);
                if (probe.first_attacker != (int8_t)opening) {
                    CHECK(rc != MSG_EOK,
                          "np=%d: a chain stripped of its penalty still replayed", np);
                }
            }
        }
    }
}

// A v8 code carries its own forced opening, so a SHARED replay (no envelope, no
// seed - just the code) rebuilds the same opening seat.
static void test_forced_opening_replay(void) {
    for (int np = 2; np <= 4; np++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 8800u + (uint32_t)np);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        g_rng = 4242u + (uint32_t)np;
        random_strategy_set_seed(g_rng);

        Game g;
        memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)np;
        for (int i = 0; i < np; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = 0;
        }
        // Derive first, so the test only runs where the penalty really differs
        // from the ordinary rule (otherwise there is no override to prove).
        start_game(&g);
        const int derived = g.first_attacker;
        const int opening = (derived + 1) % np;

        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)np;
        for (int i = 0; i < np; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = 0;
        }
        game_open_at_seat(opening);
        start_game(&g);
        game_open_at_seat(-1);
        CHECK(game_derived_opening() == derived,
              "np=%d: the derive was not recorded under a pin (%d want %d)",
              np, game_derived_opening(), derived);

        // Play a handful of legal moves so there is a real chain to encode.
        static LegalMoves ml;
        for (int step = 0; step < 24; step++) {
            if (game_done(&g) >= 0 || g.status != GAME_STATUS_PLAYING) break;
            int seat = -1, pick = -1;
            for (int s = 0; s < np && seat < 0; s++) {
                if (g.players[s].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, s, &ml);
                for (int i = 0; i < ml.n; i++)
                    if (ml.moves[i].type != MOVE_WAIT) { seat = s; pick = i; break; }
            }
            if (seat < 0 || pick < 0) break;
            AwireAction a;
            move_to_awire(&ml.moves[pick], &a);
            bool ok;
            switch (a.kind) {
                case AWIRE_ATTACK: ok = handle_attack(&g, seat, a.cards, a.n); break;
                case AWIRE_COVER:  ok = handle_cover(&g, seat, a.cards, a.attacks, a.n); break;
                case AWIRE_PASS:   ok = handle_pass(&g, seat, a.cards, a.n); break;
                case AWIRE_PICKUP: ok = handle_pickup(&g, seat); break;
                default:           ok = handle_good(&g, seat); break;
            }
            if (!ok) break;
        }

        static unsigned char code[2048];
        const int cn = replay_encode_v6_from_game(&g, seed, MSG_SEED_LEN, 1 << 30,
                                                  code, sizeof(code));
        CHECK(cn > 0, "np=%d: forced-game encode failed (%d)", np, cn);
        if (cn <= 0) continue;

        ReplayHeader hdr;
        const int d = replay_decode_atoms_v6(code, cn, &hdr, 0, 0);
        CHECK(d >= 0, "np=%d: forced-game decode failed (%d)", np, d);
        CHECK(hdr.version == REPLAY_FORMAT_VERSION_V10, "np=%d: not v10 (%d)", np, hdr.version);
        CHECK(hdr.forced_opening == 1, "np=%d: the forced bit was not set", np);
        CHECK(hdr.first_attacker == opening,
              "np=%d: the code recorded opener %d, want %d", np, hdr.first_attacker, opening);
        CHECK(hdr.derived_opening == derived,
              "np=%d: the code recorded derive %d, want %d", np, hdr.derived_opening, derived);
    }
}

// ---------- the rules ride the chain (podkidnoy) --------------------------
//
// A whole podkidnoy game, sealed and replayed on the other side. What is being
// pinned is that the RULES survive the wire - not as a display flag, but as the
// thing the body was coded against: a v6 code is a sequence of indices into the
// legal-move menu, so a receiver that rebuilt the wrong menu would read the
// same bytes as different moves.
//
// The perevodnoy control is played from the SAME seed on purpose. Without it,
// "the podkidnoy chain contains no transfer" is a claim about one game's luck;
// with it, the control's transfers are the proof that the position had them to
// offer.
static int game_has_pass(const Game *g) {
    for (int i = 0; i < g->num_logs; i++) if (g->logs[i].log_type == LOG_PASS) return 1;
    return 0;
}

static void test_podkidnoy_wire(void) {
    static unsigned char body[1024];
    static Game scratch;
    int passes_seen = 0, podkidnoy_games = 0, shorter = 0;

    for (uint32_t gi = 0; gi < 24; gi++) {
        const int np = (int)(2 + gi % 3);
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 4100 + gi);

        // The control: the same deal under the classic rules.
        Chain cc; memset(&cc, 0, sizeof(cc));
        Game cend;
        g_rng = 4100 + gi;
        play_game(seed, np, 80, &cc, &cend, -1);
        if (game_has_pass(&cend)) passes_seen++;

        Chain ch; memset(&ch, 0, sizeof(ch));
        Game end;
        g_rng = 4100 + gi;
        play_game_rules(seed, np, 80, &ch, &end, -1, GAME_RULE_NO_PASS);
        CHECK(!game_has_pass(&end), "game %u: a podkidnoy game played a transfer", gi);
        if (end.num_logs < 2) continue;
        podkidnoy_games++;

        // THE MODE REACHES THE MENU, and the size is how that shows. This game's
        // moves are legal under either variant (podkidnoy's are a subset), so
        // the SAME move stream can be coded both ways - and the podkidnoy code
        // is the shorter one, because the transfers it never had are not in the
        // model either. Equal lengths everywhere would mean the mode is being
        // stored and ignored.
        {
            Game as_classic = end;
            as_classic.rules = 0;
            unsigned char a[1024], b[1024];
            const int na = replay_encode_v6_from_game(&end, seed, MSG_SEED_LEN, 1 << 30,
                                                      a, (int)sizeof a);
            const int nb = replay_encode_v6_from_game(&as_classic, seed, MSG_SEED_LEN, 1 << 30,
                                                      b, (int)sizeof b);
            if (na > 0 && nb > 0) {
                CHECK(na <= nb, "game %u: the podkidnoy code was the longer one "
                      "(%d vs %d)", gi, na, nb);
                if (na < nb) shorter++;
            }
        }

        MsgEnvelope e;
        env_init(&e, seed, np);
        const int over = game_done(&end) >= 0 || end.status == GAME_STATUS_GAME_OVER;
        e.phase = over ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE;
        if (msg_seal(&e, &end, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK) continue;

        // The seal states the rules off the GAME - the caller never said.
        CHECK(e.variant == 0, "game %u: a podkidnoy seal wrote variant %d", gi, e.variant);
        CHECK(msg_pass_allowed(&e) == 0, "game %u: the seal claimed the transfer", gi);

        unsigned char wire[ENV_CAP];
        const int n = msg_encode(&e, wire, sizeof(wire));
        CHECK(n > 0, "game %u: podkidnoy encode failed (%d)", gi, n);
        if (n <= 0) continue;

        MsgEnvelope d;
        CHECK(msg_decode(wire, n, &d) == MSG_EOK, "game %u: podkidnoy decode failed", gi);
        CHECK(msg_pass_allowed(&d) == 0, "game %u: the rules did not survive the wire", gi);

        // The REPLAY path rebuilds a game from the code alone (no envelope, no
        // seed - a share link), and it must rebuild the same table: the code
        // carries its own pass-mode bit, and replay_steps stamps it back onto
        // the Game it plays the recorded actions over. Without that the rebuilt
        // board answers "what is legal here" with a transfer that no step of
        // the code it is showing could contain.
        if (d.actions_len > 0) {
            const int steps = replay_steps_count_v6(d.actions, d.actions_len, NULL);
            CHECK(steps > 0, "game %u: the podkidnoy code did not rebuild", gi);
            const Game *rg = replay_steps_last_game();
            CHECK(rg && !game_pass_allowed(rg),
                  "game %u: the rebuilt replay got its transfer back", gi);
        }

        Game g;
        CHECK(msg_replay(&d, &g) == MSG_EOK, "game %u: the podkidnoy chain did not replay", gi);
        // …and the game it rebuilt IS podkidnoy, so play continues under the
        // rules it arrived with rather than under this build's default.
        CHECK(!game_pass_allowed(&g), "game %u: the replayed game got its transfer back", gi);
        LegalMoves ml;
        calculate_legal_moves(&g, g.defender, &ml);
        for (int i = 0; i < ml.n; i++)
            CHECK(ml.moves[i].type != MOVE_PASS,
                  "game %u: the rebuilt board offered a transfer", gi);

        // THE HEADER AND THE BODY MUST AGREE. A chain whose variant byte says one
        // game and whose body was cut against the other does not replay - which
        // matters because the header is what a device DEALS from, before it has
        // decoded a single atom.
        {
            unsigned char t[ENV_CAP];
            memcpy(t, wire, (size_t)n);
            t[16] = MSG_VARIANT_PASS;
            MsgEnvelope liar; Game lg;
            if (msg_decode(t, n, &liar) == MSG_EOK)
                CHECK(msg_replay(&liar, &lg) == MSG_EBODY,
                      "game %u: a chain that lied about its rules replayed", gi);
        }
    }

    CHECK(podkidnoy_games >= 12, "only %d podkidnoy games sealed", podkidnoy_games);
    CHECK(shorter > 0,
          "no podkidnoy code came out shorter than the same game coded against "
          "the transfer menu - the mode is not reaching build_top_menu");
    // The control has to have USED the transfer, or none of the above is a test.
    CHECK(passes_seen >= 3,
          "the perevodnoy control transferred in only %d games - the podkidnoy "
          "chains prove nothing", passes_seen);

    // The other direction: a classic chain with its rules bit cleared is just as
    // dead. (Same fixture shape, one game.)
    {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 4242);
        Chain ch; memset(&ch, 0, sizeof(ch));
        Game end;
        g_rng = 4242;
        play_game(seed, 4, 80, &ch, &end, -1);
        MsgEnvelope e;
        env_init(&e, seed, 4);
        const int over = game_done(&end) >= 0 || end.status == GAME_STATUS_GAME_OVER;
        e.phase = over ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE;
        if (msg_seal(&e, &end, MSG_NO_BASE, body, sizeof(body), &scratch) == MSG_EOK) {
            CHECK(e.variant == MSG_VARIANT_PASS, "a classic seal wrote variant %d", e.variant);
            unsigned char wire[ENV_CAP];
            const int n = msg_encode(&e, wire, sizeof(wire));
            if (n > 0) {
                wire[16] = 0;
                MsgEnvelope liar; Game lg;
                if (msg_decode(wire, n, &liar) == MSG_EOK)
                    CHECK(msg_replay(&liar, &lg) == MSG_EBODY,
                          "a classic chain re-labelled podkidnoy replayed anyway");
            }
        }
    }
}

static void test_clock_wire(void) {
    uint8_t seed[MSG_SEED_LEN];
    seed_fill(seed, 77);
    Chain ch; memset(&ch, 0, sizeof(ch));
    Game played;
    g_rng = 77;
    play_game(seed, 2, 30, &ch, &played, -1);

    static unsigned char body[1024];
    static Game scratch;

    // Sealed WITHOUT a stamp. Every seal now writes the RULES format (5), which
    // is format 3's 62 bytes with the variant byte spent - so an unstamped seal
    // still carries the clock FIELD, holding 0, which is the wire's "this bubble
    // does not say".
    MsgEnvelope plain;
    env_init(&plain, seed, 2);
    CHECK(msg_seal(&plain, &played, MSG_NO_BASE, body, sizeof(body), &scratch) == MSG_EOK, "plain seal failed");
    CHECK(plain.format == MSG_FORMAT_RULES, "an unstamped seal picked format %d", plain.format);
    CHECK(plain.sent_at == 0, "an unstamped seal invented a clock");
    unsigned char w5[ENV_CAP];
    const int n5 = msg_encode(&plain, w5, sizeof(w5));
    CHECK(n5 > 0, "plain encode failed: %d", n5);

    // THE LEGACY HEADER, hand-built: format 2 is the same envelope minus the two
    // clock bytes and the delta byte, and it is still what every bubble sealed
    // before the clock looks like. Three bytes shorter, and this build still
    // reads it.
    MsgEnvelope legacy = plain;
    legacy.format = MSG_FORMAT_V6;
    legacy.variant = 0;      // the byte is reserved on that format, and passing
    unsigned char w2[ENV_CAP];
    const int n2 = msg_encode(&legacy, w2, sizeof(w2));
    CHECK(n2 == n5 - 3, "format 5 cost %d bytes over format 2, not 3", n5 - n2);

    // Sealed WITH a stamp: the same format and the same length (the field was
    // always there), and the stamp survives the round trip.
    MsgEnvelope stamped;
    env_init(&stamped, seed, 2);
    stamped.sent_at = 0xBEEF;
    CHECK(msg_seal(&stamped, &played, MSG_NO_BASE, body, sizeof(body), &scratch) == MSG_EOK, "stamped seal failed");
    CHECK(stamped.format == MSG_FORMAT_RULES, "a stamped seal picked format %d", stamped.format);
    unsigned char w3[ENV_CAP];
    const int n3 = msg_encode(&stamped, w3, sizeof(w3));
    CHECK(n3 == n5, "a clock cost %d bytes on a format that always carried one", n3 - n5);
    CHECK(stamped.n_new == 0, "a seal with no base claimed a bubble delta");

    MsgEnvelope d;
    CHECK(msg_decode(w3, n3, &d) == MSG_EOK, "format 5 did not decode");
    CHECK(d.format == MSG_FORMAT_RULES && d.sent_at == 0xBEEF, "the clock did not survive decode");
    CHECK(d.n_actions == stamped.n_actions && d.turn == stamped.turn &&
          d.round == stamped.round && d.n_players == stamped.n_players,
          "format 3 lost a field the clock sits between");
    CHECK(d.n_joins == stamped.n_joins && d.joins[1].name_len == stamped.joins[1].name_len,
          "the joins shifted under the clock");

    // Re-encode byte-identical, exactly as format 2 must be: parent8 chains off
    // these bytes for everyone downstream.
    unsigned char again[ENV_CAP];
    const int na = msg_encode(&d, again, sizeof(again));
    CHECK(na == n3 && !memcmp(w3, again, (size_t)n3), "format 5 did not re-encode to itself");

    // A format-2 chain decodes to NO clock rather than to a garbage one.
    MsgEnvelope d2;
    CHECK(msg_decode(w2, n2, &d2) == MSG_EOK, "format 2 stopped decoding");
    CHECK(d2.sent_at == 0, "a clockless chain decoded to a clock");
    CHECK(d2.n_new == 0, "a format-2 chain decoded to a bubble delta");

    // The pairing is enforced in both directions: format 2 cannot carry a stamp.
    MsgEnvelope liar = stamped;
    liar.format = MSG_FORMAT_V6;
    liar.variant = 0;
    unsigned char wl[ENV_CAP];
    CHECK(msg_encode(&liar, wl, sizeof(wl)) == MSG_EFORMAT, "format 2 encoded a clock");
    // …and it cannot carry a bubble delta either, for the same reason: there is
    // nowhere in a 59-byte header to put one.
    MsgEnvelope liar2 = legacy;
    liar2.n_new = 1;
    CHECK(msg_encode(&liar2, wl, sizeof(wl)) == MSG_EFORMAT, "format 2 encoded a delta");
    // …nor the RULES, which is the other half of the same rule: the variant byte
    // is reserved on a format that predates it, so an envelope claiming the
    // passing bit on format 2 is not a format-2 envelope. Without this the same
    // byte would mean two things depending on who read it.
    MsgEnvelope liar3 = legacy;
    liar3.variant = MSG_VARIANT_PASS;
    CHECK(msg_encode(&liar3, wl, sizeof(wl)) == MSG_EVARIANT, "format 2 encoded a rules byte");
    // A legacy chain reads as the passing game - the only one those formats
    // could describe - whatever this build's default happens to be.
    MsgEnvelope dl;
    CHECK(msg_decode(w2, n2, &dl) == MSG_EOK, "format 2 stopped decoding");
    CHECK(msg_pass_allowed(&dl) == 1, "a format-2 chain lost the transfer");
}

// ---------- 5. hostile bodies are rejected (validation = replay) ----------

// An "illegal chain" cannot be hand-written any more, and that is the point: a
// v6 body codes each action as an index into the legal-move MENU, so a move the
// rules forbid has no index and no encoding. Illegality is unrepresentable
// rather than merely rejected. What remains reachable is a body that is not a
// code for THIS game — garbage, a truncation, or another game's code — and each
// must be refused without a crash.
static void test_hostile_body(void) {
    uint8_t seed[MSG_SEED_LEN];
    seed_fill(seed, 99);
    g_rng = 99;
    Chain ch; memset(&ch, 0, sizeof(ch));
    Game played;
    play_game(seed, 4, 40, &ch, &played, -1);

    MsgEnvelope e;
    env_init(&e, seed, 4);
    static unsigned char body[1024];
    static Game scratch;
    CHECK(msg_seal(&e, &played, MSG_NO_BASE, body, sizeof(body), &scratch) == MSG_EOK, "seal failed");

    unsigned char wire[ENV_CAP];
    const int n = msg_encode(&e, wire, sizeof(wire));
    CHECK(n > 0, "encode failed");
    if (n <= 0) return;
    const int body_off = n - e.actions_len;

    // (a) Random garbage in the body: never EOK, never a crash.
    int accepted = 0;
    for (int t = 0; t < 2000; t++) {
        unsigned char m[ENV_CAP];
        memcpy(m, wire, (size_t)n);
        for (int k = body_off; k < n; k++) m[k] = (unsigned char)(rnd() >> 11);
        MsgEnvelope d;
        if (msg_decode(m, n, &d) != MSG_EOK) continue;
        Game g;
        if (msg_replay(&d, &g) == MSG_EOK) accepted++;
    }
    // A random body CAN happen to be a shorter legal game — the code space is
    // dense. It can never be one whose atom count matches this header's `turn`,
    // which is what makes the header the anchor.
    CHECK(accepted == 0, "%d/2000 random bodies replayed as this envelope's chain", accepted);

    // (b) Another game's code under this game's seed: the actions do not fit the
    //     deal, so the menus reject them (REPLAY_ENOTINMENU) — the codec IS the
    //     rules check.
    uint8_t other[MSG_SEED_LEN];
    seed_fill(other, 4242);
    g_rng = 4242;
    Chain ch2; memset(&ch2, 0, sizeof(ch2));
    Game played2;
    play_game(other, 4, 40, &ch2, &played2, -1);
    MsgEnvelope e2;
    env_init(&e2, other, 4);
    static unsigned char body2[1024];
    if (msg_seal(&e2, &played2, MSG_NO_BASE, body2, sizeof(body2), &scratch) == MSG_EOK) {
        MsgEnvelope mix = e;              // this game's seed + header
        mix.actions = body2;              // the OTHER game's code
        mix.actions_len = e2.actions_len;
        unsigned char w2[ENV_CAP];
        const int wn = msg_encode(&mix, w2, sizeof(w2));
        if (wn > 0) {
            MsgEnvelope d; Game g;
            if (msg_decode(w2, wn, &d) == MSG_EOK) {
                CHECK(msg_replay(&d, &g) != MSG_EOK,
                      "another game's code replayed under this seed");
            }
        }
    }
}

// ---------- 6. size guardrail --------------------------------------------

// base32 is 8 chars per 5 bytes (codec.ts), so a char budget is a byte budget.
static int b32_chars(int bytes) { return (bytes + 4) / 5 * 8; }

// Measures full games and reports the distribution. `bot` per play_game.
// Returns P95 bytes, or -1 if nothing completed.
static int measure(int games, uint32_t seed0, int np, int bot, const char *label) {
    static int sizes[512];
    static int acts[512];
    int n = 0;
    for (int gi = 0; gi < games && n < 512; gi++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, seed0 + (uint32_t)gi * 7919 + (uint32_t)np);
        g_rng = seed0 + (uint32_t)gi;
        Chain ch; memset(&ch, 0, sizeof(ch));
        Game played;
        const int rounds = play_game(seed, np, 2000, &ch, &played, bot);
        if (game_done(&played) < 0 && played.status == GAME_STATUS_PLAYING) continue; // unfinished

        MsgEnvelope e;
        env_init(&e, seed, np);
        e.phase = MSG_PHASE_FINISHED;
        (void)rounds;
        static unsigned char body[1024];
        static Game scratch;
        if (msg_seal(&e, &played, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK) continue;
        unsigned char wire[ENV_CAP];
        const int w = msg_encode(&e, wire, sizeof(wire));
        if (w > 0) { acts[n] = ch.n; sizes[n] = w; n++; }
    }
    if (n == 0) { printf("  size[%s np=%d]: no completed games\n", label, np); return -1; }
    for (int i = 1; i < n; i++) {
        const int v = sizes[i], a = acts[i];
        int j = i - 1;
        while (j >= 0 && sizes[j] > v) { sizes[j + 1] = sizes[j]; acts[j + 1] = acts[j]; j--; }
        sizes[j + 1] = v; acts[j + 1] = a;
    }
    int idx = (n * 95) / 100; if (idx >= n) idx = n - 1;
    const int p95 = sizes[idx];
    printf("  size[%-8s np=%d]: n=%3d  median %4d B (%4d ch)  P95 %4d B (%4d ch)  max %4d B  |  actions med %d\n",
           label, np, n, sizes[n / 2], b32_chars(sizes[n / 2]),
           p95, b32_chars(p95), sizes[n - 1], acts[n / 2]);
    return p95;
}

// EXPERIMENT (temporary): can a v6 code carry an ARBITRARY mid-game state?
// A turn bubble is a mid-game cut, so this is the load-bearing question for
// using v6 as the body. For each cut point: encode the partial game, decode,
// replay the decoded actions, and compare state against the truth.
static void probe_v6_midgame(uint32_t seed0, int np, int bot) {
    int cuts = 0, enc_fail = 0, dec_fail = 0, state_bad = 0, good_pending_bad = 0;
    for (int gi = 0; gi < 40; gi++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, seed0 + (uint32_t)gi * 7919 + (uint32_t)np);
        for (int cut = 1; cut <= 60; cut++) {
            g_rng = seed0 + (uint32_t)gi;
            Chain ch; memset(&ch, 0, sizeof(ch));
            Game truth;
            play_game(seed, np, cut, &ch, &truth, bot);
            if (ch.n < cut) break;                 // game ended before this cut
            if (truth.status != GAME_STATUS_PLAYING) break;
            cuts++;

            const int mask_before = truth.good_players_mask;

            unsigned char body[4096];
            const int b = replay_encode_v6_from_game(&truth, seed, MSG_SEED_LEN,
                                                    1 << 30, body, sizeof(body));
            if (b < 0) { enc_fail++; continue; }

            static unsigned char dec[1 << 20];
            const int d = replay_decode(body, b, dec, sizeof(dec));
            if (d < 0) { dec_fail++; continue; }

            // Rebuild from the decoded log stream: re-apply every action log.
            game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
            Game rg;
            memset(&rg, 0, sizeof(rg));
            rg.num_players = (int8_t)np;
            for (int i = 0; i < np; i++) {
                rg.players[i].status = PLAYER_STATUS_READY;
            }
            start_game(&rg);

            const uint32_t n_logs = (uint32_t)dec[16] | ((uint32_t)dec[17] << 8) |
                                    ((uint32_t)dec[18] << 16) | ((uint32_t)dec[19] << 24);
            int off = REPLAY_DEC_HDR;
            for (uint32_t li = 0; li < n_logs; li++) {
                const int lt = dec[off], seat = dec[off + 1], npairs = dec[off + 3];
                const unsigned char *pairs = dec + off + 4;
                off += 4 + npairs * 2;
                Card c[REPLAY_MAX_PAIRS], a[REPLAY_MAX_PAIRS];
                for (int k = 0; k < npairs; k++) {
                    c[k] = card_from_wire_state(pairs[k * 2]);
                    a[k] = card_from_wire_state(pairs[k * 2 + 1]);
                }
                if      (lt == LOG_ATTACK) handle_attack(&rg, seat, c, npairs);
                else if (lt == LOG_COVER)  handle_cover(&rg, seat, c, a, npairs);
                else if (lt == LOG_PASS)   handle_pass(&rg, seat, c, npairs);
                else if (lt == LOG_PICKUP) handle_pickup(&rg, seat);
                else if (lt == LOG_GOOD)   handle_good(&rg, seat);
            }

            int bad = 0;
            if (rg.num_battles != truth.num_battles || rg.defender != truth.defender ||
                rg.deck_count != truth.deck_count) bad = 1;
            for (int s = 0; s < np; s++)
                if (rg.players[s].hand_count != truth.players[s].hand_count) bad = 1;
            if (bad) state_bad++;
            if ((int)rg.good_players_mask != mask_before) good_pending_bad++;
        }
    }
    printf("  v6mid np=%d: %d cuts | enc_fail %d | dec_fail %d | STATE MISMATCH %d | "
           "good_mask lost %d\n", np, cuts, enc_fail, dec_fail, state_bad, good_pending_bad);
}

static void test_size_budget(int games, uint32_t seed0) {
    // §4.4's guardrail: P95 of a FULL game's envelope < 1,000 base32 chars,
    // measured AT 4 PLAYERS (the spec calls that the worst case). 625 bytes.
    //
    // The driver matters more than the spec anticipated. Uniform-random play is
    // the honest stress case for the codec but a slander of the size budget: it
    // dumps single cards, declines to end rounds, and drags games out far past
    // anything a person or a bot plays. `robusta` is the bot real humans face
    // (the default opponent), so it is the representative measurement; random
    // is reported alongside as the pessimistic bound.
    const int budget_bytes = 625;
    const int robusta = bot_roster_find("robusta");
    CHECK(robusta >= 0, "bot_roster_find(robusta) failed");

    measure(games, seed0, 2, -1, "random");
    measure(games, seed0, 4, -1, "random");
    if (robusta >= 0) {
        measure(games, seed0, 2, robusta, "robusta");
        // 8p is not v1's worst case (the UI caps at 4) but the protocol is
        // spec'd to run there, so it is reported to size any future lift.
        measure(games, seed0, 8, robusta, "robusta");
        const int p95 = measure(games, seed0, 4, robusta, "robusta");
        // §4.4's guardrail, live. It passes with ~4x margin on the v6 body
        // (measured P95 ~240 chars of the 1,000). It did NOT pass on the raw
        // body it replaced — 1,328 chars, over by 1.33x and unfixable, which is
        // what chose the codec (docs/IMESSAGE_BODY_CODEC.md).
        //
        // If this ever trips, the payload grew ~4x: suspect the body, not the
        // budget.
        if (p95 >= 0) {
            CHECK(p95 <= budget_bytes,
                  "P95 envelope %d B (%d base32 chars) exceeds the %d B (1,000 char) budget "
                  "at 4p on representative play — see docs §4.4",
                  p95, b32_chars(p95), budget_bytes);
        }
    }
}

// ---------- --twocover: two covers, sent as TWO bubbles --------------------
//
// Prints the SECOND of two bubbles that each carry ONE cover by the same seat -
// the owner's round-16 report, as a payload you can open in the simulator:
// "a defender covers a single card, sends it, then covers a second card, and
// sends that. If anyone opens the bubble for the second cover, they will see
// BOTH covers animate."
//
// The point is the two SENDS. On the chain, two covers sent separately are
// byte-for-byte what two covers staged together would be, so nothing in the
// replay steps can tell them apart - only the bubble delta each seal writes
// (msg_wire.h's n_new) can, which is exactly what this fixture exercises. Each
// seal here is given the PREVIOUS envelope's turn as its base, the same way
// fio_msg_encode gives it the chain it decoded.
//
// It prints the second bubble's hex on stdout (for dev.fatboard) and, on
// stderr, the two covering cards and the delta the bubble claims - so a filmed
// run can be checked against what the wire actually said. Sit as the ATTACKER
// (dev.seat 0): the covers are then somebody else's move, which is the case
// that animates on open.
//
// `one_bubble` seals both covers into ONE bubble instead - the CONTROL. Same
// deal, same two cards, same chain bytes: only the send in the middle differs,
// and a staged double cover must still animate BOTH. Without it a run that
// shows one flight proves nothing, since a fixture that could never show two
// would look identical.
//
// Usage: msg_wire_test --twocover [n_players] [one]
static void print_twocover(int np, int one_bubble) {
    static unsigned char body[1024];
    static Game scratch;
    static LegalMoves ml;

    for (uint32_t s = 1; s < 4000; s++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 20260822u + s * 89u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        Game g;
        memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)np;
        for (int i = 0; i < np; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = 0;
        }
        start_game(&g);

        // Two single-card attacks, so there are two slots to cover one at a
        // time. A throw-in needs its rank on the table already, so this only
        // works on deals where the attacker holds a pair.
        int thrown = 0;
        for (int step = 0; step < 8 && thrown < 2; step++) {
            const int def = g.defender;
            int acted = 0;
            for (int seat = 0; seat < np && !acted; seat++) {
                if (seat == def || g.players[seat].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, seat, &ml);
                for (int i = 0; i < ml.n; i++) {
                    if (ml.moves[i].type != MOVE_ATTACK || ml.moves[i].n_cards != 1) continue;
                    if (handle_attack(&g, seat, ml.moves[i].cards, 1)) { acted = 1; thrown++; }
                    break;
                }
            }
            if (!acted) break;
        }
        if (thrown < 2) continue;

        // Bubble 1: the attacker's throw-ins. Base 0 - a genesis chain adds all
        // of itself.
        MsgEnvelope a;
        env_init(&a, seed, np);
        a.phase = MSG_PHASE_LIVE;
        a.last_actor_seat = (uint8_t)g.logs[g.num_logs - 1].player_idx;
        a.sent_at = (uint16_t)((time(NULL) - 60) & 0xffff);
        if (msg_seal(&a, &g, 0, body, sizeof(body), &scratch) != MSG_EOK) continue;
        const int turn_a = a.turn;

        // Cover ONE, and send: bubble 2, based on bubble 1.
        const int def = g.defender;
        if (g.players[def].hand_count < 3) continue;   // keep a card after both covers
        Card cov1 = { 0 }, cov2 = { 0 };
        calculate_legal_moves(&g, def, &ml);
        int did = 0;
        for (int i = 0; i < ml.n; i++) {
            if (ml.moves[i].type != MOVE_COVER || ml.moves[i].n_cards != 1) continue;
            cov1 = ml.moves[i].cards[0];
            if (handle_cover(&g, def, ml.moves[i].cards, ml.moves[i].attack_cards, 1)) did = 1;
            break;
        }
        if (!did) continue;
        int turn_b = turn_a;
        if (!one_bubble) {
            static unsigned char body_b[1024];
            MsgEnvelope b;
            env_init(&b, seed, np);
            b.phase = MSG_PHASE_LIVE;
            b.last_actor_seat = (uint8_t)def;
            b.sent_at = (uint16_t)((time(NULL) - 30) & 0xffff);
            if (msg_seal(&b, &g, turn_a, body_b, sizeof(body_b), &scratch) != MSG_EOK) continue;
            turn_b = b.turn;
        }

        // Cover the OTHER, and send: bubble 3, based on bubble 2. This is the
        // one to open.
        calculate_legal_moves(&g, def, &ml);
        did = 0;
        for (int i = 0; i < ml.n; i++) {
            if (ml.moves[i].type != MOVE_COVER || ml.moves[i].n_cards != 1) continue;
            cov2 = ml.moves[i].cards[0];
            if (handle_cover(&g, def, ml.moves[i].cards, ml.moves[i].attack_cards, 1)) did = 1;
            break;
        }
        if (!did) continue;
        if (g.status != GAME_STATUS_PLAYING) continue;   // a bout that ended has nothing left to open
        static unsigned char body_c[1024];
        MsgEnvelope c;
        env_init(&c, seed, np);
        c.phase = MSG_PHASE_LIVE;
        c.last_actor_seat = (uint8_t)def;
        c.sent_at = (uint16_t)(time(NULL) & 0xffff);
        if (msg_seal(&c, &g, turn_b, body_c, sizeof(body_c), &scratch) != MSG_EOK) continue;
        unsigned char wire[ENV_CAP];
        const int n = msg_encode(&c, wire, sizeof(wire));
        if (n <= 0) continue;

        fprintf(stderr, "twocover: defender seat %d covered %d/%d then %d/%d\n",
                def, cov1.suit, cov1.value, cov2.suit, cov2.value);
        fprintf(stderr, "twocover: %s, turns %d -> %d -> %d, bubble claims delta %d\n",
                one_bubble ? "ONE bubble (control: BOTH covers must animate)"
                           : "TWO bubbles (only the second cover may animate)",
                turn_a, turn_b, c.turn, c.n_new);
        for (int i = 0; i < n; i++) printf("%02x", wire[i]);
        printf("\n");
        return;
    }
    fprintf(stderr, "no %dp deal posed two coverable throw-ins\n", np);
}

// ---------- --started: the bubble Start seals --------------------------------
//
// Prints the LIVE handoff a lobby's Start seals: the deal locked at create, the
// real player count, nobody has moved (turn 0, an empty body - msg_seal's
// 0-action path, exactly what fio_msg_encode writes for it). Opening this is
// "the started bubble", whose whole animation is the opening deal (owner: "Did
// not see card deal"). Seeded boards open quiet, so film it with REPLAY=1.
//
// Usage: msg_wire_test --started [n_players]
static void print_started(int np) {
    static unsigned char body[1024];
    static Game scratch;
    uint8_t seed[MSG_SEED_LEN];
    seed_fill(seed, 20261001u + (uint32_t)np * 131u);
    game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
    Game g;
    memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)np;
    for (int i = 0; i < np; i++) g.players[i].status = PLAYER_STATUS_READY;
    start_game(&g);
    MsgEnvelope e;
    env_init(&e, seed, np);
    e.phase = MSG_PHASE_LIVE;
    e.last_actor_seat = 0;
    e.sent_at = (uint16_t)((time(NULL) - 60) & 0xffff);
    if (msg_seal(&e, &g, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK) {
        fprintf(stderr, "started: the %dp handoff did not seal\n", np);
        return;
    }
    unsigned char wire[ENV_CAP];
    const int n = msg_encode(&e, wire, sizeof(wire));
    if (n <= 0) { fprintf(stderr, "started: encode failed (%d)\n", n); return; }
    fprintf(stderr, "started: %dp, turn %d, first attacker seat %d, defender=seat %d\n",
            np, e.turn, g.first_attacker, g.defender);
    for (int i = 0; i < n; i++) printf("%02x", wire[i]);
    printf("\n");
}

// `msg_wire_test --fixture` prints sealed envelopes as hex, one per line:
//   <n_players> <turn> <round> <hex>
// These are the cross-engine goldens (design §8.2): the wasm kernel and, later,
// libfoolish.a on a phone must decode them to the same game, or an iMessage
// game forks between a browser and a device. e2e/msg_wire.test.ts pins them.
// Does this state POSE the canonical race — can the defender pick up while some
// attacker can still throw in (§7.5)? A fixture cut anywhere else cannot express
// the case the concurrency suite exists to test.
static int poses_the_race(const Game *g) {
    static LegalMoves ml;
    int can_pickup = 0, can_attack = 0;
    for (int s = 0; s < g->num_players; s++) {
        if (g->players[s].status != PLAYER_STATUS_IN) continue;
        calculate_legal_moves(g, s, &ml);
        for (int i = 0; i < ml.n; i++) {
            if (ml.moves[i].type == MOVE_PICKUP) can_pickup = 1;
            if (ml.moves[i].type == MOVE_ATTACK) can_attack = 1;
        }
    }
    return can_pickup && can_attack;
}

// ---------- --fatboard: a dense table, as an FMSG payload ------------------
//
// Prints one LIVE envelope whose table carries `target` or more cards, with
// COVERED PAIRS among them, still playable, with the defender to move. Used to
// seed the iMessage extension for animation work (ios/FoolishKit/Messages/
// MessageDevBoard.swift): the search belongs here, where a whole game is
// microseconds, rather than on a device driving a UI.
//
// TWO PLAYERS, which is the interesting part. A throw-in needs its rank to be on
// the table already, so the lone attacker looks stuck after the opening card -
// but every COVER puts the cover's own rank on the table too, handing the
// attacker something new to throw at each exchange. Attack, cover, attack,
// cover: five of each is a ten-card table with the defender on their last card.
// No extra seats required.
//
// The defender always keeps a card in hand, because a cover that empties it
// discards the table inline (handle_cover) and there would be nothing left to
// pick up. Nobody says good, so the all-good transition cannot fire either.
//
// Usage: msg_wire_test --fatboard [target] [n_players]
// --passable [np]: a board where the DEFENDER may PASS, with exactly TWO
// uncovered attacks and nothing covered - the state a screenshot needs to show
// the drag hint read "Pass".
//
// It needs its own search because no existing generator can reach it. The
// chain playout takes the FIRST non-wait legal move and passes sort after
// attacks and covers, so 65 chains across 2-4 players contained not one pass;
// and every `fatboard` state has something covered, which makes passing
// illegal by rule (a pass requires a table where nothing has been defended).
// So this plays games out and STOPS the moment a seat is holding a legal pass
// against the table shape the owner asked for, rather than hoping one turns up.
static void print_passable(int np) {
    static unsigned char body[1024];
    static Game scratch;
    static LegalMoves ml;

    for (uint32_t s = 1; s < 9000; s++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 20260914u + s * 137u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        g_rng = 7717u + s;
        random_strategy_set_seed(g_rng);
        Game g;
        memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)np;
        for (int i = 0; i < np; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = 0;
        }
        start_game(&g);   // rules 0 = perevodnoy, which is what makes a pass legal

        for (int step = 0; step < 400 && g.status == GAME_STATUS_PLAYING; step++) {
            if (game_done(&g) >= 0) break;

            // THE TEST, before each move rather than after: is somebody sitting
            // on a legal pass, with two bare attacks in front of them?
            if (g.num_battles == 2 &&
                g.table_battles[0].defense.value <= 0 &&
                g.table_battles[1].defense.value <= 0 &&
                g.defender >= 0) {
                calculate_legal_moves(&g, g.defender, &ml);
                for (int i = 0; i < ml.n; i++) {
                    if (ml.moves[i].type != MOVE_PASS) continue;
                    MsgEnvelope e;
                    env_init(&e, seed, np);
                    e.phase = MSG_PHASE_LIVE;
                    e.last_actor_seat = (uint8_t)g.defender;
                    if (msg_seal(&e, &g, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK)
                        break;
                    unsigned char wire[ENV_CAP];
                    const int n = msg_encode(&e, wire, sizeof(wire));
                    if (n <= 0) break;
                    for (int b = 0; b < n; b++) printf("%02x", wire[b]);
                    printf("\n");
                    fprintf(stderr, "passable: np=%d seed#%u defender=seat %d holds %d,"
                            " 2 bare attacks, turn %d (%d bytes) hands=",
                            np, s, g.defender, g.players[g.defender].hand_count, e.turn, n);
                    for (int q = 0; q < np; q++)
                        fprintf(stderr, "%s%d", q ? "/" : "", g.players[q].hand_count);
                    fprintf(stderr, "\n");
                    return;
                }
            }

            int seat = -1, pick = -1;
            const int start = (int)(rnd() % (uint32_t)np);
            for (int t = 0; t < np && seat < 0; t++) {
                const int c = (start + t) % np;
                if (g.players[c].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, c, &ml);
                for (int i = 0; i < ml.n; i++)
                    if (ml.moves[i].type != MOVE_WAIT) { seat = c; pick = i; break; }
            }
            if (seat < 0 || pick < 0) break;
            AwireAction a;
            move_to_awire(&ml.moves[pick], &a);
            bool ok;
            switch (a.kind) {
                case AWIRE_ATTACK: ok = handle_attack(&g, seat, a.cards, a.n); break;
                case AWIRE_COVER:  ok = handle_cover(&g, seat, a.cards, a.attacks, a.n); break;
                case AWIRE_PASS:   ok = handle_pass(&g, seat, a.cards, a.n); break;
                case AWIRE_PICKUP: ok = handle_pickup(&g, seat); break;
                default:           ok = handle_good(&g, seat); break;
            }
            if (!ok) break;
        }
    }
    fprintf(stderr, "no %dp board with a legal pass over two bare attacks\n", np);
    exit(1);
}

// --goodwait [np]: the table is waiting on US and nobody else. Two attacks,
// both covered, and every attacker EXCEPT one has said good - that one being
// the seat the frame is shot from, with a hand small enough to photograph.
//
// Like --passable, this needs its own search rather than a filter over an
// existing one. `fatboard` knows about table density and nothing about
// good_players_mask, and the chain playout cannot be steered into it: the
// state is TRANSIENT by construction, because the moment the last attacker
// says good the bout ends and the table clears. So the test runs BEFORE each
// move, which is the only window in which it exists.
static void print_goodwait(int np) {
    static unsigned char body[1024];
    static Game scratch;
    static LegalMoves ml;

    for (uint32_t s = 1; s < 20000; s++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 20260915u + s * 149u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        g_rng = 3313u + s;
        random_strategy_set_seed(g_rng);
        Game g;
        memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)np;
        for (int i = 0; i < np; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = 0;
        }
        start_game(&g);

        for (int step = 0; step < 600 && g.status == GAME_STATUS_PLAYING; step++) {
            if (game_done(&g) >= 0) break;

            // EVERY SEAT STILL IN. Without this the search happily returned an
            // "8 player" board with four seats already out (hands 0/8/0/8/0/5/0/1),
            // which photographs as a half-empty table and is not what an 8p
            // showcase frame means.
            int all_in = 1;
            for (int i = 0; i < np; i++)
                if (g.players[i].status != PLAYER_STATUS_IN) all_in = 0;
            if (all_in && g.num_battles == 2 &&
                g.table_battles[0].defense.value > 0 &&
                g.table_battles[1].defense.value > 0 &&
                g.defender >= 0) {
                int pending = -1, n_pending = 0, worst = 0, n_good = 0;
                for (int i = 0; i < np; i++) {
                    if (i == g.defender || g.players[i].status != PLAYER_STATUS_IN) continue;
                    if (g.players[i].hand_count > worst) worst = g.players[i].hand_count;
                    if (!(g.good_players_mask & (1u << i))) { pending = i; n_pending++; }
                    else n_good++;
                }
                // Exactly one attacker still to answer, holding a hand that fits
                // the shoot's rules (2 cards to select from, at most 6 on screen),
                // and nobody at the table sitting on an absurd pile.
                // AND THE TWO CARDS WE SELECT MUST BE A LEGAL THROW-IN.
                // A frame showing two tapped cards is a frame claiming a move
                // is available: an attacker may only add a card whose VALUE is
                // already on the table, so a pair of 10s over a table of 5s and
                // a 9 is a picture of an illegal move. Require a value that is
                // on the table AND that we hold at least twice, and report it
                // so the rig can tap those two cards rather than guess.
                int pair_value = 0;
                if (n_pending == 1 && pending >= 0) {
                    for (int b = 0; b < g.num_battles && !pair_value; b++) {
                        const int vals[2] = { g.table_battles[b].attack.value,
                                              g.table_battles[b].defense.value };
                        for (int k = 0; k < 2 && !pair_value; k++) {
                            if (vals[k] <= 0) continue;
                            int held = 0;
                            for (int c = 0; c < g.players[pending].hand_count; c++)
                                if (g.players[pending].hand[c].value == vals[k]) held++;
                            if (held >= 2) pair_value = vals[k];
                        }
                    }
                }
                if (n_pending == 1 && pending >= 0 && pair_value &&
                    g.players[pending].hand_count >= 2 &&
                    g.players[pending].hand_count <= 6 && worst <= 7) {
                    MsgEnvelope e;
                    env_init(&e, seed, np);
                    e.phase = MSG_PHASE_LIVE;
                    e.last_actor_seat = (uint8_t)g.defender;
                    if (msg_seal(&e, &g, MSG_NO_BASE, body, sizeof(body), &scratch) == MSG_EOK) {
                        unsigned char wire[ENV_CAP];
                        const int n = msg_encode(&e, wire, sizeof(wire));
                        if (n > 0) {
                            for (int b = 0; b < n; b++) printf("%02x", wire[b]);
                            printf("\n");
                            fprintf(stderr, "goodwait: np=%d seed#%u us=seat %d holds %d,"
                                    " defender=seat %d, %d attackers good, turn %d (%d bytes)"
                                    " hands=", np, s, pending,
                                    g.players[pending].hand_count, g.defender,
                                    n_good, e.turn, n);
                            for (int q = 0; q < np; q++)
                                fprintf(stderr, "%s%d", q ? "/" : "", g.players[q].hand_count);
                            fprintf(stderr, " pair=%s hand=", 
                                    pair_value >= 1 && pair_value <= 13
                                        ? (const char *[]){"?","2","3","4","5","6","7","8",
                                                           "9","10","J","Q","K","A"}[pair_value]
                                        : "?");
                            for (int c = 0; c < g.players[pending].hand_count; c++) {
                                const int v = g.players[pending].hand[c].value;
                                const int u = g.players[pending].hand[c].suit;
                                fprintf(stderr, "%s%s%c", c ? "," : "",
                                        v >= 1 && v <= 13
                                            ? (const char *[]){"?","2","3","4","5","6","7","8",
                                                               "9","10","J","Q","K","A"}[v] : "?",
                                        u >= 0 && u < 4 ? "SHCD"[u] : '?');
                            }
                            fprintf(stderr, "\n");
                            return;
                        }
                    }
                }
            }

            int seat = -1, pick = -1;
            const int start = (int)(rnd() % (uint32_t)np);
            for (int t = 0; t < np && seat < 0; t++) {
                const int c = (start + t) % np;
                if (g.players[c].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, c, &ml);
                for (int i = 0; i < ml.n; i++)
                    if (ml.moves[i].type != MOVE_WAIT) { seat = c; pick = i; break; }
            }
            if (seat < 0 || pick < 0) break;
            AwireAction a;
            move_to_awire(&ml.moves[pick], &a);
            bool ok;
            switch (a.kind) {
                case AWIRE_ATTACK: ok = handle_attack(&g, seat, a.cards, a.n); break;
                case AWIRE_COVER:  ok = handle_cover(&g, seat, a.cards, a.attacks, a.n); break;
                case AWIRE_PASS:   ok = handle_pass(&g, seat, a.cards, a.n); break;
                case AWIRE_PICKUP: ok = handle_pickup(&g, seat); break;
                default:           ok = handle_good(&g, seat); break;
            }
            if (!ok) break;
        }
    }
    fprintf(stderr, "no %dp board waiting on exactly one attacker\n", np);
    exit(1);
}

// --endgame [np]: a FINISHED chain, as one FMSG envelope in hex - the dev board
// for verifying what "New game" does at the end of a game (the fool's penalty).
//
// Same discipline as --fatboard: the state is searched here, in C, in
// microseconds, and the device just opens it. Reaching a finished 3-player game
// by tapping is minutes of work per attempt and the fool would differ every
// run, which is exactly what makes a filmed comparison worthless.
// `passing` chooses the VARIANT the game is played (and sealed) under, so the
// rig can pose "a podkidnoy game that has just ended" - the state a rematch has
// to carry its rules out of.
typedef struct {
    unsigned char wire[ENV_CAP];
    int n;
    MsgEnvelope e;              // the sealed header (its body pointer is stale)
    int fool, out_order[8], n_out;
} EndgameSeed;

static int endgame_seal(int np, int passing, int arrival, EndgameSeed *out) {
    static unsigned char body[1024];
    static Game scratch;
    static LegalMoves ml;

    for (uint32_t s = 1; s < 4000; s++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 20260822u + s * 89u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        g_rng = 17u + s;
        random_strategy_set_seed(g_rng);

        Game g;
        memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)np;
        if (!passing) g.rules |= GAME_RULE_NO_PASS;
        for (int i = 0; i < np; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = 0;
        }
        start_game(&g);

        int last_actor = g.first_attacker;
        int out_order[8], n_out = 0;
        int before_last = MSG_NO_BASE;   // the log mark the final move was made on
        for (int step = 0; step < 400; step++) {
            if (game_done(&g) >= 0 || g.status != GAME_STATUS_PLAYING) break;
            before_last = g.num_logs;
            int seat = -1, pick = -1;
            const int start = (int)(rnd() % (uint32_t)np);
            for (int t = 0; t < np && seat < 0; t++) {
                const int c = (start + t) % np;
                if (g.players[c].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, c, &ml);
                for (int i = 0; i < ml.n; i++)
                    if (ml.moves[i].type != MOVE_WAIT) { seat = c; pick = i; break; }
            }
            if (seat < 0 || pick < 0) break;
            AwireAction a;
            move_to_awire(&ml.moves[pick], &a);
            bool ok;
            switch (a.kind) {
                case AWIRE_ATTACK: ok = handle_attack(&g, seat, a.cards, a.n); break;
                case AWIRE_COVER:  ok = handle_cover(&g, seat, a.cards, a.attacks, a.n); break;
                case AWIRE_PASS:   ok = handle_pass(&g, seat, a.cards, a.n); break;
                case AWIRE_PICKUP: ok = handle_pickup(&g, seat); break;
                default:           ok = handle_good(&g, seat); break;
            }
            if (!ok) break;
            last_actor = seat;
            // WHO WENT OUT, IN ORDER - which is the ranking the result card
            // prints. Without it the only way to know whether the frame shows
            // us at #1 or at #7 is to shoot it and read the card, and an 8p
            // endgame that puts "Alex (You)" second from last is not a
            // showcase. Recorded here so the cast can be rotated to put our
            // name wherever the frame needs it.
            for (int q = 0; q < np; q++) {
                if (g.players[q].status == PLAYER_STATUS_IN) continue;
                int already = 0;
                for (int r = 0; r < n_out; r++) if (out_order[r] == q) already = 1;
                if (!already) out_order[n_out++] = q;
            }
        }
        game_settle_status(&g);
        const int fool = game_done(&g);
        if (fool < 0) continue;

        MsgEnvelope e;
        env_init(&e, seed, np);
        // `arrival`: the bubble the FINAL move's phone sends - FINISHED, like
        // every finished chain, but sealed on the log mark the final move was
        // made on, so its claim names that move and an open (REPLAY=1) watches
        // it replay and the game-over screen arrive after it. --endgame's
        // bubble says nothing about what it added, so it opens straight onto
        // the static end screen. Same search, same fool, two different bubbles.
        // (This used to seal LIVE. msg_replay refuses a finished game sealed
        // LIVE - MSG_EPHASE - so the extension had no chain to open and showed
        // New game; test_endgame_seeds holds it.)
        e.phase = MSG_PHASE_FINISHED;
        e.last_actor_seat = (uint8_t)last_actor;
        if (arrival) e.sent_at = (uint16_t)(time(NULL) & 0xffff);
        if (msg_seal(&e, &g, arrival ? before_last : MSG_NO_BASE, body, sizeof(body),
                     &scratch) != MSG_EOK) continue;
        out->n = msg_encode(&e, out->wire, sizeof(out->wire));
        if (out->n <= 0) continue;
        out->e = e;
        out->fool = fool;
        out->n_out = n_out;
        memcpy(out->out_order, out_order, sizeof(out_order));
        return 1;
    }
    return 0;
}

static void print_endgame(int np, int passing, int arrival) {
    static EndgameSeed s;
    if (!endgame_seal(np, passing, arrival, &s)) {
        fprintf(stderr, "no %dp endgame found\n", np);
        return;
    }
    for (int i = 0; i < s.n; i++) printf("%02x", s.wire[i]);
    printf("\n");
    fprintf(stderr, "endgame: np=%d %s fool=seat %d turn=%d round=%d (%d bytes) phase=%s",
            np, passing ? "perevodnoy" : "podkidnoy", s.fool, s.e.turn, s.e.round, s.n,
            arrival ? "FINISHED(arrival)" : "FINISHED");
    fprintf(stderr, " rank=");
    for (int r = 0; r < s.n_out; r++) fprintf(stderr, "%s%d", r ? "," : "", s.out_order[r]);
    fprintf(stderr, "\n");
}

// The --endgame-arrival seed is a bubble a phone could have sent: it decodes,
// it replays (msg_replay refuses a finished game sealed LIVE, MSG_EPHASE, and
// the extension then had no chain to open and showed New game), and its claim
// leaves the final move to animate. Same for --endgame, minus the claim.
static void test_endgame_seeds(void) {
    static EndgameSeed s;
    static Game rg;
    for (int np = 2; np <= 4; np++) {
        for (int arrival = 0; arrival < 2; arrival++) {
            CHECK(endgame_seal(np, 1, arrival, &s), "endgame seed %dp arrival=%d: none found", np, arrival);
            MsgEnvelope d;
            CHECK(msg_decode(s.wire, s.n, &d) == MSG_EOK, "endgame seed %dp arrival=%d: decode", np, arrival);
            const int rc = msg_replay(&d, &rg);
            CHECK(rc == MSG_EOK, "endgame seed %dp arrival=%d: the seed does not replay (%d), so "
                  "the extension opens New game", np, arrival, rc);
            if (!arrival) continue;
            const int claim = msg_atoms_before_claim(&d);
            CHECK(claim >= 0 && claim < d.turn,
                  "endgame-arrival %dp: claim %d of %d atoms leaves no final move to animate",
                  np, claim, d.turn);
        }
    }
}

// ---------- --lastdefense: the cover that ENDS the bout --------------------
//
// Round 16, the owner: "when you cover and cause the deck to discard (last
// defense), it should give some time to let people see what you covered with."
//
// Prints a LIVE envelope one tap short of that: the defender is on move and
// holds a cover which, applied, sweeps the table in the SAME kernel step - no
// attacker gets to say good, because the defender's last card just went down
// and there is nothing left to throw at them. Sit as the defender (dev.seat is
// written by the rig) and play the card; what follows is the sequence under
// test - cover lands, HOLD, then the discard and the deals.
//
// It cannot be posed from a deal, which is why it is searched: the shape needs
// a defender down to their last coverable card, i.e. an endgame. The playout is
// the same random one --endgame uses, stopped at the first state that poses it
// rather than run to the finish.
static int cover_ends_the_bout(const Game *g, int def, const LegalMove *m) {
    Game c = *g;                      // the kernel is pure over a Game; try it
    if (!handle_cover(&c, def, m->cards, m->attack_cards, m->n_cards)) return 0;
    return c.num_battles == 0;        // the table went with the cover
}

static void print_lastdefense(int np) {
    static unsigned char body[1024];
    static Game scratch;
    static LegalMoves ml;

    for (uint32_t s = 1; s < 8000; s++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 20260822u + s * 89u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        g_rng = 17u + s;
        random_strategy_set_seed(g_rng);

        Game g;
        memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)np;
        for (int i = 0; i < np; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = 0;
        }
        start_game(&g);

        int last_actor = g.first_attacker;
        for (int step = 0; step < 400; step++) {
            if (game_done(&g) >= 0 || g.status != GAME_STATUS_PLAYING) break;

            // Does THIS state pose it? Ask before moving, so what gets sealed
            // is the board the human will be handed.
            const int def = g.defender;
            if (def >= 0 && def < np && g.players[def].status == PLAYER_STATUS_IN
                && last_actor != def) {
                calculate_legal_moves(&g, def, &ml);
                for (int i = 0; i < ml.n; i++) {
                    if (ml.moves[i].type != MOVE_COVER) continue;
                    if (!cover_ends_the_bout(&g, def, &ml.moves[i])) continue;

                    MsgEnvelope e;
                    env_init(&e, seed, np);
                    e.phase = MSG_PHASE_LIVE;
                    e.last_actor_seat = (uint8_t)last_actor;
                    // A human opens this one, so stamp it now (same reasoning
                    // as --fatboard: not a byte-reproducible fixture).
                    e.sent_at = (uint16_t)(time(NULL) & 0xffff);
                    if (msg_seal(&e, &g, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK) break;
                    unsigned char wire[ENV_CAP];
                    const int n = msg_encode(&e, wire, sizeof(wire));
                    if (n <= 0) break;

                    int uncovered = 0;
                    for (int b = 0; b < g.num_battles; b++)
                        if (card_is_none(g.table_battles[b].defense)) uncovered++;
                    fprintf(stderr, "lastdefense: %dp seed#%u defender=seat %d holds %d, "
                                    "%d battles (%d uncovered), the closer is %d/%d, "
                                    "deck %d, turn %d (%d bytes)\n",
                            np, s, def, g.players[def].hand_count, g.num_battles, uncovered,
                            ml.moves[i].cards[0].suit, ml.moves[i].cards[0].value,
                            g.deck_count, e.turn, n);
                    for (int k = 0; k < n; k++) printf("%02x", wire[k]);
                    printf("\n");
                    return;
                }
            }

            int seat = -1, pick = -1;
            const int start = (int)(rnd() % (uint32_t)np);
            for (int t = 0; t < np && seat < 0; t++) {
                const int c = (start + t) % np;
                if (g.players[c].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, c, &ml);
                for (int i = 0; i < ml.n; i++)
                    if (ml.moves[i].type != MOVE_WAIT) { seat = c; pick = i; break; }
            }
            if (seat < 0 || pick < 0) break;
            AwireAction a;
            move_to_awire(&ml.moves[pick], &a);
            bool ok;
            switch (a.kind) {
                case AWIRE_ATTACK: ok = handle_attack(&g, seat, a.cards, a.n); break;
                case AWIRE_COVER:  ok = handle_cover(&g, seat, a.cards, a.attacks, a.n); break;
                case AWIRE_PASS:   ok = handle_pass(&g, seat, a.cards, a.n); break;
                case AWIRE_PICKUP: ok = handle_pickup(&g, seat); break;
                default:           ok = handle_good(&g, seat); break;
            }
            if (!ok) break;
            last_actor = seat;
        }
    }
    fprintf(stderr, "no %dp game in 8000 posed a bout-ending cover\n", np);
}

// ---------- --lastmove <kind> [np]: one targeted action, as an FMSG payload -
//
// Generic sibling to --lastdefense/--fatboard/--twocover: random-plays a game
// (same discipline as --endgame) and, at every step before applying anything,
// asks a speculative copy "does THIS legal move match `kind`?" - the first
// seat/move that does gets applied for real and the resulting LIVE envelope
// is sealed and printed. Built for round-16/17 QA capture across the move
// types the other canned searches do not reach: a plain attack, a cover that
// does not clear the table, a pickup, a pass, a good that does not close the
// bout, a move whose tail logs a player OUT or a refill (LOG_DRAW), and a
// refill that empties the deck.
#define LASTMOVE_ATTACK       0
#define LASTMOVE_COVER_MID    1   // covers, but battles remain (bout stays open)
#define LASTMOVE_PICKUP       2
#define LASTMOVE_PASS         3
#define LASTMOVE_GOOD_MID     4   // good, but the bout is not closed by it
#define LASTMOVE_OUT          5   // tail logs LOG_PLAYER_OUT
#define LASTMOVE_REFILL       6   // tail logs LOG_DRAW, deck not yet empty
#define LASTMOVE_REFILL_EMPTY 7   // tail logs LOG_DRAW and empties the deck
#define LASTMOVE_COVER_TRUMP  8   // covers with a TRUMP, bout stays open
#define LASTMOVE_FINAL        9   // the move that ends the game (arrival)
#define LASTMOVE_GOOD_ANY    10   // ANY legal good, closing the bout or not
#define LASTMOVE_REFILL_TRUMP 11  // a refill that deals the flipped trump out

static bool lastmove_apply(Game *g, int seat, const LegalMove *m) {
    switch (m->type) {
        case MOVE_ATTACK: return handle_attack(g, seat, m->cards, m->n_cards);
        case MOVE_COVER:  return handle_cover(g, seat, m->cards, m->attack_cards, m->n_cards);
        case MOVE_PASS:   return handle_pass(g, seat, m->cards, m->n_cards);
        case MOVE_PICKUP: return handle_pickup(g, seat);
        default:          return handle_good(g, seat);
    }
}

// `live`: seal the state ONE MOVE SHORT, instead of applying it - for kinds
// whose replay animates nothing (a mid-battle good is instantaneous state,
// not a flight - `--lastmove good` opens with the checkmark already there,
// events=0). A human seated as `act_seat` (printed to stderr) then plays the
// move themselves, live, same discipline as --lastdefense.
static void print_lastmove_ex(int np, int kind, int live) {
    static unsigned char body[1024];
    static Game scratch;
    static LegalMoves ml;

    for (uint32_t s = 1; s < 8000; s++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 20260901u + s * 89u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        g_rng = 31u + s;
        random_strategy_set_seed(g_rng);

        Game g;
        memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)np;
        for (int i = 0; i < np; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = 0;
        }
        start_game(&g);

        int last_actor = g.first_attacker;
        int found = 0;
        int pre_logs = -1;
        int trump_to = -1;
        for (int step = 0; step < 400 && !found; step++) {
            if (game_done(&g) >= 0 || g.status != GAME_STATUS_PLAYING) break;

            const int start = (int)(rnd() % (uint32_t)np);
            for (int t = 0; t < np && !found; t++) {
                const int seat = (start + t) % np;
                if (g.players[seat].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, seat, &ml);
                for (int i = 0; i < ml.n && !found; i++) {
                    const LegalMove *m = &ml.moves[i];
                    if (m->type == MOVE_WAIT) continue;
                    // play_human_menu's narrowing (legal.c): a human never
                    // sees Good over an uncovered attack, even though the raw
                    // bot-facing menu offers it. Any kind captured here is
                    // meant for a human (live tap or replay) to watch or play,
                    // so hold every kind to that same human-reachable menu.
                    if (m->type == MOVE_GOOD) {
                        bool all_covered_pre = true;
                        for (int b = 0; b < g.num_battles; b++)
                            if (card_is_none(g.table_battles[b].defense)) { all_covered_pre = false; break; }
                        if (!all_covered_pre) continue;
                    }

                    int want = 0;
                    switch (kind) {
                        case LASTMOVE_ATTACK: want = (m->type == MOVE_ATTACK); break;
                        case LASTMOVE_PICKUP: want = (m->type == MOVE_PICKUP); break;
                        case LASTMOVE_PASS:   want = (m->type == MOVE_PASS); break;
                        case LASTMOVE_COVER_MID: {
                            if (m->type != MOVE_COVER) break;
                            Game c = g;
                            if (!lastmove_apply(&c, seat, m)) break;
                            want = (c.num_battles > 0);
                            break;
                        }
                        case LASTMOVE_FINAL: {
                            Game c = g;
                            if (!lastmove_apply(&c, seat, m)) break;
                            want = (game_done(&c) >= 0);
                            break;
                        }
                        case LASTMOVE_COVER_TRUMP: {
                            if (m->type != MOVE_COVER) break;
                            if (m->cards[0].suit != g.power_suit) break;
                            Game c = g;
                            if (!lastmove_apply(&c, seat, m)) break;
                            want = (c.num_battles > 0);
                            break;
                        }
                        case LASTMOVE_GOOD_MID: {
                            if (m->type != MOVE_GOOD) break;
                            Game c = g;
                            if (!lastmove_apply(&c, seat, m)) break;
                            want = (c.status == GAME_STATUS_PLAYING && c.num_battles > 0);
                            break;
                        }
                        // `good` above is specifically a good that does NOT
                        // close the bout, which needs a second attacker - so it
                        // has no two-player instance at all, and reporting that
                        // as "2 players cannot say good" is wrong twice over.
                        // At two players a good is the ORDINARY way a bout ends,
                        // and it stages like any other move, so the player sees
                        // their own green check before they send it. This kind
                        // takes any legal good, closing or not.
                        case LASTMOVE_GOOD_ANY: {
                            if (m->type != MOVE_GOOD) break;
                            Game c = g;
                            if (!lastmove_apply(&c, seat, m)) break;
                            want = (c.status == GAME_STATUS_PLAYING);
                            break;
                        }
                        case LASTMOVE_OUT: {
                            Game c = g;
                            const int before = c.num_logs;
                            if (!lastmove_apply(&c, seat, m)) break;
                            for (int L = before; L < c.num_logs; L++)
                                if (c.logs[L].log_type == LOG_PLAYER_OUT) { want = 1; break; }
                            break;
                        }
                        case LASTMOVE_REFILL:
                        case LASTMOVE_REFILL_EMPTY: {
                            // Pickup also triggers a refill for the OTHER
                            // seats (round 16's design), which is a real path
                            // but not the one this kind is for - that is
                            // `--lastmove pickup`'s territory. Require the
                            // ordinary "bout closes and discards" refill.
                            if (m->type == MOVE_PICKUP) break;
                            Game c = g;
                            const int before = c.num_logs;
                            if (!lastmove_apply(&c, seat, m)) break;
                            int drew = 0;
                            for (int L = before; L < c.num_logs; L++)
                                if (c.logs[L].log_type == LOG_DRAW) drew = 1;
                            if (!drew) break;
                            want = (kind == LASTMOVE_REFILL_EMPTY)
                                       ? (c.deck_count == 0)
                                       : (c.deck_count > 0);
                            break;
                        }
                        // The draw that takes the flipped trump from under an
                        // empty stock: the board before still holds it, the
                        // board after does not. Any move that refills counts,
                        // a pickup included (the other seats draw), and the
                        // game must still be on so the bubble opens a board.
                        // The seat that drew it is printed (trump_to=seat N),
                        // so a film can sit in that chair or another one.
                        case LASTMOVE_REFILL_TRUMP: {
                            if (!g.has_flipped) break;
                            Game c = g;
                            const int before = c.num_logs;
                            if (!lastmove_apply(&c, seat, m)) break;
                            if (c.has_flipped || c.status != GAME_STATUS_PLAYING) break;
                            // FOOLISH_TRUMP_DRAW=N: the draw that took the
                            // trump took at least N cards, so a film shows
                            // backs from the stock beside the trump's face.
                            const char *min_s = getenv("FOOLISH_TRUMP_DRAW");
                            const int min_n = min_s ? atoi(min_s) : 1;
                            for (int L = c.num_logs - 1; L >= before; L--)
                                if (c.logs[L].log_type == LOG_DRAW) {
                                    if (c.logs[L].num_pairs >= min_n) {
                                        trump_to = c.logs[L].player_idx; want = 1;
                                    }
                                    break;
                                }
                            break;
                        }
                        default: break;
                    }
                    if (!want) continue;

                    if (live) {
                        // Seal ONE MOVE SHORT: `seat` is who must play it, on
                        // the device, for the animation (or state change) to
                        // exist at all. `last_actor` is left as whoever acted
                        // before - there is no move to attribute to `seat` yet.
                        fprintf(stderr, "lastmove-live: act_seat=%d card=%d/%d "
                                        "type=%d\n",
                                seat, m->cards[0].suit, m->cards[0].value, m->type);
                        found = 1;
                        break;
                    }
                    pre_logs = g.num_logs;   // the mark: everything from here
                                              // on is what this bubble is FOR
                    if (!lastmove_apply(&g, seat, m)) continue;
                    last_actor = seat;
                    found = 1;
                }
            }
            if (found) break;

            // Nothing matched this step: advance one random legal move (same
            // discipline as --endgame/--lastdefense) and keep looking.
            int seat = -1, pick = -1;
            const int start2 = (int)(rnd() % (uint32_t)np);
            for (int t = 0; t < np && seat < 0; t++) {
                const int c = (start2 + t) % np;
                if (g.players[c].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, c, &ml);
                for (int i = 0; i < ml.n; i++)
                    if (ml.moves[i].type != MOVE_WAIT) { seat = c; pick = i; break; }
            }
            if (seat < 0 || pick < 0) break;
            if (!lastmove_apply(&g, seat, &ml.moves[pick])) break;
            last_actor = seat;
        }
        if (!found) continue;

        MsgEnvelope e;
        env_init(&e, seed, np);
        e.phase = MSG_PHASE_LIVE;
        e.last_actor_seat = (uint8_t)last_actor;
        e.sent_at = (uint16_t)(time(NULL) & 0xffff);
        // `pre_logs`, not MSG_NO_BASE: this is the mark this bubble is a delta
        // FROM, so `dev.replay` animates only the one move `kind` searched
        // for - not the "guess the boundary" fallback a NO_BASE seal gets,
        // which is most of the match (round 16's bubble delta, n_new).
        if (msg_seal(&e, &g, pre_logs, body, sizeof(body), &scratch) != MSG_EOK) continue;
        unsigned char wire[ENV_CAP];
        const int n = msg_encode(&e, wire, sizeof(wire));
        if (n <= 0) continue;

        fprintf(stderr, "lastmove: kind=%d np=%d seed#%u last_actor=seat %d deck=%d "
                        "turn=%d round=%d n_new=%d (%d bytes)\n",
                kind, np, s, last_actor, g.deck_count, e.turn, e.round, e.n_new, n);
        if (trump_to >= 0) fprintf(stderr, "lastmove: trump_to=seat %d\n", trump_to);
        for (int i = 0; i < n; i++) printf("%02x", wire[i]);
        printf("\n");
        return;
    }
    fprintf(stderr, "no %dp game in 8000 posed lastmove kind %d\n", np, kind);
    exit(1);
}

// ---------- --chain: N CONSECUTIVE bubbles of ONE real game ----------------
//
// Every other mode here seals ONE state. That is enough for a board, and it is
// NOT enough for a transcript: Messages shows the history above the drawer, so
// a photograph needs several bubbles that actually follow one another. Sealing
// several independent searches instead produces a chain no game could play -
// the deck count jumping about, an attacking queen becoming a covering queen,
// the defender changing hands between one line and the next. The owner caught
// exactly that, twice.
//
// So: play ONE game with the same random discipline as --endgame, seal an
// envelope after each of the last `count` moves, and print them oldest first.
// Each line to stderr names the seat that acted, because the caller has to know
// whose message it is - ours goes out from our thread and renders on the right,
// theirs from the other one and renders on the left.
//
// Usage: msg_wire_test --chain [n_players] [count] [depth]
static void print_chain(int np, int count, int depth) {
    static unsigned char body[1024];
    static Game scratch;
    static LegalMoves ml;
    if (count < 1) count = 1;
    if (count > 16) count = 16;

    for (uint32_t s = 1; s < 4000; s++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 20260913u + s * 131u);
        // TIE THE DEAL TO THE ENVELOPE SEED. The v6 body encodes ACTIONS, not
        // cards: the decoder re-deals from this seed and replays them. Deal the
        // game any other way and every action is illegal against the hand the
        // decoder built, and msg_seal refuses the body (MSG_EBODY) with no
        // detail to explain itself.
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        g_rng = 4242u + s;
        random_strategy_set_seed(g_rng);
        Game g;
        memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)np;
        for (int i = 0; i < np; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = 0;
        }
        start_game(&g);

        // A NEGATIVE depth anchors the chain to the END of the game instead of
        // to an absolute move number: -1 means "the last `count` moves, the
        // last of them the one that ends it".
        //
        // It needs a counting pass because a game's length is a property of the
        // seed, not something a caller can know - and without it a result card
        // can only ever be photographed over a transcript belonging to some
        // OTHER game, which is exactly the kind of frame that does not survive
        // a close look. The count is replayed rather than rewound: the deal is
        // a function of the seed and both RNGs are re-seeded below, so the
        // second pass is the same game move for move.
        int use_depth = depth;
        if (depth < 0) {
            Game cg = g;
            int moves = 0;
            for (; cg.status == GAME_STATUS_PLAYING && moves < 512; moves++) {
                if (game_done(&cg) >= 0) break;
                int cseat = -1, cpick = -1;
                const int cstart = (int)(rnd() % (uint32_t)np);
                for (int t = 0; t < np && cseat < 0; t++) {
                    const int c = (cstart + t) % np;
                    if (cg.players[c].status != PLAYER_STATUS_IN) continue;
                    calculate_legal_moves(&cg, c, &ml);
                    for (int i = 0; i < ml.n; i++)
                        if (ml.moves[i].type != MOVE_WAIT) { cseat = c; cpick = i; break; }
                }
                if (cseat < 0 || cpick < 0) break;
                AwireAction ca;
                move_to_awire(&ml.moves[cpick], &ca);
                bool cok;
                switch (ca.kind) {
                    case AWIRE_ATTACK: cok = handle_attack(&cg, cseat, ca.cards, ca.n); break;
                    case AWIRE_COVER:  cok = handle_cover(&cg, cseat, ca.cards, ca.attacks, ca.n); break;
                    case AWIRE_PASS:   cok = handle_pass(&cg, cseat, ca.cards, ca.n); break;
                    case AWIRE_PICKUP: cok = handle_pickup(&cg, cseat); break;
                    default:           cok = handle_good(&cg, cseat); break;
                }
                if (!cok) break;
            }
            // Only a game that actually ENDED can be anchored to its end.
            if (game_done(&cg) < 0 && cg.status == GAME_STATUS_PLAYING) continue;
            // `moves` is how many were PLAYED, and the last of them is the one
            // that ends the game, so the window starts `count` before it - not
            // `count - 1`, which seals five of six and rejects every seed.
            use_depth = moves - count;
            if (use_depth < 0) continue;
            g_rng = 4242u + s;
            random_strategy_set_seed(g_rng);
        }

        // Replay buffers: one envelope per kept move, plus the log mark each
        // was a delta FROM, so every bubble animates only its own move.
        unsigned char wires[16][ENV_CAP];
        int lens[16], actors[16], phases[16], kept = 0;
        int kinds[16], ncards[16], battles[16], covered[16], hands[16][8];
        Card handcards[2][MAX_HAND_SIZE];
        Card acards[16][6];

        int step = 0;
        for (; step < use_depth + count && g.status == GAME_STATUS_PLAYING; step++) {
            if (game_done(&g) >= 0) break;
            int seat = -1, pick = -1;
            const int start = (int)(rnd() % (uint32_t)np);
            for (int t = 0; t < np && seat < 0; t++) {
                const int c = (start + t) % np;
                if (g.players[c].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, c, &ml);
                for (int i = 0; i < ml.n; i++)
                    if (ml.moves[i].type != MOVE_WAIT) { seat = c; pick = i; break; }
            }
            if (seat < 0 || pick < 0) break;

            const int pre_logs = g.num_logs;
            AwireAction a;
            move_to_awire(&ml.moves[pick], &a);
            bool ok;
            switch (a.kind) {
                case AWIRE_ATTACK: ok = handle_attack(&g, seat, a.cards, a.n); break;
                case AWIRE_COVER:  ok = handle_cover(&g, seat, a.cards, a.attacks, a.n); break;
                case AWIRE_PASS:   ok = handle_pass(&g, seat, a.cards, a.n); break;
                case AWIRE_PICKUP: ok = handle_pickup(&g, seat); break;
                default:           ok = handle_good(&g, seat); break;
            }
            if (!ok) break;
            if (step < use_depth) continue;      // still warming the game up

            MsgEnvelope e;
            env_init(&e, seed, np);
            // The phase is READ, not assumed. It used to be pinned to LIVE,
            // which made the last bubble of a chain that actually ends the game
            // claim the game was still running - and a result card photographed
            // over that transcript is a state the thread could not have
            // reached. A chain that finishes should say so.
            e.phase = (game_done(&g) >= 0 || g.status != GAME_STATUS_PLAYING)
                      ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE;
            e.last_actor_seat = (uint8_t)seat;
            e.sent_at = (uint16_t)(time(NULL) & 0xffff);
            if (msg_seal(&e, &g, pre_logs, body, sizeof(body), &scratch) != MSG_EOK) break;
            const int n = msg_encode(&e, wires[kept], sizeof(wires[kept]));
            if (n <= 0) break;
            lens[kept] = n; actors[kept] = seat;
            // WHAT THE ENTRY SAYS HAPPENED, in the log rather than in a reader's
            // head. A transcript shoot compares the sentence a bubble displays
            // against "the move the chain made", and until this line that
            // second half was inferred from an actor number and a guess about
            // whose turn it was. Two consecutive bubbles displaying the SAME
            // sentence is the tell that matters, and it is only visible when
            // the expected sentences are written down next to each other.
            kinds[kept] = a.kind; ncards[kept] = a.n;
            battles[kept] = g.num_battles;
            covered[kept] = 0;
            for (int b = 0; b < g.num_battles; b++)
                if (g.table_battles[b].defense.value > 0) covered[kept]++;
            // EVERY SEAT'S HAND, not just the actor's. Printing the actor's
            // alone picked a transcript depth whose last entry looked fine
            // (hand=4) while OUR seat held nine cards and fanned them thin -
            // the frame failed the skinny gate after it was shot. The point of
            // logging this is to choose without shooting, so it has to name
            // the hand the frame will actually show.
            for (int q = 0; q < np && q < 8; q++)
                hands[kept][q] = g.players[q].hand_count;
            if (np == 2)
                for (int q = 0; q < 2; q++)
                    for (int c = 0; c < g.players[q].hand_count && c < MAX_HAND_SIZE; c++)
                        handcards[q][c] = g.players[q].hand[c];
            for (int c = 0; c < a.n && c < 6; c++) acards[kept][c] = a.cards[c];
            phases[kept] = e.phase; kept++;
            if (kept >= count) break;
        }
        if (kept < count) continue;

        fprintf(stderr, "chain: %dp seed#%u depth=%d, %d consecutive bubbles\n",
                np, s, use_depth, kept);
        for (int i = 0; i < kept; i++) {
            static const char *kindname[] = { "attack", "cover", "pass", "pickup", "good" };
            // The repo's own table (c/src/main_analyse.c, main_eval.c,
            // octogen_strategy.c all carry it): value 1 is a TWO, and the ace
            // is 13. A hand-rolled "1 is an ace" version of this printed every
            // card one rank low, and a shoot spent an afternoon treating the
            // product's correct sentences as a bug because they disagreed with
            // it. Copy the table, do not re-derive it.
            static const char rankname[14][3] = { "?", "2", "3", "4", "5", "6", "7", "8",
                                                  "9", "10", "J", "Q", "K", "A" };
            static const char suitname[4] = { 'S', 'H', 'C', 'D' };
            fprintf(stderr, "chain[%d]: actor=seat %d %s", i, actors[i],
                    kinds[i] >= 0 && kinds[i] <= 4 ? kindname[kinds[i]] : "?");
            for (int c = 0; c < ncards[i] && c < 6; c++) {
                const int v = acards[i][c].value;
                fprintf(stderr, " %s%c",
                        v >= 1 && v <= 13 ? rankname[v] : "?",
                        acards[i][c].suit >= 0 && acards[i][c].suit < 4
                            ? suitname[acards[i][c].suit] : '?');
            }
            // THE SHAPE OF THE TABLE, which is what a collapsed frame is
            // actually composed of. The owner's spec for the transcript shots
            // is "two attack cards on the table with one covered" - a board
            // that stays legible at bubble size - and without this the only
            // way to find a depth that produces one is to shoot a frame and
            // look at it, which costs minutes per guess.
            fprintf(stderr, " (%d bytes) atk=%d cov=%d hands=", lens[i],
                    battles[i], covered[i]);
            for (int q = 0; q < np && q < 8; q++)
                fprintf(stderr, "%s%d", q ? "/" : "", hands[i][q]);
            // THE ACTUAL CARDS, for 2p and for the last entry only - which is
            // the one a transcript frame photographs. Counts are not enough to
            // choose a state by: a four-card hand that happens to be four RED
            // cards reads as monotonous in the frame, and the only way to know
            // before shooting is to print the suits.
            if (np == 2 && i == kept - 1) {
                for (int q = 0; q < 2; q++) {
                    fprintf(stderr, " hand%d=", q);
                    for (int c = 0; c < hands[i][q] && c < MAX_HAND_SIZE; c++) {
                        const int v = handcards[q][c].value;
                        fprintf(stderr, "%s%s%c", c ? "," : "",
                                v >= 1 && v <= 13 ? rankname[v] : "?",
                                handcards[q][c].suit >= 0 && handcards[q][c].suit < 4
                                    ? suitname[handcards[q][c].suit] : '?');
                    }
                }
            }
            fprintf(stderr, "%s\n",
                    phases[i] == MSG_PHASE_FINISHED ? " FINISHED" : "");
            for (int b = 0; b < lens[i]; b++) printf("%02x", wires[i][b]);
            printf("\n");
        }
        return;
    }
    fprintf(stderr, "no %dp game gave %d consecutive bubbles at depth %d\n", np, count, depth);
    exit(1);
}

// `nopass` seals a PODKIDNOY (throw-in) board instead of the default
// perevodnoy one - the two render differently, so a lane that only ever shot
// the default has never looked at half the product. `preroll` plays that many
// whole bouts of random legal play FIRST, which is the only way to reach the
// states a freshly dealt table cannot show: a drained deck, a player already
// out, a hand big enough to wrap. Both default to the old behaviour (0, 0).
static void print_fatboard(int target, int np, int nopass, int preroll) {
    static unsigned char body[1024];
    static Game scratch;
    static LegalMoves ml;

    for (uint32_t s = 1; s < 4000; s++) {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 20260821u + s * 97u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        Game g;
        memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)np;
        if (nopass) g.rules |= GAME_RULE_NO_PASS;
        for (int i = 0; i < np; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = 0;
        }
        start_game(&g);

        // Play `preroll` whole bouts of random legal play before the table
        // search, so the board that gets sealed is a mid- or late-game one.
        g_rng = 7777u + s;
        if (preroll > 0) {
            int bouts = 0;
            for (int step = 0; step < 800 && bouts < preroll; step++) {
                if (g.status != GAME_STATUS_PLAYING || game_done(&g) >= 0) break;
                int seat = -1, pick = -1;
                const int start = (int)(rnd() % (uint32_t)np);
                for (int t = 0; t < np && seat < 0; t++) {
                    const int c = (start + t) % np;
                    if (g.players[c].status != PLAYER_STATUS_IN) continue;
                    calculate_legal_moves(&g, c, &ml);
                    for (int i = 0; i < ml.n; i++)
                        if (ml.moves[i].type != MOVE_WAIT) { seat = c; pick = i; break; }
                }
                if (seat < 0 || pick < 0) break;
                AwireAction a;
                move_to_awire(&ml.moves[pick], &a);
                const int battles_before = g.num_battles;
                bool ok;
                switch (a.kind) {
                    case AWIRE_ATTACK: ok = handle_attack(&g, seat, a.cards, a.n); break;
                    case AWIRE_COVER:  ok = handle_cover(&g, seat, a.cards, a.attacks, a.n); break;
                    case AWIRE_PASS:   ok = handle_pass(&g, seat, a.cards, a.n); break;
                    case AWIRE_PICKUP: ok = handle_pickup(&g, seat); break;
                    default:           ok = handle_good(&g, seat); break;
                }
                if (!ok) break;
                // A bout closes exactly when the table empties (msg_wire.c:439).
                if (battles_before > 0 && g.num_battles == 0) bouts++;
            }
            if (g.status != GAME_STATUS_PLAYING || game_done(&g) >= 0) continue;
        }

        // Attack first, cover when stuck. Single-card attacks only, so the table
        // grows one slot at a time and lands ON the target instead of stepping
        // over it.
        for (int step = 0; step < 200; step++) {
            int on_table = 0;
            for (int i = 0; i < g.num_battles; i++) {
                on_table += 1 + (card_is_none(g.table_battles[i].defense) ? 0 : 1);
            }
            if (on_table >= target) break;
            if (g.status != GAME_STATUS_PLAYING) break;

            const int def = g.defender;
            int acted = 0;
            for (int seat = 0; seat < np && !acted; seat++) {
                if (seat == def || g.players[seat].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, seat, &ml);
                for (int i = 0; i < ml.n; i++) {
                    if (ml.moves[i].type != MOVE_ATTACK || ml.moves[i].n_cards != 1) continue;
                    if (handle_attack(&g, seat, ml.moves[i].cards, 1)) acted = 1;
                    break;
                }
            }
            if (!acted && g.players[def].hand_count > 1) {
                calculate_legal_moves(&g, def, &ml);
                for (int i = 0; i < ml.n; i++) {
                    if (ml.moves[i].type != MOVE_COVER) continue;
                    if (handle_cover(&g, def, ml.moves[i].cards,
                                     ml.moves[i].attack_cards, ml.moves[i].n_cards)) acted = 1;
                    break;
                }
            }
            if (!acted) break;
        }

        int on_table = 0, covered = 0;
        for (int i = 0; i < g.num_battles; i++) {
            on_table += 1;
            if (!card_is_none(g.table_battles[i].defense)) { on_table++; covered++; }
        }
        // Covered pairs are the point, not a bonus: the pickup sweep's
        // reconstruction lays one card per slot, so it can only diverge from the
        // real table where a real pair exists to be split.
        if (on_table < target || covered < 2) continue;
        if (g.status != GAME_STATUS_PLAYING) continue;

        // ROUND 16: END ON AN ATTACK. The search above stops as soon as the
        // table is dense enough, and it covers as it goes, so it almost always
        // lands on a COVER - a state in which the defender is not facing
        // anything new and the 15-second pickup hold correctly does not apply.
        // A seeded board is the only way to put that hold on screen, so lay one
        // more attack on top, and require that the defender keeps spare capacity
        // (otherwise the capacity waiver lifts the hold for its own good
        // reasons). Fixtures that cannot do both are skipped rather than sealed
        // silently unheld.
        // FOOLISH_PREV: seal the state as it stands BEFORE the throw-in below,
        // so a caller can send that bubble first and this one second. They are
        // then genuinely consecutive - the second is the first plus exactly one
        // attack - which is what makes a transcript honest: Messages collapses
        // the older send into a CAPTION LINE describing its last move, and that
        // line now describes a move this board actually contains. Sending two
        // independently searched boards produces the same caption and a lie.
        static Game g_prev;
        if (getenv("FOOLISH_PREV")) g_prev = g;
        {
            int threw = 0;
            for (int seat = 0; seat < np && !threw; seat++) {
                if (seat == g.defender || g.players[seat].status != PLAYER_STATUS_IN) continue;
                calculate_legal_moves(&g, seat, &ml);
                for (int i = 0; i < ml.n; i++) {
                    if (ml.moves[i].type != MOVE_ATTACK || ml.moves[i].n_cards != 1) continue;
                    if (handle_attack(&g, seat, ml.moves[i].cards, 1)) threw = 1;
                    break;
                }
            }
            if (!threw) continue;
            int uncovered = 0;
            for (int i = 0; i < g.num_battles; i++) {
                if (card_is_none(g.table_battles[i].defense)) uncovered++;
            }
            if (uncovered >= g.players[g.defender].hand_count) continue;
            if (getenv("FOOLISH_PREV")) g = g_prev;   // hand back the earlier state
        }

        // FOOLISH_PLAYABLE: only seal a board the DEFENDER can actually move
        // on. A photograph wants the state to be one somebody is about to act
        // in, and a screenshot rig that drives the real UI needs a legal move
        // to exist before it can make one - the search above is free to stop on
        // a table where every uncovered attack beats every card in the
        // defender's hand, and the only move left there is a pickup, which
        // empties the table and the photograph with it.
        if (getenv("FOOLISH_PLAYABLE")) {
            calculate_legal_moves(&g, g.defender, &ml);
            int can_cover = 0;
            for (int i = 0; i < ml.n; i++)
                if (ml.moves[i].type == MOVE_COVER) { can_cover = 1; break; }
            if (!can_cover) continue;
        }

        MsgEnvelope e;
        env_init(&e, seed, np);
        e.phase = MSG_PHASE_LIVE;
        // The attacker sealed this one now (see the throw-in above), which is
        // what a defender opening it is meant to be reacting to.
        e.last_actor_seat = (uint8_t)(g.logs[g.num_logs - 1].player_idx);
        // ROUND 16: stamp the seeded bubble with THIS MACHINE's clock, so a
        // board opened from it is one whose last attack JUST happened - the only
        // way a seeded fixture can put the 15-second pickup hold on screen. The
        // clock is fine here and nowhere else in this file: this is the one
        // entry that prints a payload for a HUMAN to open, not a fixture that
        // has to reproduce byte for byte.
        e.sent_at = (uint16_t)(time(NULL) & 0xffff);
        if (msg_seal(&e, &g, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK) continue;
        unsigned char wire[ENV_CAP];
        const int n = msg_encode(&e, wire, sizeof(wire));
        if (n <= 0) continue;

        // ROUND 16: say whether this fixture actually poses the pickup hold, so a
        // run that shows the Pickup pill immediately is read as "this seed ends
        // on a cover" and not as "the hold is broken".
        fprintf(stderr, "fatboard: last log %d, hold %ds\n",
                g.num_logs ? g.logs[g.num_logs - 1].log_type : -1,
                msg_pickup_hold_remaining(&g, g.defender, e.sent_at, e.sent_at));
        // The DISCARD count is reported because a photograph wants one: a
        // board with an empty discard reads as the very first bout of a game,
        // and `preroll` alone does not guarantee otherwise - a bout that ends
        // in a PICKUP puts its cards in a hand, not on the discard pile.
        fprintf(stderr, "fatboard: %dp seed#%u  %d cards on table (%d covered), "
                        "defender=seat %d holds %d, turn %d round %d, deck %d, "
                        "discard %d, %s, %d bytes\n",
                np, s, on_table, covered, g.defender,
                g.players[g.defender].hand_count, e.turn, e.round,
                g.deck_count, g.discard_pile_length,
                nopass ? "podkidnoy" : "perevodnoy", n);
        for (int i = 0; i < n; i++) printf("%02x", wire[i]);
        printf("\n");
        return;
    }
    fprintf(stderr, "no %dp deal in 4000 tries reached a %d-card table\n", np, target);
    exit(1);
}

static void print_fixtures(void) {
    const int pcs[] = { 2, 3, 4 };
    for (int pi = 0; pi < 3; pi++) {
        const int np = pcs[pi];
        const int bot = bot_roster_find("robusta");
        int emitted = 0;
        // Search seeds and cut points for a mid-bout state that poses the race
        // AND has closed at least one round (so `round` is a live field, not 0).
        for (uint32_t s = 0; s < 400 && !emitted; s++) {
            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, 20260716u + s * 131u + (uint32_t)np);
            for (int cut = 8; cut <= 60 && !emitted; cut++) {
                g_rng = 7u + s;
                Chain ch; memset(&ch, 0, sizeof(ch));
                Game played;
                const int rounds = play_game(seed, np, cut, &ch, &played, bot);
                if (ch.n < cut) break;                       // the game ended first
                if (played.status != GAME_STATUS_PLAYING) break;
                if (rounds < 1) continue;                    // want round > 0
                if (!poses_the_race(&played)) continue;

                MsgEnvelope e;
                env_init(&e, seed, np);
                e.phase = MSG_PHASE_LIVE;
                static unsigned char body[1024];
                static Game scratch;
                if (msg_seal(&e, &played, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK) continue;
                unsigned char wire[ENV_CAP];
                const int n = msg_encode(&e, wire, sizeof(wire));
                if (n <= 0) continue;
                printf("%d %d %d %d %u ", np, e.turn, e.round, e.n_new, e.sent_at);
                for (int i = 0; i < n; i++) printf("%02x", wire[i]);
                printf("\n");

                // …and the SAME state sealed as format 3, so the goldens cover
                // the round-16 header too: a clock, and a bubble delta that says
                // the last two atoms are this bubble's. A cross-engine fixture
                // is the only thing that proves the web reads the new bytes the
                // way the phone wrote them - the two parse the header in
                // different languages, and a silent disagreement would put a
                // browser and a phone on different games.
                MsgEnvelope f;
                env_init(&f, seed, np);
                f.phase = MSG_PHASE_LIVE;
                f.sent_at = 0x1234;
                static unsigned char body3[1024];
                if (msg_seal(&f, &played, e.turn - 2, body3, sizeof(body3), &scratch) != MSG_EOK) continue;
                unsigned char wire3[ENV_CAP];
                const int n3 = msg_encode(&f, wire3, sizeof(wire3));
                if (n3 <= 0) continue;
                printf("%d %d %d %d %u ", np, f.turn, f.round, f.n_new, f.sent_at);
                for (int i = 0; i < n3; i++) printf("%02x", wire3[i]);
                printf("\n");
                emitted = 1;
            }
        }
        if (!emitted) fprintf(stderr, "no %dp fixture posed the race\n", np);
    }
}

// --fixture4: the same job for the FOOL'S PENALTY, one line per player count.
// A format-4 envelope cannot come out of print_fixtures because it needs a deal
// that was PINNED - the whole point is an opening seat the deal would not
// derive - so it gets its own generator rather than a flag on that one.
//
// What the cross-engine fixture buys, and print_fixtures cannot: the wasm
// kernel must re-deal from the seed with the SAME pin, or the body's atoms land
// on a board where they are not legal and the decode fails outright. So a
// passing fixture proves both halves at once - that the web reads the six new
// header bytes where the phone wrote them, and that it honours what they say.
static void print_fixtures4(void) {
    const int pcs[] = { 2, 3, 4 };
    for (int pi = 0; pi < 3; pi++) {
        const int np = pcs[pi];
        int emitted = 0;
        for (uint32_t s = 0; s < 400 && !emitted; s++) {
            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, 5150u + s * 97u + (uint32_t)np);

            // What the deal WOULD do on its own, so the fixture is only emitted
            // where the penalty genuinely overrides it.
            game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
            Game probe;
            memset(&probe, 0, sizeof(probe));
            probe.num_players = (int8_t)np;
            for (int i = 0; i < np; i++) probe.players[i].status = PLAYER_STATUS_READY;
            start_game(&probe);
            const int derived = probe.first_attacker;

            // Cast the DERIVED opener as the fool. That is what makes the
            // fixture worth having: the penalty then opens on the seat to their
            // right, which is never the seat the deal would have chosen, at any
            // player count. (Casting the fool as the player they would have
            // attacked collapses to the derived seat at 2 players.)
            const int opening = (derived - 1 + np) % np;
            if (opening == derived) continue;

            game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
            Game g;
            memset(&g, 0, sizeof(g));
            g.num_players = (int8_t)np;
            for (int i = 0; i < np; i++) {
                g.players[i].status = PLAYER_STATUS_READY;
                g.players[i].strategy_key = 0;
            }
            game_open_at_seat(opening);
            start_game(&g);
            game_open_at_seat(-1);
            if (g.first_attacker != (int8_t)opening) continue;

            // A short real chain, so the body is worth replaying.
            static LegalMoves ml;
            int last_actor = opening, played = 0;
            for (int step = 0; step < 10; step++) {
                if (game_done(&g) >= 0 || g.status != GAME_STATUS_PLAYING) break;
                int seat = -1, pick = -1;
                for (int t = 0; t < np && seat < 0; t++) {
                    if (g.players[t].status != PLAYER_STATUS_IN) continue;
                    calculate_legal_moves(&g, t, &ml);
                    for (int i = 0; i < ml.n; i++)
                        if (ml.moves[i].type != MOVE_WAIT) { seat = t; pick = i; break; }
                }
                if (seat < 0 || pick < 0) break;
                AwireAction a;
                move_to_awire(&ml.moves[pick], &a);
                bool ok;
                switch (a.kind) {
                    case AWIRE_ATTACK: ok = handle_attack(&g, seat, a.cards, a.n); break;
                    case AWIRE_COVER:  ok = handle_cover(&g, seat, a.cards, a.attacks, a.n); break;
                    case AWIRE_PASS:   ok = handle_pass(&g, seat, a.cards, a.n); break;
                    case AWIRE_PICKUP: ok = handle_pickup(&g, seat); break;
                    default:           ok = handle_good(&g, seat); break;
                }
                if (!ok) break;
                last_actor = seat;
                played++;
            }
            if (played < 3 || g.status != GAME_STATUS_PLAYING) continue;

            MsgEnvelope e;
            env_init(&e, seed, np);
            e.phase = MSG_PHASE_LIVE;
            e.last_actor_seat = (uint8_t)last_actor;
            e.sent_at = 0x1234;
            e.opening = (uint8_t)opening;
            static unsigned char body[1024];
            static Game scratch;
            if (msg_seal(&e, &g, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK) continue;
            unsigned char wire[ENV_CAP];
            const int n = msg_encode(&e, wire, sizeof(wire));
            if (n <= 0) continue;
            printf("%d %d %d %d %d %u ", np, e.turn, e.round, opening, derived, e.sent_at);
            for (int i = 0; i < n; i++) printf("%02x", wire[i]);
            printf("\n");
            emitted = 1;
        }
        if (!emitted) fprintf(stderr, "no %dp penalty fixture found\n", np);
    }
}

// --fixture5: a PODKIDNOY chain, one line per player count, for the same
// cross-engine job (docs/PODKIDNOY.md).
//
// What this one buys that the others cannot: the wasm kernel has to read a
// format it did not have yesterday, take the rules off its variant byte, deal a
// game under them, and build the SAME legal-move menu the phone built - because
// the body is a sequence of indices into that menu. Read the byte and ignore
// it, or build the perevodnoy menu anyway, and the atoms land on different
// moves and the decode fails outright. There is no way to pass this fixture
// while disagreeing about the rules.
static void print_fixtures5(void) {
    const int pcs[] = { 2, 3, 4 };
    for (int pi = 0; pi < 3; pi++) {
        const int np = pcs[pi];
        int emitted = 0;
        for (uint32_t s = 0; s < 400 && !emitted; s++) {
            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, 7300u + s * 61u + (uint32_t)np);

            Chain ch; memset(&ch, 0, sizeof(ch));
            Game g;
            g_rng = 7300u + s;
            play_game_rules(seed, np, 12, &ch, &g, -1, GAME_RULE_NO_PASS);
            if (g.status != GAME_STATUS_PLAYING || g.num_logs < 4) continue;

            MsgEnvelope e;
            env_init(&e, seed, np);
            e.phase = MSG_PHASE_LIVE;
            e.sent_at = 0x1234;
            static unsigned char body[1024];
            static Game scratch;
            if (msg_seal(&e, &g, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK) continue;
            if (e.turn < 3) continue;
            unsigned char wire[ENV_CAP];
            const int n = msg_encode(&e, wire, sizeof(wire));
            if (n <= 0) continue;
            printf("%d %d %d %u ", np, e.turn, e.round, e.sent_at);
            for (int i = 0; i < n; i++) printf("%02x", wire[i]);
            printf("\n");
            emitted = 1;
        }
        if (!emitted) fprintf(stderr, "no %dp podkidnoy fixture found\n", np);
    }
}

// --holdcheck <hex>: decode a payload exactly as a device does and print the
// pickup hold for every seat. The one tool that can say whether a board showing
// the Pickup pill is the WIRE's fault or the app's.
static void print_holdcheck(const char *hex) {
    unsigned char wire[ENV_CAP];
    int n = 0;
    for (const char *p = hex; p[0] && p[1] && n < (int)sizeof(wire); p += 2) {
        unsigned v = 0;
        if (sscanf(p, "%2x", &v) != 1) break;
        wire[n++] = (unsigned char)v;
    }
    MsgEnvelope e;
    const int rc = msg_decode(wire, n, &e);
    printf("holdcheck: %d bytes, decode %d, format %d, sent_at %u\n", n, rc, e.format, e.sent_at);
    if (rc != MSG_EOK) return;
    Game g;
    const int rrc = msg_replay(&e, &g);
    printf("holdcheck: replay %d, defender %d, battles %d, last log %d\n",
           rrc, g.defender, g.num_battles, g.num_logs ? g.logs[g.num_logs - 1].log_type : -1);
    if (rrc != MSG_EOK) return;
    for (int s = 0; s < g.num_players; s++) {
        printf("holdcheck: seat %d hand %d  hold(now=sent_at) %ds  hold(now=sent_at+20) %ds\n",
               s, g.players[s].hand_count,
               msg_pickup_hold_remaining(&g, s, e.sent_at, e.sent_at),
               msg_pickup_hold_remaining(&g, s, e.sent_at, (uint16_t)(e.sent_at + 20)));
    }
    printf("holdcheck: this machine's clock is %u\n", (unsigned)(time(NULL) & 0xffff));
}

// ---------- the chain layer's gates ----------------------------------------
//
// The four decisions the iMessage extension used to make in Swift
// (StaleBranchGate.isAhead, NicknameGate, SeatIdentity). They are here because
// the SECOND chain client should not re-derive them, and what is pinned is the
// two places each one is deliberately not the obvious thing: the gate compares
// round above turn and fails open on a tie, and the seat rules refuse the
// 2-player inference outside a DM.
//
// MUTATION-CHECKED, each applied to c/src/msg_wire.c on its own:
//   is_ahead compares turn above round                        -> 2 failures
//   is_ahead treats a tie as ahead (>= on turn)               -> 1 failure
//   msg_seat_resolve drops the chat_is_dm guard               -> 1 failure
//   msg_seat_resolve takes a cached seat out of range         -> 1 failure
//   msg_seat_cache_disowned calls a missing join a disownment -> 1 failure
//   resolve_in_lobby skips the membership check               -> 1 failure
//
// The nickname verdict's two caps are NOT order-sensitive - both answer
// TOO_LONG - so swapping them is not a mutation this or any test can see, and
// the order in the code is documentation rather than behaviour.
static void test_chain_gates(void) {
    // ---- the stale-branch gate ----
    CHECK(msg_chain_is_ahead(3, 0, 0, 2, 9, 99) == 1, "a finished chain outranks a live one");
    CHECK(msg_chain_is_ahead(2, 2, 0, 2, 1, 99) == 1,
          "ROUND is asked above TURN: a chain a whole bout further on is ahead even "
          "when the bout-closing fold left it with fewer atoms");
    CHECK(msg_chain_is_ahead(2, 1, 99, 2, 2, 0) == 0, "…and not the other way round");
    CHECK(msg_chain_is_ahead(2, 1, 5, 2, 1, 4) == 1, "within a round, atoms decide");
    CHECK(msg_chain_is_ahead(2, 1, 5, 2, 1, 5) == 0,
          "A TIE IS NOT AHEAD - the 1.0(40) report was a legal move refused as stale "
          "because two bubbles carried the same state");

    // ---- the nickname gate ----
    CHECK(msg_nickname_verdict(0, 0) == MSG_NAME_EMPTY, "nothing typed");
    CHECK(msg_nickname_verdict(1, 1) == MSG_NAME_OK, "one letter is a name");
    CHECK(msg_nickname_verdict(MSG_MAX_NAME_CHARS, MSG_MAX_NAME) == MSG_NAME_OK,
          "both caps are inclusive");
    CHECK(msg_nickname_verdict(MSG_MAX_NAME_CHARS + 1, 20) == MSG_NAME_TOO_LONG,
          "a long run of one-byte characters no badge can show");
    CHECK(msg_nickname_verdict(8, MSG_MAX_NAME + 1) == MSG_NAME_TOO_LONG,
          "…and a short name the SEAL would refuse: 'Владимир' is 8 letters and 16 bytes, "
          "which is the whole reason there are two caps");

    // ---- names in a roster ----
    const char *abc[] = { "Alex", "Bob", "Cindy" };
    MsgJoin j[MSG_MAX_JOINS];
    joins_of(j, abc, 3);
    CHECK(msg_name_taken(j, 3, "Bob", 3) == 1, "a seated name is taken");
    CHECK(msg_name_taken(j, 3, "Bo", 2) == 0, "a PREFIX is not the name");
    CHECK(msg_name_taken(j, 3, "Bobb", 4) == 0, "…and neither is a longer one");
    CHECK(msg_name_taken(j, 3, "Dana", 4) == 0, "a free name is free");
    CHECK(msg_name_taken(0, 3, "Bob", 3) == 0, "no roster, nothing taken");

    // ---- which seat am I ----
    CHECK(msg_seat_resolve(2, 0, 4, 0, 0) == 2, "the cache is the first answer");
    CHECK(msg_seat_resolve(9, 0, 4, 1, 0) == -1,
          "a cached seat out of range is treated as absent, never as a seat");
    CHECK(msg_seat_resolve(-1, 1, 5, 3, 0) == 3, "I sent it, so I am its last actor");
    CHECK(msg_seat_resolve(-1, 0, 2, 0, 1) == 1, "a DM 2p bubble I did not send is the other seat");
    CHECK(msg_seat_resolve(-1, 0, 2, 0, 0) == -1,
          "…and in a GROUP chat it is ambiguous: a third member is one tap from being "
          "seated on somebody's face-up hand");
    CHECK(msg_seat_resolve(-1, 0, 3, 0, 1) == -1, "3 players and no signal is ambiguous");

    // ---- the claim name ----
    CHECK(msg_seat_claimed_by_name(j, 3, "Cindy", 5) == 2, "my claim name finds my seat");
    CHECK(msg_seat_claimed_by_name(j, 3, "Dana", 4) == -1, "a name nobody holds finds nothing");
    CHECK(msg_seat_claimed_by_name(j, 3, "", 0) == -1, "no recorded name, no scan");

    CHECK(msg_seat_cache_disowned(j, 3, 1, "Bob", 3) == 0, "the roster agrees with the cache");
    CHECK(msg_seat_cache_disowned(j, 3, 1, "Alex", 4) == 1,
          "seat 1 is somebody else now - a claim race this device lost");
    CHECK(msg_seat_cache_disowned(j, 3, 7, "Bob", 3) == 0,
          "a seat this bubble names nobody at is NOT a disownment - stay permissive");
    CHECK(msg_seat_cache_disowned(j, 3, 1, "", 0) == 0, "no recorded name, no disownment");

    // ---- the lobby gate ----
    CHECK(msg_seat_resolve_in_lobby(j, 3, 1, 0, 3, 0, 0, "Bob", 3) == 1,
          "cached, listed, and named as me");
    CHECK(msg_seat_resolve_in_lobby(j, 3, 1, 0, 3, 0, 0, "Cindy", 5) == 2,
          "the NAME recovers the seat when the numeric cache lost its race");
    CHECK(msg_seat_resolve_in_lobby(j, 2, 2, 0, 3, 0, 0, 0, 0) == -1,
          "resolved to a seat this OLDER lobby bubble does not list yet - not mine here, "
          "which is what keeps Start off a bubble that predates my join");
    CHECK(msg_seat_resolve_in_lobby(j, 3, 1, 0, 3, 0, 0, "Alex", 4) == 0,
          "a disowned cache falls through to the name, which finds seat 0");
    CHECK(msg_seat_resolve_in_lobby(j, 3, -1, 0, 3, 0, 0, 0, 0) == -1, "no signal, not mine");
}

// ---------- the turn controller as a transition function --------------------
//
// The decisions MessageTurnController used to make in Swift across its own
// awaits: what may be staged, what an arriving chain does to it, what a send
// means, and what a read publishes while a settlement is withheld.
//
// What is pinned is the places each rule is deliberately not the obvious thing.
// The staging gate counts the HUMAN menu, because the raw one always offers a
// `good` a human may not make. The arrival verdict asks about the retraction
// BEFORE the staged moves, because a retraction has already emptied them. The
// send picker prefers OUR OWN sealed chain over the host's payload whenever
// anything is staged, which is what makes a stale host payload a rebase rather
// than a refusal. The held settlement publishes an empty menu as well as an
// older board, and it is the menu half that stops a player acting on a deal
// they have not been shown.
//
// MUTATION-CHECKED, each applied to c/src/msg_wire.c on its own, with the
// unmutated baseline run first and reporting 0:
//   can_act ignores SUPERSEDED                              -> 1 failure
//   can_stage drops the genesis clause                      -> 1 failure
//   cancel returns CLEAR whenever anything is staged        -> 1 failure
//   cancel undoes unconditionally (drops the STAGED guard)   -> 1 failure
//   cancel ignores the send window                           -> 1 failure
//   cancel ignores the retraction in flight                  -> 1 failure
//   admit asks SUPERSEDED before RETRACTING                 -> 1 failure
//   admit drops the pickup hold                             -> 1 failure
//   arrival asks RETRACTING after the staged test           -> 1 failure
//   arrival drops the same-chain SKIP                       -> 1 failure
//   arrival retracts during the send window                 -> 1 failure
//   adopt_duplicate ignores the staged moves                -> 1 failure
//   sent_source prefers the host's bytes when staged        -> 4 failures
//   send_verdict calls a stale host payload FOREIGN         -> 6 failures
//   send_verdict adopts another game's chain                -> 7 failures
//   hold_state returns cut-1 with no clamp at 0             -> 1 failure
//   publish never publishes the empty menu                  -> 1 failure
//   publish never shows the held view                       -> 1 failure
//   publish raises the veil without view_would_change       -> 1 failure
//   publish always animates from the base boundary          -> 1 failure
static void test_turn_controller(void) {
    // ---- what may be staged ----
    const int live = MSG_TURN_READY;
    CHECK(msg_turn_can_send(live | MSG_TURN_STAGED) == 1, "a staged bubble is sendable");
    CHECK(msg_turn_can_send(live) == 0, "nothing staged, nothing to send");
    CHECK(msg_turn_can_send(live | MSG_TURN_STAGED | MSG_TURN_SENDING) == 0,
          "the send window has already claimed those bytes - no second send, no undo");

    CHECK(msg_turn_can_act(live, 3) == 1, "three human moves is a turn");
    CHECK(msg_turn_can_act(live, 0) == 0, "an empty HUMAN menu is not a turn, however "
          "many entries the raw one has");
    CHECK(msg_turn_can_act(live | MSG_TURN_SUPERSEDED, 3) == 0,
          "a board branching off an old bubble is read-only");

    CHECK(msg_turn_can_stage(live | MSG_TURN_STAGED, 0) == 1, "staged, so stageable");
    CHECK(msg_turn_can_stage(live | MSG_TURN_GENESIS, 0) == 1,
          "a genesis with no move of my own still has the DEAL to send on - without "
          "this the creator who does not open is stuck on a board with no send");
    CHECK(msg_turn_can_stage(live | MSG_TURN_GENESIS, 2) == 0,
          "…but a genesis I can play on waits for me to play");
    CHECK(msg_turn_can_stage(live, 2) == 0, "a continuation with nothing staged stages nothing");
    CHECK(msg_turn_can_stage(live | MSG_TURN_STAGED | MSG_TURN_SUPERSEDED, 0) == 0,
          "superseded stands the whole send path down");

    // ---- the staged bubble, X-ed out of the input field ----
    CHECK(msg_turn_cancel(live | MSG_TURN_STAGED, 1) == MSG_TURN_CANCEL_CLEAR,
          "one move staged: take it back, and put NO bubble back - the human "
          "deleted the one there was");
    CHECK(msg_turn_cancel(live | MSG_TURN_STAGED, 3) == MSG_TURN_CANCEL_RESTAGE,
          "a throw-in stacked on an attack: undo one, and the shorter chain "
          "still needs a bubble");
    CHECK(msg_turn_cancel(live, 0) == MSG_TURN_CANCEL_NOOP,
          "stage, undo, THEN X the base bubble: nothing of mine is staged, so "
          "the cancel must not reach into the game and undo a second move");
    CHECK(msg_turn_cancel(live | MSG_TURN_STAGED | MSG_TURN_SENDING, 1)
              == MSG_TURN_CANCEL_NOOP,
          "the send window has the bytes - the same reason can_send refuses");
    CHECK(msg_turn_cancel(live | MSG_TURN_STAGED | MSG_TURN_RETRACTING, 2)
              == MSG_TURN_CANCEL_NOOP,
          "a retraction IS an undo of everything staged, already in flight");
    CHECK(msg_turn_cancel(live | MSG_TURN_GENESIS, 0) == MSG_TURN_CANCEL_NOOP,
          "a genesis deal is stageable with nothing pending, and X-ing it is "
          "still not an undo of a move nobody made");

    // ---- the door every gesture comes through ----
    CHECK(msg_turn_admit(live, MOVE_ATTACK, 0) == MSG_TURN_ADMIT_OK, "an ordinary attack");
    CHECK(msg_turn_admit(live | MSG_TURN_RETRACTING | MSG_TURN_SUPERSEDED, MOVE_ATTACK, 0)
          == MSG_TURN_ADMIT_RETRACTING,
          "the retraction is asked FIRST because it is the SILENT refusal - a toast for "
          "a tap in a one-flight window is noise, and the board's veil comes down on the "
          "arrival either way");
    CHECK(msg_turn_admit(live | MSG_TURN_SUPERSEDED, MOVE_ATTACK, 0)
          == MSG_TURN_ADMIT_SUPERSEDED, "a stale branch may not be played on");
    CHECK(msg_turn_admit(live, MOVE_PICKUP, 7) == MSG_TURN_ADMIT_HELD_PICKUP,
          "the pickup hold is enforced, not merely displayed");
    CHECK(msg_turn_admit(live, MOVE_PICKUP, 0) == MSG_TURN_ADMIT_OK, "…and it lapses");
    CHECK(msg_turn_admit(live, MOVE_COVER, 7) == MSG_TURN_ADMIT_OK,
          "the hold is about picking up, not about playing");

    // ---- a chain that arrived ----
    const int watching = live | MSG_TURN_BOARD_WATCHING;
    CHECK(msg_turn_arrival(watching | MSG_TURN_STAGED, 1) == MSG_TURN_ARRIVE_SKIP,
          "a re-delivery of the chain I am ALREADY on keeps my staged moves - they were "
          "composed against exactly these bytes");
    CHECK(msg_turn_arrival(MSG_TURN_STAGED, 1) == MSG_TURN_ARRIVE_ADOPT,
          "…but only once a base has been established; before that there is nothing to "
          "compare against");
    CHECK(msg_turn_arrival(watching | MSG_TURN_RETRACTING, 0) == MSG_TURN_ARRIVE_LATCH,
          "mid-retraction only the latch moves, and it moves although the retraction has "
          "already emptied the staged list - newest wins");
    CHECK(msg_turn_arrival(watching | MSG_TURN_STAGED, 0) == MSG_TURN_ARRIVE_RETRACT,
          "a chain landing over a staged move is visibly retracted first");
    CHECK(msg_turn_arrival(live | MSG_TURN_STAGED, 0) == MSG_TURN_ARRIVE_ADOPT,
          "no board mounted, no theatre");
    CHECK(msg_turn_arrival(watching, 0) == MSG_TURN_ARRIVE_ADOPT, "nothing staged to retract");
    CHECK(msg_turn_arrival(watching | MSG_TURN_STAGED | MSG_TURN_SENDING, 0)
          == MSG_TURN_ARRIVE_ADOPT,
          "a move the human has already SENT is not a staged move - retracting it offers "
          "to take back a bubble the thread already has");

    CHECK(msg_turn_adopt_duplicate(live, 1) == 1, "the chain already resident");
    CHECK(msg_turn_adopt_duplicate(live | MSG_TURN_STAGED, 1) == 0,
          "with moves staged the resident game is base+pending, so a direct adopt rebuilds");
    CHECK(msg_turn_adopt_duplicate(0, 1) == 0, "nothing adopted yet is not a duplicate");

    // ---- which bytes went out ----
    CHECK(msg_turn_sent_source(1, 1, 1) == MSG_TURN_BYTES_SEALED,
          "staged: OUR OWN sealed chain is the bubble, whatever the host handed over");
    CHECK(msg_turn_sent_source(1, 0, 1) == MSG_TURN_BYTES_SEALED,
          "…including when the host's payload never arrived");
    CHECK(msg_turn_sent_source(1, 1, 0) == MSG_TURN_BYTES_HOST,
          "staged with nothing sealed - only the host can say");
    CHECK(msg_turn_sent_source(1, 0, 0) == MSG_TURN_BYTES_NONE, "staged and blind");
    CHECK(msg_turn_sent_source(0, 1, 1) == MSG_TURN_BYTES_HOST,
          "UNSTAGED: the sealed chain makes no claim on a bubble I did not just stage");
    CHECK(msg_turn_sent_source(0, 0, 1) == MSG_TURN_BYTES_NONE, "…so a lone seal is not a send");
    CHECK(msg_turn_sent_source(0, 1, 0) == MSG_TURN_BYTES_HOST, "unstaged, host only");
    CHECK(msg_turn_sent_source(0, 0, 0) == MSG_TURN_BYTES_NONE, "no bytes at all");

    // ---- what a send does ----
    // 1.0(26): the signal arrives with NO payload while a move is staged. It
    // must rebase onto our own sealed chain, not empty `pending` and leave the
    // base where it stood - that un-plays the staged move, and the board then
    // reads its own table clearing as a bout end and animates the bubble BEFORE
    // the one just sent.
    CHECK(msg_turn_send_verdict(1, 0, 1, 0, -1, -1, -1, -1) == MSG_TURN_SEND_DECODE,
          "staged, bytesless: our sealed chain is the bubble and it is ours to decode");
    CHECK(msg_turn_send_verdict(1, 0, 1, 0, 1, 1, -1, -1) == MSG_TURN_SEND_REBASE, "…and to rebase onto");
    // 1.0(36): the signal arrives with a STALE payload. It took the refusal
    // below instead, which stranded the withheld settlement and left the staged
    // move to be red-retracted by the next arrival ("Somehow this caused an UNDO
    // animation of the previous pickup").
    CHECK(msg_turn_send_verdict(1, 1, 1, 0, -1, -1, -1, -1) == MSG_TURN_SEND_DECODE,
          "staged with a STALE host payload is not foreign - ours substitutes");
    CHECK(msg_turn_send_verdict(1, 1, 1, 0, 1, 1, -1, -1) == MSG_TURN_SEND_REBASE, "…and rebases");
    CHECK(msg_turn_send_verdict(0, 1, 1, 0, -1, -1, -1, -1) == MSG_TURN_SEND_FOREIGN,
          "UNSTAGED with a payload that is not the chain we sealed - a reload's chain, "
          "left alone");
    CHECK(msg_turn_send_verdict(1, 0, 0, 0, -1, -1, -1, -1) == MSG_TURN_SEND_BLIND,
          "staged with no chain at all: KEEP the moves - dropping them without a base to "
          "replace them walks the board back by the move just watched");
    CHECK(msg_turn_send_verdict(0, 0, 0, 0, -1, -1, -1, -1) == MSG_TURN_SEND_NOOP,
          "nothing staged and no bytes was not a send of ours");
    CHECK(msg_turn_send_verdict(0, 0, 1, 0, -1, -1, -1, -1) == MSG_TURN_SEND_NOOP,
          "a sealed chain nobody sent is still not a send");
    CHECK(msg_turn_send_verdict(1, 1, 0, 0, 0, 1, -1, -1) == MSG_TURN_SEND_UNREADABLE,
          "bytes that will not decode leave the board on its staged move");
    CHECK(msg_turn_send_verdict(0, 1, 0, 0, 1, 1, -1, -1) == MSG_TURN_SEND_REBASE,
          "unstaged with a host payload and nothing sealed - no opinion, so adopt it");
    // 1.0(37): …and THAT is the hole another game's draft comes through. A
    // thread holds many games, a staged bubble is a draft that survives the
    // human tapping away to one of them, and the send signal for it reaches
    // whatever board is on screen. Same facts as the line above - unstaged,
    // host bytes, nothing sealed - and only the game id separates them.
    CHECK(msg_turn_send_verdict(0, 1, 0, 0, 1, 0, -1, -1) == MSG_TURN_SEND_OTHERGAME,
          "a chain for a DIFFERENT GAME is never this board's to adopt - a rebase would "
          "decode it MASKED FOR THIS BOARD'S SEAT, which over there is somebody else");
    CHECK(msg_turn_send_verdict(1, 1, 1, 1, 1, 0, -1, -1) == MSG_TURN_SEND_OTHERGAME,
          "…and staging our own move on this board does not make another game's chain ours");
    CHECK(msg_turn_send_verdict(0, 1, 0, 0, 0, 0, -1, -1) == MSG_TURN_SEND_UNREADABLE,
          "bytes that will not decode are unreadable whoever they belong to - the game "
          "test sits UNDER the decode test, because there is no game id to compare yet");

    // The whole input space, for the structural claims. FOREIGN unreachable
    // while anything is staged is the 1.0(36) fix stated as an invariant: with
    // a move staged, the bytes that went out are always our own.
    for (int staged = 0; staged < 2; staged++)
    for (int host = 0; host < 2; host++)
    for (int sealed = 0; sealed < 2; sealed++)
    for (int same = 0; same < 2; same++) {
        const int first = msg_turn_send_verdict(staged, host, sealed, same, -1, -1, -1, -1);
        const int src = msg_turn_sent_source(staged, host, sealed);
        CHECK(!(staged && first == MSG_TURN_SEND_FOREIGN),
              "FOREIGN with moves staged (%d/%d/%d/%d) - the send path can only refuse a "
              "chain this device did not seal", staged, host, sealed, same);
        CHECK((first == MSG_TURN_SEND_DECODE) == (src != MSG_TURN_BYTES_NONE
                                                  && first != MSG_TURN_SEND_FOREIGN),
              "a decode is owed exactly when there are bytes we vouch for (%d/%d/%d/%d)",
              staged, host, sealed, same);
        CHECK((first == MSG_TURN_SEND_NOOP) == (!staged && src == MSG_TURN_BYTES_NONE),
              "NOOP is nothing staged and no bytes, and only that (%d/%d/%d/%d)",
              staged, host, sealed, same);
        CHECK(first != MSG_TURN_SEND_OTHERGAME,
              "the game is never asked about before the decode (%d/%d/%d/%d)",
              staged, host, sealed, same);
        if (first != MSG_TURN_SEND_DECODE) continue;
        CHECK(msg_turn_send_verdict(staged, host, sealed, same, 1, 1, -1, -1) == MSG_TURN_SEND_REBASE,
              "a decode that reads rebases (%d/%d/%d/%d)", staged, host, sealed, same);
        CHECK(msg_turn_send_verdict(staged, host, sealed, same, 0, 1, -1, -1) == MSG_TURN_SEND_UNREADABLE,
              "a decode that fails keeps the board (%d/%d/%d/%d)", staged, host, sealed, same);
        CHECK(msg_turn_send_verdict(staged, host, sealed, same, 1, 0, -1, -1) == MSG_TURN_SEND_OTHERGAME,
              "…and a decode that reads ANOTHER GAME is refused from every set of facts "
              "that owed a decode at all (%d/%d/%d/%d)", staged, host, sealed, same);
        // An arrival that raced the send (notes 4/5). The ordinary send was
        // built on the board's own chain and rebases; bytes the board already
        // holds or has moved past, and a sibling Rule P ranks below the
        // arrival, leave the board where the arrival put it.
        CHECK(msg_turn_send_verdict(staged, host, sealed, same, 1, 1, MSG_FATE_STANDS, -1)
              == MSG_TURN_SEND_REBASE, "the ordinary send rebases (%d/%d/%d/%d)",
              staged, host, sealed, same);
        CHECK(msg_turn_send_verdict(staged, host, sealed, same, 1, 1, MSG_FATE_LANDED, -1)
              == MSG_TURN_SEND_OVERTAKEN, "the board is already past the sent bytes (%d/%d/%d/%d)",
              staged, host, sealed, same);
        CHECK(msg_turn_send_verdict(staged, host, sealed, same, 1, 1, MSG_FATE_SUPERSEDED, 0)
              == MSG_TURN_SEND_OVERTAKEN, "a sibling the arrival beats is not adopted (%d/%d/%d/%d)",
              staged, host, sealed, same);
        CHECK(msg_turn_send_verdict(staged, host, sealed, same, 1, 1, MSG_FATE_SUPERSEDED, 1)
              == MSG_TURN_SEND_REBASE, "a sibling that beats the arrival is where the thread "
              "goes (%d/%d/%d/%d)", staged, host, sealed, same);
        CHECK(msg_turn_send_verdict(staged, host, sealed, same, 1, 1, MSG_FATE_SUPERSEDED, -1)
              == MSG_TURN_SEND_REBASE, "Rule P not asked is no refusal (%d/%d/%d/%d)",
              staged, host, sealed, same);
        CHECK(msg_turn_send_verdict(staged, host, sealed, same, 1, 0, MSG_FATE_LANDED, 0)
              == MSG_TURN_SEND_OTHERGAME, "another game is refused as such, first (%d/%d/%d/%d)",
              staged, host, sealed, same);
        CHECK(msg_turn_send_verdict(staged, host, sealed, same, 0, 1, MSG_FATE_LANDED, 0)
              == MSG_TURN_SEND_UNREADABLE, "unreadable bytes have no fate (%d/%d/%d/%d)",
              staged, host, sealed, same);
    }
    CHECK(msg_turn_send_verdict(0, 0, 0, 0, -1, -1, MSG_FATE_LANDED, 0) == MSG_TURN_SEND_NOOP,
          "no bytes, no send, whatever an arrival did");

    // ---- the input field, after an arrival was adopted ----
    CHECK(msg_turn_field_after_arrival(watching, -1) == MSG_TURN_FIELD_KEEP,
          "nothing of mine in the field: nothing to overwrite");
    CHECK(msg_turn_field_after_arrival(watching | MSG_TURN_STAGED, MSG_FATE_SUPERSEDED)
          == MSG_TURN_FIELD_NOTHING,
          "a staged move the arrival does not carry is overwritten with the NOTHING bubble - the "
          "field may never hold a move the board is not showing");
    CHECK(msg_turn_field_after_arrival(watching, MSG_FATE_SUPERSEDED) == MSG_TURN_FIELD_NOTHING,
          "…and so is a stale NOTHING bubble an Undo left there");
    CHECK(msg_turn_field_after_arrival(live | MSG_TURN_STAGED, MSG_FATE_SUPERSEDED)
          == MSG_TURN_FIELD_NOTHING, "with or without a board mounted");
    CHECK(msg_turn_field_after_arrival(watching | MSG_TURN_STAGED, MSG_FATE_STANDS)
          == MSG_TURN_FIELD_NOTHING,
          "an arrival that adds nothing still moved the base, and the staged bubble still names "
          "the old parent - the adopt dropped the move, so the field must too");
    CHECK(msg_turn_field_after_arrival(watching | MSG_TURN_STAGED, MSG_FATE_LANDED)
          == MSG_TURN_FIELD_KEEP, "the arrival carries my bubble: it went out, the field is empty");
    CHECK(msg_turn_field_after_arrival(watching | MSG_TURN_STAGED | MSG_TURN_SENDING,
                                       MSG_FATE_SUPERSEDED) == MSG_TURN_FIELD_KEEP,
          "Send was pressed: the bubble is on its way, and the send verdict owns the rest");
    CHECK(msg_turn_field_after_arrival(MSG_TURN_GENESIS | MSG_TURN_READY, MSG_FATE_SUPERSEDED)
          == MSG_TURN_FIELD_KEEP, "a genesis has no NOTHING bubble to seal");

    // ---- what is withheld ----
    CHECK(msg_turn_hold_state(4, 2) == 1,
          "the ACTING step's committed board: the cover on the table, the table taken");
    CHECK(msg_turn_hold_state(3, 0) == 0,
          "a GOOD emits no step of its own, so the cut lands on the transition step - which "
          "carries the PRE-discard board, and that is the state being asked for");
    CHECK(msg_turn_hold_state(3, -1) == -1, "no cut, nothing to hold");
    CHECK(msg_turn_hold_state(3, 3) == -1, "a cut past the end holds nothing");
    CHECK(msg_turn_hold_state(0, 0) == -1, "no events, nothing to hold");

    // ---- what a read publishes ----
    MsgTurnRead r;
    MsgTurnPublished p;
    memset(&r, 0, sizeof r);

    r.state = live | MSG_TURN_STAGED | MSG_TURN_HELD;
    r.base_atoms_before = 45; r.staged_atoms_before = 51;
    r.n_open_replay = 0; r.view_would_change = 1;
    msg_turn_publish(&r, &p);
    CHECK(p.show_held_view == 1, "a withheld settlement shows the board BEFORE it");
    CHECK(p.empty_menu == 1,
          "…and publishes an EMPTY menu, which is the half that stops the same player "
          "picking an attack out of a hand they have not been shown yet");
    CHECK(p.anim_atoms_before == 51,
          "with moves staged the animation starts at the KERNEL's mark, not the adopted "
          "chain's atom count - which can land past the end of the re-derived stream");
    CHECK(p.raise_veil == 0, "no open replay, no veil");

    r.state = live;
    msg_turn_publish(&r, &p);
    CHECK(p.show_held_view == 0 && p.empty_menu == 0, "with nothing held the board is the board");
    CHECK(p.anim_atoms_before == 45, "and the animation starts where the bubble says");

    r.state = live | MSG_TURN_STAGED;
    r.n_open_replay = 3;
    msg_turn_publish(&r, &p);
    CHECK(p.raise_veil == 1, "cards to animate over a board about to change: veil them");
    r.view_would_change = 0;
    msg_turn_publish(&r, &p);
    CHECK(p.raise_veil == 0,
          "a veil nothing will take down must never go up - the board's view-change "
          "handler is its only consumer, and an unchanged board fires none");
}

// ---------- where the replay of an arriving chain starts ------------------
//
// msg_open_boundary, the kernel's answer to "which atoms of this arriving
// chain has the board already shown". Each fixture is a pair of REAL bubbles,
// sealed the way a phone seals them (msg_seal with the log mark of the bubble
// it continues), and each case pins the boundary against the atoms themselves.
//
// The bug this exists for: the board used the previous chain's TURN as a floor.
// A pending good is an atom only until something follows it, so after pending
// goods that floor overshoots the arriving chain and nothing animates - every
// bout-closing good and every move after a pending good, at 3+ seats.
// `old_floor` below is that rule, kept so each case can say the old answer
// would have animated nothing.

typedef struct {
    unsigned char w[ENV_CAP];
    int n;
    MsgEnvelope e;   // borrows w: an OBubble is never copied
    int logs;        // the log mark adopting this bubble leaves
} OBubble;

static int ob_seal(OBubble *b, const Game *g, const uint8_t *seed, int np,
                   uint64_t game_id, int base_logs) {
    static unsigned char body[2048];
    static Game scratch;
    MsgEnvelope e;
    env_init(&e, seed, np);
    e.game_id = game_id;
    const int over = game_done(g) >= 0 || g->status == GAME_STATUS_GAME_OVER;
    e.phase = over ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE;
    if (msg_seal(&e, g, base_logs, body, sizeof(body), &scratch) != MSG_EOK) return 0;
    b->n = msg_encode(&e, b->w, sizeof(b->w));
    if (b->n <= 0) return 0;
    if (msg_decode(b->w, b->n, &b->e) != MSG_EOK) return 0;
    b->logs = g->num_logs;
    return 1;
}

static int ob_boundary(const OBubble *shown, const OBubble *arriving) {
    static unsigned char scratch[MSG_OPEN_SCRATCH];
    return msg_open_boundary(shown ? &shown->e : NULL, &arriving->e, scratch, sizeof(scratch));
}

// The rule this replaces: max(claim, the previous chain's turn).
static int old_floor(const OBubble *shown, const OBubble *arriving) {
    const int claim = msg_atoms_before_claim(&arriving->e);
    return shown->e.turn > claim ? shown->e.turn : claim;
}

// The kind of the arriving chain's action atom at `at` (-1 past the end).
static int ob_atom_kind(const OBubble *b, int at) {
    static DAtoms d;
    if (datoms_of(b->e.actions, b->e.actions_len, &d) < 0) return -2;
    return at >= 0 && at < d.n ? d.a[at].kind : -1;
}

static void og_start(Game *g, const uint8_t *seed, int np) {
    game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
    memset(g, 0, sizeof(*g));
    g->num_players = (int8_t)np;
    for (int i = 0; i < np; i++) {
        g->players[i].status = PLAYER_STATUS_READY;
        g->players[i].strategy_key = 0;
    }
    start_game(g);
}

static int og_live(const Game *g) {
    return game_done(g) < 0 && g->status == GAME_STATUS_PLAYING;
}

// Find a seat holding a legal move of `type` (single-card when `single`), the
// scan starting at a random seat. Returns the seat, or -1.
static int og_find(const Game *g, int type, int single, LegalMove *out) {
    static LegalMoves ml;
    const int np = g->num_players;
    const int start = (int)(rnd() % (uint32_t)np);
    for (int t = 0; t < np; t++) {
        const int s = (start + t) % np;
        if (g->players[s].status != PLAYER_STATUS_IN) continue;
        calculate_legal_moves(g, s, &ml);
        for (int i = 0; i < ml.n; i++) {
            if (ml.moves[i].type != type) continue;
            if (single && ml.moves[i].n_cards != 1) continue;
            *out = ml.moves[i];
            return s;
        }
    }
    return -1;
}

// One random legal move by any seat that may act. 0 when nobody can.
static int og_random_step(Game *g) {
    static LegalMoves ml;
    const int np = g->num_players;
    const int start = (int)(rnd() % (uint32_t)np);
    for (int t = 0; t < np; t++) {
        const int s = (start + t) % np;
        if (g->players[s].status != PLAYER_STATUS_IN) continue;
        calculate_legal_moves(g, s, &ml);
        int acts = 0;
        for (int i = 0; i < ml.n; i++) acts += ml.moves[i].type != MOVE_WAIT;
        if (!acts) continue;
        int k = (int)(rnd() % (uint32_t)acts);
        for (int i = 0; i < ml.n; i++) {
            if (ml.moves[i].type == MOVE_WAIT) continue;
            if (k-- == 0) return legal_move_apply(g, s, &ml.moves[i]) ? 1 : 0;
        }
    }
    return 0;
}

static int og_last_log(const Game *g) {
    return g->num_logs > 0 ? g->logs[g->num_logs - 1].log_type : -1;
}

static const int OB_SEATS[] = { 2, 3, 4 };

// A. THE BOUT-CLOSING GOOD OVER PENDING GOODS. The attackers say good one
// bubble at a time; the shown chain is the one with every good but the last
// pending, and the arriving one closes the bout (its goods fold into one
// round_end atom). At 2 seats the one attacker's good closes at once, so there
// is no pending good: the case is the plain covered table, and the old floor
// was right there - which is why every 2p test passed.
static void ob_closing_good(int *posed) {
    static Game g, s0;
    static OBubble bub[MAX_PLAYERS + 1];
    for (int pi = 0; pi < 3; pi++) {
        const int np = OB_SEATS[pi];
        int found = 0;
        for (uint32_t gi = 0; gi < 400 && found < 6; gi++) {
            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, 7100u + gi * 37u + (uint32_t)np);
            g_rng = 5300u + gi * 11u + (uint32_t)np;
            og_start(&g, seed, np);
            const uint64_t gid = 0xB0A0ULL + gi;
            for (int step = 0; step < 120 && og_live(&g) && found < 6; step++) {
                // A table with something on it, every attack covered, no good
                // pending yet: the start of a good run.
                if (g.num_battles > 0 && uncovered_count(&g) == 0 && og_last_log(&g) != LOG_GOOD) {
                    game_clone(&s0, &g);
                    if (!ob_seal(&bub[0], &s0, seed, np, gid, MSG_NO_BASE)) break;
                    int k = 0, closed = 0;
                    while (k < np && og_live(&g)) {
                        LegalMove m;
                        const int seat = og_find(&g, MOVE_GOOD, 0, &m);
                        if (seat < 0 || !legal_move_apply(&g, seat, &m)) break;
                        k++;
                        if (!ob_seal(&bub[k], &g, seed, np, gid, bub[k - 1].logs)) break;
                        if (g.num_battles == 0) { closed = 1; break; }
                    }
                    const int pending = k - 1;   // goods on the shown chain
                    if (closed && (np == 2 ? pending == 0 : pending >= 1)) {
                        const OBubble *shown = &bub[k - 1], *arr = &bub[k];
                        const int b = ob_boundary(shown, arr);
                        const int claim = msg_atoms_before_claim(&arr->e);
                        CHECK(b == claim,
                              "closing good %dp (%d pending): boundary %d, want the sender's %d",
                              np, pending, b, claim);
                        CHECK(b < arr->e.turn,
                              "closing good %dp (%d pending): boundary %d leaves nothing of %d atoms to animate",
                              np, pending, b, arr->e.turn);
                        CHECK(ob_atom_kind(arr, b) == REPLAY_ATOM_ROUND_END,
                              "closing good %dp: the first animated atom is %d, not the round end",
                              np, ob_atom_kind(arr, b));
                        if (np > 2)
                            CHECK(old_floor(shown, arr) >= arr->e.turn,
                                  "closing good %dp: the old floor (%d) animated this too, so the fixture "
                                  "does not pose the bug", np, old_floor(shown, arr));
                        found++;
                        posed[pi]++;
                    }
                    game_clone(&g, &s0);
                    // Move past this table so the next look is a new one.
                    if (!og_random_step(&g)) break;
                    continue;
                }
                if (!og_random_step(&g)) break;
            }
        }
        CHECK(found > 0, "closing good %dp: no fixture built", np);
    }
}

// B. A MOVE AFTER A PENDING GOOD: a throw-in, a cover, a transfer or a pickup
// lands while an attacker's good is pending. The good stops being an atom, so
// the child is no longer than the chain the board showed.
static void ob_after_good(int posed[3][4]) {
    static Game g, s0, s1;
    static OBubble shown, arr, earlier;
    static const int kinds[4] = { MOVE_ATTACK, MOVE_COVER, MOVE_PASS, MOVE_PICKUP };
    static const int atom_of[4] = { REPLAY_ATOM_ATTACK, REPLAY_ATOM_COVER,
                                    REPLAY_ATOM_PASS, REPLAY_ATOM_PICKUP };
    for (int pi = 1; pi < 3; pi++) {   // a pending good needs 3+ seats
        const int np = OB_SEATS[pi];
        for (uint32_t gi = 0; gi < 120; gi++) {
            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, 9100u + gi * 53u + (uint32_t)np);
            g_rng = 6100u + gi * 7u + (uint32_t)np;
            og_start(&g, seed, np);
            const uint64_t gid = 0xC0A0ULL + gi;
            // The bubble before the good, then the good on its own bubble.
            int prev_logs = MSG_NO_BASE;
            for (int step = 0; step < 160 && og_live(&g); step++) {
                game_clone(&s0, &g);
                if (!og_random_step(&g)) break;
                if (!(og_last_log(&g) == LOG_GOOD && g.num_battles > 0)) continue;
                if (!ob_seal(&earlier, &s0, seed, np, gid, prev_logs)) break;
                if (!ob_seal(&shown, &g, seed, np, gid, earlier.logs)) break;
                for (int ki = 0; ki < 4; ki++) {
                    game_clone(&s1, &g);
                    LegalMove m;
                    const int seat = og_find(&s1, kinds[ki], 0, &m);
                    if (seat < 0 || !legal_move_apply(&s1, seat, &m)) continue;
                    if (!ob_seal(&arr, &s1, seed, np, gid, shown.logs)) continue;
                    const int b = ob_boundary(&shown, &arr);
                    const int claim = msg_atoms_before_claim(&arr.e);
                    CHECK(b == claim, "after good %dp kind %d: boundary %d, want the sender's %d",
                          np, kinds[ki], b, claim);
                    CHECK(b < arr.e.turn, "after good %dp kind %d: boundary %d animates nothing of %d",
                          np, kinds[ki], b, arr.e.turn);
                    CHECK(ob_atom_kind(&arr, b) == atom_of[ki],
                          "after good %dp kind %d: first animated atom is %d", np, kinds[ki],
                          ob_atom_kind(&arr, b));
                    CHECK(old_floor(&shown, &arr) >= arr.e.turn,
                          "after good %dp kind %d: the old floor animated this too", np, kinds[ki]);

                    // The same pair the other way round is an OLDER chain
                    // arriving on a board that is ahead of it: it is all
                    // shown, so nothing animates. And a re-delivery of the
                    // chain on screen animates nothing either.
                    CHECK(ob_boundary(&arr, &shown) == shown.e.turn,
                          "after good %dp: an older chain opened at %d of %d",
                          np, ob_boundary(&arr, &shown), shown.e.turn);
                    CHECK(ob_boundary(&arr, &arr) == arr.e.turn,
                          "after good %dp: a re-delivery opened at %d of %d",
                          np, ob_boundary(&arr, &arr), arr.e.turn);
                    posed[pi][ki]++;
                }
                // TWO GOODS RACED off one parent: another attacker's good,
                // sealed on the table the shown good was said over. The board
                // showed the first; the second is new and must animate (its
                // role mark), from the shared parent.
                {
                    static LegalMoves ml;
                    const int first = g.logs[g.num_logs - 1].player_idx;
                    for (int s = 0; s < np; s++) {
                        if (s == first || s0.players[s].status != PLAYER_STATUS_IN) continue;
                        calculate_legal_moves(&s0, s, &ml);
                        int gi2 = -1;
                        for (int i = 0; i < ml.n && gi2 < 0; i++)
                            if (ml.moves[i].type == MOVE_GOOD) gi2 = i;
                        if (gi2 < 0) continue;
                        game_clone(&s1, &s0);
                        if (!legal_move_apply(&s1, s, &ml.moves[gi2]) || s1.num_battles == 0) continue;
                        if (!ob_seal(&arr, &s1, seed, np, gid, earlier.logs)) continue;
                        const int b = ob_boundary(&shown, &arr);
                        CHECK(b == msg_atoms_before_claim(&arr.e) && b < arr.e.turn,
                              "raced goods %dp: boundary %d of %d (claim %d)", np, b, arr.e.turn,
                              msg_atoms_before_claim(&arr.e));
                        CHECK(ob_atom_kind(&arr, b) == REPLAY_ATOM_GOOD,
                              "raced goods %dp: the raced good does not animate", np);
                        posed[0][pi]++;
                        break;
                    }
                }
                prev_logs = shown.logs;
                break;   // one pending good per deal is plenty
            }
        }
    }
    for (int pi = 1; pi < 3; pi++) {
        CHECK(posed[pi][0] > 0, "after good %dp: no throw-in posed", OB_SEATS[pi]);
        CHECK(posed[pi][1] > 0, "after good %dp: no cover posed", OB_SEATS[pi]);
        CHECK(posed[pi][3] > 0, "after good %dp: no pickup posed", OB_SEATS[pi]);
        CHECK(posed[0][pi] > 0, "raced goods %dp: none posed", OB_SEATS[pi]);
    }
}

// C. THE STALE SENDER (ReplayFloorTests on the phone): the defender covers,
// sends, covers again and sends, but the second bubble claims BOTH covers.
// The board already showed the first, so it must not fly again. Also: two
// covers off the same table are SIBLINGS, and a sibling's move is new to a
// board that showed the other one. And the cold open is never clamped.
static void ob_stale_and_siblings(int *posed) {
    static Game g, s0, s1, s2, sx;
    static OBubble before, one, stale, honest, sib;
    for (int pi = 0; pi < 3; pi++) {
        const int np = OB_SEATS[pi];
        int found = 0;
        for (uint32_t gi = 0; gi < 600 && found < 6; gi++) {
            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, 11300u + gi * 29u + (uint32_t)np);
            g_rng = 8800u + gi * 13u + (uint32_t)np;
            og_start(&g, seed, np);
            const uint64_t gid = 0xD0A0ULL + gi;
            for (int step = 0; step < 160 && og_live(&g); step++) {
                if (uncovered_count(&g) >= 2) {
                    LegalMove c1, c2, cx;
                    game_clone(&s0, &g);
                    const int d1 = og_find(&s0, MOVE_COVER, 1, &c1);
                    if (d1 >= 0) {
                        game_clone(&s1, &s0);
                        if (legal_move_apply(&s1, d1, &c1) && uncovered_count(&s1) >= 1) {
                            game_clone(&s2, &s1);
                            const int d2 = og_find(&s2, MOVE_COVER, 1, &c2);
                            if (d2 >= 0 && legal_move_apply(&s2, d2, &c2)
                                && ob_seal(&before, &s0, seed, np, gid, MSG_NO_BASE)
                                && ob_seal(&one, &s1, seed, np, gid, before.logs)
                                && ob_seal(&stale, &s2, seed, np, gid, before.logs)
                                && ob_seal(&honest, &s2, seed, np, gid, one.logs)) {
                                const int sc = msg_atoms_before_claim(&stale.e);
                                CHECK(sc == stale.e.turn - 2,
                                      "stale %dp: the fixture's stale claim is %d of %d", np, sc, stale.e.turn);
                                CHECK(ob_boundary(&one, &stale) == one.e.turn,
                                      "stale %dp: boundary %d re-animates the cover already shown (want %d)",
                                      np, ob_boundary(&one, &stale), one.e.turn);
                                CHECK(ob_boundary(&one, &honest) == msg_atoms_before_claim(&honest.e),
                                      "stale %dp: an honest sender's claim was moved", np);
                                // Cold: no chain on screen, no clamp - the
                                // sender's claim stands, both covers fly.
                                CHECK(ob_boundary(NULL, &stale) == sc,
                                      "stale %dp: a cold open was clamped to %d", np, ob_boundary(NULL, &stale));
                                // A recording cut short can only clamp LESS.
                                static unsigned char tiny[6];
                                const int t = msg_open_boundary(&one.e, &stale.e, tiny, sizeof(tiny));
                                CHECK(t >= sc && t <= one.e.turn,
                                      "stale %dp: a short scratch answered %d outside [%d, %d]",
                                      np, t, sc, one.e.turn);

                                // A SIBLING: a different single cover off the
                                // same table, by whichever seat holds one.
                                static LegalMoves ml;
                                calculate_legal_moves(&s0, d1, &ml);
                                int have_sib = 0;
                                for (int i = 0; i < ml.n && !have_sib; i++) {
                                    if (ml.moves[i].type != MOVE_COVER || ml.moves[i].n_cards != 1) continue;
                                    cx = ml.moves[i];
                                    if (!memcmp(&cx.cards[0], &c1.cards[0], 1)
                                        && !memcmp(&cx.attack_cards[0], &c1.attack_cards[0], 1)) continue;
                                    game_clone(&sx, &s0);
                                    if (legal_move_apply(&sx, d1, &cx)
                                        && ob_seal(&sib, &sx, seed, np, gid, before.logs)) have_sib = 1;
                                }
                                if (have_sib) {
                                    const int sb = ob_boundary(&one, &sib);
                                    CHECK(sb == before.e.turn,
                                          "sibling %dp: boundary %d, want the shared parent's %d",
                                          np, sb, before.e.turn);
                                    CHECK(ob_atom_kind(&sib, sb) == REPLAY_ATOM_COVER,
                                          "sibling %dp: the sibling's cover does not animate", np);
                                    posed[3 + pi]++;
                                }
                                found++;
                                posed[pi]++;
                                break;   // next deal
                            }
                        }
                    }
                }
                if (!og_random_step(&g)) break;
            }
        }
        CHECK(found > 0, "stale %dp: no fixture built", np);
        CHECK(posed[3 + pi] > 0, "sibling %dp: no fixture built", np);
    }
}

// D. THE EDGES: a chain with no body on screen, another game on screen, a
// NOTHING reseal, and a sender that does not say (format 2).
static void ob_edges(void) {
    static Game g, h;
    static OBubble dealt, first, other, nothing, nobase;
    for (int pi = 0; pi < 3; pi++) {
        const int np = OB_SEATS[pi];
        uint8_t seed[MSG_SEED_LEN], seed2[MSG_SEED_LEN];
        seed_fill(seed, 13900u + (uint32_t)np);
        seed_fill(seed2, 14900u + (uint32_t)np);
        g_rng = 4400u + (uint32_t)np;
        og_start(&g, seed, np);
        const uint64_t gid = 0xE0A0ULL + (uint64_t)np;
        // The just-dealt table: the LIVE handoff's shape, no action atoms.
        CHECK(ob_seal(&dealt, &g, seed, np, gid, 0), "edges %dp: dealt seal", np);
        CHECK(dealt.e.turn == 0, "edges %dp: a dealt table sealed %d atoms", np, dealt.e.turn);
        LegalMove m;
        const int a = og_find(&g, MOVE_ATTACK, 0, &m);
        CHECK(a >= 0 && legal_move_apply(&g, a, &m), "edges %dp: no opening attack", np);
        CHECK(ob_seal(&first, &g, seed, np, gid, dealt.logs), "edges %dp: first seal", np);
        CHECK(ob_boundary(&dealt, &first) == 0,
              "edges %dp: the opening attack over a dealt table opened at %d",
              np, ob_boundary(&dealt, &first));

        // START ARRIVING ON A LOBBY ON SCREEN (#255: a started bubble's replay
        // is its deal, one step, which the bridge plays only from atom 0). The
        // lobby has no body and neither does the started chain, so nothing of
        // it has been shown: the boundary is at most 0 (the claim, -1 "does not
        // say", or the empty shared prefix), whether the lobby is the invite at
        // capacity or the full table the last join left. The bridge
        // (fio_replay_last_events_packed) plays a one-step chain's deal for
        // either; anything above 0 would skip it.
        {
            static OBubble lobby;
            const int caps[2] = { 8, np };
            for (int c = 0; c < 2; c++) {
                MsgEnvelope e;
                env_init(&e, seed, caps[c]);
                e.phase = MSG_PHASE_WAITING;
                e.game_id = gid;
                e.n_joins = (uint8_t)(c ? np : 1);
                lobby.n = msg_encode(&e, lobby.w, sizeof(lobby.w));
                CHECK(lobby.n > 0 && msg_decode(lobby.w, lobby.n, &lobby.e) == MSG_EOK,
                      "edges %dp: lobby (cap %d) encode", np, caps[c]);
                CHECK(ob_boundary(&lobby, &dealt) <= 0,
                      "edges %dp: Start over a lobby of %d opened at %d, so its deal never plays",
                      np, caps[c], ob_boundary(&lobby, &dealt));
            }
            CHECK(ob_boundary(NULL, &dealt) <= 0,
                  "edges %dp: a started bubble opened cold at %d", np, ob_boundary(NULL, &dealt));
        }

        // NOTHING: the undo-to-empty reseal of the chain on screen.
        CHECK(ob_seal(&nothing, &g, seed, np, gid, MSG_BASE_NOTHING), "edges %dp: nothing seal", np);
        CHECK(nothing.e.n_new == MSG_NEW_NOTHING, "edges %dp: the reseal is not NOTHING", np);
        CHECK(ob_boundary(&first, &nothing) == nothing.e.turn,
              "edges %dp: a NOTHING reseal opened at %d of %d", np,
              ob_boundary(&first, &nothing), nothing.e.turn);

        // A sender that does not say: the claim is -1 (guess), and a board
        // that showed the parent pins it to the parent's atoms.
        CHECK(ob_seal(&nobase, &g, seed, np, gid, MSG_NO_BASE), "edges %dp: no-base seal", np);
        CHECK(msg_atoms_before_claim(&nobase.e) == -1, "edges %dp: a no-base claim", np);
        CHECK(ob_boundary(NULL, &nobase) == -1, "edges %dp: a cold no-base open was clamped", np);
        CHECK(ob_boundary(&dealt, &nobase) == 0,
              "edges %dp: a no-base chain over its own dealt table opened at %d",
              np, ob_boundary(&dealt, &nobase));

        // ANOTHER GAME on screen shares nothing: same seed but another id, and
        // another seed under the same id.
        og_start(&h, seed2, np);
        CHECK(ob_seal(&other, &h, seed2, np, gid, 0), "edges %dp: other seal", np);
        CHECK(ob_boundary(&other, &first) == msg_atoms_before_claim(&first.e),
              "edges %dp: another deal clamped the boundary", np);
        CHECK(ob_seal(&other, &g, seed, np, gid + 1, MSG_NO_BASE), "edges %dp: other id seal", np);
        CHECK(ob_boundary(&other, &nobase) == -1,
              "edges %dp: another game id clamped the boundary to %d", np, ob_boundary(&other, &nobase));
    }
}

// E. THE MOVE THAT ENDS THE GAME, arriving on a board that showed the chain
// before it. The arriving bubble is FINISHED and msg_replay now settles a
// finished chain's status (game_settle_status), so this pins that the boundary
// is still the sender's claim, that the final move is left to animate, and that
// adopting the arrival rebuilds a game that is over.
static void ob_game_ending(int *posed) {
    static Game g, s0, rg;
    static OBubble shown, arr;
    for (int pi = 0; pi < 3; pi++) {
        const int np = OB_SEATS[pi];
        for (uint32_t gi = 0; gi < 60 && posed[pi] < 6; gi++) {
            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, 17300u + gi * 41u + (uint32_t)np);
            g_rng = 9900u + gi * 17u + (uint32_t)np;
            og_start(&g, seed, np);
            const uint64_t gid = 0xF0A0ULL + gi;
            for (int step = 0; step < 2000 && og_live(&g); step++) {
                game_clone(&s0, &g);
                if (!og_random_step(&g)) break;
                if (og_live(&g)) continue;
                // `g` is over and `s0` is the table one move before.
                if (!ob_seal(&shown, &s0, seed, np, gid, MSG_NO_BASE)) break;
                if (!ob_seal(&arr, &g, seed, np, gid, shown.logs)) break;
                CHECK(shown.e.phase == MSG_PHASE_LIVE && arr.e.phase == MSG_PHASE_FINISHED,
                      "game end %dp: phases %d -> %d", np, shown.e.phase, arr.e.phase);
                const int b = ob_boundary(&shown, &arr);
                const int claim = msg_atoms_before_claim(&arr.e);
                CHECK(b == claim, "game end %dp: boundary %d, want the sender's %d", np, b, claim);
                CHECK(b >= 0 && b < arr.e.turn,
                      "game end %dp: boundary %d leaves nothing of %d atoms to animate",
                      np, b, arr.e.turn);
                CHECK(ob_atom_kind(&arr, b) >= 0,
                      "game end %dp: no action atom at the boundary %d", np, b);
                // Adopting it rebuilds a game that is over; the shown chain
                // still replays as one in play.
                CHECK(msg_replay(&arr.e, &rg) == MSG_EOK && rg.status == GAME_STATUS_GAME_OVER,
                      "game end %dp: the finished arrival replayed status %d", np, rg.status);
                CHECK(msg_replay(&shown.e, &rg) == MSG_EOK && rg.status == GAME_STATUS_PLAYING,
                      "game end %dp: the chain before the end replayed status %d", np, rg.status);
                // A re-delivery of the finished chain animates nothing.
                CHECK(ob_boundary(&arr, &arr) == arr.e.turn,
                      "game end %dp: a re-delivery opened at %d of %d",
                      np, ob_boundary(&arr, &arr), arr.e.turn);
                posed[pi]++;
                break;
            }
        }
        CHECK(posed[pi] > 0, "game end %dp: no fixture built", np);
    }
}

static void test_open_boundary(void) {
    int closing[3] = { 0 }, after[3][4] = { { 0 } }, stale[6] = { 0 }, ending[3] = { 0 };
    ob_closing_good(closing);
    ob_after_good(after);
    ob_stale_and_siblings(stale);
    ob_edges();
    ob_game_ending(ending);
    printf("  open boundary: game end %d/%d/%d (2/3/4p)\n", ending[0], ending[1], ending[2]);
    printf("  open boundary: closing good %d/%d/%d (2/3/4p); after a pending good 3p "
           "attack %d cover %d pass %d pickup %d, 4p attack %d cover %d pass %d pickup %d; "
           "raced goods %d/%d (3/4p); stale %d/%d/%d, sibling %d/%d/%d\n",
           closing[0], closing[1], closing[2],
           after[1][0], after[1][1], after[1][2], after[1][3],
           after[2][0], after[2][1], after[2][2], after[2][3],
           after[0][1], after[0][2],
           stale[0], stale[1], stale[2], stale[3], stale[4], stale[5]);
}

// ---------- what an arrival leaves of a staged bubble ----------------------
//
// msg_staged_fate, the kernel's answer to "may the bubble I put in the input
// field still be sent, now that this chain arrived". Every fixture is real
// bubbles sealed the way a phone seals them, AND LINKED the way a phone links
// them (parent8 = the first eight bytes of the parent's digest), because the
// header is half the answer: a staged good that the very next bubble folded
// away is recognised as landed only by the child naming it.
//
// The matrix: 2, 3 and 4 seats; the staged move is each kind a seat can stage
// (an opening attack, a throw-in, a cover, a pass, a pickup, a good); the
// arrival is the staged bubble itself, a child and a grandchild of it (it went
// out), the chain it was built on and that chain's own parent (nothing new), a
// SIBLING off the same parent and a child of that sibling (the note-6 shape),
// and another game.

static int fb_seal(OBubble *b, const Game *g, const uint8_t *seed, int np,
                   uint64_t game_id, int base_logs, const OBubble *parent) {
    static unsigned char body[2048];
    static Game scratch;
    MsgEnvelope e;
    env_init(&e, seed, np);
    e.game_id = game_id;
    const int over = game_done(g) >= 0 || g->status == GAME_STATUS_GAME_OVER;
    e.phase = over ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE;
    if (parent) {
        uint8_t d[SHA256_DIGEST_LEN];
        msg_digest(parent->w, parent->n, d);
        memcpy(e.parent8, d, MSG_PARENT_LEN);
    }
    if (msg_seal(&e, g, base_logs, body, sizeof(body), &scratch) != MSG_EOK) return 0;
    b->n = msg_encode(&e, b->w, sizeof(b->w));
    if (b->n <= 0) return 0;
    if (msg_decode(b->w, b->n, &b->e) != MSG_EOK) return 0;
    b->logs = g->num_logs;
    return 1;
}

static int fb_fate(const OBubble *staged, const OBubble *arrived) {
    static unsigned char scratch[MSG_OPEN_SCRATCH];
    return msg_staged_fate(staged->w, staged->n, arrived->w, arrived->n, scratch, sizeof(scratch));
}

// The six kinds a human can stage, as this matrix tells them apart.
enum { FK_OPEN, FK_THROW, FK_COVER, FK_PASS, FK_PICKUP, FK_GOOD, FK_N };
static const char *const FK_NAME[FK_N] = { "attack", "throw-in", "cover", "pass", "pickup", "good" };

static int fb_kind(const Game *g, const LegalMove *m) {
    switch (m->type) {
        case MOVE_ATTACK: return g->num_battles == 0 ? FK_OPEN : FK_THROW;
        case MOVE_COVER:  return FK_COVER;
        case MOVE_PASS:   return FK_PASS;
        case MOVE_PICKUP: return FK_PICKUP;
        case MOVE_GOOD:   return FK_GOOD;
        default:          return -1;
    }
}

// One random legal move by any seat but `not_seat`. 0 when none can.
static int fb_step_by_another(Game *g, int not_seat) {
    static LegalMoves ml;
    const int np = g->num_players;
    const int start = (int)(rnd() % (uint32_t)np);
    for (int t = 0; t < np; t++) {
        const int s = (start + t) % np;
        if (s == not_seat || g->players[s].status != PLAYER_STATUS_IN) continue;
        calculate_legal_moves(g, s, &ml);
        for (int i = 0; i < ml.n; i++) {
            if (ml.moves[i].type == MOVE_WAIT) continue;
            return legal_move_apply(g, s, &ml.moves[i]) ? 1 : 0;
        }
    }
    return 0;
}

// Pose one staged move `m` by `seat` on the table `g`, whose bubble is `p`
// (built on `gp`, which may be NULL), and check every arrival against it.
static void fb_pose(const Game *g, const OBubble *p, const Game *gpg, const OBubble *gp,
                    int seat, const LegalMove *m, const uint8_t *seed, int np, uint64_t gid,
                    int posed[3][FK_N + 1], int pi) {
    static Game s, n1, c1, c2;
    static OBubble S, N1, NC, C1, C2, NOTH, NOTH_ARR, NOTH_CHILD;
    static LegalMoves ml;
    const int kind = fb_kind(g, m);
    if (kind < 0) return;
    game_clone(&s, g);
    if (!legal_move_apply(&s, seat, m)) return;
    if (!fb_seal(&S, &s, seed, np, gid, p->logs, p)) return;

    // A SIBLING: ANOTHER SEAT's move off the same table - two people acting on
    // one bubble. Never this seat's: only this device makes this seat's moves,
    // so a chain holding them came from here, and the atoms rightly say so (a
    // two-card attack is the one-card attack plus a throw-in of the second).
    int have_sib = 0;
    for (int t = 1; t < np && !have_sib; t++) {
        const int who = (seat + t) % np;
        if (g->players[who].status != PLAYER_STATUS_IN) continue;
        calculate_legal_moves(g, who, &ml);
        for (int i = 0; i < ml.n && !have_sib; i++) {
            if (ml.moves[i].type == MOVE_WAIT) continue;
            game_clone(&n1, g);
            if (legal_move_apply(&n1, who, &ml.moves[i])
                && fb_seal(&N1, &n1, seed, np, gid, p->logs, p)) have_sib = 1;
        }
    }
    // Nobody else may act here (an opening attack: the others wait). Then the
    // only chain that can supersede the staged move is a FORK FURTHER BACK:
    // another seat's move off the bubble before, which the table took instead
    // of the one this board stood on.
    for (int t = 1; t < np && !have_sib && gpg && gp; t++) {
        const int who = (seat + t) % np;
        if (gpg->players[who].status != PLAYER_STATUS_IN) continue;
        calculate_legal_moves(gpg, who, &ml);
        for (int i = 0; i < ml.n && !have_sib; i++) {
            if (ml.moves[i].type == MOVE_WAIT) continue;
            game_clone(&n1, gpg);
            if (!legal_move_apply(&n1, who, &ml.moves[i])
                || !fb_seal(&N1, &n1, seed, np, gid, gp->logs, gp)) continue;
            // A real fork from where the board stood, not the same moves cut
            // at another bubble boundary: a one-card cover off the bubble
            // before is the first atom of the two-card cover this board saw.
            have_sib = fb_fate(p, &N1) == MSG_FATE_SUPERSEDED;
        }
    }
    if (!have_sib) return;

    const char *kn = FK_NAME[kind];
    // THE BUBBLE ITSELF, AND ITS DESCENDANTS: it went out.
    CHECK(fb_fate(&S, &S) == MSG_FATE_LANDED, "fate %dp %s: the same bytes", np, kn);
    game_clone(&c1, &s);
    if (og_live(&c1) && og_random_step(&c1) && fb_seal(&C1, &c1, seed, np, gid, S.logs, &S)) {
        CHECK(fb_fate(&S, &C1) == MSG_FATE_LANDED,
              "fate %dp %s: a child of the staged bubble is %d, want LANDED", np, kn, fb_fate(&S, &C1));
        // THE HEADER IS LOAD-BEARING for a good the child folded away: the
        // same child without its parent link reads as a fork (the safe side).
        static OBubble C1U;
        if (ob_atom_kind(&S, S.e.turn - 1) == REPLAY_ATOM_GOOD
            && ob_atom_kind(&C1, S.e.turn - 1) != REPLAY_ATOM_GOOD
            && fb_seal(&C1U, &c1, seed, np, gid, S.logs, NULL)) {
            CHECK(fb_fate(&S, &C1U) == MSG_FATE_SUPERSEDED,
                  "fate %dp: a folded good's unlinked child is %d", np, fb_fate(&S, &C1U));
            posed[pi][FK_N]++;
        }
        game_clone(&c2, &c1);
        if (og_live(&c2) && og_random_step(&c2) && fb_seal(&C2, &c2, seed, np, gid, C1.logs, &C1)) {
            // Two hops: the atoms decide. A staged GOOD the next move folded
            // away is the documented conservative case.
            const int want = ob_atom_kind(&S, S.e.turn - 1) == REPLAY_ATOM_GOOD
                ? -1 : MSG_FATE_LANDED;
            if (want >= 0)
                CHECK(fb_fate(&S, &C2) == want,
                      "fate %dp %s: a grandchild is %d, want LANDED", np, kn, fb_fate(&S, &C2));
            else
                CHECK(fb_fate(&S, &C2) != MSG_FATE_STANDS,
                      "fate %dp %s: a folded good's grandchild read as the past", np, kn);
        }
    }
    // WHAT IT WAS BUILT ON: nothing new.
    CHECK(fb_fate(&S, p) == MSG_FATE_STANDS,
          "fate %dp %s: the parent is %d, want STANDS", np, kn, fb_fate(&S, p));
    if (gp && ob_atom_kind(p, p->e.turn - 1) != REPLAY_ATOM_GOOD
        && ob_atom_kind(gp, gp->e.turn - 1) != REPLAY_ATOM_GOOD)
        CHECK(fb_fate(&S, gp) == MSG_FATE_STANDS,
              "fate %dp %s: the grandparent is %d, want STANDS", np, kn, fb_fate(&S, gp));
    // THE NOTE-6 SHAPE: another move off the same bubble.
    CHECK(fb_fate(&S, &N1) == MSG_FATE_SUPERSEDED,
          "fate %dp %s: a sibling is %d, want SUPERSEDED", np, kn, fb_fate(&S, &N1));
    CHECK(msg_turn_field_after_arrival(MSG_TURN_READY | MSG_TURN_BOARD_WATCHING | MSG_TURN_STAGED,
                                       fb_fate(&S, &N1)) == MSG_TURN_FIELD_NOTHING,
          "fate %dp %s: a sibling's arrival leaves the stale move in the field", np, kn);
    game_clone(&c1, &n1);
    if (og_live(&c1) && fb_step_by_another(&c1, seat)
        && fb_seal(&NC, &c1, seed, np, gid, N1.logs, &N1))
        CHECK(fb_fate(&S, &NC) == MSG_FATE_SUPERSEDED,
              "fate %dp %s: a child of the sibling is %d, want SUPERSEDED", np, kn, fb_fate(&S, &NC));

    // THE UNDO'S BUBBLE: the NOTHING reseal of the parent. It carries no move,
    // so only the header can say it landed - and a stale one is overwritten.
    // (A dealt table with no action yet seals no body at all, so there is no
    // NOTHING to say: its reseal is the turn-0 handoff's shape, n_new 0.)
    if (p->e.turn > 0 && fb_seal(&NOTH, g, seed, np, gid, MSG_BASE_NOTHING, p)) {
        CHECK(NOTH.e.n_new == MSG_NEW_NOTHING, "fate %dp: the reseal is not NOTHING", np);
        CHECK(fb_fate(&NOTH, p) == MSG_FATE_STANDS, "fate %dp: NOTHING over its parent", np);
        CHECK(fb_fate(&NOTH, &NOTH) == MSG_FATE_LANDED, "fate %dp: NOTHING came back", np);
        CHECK(fb_fate(&NOTH, &N1) == MSG_FATE_SUPERSEDED,
              "fate %dp %s: a move off the parent left the stale NOTHING bubble standing (%d)",
              np, kn, fb_fate(&NOTH, &N1));
        CHECK(fb_fate(&NOTH, &S) == MSG_FATE_SUPERSEDED,
              "fate %dp %s: a NOTHING bubble is not carried by its parent's other children", np, kn);
        game_clone(&c1, g);
        if (og_live(&c1) && fb_step_by_another(&c1, seat)
            && fb_seal(&NOTH_CHILD, &c1, seed, np, gid, NOTH.logs, &NOTH))
            CHECK(fb_fate(&NOTH, &NOTH_CHILD) == MSG_FATE_LANDED,
                  "fate %dp: a child naming the NOTHING bubble carries it", np);
        // …and somebody ELSE's NOTHING bubble over the chain mine was built
        // on adds nothing: STANDS, which the turn layer still overwrites.
        if (fb_seal(&NOTH_ARR, g, seed, np, gid, MSG_BASE_NOTHING, p)) {
            const int f = fb_fate(&S, &NOTH_ARR);
            CHECK(f != MSG_FATE_LANDED, "fate %dp %s: a NOTHING arrival carried my move", np, kn);
            CHECK(msg_turn_field_after_arrival(MSG_TURN_READY | MSG_TURN_STAGED, f)
                  == MSG_TURN_FIELD_NOTHING, "fate %dp %s: the field kept a dropped move", np, kn);
        }
    }
    // The other way round: the sibling's own field, after MY move arrived.
    CHECK(fb_fate(&N1, &S) == MSG_FATE_SUPERSEDED, "fate %dp %s: siblings are mutual", np, kn);
    posed[pi][kind]++;
}

static void test_staged_fate(void) {
    static Game g, prev;
    static OBubble p, gp, other;
    int posed[3][FK_N + 1] = { { 0 } };
    for (int pi = 0; pi < 3; pi++) {
        const int np = OB_SEATS[pi];
        for (uint32_t gi = 0; gi < 40; gi++) {
            uint8_t seed[MSG_SEED_LEN];
            seed_fill(seed, 21700u + gi * 31u + (uint32_t)np);
            g_rng = 9900u + gi * 17u + (uint32_t)np;
            og_start(&g, seed, np);
            const uint64_t gid = 0xF0A0ULL + gi;
            int have_p = 0, have_gp = 0;
            for (int step = 0; step < 140 && og_live(&g); step++) {
                // The table is a bubble, linked to the bubble before it.
                if (have_p) { gp = p; have_gp = 1; }
                if (!fb_seal(&p, &g, seed, np, gid, have_p ? gp.logs : MSG_NO_BASE,
                             have_p ? &gp : NULL)) break;
                // gp borrowed p's bytes by value; re-point its envelope.
                if (have_gp && msg_decode(gp.w, gp.n, &gp.e) != MSG_EOK) break;
                have_p = 1;
                static LegalMoves ml;
                for (int seat = 0; seat < np; seat++) {
                    if (g.players[seat].status != PLAYER_STATUS_IN) continue;
                    calculate_legal_moves(&g, seat, &ml);
                    for (int i = 0; i < ml.n; i++) {
                        const int k = fb_kind(&g, &ml.moves[i]);
                        if (k < 0 || posed[pi][k] >= 6) continue;
                        fb_pose(&g, &p, have_gp ? &prev : NULL, have_gp ? &gp : NULL, seat,
                                &ml.moves[i], seed, np, gid, posed, pi);
                    }
                }
                game_clone(&prev, &g);
                if (!og_random_step(&g)) break;
            }
            // ANOTHER GAME shares nothing, whatever its atoms.
            if (have_p && fb_seal(&other, &prev, seed, np, gid + 1000, MSG_NO_BASE, NULL))
                CHECK(fb_fate(&p, &other) == MSG_FATE_SUPERSEDED,
                      "fate %dp: another game id is %d, want SUPERSEDED", np, fb_fate(&p, &other));
        }
        if (np > 2) CHECK(posed[pi][FK_N] > 0, "fate %dp: no folded good posed", np);
        for (int k = 0; k < FK_N; k++) {
            // A pending good cannot exist at two seats - the one attacker's
            // good closes the bout - but staging the closing good can.
            CHECK(posed[pi][k] > 0, "fate %dp: no %s posed", np, FK_NAME[k]);
        }
    }
    // Junk is an error, never a fate.
    {
        static unsigned char junk[4] = { 1, 2, 3, 4 }, scratch[MSG_OPEN_SCRATCH];
        CHECK(msg_staged_fate(junk, 4, p.w, p.n, scratch, sizeof(scratch)) < 0, "junk staged");
        CHECK(msg_staged_fate(p.w, p.n, junk, 4, scratch, sizeof(scratch)) < 0, "junk arrived");
        CHECK(msg_staged_fate(NULL, 0, p.w, p.n, scratch, sizeof(scratch)) < 0, "no staged bytes");
    }
    printf("  staged fate: 2p %d/%d/%d/%d/%d/%d, 3p %d/%d/%d/%d/%d/%d, 4p %d/%d/%d/%d/%d/%d "
           "(attack/throw-in/cover/pass/pickup/good); folded goods %d/%d (3/4p)\n",
           posed[0][0], posed[0][1], posed[0][2], posed[0][3], posed[0][4], posed[0][5],
           posed[1][0], posed[1][1], posed[1][2], posed[1][3], posed[1][4], posed[1][5],
           posed[2][0], posed[2][1], posed[2][2], posed[2][3], posed[2][4], posed[2][5],
           posed[1][FK_N], posed[2][FK_N]);
}

// ---------- formats 5 and 6, pinned as bytes ------------------------------
//
// Formats 5 and 6 are what every shipped build writes for an ordinary game and
// for a fool's-penalty game, so they must seal and decode EXACTLY as they do
// today whatever format is added after them. These four envelopes are built
// deterministically and their bytes are pinned in GOLDEN_HEX
// (`msg_wire_test --print-goldens` prints them):
//
//   0  format 5, LIVE, 3 players, passing, a sent clock and a bubble delta
//   1  format 5, FINISHED, 2 players, podkidnoy
//   2  format 6, WAITING, 4 seated, with a rematch carry
//   3  format 6, LIVE, 4 players, opening pinned to seat 2
//
// What is asserted (test_format56_goldens): the builder still produces the
// same bytes (the seal and the encoder did not move), and each golden decodes,
// replays and re-encodes to itself byte for byte.
static int golden_build(int which, unsigned char *out, int cap) {
    static unsigned char body[1024];
    static Game scratch;
    Chain ch; memset(&ch, 0, sizeof(ch));
    Game g;
    uint8_t seed[MSG_SEED_LEN];
    MsgEnvelope e;
    switch (which) {
    case 0: {
        seed_fill(seed, 5101u);
        play_game_rules(seed, 3, 9, &ch, &g, -1, 0);
        env_init(&e, seed, 3);
        e.phase = MSG_PHASE_LIVE;
        e.game_id = 0x5151515151515151ULL;
        e.sent_at = 0x1234;
        e.last_actor_seat = 1;
        if (msg_seal(&e, &g, 0, body, sizeof(body), &scratch) != MSG_EOK) return -1;
        break;
    }
    case 1: {
        // Seeds are walked until a podkidnoy game reaches a fool, so the golden
        // is a FINISHED chain whatever the seed range happens to hold.
        int found = 0;
        for (uint32_t s = 5201u; s < 5301u && !found; s++) {
            seed_fill(seed, s);
            memset(&ch, 0, sizeof(ch));
            play_game_rules(seed, 2, 600, &ch, &g, -1, (int8_t)GAME_RULE_NO_PASS);
            game_settle_status(&g);
            found = game_done(&g) >= 0;
        }
        if (!found) return -1;
        env_init(&e, seed, 2);
        e.phase = MSG_PHASE_FINISHED;
        e.game_id = 0x5252525252525252ULL;
        e.sent_at = 0x2345;
        e.last_actor_seat = 0;
        if (msg_seal(&e, &g, 0, body, sizeof(body), &scratch) != MSG_EOK) return -1;
        break;
    }
    case 2: {
        seed_fill(seed, 5301u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        memset(&g, 0, sizeof(g));
        g.num_players = 4;
        for (int i = 0; i < 4; i++) g.players[i].status = PLAYER_STATUS_READY;
        start_game(&g);
        env_init(&e, seed, 4);
        e.phase = MSG_PHASE_WAITING;
        e.game_id = 0x5353535353535353ULL;
        e.sent_at = 0x0BAD;
        uint32_t key = 0; int rot = 0;
        if (msg_roster_key(e.joins, 4, &key, &rot) != MSG_EOK) return -1;
        e.carry_key = key;
        e.carry_fool = 2;
        if (msg_seal(&e, &g, MSG_NO_BASE, body, sizeof(body), &scratch) != MSG_EOK) return -1;
        break;
    }
    case 3: {
        seed_fill(seed, 5401u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        memset(&g, 0, sizeof(g));
        g.num_players = 4;
        for (int i = 0; i < 4; i++) g.players[i].status = PLAYER_STATUS_READY;
        game_open_at_seat(2);
        start_game(&g);
        game_open_at_seat(-1);
        static LegalMoves ml;
        calculate_legal_moves(&g, 2, &ml);
        int pick = -1;
        for (int i = 0; i < ml.n && pick < 0; i++) if (ml.moves[i].type == MOVE_ATTACK) pick = i;
        if (pick < 0) return -1;
        AwireAction a;
        move_to_awire(&ml.moves[pick], &a);
        if (!handle_attack(&g, 2, a.cards, a.n)) return -1;
        env_init(&e, seed, 4);
        e.phase = MSG_PHASE_LIVE;
        e.game_id = 0x5454545454545454ULL;
        e.sent_at = 0x0C0D;
        e.last_actor_seat = 2;
        e.opening = 2;
        if (msg_seal(&e, &g, 0, body, sizeof(body), &scratch) != MSG_EOK) return -1;
        break;
    }
    default: return -1;
    }
    return msg_encode(&e, out, cap);
}

static void print_goldens(void) {
    for (int w = 0; w < 4; w++) {
        unsigned char wire[ENV_CAP];
        const int n = golden_build(w, wire, sizeof(wire));
        if (n <= 0) { printf("golden %d: build failed (%d)\n", w, n); continue; }
        static char hx[ENV_CAP * 2 + 1];
        hex(wire, n, hx);
        printf("golden %d (format %d, %d B):\n%s\n", w, wire[1], n, hx);
    }
}

// Captured with --print-goldens at origin/main 4c7e65f6, before any rematch
// generation existed. NEVER regenerate these to make a test pass: a diff here
// means a shipped bubble now means something else.
static const char *const GOLDEN_HEX[4] = {
    "f7050002515151515151515107000103010100000000000000002bf3a623483c4f879a33932efdbeef2b"
    "36a75bc695b9e59694e15aa5764ede8f341207030004416c657801044d69726102054a6f6e6173070007"
    "35df9bd38d17730682ea37771ff4b43a",
    "f7050003525252525252525272000002002e00000000000000008a15e717926cb1a776a796b62bfe3d79"
    "32ad677732128daca1ca85a45cd81e9c452372020004416c657801044d69726172002464460f7b2bb8d5"
    "58d69178f6a7890a2f7d1a70187be79bd51f4d926ee8f061276f3badeab534d420564003fd55c55f7ec2"
    "823ec1e62a",
    "f706000053535353535353530000000401000000000000000000e837280bdc9b13c8521a993e5a3e8bc6"
    "2db37329d06a35c3afb3b1a343625ea9ad0b0002ff44c9ed0102040004416c657801044d69726102054a"
    "6f6e6173030550726979610000",
    "f706000254545454545454540100020401000000000000000000475869fe26cb76e82e8e9dc7887ed913"
    "29b97fda6dc3dddabd9cdca229eb9fb50d0c01020200000000ff040004416c657801044d69726102054a"
    "6f6e61730305507269796101000d37490e3be569077df74386ce7a",
};

// A format-7 bubble as hex, kept by test_format7_wire for --print-format7, so a
// binary built before format 7 existed can be shown one (`--decode`).
static char g_format7_sample[ENV_CAP * 2 + 1];
static int g_format7_sample_len;

static int unhex(const char *h, unsigned char *out, int cap) {
    int n = 0;
    for (const char *p = h; p[0] && p[1] && n < cap; p += 2) {
        unsigned v = 0;
        if (sscanf(p, "%2x", &v) != 1) return -1;
        out[n++] = (unsigned char)v;
    }
    return n;
}

static void test_format56_goldens(void) {
    for (int w = 0; w < 4; w++) {
        unsigned char want[ENV_CAP], got[ENV_CAP], again[ENV_CAP];
        const int nw = unhex(GOLDEN_HEX[w], want, sizeof(want));
        const int ng = golden_build(w, got, sizeof(got));
        CHECK(ng == nw && ng > 0 && !memcmp(got, want, (size_t)nw),
              "golden %d: the builder now seals %d bytes, not the pinned %d - formats 5/6 moved", w, ng, nw);
        MsgEnvelope e;
        const int rc = msg_decode(want, nw, &e);
        CHECK(rc == MSG_EOK, "golden %d: decode %d", w, rc);
        if (rc != MSG_EOK) continue;
        CHECK(e.format == (w < 2 ? MSG_FORMAT_RULES : MSG_FORMAT_RULES_REMATCH),
              "golden %d: decoded format %d", w, e.format);
        const int nr = msg_encode(&e, again, sizeof(again));
        CHECK(nr == nw && !memcmp(again, want, (size_t)nw),
              "golden %d: re-encode is %d bytes and not the golden's %d", w, nr, nw);
        static Game g;
        CHECK(msg_replay(&e, &g) == MSG_EOK, "golden %d: replay refused", w);
        CHECK(e.generation == 0, "golden %d: a format-%d chain decoded as generation %u",
              w, e.format, e.generation);
    }
}

// ---------- format 7: the rematch generation -------------------------------
//
// The layout, byte for byte: format 6's header, then the generation as a u16 LE
// at 68, then n_joins at 70. A seal writes 7 exactly when the generation is not
// 0, and the format and the field must agree in both directions.
static void test_format7_wire(void) {
    for (int w = 0; w < 4; w++) {
        static unsigned char base[ENV_CAP];
        const int nb = unhex(GOLDEN_HEX[w], base, sizeof(base));
        MsgEnvelope e;
        if (msg_decode(base, nb, &e) != MSG_EOK) { CHECK(0, "format7 %d: golden did not decode", w); continue; }
        const int had_block = base[1] == MSG_FORMAT_RULES_REMATCH;
        const uint16_t gen = (uint16_t)(0x0102 + w);
        e.generation = gen;
        e.format = MSG_FORMAT_GENERATION;
        unsigned char wire[ENV_CAP];
        const int n = msg_encode(&e, wire, sizeof(wire));
        // 7 always carries the rematch block (7 bytes over format 5's 62) and
        // then two generation bytes.
        const int want_n = nb + 2 + (had_block ? 0 : MSG_HEADER_LEN_REMATCH - MSG_HEADER_LEN_CLOCK);
        CHECK(n == want_n, "format7 %d: %d bytes, want %d", w, n, want_n);
        if (n <= 0) continue;
        CHECK(wire[1] == 7, "format7 %d: format byte %d", w, wire[1]);
        CHECK(!memcmp(wire + 2, base + 2, MSG_CLOCK_OFF - 2), "format7 %d: the shared prefix moved", w);
        CHECK(wire[MSG_GEN_OFF] == (gen & 0xff) && wire[MSG_GEN_OFF + 1] == (gen >> 8),
              "format7 %d: generation bytes %02x %02x", w, wire[MSG_GEN_OFF], wire[MSG_GEN_OFF + 1]);
        CHECK(wire[MSG_HEADER_LEN_GENERATION - 1] == e.n_joins,
              "format7 %d: n_joins not at %d", w, MSG_HEADER_LEN_GENERATION - 1);
        MsgEnvelope d;
        CHECK(msg_decode(wire, n, &d) == MSG_EOK && d.generation == gen && d.game_id == e.game_id,
              "format7 %d: did not decode back to generation %u", w, gen);
        unsigned char again[ENV_CAP];
        CHECK(msg_encode(&d, again, sizeof(again)) == n && !memcmp(again, wire, (size_t)n),
              "format7 %d: re-encode is not byte-identical", w);
        static Game g;
        CHECK(msg_replay(&d, &g) == MSG_EOK, "format7 %d: a format-7 chain did not replay", w);

        // BOTH DIRECTIONS. A format-7 header that says 0 is a second spelling
        // of a format-5/6 chain; an earlier format with a generation cannot
        // carry it.
        unsigned char t[ENV_CAP];
        memcpy(t, wire, (size_t)n);
        t[MSG_GEN_OFF] = 0; t[MSG_GEN_OFF + 1] = 0;
        CHECK(msg_decode(t, n, &d) == MSG_EFORMAT, "format7 %d: a generation-0 format-7 header decoded", w);
        MsgEnvelope bad = e;
        bad.format = MSG_FORMAT_RULES_REMATCH;
        CHECK(msg_encode(&bad, t, sizeof(t)) == MSG_EFORMAT,
              "format7 %d: a format-6 envelope with a generation encoded", w);
        if (w == 2) {
            static char hx[ENV_CAP * 2 + 1];
            hex(wire, n, hx);
            g_format7_sample_len = n;
            memcpy(g_format7_sample, hx, (size_t)n * 2 + 1);
        }
    }

    // THE SEAL CHOOSES: generation 0 seals 5 (the goldens prove those bytes),
    // anything else seals 7.
    {
        uint8_t seed[MSG_SEED_LEN];
        seed_fill(seed, 7007u);
        game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
        static Game g, scratch;
        memset(&g, 0, sizeof(g));
        g.num_players = 3;
        for (int i = 0; i < 3; i++) g.players[i].status = PLAYER_STATUS_READY;
        start_game(&g);
        MsgEnvelope e;
        env_init(&e, seed, 3);
        e.phase = MSG_PHASE_WAITING;
        static unsigned char body[512];
        CHECK(msg_seal(&e, &g, 0, body, sizeof(body), &scratch) == MSG_EOK
              && e.format == MSG_FORMAT_RULES, "seal: generation 0 did not seal format 5 (%d)", e.format);
        e.generation = 1;
        CHECK(msg_seal(&e, &g, 0, body, sizeof(body), &scratch) == MSG_EOK
              && e.format == MSG_FORMAT_GENERATION, "seal: generation 1 did not seal format 7 (%d)", e.format);
    }
}

// --decode <hex>: what THIS build's decoder says about a payload. Kept so a
// binary built from an older commit can be pointed at a bubble a newer build
// sealed - that is the only honest model of an old client.
static void print_decode(const char *h) {
    static unsigned char wire[ENV_CAP];
    const int n = unhex(h, wire, sizeof(wire));
    MsgEnvelope e;
    const int rc = n > 0 ? msg_decode(wire, n, &e) : MSG_ESHORT;
    printf("decode: %d bytes, format byte %d, msg_decode %d\n", n, n > 1 ? wire[1] : -1, rc);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--print-goldens")) { print_goldens(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--fixture")) { print_fixtures(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--fixture4")) { print_fixtures4(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--fixture5")) { print_fixtures5(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--goodwait")) {
        print_goodwait(argc > 2 ? atoi(argv[2]) : 8);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--passable")) {
        print_passable(argc > 2 ? atoi(argv[2]) : 3);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--endgame")) {
        print_endgame(argc > 2 ? atoi(argv[2]) : 3,
                      !(argc > 3 && !strcmp(argv[3], "nopass")), 0);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--endgame-arrival")) {
        print_endgame(argc > 2 ? atoi(argv[2]) : 3,
                      !(argc > 3 && !strcmp(argv[3], "nopass")), 1);
        return 0;
    }
    if (argc > 2 && (!strcmp(argv[1], "--lastmove") || !strcmp(argv[1], "--lastmove-live"))) {
        static const char *names[] = { "attack", "cover", "pickup", "pass",
                                        "good", "out", "refill", "refillempty",
                                        "covertrump", "final", "goodany",
                                        "refilltrump" };
        int kind = -1;
        for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
            if (!strcmp(argv[2], names[k])) { kind = (int)k; break; }
        if (kind < 0) {
            fprintf(stderr, "--lastmove: unknown kind '%s' (attack|cover|pickup|pass|"
                            "good|goodany|out|refill|refillempty|refilltrump|covertrump|final)\n", argv[2]);
            return 2;
        }
        print_lastmove_ex(argc > 3 ? atoi(argv[3]) : 2, kind,
                           !strcmp(argv[1], "--lastmove-live"));
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--holdcheck")) { print_holdcheck(argv[2]); return 0; }
    if (argc > 2 && !strcmp(argv[1], "--decode")) { print_decode(argv[2]); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--print-format7")) {
        test_format7_wire();
        printf("format 7 (%d B):\n%s\n", g_format7_sample_len, g_format7_sample);
        return g_fails ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--lastdefense")) {
        print_lastdefense(argc > 2 ? atoi(argv[2]) : 2);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--chain")) {
        print_chain(argc > 2 ? atoi(argv[2]) : 2,
                    argc > 3 ? atoi(argv[3]) : 6,
                    argc > 4 ? atoi(argv[4]) : 12);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--fatboard")) {
        print_fatboard(argc > 2 ? atoi(argv[2]) : 10, argc > 3 ? atoi(argv[3]) : 2,
                       argc > 4 && !strcmp(argv[4], "nopass"),
                       argc > 5 ? atoi(argv[5]) : 0);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--started")) {
        print_started(argc > 2 ? atoi(argv[2]) : 2);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--twocover")) {
        print_twocover(argc > 2 ? atoi(argv[2]) : 2,
                       argc > 3 && !strcmp(argv[3], "one"));
        return 0;
    }
    const int games = argc > 1 ? atoi(argv[1]) : 20;
    const uint32_t seed0 = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 20260716u;

    printf("msg_wire_test: %d games/pc, seed0=%u\n", games, seed0);
    test_sha256_kat();
    test_roundtrip(games, seed0);
    CHECK(g_finished_replays > 0, "the roundtrip replayed no finished game, so its status check proved nothing");
    test_waiting_phase();
    test_name_length_boundary();
    test_rule_p_started_beats_lobby();
    test_endgame_seeds();
    test_rule_p_fuller_start_wins();
    test_rule_p_child_beats_parent();
    test_surface_delta();
    test_tamper();
    test_hostile_body();
    test_pickup_hold();
    test_clock_wire();
    test_podkidnoy_wire();
    test_bubble_delta();
    test_nothing_bubble();
    test_open_boundary();
    test_staged_fate();
    test_roster_key();
    test_chain_gates();
    test_turn_controller();
    test_rematch_opening();
    test_fool_penalty_wire();
    test_forced_opening_replay();
    test_format56_goldens();
    test_format7_wire();
    test_size_budget(games * 4, seed0);
    { const int rb = bot_roster_find("robusta");
      probe_v6_midgame(seed0, 2, rb);
      probe_v6_midgame(seed0, 4, rb); }

    if (g_fails) { printf("msg_wire_test: %d FAILURES\n", g_fails); return 1; }
    printf("msg_wire_test: OK\n");
    return 0;
}
