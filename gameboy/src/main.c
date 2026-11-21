// Durak for the Game Boy: you (seat 0) against a bot (seat 1) that plays a
// uniformly random legal move. Rules: gd_rules.c. What each button does on a
// board: gd_play.c and up() below, a port of the website's keyboard mode
// (foolish src/components/GameDisplay/KeyboardPlayMode.tsx). README.md has the
// controls table.
#include <gb/gb.h>
#include <stdint.h>
#include <string.h>

#include "gd_rules.h"
#include "gd_play.h"
#include "gd_rng.h"
#include "gfx.h"
#include "input.h"
#include "art_tiles.h"

// THE GAME'S ONE ENTROPY SOURCE: the frame count when a button is first
// pressed on the home screen seeds the one RNG (gd_rng.c), and every deal and
// every bot move after it is a draw from that stream. Make this a nonzero
// constant and every run deals the same games.
#define GD_SEED_FIXED 0

#define HUMAN 0
#define BOT   1
// The bot moves once the board has been still this long (frames; 60 = 1 s).
#define BOT_DELAY    30
// How long a bot move's message ("BOT TAKES") stays on the status line.
#define FLASH_FRAMES 90

#define TGT_PASS 0xFEu

// The game and the hand state are not static so tools/gbrun scripts can read
// them by name (peek _G, expect _cur ...).
gd_game G;
gd_rep  R;
static uint8_t live;            // a game has been dealt
static uint8_t seeded;

// ---- the hand as shown: the website's sticky arrangement memory --------------------
// (foolish src/state/clientReconcile.ts reconcileHandMemory / displayedHand):
// every card ever held keeps its slot, so a card that comes back (a pickup)
// returns to where it was; new cards join at the end in the order they arrive.
static uint8_t mem[GD_NCARDS], mem_n;
static gd_mask memm;            // the cards in mem[], as a set
uint8_t disp[GD_NCARDS], disp_n;
int8_t  cur = -1;        // the cursor, an index into disp; -1 none
gd_mask selm;            // the selection (the website's selectedCards)
uint8_t in_target;       // the cover/pass TARGET mode
uint8_t tgt[GD_MAXB + 1], tgt_n, tgt_i;
static uint8_t view_off;        // first shown card when the hand is wider than the screen

static uint8_t idle;            // frames since the board last changed
static uint8_t flash_t;
static const char *flash_msg;
static uint8_t dirty;          // redraw the board
static uint8_t dirty_ui;       // redraw the foot rail and the sprites only
#define SCREEN_DASH  0
#define SCREEN_TABLE 1
#define SCREEN_END   2      // the win or lose picture
uint8_t screen = SCREEN_DASH, dash_item;
// The bots on the dashboard. Only the random brain exists so far, so every
// city plays it; the city is the name the board shows (README.md "Not done").
static const char *const bot_names[4] = { "MIAMI", "NEW YORK", "SEOUL", "WARSAW" };
uint8_t bot_city;
// The finished table stays up this long before the win or lose picture.
#define END_DELAY 90
static uint8_t end_t;
static char flash_buf[21];

// How long the bot's last decision took, in scanlines (1 line = 108.7 us):
// count + unrank + apply. Read by tools/gbrun (peek _bot_lines_max).
uint16_t bot_lines_last, bot_lines_max;
uint32_t bot_lines_sum;
uint16_t bot_moves;

// Scanlines since an arbitrary start, for timing: sys_time counts VBlanks,
// which begin at line 144, so the line is counted from there too.
static uint16_t line_clock(void) {
    uint16_t t;
    uint8_t ly;
    CRITICAL { t = sys_time; ly = LY_REG; }
    // A VBlank that raised its interrupt but has not run it yet: count it.
    if (ly >= 144 && (IF_REG & VBL_IFLAG)) t++;
    return (uint16_t)(t * 154u + (ly >= 144 ? ly - 144u : ly + 10u));
}
// The same for one redraw of the table (board, rails, flush and sprites).
uint16_t draw_lines_last, draw_lines_max;

static void rebuild_disp(void) {
    uint8_t i;
    disp_n = 0;
    for (i = 0; i < mem_n; i++) if (GD_HAS(G.hand[HUMAN], mem[i])) disp[disp_n++] = mem[i];
    for (i = 0; i < 5; i++) selm.b[i] &= G.hand[HUMAN].b[i];
    // The website's clamp: keep the index, pull it back to the last card if
    // what it sat on left the end, and drop it once the hand is empty.
    if (cur >= (int8_t)disp_n) cur = disp_n ? (int8_t)(disp_n - 1u) : -1;
}

