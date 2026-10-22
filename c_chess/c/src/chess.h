/* chess.h - standard chess rules for the round server: a position, legal moves, making a move,
 * FEN in and out, UCI and SAN, and the end-of-game tests (mate, stalemate, the fifty-move and
 * seventy-five-move counters, repetition, insufficient material).
 *
 * Written for this project from the rules of chess (FIDE Laws, Article 3 and 9) and checked by
 * perft against the published node counts (test/unit_chess.c); no other engine's code is in it.
 *
 * Squares are 0..63, a1 = 0, b1 = 1, ..., h8 = 63. A move is 16 bits: from (6), to (6), and the
 * promotion piece (3: 0 none, else CH_KNIGHT..CH_QUEEN). Castling is the king's two-square move
 * (e1g1); en passant is the pawn's diagonal move to the empty en-passant square. Copy-make: a
 * move is made into a new chess_pos, so there is no unmake and no history inside the position. */
#ifndef LICHESS_ROUND_CHESS_H
#define LICHESS_ROUND_CHESS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum { CH_WHITE = 0, CH_BLACK = 1 };
enum { CH_PAWN = 0, CH_KNIGHT = 1, CH_BISHOP = 2, CH_ROOK = 3, CH_QUEEN = 4, CH_KING = 5 };
enum { CH_WK = 1, CH_WQ = 2, CH_BK = 4, CH_BQ = 8 };   /* castling rights */

#define CH_NO_SQ 64
#define CH_MAX_MOVES 256

typedef uint16_t ch_move;
#define CH_MOVE(from, to, promo) ((ch_move)((from) | ((to) << 6) | ((promo) << 12)))
#define CH_FROM(m) ((int)((m) & 63))
#define CH_TO(m) ((int)(((m) >> 6) & 63))
#define CH_PROMO(m) ((int)(((m) >> 12) & 7))
#define CH_MOVE_NONE ((ch_move)0)

typedef struct {
    uint64_t bb[2][6];    /* pieces by color and type */
    uint64_t occ[2];      /* all pieces of a color */
    uint8_t sq[64];       /* 0 empty, else 1 + type + 8 * color */
    uint8_t stm;          /* side to move */
    uint8_t castle;       /* CH_WK | CH_WQ | CH_BK | CH_BQ */
    uint8_t ep;           /* the en-passant target square when a pawn just moved two, else CH_NO_SQ */
    uint8_t pad;
    uint16_t halfmove;    /* plies since the last capture or pawn move */
    uint16_t fullmove;    /* starts at 1, incremented after Black moves */
    uint64_t key;         /* Zobrist hash of the position for repetition (ep only when capturable) */
} chess_pos;

typedef struct { ch_move m[CH_MAX_MOVES]; int n; } ch_moves;

void ch_init(void);                         /* the tables; idempotent, call once at start */
void ch_startpos(chess_pos *p);
/* FEN. Parse returns 0 on success, -1 on malformed input (the position is then unspecified).
 * Write produces the six fields, with the en-passant square only when a legal en-passant capture
 * exists (as lichess writes it), into out (at least 92 bytes); returns the length. */
int  ch_from_fen(chess_pos *p, const char *fen);
int  ch_to_fen(const chess_pos *p, char *out);
/* Only the board, side, castling and en-passant fields (the repetition identity of a position). */
int  ch_to_fen_board(const chess_pos *p, char *out);

void ch_legal(const chess_pos *p, ch_moves *out);
bool ch_in_check(const chess_pos *p);
void ch_make(const chess_pos *p, ch_move m, chess_pos *out);   /* m must be legal */
bool ch_is_legal(const chess_pos *p, ch_move m);
uint64_t ch_perft(const chess_pos *p, int depth);

/* UCI: "e2e4", "e7e8q"; castling may also be given as the king taking its own rook ("e1h1").
 * Parse returns the legal move or CH_MOVE_NONE. Write returns the length (4 or 5). */
ch_move ch_parse_uci(const chess_pos *p, const char *s, size_t n);
int  ch_uci(ch_move m, char *out);
/* SAN of a legal move in p, with + or #; returns the length (at most 7). */
int  ch_san(const chess_pos *p, ch_move m, char *out);

/* The piece on a square: -1 empty, else type; color in *color. */
int  ch_piece_at(const chess_pos *p, int sq, int *color);
void ch_sq_name(int sq, char out[2]);

/* Insufficient material, as lichess judges it in standard chess: the game is drawn when neither
 * side can mate by any series of legal moves (K v K, K+N v K, K+B v K, and kings with bishops
 * that all stand on one color). */
bool ch_insufficient(const chess_pos *p);
/* Whether `color` alone has too little material to ever mate (used when the other side flags). */
bool ch_insufficient_for(const chess_pos *p, int color);

#endif
