/* chess.c - see chess.h. Bitboards with ray attacks found by the first blocker on each ray
 * (no magic tables: eight rays per square, 4 KB); copy-make; legality by making the move and
 * asking whether the mover's king is attacked. */
#include "chess.h"

#include <string.h>

static uint64_t KNIGHT[64], KING[64], PAWN_ATT[2][64], RAY[8][64];
static uint64_t Z_PIECE[2][6][64], Z_CASTLE[16], Z_EP[8], Z_STM;
/* The castling rights that survive a move touching each square (a king or rook leaving home, or
 * a rook captured at home). */
static uint8_t CASTLE_KEEP[64];
static int inited;

/* Rays: N, NE, E, NW go up the square numbers (first blocker = lowest set bit); S, SW, W, SE go
 * down (first blocker = highest set bit). */
enum { D_N, D_NE, D_E, D_NW, D_S, D_SW, D_W, D_SE };
static const int DR[8] = { 1, 1, 0, 1, -1, -1, 0, -1 };
static const int DF[8] = { 0, 1, 1, -1, 0, -1, -1, 1 };

static inline int lsb(uint64_t b) { return __builtin_ctzll(b); }
static inline int msb(uint64_t b) { return 63 - __builtin_clzll(b); }
static inline int popcnt(uint64_t b) { return __builtin_popcountll(b); }
#define BIT(s) (1ULL << (s))

static uint64_t splitmix(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

void ch_init(void) {
    if (inited) return;
    for (int s = 0; s < 64; s++) {
        int r = s / 8, f = s % 8;
        static const int KN[8][2] = { {1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2} };
        for (int i = 0; i < 8; i++) {
            int rr = r + KN[i][0], ff = f + KN[i][1];
            if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) KNIGHT[s] |= BIT(rr * 8 + ff);
        }
        for (int dr = -1; dr <= 1; dr++)
            for (int df = -1; df <= 1; df++) {
                int rr = r + dr, ff = f + df;
                if ((dr || df) && rr >= 0 && rr < 8 && ff >= 0 && ff < 8) KING[s] |= BIT(rr * 8 + ff);
            }
        if (r < 7) { if (f > 0) PAWN_ATT[CH_WHITE][s] |= BIT(s + 7); if (f < 7) PAWN_ATT[CH_WHITE][s] |= BIT(s + 9); }
        if (r > 0) { if (f > 0) PAWN_ATT[CH_BLACK][s] |= BIT(s - 9); if (f < 7) PAWN_ATT[CH_BLACK][s] |= BIT(s - 7); }
        for (int d = 0; d < 8; d++)
            for (int rr = r + DR[d], ff = f + DF[d]; rr >= 0 && rr < 8 && ff >= 0 && ff < 8; rr += DR[d], ff += DF[d])
                RAY[d][s] |= BIT(rr * 8 + ff);
        CASTLE_KEEP[s] = 15;
    }
    CASTLE_KEEP[4] &= (uint8_t)~(CH_WK | CH_WQ);
    CASTLE_KEEP[0] &= (uint8_t)~CH_WQ;
    CASTLE_KEEP[7] &= (uint8_t)~CH_WK;
    CASTLE_KEEP[60] &= (uint8_t)~(CH_BK | CH_BQ);
    CASTLE_KEEP[56] &= (uint8_t)~CH_BQ;
    CASTLE_KEEP[63] &= (uint8_t)~CH_BK;
    uint64_t seed = 0x6c69636865737321ULL;   /* fixed: keys are stable across runs */
    for (int c = 0; c < 2; c++) for (int t = 0; t < 6; t++) for (int s = 0; s < 64; s++) Z_PIECE[c][t][s] = splitmix(&seed);
    for (int i = 0; i < 16; i++) Z_CASTLE[i] = splitmix(&seed);
    for (int i = 0; i < 8; i++) Z_EP[i] = splitmix(&seed);
    Z_STM = splitmix(&seed);
    inited = 1;
}