// Where a card arrived from, for the order new cards join: the table (in
// battle order, a pickup), then the stock (in draw order), then the trump card.
static uint8_t arrival(const gd_game *b, uint8_t c) {
    uint8_t i;
    for (i = 0; i < b->nb; i++) {
        if (b->att[i] == c) return (uint8_t)(2u * i);
        if (b->def[i] == c) return (uint8_t)(2u * i + 1u);
    }
    for (i = 0; i < b->stock_n; i++) if (b->stock[i] == c) return (uint8_t)(40u + i);
    return 100;
}

static void note_new_cards(const gd_game *b) {
    uint8_t c, best, best_c, any;
    do {
        any = 0; best = 255; best_c = 0;
        for (c = 0; c < GD_NCARDS; c++)
            if (GD_HAS(G.hand[HUMAN], c) && !GD_HAS(memm, c)) {
                const uint8_t a = arrival(b, c);
                if (!any || a < best) { best = a; best_c = c; any = 1; }
            }
        if (any) { mem[mem_n++] = best_c; GD_SET(memm, best_c); }
    } while (any);
    rebuild_disp();
}

static void flash(const char *s) { flash_msg = s; flash_t = FLASH_FRAMES; }

// "<CITY> <verb>" on the foot rail.
static void flash_bot(const char *verb) {
    uint8_t n = 0;
    const char *p = bot_names[bot_city];
    while (*p && n < 20) flash_buf[n++] = *p++;
    if (n < 20) flash_buf[n++] = ' ';
    while (*verb && n < 20) flash_buf[n++] = *verb++;
    flash_buf[n] = 0;
    flash(flash_buf);
}

static gd_game before;

// Apply a move for a seat; 1 if it was legal.
static uint8_t play(uint8_t seat, const gd_move *m) {
    before = G;
    if (!gd_apply(&G, &R, seat, m)) return 0;
    note_new_cards(&before);
    idle = 0;
    dirty = 1;
    // The website's afterMove leaves target mode after my own move. A bot move
    // leaves it only when the table changed under the aim: a silent good (the
    // bot stops adding) changes nothing there.
    if (seat == HUMAN || G.nb != before.nb || memcmp(G.def, before.def, GD_MAXB) || memcmp(G.att, before.att, GD_MAXB))
        in_target = 0;
    if (seat == BOT) {
        switch (m->type) {
        case GD_ATTACK: flash_bot(before.nb ? "ADDS" : "ATTACKS"); break;
        case GD_COVER:  flash_bot("COVERS"); break;
        case GD_PASS:   flash_bot("PASSES"); break;
        case GD_PICKUP: flash_bot("TAKES"); break;
        case GD_GOOD:
            // A good over an uncovered attack is a silent one (foolish
            // game_shown_good_mask): the bot just stops adding. Not shown.
            if (before.nb && !gd_uncovered(&before)) flash_bot("IS DONE");
            break;
        }
    } else {
        flash_t = 0;    // my move replaces whatever the bot's last one said
        if (m->type == GD_PICKUP) flash("YOU TAKE");
    }
    return 1;
}

