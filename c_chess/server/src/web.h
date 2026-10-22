/* web.h - the built site, served from memory (web.c). */
#ifndef C_CHESS_WEB_H
#define C_CHESS_WEB_H
#include "cbase/app.h"
extern const cb_route_list cc_web_routes;
/* Reads every file under dir (none when dir is NULL or empty): how many. */
int  cc_web_load(const char *dir);
void cc_web_free(void);
#endif
