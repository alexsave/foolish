/* Chui Niu - the structs the bridge hands Swift, beside the kernel's own.
 *
 * SWIFT NEVER LEARNS A BYTE LAYOUT. cn_api.h returns `const void *` to one of
 * these (or to a kernel struct: CnView, CnBeats, CnBeatFrame), and Swift
 * reads it through readers shared/tools/structgen generates from THESE
 * headers (ios/layout.args) under the iOS triple. The library is stamped
 * with the same layout's hash (cn_api_layout_hash), so a stale binding is a
 * refusal at startup, never a wrong offset.
 *
 * Not shipped as a header: the xcframework carries cn_api.h alone. */
#ifndef CN_API_LAYOUT_H
#define CN_API_LAYOUT_H

#include "../src/cn.h"
#include "../src/cn_plan.h"
#include "../src/cn_view.h"
#include "../src/cn_msg.h"
#include "../src/cn_beats.h"
#include "../src/cn_stage.h"

/* A plan the host asked for. A range too long for this is refused and the
 * host asks for a shorter one; one bubble is at most CN_EVENTS_PER_MOVE. */
#define CN_API_EVENTS 64

typedef struct {
    uint16_t n;
    uint16_t pad0;
    CnEvent  ev[CN_API_EVENTS];
} CnApiEvents;

typedef struct {
    uint8_t name_len;
    char    name[CN_NAME_MAX_BYTES];      /* UTF-8, name_len bytes, no NUL */
} CnApiName;

/* What a staged bubble holds. */
enum { CN_API_STAGED_NONE = 0, CN_API_STAGED_BID = 1, CN_API_STAGED_CALL = 2 };

/* The resident message as a host draws its frame: the phase, who is where,
 * which seat is mine and why, the lobby's verdicts for me, and my staged
 * move. */
typedef struct {
    uint8_t  readable;        /* 1 once a message is resident                    */
    uint8_t  phase;           /* CN_PHASE_* (the envelope's: WAITING, LIVE, ...)  */
    uint8_t  game_phase;      /* CN_PH_* once started, else 0                     */
    uint8_t  dm;
    uint8_t  n_seats;
    uint8_t  me;              /* my seat, or CN_SEAT_NONE                        */
    uint8_t  by;              /* CN_BY_*: the witness that seated me             */
    uint8_t  offered;         /* CN_LOBBY_* for me while WAITING, else 0         */
    uint8_t  can_exit;        /* I may leave this lobby                          */
    uint8_t  can_join_start;  /* my join would fill the table: join and start    */
    uint8_t  starter;         /* CN_SEAT_NONE while WAITING                      */
    uint8_t  sender;          /* the seat that sent this bubble, or CN_SEAT_NONE */
    uint8_t  staged;          /* CN_API_STAGED_*                                 */
    uint8_t  staged_q, staged_f;
    uint8_t  pad0;
    uint16_t lobby_rev;
    uint16_t moves;           /* committed moves                                 */
    CnApiName seat[CN_MAX_SEATS];
} CnApiTable;

#endif