static void new_game(void) {
    uint8_t deck[GD_NCARDS], i, j, t;
    for (i = 0; i < GD_NCARDS; i++) deck[i] = i;
    for (i = GD_NCARDS - 1u; i > 0; i--) {
        j = (uint8_t)gd_rng_below((uint32_t)i + 1u);
        t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    // Drawn on every deal, used only when nobody holds a trump.
    t = (uint8_t)gd_rng_below(2);
    gd_deal(&G, &R, deck, t);
    mem_n = 0;
    for (i = 0; i < 5; i++) memm.b[i] = 0;
    for (i = 0; i < GD_NCARDS; i++) if (GD_HAS(G.hand[HUMAN], i)) { mem[mem_n++] = i; GD_SET(memm, i); }
    for (i = 0; i < 5; i++) selm.b[i] = 0;
    cur = -1;
    in_target = 0;
    view_off = 0;
    idle = 0;
    flash_t = 0;
    live = 1;
    end_t = 0;
    dirty = 1;
    rebuild_disp();
}

// ---- the selection --------------------------------------------------------------
static uint8_t sel_list(uint8_t *out) {
    uint8_t i, n = 0;
    // in the order shown
    for (i = 0; i < disp_n; i++) if (GD_HAS(selm, disp[i])) out[n++] = disp[i];
    return n;
}

static void toggle(uint8_t c) {
    if (GD_HAS(selm, c)) GD_CLR(selm, c); else GD_SET(selm, c);
}

static void clear_sel(void) {
    uint8_t i;
    for (i = 0; i < 5; i++) selm.b[i] = 0;
}

// ---- the moves a button makes -------------------------------------------------------
static gd_move mv;

static uint8_t fire(uint8_t type, const uint8_t *cards, uint8_t n) {
    uint8_t i;
    mv.type = type;
    mv.n = n;
    for (i = 0; i < n; i++) mv.cards[i] = cards[i];
    return play(HUMAN, &mv);
}

// DOWN: the Take or the Done button, whichever the board shows for the
// selection, so neither while cards are selected.
static void take_or_good(void) {
    uint8_t s[GD_NCARDS];
    const uint8_t n = sel_list(s);
    const uint8_t pills = gd_pills(&G, HUMAN, s, n);
    if (pills & GD_PILL_PICKUP) fire(GD_PICKUP, 0, 0);
    else if (pills & GD_PILL_GOOD) fire(GD_GOOD, 0, 0);
}

// UP: play. Ported line for line from KeyboardPlayMode's ArrowUp.
static void up(void) {
    uint8_t s[GD_NCARDS], n, card, pills, i, nc = 0, pass_ok;
    uint32_t cov;
    if (in_target) {
        if (cur < 0) { in_target = 0; return; }
        card = disp[(uint8_t)cur];
        if (tgt[tgt_i] == TGT_PASS) fire(GD_PASS, &card, 1);
        else { mv.atts[0] = G.att[tgt[tgt_i]]; fire(GD_COVER, &card, 1); }
        in_target = 0;
        dirty = 1;
        return;
    }
    if (cur < 0) return;
    if (!disp_n) { cur = -1; return; }
    card = disp[(uint8_t)cur];
    n = sel_list(s);
    if (n > 0) {
        // Commit a move from the SET, like the website's buttons.
        pills = gd_pills(&G, HUMAN, s, n);
        if (G.defender != HUMAN) {
            if ((pills & GD_PILL_ATTACK) && fire(GD_ATTACK, s, n)) clear_sel();
            return;
        }
        {
            const int8_t b = gd_best_cover(&G, HUMAN, s, n);
            if (b >= 0 && gd_resolve_cover(&G, HUMAN, s, n, (uint8_t)b, &mv)) {
                if (play(HUMAN, &mv)) clear_sel();
                return;
            }
        }
        if ((pills & GD_PILL_PASS) && fire(GD_PASS, s, n)) clear_sel();
        return;   // an ambiguous or illegal set: nothing, the selection stays
    }
    pills = gd_pills(&G, HUMAN, &card, 1);
    if (G.defender != HUMAN) {
        if (pills & GD_PILL_ATTACK) fire(GD_ATTACK, &card, 1);
        return;
    }
    cov = gd_coverable(&G, HUMAN, &card, 1);
    pass_ok = (pills & GD_PILL_PASS) != 0;
    tgt_n = 0;
    for (i = 0; i < G.nb; i++) if (cov & ((uint32_t)1u << i)) { tgt[tgt_n++] = i; nc++; }
    if (nc == 0 && pass_ok) { fire(GD_PASS, &card, 1); return; }
    if (nc == 1 && !pass_ok) { mv.atts[0] = G.att[tgt[0]]; fire(GD_COVER, &card, 1); return; }
    if (nc == 0) return;
    // Several attacks it covers, or cover AND pass: TARGET mode, aimed at the
    // first; the pass is the empty space right of the last attack.
    if (pass_ok) tgt[tgt_n++] = TGT_PASS;
    tgt_i = 0;
    in_target = 1;
    dirty = 1;
}

static void rearrange(int8_t step) {
    // The website's Cmd+left/right: drag the cursor card one place, the cursor
    // follows; clamped at the ends. The shown order becomes the memory, and
    // cards not in hand keep their remembered places after it.
    uint8_t to, t, i, k = 0, rest[GD_NCARDS], rn = 0;
    if (cur < 0 || (uint8_t)cur >= disp_n) return;
    if (step < 0 && cur == 0) return;
    if (step > 0 && (uint8_t)cur + 1u >= disp_n) return;
    to = (uint8_t)(cur + step);
    t = disp[to]; disp[to] = disp[(uint8_t)cur]; disp[(uint8_t)cur] = t;
    for (i = 0; i < mem_n; i++) if (!GD_HAS(G.hand[HUMAN], mem[i])) rest[rn++] = mem[i];
    for (i = 0; i < disp_n; i++) mem[k++] = disp[i];
    for (i = 0; i < rn; i++) mem[k++] = rest[i];
    cur = (int8_t)to;
}

static void sweep(int8_t step) {
    // The website's Shift+left/right: move the cursor and toggle each card it lands on.
    if (in_target || !disp_n) return;
    if (cur < 0) cur = 0;
    else cur = (int8_t)(((int16_t)cur + step + disp_n) % disp_n);
    toggle(disp[(uint8_t)cur]);
}

// One LEFT (-1) or RIGHT (+1) step.
static void lr_step(int8_t step) {
    if (in_select_held()) rearrange(step);
    else if (in_a_held()) sweep(step);
    else if (in_target) tgt_i = (uint8_t)(((int16_t)tgt_i + step + tgt_n) % tgt_n);
    else if (cur < 0) { if (disp_n) cur = 0; }
    else if (!disp_n) cur = -1;
    else cur = (int8_t)(((int16_t)cur + step + disp_n) % disp_n);
}

static void human_input(void) {
    int8_t steps = in_lr_steps();
    if (in_pressed(J_START)) {
        screen = SCREEN_DASH; dash_item = 0; dirty = 1;
        // presses latched with START belong to the dashboard
        in_keep_except(J_START);
        return;
    }
    if (G.status != GD_PLAYING) return;
    // A is the website's Shift press: toggle the cursor card in or out of the selection.
    if (in_pressed(J_A) && cur >= 0 && !in_target && (uint8_t)cur < disp_n) { toggle(disp[(uint8_t)cur]); dirty = 1; }
    // B backs out: of target mode, else clears the selection.
    if (in_pressed(J_B)) { if (in_target) in_target = 0; else clear_sel(); dirty = 1; }
    // Cursor-only changes redraw the rail and the sprites; the rest the board.
    // (target mode redraws the board: the pass slot is background tiles)
    if (steps) { if (in_select_held() || in_a_held() || in_target) dirty = 1; else dirty_ui = 1; }
    while (steps > 0) { lr_step(1); steps--; }
    while (steps < 0) { lr_step(-1); steps++; }
    // A or SELECT held: up and down are inert, so a held modifier never plays.
    if (in_down(J_A) || in_down(J_SELECT)) return;
    if (in_pressed(J_DOWN)) { if (in_target) in_target = 0; else take_or_good(); dirty = 1; }
    if (in_pressed(J_UP)) { up(); dirty = 1; }
}

static void bot_turn(void) {
    uint32_t n, r;
    uint16_t t0;
    gd_move m;
    if (G.status != GD_PLAYING || idle < BOT_DELAY) return;
    t0 = line_clock();
    n = gd_menu_count(&G, BOT);
    if (!n) return;
    r = gd_rng_below(n);
    gd_menu_move(&G, BOT, r, &m);
    play(BOT, &m);
    bot_lines_last = (uint16_t)(line_clock() - t0);
    if (bot_lines_last > bot_lines_max) bot_lines_max = bot_lines_last;
    bot_lines_sum += bot_lines_last;
    bot_moves++;
}

// ---- drawing: the table (gameboy/art/README.md "The table screen") ----------------------
#define ROW_BOTHAND 1
#define ROW_RAISE   12
#define ROW_HAND    13
#define ROW_UNDER   16
#define ROW_FOOT    17
#define HAND_SHOWN  19      // the most cards shown at once; more scroll

// Battle i of n: its attack card's tile. Up to six follow the art (one row of
// up to three centred at row 6, else three over the rest at rows 4 and 8, four
// columns a battle). A bout can hold up to 18 battles; past six they pack
// three columns apart, six a row, in rows 4 and 8, and past twelve a third row
// at 10 overlaps (rare: the most the differential test saw was a few more than six).
static void battle_xy(uint8_t i, uint8_t n, uint8_t *x, uint8_t *y) {
    uint8_t per, row, k, m, step;
    if (n <= 3) { per = 3; step = 4; }
    else if (n <= 6) { per = 3; step = 4; }
    else { per = 6; step = 3; }
    row = (uint8_t)(i / per);
    k = (uint8_t)(i % per);
    m = (uint8_t)(n - row * per);
    if (m > per) m = per;
    *x = (uint8_t)((SCR_W - (step * m - 1u)) / 2u + step * k);
    *y = n <= 3 ? 6 : (uint8_t)(4u + 4u * row);
    if (row >= 2) *y = 10;
}

static void pass_xy(uint8_t n, uint8_t *x, uint8_t *y) {
    battle_xy((uint8_t)(n - 1u), n, x, y);
    *x = (uint8_t)(*x + (n > 6 ? 3u : 4u));
    if (*x > SCR_W - 2u) *x = SCR_W - 2u;
}

// The hand's columns (the art's hand_columns): up to 10 cards two columns each,
// 11 to 19 one column each, both centred; 20 or more show 19 and scroll.
static uint8_t hand_x0, hand_step, hand_shown;

static void hand_layout(void) {
    if (disp_n <= 10) { hand_step = 2; hand_shown = disp_n; hand_x0 = (uint8_t)((SCR_W - 2u * disp_n) / 2u); }
    else if (disp_n <= HAND_SHOWN) { hand_step = 1; hand_shown = disp_n; hand_x0 = (uint8_t)((SCR_W - (disp_n + 1u)) / 2u); }
    else { hand_step = 1; hand_shown = HAND_SHOWN; hand_x0 = 0; }
    if (disp_n <= HAND_SHOWN) view_off = 0;
    else {
        // Scroll a card early, so the cursor is never on an end card while
        // cards hide past it: its underline would sit on that end's tab.
        if (cur >= 0 && (uint8_t)cur <= view_off) view_off = cur > 0 ? (uint8_t)(cur - 1) : 0;
        if (cur >= 0 && (uint8_t)cur + 2u >= view_off + HAND_SHOWN) view_off = (uint8_t)(cur + 2 - HAND_SHOWN);
        if (view_off + HAND_SHOWN > disp_n) view_off = (uint8_t)(disp_n - HAND_SHOWN);
    }
}

// Where hand card i is drawn, and how many columns of it show.
static void hand_place(uint8_t i, uint8_t *x, uint8_t *y, uint8_t *w) {
    const uint8_t k = (uint8_t)(i - view_off);
    *x = (uint8_t)(hand_x0 + hand_step * k);
    *y = GD_HAS(selm, disp[i]) ? ROW_RAISE : ROW_HAND;
    *w = (hand_step == 2 || k + 1u == hand_shown) ? 2 : 1;
}

// Whose move it is, short enough (12 at most) to sit beside any action word.
static const char *role_text(void) {
    if (G.nb == 0) return G.fa == HUMAN ? "YOUR ATTACK" : "THEIR ATTACK";
    if (G.defender == HUMAN) return gd_uncovered(&G) ? "YOU DEFEND" : "THEY MAY ADD";
    return gd_uncovered(&G) ? "THEY DEFEND" : "ALL COVERED";
}

// The move UP makes for the selection, or for the cursor card alone; then the
// move DOWN makes. The foot rail shows the first there is.
static const char *action_text(void) {
    uint8_t s[GD_NCARDS], n = sel_list(s), pills;
    if (n == 0 && cur >= 0) { s[0] = disp[(uint8_t)cur]; n = 1; }
    if (n) {
        pills = gd_pills(&G, HUMAN, s, n);
        if (pills & GD_PILL_ATTACK) return IC_UP "PLAY";
        if (pills & GD_PILL_COVER) return IC_UP "COVER";
        if (pills & GD_PILL_PASS) return IC_UP "PASS";
    }
    n = sel_list(s);
    pills = gd_pills(&G, HUMAN, s, n);
    if (pills & GD_PILL_PICKUP) return IC_DOWN "PICK UP";
    if (pills & GD_PILL_GOOD) return IC_DOWN "DONE";
    return "";
}

static char rail_buf[21];

static const char *top_left(void) {
    const char *p = bot_names[bot_city];
    uint8_t n = 0, c = gd_hand_n(&G, BOT);
    while (*p) rail_buf[n++] = *p++;
    rail_buf[n++] = ' ';
    rail_buf[n++] = IC_BACK[0];
    if (c >= 10) rail_buf[n++] = (char)('0' + c / 10);
    rail_buf[n++] = (char)('0' + c % 10);
    rail_buf[n] = 0;
    return rail_buf;
}

// The foot rail: the status, and the move DOWN or UP makes.
static void draw_foot(void) {
    uint8_t i, n;
    char foot_left[21];
    const char *fr;
    if (G.status != GD_PLAYING) {
        const uint8_t r = gd_result(&G);
        gfx_rail(ROW_FOOT, r == GD_RESULT_FOOL1 ? "YOU WIN" : r == GD_RESULT_FOOL0 ? "YOU ARE THE FOOL" : "DRAW: REPEATED",
                 r == GD_RESULT_DRAW ? IC_A "AGAIN" : "");
    } else if (in_target) {
        gfx_rail(ROW_FOOT, IC_LEFT IC_RIGHT "TARGET", tgt[tgt_i] == TGT_PASS ? IC_UP "PASS" : IC_UP "COVER");
    } else if (in_select_held() && cur >= 0) {
        gfx_rail(ROW_FOOT, "SELECT+" IC_LEFT IC_RIGHT, "MOVE CARD");
    } else {
        uint8_t s[GD_NCARDS];
        n = sel_list(s);
        fr = flash_t ? flash_msg : role_text();
        if (n && !flash_t) {
            foot_left[0] = n >= 10 ? (char)('0' + n / 10) : (char)('0' + n);
            i = 1;
            if (n >= 10) foot_left[i++] = (char)('0' + n % 10);
            foot_left[i] = 0;
            {
                const char *sel = " SELECTED";
                while (*sel) foot_left[i++] = *sel++;
                foot_left[i] = 0;
            }
            fr = foot_left;
        }
        gfx_rail(ROW_FOOT, fr, action_text());
    }
}

static void draw_table(void) {
    uint8_t i, n, x, y, w, deck;
    char right[8];
    gfx_clear_table();
    // the top rail: the bot and its count; the deck and the trump suit
    deck = (uint8_t)(G.stock_n + (G.has_flipped ? 1u : 0u));
    n = 0;
    if (deck) {
        right[n++] = IC_DECK[0];
        if (deck >= 10) right[n++] = (char)('0' + deck / 10);
        right[n++] = (char)('0' + deck % 10);
        right[n++] = ' ';
    }
    right[n++] = (char)(IC_SUIT0 + G.trump);
    right[n] = 0;
    gfx_rail(0, top_left(), right);
    // the bot's hand: backs one column apart, centred, 18 at most
    n = gd_hand_n(&G, BOT);
    if (n > 18) n = 18;
    x = (uint8_t)((SCR_W - (n + 1u)) / 2u);
    for (i = 0; i < n; i++) gfx_back((uint8_t)(x + i), ROW_BOTHAND);
    // the trump card under the deck, and the discard pile
    if (G.has_flipped) gfx_card(0, 4, G.flipped);
    if (G.stock_n) gfx_back(1, 4);
    if (G.discard) { gfx_back(0, 8); if (G.discard >= 12) gfx_back(1, 8); }
    // the table
    for (i = 0; i < G.nb; i++) {
        battle_xy(i, G.nb, &x, &y);
        gfx_card(x, y, G.att[i]);
        if (G.def[i] != GD_NONE) gfx_card((uint8_t)(x + 1u), (uint8_t)(y + 1u), G.def[i]);
    }
    if (in_target && tgt[tgt_i] == TGT_PASS) { pass_xy(G.nb, &x, &y); gfx_pass_slot(x, y); }
    // my hand: unraised cards first, then raised ones over them, left to right
    hand_layout();
    for (i = view_off; i < view_off + hand_shown; i++) {
        hand_place(i, &x, &y, &w);
        gfx_card(x, y, disp[i]);
    }
    if (disp_n > HAND_SHOWN) {
        // tabs counting the hidden cards each side
        char t[5];
        uint8_t k = 0;
        if (view_off) {
            t[k++] = IC_LEFT[0];
            if (view_off >= 10) t[k++] = (char)('0' + view_off / 10);
            t[k++] = (char)('0' + view_off % 10);
            t[k] = 0;
            gfx_text(0, ROW_UNDER, t);
        }
        n = (uint8_t)(disp_n - view_off - HAND_SHOWN);
        if (n) {
            k = 0;
            if (n >= 10) t[k++] = (char)('0' + n / 10);
            t[k++] = (char)('0' + n % 10);
            t[k++] = IC_RIGHT[0];
            t[k] = 0;
            gfx_text((uint8_t)(SCR_W - k), ROW_UNDER, t);
        }
    }
    draw_foot();
    gfx_flush();
}

// ---- sprites on the table ------------------------------------------------------------------
static uint16_t isqrt(uint16_t v) {
    uint16_t r = 0, b = 0x4000;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}

// The cover/pass arrow: dots from the cursor card's top to just under the
// target, and a head (the art's arrow()).
static void arrow(int16_t sx, int16_t sy, int16_t dx, int16_t dy, uint8_t dashed) {
    const int16_t ax = dx - sx, ay = dy - sy;
    const uint16_t adx = (uint16_t)(ax < 0 ? -ax : ax), ady = (uint16_t)(ay < 0 ? -ay : ay);
    const uint16_t len = isqrt((uint16_t)(adx * adx + ady * ady));
    const uint8_t step = dashed ? 10 : 6;
    uint16_t k, nd;
    int16_t stx, sty, fx, fy;
    if (len < 10) return;
    nd = (uint16_t)((len - 9u) / step);
    // one step along the line, in 8.8 fixed point: two divisions, not two a dot
    stx = (int16_t)(((int32_t)ax * step * 256) / (int16_t)len);
    sty = (int16_t)(((int32_t)ay * step * 256) / (int16_t)len);
    fx = (int16_t)(sx << 8); fy = (int16_t)(sy << 8);
    for (k = 1; k <= nd; k++) {
        fx += stx; fy += sty;
        gfx_spr((int16_t)(fx >> 8) - 2, (int16_t)(fy >> 8) - 2, T_SP_DOT, 0);
    }
    if (ay <= 0 && adx * 5u < ady * 2u) gfx_spr(dx - 4, dy - 4, T_SP_HEAD_N, 0);
    else if (ay <= 0) gfx_spr(dx - 4, dy - 4, T_SP_HEAD_NE, ax < 0 ? S_FLIPX : 0);
    else gfx_spr(dx - 4, dy - 4, T_SP_HEAD_N, S_FLIPY);
}

static void draw_table_sprites(void) {
    uint8_t x = 0, y = 0, w = 1, tx, ty;
    gfx_spr_begin();
    if (G.status == GD_PLAYING && cur >= 0 && (uint8_t)cur < disp_n) {
        hand_layout();
        hand_place((uint8_t)cur, &x, &y, &w);
        // the underline, 1 px under the card
        if (w == 1) gfx_spr(x * 8, (y + 3) * 8 + 1, T_SP_UL_ONE, 0);
        else { gfx_spr(x * 8, (y + 3) * 8 + 1, T_SP_UL_LEFT, 0); gfx_spr(x * 8 + 8, (y + 3) * 8 + 1, T_SP_UL_RIGHT, 0); }
        if (in_select_held()) {
            gfx_spr(x * 8 - 6, (y + 3) * 8 - 1, T_SP_NUDGE, 0);
            gfx_spr(x * 8 + w * 8 - 2, (y + 3) * 8 - 1, T_SP_NUDGE, S_FLIPX);
        }
        if (in_target) {
            const int16_t sx = x * 8 + (w == 1 ? 4 : 8), sy = y * 8 - 1;
            if (tgt[tgt_i] == TGT_PASS) {
                pass_xy(G.nb, &tx, &ty);
                arrow(sx, sy, tx * 8 + 8, ty * 8 + 32, 1);
                gfx_spr_text(tx * 8 - 8, ty * 8 - 9, "PASS");
            } else {
                battle_xy(tgt[tgt_i], G.nb, &tx, &ty);
                arrow(sx, sy, tx * 8 + 8, ty * 8 + 32, 0);
                gfx_spr(tx * 8 - 3, ty * 8 - 3, T_SP_BRACKET, 0);
                gfx_spr(tx * 8 + 11, ty * 8 - 3, T_SP_BRACKET, S_FLIPX);
                gfx_spr(tx * 8 - 3, ty * 8 + 19, T_SP_BRACKET, S_FLIPY);
                gfx_spr(tx * 8 + 11, ty * 8 + 19, T_SP_BRACKET, S_FLIPX | S_FLIPY);
            }
        }
    }
    gfx_spr_end();
}

// ---- the dashboard (the traveller), and the end of a game ---------------------------------
// gameboy/art/README.md "Dashboard": the pointer on the selected row (the
// boarding pass first when a game is open), and the plane over the selected
// city on the flight route, gliding when the pointer moves.
#define DASH_CONTINUE 0
#define DASH_ROW_CONTINUE 10
#define DASH_ROW_BOT0 13
#define PLANE_STEP 2            // pixels a frame the plane glides
static const uint8_t city_x[4] = { 14, 44, 75, 112 };    // art travel.py CITY_X
static uint8_t plane_x;

static uint8_t can_continue(void) { return live && G.status == GD_PLAYING; }

static void dash_sprites(void) {
    const uint8_t row = dash_item == DASH_CONTINUE ? DASH_ROW_CONTINUE : (uint8_t)(DASH_ROW_BOT0 + dash_item - 1u);
    const uint8_t city = dash_item == DASH_CONTINUE ? bot_city : (uint8_t)(dash_item - 1u);
    const uint8_t want = city_x[city];
    if (plane_x < want) plane_x = (uint8_t)(want - plane_x < PLANE_STEP ? want : plane_x + PLANE_STEP);
    else if (plane_x > want) plane_x = (uint8_t)(plane_x - want < PLANE_STEP ? want : plane_x - PLANE_STEP);
    gfx_spr_begin();
    gfx_spr(0, row * 8, ART_DASH_POINTER_TILE, 0);
    gfx_spr((int16_t)plane_x - 4, ART_DASH_ROUTE_Y - 4, ART_DASH_PLANE_TILE, 0);
    gfx_spr_end();
}

// `full`: load the picture (entering the screen); otherwise only the sprites move.
static void draw_dash(uint8_t full) {
    if (full) {
        gfx_load_full(can_continue() ? FULL_DASHBOARD : FULL_DASHBOARD_NEW);
        // the open game's city on the boarding pass, in the paper font
        if (can_continue()) gfx_text(ART_DASH_CITY_COL, ART_DASH_CITY_ROW, bot_names[bot_city]);
        gfx_flush();
        if (!plane_x) plane_x = city_x[bot_city];
    }
    dash_sprites();
}

// The win screen's fireworks (art wins.py BURSTS and SCHEDULE): three bursts,
// each a phase behind the last, in a 4-frame loop of FW_FRAME vsyncs a frame.
#define FW_FRAME 8
static const uint8_t fw_x[3] = { 24, 104, 140 }, fw_y[3] = { 40, 32, 56 };
// per loop frame, per burst: 0xFF none, 0xFE a rising shell, else the phase
static const uint8_t fw_sched[4][3] = {
    { 0, 0xFE, 0xFF }, { 1, 0, 0xFE }, { 2, 1, 0 }, { 0xFF, 2, 1 },
};
static uint8_t fw_t;

static void fireworks(void) {
    uint8_t k;
    const uint8_t f = (uint8_t)((fw_t / FW_FRAME) & 3u);
    gfx_spr_begin();
    for (k = 0; k < 3; k++) {
        const uint8_t ph = fw_sched[f][k];
        const int16_t x = fw_x[k], y = fw_y[k];
        if (ph == 0xFF) continue;
        if (ph == 0xFE) { gfx_spr(x - 4, y + 10, T_FW_SHELL, 0); continue; }
        gfx_spr(x - 8, y - 8, (uint8_t)(T_FW_SPARK + ph), 0);
        gfx_spr(x, y - 8, (uint8_t)(T_FW_SPARK + ph), S_FLIPX);
        gfx_spr(x - 8, y, (uint8_t)(T_FW_SPARK + ph), S_FLIPY);
        gfx_spr(x, y, (uint8_t)(T_FW_SPARK + ph), S_FLIPX | S_FLIPY);
    }
    gfx_spr_end();
}

static uint8_t won(void) { return gd_result(&G) == GD_RESULT_FOOL1; }

static void draw_end(void) {
    if (won()) {
        gfx_load_full((uint8_t)(FULL_WIN + bot_city));
        // the bot's name, centred, in the lacquer font
        gfx_text((uint8_t)((SCR_W - gfx_text_len(bot_names[bot_city])) / 2u), ART_WIN_NAME_ROW, bot_names[bot_city]);
    } else {
        gfx_load_full(FULL_LOSE);
    }
    gfx_flush();
    fw_t = 0;
    gfx_spr_begin();
    gfx_spr_end();
}

static void go_table(void) {
    screen = SCREEN_TABLE;
    dirty = 1;
}

static void dash_input(void) {
    const uint8_t first = can_continue() ? DASH_CONTINUE : 1u;
    if (in_any_pressed() && !seeded) {
        gd_rng_seed(GD_SEED_FIXED ? (uint16_t)GD_SEED_FIXED : in_first_press_time());
        seeded = 1;
    }
    if (dash_item < first) dash_item = first;
    if (in_pressed(J_UP) && dash_item > first) { dash_item--; dirty = 1; }
    if (in_pressed(J_DOWN) && dash_item < 4) { dash_item++; dirty = 1; }
    if (in_pressed(J_A) || in_pressed(J_START)) {
        if (dash_item == DASH_CONTINUE) { go_table(); return; }
        bot_city = (uint8_t)(dash_item - 1u);
        new_game();
        go_table();
    }
}

static void end_input(void) {
    if (in_pressed(J_A)) { new_game(); go_table(); }
    else if (in_pressed(J_B) || in_pressed(J_START)) { screen = SCREEN_DASH; dash_item = 1; dirty = 1; }
}

void main(void) {
    uint8_t shown = 0xFF, was_select = 0;
    gfx_init();
    in_init();
    dirty = 1;
    for (;;) {
        vsync();
        in_update();
        if (screen == SCREEN_DASH) {
            dash_input();
            if (shown == SCREEN_DASH && !dirty) dash_sprites();
        } else if (screen == SCREEN_END) {
            end_input();
            if (shown == SCREEN_END && won()) { fw_t++; if (fw_t % FW_FRAME == 0) fireworks(); }
        } else {
            if (idle < 255) idle++;
            if (flash_t) { flash_t--; if (!flash_t) dirty = 1; }
            if (G.status != GD_PLAYING) {
                // the finished table, then the picture; a draw has no picture
                if (gd_result(&G) == GD_RESULT_DRAW) { if (in_pressed(J_A)) new_game(); }
                else if (++end_t >= END_DELAY) { screen = SCREEN_END; dirty = 1; }
                if (in_pressed(J_START)) { screen = SCREEN_DASH; dash_item = 1; dirty = 1; }
            } else {
                human_input();
                if (screen == SCREEN_TABLE) bot_turn();
            }
            if (in_select_held() != was_select) { was_select = in_select_held(); dirty = 1; }
        }
        if (screen != shown) dirty = 1;
        if (dirty_ui && !dirty && screen == SCREEN_TABLE) {
            // a cursor move: the hand only needs redrawing when it scrolls
            const uint8_t off = view_off;
            hand_layout();
            if (view_off != off) dirty = 1;
            else { draw_foot(); gfx_flush(); draw_table_sprites(); }
        }
        dirty_ui = 0;
        if (dirty) {
            dirty = 0;
            if (screen == SCREEN_DASH) draw_dash(shown != SCREEN_DASH);
            else if (screen == SCREEN_END) draw_end();
            else {
                const uint16_t t0 = line_clock();
                if (shown != SCREEN_TABLE) gfx_load_table();
                draw_table();
                draw_table_sprites();
                draw_lines_last = (uint16_t)(line_clock() - t0);
                if (shown == SCREEN_TABLE && draw_lines_last > draw_lines_max) draw_lines_max = draw_lines_last;
            }
            shown = screen;
        }
    }
}
