/* game.c - see game.h. */
#include "game.h"

#include <string.h>

void cc_game_new(cc_game *g, int32_t base_ms, int32_t inc_ms, int64_t now) {
    memset(g, 0, sizeof *g);
    ch_startpos(&g->pos);
    g->status = CC_PLAYING;
    g->winner = CC_NO_WINNER;
    g->base_ms = base_ms;
    g->inc_ms = inc_ms;
    g->left_ms[0] = g->left_ms[1] = base_ms;
    g->turn_at = now;
    g->created_at = now;
    g->rep[0] = g->pos.key;
}

bool cc_game_over(const cc_game *g) { return g->status != CC_PLAYING; }

bool cc_game_clock_running(const cc_game *g) { return g->status == CC_PLAYING && g->ply >= 2; }

int64_t cc_game_left(const cc_game *g, int color, int64_t now) {
    int64_t t = g->left_ms[color];
    if (cc_game_clock_running(g) && color == g->pos.stm) t -= now - g->turn_at;
    return t < 0 ? 0 : t;
}

static void end(cc_game *g, int status, int winner, int64_t now) {
    g->status = (uint8_t)status;
    g->winner = (uint8_t)winner;
    g->ended_at = now;
    g->draw_offer = 0;
}

/* The side to move ran out of time at `now`: it loses unless the other side cannot mate. */
static void flag(cc_game *g, int64_t now) {
    int loser = g->pos.stm;
    g->left_ms[loser] = 0;
    if (ch_insufficient_for(&g->pos, loser ^ 1)) end(g, CC_DRAW_FLAG_MATERIAL, CC_NO_WINNER, now);
    else end(g, CC_FLAG, loser ^ 1, now);
}

static bool flagged(const cc_game *g, int64_t now) {
    return cc_game_clock_running(g) && g->left_ms[g->pos.stm] - (now - g->turn_at) <= 0;
}

/* Threefold: the current key appears twice more among the positions since the last irreversible
 * move, with the same side to move (every second ply back). */
static bool threefold(const cc_game *g) {
    uint64_t k = g->pos.key;
    int seen = 1, back = g->pos.halfmove;
    if (back > g->ply) back = g->ply;
    for (int d = 2; d <= back; d += 2)
        if (g->rep[(g->ply - d) % CC_REP_RING] == k && ++seen >= 3) return true;
    return false;
}

int cc_game_move(cc_game *g, int color, int ply, ch_move m, int64_t now) {
    if (cc_game_over(g)) return CC_E_OVER;
    if (color != g->pos.stm) return CC_E_TURN;
    if (ply != g->ply) return CC_E_PLY;
    if (!ch_is_legal(&g->pos, m)) return CC_E_ILLEGAL;
    if (flagged(g, now)) { flag(g, now); return CC_E_OVER; }
    if (cc_game_clock_running(g)) {
        g->left_ms[color] -= now - g->turn_at;
        g->left_ms[color] += g->inc_ms;
    }
    chess_pos next;
    ch_make(&g->pos, m, &next);
    g->pos = next;
    g->moves[g->ply] = m;
    g->ply++;
    g->rep[g->ply % CC_REP_RING] = g->pos.key;
    g->turn_at = now;
    g->draw_offer &= (uint8_t)(1u << color);   /* the mover's own offer stands; the other's is declined */
    if (!ch_has_legal(&g->pos)) {
        if (ch_in_check(&g->pos)) end(g, CC_MATE, color, now);
        else end(g, CC_STALEMATE, CC_NO_WINNER, now);
    } else if (ch_insufficient(&g->pos)) end(g, CC_DRAW_MATERIAL, CC_NO_WINNER, now);
    else if (threefold(g)) end(g, CC_DRAW_REPETITION, CC_NO_WINNER, now);
    else if (g->pos.halfmove >= 100) end(g, CC_DRAW_FIFTY, CC_NO_WINNER, now);
    else if (g->ply >= CC_MAX_PLY) end(g, CC_DRAW_MAX_PLY, CC_NO_WINNER, now);
    return CC_OK;
}

bool cc_game_tick(cc_game *g, int64_t now, int64_t *next_ms) {
    bool ended = false;
    if (!cc_game_over(g)) {
        if (g->ply < 2) {
            if (now - g->turn_at >= CC_FIRST_MOVE_MS) { end(g, CC_ABORTED, CC_NO_WINNER, now); ended = true; }
        } else if (flagged(g, now)) {
            flag(g, now);
            ended = true;
        }
    }
    if (next_ms) {
        if (cc_game_over(g)) *next_ms = -1;
        else if (g->ply < 2) *next_ms = CC_FIRST_MOVE_MS - (now - g->turn_at);
        else *next_ms = g->left_ms[g->pos.stm] - (now - g->turn_at);
        if (next_ms && *next_ms < 0 && !cc_game_over(g)) *next_ms = 0;
    }
    return ended;
}

int cc_game_resign(cc_game *g, int color, int64_t now) {
    if (cc_game_over(g)) return CC_E_OVER;
    if (flagged(g, now)) { flag(g, now); return CC_E_OVER; }
    end(g, CC_RESIGN, color ^ 1, now);
    return CC_OK;
}

int cc_game_abort(cc_game *g, int color, int64_t now) {
    if (cc_game_over(g)) return CC_E_OVER;
    if (g->ply >= 2) return CC_E_STATE;
    end(g, CC_ABORTED, CC_NO_WINNER, now);
    return CC_OK;
}

int cc_game_draw(cc_game *g, int color, int64_t now) {
    if (cc_game_over(g)) return CC_E_OVER;
    if (flagged(g, now)) { flag(g, now); return CC_E_OVER; }
    if (g->draw_offer & (1u << (color ^ 1))) { end(g, CC_DRAW_AGREED, CC_NO_WINNER, now); return CC_OK; }
    if (g->draw_offer & (1u << color)) return CC_E_STATE;
    g->draw_offer |= (uint8_t)(1u << color);
    return CC_OK;
}

int cc_game_draw_decline(cc_game *g, int color) {
    if (cc_game_over(g)) return CC_E_OVER;
    if (!(g->draw_offer & (1u << (color ^ 1)))) return CC_E_STATE;
    g->draw_offer &= (uint8_t)~(1u << (color ^ 1));
    return CC_OK;
}

double cc_game_white_score(const cc_game *g) {
    if (g->winner == CH_WHITE) return 1.0;
    if (g->winner == CH_BLACK) return 0.0;
    return 0.5;
}

int cc_game_replay(cc_game *g, const ch_move *moves, int n) {
    for (int i = 0; i < n; i++) {
        int rc = cc_game_move(g, g->pos.stm, g->ply, moves[i], g->turn_at);
        if (rc != CC_OK) return rc;
    }
    return CC_OK;
}
