// Durak for the Game Boy: you (seat 0) against a bot (seat 1) that plays a
// uniformly random legal move. Rules: gd_rules.c. What each button does on a
// board: gd_play.c and up() below, a port of the website's keyboard mode
// (foolish src/components/GameDisplay/KeyboardPlayMode.tsx). README.md has the
// controls table.
#include <gb/gb.h>
#include <stdint.h>

#include "gd_rules.h"
#include "gd_play.h"
#include "gd_rng.h"
#include "gfx.h"
#include "input.h"

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

static gd_game G;
static gd_rep  R;
static uint8_t live;            // a game has been dealt
static uint8_t seeded;

// ---- the hand as shown: the website's sticky arrangement memory --------------------
// (foolish src/state/clientReconcile.ts reconcileHandMemory / displayedHand):
// every card ever held keeps its slot, so a card that comes back (a pickup)
// returns to where it was; new cards join at the end in the order they arrive.
static uint8_t mem[GD_NCARDS], mem_n;
static uint8_t disp[GD_NCARDS], disp_n;
static int8_t  cur = -1;        // the cursor, an index into disp; -1 none
static gd_mask selm;            // the selection (the website's selectedCards)
static uint8_t in_target;       // the cover/pass TARGET mode
static uint8_t tgt[GD_MAXB + 1], tgt_n, tgt_i;
static uint8_t view_off;        // first shown card when the hand is wider than the screen

static uint8_t idle;            // frames since the board last changed
static uint8_t flash_t;
static const char *flash_msg;
static uint8_t dirty;
static uint8_t home = 1, home_item;

// How long the bot's last decision took, in scanlines (1 line = 108.7 us):
// count + unrank + apply. Read by tools/gbrun (peek _bot_lines_max).
uint16_t bot_lines_last, bot_lines_max;

static void rebuild_disp(void) {
    uint8_t i;
    disp_n = 0;
    for (i = 0; i < mem_n; i++) if (GD_HAS(G.hand[HUMAN], mem[i])) disp[disp_n++] = mem[i];
    for (i = 0; i < 5; i++) selm.b[i] &= G.hand[HUMAN].b[i];
    // The website's clamp: keep the index, pull it back to the last card if
    // what it sat on left the end, and drop it once the hand is empty.
    if (cur >= (int8_t)disp_n) cur = disp_n ? (int8_t)(disp_n - 1u) : -1;
}

static uint8_t in_mem(uint8_t c) {
    uint8_t i;
    for (i = 0; i < mem_n; i++) if (mem[i] == c) return 1;
    return 0;
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
            if (GD_HAS(G.hand[HUMAN], c) && !in_mem(c)) {
                const uint8_t a = arrival(b, c);
                if (!any || a < best) { best = a; best_c = c; any = 1; }
            }
        if (any) mem[mem_n++] = best_c;
    } while (any);
    rebuild_disp();
}

static void flash(const char *s) { flash_msg = s; flash_t = FLASH_FRAMES; }

static gd_game before;

