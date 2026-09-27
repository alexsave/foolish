/* Tallybones - the structs the bridge hands Swift, beside the kernel's own.
 *
 * SWIFT NEVER LEARNS A BYTE LAYOUT. tb_api.h returns `const void *` to one of
 * these (or to a kernel struct: TbView, TbBeats, TbBeatFrame, TbBeatSample),
 * and Swift reads it through readers shared/tools/structgen generates from
 * THESE headers (ios/layout.args) under the iOS triple. The library is
 * stamped with the same layout's hash (tb_api_layout_hash), so a stale
 * binding is a refusal at startup, never a wrong offset.
 *
 * Not shipped as a header: the xcframework carries tb_api.h alone. */
#ifndef TB_API_LAYOUT_H
#define TB_API_LAYOUT_H

#include "../src/tb.h"
#include "../src/tb_plan.h"
#include "../src/tb_view.h"
#include "../src/tb_msg.h"
#include "../src/tb_beats.h"

#define TB_API_EVENTS TB_BEATS_EVENTS

typedef struct {
    uint16_t n;
    uint16_t pad0;
    TbEvent  ev[TB_API_EVENTS];
} TbApiEvents;

typedef struct {
    uint8_t name_len;
    char    name[TB_NAME_MAX_BYTES];      /* UTF-8, name_len bytes, no NUL */
} TbApiName;

/* The resident message as a host draws its frame: the phase, who is where,
 * which seat is mine and why, the lobby's verdicts for me, and my staged
 * move. */
typedef struct {
    uint8_t  readable;        /* 1 once a message is resident                    */
    uint8_t  phase;           /* TB_PHASE_*                                      */
    uint8_t  dm;
    uint8_t  n_seats;
    uint8_t  me;              /* my seat, or TB_SEAT_NONE                        */
    uint8_t  by;              /* TB_BY_*: the witness that seated me             */
    uint8_t  offered;         /* TB_LOBBY_* for me while WAITING, else 0         */
    uint8_t  can_exit;        /* I may leave this lobby                          */
    uint8_t  can_join_start;  /* my join would fill the table: join and start    */
    uint8_t  starter;         /* TB_SEAT_NONE while WAITING                      */
    uint8_t  sender;          /* the seat that sent this bubble, or TB_SEAT_NONE */
    uint8_t  my_turn;         /* the game waits on my KEEP or SCORE              */
    uint8_t  staged;          /* a move of mine is staged (tb_api_text sends it) */
    uint8_t  staged_kind;     /* its TB_M_*                                      */
    uint8_t  staged_arg;      /* its mask or category                            */
    uint8_t  pad0;
    uint16_t lobby_rev;
    uint16_t bubbles;         /* resident moves (the staged one not counted)     */
    uint16_t turns;
    uint16_t pad1;
    TbApiName seat[TB_MAX_SEATS];
} TbApiTable;

#endif