static inline uint64_t ray(int d, int s, uint64_t occ) {
    uint64_t a = RAY[d][s], b = a & occ;
    if (b) a ^= RAY[d][d < 4 ? lsb(b) : msb(b)];
    return a;
}
static inline uint64_t bishop_att(int s, uint64_t occ) { return ray(D_NE, s, occ) | ray(D_NW, s, occ) | ray(D_SE, s, occ) | ray(D_SW, s, occ); }
static inline uint64_t rook_att(int s, uint64_t occ) { return ray(D_N, s, occ) | ray(D_E, s, occ) | ray(D_S, s, occ) | ray(D_W, s, occ); }

static bool attacked(const chess_pos *p, int s, int by) {
    uint64_t occ = p->occ[0] | p->occ[1];
    const uint64_t *b = p->bb[by];
    if (PAWN_ATT[by ^ 1][s] & b[CH_PAWN]) return true;
    if (KNIGHT[s] & b[CH_KNIGHT]) return true;
    if (KING[s] & b[CH_KING]) return true;
    if (bishop_att(s, occ) & (b[CH_BISHOP] | b[CH_QUEEN])) return true;
    if (rook_att(s, occ) & (b[CH_ROOK] | b[CH_QUEEN])) return true;
    return false;
}

bool ch_in_check(const chess_pos *p) {
    uint64_t k = p->bb[p->stm][CH_KING];
    return k && attacked(p, lsb(k), p->stm ^ 1);
}

static inline void put(chess_pos *p, int c, int t, int s) {
    p->bb[c][t] |= BIT(s); p->occ[c] |= BIT(s); p->sq[s] = (uint8_t)(1 + t + 8 * c); p->key ^= Z_PIECE[c][t][s];
}
static inline void take(chess_pos *p, int c, int t, int s) {
    p->bb[c][t] &= ~BIT(s); p->occ[c] &= ~BIT(s); p->sq[s] = 0; p->key ^= Z_PIECE[c][t][s];
}

/* Whether the side to move has a pawn that could capture onto the en-passant square (the
 * repetition key counts the square only then). */
static bool ep_capturable(const chess_pos *p) {
    return p->ep != CH_NO_SQ && (PAWN_ATT[p->stm ^ 1][p->ep] & p->bb[p->stm][CH_PAWN]);
}

static void rekey(chess_pos *p) {
    uint64_t k = 0;
    for (int s = 0; s < 64; s++)
        if (p->sq[s]) { int v = p->sq[s] - 1; k ^= Z_PIECE[v >> 3][v & 7][s]; }
    k ^= Z_CASTLE[p->castle];
    if (ep_capturable(p)) k ^= Z_EP[p->ep & 7];
    if (p->stm) k ^= Z_STM;
    p->key = k;
}

int ch_piece_at(const chess_pos *p, int s, int *color) {
    if (!p->sq[s]) return -1;
    int v = p->sq[s] - 1;
    if (color) *color = v >> 3;
    return v & 7;
}

void ch_sq_name(int s, char out[2]) { out[0] = (char)('a' + s % 8); out[1] = (char)('1' + s / 8); }

