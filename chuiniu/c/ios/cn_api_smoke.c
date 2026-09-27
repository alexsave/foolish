/* The bridge, proved WITHOUT a Mac: every entry point Swift will call, run
 * against a real game passed between simulated phones as the link text
 * Messages carries, so a broken boundary fails here rather than in Xcode.
 *
 *     make -C chuiniu/c ios-smoke
 *
 * It reads the returned structs through cn_api_layout.h because it is C; the
 * Swift host reads the same pointers through the generated readers
 * (cn_api_smoke.swift). */
#include "include/cn_api.h"
#include "cn_api_layout.h"
#include "../i18n/keys.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

static uint8_t recs[3][CN_API_REC_BYTES];
static int     recn[3];
static int     who = -1;
static const char *NICK[3] = { "Alex", "Bo", "Cy" };
static char link[CN_API_TEXT_MAX], prev[CN_API_TEXT_MAX], line[512];

static void be(int i)
{
    if (who >= 0) recn[who] = cn_api_seats_save(recs[who], CN_API_REC_BYTES);
    cn_api_seats_load(recs[i], recn[i]);
    cn_api_sender(NULL, 0, -1);
    uint8_t id[16];
    for (int k = 0; k < 16; k++) id[k] = (uint8_t)(i * 37 + k);
    cn_api_me(id, 16);
    cn_api_nickname((const uint8_t *)NICK[i], (int)strlen(NICK[i]));
    who = i;
}

static const CnApiTable *table(void) { return (const CnApiTable *)cn_api_table(); }

static void send(void)
{
    memcpy(prev, link, sizeof prev);
    OK(cn_api_text(link, sizeof link) > 0, "a link");
    cn_api_commit();
}

