/* analysis.h - the engine's view of any position, for the analysis board: asked over the socket,
 * answered by a push to the asker; a position already analysed is answered from memory at once. */
#ifndef C_CHESS_ANALYSIS_H
#define C_CHESS_ANALYSIS_H

#include "cc_server.h"
#include "cbase/app.h"

extern const cb_route_list cc_analysis_routes;
void cc_analysis_counts(uint64_t *asked, uint64_t *cached, uint64_t *refused);

#endif