void ch_startpos(chess_pos *p) {
    ch_from_fen(p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

static int piece_of_char(char ch, int *color) {
    const char *w = "PNBRQK", *b = "pnbrqk";
    for (int i = 0; i < 6; i++) {
        if (ch == w[i]) { *color = CH_WHITE; return i; }
        if (ch == b[i]) { *color = CH_BLACK; return i; }
    }
    return -1;
}

static const char *parse_uint(const char *s, unsigned *v) {
    if (*s < '0' || *s > '9') return NULL;
    unsigned x = 0;
    while (*s >= '0' && *s <= '9') { x = x * 10 + (unsigned)(*s - '0'); if (x > 65535) return NULL; s++; }
    *v = x;
    return s;
}

int ch_from_fen(chess_pos *p, const char *fen) {
    ch_init();
    memset(p, 0, sizeof *p);
    p->ep = CH_NO_SQ;
    const char *s = fen;
    int r = 7, f = 0;
    for (; *s && *s != ' '; s++) {
        if (*s == '/') { if (f != 8 || r == 0) return -1; r--; f = 0; continue; }
        if (*s >= '1' && *s <= '8') { f += *s - '0'; if (f > 8) return -1; continue; }
        int c, t = piece_of_char(*s, &c);
        if (t < 0 || f > 7) return -1;
        put(p, c, t, r * 8 + f);
        f++;
    }
    if (r != 0 || f != 8 || *s != ' ') return -1;
    s++;
    if (*s == 'w') p->stm = CH_WHITE; else if (*s == 'b') p->stm = CH_BLACK; else return -1;
    s++;
    if (*s != ' ') return -1;
    s++;
    if (*s == '-') s++;
    else
        for (; *s && *s != ' '; s++) {
            switch (*s) {
            case 'K': p->castle |= CH_WK; break;
            case 'Q': p->castle |= CH_WQ; break;
            case 'k': p->castle |= CH_BK; break;
            case 'q': p->castle |= CH_BQ; break;
            default: return -1;
            }
        }
    /* a right without its king and rook at home is dropped */
    if (!(p->sq[4] == 1 + CH_KING && p->sq[7] == 1 + CH_ROOK)) p->castle &= (uint8_t)~CH_WK;
    if (!(p->sq[4] == 1 + CH_KING && p->sq[0] == 1 + CH_ROOK)) p->castle &= (uint8_t)~CH_WQ;
    if (!(p->sq[60] == 9 + CH_KING && p->sq[63] == 9 + CH_ROOK)) p->castle &= (uint8_t)~CH_BK;
    if (!(p->sq[60] == 9 + CH_KING && p->sq[56] == 9 + CH_ROOK)) p->castle &= (uint8_t)~CH_BQ;
    p->halfmove = 0; p->fullmove = 1;
    if (*s == ' ') {
        s++;
        if (*s == '-') s++;
        else {
            if (s[0] < 'a' || s[0] > 'h' || (s[1] != '3' && s[1] != '6')) return -1;
            p->ep = (uint8_t)((s[1] - '1') * 8 + (s[0] - 'a'));
            s += 2;
        }
        unsigned v;
        if (*s == ' ') { s++; if (!(s = parse_uint(s, &v))) return -1; p->halfmove = (uint16_t)v; }
        if (*s == ' ') { s++; if (!(s = parse_uint(s, &v))) return -1; p->fullmove = (uint16_t)(v ? v : 1); }
    }
    if (*s) return -1;
    if (popcnt(p->bb[0][CH_KING]) != 1 || popcnt(p->bb[1][CH_KING]) != 1) return -1;
    rekey(p);
    return 0;
}

static int write_board(const chess_pos *p, char *o) {
    static const char PC[2][6] = { { 'P', 'N', 'B', 'R', 'Q', 'K' }, { 'p', 'n', 'b', 'r', 'q', 'k' } };
    char *o0 = o;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            int c, t = ch_piece_at(p, r * 8 + f, &c);
            if (t < 0) { empty++; continue; }
            if (empty) { *o++ = (char)('0' + empty); empty = 0; }
            *o++ = PC[c][t];
        }
        if (empty) *o++ = (char)('0' + empty);
        if (r) *o++ = '/';
    }
    *o++ = ' ';
    *o++ = p->stm ? 'b' : 'w';
    *o++ = ' ';
    if (!p->castle) *o++ = '-';
    if (p->castle & CH_WK) *o++ = 'K';
    if (p->castle & CH_WQ) *o++ = 'Q';
    if (p->castle & CH_BK) *o++ = 'k';
    if (p->castle & CH_BQ) *o++ = 'q';
    *o++ = ' ';
    bool ep = false;
    if (ep_capturable(p)) {   /* written only when a legal en-passant capture exists */
        ch_moves ms;
        ch_legal(p, &ms);
        for (int i = 0; i < ms.n; i++)
            if (CH_TO(ms.m[i]) == p->ep && (p->bb[p->stm][CH_PAWN] & BIT(CH_FROM(ms.m[i])))) { ep = true; break; }
    }
    if (ep) { ch_sq_name(p->ep, o); o += 2; } else *o++ = '-';
    return (int)(o - o0);
}

