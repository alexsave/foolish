// gd_sound - see gd_sound.h.
//
// The website's rules, kept (audio/DESIGN.md "The website's rules"): a cue a
// card, at most SND_MAX_HITS a beat, SND_STAGGER ticks apart, all done by
// snd_strike; a landing is struck its pre-roll early so its moment is the
// landing; every strike starts on the tick after it is made, so a sound
// trails its picture, never leads it.
#include "gd_sound.h"
#include "snd.h"

#define BOT 1

// A landing `land` frames from now: struck its pre-roll early.
static void land_at(uint8_t cue, uint8_t land, uint8_t n, uint8_t flags) {
    snd_strike(cue, (uint8_t)(land - snd_pre[cue]), n, flags);
}

void gd_snd_move(uint8_t seat, const gd_game *b, const gd_game *g, const gd_move *m, uint8_t land) GD_BANKED {
    // the bout ended on the discard: the table is swept at once, no flight
    const uint8_t swept = b->nb && !g->nb && m->type != GD_PICKUP;
    switch (m->type) {
    case GD_ATTACK:
    case GD_COVER:
    case GD_PASS:
        if (!swept) snd_strike(SND_LIFT, 0, 1, 0);
        if (land) {
            land_at(m->type == GD_COVER ? SND_COVER : SND_PLACE, land, m->n, 0);
            // the transfer's slide, 249 ms after it lands (the take's own late start)
            if (m->type == GD_PASS) snd_strike(SND_PASS, (uint8_t)(land + 15u), 1, 0);
        }
        break;
    case GD_PICKUP:
        snd_strike(SND_PICKUP, 0, 1, 0);
        if (land) land_at(SND_INTO_HAND, land, 1, seat == BOT ? SND_FAR : 0);
        break;
    case GD_GOOD:
        // a shown good (a silent one, over an uncovered attack, is not heard)
        if (b->nb && !gd_uncovered(b)) snd_strike(SND_GOOD, 0, 1, 0);
        break;
    }
    if (swept) {
        snd_strike(SND_SWEEP, 0, 1, 0);
        snd_strike(SND_PILE, 16, 1, 0);
    }
    // a seat out: at two players, the game's last move (a draw has no one out)
    if (g->status != GD_PLAYING && gd_result(g) != GD_RESULT_DRAW) snd_strike(SND_OUT, 0, 1, 0);
}

void gd_snd_deal(uint8_t seat, uint8_t land) GD_BANKED {
    snd_strike(SND_DEAL, 0, 1, 0);
    land_at(SND_DEAL_LAND, land, 1, seat == BOT ? SND_FAR : 0);
}

void gd_snd_trump(uint8_t land) GD_BANKED {
    snd_strike(SND_TURN, 0, 1, 0);
    land_at(SND_FLIP, land, 1, 0);
}
