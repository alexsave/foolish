// msg_flow_sim.c — Monte-Carlo of the iMessage SWIFT layer over the real
// kernel, via the same fio_* bridge the extension links. Build + run:
//
//   make msg-flow-sim            (or: ./build/msg_flow_sim <trials> <humans> <moves 0|1>)
//
// Ports, line for line, the decisions the Swift surface makes — so schedule
// races the simulator proves safe here stay safe there, and a Swift refactor
// that drifts from these ports should update BOTH. Ported pieces:
//   MessageSurfaceRouter.resolve       (Rule P routing vs the cached row)
//   SeatIdentity.resolve / resolveInLobby / cacheDisownedByJoins /
//     seatClaimedByName                (DM gate, ghost guard, name recovery)
//   LobbyControls.offered              (incl. the M9 authorship gate)
//   NicknameGate.isTaken               (per-chain name uniqueness)
//   GameSurface.maybeAdoptIncoming     (adopt-on-arrival, Rule P deciding)
//   joinLobby / startFromLobby         (lowest-free claim; reseat-at-count)
//
// This is the file that caught the double-claim liveness stall (a player
// spectating their own game after re-claiming across forks) that
// seatClaimedByName now closes.
//
// Monte-Carlo of the LATEST branch's Swift layer over the real
// kernel: lobby v3 (open capacity, Start-at-join-count via reseat), the
// MessageSurfaceRouter's Rule P routing, LobbyControls with the M9 authorship
// gate, SeatIdentity with the DM gate + ghost-seat disown, the name-taken join
// gate, and adopt-on-arrival. Random schedules: stale taps, racing joins,
// racing Starts, evaporated sends (§17.2), reopens.
//
// INVARIANTS
//  S (safety, checked at every render): a device on a BOARD with viewer v is
//    rendering chain C ⇒ C.joins[v].name == the device's own name. Anything
//    else is literally showing someone else's hand.
//  C (convergence, at quiescence): after everyone taps the newest and the
//    canonical bubble, every device that renders a board renders the Rule-P
//    canonical chain.
//  L (liveness, at quiescence): if the canonical chain is LIVE at turn 0, the
//    device named at its first-attacker seat is on that board, at that seat,
//    with a legal move — the original deadlock cannot recur.
//  X (exclusion): a device whose name is NOT in the canonical roster never
//    renders a board (lobby-full / spectator / lobby only).
#include "ios_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#define MAXD 9
#define MAXB 96
#define PLEN 2048

typedef struct { uint8_t p[PLEN]; int len; int sender; } Bubble;
typedef struct { int have; int mySeat; char claimName[8]; uint8_t payload[PLEN]; int plen; } Row;
typedef enum { UI_NONE, UI_LOBBY, UI_BOARD, UI_SPECT } UiKind;
typedef struct { UiKind kind; int viewer; uint8_t chain[PLEN]; int clen; } Ui;

static int N;                        // humans in the chat
static int CAP;                      // lobby capacity (8 group, 2 dm)
static Bubble tr_[MAXB]; static int nb;
static Row row[MAXD];
static Ui ui[MAXD];
static char dname[MAXD][8];
static uint64_t GID;
static unsigned char meta[4096];
static int fails, s_viol, x_viol, c_viol, l_viol, limbo;
static int trial;

typedef struct {
    int phase, np, la, turn, n_joins;
    uint8_t digest[32];
    int jseat[8]; char jname[8][8];
} Env;

static uint32_t rng;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static int dec(const uint8_t *p, int len, Env *e) {
    int rc = fio_msg_decode_packed(p, len, meta, sizeof meta);
    if (rc <= 0) return rc;
    if (e) {
        e->phase = meta[0]; e->np = meta[1]; e->la = meta[2];
        e->turn = meta[4] | (meta[5] << 8);
        memcpy(e->digest, meta + 22, 32);
        e->n_joins = meta[54];
        int q = 55;
        for (int i = 0; i < e->n_joins && i < 8; i++) {
            e->jseat[i] = meta[q]; int nl = meta[q + 1]; q += 2;
            memcpy(e->jname[i], meta + q, nl); e->jname[i][nl] = 0; q += nl;
        }
    }
    return rc;
}

