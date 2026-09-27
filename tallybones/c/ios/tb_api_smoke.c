/* Every bridge entry point, with the host compiler, so an invariant on the
 * bridge is testable without a Mac (`make ios-smoke`, and `make run`). The
 * whole-game flow is tests/tb_twophone_test.c; this is the surface: each
 * function once with good arguments and once with bad ones, and the layout
 * numbers. */
#include "include/tb_api.h"
#include "tb_api_layout.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define OK(c, what) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

static void be(uint8_t id, const char *nick)
{
    uint8_t ident[16];
    memset(ident, id, sizeof ident);
    tb_api_me(ident, 16);
    tb_api_nickname((const uint8_t *)nick, (int)strlen(nick));
    tb_api_seats_load(0, 0);
}

int main(void)
{
    static char text[TB_API_TEXT_MAX], line[256];
    OK(tb_api_layout_hash() == 0, "an unstamped build says 0");
    OK(tb_api_name_verdict((const uint8_t *)"Alex", 4) == TB_NAME_OK, "a name");
    OK(tb_api_name_verdict((const uint8_t *)"", 0) == TB_NAME_EMPTY, "no name");
    OK(tb_api_name_verdict((const uint8_t *)"abcdefghijklmnopq", 17) == TB_NAME_TOO_LONG, "17 characters");

    /* nothing resident */
    be(1, "Alex");
    OK(tb_api_text(text, sizeof text) < 0 && tb_api_view() && tb_api_table(), "reads with nothing resident");
    OK(((const TbApiTable *)tb_api_table())->readable == 0, "not readable");
    OK(tb_api_plan(-1, 0) == 0 && tb_api_plan_draft() == 0 && tb_api_beats(-1, 0, 0) == 0, "no plans");
    OK(tb_api_stage_keep(0) == 0 && tb_api_stage_score(0) == 0 && tb_api_cancel() == 0 && tb_api_mark_sent() == 0,
       "nothing to stage");
    OK(tb_api_read("https://x/?m=AAAA") < 0 && tb_api_check("no link") == TB_ETEXT, "junk does not read");

    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 1);
    OK(tb_api_new(seed, 1) == TB_EOK, "a new lobby");
    OK(tb_api_text(text, sizeof text) > 0 && tb_api_check(text) == TB_EOK, "its link");
    OK(tb_api_seats_dirty() == 1, "a new seat is a new record");
    static uint8_t recs[TB_API_REC_BYTES];
    int rn = tb_api_seats_save(recs, sizeof recs);
    OK(rn == 17 && tb_api_seats_dirty() == 0, "one record saved");
    OK(tb_api_words(TB_API_W_LOBBY_ROW, 0, line, sizeof line) > 0 && !strcmp(line, "1. Alex (You)"), "my row");
    OK(tb_api_words(TB_API_W_PUBLIC_ROW, 0, line, sizeof line) > 0 && !strcmp(line, "1. Alex"), "the public row");
    OK(tb_api_words(TB_API_W_CAPTION, 0, line, sizeof line) == 0, "no game caption in a lobby");
    OK(tb_api_stage_start() == TB_EREFUSED, "alone: no start");

    be(2, "Bo");
    OK(tb_api_read(text) == TB_EOK && ((const TbApiTable *)tb_api_table())->offered == TB_LOBBY_JOIN, "Bo may join");
    static char lobby[TB_API_TEXT_MAX];
    memcpy(lobby, text, sizeof lobby);
    OK(tb_api_stage_join() == 1, "Bo joins");
    const TbApiEvents *le = tb_api_plan_lobby(lobby);
    OK(le && le->n == 1 && le->ev[0].kind == TB_EV_LOBBY_JOIN && le->ev[0].seat == 1, "the roster's events");
    OK(tb_api_stage_start() == TB_EOK, "a full DM: the newest joiner may start");
    OK(tb_api_text(text, sizeof text) > 0, "the start bubble");
    be(1, "Alex");
    tb_api_seats_load(recs, rn);
    OK(tb_api_adopt(text, 0) == TB_EOK, "Alex adopts");
    const TbApiTable *t = tb_api_table();
    OK(t->phase == TB_PHASE_LIVE && t->me == 0 && t->by == TB_BY_RECORD && t->my_turn, "Alex, by the record, on turn");
    OK(t->n_seats == 2 && t->seat[1].name_len == 2 && !memcmp(t->seat[1].name, "Bo", 2), "the roster");
    const TbView *v = tb_api_view();
    OK(v->known == 31 && v->n == 2 && v->seat[0].in && v->rolls_left == 2, "the view");
    /* one process, one slot: the start is already resident here, so the
     * adopt had nothing new to play (tb_twophone_test has the real flow) */
    OK(!tb_api_beats_now(), "the same bubble again: no motion");
    OK(tb_api_beats(-1, 0, TB_BEATS_OPEN) && tb_api_beats_now() && tb_api_beats_serial() > 0, "the start's plan");
    OK(!tb_api_beats(0, 5, TB_BEATS_OPEN) && !tb_api_beats_now(), "a range off the game: the settled view");
    OK(tb_api_beats(-1, 0, TB_BEATS_OPEN) != 0, "the start's plan again");
    const TbBeatFrame *f = tb_api_beats_frame(0);
    OK(f && f->dice[0] == 0, "the frame before the settle");
    OK(tb_api_beat_sample(0, 0, 0) && !tb_api_beat_sample(99, 0, 0), "a sample, and none off the plan");

    OK(tb_api_can_keep(5) && !tb_api_can_keep(31) && !tb_api_can_keep(-1), "can keep");
    OK(tb_api_can_score(TB_C_ANY) && !tb_api_can_score(13), "can score");
    OK(tb_api_score_if(TB_C_ANY) >= 5 && tb_api_score_if(13) == -1, "a preview");
    OK(tb_api_stage_keep(5) == 1 && ((const TbApiTable *)tb_api_table())->staged, "staged");
    OK(tb_api_plan_draft() && ((const TbApiEvents *)tb_api_plan_draft())->n == 2, "the staged plan: KEEP, ROLL");
    OK(tb_api_words(TB_API_W_STAGED_CAPTION, 0, line, sizeof line) > 0, "the staged caption");
    OK(tb_api_words(TB_API_W_HEADLINE, 0, line, sizeof line) > 0 && !strcmp(line, "Send it to reroll"), "the headline");
    OK(tb_api_words(TB_API_W_SPOKEN_DIE, 1, line, sizeof line) > 0 && !strcmp(line, "Die 2, rolling"), "spoken");
    OK(tb_api_stage_leave() == 1 && ((const TbView *)tb_api_view())->pending_kind == TB_M_LEAVE, "a leave replaces it");
    OK(tb_api_stage_score(TB_C_ANY) == 1, "a score replaces that");
    OK(tb_api_text(text, sizeof text) > 0 && tb_api_read(text) == TB_ESTAGED, "my staged bubble is not mine to read");
    OK(tb_api_words(TB_API_W_ERROR, TB_ESTAGED, line, sizeof line) > 0, "and it says why");
    OK(tb_api_mark_sent() == 1 && tb_api_plan(0, 1) && ((const TbApiEvents *)tb_api_plan(0, 1))->n >= 3, "sent");
    OK(tb_api_words(TB_API_W_SUMMARY, 1, line, sizeof line) > 0 && strstr(line, "Bo to roll"), "the summary");
    OK(tb_api_plan(0, 9) == 0, "a range off the game");
    uint8_t rank[8];
    OK(tb_api_ranks(rank) == 2 && rank[0] == 0, "Alex leads");
    OK(tb_api_words(TB_API_W_RANK_ROW, 0, line, sizeof line) > 0, "a rank row");
    OK(tb_api_words(TB_API_W_RANK_ROW, 5, line, sizeof line) == -1, "no sixth place");
    for (int w = 0; w < TB_API_W_COUNT; w++) tb_api_words(w, 0, line, sizeof line);
    OK(tb_api_words(TB_API_W_COUNT, 0, line, sizeof line) == -1 && tb_api_words(0, 0, line, 1) <= 0, "bad words");
    OK(tb_api_string(0, line, sizeof line) > 0 && !strcmp(line, "Tallybones") && tb_api_string(-1, line, 9) == -1, "a key");
    OK(tb_api_words(TB_API_W_CAT, TB_C_TALLYBONES, line, sizeof line) > 0 && !strcmp(line, "Tallybones"), "the category");
    OK(tb_api_words(TB_API_W_ROW_LABEL, TB_ROW_BONUS, line, sizeof line) > 0 && !strcmp(line, "Bonus"), "a row label");
    OK(tb_api_words(TB_API_W_RULE, 0, line, sizeof line) > 0 && tb_api_words(TB_API_W_RULE, 99, line, sizeof line) == -1, "rules");

    /* two messages */
    static char older[TB_API_TEXT_MAX];
    memcpy(older, text, sizeof older);
    OK(tb_api_text(text, sizeof text) > 0, "the resident's link");
    OK(tb_api_prefer(text, lobby) < 0 && tb_api_prefer(lobby, text) > 0, "a started chain beats its lobby");
    OK(tb_api_same_game(text, lobby) == 1 && tb_api_common(text, older) == 1 && tb_api_common("x", text) == -1, "chains");
    OK(tb_api_prefer("junk", text) > 0, "an unreadable one loses");

    /* the layout */
    float x, y, w, h, s;
    OK(tb_lay_collapse(500) == 0.0f && tb_lay_collapse(300) == 1.0f && tb_lay_collapse(390) == 0.5f, "collapse");
    tb_lay_tray(390, 500, 0, &x, &y, &w, &h);
    OK(w <= 390 && x >= 0 && h > 0, "the tray fits");
    OK(tb_lay_die(0, 390, 500, 0, 0, &x, &y, &s) == 0 && s >= TB_LAY_DIE_MIN && s <= TB_LAY_DIE_MAX, "die 0");
    float x4, y4;
    OK(tb_lay_die(4, 390, 500, 0, 1, &x4, &y4, &s) == 0 && x4 > x && y4 == y + TB_LAY_KEEP_LIFT, "die 4, kept");
    OK(tb_lay_die(5, 390, 500, 0, 0, &x, &y, &s) == -1, "no sixth die");
    float fx, fy;
    OK(tb_lay_pip(1, 0, &fx, &fy) == 1 && fx == 0.5f && fy == 0.5f && tb_lay_pip(7, 0, &fx, &fy) == -1, "pips");
    OK(tb_lay_row_cat(TB_ROW_ONES) == TB_C_ONES && tb_lay_row_cat(TB_ROW_BONUS) == -1
       && tb_lay_row_cat(TB_ROW_ANY) == TB_C_ANY && tb_lay_row_cat(TB_ROW_TOTAL) == -1, "rows");
    OK(tb_lay_row_y(TB_ROW_TOTAL, &y) == 0 && y > 0 && tb_lay_card_h() > y && tb_lay_row_y(16, &y) == -1, "row tops");

    printf("tb_api smoke: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
