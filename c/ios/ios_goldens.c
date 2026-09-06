// ios_goldens.c — emits ios/Fixtures/goldens.json (§16.A3), a test fixture the
// Swift suite reads. Drives ~20 seeded
// games PURELY through the Swift-facing bridge (fio_*), with a fully
// deterministic "lowest eligible seat plays its first legal move" policy, and
// records the exact bridge output. The Swift EngineGoldenTests (§16.A6) replay
// the same seeds through libfoolish.a and assert byte-equality — the keystone
// that proves the native build is the same engine the goldens came from.
//
// The bridge compiles the SAME game.c/legal.c/view.c as the production wasm
// kernel, so native == wasm by construction (the existing difftests police the
// bitboard/replay mirrors); these goldens pin the bridge's PACKED wire surface
// (state_put + the packed legal move wire) — the bytes the app actually ships.
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

// First packed legal move -> its awire action frame, so the golden is DRIVEN
// through the same packed path the app ships (EngineC.apply -> fio_apply_awire),
// Mirrors Swift MoveWire.encodeAction exactly.
// packed layout (fio_legal_packed): u32 count, then per move
//   type(1) n(1) cards[n] attacks[n]; kinds align with AWIRE_* (attack0..good4).
static int first_move_awire(const unsigned char *packed, int len, unsigned char *out) {
    if (len < 6) return 0;
    int t = packed[4], n = packed[5];
    if (6 + 2 * n > len) return 0;
    const unsigned char *cards = packed + 6, *attacks = packed + 6 + n;
    int o = 0;
    if (t == 3 || t == 4) { out[o++] = (unsigned char)t; out[o++] = 0; return o; } // pickup/good
    out[o++] = (unsigned char)t; out[o++] = (unsigned char)n;
    for (int i = 0; i < n; i++) out[o++] = cards[i];
    if (t == 1) for (int i = 0; i < n; i++) out[o++] = attacks[i];   // cover carries attacks
    return o;
}

int main(void) {
    const int N_PLAYERS = 4;
    const int N_SEEDS = 20;

    printf("{\n  \"generator\": \"c/ios/ios_goldens.c\",\n");
    printf("  \"nPlayers\": %d,\n  \"games\": [\n", N_PLAYERS);

    for (int g = 0; g < N_SEEDS; g++) {
        // Deterministic 32-byte seed from the game index (wide ChaCha deal).
        uint8_t seed[32];
        for (int i = 0; i < 32; i++) seed[i] = (uint8_t)((g + 1) * 131 + i * 17);

        if (fio_new_game(seed, 32, N_PLAYERS) != FIO_EOK) { fprintf(stderr, "new_game failed\n"); return 1; }

        // Deal fingerprint: the initial masked PACKED state for viewer 0
        // (view.c state_put — the wire the app ships).
        int dealLen = fio_state_packed(0, buf, sizeof(buf));
        uint64_t dealHash = fnv1a(1469598103934665603ULL, buf, dealLen);

        // Seat-0 legal menu as the PACKED move wire (pins the enumeration/order).
        int legalLen = fio_legal_packed(0, buf, sizeof(buf));
        uint64_t seat0LegalHash = fnv1a(1469598103934665603ULL, buf, legalLen);

        // Deterministic playthrough: lowest eligible seat plays its first legal
        // move. Roll a hash over every post-move state(0). No bot auto-play (all
        // seats are strategy 0 and we never call fio_bot_step) → fully repeatable.
        uint64_t playHash = 1469598103934665603ULL;
        int steps = 0;
        // A mid-game menu is combinatorial (every legal cover assignment), so it
        // runs to a few hundred KB — the packed legal wire needs room. This is a
        // 1 MB buffer: FIO_ECAP here means the loop breaks and the golden records
        // a TRUNCATED game, which the Swift walk (growing buffer) would then
        // disagree with — so we bail loudly (below) instead. Static: too big for
        // the stack.
        static char legal[1 << 20];
        unsigned char awire[64];
        while (fio_game_over() < 0 && steps < 5000) {
            int mask = fio_actor_mask();
            if (mask <= 0) break;
            int seat = -1;
            for (int s = 0; s < N_PLAYERS; s++) if (mask & (1 << s)) { seat = s; break; }
            if (seat < 0) break;
            // Drive through the PACKED path the app ships. Bail
            // loudly rather than silently freezing a half-played golden.
            int lrc = fio_legal_packed(seat, legal, sizeof(legal));
            if (lrc < 0) { fprintf(stderr, "goldens: legal menu did not fit (rc=%d)\n", lrc); return 1; }
            int al = first_move_awire((const unsigned char *)legal, lrc, awire);
            if (al == 0) break;
            if (fio_apply_awire(seat, awire, al) != FIO_EOK) break;
            int sl = fio_state_packed(0, buf, sizeof(buf));
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