static const char *join_name_at(const Env *e, int seat) {
    for (int i = 0; i < e->n_joins; i++) if (e->jseat[i] == seat) return e->jname[i];
    return NULL;
}

static const char *jjson(const Env *e, int extra_seat, const char *extra_name) {
    static char j[512]; int off = 0, done = 0, first = 1;
    off += snprintf(j + off, sizeof j - off, "[");
    for (int s = 0; s < 8; s++) {
        const char *nm = join_name_at(e, s);
        if (nm) { off += snprintf(j + off, sizeof j - off, "%s{\"seat\":%d,\"name\":\"%s\"}", first ? "" : ",", s, nm); first = 0; }
        else if (s == extra_seat && !done) {
            off += snprintf(j + off, sizeof j - off, "%s{\"seat\":%d,\"name\":\"%s\"}", first ? "" : ",", s, extra_name);
            first = 0; done = 1;
        }
    }
    snprintf(j + off, sizeof j - off, "]");
    return j;
}

static void put_row(int d, int seat, const char *name, const uint8_t *p, int len) {
    row[d].have = 1; row[d].mySeat = seat;
    snprintf(row[d].claimName, sizeof row[d].claimName, "%s", name);
    memcpy(row[d].payload, p, len); row[d].plen = len;
}

static void deliver(const uint8_t *p, int len, int sender) {
    if (nb >= MAXB) return;
    memcpy(tr_[nb].p, p, len); tr_[nb].len = len; tr_[nb].sender = sender; nb++;
}

// ---- SeatIdentity ports (exact) -------------------------------------------
static int disowned(int d, const Env *e) {           // cacheDisownedByJoins
    if (!row[d].have) return 0;
    const char *listed = join_name_at(e, row[d].mySeat);
    if (!listed || !row[d].claimName[0]) return 0;
    return strcmp(listed, row[d].claimName) != 0;
}
static int seat_by_name(int d, const Env *e) {       // seatClaimedByName
    if (!row[d].have || !row[d].claimName[0]) return -1;
    for (int i = 0; i < e->n_joins; i++)
        if (!strcmp(e->jname[i], row[d].claimName)) return e->jseat[i];
    return -1;
}
static int resolve_board(int d, const Env *e, int senderIsLocal) {   // -1 ambiguous
    int byname = seat_by_name(d, e);
    int cached = byname >= 0 ? byname
               : (row[d].have && !disowned(d, e)) ? row[d].mySeat : -1;
    if (cached >= 0 && cached < e->np) return cached;
    if (senderIsLocal && e->la >= 0 && e->la < e->np) return e->la;
    // chatIsDM=false throughout (group sim): the 2p branch never fires.
    return -1;
}
static int resolve_lobby(int d, const Env *e, int senderIsLocal) {   // -1 not joined
    int s = resolve_board(d, e, senderIsLocal);
    if (s < 0) return -1;
    return join_name_at(e, s) ? s : -1;
}
// LobbyControls.offered (exact port)
enum { LC_START, LC_INVITE, LC_WAIT, LC_JOIN, LC_FULL };
static int lc_offered(int mySeat, int joined, int capacity, int iSent) {
    if (mySeat >= 0) {
        if (joined >= 2) { if (iSent && joined < capacity) return LC_WAIT; return LC_START; }
        return iSent ? LC_WAIT : LC_INVITE;
    }
    return joined < capacity ? LC_JOIN : LC_FULL;
}
static int name_taken(const Env *e, const char *nm) {   // NicknameGate.isTaken
    for (int i = 0; i < e->n_joins; i++) if (!strcmp(e->jname[i], nm)) return 1;
    return 0;
}

// ---- render helpers with the SAFETY invariant ------------------------------
static void show_board(int d, const uint8_t *p, int len, int viewer, const Env *e) {
    const char *nm = join_name_at(e, viewer);
    if (!nm || strcmp(nm, dname[d]) != 0) {
        printf("trial %d: SAFETY: %s renders seat %d ('%s') of a chain\n",
               trial, dname[d], viewer, nm ? nm : "?");
        s_viol++; fails++;
    }
    ui[d].kind = UI_BOARD; ui[d].viewer = viewer;
    memcpy(ui[d].chain, p, len); ui[d].clen = len;
}
static void show_lobby(int d, const uint8_t *p, int len) {
    ui[d].kind = UI_LOBBY; ui[d].viewer = -1;
    memcpy(ui[d].chain, p, len); ui[d].clen = len;
}

