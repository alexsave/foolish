/* Two phones, one game, end to end through the iOS bridge: a whole
 * two-player game played through the entry points Swift calls
 * (ios/include/tb_api.h), with a KEEP KEEP SCORE turn, a cancel and a
 * restage, and T11 checked where the extension would break it: the staged
 * bubble shows no reroll, reading my own staged link is refused, and the
 * reroll appears at the send echo, the same on both phones.
 *
 * TWO PHONES, ONE RESIDENT SLOT (pickemup/c/tests/pk_twophone_test.c): a
 * phone here is what a device keeps between bubbles (its identity bytes, its
 * nickname, its seat records). Switching phone saves the records of the one
 * put down, loads the other's and ADOPTS the newest link of the thread, as a
 * tap on the bubble does. Every bubble is staged, written with tb_api_text,
 * marked sent (didStartSending) and only then handed over. */
#include "../ios/tb_api.c"
#include "../ios/tb_lay.c"
#include <stdio.h>
#include <stdlib.h>

static int         g_checks, g_fails;
static const char *g_step = "";
#define STEP(name) (g_step = (name))
#define OK(c, ...) do {                                                          \
        g_checks++;                                                              \
        if (!(c)) {                                                              \
            g_fails++;                                                           \
            if (g_fails <= 40) {                                                 \
                fprintf(stderr, "FAIL %s:%d [%s] %s: ", __FILE__, __LINE__,      \
                        g_step, #c);                                             \
                fprintf(stderr, __VA_ARGS__);                                    \
                fputc('\n', stderr);                                             \
            }                                                                    \
        }                                                                        \
    } while (0)

enum { A = 0, B = 1 };
static const char *NICK[2] = { "Alex", "Bo" };
static uint8_t recs[2][TB_API_REC_BYTES];
static int     recn[2];
static int     holding = -1;
static char    tip[TB_API_TEXT_MAX];

/* What each phone's extension last had resident: picking a phone up puts
 * its own board back in the one slot before anything new is opened. */
static char last[2][TB_API_TEXT_MAX];

static void pick_up(int p)
{
    if (holding >= 0) {
        recn[holding] = tb_api_seats_save(recs[holding], TB_API_REC_BYTES);
        if (tb_api_text(last[holding], sizeof last[holding]) <= 0) last[holding][0] = 0;
    }
    tb_api_seats_load(recs[p], recn[p]);
    tb_api_sender(NULL, 0, -1);
    uint8_t id[16];
    for (int k = 0; k < 16; k++) id[k] = (uint8_t)(p * 53 + k * 7 + 1);
    tb_api_me(id, 16);
    tb_api_nickname((const uint8_t *)NICK[p], (int)strlen(NICK[p]));
    holding = p;
    if (last[p][0]) OK(tb_api_read(last[p]) == TB_EOK, "%s's own board comes back", NICK[p]);
}

/* Phone p opens the newest bubble, which it did not send, in a DM. */
static void open_tip(int p)
{
    pick_up(p);
    tb_api_sender(tip, 1, 0);
    OK(tb_api_adopt(tip, 1) == TB_EOK, "%s opens the tip", NICK[p]);
}

static TbView view(void) { return *(const TbView *)tb_api_view(); }
static TbApiTable table(void) { return *(const TbApiTable *)tb_api_table(); }

static void words(int what, int arg, char *out)
{
    int n = tb_api_words(what, arg, out, 256);
    OK(n >= 0, "words %d/%d", what, arg);
}

/* Send what is staged: the link into the thread, then the echo. */
static TbView send_staged(void)
{
    OK(tb_api_text(tip, sizeof tip) > 0, "the staged bubble writes");
    OK(tb_api_mark_sent() == 1, "the send echo");
    OK(!table().staged, "nothing staged after the echo");
    return view();
}

/* The digits of a line, counted. */
static void digits(const char *s, int out[7])
{
    memset(out, 0, 7 * sizeof out[0]);
    for (; *s; s++) if (*s >= '1' && *s <= '6') out[*s - '0']++;
}

/* A staged keep: the view has values only where kept, and the caption names
 * only those (T11.1, through the bridge). */
static void staged_keep_blind(int mask, const TbView *before)
{
    TbView v = view();
    OK(v.draft && v.pending_kind == TB_M_KEEP && v.pending_arg == mask && v.known == mask, "keep %d staged, known %d",
       mask, v.known);
    for (int i = 0; i < 5; i++)
        OK((mask >> i & 1) ? v.dice[i] == before->dice[i] : v.dice[i] == 0, "die %d: %d, no value until it is sent", i,
           v.dice[i]);
    for (int c = 0; c < TB_CATS; c++) OK(v.would[c] == 0 && tb_api_score_if(c) == -1, "no preview on unknown dice");
    char cap[256];
    words(TB_API_W_STAGED_SUMMARY, 0, cap);
    int got[7], want[7] = { 0 };
    digits(cap, got);
    for (int i = 0; i < 5; i++) if (mask >> i & 1) want[before->dice[i]]++;
    OK(!memcmp(got, want, sizeof want), "the staged caption names the kept dice only: %s", cap);
    const TbApiEvents *e = tb_api_plan_draft();
    for (int k = 0; e && k < e->n; k++)
        if (e->ev[k].kind == TB_EV_ROLL)
            for (int i = 0; i < 5; i++) OK(!(e->ev[k].mask >> i & 1) || e->ev[k].dice[i] == 0, "the staged plan rolls blind");
}

