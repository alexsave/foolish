/* Pick 'Em Up - the structs the bridge hands Swift, beside the kernel's own.
 *
 * SWIFT NEVER LEARNS A BYTE LAYOUT. pk_api.h returns `const void *` to one of
 * these (or to a kernel struct: PkView, PkSince), and Swift reads it through
 * readers shared/tools/structgen generates from THESE headers (ios/layout.args)
 * under the iOS triple. The library is stamped with the same layout's hash
 * (pk_api_layout_hash), so a stale binding is a refusal at startup, never a
 * wrong offset.
 *
 * Not shipped as a header: the xcframework carries pk_api.h alone. */
#ifndef PK_API_LAYOUT_H
#define PK_API_LAYOUT_H

#include "../src/pk.h"
#include "../src/pk_plan.h"
#include "../src/pk_view.h"
#include "../src/pk_msg.h"

/* A plan the host asked for (pk_api_plan, pk_api_plan_draft,
 * pk_api_plan_lobby). A range too long for this is refused, and the host
 * asks for a shorter one; a single bubble is far below it. */
#define PK_API_EVENTS 4096

typedef struct {
    uint16_t n;
    uint16_t pad0;
    PkEvent  ev[PK_API_EVENTS];
} PkApiEvents;

typedef struct {
    uint8_t name_len;
    char    name[PK_NAME_MAX_BYTES];      /* UTF-8, name_len bytes, no NUL */
} PkApiName;

/* The resident message as a host draws its frame: the phase, who is where,
 * which seat is mine and why, and the lobby's verdicts for me. */
typedef struct {
    uint8_t  readable;        /* 1 once a message is resident                   */
    uint8_t  phase;           /* PK_PHASE_*                                     */
    uint8_t  dm;
    uint8_t  n_seats;
    uint8_t  me;              /* my seat, or PK_SEAT_NONE                       */
    uint8_t  by;              /* PK_BY_*: the witness that seated me            */
    uint8_t  offered;         /* PK_LOBBY_* for me while WAITING, else 0        */
    uint8_t  can_exit;        /* I may leave this lobby                         */
    uint8_t  can_join_start;  /* my join would fill the table: join and start   */
    uint8_t  starter;         /* PK_SEAT_NONE while WAITING                     */
    uint8_t  sender;          /* the seat that sent this bubble, or PK_SEAT_NONE */
    uint8_t  tip_said;        /* the newest bubble said "Last card!"            */
    uint8_t  draft;           /* my bubble is open                              */
    uint8_t  can_send;        /* ...and may be sealed                           */
    uint16_t lobby_rev;
    uint16_t bubbles;
    uint16_t turns;
    PkApiName seat[PK_MAX_SEATS];
} PkApiTable;

#endif