// ---- the surface: router + adopt + lobby actions ---------------------------
static void act_maybe(int d);   // fwd

static void open_surface(int d, const uint8_t *tapped, int tlen, int sender, int evaporate) {
    Env te;
    if (dec(tapped, tlen, &te) <= 0) return;
    int senderIsLocal = (sender == d);

    // MessageSurfaceRouter.resolve: Rule P against the row for this game.
    const uint8_t *win = tapped; int wlen = tlen;
    if (row[d].have && (row[d].plen != tlen || memcmp(row[d].payload, tapped, tlen) != 0)) {
        if (fio_msg_rule_p(row[d].payload, row[d].plen, tapped, tlen) < 0) {
            win = row[d].payload; wlen = row[d].plen;
        }
    }
    Env we;
    if (dec(win, wlen, &we) <= 0) return;

    if (we.phase == 0) {
        // LOBBY. lobbySeat -> LobbyControls.
        int seat = resolve_lobby(d, &we, senderIsLocal);
        int offered = lc_offered(seat, we.n_joins, we.np, seat >= 0 && we.la == seat);
        show_lobby(d, win, wlen);
        if (offered == LC_JOIN) {
            int free_ = -1;
            for (int s = 0; s < we.np && free_ < 0; s++) if (!join_name_at(&we, s)) free_ = s;
            if (free_ < 0) return;
            if (name_taken(&we, dname[d])) return;            // the join gate
            uint8_t out[PLEN];
            int n = fio_msg_encode(0, free_, GID, we.digest, jjson(&we, free_, dname[d]), 0 /* no send clock in this harness */, out, PLEN);
            if (n <= 0) { printf("trial %d: join seal err=%d\n", trial, fio_last_msg_error()); fails++; return; }
            put_row(d, free_, dname[d], out, n);
            if (!evaporate) deliver(out, n, d);
            show_lobby(d, out, n);
        } else if (offered == LC_START && (rnd() % 100) < 45) {
            // startFromLobby: re-adopt the lobby chain, reseat at joins.count, seal LIVE.
            if (dec(win, wlen, &we) <= 0) return;
            if (fio_reseat_game(we.n_joins) != 0) { fails++; return; }
            uint8_t out[PLEN];
            int n = fio_msg_encode(2, seat, GID, we.digest, jjson(&we, -1, ""), 0 /* no send clock in this harness */, out, PLEN);
            if (n <= 0) { printf("trial %d: start seal err=%d\n", trial, fio_last_msg_error()); fails++; return; }
            Env le; if (dec(out, n, &le) <= 0) { fails++; return; }
            put_row(d, seat, dname[d], out, n);
            if (!evaporate) deliver(out, n, d);
            show_board(d, out, n, seat, &le);
        }
        return;
    }

    // BOARD path (adopt): ghost guard + resolve; ambiguous -> spectator.
    int seat = resolve_board(d, &we, senderIsLocal);
    if (seat < 0) { ui[d].kind = UI_SPECT; ui[d].viewer = -1; memcpy(ui[d].chain, win, wlen); ui[d].clen = wlen; return; }
    put_row(d, seat, join_name_at(&we, seat) ? join_name_at(&we, seat) : dname[d], win, wlen);
    show_board(d, win, wlen, seat, &we);
}

// maybeAdoptIncoming: an arrival folds in iff it strictly beats what's showing.
static void incoming(int d, const uint8_t *p, int len, int sender) {
    if (ui[d].kind == UI_NONE) return;
    if (ui[d].clen == len && !memcmp(ui[d].chain, p, len)) return;
    if (ui[d].clen > 0 && fio_msg_rule_p(ui[d].chain, ui[d].clen, p, len) <= 0) return;
    open_surface(d, p, len, sender, 0);
}