static int best_open(const TbView *v)
{
    int best = -1, pts = -1;
    for (int c = 0; c < TB_CATS; c++)
        if (tb_api_can_score(c) && tb_api_score_if(c) > pts) { pts = tb_api_score_if(c); best = c; }
    (void)v;
    return best;
}

static void lobby(void)
{
    STEP("lobby");
    holding = -1;
    last[0][0] = last[1][0] = 0;
    pick_up(A);
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 29 + 3);
    OK(tb_api_new(seed, 1) == TB_EOK, "Alex opens a DM lobby");
    char line[256];
    words(TB_API_W_INVITE, 0, line);
    OK(!strcmp(line, "Alex wants a game of Tallybones. Tap to join"), "%s", line);
    OK(tb_api_text(tip, sizeof tip) > 0, "the invitation");
    pick_up(B);
    tb_api_sender(tip, 1, 0);
    OK(tb_api_adopt(tip, 1) == TB_EOK, "Bo opens it");
    TbApiTable t = table();
    OK(t.phase == TB_PHASE_WAITING && t.me == TB_SEAT_NONE && t.offered == TB_LOBBY_JOIN && t.can_join_start,
       "Bo may join and start");
    OK(tb_api_stage_join_start() == 1, "Bo joins and starts");
    OK(tb_api_text(tip, sizeof tip) > 0, "the start bubble");
    words(TB_API_W_STAGED_CAPTION, 0, line);
    OK(!strcmp(line, "Tallybones is on. Alex to roll"), "the start's caption: %s", line);
    open_tip(A);
    t = table();
    OK(t.phase == TB_PHASE_LIVE && t.me == 0 && t.my_turn && t.bubbles == 0, "Alex is seat 0 and rolls first");
    TbView v = view();
    OK(v.known == 31 && v.roll == 1 && v.rolls_left == 2, "roll 1 is there on opening");
    const TbBeats *b = tb_api_beats_now();
    OK(b && b->n == 2 && b->beat[1].kind == TB_BK_SETTLE && b->beat[1].mask == 31, "opening the start settles five dice");
}

/* Alex's first turn: KEEP, cancel, a different KEEP, cancel, the first again
 * and send; KEEP again; SCORE. */
