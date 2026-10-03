/* What the 243 page reads out of its kernel (uttt_243_web.c), as structs.
 *
 * THE PAGE NEVER LEARNS A BYTE LAYOUT. These structs are read in TypeScript
 * through readers shared/tools/structgen writes from THIS header
 * (uttt/web/lib/gen/arena_layout.ts, a build output), and the module is
 * stamped with the layout's hash (ua_layout_hash) so a page whose generated
 * readers and wasm came from different headers refuses to start rather than
 * read a field from the wrong offset.
 *
 * The board itself is not a struct: the picture (ua_grid) and the node
 * statuses (ua_nodes) are plain byte arrays, one byte a cell or node.
 */
#ifndef UTTT_243_WEB_H
#define UTTT_243_WEB_H

#include <stdint.h>
#include "uttt.h"

/* What a cell or a node holds, for the page (generated as UA_ constants). */
enum {
    UA_OPEN = UTTT_OPEN,
    UA_X    = UTTT_X,
    UA_O    = UTTT_O,
    UA_DRAW = UTTT_DRAW
};

/* THE CONTROL ON THE PAGE: how far each bot looks (plies) and how much work
 * a move may spend (a budget, in uttt_big_bot.h's work units), offered as
 * these four sizes. ua_set_bots takes any values and clamps them; the page
 * offers these. uttt_243_web.c holds them to uttt_big_bot.h's bounds. */
enum {
    UA_PLIES_MIN    = 1,
    UA_PLIES_MAX    = 8,
    UA_BUDGET_SMALL = 4000,         /* the first page's setting            */
    UA_BUDGET_MED   = 30000,
    UA_BUDGET_LARGE = 150000,       /* the default                          */
    UA_BUDGET_HUGE  = 1000000,      /* the most a move may spend            */
    UA_DEFAULT_PLIES  = 6,
    UA_DEFAULT_BUDGET = UA_BUDGET_LARGE
};

/* A square of the board in cells: column, row and side. */
typedef struct {
    int32_t x, y, size;
} UaBox;

/* What one seat's bot saw on its last move, and what that side holds. */
typedef struct {
    int32_t value;          /* the chosen move's value, its own side's view   */
    int32_t searched;       /* the plies that search finished                 */
    int32_t work;           /* the work units it spent                        */
    int32_t material;       /* every node the side has won, by the weights    */
} UaSeat;

/* The game now. */
typedef struct {
    int32_t plies;
    int32_t turn;           /* 1 X, 2 O                                       */
    int32_t over;           /* 0, or 1 X won, 2 O won, 3 drawn                */
    int32_t last;           /* the last move's leaf, -1 before the first      */
    int32_t region;         /* the node the next move must go in, -1 when over */
    UaBox   last_box;       /* the last move's cell (size 0 before the first) */
    UaBox   region_box;     /* the region's square (size 0 when over)         */
    UaSeat  x, o;
} UaStatus;

/* The game's shape and the bots' settings, for the page to state. */
typedef struct {
    int32_t depth;          /* 5 on the page                                  */
    int32_t side;           /* 3^depth cells across                           */
    int32_t leaves;         /* side * side                                    */
    int32_t nodes;          /* the internal nodes                             */
    int32_t plies;          /* the deepest search                             */
    int32_t budget;         /* work units a move may spend past its first ply */
    int32_t cap_root, cap_node;   /* candidates searched at the root, below it */
    int32_t weight[5];      /* a won 3 x 3, 9 x 9, 27 x 27, 81 x 81, the game */
    int32_t threat[5];      /* one open two-in-a-row in each of those grids   */
} UaConfig;

#endif