int main(void)
{
    OK(cn_api_layout_hash() == 0, "an unstamped build reports hash 0");
    OK(cn_api_name_verdict((const uint8_t *)"Alex", 4) == CN_NAME_OK, "a name");
    OK(cn_api_name_verdict((const uint8_t *)"", 0) == CN_NAME_EMPTY, "no name");
    OK(cn_api_name_verdict((const uint8_t *)"abcdefghijklmnopq", 17) == CN_NAME_TOO_LONG, "17 characters");
    OK(cn_api_table() && !table()->readable, "nothing resident");
    OK(cn_api_view(CN_API_ME) == 0 && cn_api_text(link, sizeof link) < 0, "no game, no view, no link");

    /* the lobby, in a DM: Alex makes it, Bo joins and starts */
    be(0);
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(200 - i);
    OK(cn_api_new(seed, 1) == CN_EOK && table()->me == 0 && table()->dm, "a DM lobby");
    OK(cn_api_seats_dirty() == 1, "the seat is recorded");
    OK(cn_api_words(CN_API_W_STAGED_CAPTION, 0, line, sizeof line) > 0 && !strcmp(line, "Alex wants a game of Chui Niu. Tap to join"), line);
    OK(cn_api_words(CN_API_W_HEADLINE, 0, line, sizeof line) > 0 && !strcmp(line, "Waiting for players"), line);
    send();
    be(1);
    /* ONE SLOT: this process still holds Alex's lobby; a check leaves it */
    OK(table()->me == 0 && cn_api_check(link) == CN_EOK && table()->me == 0, "check does not adopt");
    cn_api_sender(link, 1, 0);
    OK(cn_api_read(link) == CN_EOK && table()->offered == CN_LOBBY_JOIN && table()->can_join_start, "Bo may join and start");
    OK(cn_api_join_start() == 1 && table()->phase == CN_PHASE_LIVE && table()->starter == 1, "Bo joins and starts");
    OK(cn_api_words(CN_API_W_STAGED_CAPTION, 0, line, sizeof line) > 0 && !strcmp(line, "Dice rolled. Alex bids first"), line);
    send();

    /* Alex bids */
    be(0);
    OK(cn_api_read(prev) == CN_EOK && table()->phase == CN_PHASE_WAITING, "Alex's phone still shows its lobby");
    cn_api_sender(link, 1, 0);
    OK(cn_api_adopt(link) == CN_EOK && table()->me == 0, "Alex adopts the start");
    const CnBeats *b = (const CnBeats *)cn_api_beats_now();
    OK(b && b->n == 1 && b->beat[0].kind == CN_BK_SHAKE, "opened cold on the start: the cups shake");
    const CnBeatFrame *f = (const CnBeatFrame *)cn_api_beats_frame(0);
    OK(f && f->shaking && !f->done, "shaking at 0");
    f = (const CnBeatFrame *)cn_api_beats_frame(b ? b->total_ms : 0);
    OK(f && f->done && !f->shaking, "settled at the end");
    const CnView *v = (const CnView *)cn_api_view(CN_API_ME);
    OK(v && v->my_n == 5 && v->my_turn && v->can_raise && !v->can_call && v->min_q == 1 && v->min_f == 2, "Alex's menu");
    OK(cn_api_can_raise(1, 2) && !cn_api_can_raise(1, 1) && !cn_api_can_raise(11, 2), "can_raise");
    OK(cn_api_call() == 0, "no call on the opening");
    OK(cn_api_raise(2, 5) == 1 && table()->staged == CN_API_STAGED_BID && table()->staged_q == 2, "Alex stages two 5s");
    const CnApiEvents *e = (const CnApiEvents *)cn_api_plan_staged();
    OK(e && e->n == 1 && e->ev[0].kind == CN_EV_BID, "the staged plan");
    b = (const CnBeats *)cn_api_beats_staged();
    OK(b && b->n == 1 && b->beat[0].kind == CN_BK_BID, "the staged beats");
    OK(cn_api_words(CN_API_W_HEADLINE, 0, line, sizeof line) > 0 && !strcmp(line, "Send to bid two 5s"), line);
    OK(cn_api_cancel() == 1 && table()->staged == CN_API_STAGED_NONE, "cancelled");
    OK(cn_api_raise(2, 5) == 1, "staged again");
    send();
    OK(table()->moves == 1 && table()->staged == CN_API_STAGED_NONE, "committed");
    OK(cn_api_words(CN_API_W_CAPTION, 1, line, sizeof line) > 0 && !strcmp(line, "Alex bid two 5s"), line);

    /* Bo calls */
    be(1);
    OK(cn_api_adopt(link) == CN_EOK && table()->me == 1, "Bo adopts");
    OK(cn_api_words(CN_API_W_SUBLINE, 0, line, sizeof line) > 0 && !strcmp(line, "Bid to beat: two 5s by Alex"), line);
    OK(cn_api_words(CN_API_W_TABLE, 0, line, sizeof line) > 0 && !strcmp(line, "10 dice on the table"), line);
    OK(cn_api_call() == 1 && table()->staged == CN_API_STAGED_CALL, "Bo stages the call");
    OK(cn_api_words(CN_API_W_STAGED_CAPTION, 0, line, sizeof line) > 0 && !strcmp(line, "Bo calls two 5s"), line);
    send();
    v = (const CnView *)cn_api_view(CN_API_ME);
    OK(v->revealed && v->call_seat == 1 && v->shown_n[0] == 5, "revealed");
    OK(cn_api_words(CN_API_W_OUTCOME, 0, line, sizeof line) > 0 && !strncmp(line, "Bo calls. Two 5s was ", 21), line);
    OK(cn_api_words(CN_API_W_REVEAL_COUNT, 0, line, sizeof line) > 0 && !strncmp(line, "There were ", 11), line);
    e = (const CnApiEvents *)cn_api_plan(1, 2);
    OK(e && e->n == 4 && e->ev[0].kind == CN_EV_CALL && e->ev[1].kind == CN_EV_REVEAL && e->ev[3].kind == CN_EV_ROUND, "the call's plan");
    b = (const CnBeats *)cn_api_beats(1, 2);
    OK(b && b->n == 5 && b->beat[4].kind == CN_BK_SHAKE, "CALL, LIFT, COUNT, DROP, SHAKE");
    OK(cn_api_plan(-1, 99) == 0, "a bad range");

    /* two messages */
    OK(cn_api_prefer(prev, link) > 0 && cn_api_prefer(link, prev) < 0 && cn_api_prefer(link, link) == 0, "prefer");
    OK(cn_api_prefer("junk", link) > 0 && cn_api_prefer(link, "junk") < 0, "an unreadable one loses");
    OK(cn_api_same_game(prev, link) == 1 && cn_api_common(prev, link) == 1, "same game, one common move");
    OK(cn_api_common("junk", link) == -1, "common of junk");

    /* the words by key and by what */
    OK(cn_api_string(CN_K_GAME_NAME, line, sizeof line) > 0 && !strcmp(line, "Chui Niu"), "GAME_NAME");
    OK(cn_api_string(CN_K_COUNT, line, sizeof line) == -1, "off the table");
    for (int w = 0; w < CN_API_W_COUNT; w++) {
        int arg = w == CN_API_W_BID ? 4 * 8 + 3 : w == CN_API_W_ERROR ? CN_ECHECK : w == CN_API_W_DICE_N ? 3 : w == CN_API_W_CAPTION ? 1 : 0;
        int n = cn_api_words(w, arg, line, sizeof line);
        OK(n >= 0, "every word answers");
    }
    OK(cn_api_words(CN_API_W_BID, 4 * 8 + 3, line, sizeof line) > 0 && !strcmp(line, "four 3s"), line);
    OK(cn_api_words(CN_API_W_COUNT, 0, line, sizeof line) == -1, "past the list");

    /* the records */
    uint8_t saved[CN_API_REC_BYTES];
    int n = cn_api_seats_save(saved, sizeof saved);
    OK(n > 0 && n % 17 == 0 && !cn_api_seats_dirty(), "saved");
    OK(cn_api_seats_save(saved, 3) == -1, "a small buffer");

    /* a leave in a new group lobby */
    be(0);
    cn_api_new(seed, 0);
    send();
    be(2);
    cn_api_read(link);
    OK(cn_api_join() == 1, "Cy joins");
    OK(cn_api_words(CN_API_W_LEFT, 1, line, sizeof line) > 0 && !strcmp(line, "Cy left"), line);
    OK(cn_api_leave() == CN_EOK && table()->me == CN_SEAT_NONE && table()->n_seats == 1, "Cy leaves");
    OK(cn_api_start() == CN_EREFUSED, "not seated: no start");

    printf("ios smoke: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
