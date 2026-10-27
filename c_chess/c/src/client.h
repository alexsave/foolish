/* client.h - the client side of c_chess's protocol, in C: one connection's state, the frames it
 * sends (shared/c/stream_frame.h's HELLO, SUB and REQ around wire.h's bodies) and what it makes of
 * the frames it receives. The browser runs it as wasm and the load generator runs it natively, so
 * the bots play over exactly the protocol a person does, and the page holds no game logic: it hands
 * this the socket's bytes, takes the bytes to send, and draws cc_view.
 *
 * It mirrors the game it is in or watching (a cc_game of its own), so legal moves, the board and
 * the move list are known at once, and a move is shown before the server answers; the server's
 * state push confirms or corrects it.
 *
 * No I/O, no allocation, no clock: the host passes `now` (its own milliseconds, any origin) to the
 * calls that need time. */
#ifndef C_CHESS_CLIENT_H
#define C_CHESS_CLIENT_H

#include "game.h"
#include "wire.h"

#include <stddef.h>
#include <stdint.h>

#define CC_OUT_CAP 8192        /* bytes of frames waiting to be sent */
#define CC_PENDING 64          /* requests in flight */
#define CC_TOKEN_MAX 2048

/* Events since the host last took them (cc_client_events): what changed. */
enum {
    CC_EV_READY = 1 << 0,      /* the socket was accepted (HELLO answered) */
    CC_EV_GAME = 1 << 1,       /* the game shown changed: a move, a clock, an offer, the end */
    CC_EV_START = 1 << 2,      /* a game of ours began (cc_client.start_game) */
    CC_EV_LOBBY = 1 << 3,
    CC_EV_SEEK = 1 << 4,       /* our seek is waiting (no game yet) */
    CC_EV_CHALLENGE = 1 << 5,  /* a challenge of ours was made (cc_client.challenge_id) */
    CC_EV_ERROR = 1 << 6,      /* a request was refused (cc_client.err) */
    CC_EV_AUTH = 1 << 7,       /* a sign-in or sign-up answered (cc_client.auth_status) */
    CC_EV_OVER = 1 << 8,       /* the game shown has just ended */
    CC_EV_MOVED = 1 << 9,      /* the other side moved in the game shown */
    CC_EV_TV = 1 << 10,        /* the lobby's live games changed (cc_client.tv) */
    CC_EV_READ = 1 << 11,      /* a read asked by cc_client_read answered (cc_client.read_*) */
    CC_EV_EVAL = 1 << 12,      /* the engine's view of a position arrived (cc_client.eval) */
    CC_EV_ARENA = 1 << 13,     /* an arena's frame arrived (cc_client.arena): its room's push, or an answer taken */
    CC_EV_CHAT = 1 << 14,      /* a room's chat changed (cc_client_chat): a line pushed, or its log read */
};

/* What a request was, to read its answer. */
enum { CC_RQ_NONE, CC_RQ_SEEK, CC_RQ_UNSEEK, CC_RQ_MOVE, CC_RQ_ACT, CC_RQ_SNAP, CC_RQ_LOBBY, CC_RQ_CHALLENGE, CC_RQ_ACCEPT, CC_RQ_SUB, CC_RQ_TV, CC_RQ_READ,
       CC_RQ_CHAT_LOG };
#define CC_READ_CAP 16384   /* the largest answer a read keeps (a profile with its points, a game list) */

typedef struct { uint32_t id; uint8_t kind; uint8_t pad[3]; uint32_t game; char room[CW_ROOM]; } cc_pending;   /* room: a chat log's */

/* A room's chat as the client keeps it: its last lines, oldest first. The client keeps the rooms it
 * hears from, at most CC_CHAT_ROOMS (the lobby, the game shown and an arena are watched together at
 * most); another room takes the slot touched longest ago. */
#define CC_CHAT_ROOMS 3
typedef struct {
    char         room[CW_ROOM];   /* "": a free slot */
    uint16_t     n;
    uint8_t      pad[2];
    uint32_t     used;            /* when last touched (cc_client.chat_seq) */
    cw_chat_line l[CW_CHAT_LOG];
} cc_chat_room;