// A board device plays the first legal move on ITS chain and sends.
static void act_maybe(int d) {
    if (ui[d].kind != UI_BOARD) return;
    Env e;
    if (dec(ui[d].chain, ui[d].clen, &e) <= 0) return;
    if (e.phase != 2) return;
    static char lbuf[64 * 1024];
    int n = fio_legal_packed(ui[d].viewer, lbuf, sizeof lbuf);
    if (n < 4) return;
    unsigned cnt = (unsigned char)lbuf[0] | ((unsigned char)lbuf[1] << 8);
    if (cnt == 0) return;
    unsigned char *q = (unsigned char *)lbuf + 4;
    int type = q[0], ncards = q[1];
    uint8_t aw[64]; int an = 0;
    aw[an++] = (uint8_t)type; aw[an++] = (uint8_t)ncards;
    for (int i = 0; i < ncards; i++) aw[an++] = q[2 + i];
    if (type == 1) for (int i = 0; i < ncards; i++) aw[an++] = q[2 + ncards + i];
    if (fio_apply_awire(ui[d].viewer, aw, an) != 0) return;
    uint8_t out[PLEN];
    int len = fio_msg_encode(2, ui[d].viewer, GID, e.digest, jjson(&e, -1, ""), 0 /* no send clock in this harness */, out, PLEN);
    if (len <= 0) return;
    Env ne; if (dec(out, len, &ne) <= 0) return;
    put_row(d, ui[d].viewer, dname[d], out, len);
    deliver(out, len, d);
    show_board(d, out, len, ui[d].viewer, &ne);
}

