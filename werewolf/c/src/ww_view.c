#include "ww_view.h"

// ONE walk over the layout, used both to write it and to measure it. A separate
// measure function is a second implementation of the layout, and the two drift
// the first time a field is added - which is the bug that ships as a Swift
// decoder reading garbage at the tail.
typedef struct { unsigned char *p; int n; } Put;

static void u8(Put *w, int v) {
    if (w->p) w->p[w->n] = (unsigned char)v;
    w->n++;
}
static void u16le(Put *w, unsigned v) {
    u8(w, v & 0xff);
    u8(w, (v >> 8) & 0xff);
}
static void bytes(Put *w, const char *src, int n) {
    for (int i = 0; i < n; i++) u8(w, (unsigned char)src[i]);
}

// Is the viewer a LIVING wolf? Dead wolves lose the channel: a corpse reading
// tonight's plan is a corpse that can still talk to the table.
static int viewer_is_wolf(const WwGame *g, int viewer) {
    return viewer >= 0 && viewer < g->n_players
        && ww_is_alive(g, viewer) && g->role[viewer] == WW_ROLE_WOLF;
}

static int viewer_is_seer(const WwGame *g, int viewer) {
    return viewer >= 0 && viewer < g->n_players
        && ww_is_alive(g, viewer) && g->role[viewer] == WW_ROLE_SEER;
}

// What this seat learned about `seat`'s record for the night in play. Note what
// is NOT here: the target, the flags beyond "who sent it", and whether a line
// rode along. CARRIED is the one distinction that is safe to publish, because
// it is already public - the carrier's own bubble says whose passes it brought.
static int sent_state(const WwGame *g, int seat) {
    for (int i = 0; i < g->n_records; i++) {
        const WwRecord *r = &g->rec[i];
        if (r->night != g->night || (r->flags & WW_REC_LYNCH)) continue;
        if (r->seat != seat) continue;
        return (r->flags & WW_REC_AUTO_PASS) ? WW_SENT_CARRIED : WW_SENT_YES;
    }
    return WW_SENT_NO;
}

// Is this viewer entitled to seat `s`'s role?
static int may_know_role(const WwGame *g, int viewer, int s) {
    if (!ww_is_alive(g, s)) return 1;                 // the dead are public
    if (viewer < 0) return 0;                         // a spectator knows nothing
    if (s == viewer) return 1;                        // your own, always
    // A wolf knows the pack. Only a LIVING wolf: see viewer_is_wolf.
    return viewer_is_wolf(g, viewer) && g->role[s] == WW_ROLE_WOLF;
}

static int walk(const WwGame *g, int viewer, Put *w) {
    const int wolf = viewer_is_wolf(g, viewer);
    const int seer = viewer_is_seer(g, viewer);

    u8(w, WW_VIEW_FORMAT_VERSION);
    u8(w, g->phase);
    u8(w, g->n_players);
    u8(w, g->night);
    u16le(w, g->alive);
    u16le(w, g->turn);
    u8(w, g->winner);
    u8(w, viewer < 0 ? WW_NO_SEAT : viewer);
    u8(w, (viewer < 0 || viewer >= g->n_players) ? WW_ROLE_UNKNOWN : g->role[viewer]);

    int n_roles = 0;
    for (int s = 0; s < g->n_players; s++) n_roles += may_know_role(g, viewer, s);
    u8(w, n_roles);
    for (int s = 0; s < g->n_players; s++) {
        if (!may_know_role(g, viewer, s)) continue;
        u8(w, s);
        u8(w, g->role[s]);
    }

    // CONTRACT 1. Two bytes per seat, and the second one says only whether they
    // sent. Whatever the sender's role was, whatever they picked, whether a wolf
    // line rode along - it all reduces to the same byte here.
    u8(w, g->n_players);
    for (int s = 0; s < g->n_players; s++) {
        u8(w, s);
        u8(w, sent_state(g, s));
    }

    // The viewer's own choice, so the screen can show what they already picked
    // when they re-open the bubble.
    {
        int have = 0, target = WW_NO_SEAT;
        if (viewer >= 0) {
            for (int i = 0; i < g->n_records; i++) {
                const WwRecord *r = &g->rec[i];
                if (r->night != g->night || (r->flags & WW_REC_LYNCH)) continue;
                if (r->seat != viewer) continue;
                if (r->flags & WW_REC_AUTO_PASS) continue;
                have = 1; target = r->target;
            }
        }
        u8(w, have);
        u8(w, target);
    }

    // CONTRACT 2, and the decider with it: whose call tonight is, is wolf
    // knowledge. A non-wolf that learned the decider's seat would learn a wolf.
    u8(w, wolf ? ww_night_decider(g) : WW_NO_SEAT);
    if (!wolf) {
        u8(w, 0);                 // no rows, and therefore no lengths to count
    } else {
        int n_chat = 0;
        for (int i = 0; i < g->n_records; i++) {
            const WwRecord *r = &g->rec[i];
            if (r->night == g->night && (r->flags & WW_REC_HAS_CHAT)) n_chat++;
        }
        u8(w, n_chat);
        for (int i = 0; i < g->n_records; i++) {
            const WwRecord *r = &g->rec[i];
            if (r->night != g->night || !(r->flags & WW_REC_HAS_CHAT)) continue;
            u8(w, r->seat);
            u8(w, r->chat_len);
            bytes(w, r->chat, r->chat_len);
        }
    }

    // CONTRACT 3. The seer's answers are computed from the seer's own records,
    // so they land the moment the seer sends rather than at dawn - the question
    // was asked, and there is nobody to wait for.
    if (!seer) {
        u8(w, 0);
    } else {
        int n = 0;
        for (int i = 0; i < g->n_records; i++) {
            const WwRecord *r = &g->rec[i];
            if (r->seat != viewer || (r->flags & WW_REC_LYNCH)) continue;
            if ((r->flags & WW_REC_AUTO_PASS) || r->target == WW_NO_SEAT) continue;
            n++;
        }
        u8(w, n);
        for (int i = 0; i < g->n_records; i++) {
            const WwRecord *r = &g->rec[i];
            if (r->seat != viewer || (r->flags & WW_REC_LYNCH)) continue;
            if ((r->flags & WW_REC_AUTO_PASS) || r->target == WW_NO_SEAT) continue;
            u8(w, r->target);
            u8(w, ww_team_of(g->role[r->target]));
        }
    }

    // CONTRACT 4's other half: who died, and how. Public to everyone, including
    // a spectator - it is what the thread itself shows.
    {
        const int n = g->night + 1;
        u8(w, n);
        for (int i = 0; i < n; i++) {
            u8(w, i);
            u8(w, g->victim[i]);
            u8(w, g->lynched[i]);
        }
    }
    return w->n;
}

int ww_view_put(const WwGame *g, int viewer, unsigned char *out) {
    Put w; w.p = out; w.n = 0;
    return walk(g, viewer, &w);
}

int ww_view_measure(const WwGame *g, int viewer) {
    Put w; w.p = 0; w.n = 0;
    return walk(g, viewer, &w);
}