int ch_to_fen_board(const chess_pos *p, char *out) { int n = write_board(p, out); out[n] = 0; return n; }

static char *put_uint(char *o, unsigned v) {
    char t[8]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *o++ = t[--n];
    return o;
}

int ch_to_fen(const chess_pos *p, char *out) {
    char *o = out + write_board(p, out);
    *o++ = ' ';
    o = put_uint(o, p->halfmove);
    *o++ = ' ';
    o = put_uint(o, p->fullmove);
    *o = 0;
    return (int)(o - out);
}

/* ---- moves ---- */

static void add_targets(ch_moves *out, int from, uint64_t t) {
    while (t) { int to = lsb(t); t &= t - 1; out->m[out->n++] = CH_MOVE(from, to, 0); }
}

static void pseudo(const chess_pos *p, ch_moves *out) {
    int us = p->stm, them = us ^ 1;
    uint64_t own = p->occ[us], opp = p->occ[them], occ = own | opp;
    const uint64_t *b = p->bb[us];
    out->n = 0;
    /* pawns */
    uint64_t pawns = b[CH_PAWN];
    int fwd = us == CH_WHITE ? 8 : -8;
    int promo_rank = us == CH_WHITE ? 7 : 0, start_rank = us == CH_WHITE ? 1 : 6;
    uint64_t ep_bb = p->ep != CH_NO_SQ ? BIT(p->ep) : 0;
    while (pawns) {
        int s = lsb(pawns); pawns &= pawns - 1;
        uint64_t t = PAWN_ATT[us][s] & (opp | ep_bb);
        int one = s + fwd;
        if (!(occ & BIT(one))) {
            t |= BIT(one);
            if (s / 8 == start_rank && !(occ & BIT(one + fwd))) t |= BIT(one + fwd);
        }
        while (t) {
            int to = lsb(t); t &= t - 1;
            if (to / 8 == promo_rank)
                for (int pr = CH_QUEEN; pr >= CH_KNIGHT; pr--) out->m[out->n++] = CH_MOVE(s, to, pr);
            else out->m[out->n++] = CH_MOVE(s, to, 0);
        }
    }
    uint64_t x;
    for (x = b[CH_KNIGHT]; x; x &= x - 1) { int s = lsb(x); add_targets(out, s, KNIGHT[s] & ~own); }
    for (x = b[CH_BISHOP]; x; x &= x - 1) { int s = lsb(x); add_targets(out, s, bishop_att(s, occ) & ~own); }
    for (x = b[CH_ROOK]; x; x &= x - 1) { int s = lsb(x); add_targets(out, s, rook_att(s, occ) & ~own); }
    for (x = b[CH_QUEEN]; x; x &= x - 1) { int s = lsb(x); add_targets(out, s, (bishop_att(s, occ) | rook_att(s, occ)) & ~own); }
    int k = lsb(b[CH_KING]);
    add_targets(out, k, KING[k] & ~own);
    /* castling: rights, an empty path, and the king not in, through or into check */
    if (us == CH_WHITE) {
        if ((p->castle & CH_WK) && !(occ & (BIT(5) | BIT(6))) && !attacked(p, 4, them) && !attacked(p, 5, them) && !attacked(p, 6, them))
            out->m[out->n++] = CH_MOVE(4, 6, 0);
        if ((p->castle & CH_WQ) && !(occ & (BIT(1) | BIT(2) | BIT(3))) && !attacked(p, 4, them) && !attacked(p, 3, them) && !attacked(p, 2, them))
            out->m[out->n++] = CH_MOVE(4, 2, 0);
    } else {
        if ((p->castle & CH_BK) && !(occ & (BIT(61) | BIT(62))) && !attacked(p, 60, them) && !attacked(p, 61, them) && !attacked(p, 62, them))
            out->m[out->n++] = CH_MOVE(60, 62, 0);
        if ((p->castle & CH_BQ) && !(occ & (BIT(57) | BIT(58) | BIT(59))) && !attacked(p, 60, them) && !attacked(p, 59, them) && !attacked(p, 58, them))
            out->m[out->n++] = CH_MOVE(60, 58, 0);
    }
}