typedef struct {
    /* the socket */
    uint8_t   out[CC_OUT_CAP];
    uint32_t  out_len;
    uint32_t  out_head;        /* where cc_client_next_out reads next */
    uint32_t  next_id;
    cc_pending pend[CC_PENDING];
    uint8_t   ready;
    uint8_t   pad0[3];
    uint32_t  events;
    /* the account */
    char      token[CC_TOKEN_MAX + 1];
    uint16_t  token_len;
    char      user_id[CW_UUID + 1];
    char      name[CW_NAME + 1];
    char      refresh[CW_REFRESH + 1];
    uint32_t  token_expires_in;
    int32_t   auth_status;     /* the last sign-in's HTTP status */
    uint8_t   auth_code;       /* and its cw_err code */
    /* the game shown */
    uint32_t  game;            /* 0: none */
    int8_t    color;           /* ours, or -1 watching */
    uint8_t   synced;          /* the snapshot arrived */
    uint8_t   base_rated[2];
    cc_game   g;
    char      names[2][CW_NAME + 1];
    int16_t   rating[2], diff[2];
    uint8_t   rematch_offer;
    uint8_t   clock_running;
    uint32_t  next_game;
    uint32_t  game_arena;      /* the arena that made the game shown (its snapshot); 0: none */
    uint32_t  left_ms[2];      /* as of left_at */
    int64_t   left_at;
    char      san[CC_MAX_PLY][8];
    uint16_t  confirmed_ply;   /* the server's ply (ours may be one ahead while a move is in flight) */
    /* when the position became ours to move, by the host's clock (the move's thinking time) */
    int64_t   turn_seen_at;
    /* a premove: played the moment the other side's move arrives, if it is legal then */
    ch_move   premove;
    /* our own move in flight, for the load generator's timing */
    int64_t   move_sent_at;
    uint16_t  move_sent_ply;
    /* other answers */
    uint32_t  start_game;      /* CC_EV_START */
    uint8_t   start_color;
    uint32_t  challenge_id;    /* CC_EV_CHALLENGE */
    cw_err    err;             /* CC_EV_ERROR */
    uint16_t  err_status;
    cw_lobby  lobby;           /* CC_EV_LOBBY */
    cw_tv     tv;              /* CC_EV_TV */
    int64_t   tv_at;           /* when it arrived (its clocks are as of then) */
    uint32_t  moves_sent, moves_confirmed, pushes;
    /* the last read's answer (CC_EV_READ): the tag it was asked with, its status, its bytes */
    cw_eval   eval;            /* CC_EV_EVAL */
    /* the arena shown (CC_EV_ARENA): cw_arena_hdr then its rows, received at arena_at */
    uint8_t   arena[CW_ARENA_FRAME_MAX];
    uint32_t  arena_len;
    int64_t   arena_at;
    /* the chat of the rooms heard from (CC_EV_CHAT) */
    cc_chat_room chat[CC_CHAT_ROOMS];
    uint32_t  chat_seq;
    uint32_t  read_tag;
    uint16_t  read_status;
    uint32_t  read_len;
    uint8_t   read_buf[CC_READ_CAP];
} cc_client;

void cc_client_init(cc_client *c);
/* The next frame to send (one WebSocket binary message): its length, 0 when none. *p stays valid
 * until the next call into the client. */
size_t cc_client_next_out(cc_client *c, const uint8_t **p);
/* One frame the socket received. */
void cc_client_in(cc_client *c, const uint8_t *p, size_t n, int64_t now);
uint32_t cc_client_events(cc_client *c);   /* and clears them */

/* ---- the account (HTTP: the host posts the body and hands back the answer) ---- */
/* The body of POST /c/auth into out (sizeof(cw_auth_req)). */
void cc_client_auth_body(const char *name, const char *password, int mode, cw_auth_req *out);
/* The answer of POST /c/auth or /c/refresh: its status and bytes (CC_EV_AUTH). */
void cc_client_auth_answer(cc_client *c, int status, const uint8_t *p, size_t n);
void cc_client_refresh_body(const cc_client *c, cw_refresh_req *out);
/* Queues HELLO with the access token: the first frame of every connection. */
void cc_client_hello(cc_client *c);

