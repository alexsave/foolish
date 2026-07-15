// ios_goldens.c — emits ios/Fixtures/goldens.json (§16.A3). Drives ~20 seeded
// games PURELY through the Swift-facing bridge (fio_*), with a fully
// deterministic "lowest eligible seat plays its first legal move" policy, and
// records the exact bridge output. The Swift EngineGoldenTests (§16.A6) replay
// the same seeds through libfoolish.a and assert byte-equality — the keystone
// that proves the native build is the same engine the goldens came from.
//
// The bridge compiles the SAME game.c/legal.c/view.c as the production wasm
// kernel, so native == wasm by construction (the existing difftests police the
// bitboard/replay mirrors); these goldens pin the bridge's JSON surface.
//
// Build/run via `make ios-goldens` (writes the file) or run directly to stdout.

#include "ios_api.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static char buf[1 << 16];

// FNV-1a 64-bit over the exact bytes we hashed, so Swift can reproduce it.
static uint64_t fnv1a(uint64_t h, const char *s, int n) {
    for (int i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

// Extract the first move object from a legal-moves array via balanced braces
// (moves nest card objects). Returns 1 and fills `out` (NUL-terminated), or 0.
static int first_move(const char *arr, char *out, int cap) {
    const char *start = strchr(arr, '{');
    if (!start) return 0;
    int depth = 0;
    for (const char *p = start; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) {
            int n = (int)(p - start + 1);
            if (n >= cap) return 0;
            memcpy(out, start, n); out[n] = 0; return 1;
        }
    }
    return 0;
}

int main(void) {
    const int N_PLAYERS = 4;
    const int N_SEEDS = 20;

    printf("{\n  \"generator\": \"cnitro/ios/ios_goldens.c\",\n");
    printf("  \"nPlayers\": %d,\n  \"games\": [\n", N_PLAYERS);

    for (int g = 0; g < N_SEEDS; g++) {
        // Deterministic 32-byte seed from the game index (wide ChaCha deal).
        uint8_t seed[32];
        for (int i = 0; i < 32; i++) seed[i] = (uint8_t)((g + 1) * 131 + i * 17);

        if (fio_new_game(seed, 32, N_PLAYERS) != FIO_EOK) { fprintf(stderr, "new_game failed\n"); return 1; }

        // Deal fingerprint: the initial masked state for viewer 0.
        int dealLen = fio_state_json(0, buf, sizeof(buf));
        uint64_t dealHash = fnv1a(1469598103934665603ULL, buf, dealLen);

        // First-attacker legal menu (pins the legal enumeration/ordering).
        // Parse defender/firstAttacker out of the state we just printed is
        // awkward in C; instead capture seat 0's menu and a full-play hash.
        int legalLen = fio_legal_moves_json(0, buf, sizeof(buf));
        uint64_t seat0LegalHash = fnv1a(1469598103934665603ULL, buf, legalLen);

        // Deterministic playthrough: lowest eligible seat plays its first legal
        // move. Roll a hash over every post-move state(0). No bot auto-play (all
        // seats are strategy 0 and we never call fio_bot_step) → fully repeatable.
        uint64_t playHash = 1469598103934665603ULL;
        int steps = 0;
        // A mid-game menu is combinatorial (every legal cover assignment), so it
        // runs to a few hundred KB — comfortably past the 64KB this used to
        // give it. fio_legal_moves_json then returned FIO_ECAP, the loop broke,
        // and the golden recorded a TRUNCATED game: every "fool": -1 in the
        // fixture is a game that never actually finished, frozen at the step
        // where the menu first outgrew the buffer. EngineGoldenTests drives the
        // same walk from Swift with a growing buffer, so it played on and
        // disagreed with the fixture on every count — the keystone Swift-vs-C
        // test could not have passed. Static: too big for the stack.
        static char move[4096], legal[1 << 20];
        while (fio_game_over() < 0 && steps < 5000) {
            int mask = fio_actor_mask();
            if (mask <= 0) break;
            int seat = -1;
            for (int s = 0; s < N_PLAYERS; s++) if (mask & (1 << s)) { seat = s; break; }
            if (seat < 0) break;
            // Bail loudly rather than silently freezing a half-played golden.
            int lrc = fio_legal_moves_json(seat, legal, sizeof(legal));
            if (lrc < 0) { fprintf(stderr, "goldens: legal menu did not fit (rc=%d)\n", lrc); return 1; }
            if (!first_move(legal, move, sizeof(move))) break;
            if (fio_apply_json(seat, move) != FIO_EOK) break;
            int sl = fio_state_json(0, buf, sizeof(buf));
            playHash = fnv1a(playHash, buf, sl);
            steps++;
        }
        int fool = fio_game_over();

        printf("    {\n");
        printf("      \"seedByte0\": %d,\n", seed[0]);
        printf("      \"dealHash\": \"%016llx\",\n", (unsigned long long)dealHash);
        printf("      \"seat0LegalHash\": \"%016llx\",\n", (unsigned long long)seat0LegalHash);
        printf("      \"steps\": %d,\n", steps);
        printf("      \"fool\": %d,\n", fool);
        printf("      \"playHash\": \"%016llx\"\n", (unsigned long long)playHash);
        printf("    }%s\n", g == N_SEEDS - 1 ? "" : ",");
    }
    printf("  ]\n}\n");
    return 0;
}
