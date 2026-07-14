// ios_view_test.c — proves fio_view_from_packed_json (§16.D4) decodes the SAME
// masked-view wire the server emits back into the app's GameView JSON, matching
// the offline decode. With kernel access here (unlike the smoke harness), it
// builds a game, serializes seat 0's masked view with state_put (the exact wire
// the server's player_views cache stores), decodes it through the bridge, and
// checks the viewer's hand + public scalars match fio_state_json(0) on the same
// seeded game. Build/run via `make ios-view-test`.

#include "ios_api.h"
#include "game.h"
#include "view.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Extract the substring of the "hand":[ ... ] array for the viewer player, and
// the value of a scalar key, so we can compare the two decode paths field-wise
// without a full JSON parser.
static int find_scalar(const char *json, const char *key, char *out, int cap) {
    char pat[48]; snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    int n = 0;
    while (*p && *p != ',' && *p != '}' && n < cap - 1) out[n++] = *p++;
    out[n] = 0;
    return n;
}

// The first "hand":[...] array in the JSON is the viewer seat (players[0], and
// we ask for viewer 0 in both paths).
static int find_first_hand(const char *json, char *out, int cap) {
    const char *p = strstr(json, "\"hand\":[");
    if (!p) return -1;
    p += strlen("\"hand\":");
    int depth = 0, n = 0;
    for (; *p && n < cap - 1; p++) {
        out[n++] = *p;
        if (*p == '[') depth++;
        else if (*p == ']') { if (--depth == 0) break; }
    }
    out[n] = 0;
    return n;
}

int main(void) {
    unsigned char seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 7 + 1);

    // Reference path: the live game via the bridge.
    static char ref[1 << 16];
    if (fio_new_game(seed, 32, 4) != FIO_EOK) { printf("FAIL new_game\n"); return 1; }
    for (int i = 0; i < 5; i++) { char b[4096]; (void)fio_bot_step_json(-1, b, sizeof(b)); }
    if (fio_state_json(0, ref, sizeof(ref)) < 0) { printf("FAIL ref\n"); return 1; }

    // Build an INDEPENDENT identical game with kernel access, drive it the same
    // way, serialize seat 0's masked view, and decode through the bridge.
    game_set_deal_seed_bytes(seed, 32);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = 4;
    for (int i = 0; i < 4; i++) { g.players[i].status = PLAYER_STATUS_READY;
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i); }
    start_game(&g);
    // (No moves applied to `g`; we compare against fio_state_json(0) taken BEFORE
    // bot moves would diverge — so re-take the reference on the fresh deal.)
    fio_new_game(seed, 32, 4);
    fio_state_json(0, ref, sizeof(ref));

    static unsigned char packed[1 << 16];
    int plen = state_put(&g, 0, packed);
    if (plen <= 0) { printf("FAIL state_put\n"); return 1; }

    static char dec[1 << 16];
    int dlen = fio_view_from_packed_json(packed, plen, 0, dec, sizeof(dec));
    if (dlen < 0) { printf("FAIL view_from_packed err=%d\n", dlen); return 1; }

    // Compare the viewer's hand and key public scalars.
    char h1[8192], h2[8192];
    find_first_hand(ref, h1, sizeof(h1));
    find_first_hand(dec, h2, sizeof(h2));
    if (strcmp(h1, h2) != 0) {
        printf("FAIL hand mismatch\n ref=%s\n dec=%s\n", h1, h2); return 1;
    }
    const char *scalars[] = {"numPlayers", "powerSuit", "deckCount", "defender", "firstAttacker"};
    for (int i = 0; i < 5; i++) {
        char a[64], b[64];
        find_scalar(ref, scalars[i], a, sizeof(a));
        find_scalar(dec, scalars[i], b, sizeof(b));
        if (strcmp(a, b) != 0) { printf("FAIL %s: ref=%s dec=%s\n", scalars[i], a, b); return 1; }
    }
    printf("packed-view decode matches offline decode (hand + scalars)\n");

    // Legal moves from the packed view must match the live-game legal moves for
    // the same seat (online enable-states are kernel-driven, §3).
    static char lref[1 << 16], lpk[1 << 16];
    if (fio_legal_moves_json(0, lref, sizeof(lref)) < 0) { printf("FAIL legal ref\n"); return 1; }
    if (fio_legal_from_packed_json(packed, plen, 0, lpk, sizeof(lpk)) < 0) { printf("FAIL legal packed\n"); return 1; }
    if (strcmp(lref, lpk) != 0) {
        printf("FAIL legal mismatch\n ref=%.200s\n pk=%.200s\n", lref, lpk); return 1;
    }
    printf("packed-view legal moves match offline (%zu bytes)\n", strlen(lref));

    printf("VIEW TEST OK\n");
    return 0;
}