static void first_turn(void)
{
    STEP("first turn");
    TbView r1 = view(), a, b2;
    OK(tb_api_stage_keep(0x03), "stage keep A (dice 0 and 1)");
    staged_keep_blind(0x03, &r1);
    char staged[TB_API_TEXT_MAX];
    OK(tb_api_text(staged, sizeof staged) > 0, "the staged link");
    OK(tb_api_read(staged) == TB_ESTAGED, "my own staged link does not read");
    OK(tb_api_adopt(staged, 0) == TB_ESTAGED && table().staged, "nor adopt, and it stays staged");
    OK(tb_api_cancel() == 1 && !view().draft, "cancel");
    a = view();
    OK(!memcmp(a.dice, r1.dice, 5) && a.known == 31, "the tray is roll 1 again");
    OK(tb_api_stage_keep(0x07), "stage keep B");
    staged_keep_blind(0x07, &r1);
    OK(tb_api_stage_keep(31) == 0 && view().pending_arg == 0x07, "keeping all five is refused, B stays");
    OK(tb_api_cancel() == 1, "cancel B");
    OK(tb_api_stage_keep(0x03), "restage A");
    staged_keep_blind(0x03, &r1);
    char cap[256];
    words(TB_API_W_STAGED_CAPTION, 0, cap);
    a = send_staged();
    OK(a.known == 31 && !a.draft && a.roll == 2 && a.kept == 0x03, "sent: the reroll is there");
    OK(a.dice[0] == r1.dice[0] && a.dice[1] == r1.dice[1], "kept dice held");
    for (int i = 2; i < 5; i++) OK(a.dice[i] >= 1 && a.dice[i] <= 6, "die %d rolled", i);
    const TbBeats *bt = tb_api_beats_now();
    OK(bt && bt->n == 1 && bt->beat[0].mask == 0x1C && bt->beat[0].start_ms == TB_T_LEAD_LIVE, "the echo settles three dice");
    char c1[256];
    words(TB_API_W_CAPTION, 1, c1);
    OK(!strcmp(c1, cap), "the sent caption is the staged one: %s / %s", c1, cap);
    /* the same again and again */
    OK(!memcmp(view().dice, a.dice, 5), "stable across reads");

    open_tip(B);
    b2 = view();
    OK(!memcmp(b2.dice, a.dice, 5) && b2.roll == 2, "Bo derives the same reroll");
    OK(!tb_api_stage_keep(0) && !tb_api_stage_score(0), "not Bo's turn");
    char line[256];
    words(TB_API_W_HEADLINE, 0, line);
    OK(!strcmp(line, "Waiting on Alex"), "%s", line);
    words(TB_API_W_SUBLINE, 0, line);
    OK(!strcmp(line, "Alex is on roll 2 of 3"), "%s", line);
    bt = tb_api_beats_now();
    OK(bt && bt->n == 1 && bt->beat[0].mask == 0x1C && bt->beat[0].start_ms == TB_T_LEAD_LIVE,
       "Bo sees the same three dice land");

    open_tip(A);
    OK(!memcmp(view().dice, a.dice, 5) && !tb_api_beats_now(), "Alex reopens his own bubble: no motion");
    OK(tb_api_stage_keep(0x01), "second keep");
    a = send_staged();
    OK(a.roll == 3 && a.rolls_left == 0 && !tb_api_can_keep(0), "roll 3: no more rerolls");
    words(TB_API_W_SUBLINE, 0, line);
    OK(!strcmp(line, "Pick a category"), "%s", line);
    int cat = best_open(&a), pts = tb_api_score_if(cat);
    OK(cat >= 0 && tb_api_stage_score(cat), "score the best");
    TbView d = view();
    OK(d.draft && d.seat[0].score[cat] == pts && (d.seat[0].filled >> cat & 1), "staged: on the card");
    OK(d.turn == 0 && d.known == 31 && !memcmp(d.dice, a.dice, 5),
       "staged: the scored dice stay on the tray and the turn stays with Alex (T66)");
    a = send_staged();
    OK(a.turn == 1 && a.known == 31 && (a.seat[0].filled >> cat & 1), "sent: Bo's roll 1 exists");
    bt = tb_api_beats_now();
    OK(bt && bt->n == 3 && bt->beat[0].kind == TB_BK_STAMP && bt->beat[1].kind == TB_BK_TURN
       && bt->beat[2].kind == TB_BK_SETTLE && bt->beat[2].mask == 31, "stamp, turn, five dice");
    open_tip(B);
    b2 = view();
    OK(!memcmp(b2.dice, a.dice, 5) && b2.seat[0].total == a.seat[0].total && table().my_turn, "Bo's turn, the same dice");
    OK(tb_api_common(tip, staged) == 0 || tb_api_common(tip, staged) == 1, "the chains share the start and the keep");
}

static void rest_of_game(void)
{
    STEP("the rest");
    int guard = 0;
    for (;;) {
        TbApiTable t = table();
        if (t.phase == TB_PHASE_FINISHED || guard++ > 400) break;
        OK(t.my_turn, "the phone in hand is on turn (%s)", NICK[holding]);
        TbView v = view();
        int keeps = (int)((v.turns * 7 + holding) % 3);
        for (int k = 0; k < keeps && tb_api_can_keep(0); k++) {
            OK(tb_api_stage_keep(v.dice[0] == 6 ? 1 : 0), "keep");
            send_staged();
            v = view();
        }
        int cat = best_open(&v);
        OK(tb_api_stage_score(cat), "score %d", cat);
        TbView sent = send_staged();
        open_tip(1 - holding);
        TbView got = view();
        OK(!memcmp(&got, &sent, sizeof got), "both phones see one game (bubble %d)", got.bubbles);
    }
    TbView v = view();
    OK(v.over && v.winners, "over, with a winner");
    OK(v.seat[0].filled == TB_FULL_CARD && v.seat[1].filled == TB_FULL_CARD, "both cards full");
    uint8_t rank[8];
    OK(tb_api_ranks(rank) == 2 && v.seat[rank[0]].total >= v.seat[rank[1]].total, "ranked");
    char line[256], want[256];
    words(TB_API_W_CAPTION, v.bubbles, line);
    if (v.seat[0].total == v.seat[1].total) snprintf(want, sizeof want, "Alex and Bo tie at %d", v.seat[0].total);
    else snprintf(want, sizeof want, "%s wins with %d", NICK[rank[0]], v.seat[rank[0]].total);
    OK(!strcmp(line, want), "the last caption: %s, want %s", line, want);
    words(TB_API_W_RANK_ROW, 0, line);
    snprintf(want, sizeof want, "1. %s, %d", NICK[rank[0]], v.seat[rank[0]].total);
    OK(!strcmp(line, want), "%s", line);
    printf("  the game: %d bubbles, %s %d, %s %d\n", v.bubbles, NICK[0], v.seat[0].total, NICK[1], v.seat[1].total);
}

int main(void)
{
    lobby();
    first_turn();
    rest_of_game();
    printf("tb_twophone_test: %d assertions, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
