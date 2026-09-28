/* What a bubble's link holds, through the bridge (ios/include/cn_api.h), for
 * checking a simulator run against the kernel: the dice every seat holds
 * this round (the tests-only CN_API_ALL view), the standing bid, the newest
 * call's reveal and its outcome line. Not a test and not in `make run`; a
 * Debug build of the extension writes the links it stages and sends to the
 * App Group as dev.staged and dev.sent (ChuiniuDev).
 *
 *   make -C chuiniu/c build/cn_link_dump
 *   ./chuiniu/c/build/cn_link_dump "$(cat <group>/dev.sent)"
 *
 * The bridge is compiled in, so this reads what the app's kernel reads. */
#include "../ios/cn_api.c"
#include <stdio.h>

static void dice(const char *label, const uint8_t *d, int n)
{
    printf("%s", label);
    for (int i = 0; i < n; i++) printf(" %d", d[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    static char text[CN_API_TEXT_MAX + 64];
    if (argc > 1) snprintf(text, sizeof text, "%s", argv[1]);
    else if (!fgets(text, sizeof text, stdin)) return 2;
    text[strcspn(text, "\r\n")] = 0;
    int e = cn_api_read(text);
    if (e) { printf("unreadable: %d\n", e); return 1; }
    const CnApiTable *t = (const CnApiTable *)cn_api_table();
    char line[512];
    printf("phase %d  game_phase %d  seats %d  moves %d\n", t->phase, t->game_phase, t->n_seats, t->moves);
    cn_api_words(CN_API_W_STAGED_CAPTION, 0, line, sizeof line);
    printf("caption: %s\n", line);
    const CnView *all = (const CnView *)cn_api_view(CN_API_ALL);
    if (!all) return 0;
    printf("round %d  turn %d  bid %d x %d by %d  dice on the table %d\n",
           all->round, all->turn, all->bid_q, all->bid_f, all->bidder, all->total);
    for (int s = 0; s < t->n_seats; s++) {
        char label[96];
        cn_api_words(CN_API_W_SEAT, s, line, sizeof line);
        snprintf(label, sizeof label, "  now   %-6s (%d):", line, all->dice_n[s]);
        dice(label, all->all + s * CN_START_DICE, all->dice_n[s]);
    }
    if (all->call_seat != CN_SEAT_NONE) {
        printf("newest call: seat %d on %d x %d, %d counted, seat %d lost\n",
               all->call_seat, all->call_q, all->call_f, all->call_count, all->call_loser);
        for (int s = 0; s < t->n_seats; s++) {
            char label[64];
            snprintf(label, sizeof label, "  shown seat %d:", s);
            dice(label, all->shown + s * CN_START_DICE, all->shown_n[s]);
        }
        cn_api_words(CN_API_W_OUTCOME, 0, line, sizeof line);
        printf("outcome: %s\n", line);
    }
    if (all->phase == CN_PH_OVER) printf("winner: seat %d\n", all->winner);
    return 0;
}
