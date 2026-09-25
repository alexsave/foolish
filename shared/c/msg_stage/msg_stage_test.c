/* msg_stage_test.c - the insert-staging decisions (msg_stage.h).
 *
 *   cc -std=c11 -Wall -Wextra -Werror msg_stage_test.c -o msg_stage_test && ./msg_stage_test
 *
 * No -I: the header is beside this file. Exits 1 on any failure. */
#include <stdio.h>
#include "msg_stage.h"

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", what); } } while (0)

int main(void)
{
    /* A silent insert is a refusal only in the compact drawer, is asked again
     * until the budget runs out, and then hands over the door. */
    OK(ms_insert_silence(1, 1) == MS_INSERT_RETRY, "insert: a first silence in compact retries");
    OK(ms_insert_silence(MS_INSERT_ATTEMPTS - 1, 1) == MS_INSERT_RETRY,
       "insert: the last try but one still retries");
    OK(ms_insert_silence(MS_INSERT_ATTEMPTS, 1) == MS_INSERT_DOOR,
       "insert: the last try's silence hands over the door");
    OK(ms_insert_silence(MS_INSERT_ATTEMPTS + 3, 1) == MS_INSERT_DOOR,
       "insert: past the budget it is still the door");
    int listened = 1;
    for (int a = 1; a <= MS_INSERT_ATTEMPTS + 1; a++)
        listened &= ms_insert_silence(a, 0) == MS_INSERT_LISTEN;
    OK(listened, "insert: expanded silence is never a refusal");
    OK(MS_INSERT_SILENCE_MS * MS_INSERT_ATTEMPTS >= 4000 &&
       MS_INSERT_SILENCE_MS * MS_INSERT_ATTEMPTS <= 6000,
       "insert: the whole budget is about five seconds");

    /* The + drawer's window-sized first appearance never counts, an expanded
     * drawer short of the window always does (647 in 667), and a compact one
     * must be well short. */
    OK(!ms_drawer_up(932, 932, 0), "drawer: the + drawer's first, window-sized appear is not up");
    OK(!ms_drawer_up(932, 932, 1), "drawer: window-sized is not up even when expanded");
    OK(!ms_drawer_up(667, 667, 1), "drawer: a 667 window itself is not up");
    OK(ms_drawer_up(667, 647, 1), "drawer: a tapped, expanded drawer (647 of 667) is up");
    OK(ms_drawer_up(956, 897, 1), "drawer: a large phone's expanded drawer is up");
    OK(ms_drawer_up(932, 343, 0), "drawer: a phone's compact drawer is up");
    OK(ms_drawer_up(667, 309, 0), "drawer: a 667 phone's compact drawer is up");
    OK(!ms_drawer_up(667, 647, 0), "drawer: compact but nearly the window is not up yet");
    OK(!ms_drawer_up(667, 0, 1) && !ms_drawer_up(0, 300, 0), "drawer: no size is not up");

    /* My own bubble is never an arrival; my staged one coming back is the send. */
    OK(ms_receive(0, 0) == MS_RECV_ARRIVAL, "receive: somebody else's bubble is an arrival");
    OK(ms_receive(0, 1) == MS_RECV_ARRIVAL, "receive: not mine is an arrival whatever else is said");
    OK(ms_receive(1, 0) == MS_RECV_ECHO, "receive: my own sent or draft bubble is an echo");
    OK(ms_receive(1, 1) == MS_RECV_ECHO_SENT, "receive: my staged bubble back is the send landing");

    printf("msg_stage: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
