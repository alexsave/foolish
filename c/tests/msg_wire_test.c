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
#include "../src/sha256.h"
#include "../src/replay.h"
#include "../wasm/wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void env_init(MsgEnvelope *e, const uint8_t *seed, int n_players) {
    memset(e, 0, sizeof(*e));
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
        e->joins[i].name_len = 4;
        memcpy(e->joins[i].name, "Ann\0", 4);
        e->joins[i].name[3] = (char)('0' + i);
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
static int play_game(const uint8_t *seed, int n_players, int max_actions,
                     Chain *ch, Game *end, int bot) {
    // Pin the strategies' RNG per game: it is process-global, so without this a
    // measurement would depend on what ran before it (the 4p size moved by ~10%
    // just from adding an 8p sample ahead of it).
    random_strategy_set_seed(((uint32_t)seed[0] << 24) | ((uint32_t)seed[1] << 16) |
                             ((uint32_t)seed[2] << 8) | (uint32_t)seed[3] | 1u);
    game_set_deal_seed_bytes(seed, MSG_SEED_LEN);
    Game g;
    memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = 0;
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
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
            const int over = game_done(&played) >= 0 || played.status == GAME_STATUS_GAME_OVER;
            e.phase = over ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE;
            static unsigned char body[1024];
            static Game scratch;
            const int src = msg_seal(&e, &played, body, sizeof(body), &scratch);
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
        CHECK(msg_seal(&e, &played, body, sizeof(body), &scratch) == MSG_EOK,
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
        CHECK(msg_seal(&e, &played, body, sizeof(body), &scratch) == MSG_EOK,
              "name boundary tamper fixture seal failed");
        unsigned char wire[ENV_CAP];
        const int n = msg_encode(&e, wire, sizeof(wire));
        CHECK(n > 0, "name boundary tamper fixture encode failed");
        if (n > 0) {
            wire[MSG_HEADER_LEN + 1] = MSG_MAX_NAME + 1;   // the first join's name_len byte
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
    CHECK(msg_seal(&e, &played, body, sizeof(body), &scratch) == MSG_EOK, "tamper seal failed");

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
    struct { const char *what; int off; unsigned char val; int want; } cases[] = {
        { "magic",        0,  0xF6, MSG_EMAGIC },
        { "format",       1,  MSG_FORMAT_V6 + 1, MSG_EFORMAT },
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
        { "variant",      16, 1,    MSG_EVARIANT },
        { "actor seat",   14, 4,    MSG_ESEAT },
        { "n_joins:0",    58, 0,    MSG_EJOINS },
        { "n_joins:9",    58, 9,    MSG_EJOINS },
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
        t[MSG_HEADER_LEN + 2] = 0x01; // first byte of the first join's name
        MsgEnvelope d;
        CHECK(msg_decode(t, n, &d) == MSG_ENAME, "control byte in name accepted");
    }
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
    CHECK(msg_seal(&e, &played, body, sizeof(body), &scratch) == MSG_EOK, "seal failed");

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
    if (msg_seal(&e2, &played2, body2, sizeof(body2), &scratch) == MSG_EOK) {
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
        if (msg_seal(&e, &played, body, sizeof(body), &scratch) != MSG_EOK) continue;
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
                snprintf(rg.players[i].player_id, sizeof(rg.players[i].player_id), "p%d", i);
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
static void print_fatboard(int target, int np) {
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
        for (int i = 0; i < np; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = 0;
            snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
        }
        start_game(&g);

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

        MsgEnvelope e;
        env_init(&e, seed, np);
        e.phase = MSG_PHASE_LIVE;
        e.last_actor_seat = (uint8_t)g.defender;
        if (msg_seal(&e, &g, body, sizeof(body), &scratch) != MSG_EOK) continue;
        unsigned char wire[ENV_CAP];
        const int n = msg_encode(&e, wire, sizeof(wire));
        if (n <= 0) continue;

        fprintf(stderr, "fatboard: %dp seed#%u  %d cards on table (%d covered), "
                        "defender=seat %d holds %d, turn %d round %d, %d bytes\n",
                np, s, on_table, covered, g.defender,
                g.players[g.defender].hand_count, e.turn, e.round, n);
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
                if (msg_seal(&e, &played, body, sizeof(body), &scratch) != MSG_EOK) continue;
                unsigned char wire[ENV_CAP];
                const int n = msg_encode(&e, wire, sizeof(wire));
                if (n <= 0) continue;
                printf("%d %d %d ", np, e.turn, e.round);
                for (int i = 0; i < n; i++) printf("%02x", wire[i]);
                printf("\n");
                emitted = 1;
            }
        }
        if (!emitted) fprintf(stderr, "no %dp fixture posed the race\n", np);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--fixture")) { print_fixtures(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--fatboard")) {
        print_fatboard(argc > 2 ? atoi(argv[2]) : 10, argc > 3 ? atoi(argv[3]) : 2);
        return 0;
    }
    const int games = argc > 1 ? atoi(argv[1]) : 20;
    const uint32_t seed0 = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 20260716u;

    printf("msg_wire_test: %d games/pc, seed0=%u\n", games, seed0);
    test_sha256_kat();
    test_roundtrip(games, seed0);
    test_waiting_phase();
    test_name_length_boundary();
    test_rule_p_started_beats_lobby();
    test_rule_p_fuller_start_wins();
    test_tamper();
    test_hostile_body();
    test_size_budget(games * 4, seed0);
    { const int rb = bot_roster_find("robusta");
      probe_v6_midgame(seed0, 2, rb);
      probe_v6_midgame(seed0, 4, rb); }

    if (g_fails) { printf("msg_wire_test: %d FAILURES\n", g_fails); return 1; }
    printf("msg_wire_test: OK\n");
    return 0;
}
