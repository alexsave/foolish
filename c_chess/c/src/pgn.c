/* pgn.c - see pgn.h. Writes the seven-tag roster plus TimeControl and Termination, then the moves
 * in SAN, wrapped at 80 columns. */
#include "pgn.h"

#include <stdio.h>
#include <string.h>

const char *cc_result_str(int status, int winner) {
    if (status == CC_PLAYING || status == CC_ABORTED) return "*";
    if (winner == CH_WHITE) return "1-0";
    if (winner == CH_BLACK) return "0-1";
    return "1/2-1/2";
}

static const char *termination(int status) {
    switch (status) {
    case CC_PLAYING: return "Unterminated";
    case CC_FLAG: case CC_DRAW_FLAG_MATERIAL: return "Time forfeit";
    case CC_ABORTED: return "Abandoned";
    default: return "Normal";
    }
}

typedef struct { char *p; size_t cap, n; } out_t;

static void put(out_t *o, const char *s, size_t len) {
    if (o->n + len < o->cap) memcpy(o->p + o->n, s, len);
    o->n += len;
}
static void puts_(out_t *o, const char *s) { put(o, s, strlen(s)); }

/* A tag's value with " and \ escaped. */
static void tag(out_t *o, const char *name, const char *value) {
    puts_(o, "[");
    puts_(o, name);
    puts_(o, " \"");
    for (const char *s = value ? value : "?"; *s; s++) {
        if (*s == '"' || *s == '\\') put(o, "\\", 1);
        put(o, s, 1);
    }
    puts_(o, "\"]\n");
}

size_t cc_pgn(const cc_pgn_info *in, const ch_move *moves, int n, char *out, size_t cap) {
    out_t o = { out, cap, 0 };
    char num[32];
    tag(&o, "Event", "Casual game");
    tag(&o, "Site", in->site);
    tag(&o, "Date", in->date);
    tag(&o, "Round", "-");
    tag(&o, "White", in->white);
    tag(&o, "Black", in->black);
    const char *result = cc_result_str(in->status, in->winner);
    tag(&o, "Result", result);
    if (in->white_elo) { snprintf(num, sizeof num, "%d", in->white_elo); tag(&o, "WhiteElo", num); }
    if (in->black_elo) { snprintf(num, sizeof num, "%d", in->black_elo); tag(&o, "BlackElo", num); }
    snprintf(num, sizeof num, "%d+%d", in->base_s, in->inc_s);
    tag(&o, "TimeControl", num);
    tag(&o, "Termination", termination(in->status));
    puts_(&o, "\n");
    chess_pos p, q;
    ch_startpos(&p);
    size_t col = 0;
    for (int i = 0; i < n; i++) {
        char tok[24];
        int k = 0;
        if (i % 2 == 0) k = snprintf(tok, sizeof tok, "%d. ", i / 2 + 1);
        char san[8];
        int sl = ch_san(&p, moves[i], san);
        memcpy(tok + k, san, (size_t)sl);
        k += sl;
        if (col && col + 1 + (size_t)k > 80) { put(&o, "\n", 1); col = 0; }
        else if (col) { put(&o, " ", 1); col++; }
        put(&o, tok, (size_t)k);
        col += (size_t)k;
        ch_make(&p, moves[i], &q);
        p = q;
    }
    size_t rl = strlen(result);
    if (col && col + 1 + rl > 80) put(&o, "\n", 1);
    else if (col) put(&o, " ", 1);
    put(&o, result, rl);
    put(&o, "\n", 1);
    if (o.n < cap) out[o.n] = 0;
    else if (cap) out[cap - 1] = 0;
    return o.n;
}
