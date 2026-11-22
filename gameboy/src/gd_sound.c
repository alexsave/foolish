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

// A landing, held for the frame clock its flight runs on (snd_strike_at):
// it starts on the first tick once sys_time reads `land - lead`. A card
// landing's take (PLACE, COVER, FLIP, DEAL_LAND) is a volume-1 lead-in row and
// then its impact (snd_tab.c, row 1 of every take), so `lead` 1 puts the
// lead-in in the flight's last frame and the impact, the take's moment, in
// the landing frame (heard 0 to 1.1 frames after it begins, whatever the
// tick's phase); the slide into a hand has no impact and starts on the
// landing, `lead` 0. The design counted ticks from the move (snd_pre early),
// which mGBA's capture found up to a frame off either way, as the 60.24 Hz
// tick slides against the 59.73 Hz frame (README "Sound").
#define IMPACT 1u
static void land_at(uint8_t cue, uint16_t land, uint8_t lead, uint8_t n, uint8_t flags) {
    snd_strike_at(cue, (uint16_t)(land - lead), 0, n, flags);
}

void gd_snd_move(uint8_t seat, const gd_game *b, const gd_game *g, const gd_move *m, uint8_t flew, uint16_t land) GD_BANKED {
    // the bout ended on the discard: the table's top cards fly to the pile
    // (README "The discard pile and the sweep"), and its sound is the sweep's
    const uint8_t swept = b->nb && !g->nb && m->type != GD_PICKUP;
    switch (m->type) {
    case GD_ATTACK:
    case GD_COVER:
    case GD_PASS:
        if (!swept) snd_strike(SND_LIFT, 0, 1, 0);
        if (flew && !swept) {
            land_at(m->type == GD_COVER ? SND_COVER : SND_PLACE, land, IMPACT, m->n, 0);
            // the transfer's slide, 15 ticks (249 ms) after it lands (the take's own late start)
            if (m->type == GD_PASS) snd_strike_at(SND_PASS, land, 15, 1, 0);
        }
        break;
    case GD_PICKUP:
        snd_strike(SND_PICKUP, 0, 1, 0);
        if (flew) land_at(SND_INTO_HAND, land, 0, 1, seat == BOT ? SND_FAR : 0);
        break;
    case GD_GOOD:
        // a shown good (a silent one, over an uncovered attack, is not heard)
        if (b->nb && !gd_uncovered(b)) snd_strike(SND_GOOD, 0, 1, 0);
        break;
    }
    if (swept) {
        snd_strike(SND_SWEEP, 0, 1, 0);
        // the cards come to rest on the pile as the sweep's flight lands
        if (flew) land_at(SND_PILE, land, 0, 1, 0);
        else snd_strike(SND_PILE, 16, 1, 0);
    }
    // a seat out: at two players, the game's last move (a draw has no one out)
    if (g->status != GD_PLAYING && gd_result(g) != GD_RESULT_DRAW) snd_strike(SND_OUT, 0, 1, 0);
}

void gd_snd_deal(uint8_t seat, uint16_t land) GD_BANKED {
    snd_strike(SND_DEAL, 0, 1, 0);
    land_at(SND_DEAL_LAND, land, IMPACT, 1, seat == BOT ? SND_FAR : 0);
}

void gd_snd_trump(uint16_t land) GD_BANKED {
    snd_strike(SND_TURN, 0, 1, 0);
    land_at(SND_FLIP, land, IMPACT, 1, 0);
}