void ch_make(const chess_pos *p, ch_move m, chess_pos *o) {
    *o = *p;
    int from = CH_FROM(m), to = CH_TO(m), pr = CH_PROMO(m);
    int us = p->stm, them = us ^ 1;
    int t = (p->sq[from] - 1) & 7;
    int cap = p->sq[to] ? ((p->sq[to] - 1) & 7) : -1;
    o->key ^= Z_CASTLE[p->castle];
    if (ep_capturable(p)) o->key ^= Z_EP[p->ep & 7];
    if (cap >= 0) take(o, them, cap, to);
    take(o, us, t, from);
    put(o, us, pr ? pr : t, to);
    o->ep = CH_NO_SQ;
    if (t == CH_PAWN) {
        if (to == p->ep) take(o, them, CH_PAWN, to + (us == CH_WHITE ? -8 : 8));
        else if (to - from == 16 || from - to == 16) o->ep = (uint8_t)((from + to) / 2);
    } else if (t == CH_KING && (to - from == 2 || from - to == 2)) {
        int rf = to > from ? from + 3 : from - 4, rt = to > from ? from + 1 : from - 1;
        take(o, us, CH_ROOK, rf);
        put(o, us, CH_ROOK, rt);
    }
    o->castle = p->castle & CASTLE_KEEP[from] & CASTLE_KEEP[to];
    o->halfmove = (t == CH_PAWN || cap >= 0) ? 0 : (uint16_t)(p->halfmove + 1);
    if (us == CH_BLACK) o->fullmove++;
    o->stm = (uint8_t)them;
    o->key ^= Z_STM ^ Z_CASTLE[o->castle];
    if (ep_capturable(o)) o->key ^= Z_EP[o->ep & 7];
}

static inline bool leaves_king_safe(const chess_pos *after) {
    /* after the move it is the opponent's turn: the mover is after->stm ^ 1 */
    int mover = after->stm ^ 1;
    return !attacked(after, lsb(after->bb[mover][CH_KING]), after->stm);
}

void ch_legal(const chess_pos *p, ch_moves *out) {
    ch_moves ps;
    pseudo(p, &ps);
    out->n = 0;
    chess_pos q;
    for (int i = 0; i < ps.n; i++) {
        ch_make(p, ps.m[i], &q);
        if (leaves_king_safe(&q)) out->m[out->n++] = ps.m[i];
    }
}

bool ch_is_legal(const chess_pos *p, ch_move m) {
    ch_moves ms;
    ch_legal(p, &ms);
    for (int i = 0; i < ms.n; i++) if (ms.m[i] == m) return true;
    return false;
}

uint64_t ch_perft(const chess_pos *p, int depth) {
    ch_moves ms;
    ch_legal(p, &ms);
    if (depth <= 1) return depth == 1 ? (uint64_t)ms.n : 1;
    uint64_t n = 0;
    chess_pos q;
    for (int i = 0; i < ms.n; i++) { ch_make(p, ms.m[i], &q); n += ch_perft(&q, depth - 1); }
    return n;
}

/* ---- notation ---- */

int ch_uci(ch_move m, char *out) {
    ch_sq_name(CH_FROM(m), out);
    ch_sq_name(CH_TO(m), out + 2);
    if (CH_PROMO(m)) { out[4] = "?nbrq"[CH_PROMO(m)]; out[5] = 0; return 5; }
    out[4] = 0;
    return 4;
}

