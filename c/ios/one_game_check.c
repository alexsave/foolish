// One resident game across the core/bots boundary.
//
// The bridge keeps a single static Game (c/ios/ios_api.c) and hands it to the
// bot half through fio_resident_game (ios_internal.h). Splitting the C into two
// libraries puts that promise at the mercy of the linker: if libfoolishbots.a
// ever pulled its own copy of ios_api.o - which is exactly what happens if the
// core library is on the bots link line, or if a bot file starts naming a core
// symbol the bots archive also defines - there would be TWO resident games. The
// app would deal a board, the bots would drive a different empty one, and
// nothing would crash. It would just play nonsense.
//
// So it is checked rather than assumed: deal through the core, drive through
// the bots, read the board back through the core.
#include "ios_api.h"
#include "ios_bots_api.h"
#include <stdio.h>

int main(void) {
    unsigned char seed[32] = { 7 };
    if (fio_new_game(seed, 32, 2) != 0)          { puts("FAIL new_game"); return 1; }
    if (fio_set_seat_strategy(1, 0) != 0)        { puts("FAIL set_seat_strategy"); return 1; }

    char drv[4096];
    const int rc = fio_bot_drive_packed(1 /* seat 0 is the human */, drv, sizeof drv);
    if (rc == -2) {   // FIO_ENOGAME
        puts("FAIL: the bot half has its OWN empty game - two copies of ios_api.o");
        return 1;
    }
    if (rc < 0) { printf("FAIL: bot drive error %d\n", rc); return 1; }

    char st[4096];
    const int n = fio_state_packed(0, st, sizeof st);
    if (n <= 0) { printf("FAIL: no board after the drive (%d)\n", n); return 1; }

    printf("one resident game ok (drive wrote %d B, board reads %d B)\n", rc, n);
    return 0;
}
