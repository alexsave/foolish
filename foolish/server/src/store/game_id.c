// game_id.c - THE GAME ID RULE (game_id.h).
#include "store/game_id.h"

#include "util/uuid.h"

#include <stdint.h>
#include <string.h>

#define N_ALPHABET ((unsigned)(sizeof CB_GAME_ID_ALPHABET - 1))
_Static_assert(N_ALPHABET >= 2 && N_ALPHABET <= 256, "an alphabet of 2..256 characters");

bool cb_game_id_ok(const char *s, size_t n) {
    if (!s || n != CB_GAME_ID_LEN) return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] == 0 || !memchr(CB_GAME_ID_ALPHABET, s[i], N_ALPHABET)) return false;
    return true;
}

void cb_game_id_random(char out[CB_GAME_ID_MAX + 1]) {
    // Each character is one CSPRNG byte modulo the alphabet's size, and a byte at or past the
    // largest multiple of that size is drawn again, so every character is uniform whatever
    // the alphabet (16 divides 256: nothing is ever drawn again today).
    const unsigned keep = 256u - 256u % N_ALPHABET;
    int at = 0;
    while (at < CB_GAME_ID_LEN) {
        uint8_t raw[CB_GAME_ID_LEN];
        cb_random_bytes(raw, sizeof raw);
        for (size_t i = 0; i < sizeof raw && at < CB_GAME_ID_LEN; i++)
            if (raw[i] < keep) out[at++] = CB_GAME_ID_ALPHABET[raw[i] % N_ALPHABET];
    }
    out[CB_GAME_ID_LEN] = 0;
}