// Apply a move for a seat; 1 if it was legal.
static uint8_t play(uint8_t seat, const gd_move *m) {
    before = G;
    if (!gd_apply(&G, &R, seat, m)) return 0;
    note_new_cards(&before);
    idle = 0;
    dirty = 1;
    in_target = 0;      // the website's afterMove (and a stale target after a bot move)
    if (seat == BOT) {
        switch (m->type) {
        case GD_ATTACK: flash(before.nb ? "BOT ADDS" : "BOT ATTACKS"); break;
        case GD_COVER:  flash("BOT COVERS"); break;
        case GD_PASS:   flash("BOT PASSES IT ON"); break;
        case GD_PICKUP: flash("BOT TAKES"); break;
        case GD_GOOD:
            // A good over an uncovered attack is a silent one (foolish
            // game_shown_good_mask): the bot just stops adding. Not shown.
            if (before.nb && !gd_uncovered(&before)) flash("BOT: DONE");
            break;
        }
    } else if (m->type == GD_PICKUP) {
        flash("YOU TAKE");
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
    for (i = 0; i < GD_NCARDS; i++) if (GD_HAS(G.hand[HUMAN], i)) mem[mem_n++] = i;
    for (i = 0; i < 5; i++) selm.b[i] = 0;
    cur = -1;
    in_target = 0;
    view_off = 0;
    idle = 0;
    flash_t = 0;
    live = 1;
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

static void human_input(void) {
    const int8_t step = in_lr_step();
    if (in_pressed(J_START)) { home = 1; home_item = 0; dirty = 1; return; }
    if (G.status != GD_PLAYING) {
        if (in_pressed(J_A)) new_game();
        return;
    }
    if (in_pressed(J_A) || in_pressed(J_B) || in_pressed(J_UP) || in_pressed(J_DOWN) || step) dirty = 1;
    // A is the website's Shift press: toggle the cursor card in or out of the selection.
    if (in_pressed(J_A) && cur >= 0 && !in_target && (uint8_t)cur < disp_n) toggle(disp[(uint8_t)cur]);
    // B backs out: of target mode, else clears the selection.
    if (in_pressed(J_B)) { if (in_target) in_target = 0; else clear_sel(); }
    if (step) {
        if (in_select_held()) rearrange(step);
        else if (in_a_held()) sweep(step);
        else if (in_target) tgt_i = (uint8_t)(((int16_t)tgt_i + step + tgt_n) % tgt_n);
        else if (cur < 0) { if (disp_n) cur = 0; }
        else if (!disp_n) cur = -1;
        else cur = (int8_t)(((int16_t)cur + step + disp_n) % disp_n);
    }
    // A or SELECT held: up and down are inert, so a held modifier never plays.
    if (in_down(J_A) || in_down(J_SELECT)) return;
    if (in_pressed(J_DOWN)) { if (in_target) in_target = 0; else take_or_good(); }
    if (in_pressed(J_UP)) up();
}

static void bot_turn(void) {
    uint32_t n, r;
    uint16_t t0, t1;
    uint8_t ly0, ly1;
    gd_move m;
    if (G.status != GD_PLAYING || idle < BOT_DELAY) return;
    t0 = sys_time; ly0 = LY_REG;
    n = gd_menu_count(&G, BOT);
    if (!n) return;
    r = gd_rng_below(n);
    gd_menu_move(&G, BOT, r, &m);
    play(BOT, &m);
    t1 = sys_time; ly1 = LY_REG;
    bot_lines_last = (uint16_t)((uint16_t)(t1 - t0) * 154u + ly1 - ly0);
    if (bot_lines_last > bot_lines_max) bot_lines_max = bot_lines_last;
}

// ---- drawing ---------------------------------------------------------------------------
#define HAND_Y 15
#define TABLE_Y 3

static void battle_xy(uint8_t i, uint8_t *x, uint8_t *y) {
    *x = (uint8_t)(1u + 3u * (i % 6u));
    *y = (uint8_t)(TABLE_Y + 3u * (i / 6u));
}

static const char *role_text(void) {
    if (G.nb == 0) return G.fa == HUMAN ? "YOUR ATTACK" : "BOT TO ATTACK";
    if (G.defender == HUMAN) return gd_uncovered(&G) ? "DEFEND" : "BOT MAY ADD";
    return gd_uncovered(&G) ? "BOT DEFENDS" : "ADD OR DONE";
}

static void draw_game(void) {
    uint8_t i, n, x, y, w, x0, s[GD_NCARDS], sn, pills;
    gfx_clear();
    // the bot's hand: backs and a count
    n = gd_hand_n(&G, BOT);
    for (i = 0; i < n && i < 15; i++) gfx_tile(i, 0, 2);
    gfx_text(16, 0, "x");
    gfx_num(17, 0, n);
    // the trump card under the stock, and the stock
    if (G.has_flipped) gfx_card(0, 1, G.flipped, 0);
    else gfx_suit(0, 2, G.trump);
    if (G.stock_n) { gfx_tile(1, 1, 3); gfx_tile(1, 2, 4); }
    gfx_num(3, 1, (uint8_t)(G.stock_n + (G.has_flipped ? 1u : 0u)));
    gfx_text(3, 2, "LEFT");
    // the table
    for (i = 0; i < G.nb; i++) {
        battle_xy(i, &x, &y);
        gfx_card(x, y, G.att[i], 0);
        if (G.def[i] != GD_NONE) gfx_card((uint8_t)(x + 1u), (uint8_t)(y + 1u), G.def[i], 0);
    }
    if (in_target && tgt[tgt_i] == TGT_PASS && G.nb < GD_MAXB) {
        battle_xy(G.nb, &x, &y);
        gfx_tile(x, y, 5); gfx_tile(x, (uint8_t)(y + 1u), 6);
    }
    // status and hints
    if (G.status != GD_PLAYING) {
        const uint8_t r = gd_result(&G);
        gfx_text(0, 12, r == GD_RESULT_FOOL1 ? "YOU WIN!" : r == GD_RESULT_FOOL0 ? "YOU ARE THE FOOL" : "DRAW: REPEATED");
        gfx_text(0, 13, "A:AGAIN START:HOME");
    } else {
        gfx_text(0, 12, flash_t ? flash_msg : role_text());
        if (in_target) {
            gfx_text(0, 13, tgt[tgt_i] == TGT_PASS ? "<> AIM ^PASS vBACK" : "<> AIM ^COVER vBACK");
        } else {
            sn = sel_list(s);
            if (sn == 0 && cur >= 0) { s[0] = disp[(uint8_t)cur]; pills = gd_pills(&G, HUMAN, s, 1); }
            else pills = gd_pills(&G, HUMAN, s, sn);
            if (pills & GD_PILL_ATTACK) gfx_text(0, 13, "^ATTACK");
            else if (pills & GD_PILL_COVER) gfx_text(0, 13, "^COVER");
            else if (pills & GD_PILL_PASS) gfx_text(0, 13, "^PASS");
            pills = gd_pills(&G, HUMAN, s, sel_list(s));
            if (pills & GD_PILL_PICKUP) gfx_text(14, 13, "vTAKE");
            else if (pills & GD_PILL_GOOD) gfx_text(14, 13, "vDONE");
        }
    }
    // my hand, centred; scrolled when wider than the screen
    w = disp_n <= SCR_W ? disp_n : (uint8_t)(SCR_W - 2u);
    x0 = disp_n <= SCR_W ? (uint8_t)((SCR_W - disp_n) / 2u) : 1u;
    if (disp_n <= SCR_W) view_off = 0;
    else {
        if (cur >= 0 && (uint8_t)cur < view_off) view_off = (uint8_t)cur;
        if (cur >= 0 && (uint8_t)cur >= view_off + w) view_off = (uint8_t)(cur - w + 1);
        if (view_off + w > disp_n) view_off = (uint8_t)(disp_n - w);
        if (view_off) gfx_text(0, HAND_Y, "<");
        if (view_off + w < disp_n) gfx_text(SCR_W - 1u, HAND_Y, ">");
    }
    for (i = 0; i < w; i++) gfx_card((uint8_t)(x0 + i), HAND_Y, disp[view_off + i], GD_HAS(selm, disp[view_off + i]));
    gfx_flush();
    // sprites: the cursor's underline, and the target frame
    gfx_sprite(SPR_CURSOR, cur >= 0, (uint8_t)(x0 + (uint8_t)cur - view_off), HAND_Y + 2);
}

static void draw_sprites_blink(void) {
    uint8_t x, y, show;
    show = in_target && (sys_time & 16u) == 0;
    if (in_target) {
        if (tgt[tgt_i] == TGT_PASS) battle_xy(G.nb, &x, &y);
        else battle_xy(tgt[tgt_i], &x, &y);
    } else { x = 0; y = 0; }
    gfx_sprite(SPR_FRAME_TOP, show, x, y);
    gfx_sprite(SPR_FRAME_BOT, show, x, (uint8_t)(y + 1u));
}

static void draw_home(void) {
    gfx_clear();
    gfx_text(7, 3, "DURAK");
    gfx_text(4, 5, "THE FOOL GAME");
    gfx_text(5, 9, "NEW GAME");
    if (live && G.status == GD_PLAYING) gfx_text(5, 10, "CONTINUE");
    gfx_text(3, home_item ? 10 : 9, ">");
    gfx_text(3, 13, "BOT: RANDOM");
    gfx_text(1, 16, "A:CHOOSE  ^v:MOVE");
    gfx_flush();
    gfx_sprite(SPR_CURSOR, 0, 0, 0);
    gfx_sprite(SPR_FRAME_TOP, 0, 0, 0);
    gfx_sprite(SPR_FRAME_BOT, 0, 0, 0);
}

static void home_input(void) {
    const uint8_t can_continue = live && G.status == GD_PLAYING;
    if (in_any_pressed() && !seeded) {
        gd_rng_seed(GD_SEED_FIXED ? (uint16_t)GD_SEED_FIXED : sys_time);
        seeded = 1;
    }
    if (in_pressed(J_UP) || in_pressed(J_DOWN)) { if (can_continue) home_item ^= 1u; dirty = 1; }
    if (in_pressed(J_A) || in_pressed(J_START)) {
        if (home_item == 1 && can_continue) { home = 0; dirty = 1; return; }
        new_game();
        home = 0;
        dirty = 1;
    }
}

void main(void) {
    gfx_init();
    dirty = 1;
    for (;;) {
        vsync();
        in_update();
        if (home) {
            home_input();
            if (dirty) { dirty = 0; if (home) draw_home(); else draw_game(); }
            continue;
        }
        if (idle < 255) idle++;
        if (flash_t) { flash_t--; if (!flash_t) dirty = 1; }
        human_input();
        if (!home) bot_turn();
        if (dirty) { dirty = 0; if (home) draw_home(); else draw_game(); }
        if (!home) draw_sprites_blink();
    }
}
