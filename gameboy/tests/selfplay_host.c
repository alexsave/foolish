// selfplay_host - the clang side of the cross-compiler check: prints the
// sum as the gbrun expect line the ROM must match.
#include "selfplay.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    uint16_t moves, sum = selfplay(SELFPLAY_SEED, SELFPLAY_GAMES, &moves);
    printf("%02x %02x %02x %02x\n", sum & 0xFF, sum >> 8, moves & 0xFF, moves >> 8);
    if (getenv("SELFPLAY_TRACE"))
        for (int i = 0; i < SELFPLAY_LOG; i++) fprintf(stderr, "%02x\n", selfplay_log[i]);
    return 0;
}
