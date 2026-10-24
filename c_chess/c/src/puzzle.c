/* puzzle.c - see puzzle.h. */
#include "puzzle.h"

#include <string.h>

bool cp_accepts(const chess_pos *p, ch_move expected, ch_move played) {
    if (!ch_is_legal(p, played)) return false;
    if (played == expected) return true;
    chess_pos q;
    ch_make(p, played, &q);
    return ch_in_check(&q) && !ch_has_legal(&q);
}

int cp_begin(cp_solve *s, const cp_puzzle *p) {
    memset(s, 0, sizeof *s);
    s->p = *p;
    if (p->n < 2 || p->n > CP_MOVES) return -1;
    char fen[sizeof p->fen + 1];   /* a record's FEN need not end in a NUL inside the record */
    memcpy(fen, p->fen, sizeof p->fen);
    fen[sizeof p->fen] = 0;
    if (ch_from_fen(&s->pos, fen) != 0) return -1;
    chess_pos a = s->pos, b;
    for (int i = 0; i < p->n; i++) {          /* the whole line must be legal */
        if (!ch_is_legal(&a, p->moves[i])) return -1;
        ch_make(&a, p->moves[i], &b);
        a = b;
    }
    ch_make(&s->pos, p->moves[0], &b);
    s->pos = b;
    s->at = 1;
    s->state = CP_SOLVING;
    return 0;
}

int cp_try(cp_solve *s, ch_move m) {
    if (s->state != CP_SOLVING) return s->state;
    if (!cp_accepts(&s->pos, s->p.moves[s->at], m)) { s->state = CP_FAILED; return s->state; }
    chess_pos q;
    ch_make(&s->pos, m, &q);
    s->pos = q;
    s->last[0] = m;
    s->last[1] = CH_MOVE_NONE;
    if (m != s->p.moves[s->at] || s->at + 1 >= s->p.n) {   /* another mate, or the line's end */
        s->at = s->p.n;
        s->state = CP_SOLVED;
        return s->state;
    }
    s->last[1] = s->p.moves[s->at + 1];
    ch_make(&s->pos, s->last[1], &q);
    s->pos = q;
    s->at += 2;
    if (s->at >= s->p.n) { s->state = CP_SOLVED; }
    return s->state;
}

bool cp_reveal(cp_solve *s, ch_move *played) {
    if (s->at < 1 || s->at >= s->p.n) return false;
    chess_pos q;
    ch_make(&s->pos, s->p.moves[s->at], &q);
    s->pos = q;
    *played = s->p.moves[s->at++];
    return true;
}