int main(int argc, char **argv) {
    int trials = argc > 1 ? atoi(argv[1]) : 400;
    int humans = argc > 2 ? atoi(argv[2]) : 9;   // 9 vs the 8-seat wire by default
    int with_moves = argc > 3 ? atoi(argv[3]) : 0;
    N = humans; CAP = 8;

    for (trial = 1; trial <= trials; trial++) {
        rng = 77777u * (uint32_t)trial + 13u;
        nb = 0; memset(row, 0, sizeof row); memset(ui, 0, sizeof ui);
        for (int d = 0; d < N; d++) snprintf(dname[d], sizeof dname[d], "%c", 'A' + d);
        GID = 0x33000000 + trial;

        uint8_t seed[32];
        for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(rnd() | 1);
        if (fio_new_game(seed, 32, CAP) != 0) return 1;
        uint8_t zeros[8] = {0}, w0[PLEN];
        char j0[64]; snprintf(j0, sizeof j0, "[{\"seat\":0,\"name\":\"%s\"}]", dname[0]);
        int l0 = fio_msg_encode(0, 0, GID, zeros, j0, 0 /* no send clock in this harness */, w0, PLEN);
        if (l0 <= 0) return 1;
        put_row(0, 0, dname[0], w0, l0);
        deliver(w0, l0, 0);
        show_lobby(0, w0, l0);

        // ---- random schedule ------------------------------------------------
        for (int ev = 0; ev < 60 && nb < MAXB - 2; ev++) {
            int d = (int)(rnd() % N);
            int r = (int)(rnd() % 100);
            int before = nb;
            if (r < 75 || nb < 2) {
                int back = (int)(rnd() % 3);                  // newest, or up to 2 stale
                int bi = nb - 1 - (back < nb ? back : nb - 1);
                open_surface(d, tr_[bi].p, tr_[bi].len, tr_[bi].sender, (rnd() % 100) < 6);
            } else if (r < 85 && row[d].have) {
                open_surface(d, row[d].payload, row[d].plen, -9, 0);   // reopen from cache
            } else if (with_moves) {
                act_maybe(d);
            }
            // every send fans out to open surfaces (didReceive -> adopt-on-arrival)
            for (int b = before; b < nb; b++)
                for (int o = 0; o < N; o++)
                    if (o != tr_[b].sender && (rnd() % 100) < 80)
                        incoming(o, tr_[b].p, tr_[b].len, tr_[b].sender);
        }

        // ---- quiescence: everyone sees the newest, then the canonical -------
        int can = 0;
        for (int i = 1; i < nb; i++)
            if (fio_msg_rule_p(tr_[i].p, tr_[i].len, tr_[can].p, tr_[can].len) < 0) can = i;
        for (int d = 0; d < N; d++) {
            open_surface(d, tr_[nb - 1].p, tr_[nb - 1].len, tr_[nb - 1].sender, 0);
            open_surface(d, tr_[can].p, tr_[can].len, tr_[can].sender, 0);
        }

        // ---- invariants -----------------------------------------------------
        Env ce; if (dec(tr_[can].p, tr_[can].len, &ce) <= 0) { fails++; continue; }
        for (int d = 0; d < N; d++) {
            const char *mine = NULL; int myseat = -1;
            for (int i = 0; i < ce.n_joins; i++)
                if (!strcmp(ce.jname[i], dname[d])) { mine = ce.jname[i]; myseat = ce.jseat[i]; }
            int on_canonical = ui[d].kind == UI_BOARD &&
                ui[d].clen == tr_[can].len && !memcmp(ui[d].chain, tr_[can].p, tr_[can].len);
            int in_limbo = ui[d].kind == UI_BOARD && !on_canonical &&
                fio_msg_rule_p(ui[d].chain, ui[d].clen, tr_[can].p, tr_[can].len) < 0;
            if (in_limbo) {
                // §17.2 evaporation: this device's own chain never delivered and
                // out-ranks everything that did. Nothing in the thread can win
                // against it; it heals the moment any move lands anywhere (turn
                // beats a turn-0 private fork). Counted, never failed.
                limbo++;
            } else if (ui[d].kind == UI_BOARD) {
                if (!on_canonical) {
                    printf("trial %d: CONVERGENCE: %s is on a LOSING non-canonical board\n", trial, dname[d]);
                    c_viol++; fails++;
                } else if (!mine) {
                    printf("trial %d: EXCLUSION: %s (not in roster) on the canonical board\n", trial, dname[d]);
                    x_viol++; fails++;
                } else if (ui[d].viewer != myseat) {
                    printf("trial %d: %s on seat %d, roster says %d\n", trial, dname[d], ui[d].viewer, myseat);
                    s_viol++; fails++;
                }
            } else if (mine && ce.phase >= 2) {
                printf("trial %d: roster member %s (seat %d) has no board (ui=%d)\n",
                       trial, dname[d], myseat, ui[d].kind);
                c_viol++; fails++;
            }
        }
        // L: at a fresh LIVE canonical chain, the first attacker's owner can act.
        if (ce.phase == 2 && ce.turn == 0) {
            static char sbuf[64 * 1024];
            if (dec(tr_[can].p, tr_[can].len, NULL) <= 0) { fails++; continue; }
            if (fio_state_packed(-2, sbuf, sizeof sbuf) <= 0) { fails++; continue; }
            int fa = (signed char)((unsigned char *)sbuf)[3];
            const char *fan = join_name_at(&ce, fa);
            int owner = -1;
            for (int d = 0; d < N; d++) if (fan && !strcmp(dname[d], fan)) owner = d;
            int owner_limbo = owner >= 0 && ui[owner].kind == UI_BOARD &&
                (ui[owner].clen != tr_[can].len || memcmp(ui[owner].chain, tr_[can].p, tr_[can].len)) &&
                fio_msg_rule_p(ui[owner].chain, ui[owner].clen, tr_[can].p, tr_[can].len) < 0;
            if (owner_limbo) { limbo++; }
            else if (owner < 0) { printf("trial %d: first attacker seat %d unowned\n", trial, fa); l_viol++; fails++; }
            else if (ui[owner].kind != UI_BOARD || ui[owner].viewer != fa) {
                printf("trial %d: LIVENESS: first attacker %s not on their board (ui=%d v=%d)\n",
                       trial, fan, ui[owner].kind, ui[owner].viewer);
                l_viol++; fails++;
            } else {
                static char lbuf[64 * 1024];
                int n = fio_legal_packed(fa, lbuf, sizeof lbuf);
                unsigned cnt = n >= 4 ? ((unsigned char)lbuf[0] | ((unsigned char)lbuf[1] << 8)) : 0;
                if (cnt == 0) { printf("trial %d: LIVENESS: first attacker has no move\n", trial); l_viol++; fails++; }
            }
        }
    }

    printf("msg_flow_sim: %d trials x %d humans (cap 8, moves=%d): %d failures "
           "(safety %d, exclusion %d, convergence %d, liveness %d), evaporation-limbo %d\n",
           trials, N, argc > 3 ? atoi(argv[3]) : 0, fails, s_viol, x_viol, c_viol, l_viol, limbo);
    return fails != 0;
}