static int sq_parse(const char *s) {
    if (s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') return -1;
    return (s[1] - '1') * 8 + (s[0] - 'a');
}

ch_move ch_parse_uci(const chess_pos *p, const char *s, size_t n) {
    if (n != 4 && n != 5) return CH_MOVE_NONE;
    int from = sq_parse(s), to = sq_parse(s + 2), pr = 0;
    if (from < 0 || to < 0) return CH_MOVE_NONE;
    if (n == 5) {
        switch (s[4]) {
        case 'n': pr = CH_KNIGHT; break;
        case 'b': pr = CH_BISHOP; break;
        case 'r': pr = CH_ROOK; break;
        case 'q': pr = CH_QUEEN; break;
        default: return CH_MOVE_NONE;
        }
    }
    int c, t = ch_piece_at(p, from, &c), tc, tt = ch_piece_at(p, to, &tc);
    /* the king taking its own rook is castling toward that rook */
    if (t == CH_KING && tt == CH_ROOK && c == tc && c == p->stm && from / 8 == to / 8 && (from == 4 || from == 60))
        to = to > from ? from + 2 : from - 2;
    ch_move m = CH_MOVE(from, to, pr);
    return ch_is_legal(p, m) ? m : CH_MOVE_NONE;
}

int ch_san(const chess_pos *p, ch_move m, char *out) {
    int from = CH_FROM(m), to = CH_TO(m), pr = CH_PROMO(m);
    int t = ch_piece_at(p, from, NULL);
    bool capture = p->sq[to] != 0 || (t == CH_PAWN && to == p->ep);
    char *o = out;
    if (t == CH_KING && (to - from == 2 || from - to == 2)) {
        memcpy(o, to > from ? "O-O" : "O-O-O", to > from ? 3 : 5);
        o += to > from ? 3 : 5;
    } else if (t == CH_PAWN) {
        if (capture) { *o++ = (char)('a' + from % 8); *o++ = 'x'; }
        ch_sq_name(to, o); o += 2;
        if (pr) { *o++ = '='; *o++ = "PNBRQK"[pr]; }
    } else {
        *o++ = "PNBRQK"[t];
        ch_moves ms;
        ch_legal(p, &ms);
        bool other = false, same_file = false, same_rank = false;
        for (int i = 0; i < ms.n; i++) {
            int f2 = CH_FROM(ms.m[i]);
            if (f2 == from || CH_TO(ms.m[i]) != to || ch_piece_at(p, f2, NULL) != t) continue;
            other = true;
            if (f2 % 8 == from % 8) same_file = true;
            if (f2 / 8 == from / 8) same_rank = true;
        }
        if (other) {
            if (!same_file) *o++ = (char)('a' + from % 8);
            else if (!same_rank) *o++ = (char)('1' + from / 8);
            else { *o++ = (char)('a' + from % 8); *o++ = (char)('1' + from / 8); }
        }
        if (capture) *o++ = 'x';
        ch_sq_name(to, o); o += 2;
    }
    chess_pos q;
    ch_make(p, m, &q);
    if (ch_in_check(&q)) {
        ch_moves ms;
        ch_legal(&q, &ms);
        *o++ = ms.n ? '+' : '#';
    }
    *o = 0;
    return (int)(o - out);
}

/* ---- material ---- */

static const uint64_t LIGHT = 0x55AA55AA55AA55AAULL;   /* the light squares (h1 = 7 is light) */

bool ch_insufficient(const chess_pos *p) {
    for (int c = 0; c < 2; c++)
        if (p->bb[c][CH_PAWN] | p->bb[c][CH_ROOK] | p->bb[c][CH_QUEEN]) return false;
    uint64_t kn = p->bb[0][CH_KNIGHT] | p->bb[1][CH_KNIGHT];
    uint64_t bi = p->bb[0][CH_BISHOP] | p->bb[1][CH_BISHOP];
    if (popcnt(kn) + popcnt(bi) <= 1) return true;
    if (kn) return false;
    return !(bi & LIGHT) || !(bi & ~LIGHT);
}

bool ch_insufficient_for(const chess_pos *p, int color) {
    const uint64_t *b = p->bb[color];
    if (b[CH_PAWN] | b[CH_ROOK] | b[CH_QUEEN]) return false;
    return popcnt(b[CH_KNIGHT]) + popcnt(b[CH_BISHOP]) <= 1;
}