/* ---- requests ---- */
void cc_client_seek(cc_client *c, int base_s, int inc_s, int rated);
void cc_client_unseek(cc_client *c);
void cc_client_challenge(cc_client *c, int base_s, int inc_s, int rated);
void cc_client_accept(cc_client *c, uint32_t challenge);
/* A casual game against the computer at a level 1..8, as white (0), black (1) or either (2):
 * CC_EV_START with the game. */
void cc_client_ai(cc_client *c, int base_s, int inc_s, int level, int color);
void cc_client_lobby(cc_client *c);           /* SUB to the lobby and ask for it now */
void cc_client_lobby_leave(cc_client *c);
/* Shows a game: SUB to its room and ask for its snapshot; our color in it is found from the names. */
void cc_client_open(cc_client *c, uint32_t game);
void cc_client_close_game(cc_client *c);
/* Plays a move in the game shown: CC_OK (sent, and shown at once), or the rules' refusal. */
int  cc_client_move(cc_client *c, int from, int to, int promo, int64_t now);
/* A premove while the other side thinks: CC_OK when kept (it replaces any earlier one). */
int  cc_client_premove(cc_client *c, int from, int to, int promo);
void cc_client_premove_cancel(cc_client *c);
/* The squares a premove from `from` may go to (the piece's moves on the current board as if it
 * were ours to move; whether it is legal is decided when it is played). */
uint64_t cc_client_premove_targets(const cc_client *c, int from);
void cc_client_act(cc_client *c, int act);
/* A GET over the socket (a profile, a game list, the leaderboard): the answer arrives as
 * CC_EV_READ with this tag. Everything after sign-in goes over the one authenticated socket. */
void cc_client_read(cc_client *c, const char *path, uint32_t tag);
/* A POST over the socket whose answer is read like a read's (CC_EV_READ with this tag). */
void cc_client_post(cc_client *c, const char *path, const void *body, size_t n, uint32_t tag);
/* Asks the engine about the position after the first `ply` moves of the game shown: CC_EV_EVAL. */
void cc_client_eval(cc_client *c, int ply);
/* Watches a room by name (an arena's "a<id>": its pushes arrive as CC_EV_ARENA), or stops. A socket
 * may watch the lobby, a game and a few rooms more (the hub's limit is 32). */
void cc_client_watch(cc_client *c, const char *room);
void cc_client_unwatch(cc_client *c, const char *room);
/* An arena's frame: a push of its room (answer 0) or the answer of GET /c/arena/<id>, a join or a
 * withdrawal (answer 1). Kept when well formed (CC_EV_ARENA): 1, else 0. A push holds nobody's own
 * place: the last known of the same arena stays, and the reader's own row, when listed, renews it. */
int  cc_client_arena_take(cc_client *c, const uint8_t *p, size_t n, int64_t now, int answer);

/* ---- chat ---- */
/* Reads a room's last lines (GET /c/chat/<room>) into its slot: CC_EV_CHAT. Lines pushed while the
 * read is in flight are kept. */
void cc_client_chat_log(cc_client *c, const char *room);
/* Says n bytes of text in a room (POST /c/chat): the answer arrives as a read's does (CC_EV_READ
 * with this tag: 200, or a cw_chat_err); the line itself comes back as the room's push. */
void cc_client_chat_say(cc_client *c, const char *room, const char *text, size_t n, uint32_t tag);
/* A room's chat as kept: NULL when the client has none. */
const cc_chat_room *cc_client_chat(const cc_client *c, const char *room);

/* ---- what to draw ---- */
/* A side's clock now. */
int64_t cc_client_clock(const cc_client *c, int color, int64_t now);
/* The squares the piece on `from` may move to, as a bitboard (0 when it is not ours to move). */
uint64_t cc_client_targets(const cc_client *c, int from);
/* Whether moving from -> to is a promotion (the page asks which piece). */
int  cc_client_is_promotion(const cc_client *c, int from, int to);
int  cc_client_my_turn(const cc_client *c);

#endif
