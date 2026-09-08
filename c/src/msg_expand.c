/* msg_expand.c - the name-entry drawer decision. See msg_expand.h for the
 * flight log that measured it; this file is only the transition function.
 *
 * It lives apart from msg_wire.c on purpose: nothing here touches the FMSG
 * envelope, a game, or a roster. It is the extension's presentation host asking
 * the kernel a question about its own sheet.
 */
#include "msg_expand.h"

void msg_expand_init(MsgExpand *st) {
    if (!st) return;
    st->pending = 0;
    st->retries = 0;
    st->wanted_at = 0.0;
}

int msg_expand_note(MsgExpand *st, int event, double now) {
    if (!st) return 0;

    switch (event) {

    case MSG_EXPAND_WANTED:
        /* Already asking. A second name screen appearing inside the same
         * opening (the lobby's join row after the setup card, say) does not get
         * its own budget - the drawer is being asked for once. */
        if (st->pending) return 0;
        st->pending = 1;
        st->retries = 0;
        st->wanted_at = now;
        /* Issue immediately, because when the drawer is ALREADY installed - a
         * name screen reached mid-session, which is every path except the cold
         * open - this is the request that works, and waiting for a transition
         * that is not coming would make the feature worse than it is today. */
        return 1;

    case MSG_EXPAND_EXPANDED:
        /* The host is expanded, which is the whole ask. Stop, and in particular
         * stop BEFORE the late compact report Messages emits after the expand
         * (0.960 in the header's trace): with this cleared, that one cannot turn
         * into a re-request, and neither can a human dragging the drawer shut a
         * moment later. */
        st->pending = 0;
        st->retries = 0;
        return 0;

    case MSG_EXPAND_COMPACT:
        /* Nothing outstanding - including the host's opening pair at 0.029,
         * which arrives before any name screen exists. */
        if (!st->pending) return 0;
        if (st->retries >= MSG_EXPAND_MAX_RETRIES
            || now - st->wanted_at > MSG_EXPAND_WINDOW_SEC) {
            /* Spent, or stale. Leave the drawer alone rather than keep asking. */
            st->pending = 0;
            return 0;
        }
        st->retries++;
        return 1;

    default:
        return 0;
    }
}
