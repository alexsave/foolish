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
    /* THE INSERT LOOP. A silent try in the compact drawer is asked again
     * until the silent budget runs out, and then the door comes. */
    ms_stage st;
    OK(ms_stage_first(&st) == MS_ACT_INSERT && st.try_no == 1, "insert: the first try goes at once");
    OK(ms_stage_silence(&st, 1, 1) == MS_ACT_INSERT && st.try_no == 2,
       "insert: a first silence in compact retries as try 2");
    OK(ms_stage_silence(&st, 1, 1) == MS_ACT_NONE && st.try_no == 2,
       "insert: an overtaken try's silence changes nothing");
    for (int t = 2; t < MS_INSERT_ATTEMPTS; t++)
        OK(ms_stage_silence(&st, t, 1) == MS_ACT_INSERT, "insert: every silence short of the budget retries");
    OK(st.try_no == MS_INSERT_ATTEMPTS, "insert: the budget's last try is the tenth");
    OK(ms_stage_silence(&st, MS_INSERT_ATTEMPTS, 1) == MS_ACT_DOOR && st.state == MS_STAGE_DOOR,
       "insert: the last try's silence hands over the door");
    OK(ms_stage_silence(&st, MS_INSERT_ATTEMPTS, 1) == MS_ACT_NONE,
       "insert: behind the door nothing is asked again on its own");
    OK(ms_stage_door_tap(&st) == MS_ACT_INSERT && st.try_no == MS_INSERT_ATTEMPTS + 1 && st.silent == 0,
       "door: a tap inserts again as a new try with the silent budget whole");
    OK(ms_stage_door_tap(&st) == MS_ACT_NONE, "door: a tap with no door up is nothing");
    OK(ms_stage_answer(&st, MS_INSERT_ATTEMPTS + 1, 1) == MS_ACT_LANDED && st.state == MS_STAGE_LANDED,
       "insert: a yes lands");
    OK(ms_stage_answer(&st, MS_INSERT_ATTEMPTS + 1, 1) == MS_ACT_NONE, "insert: a second yes is nothing");
    OK(ms_stage_silence(&st, MS_INSERT_ATTEMPTS + 1, 1) == MS_ACT_NONE,
       "insert: a landed stage's watchdog stands down");
    OK(MS_INSERT_SILENCE_MS * MS_INSERT_ATTEMPTS >= 4000 &&
       MS_INSERT_SILENCE_MS * MS_INSERT_ATTEMPTS <= 6000,
       "insert: the whole silent budget is about five seconds");

    /* A yes from any try lands: a late one, one from behind the door. */
    ms_stage_first(&st);
    ms_stage_silence(&st, 1, 1);
    OK(ms_stage_answer(&st, 1, 1) == MS_ACT_LANDED, "insert: a late yes from an overtaken try lands");
    OK(ms_stage_answer(&st, 2, 1) == MS_ACT_NONE, "insert: the newer try's yes after that is nothing");
    ms_stage_first(&st);
    for (int t = 1; t <= MS_INSERT_ATTEMPTS; t++) ms_stage_silence(&st, t, 1);
    OK(st.state == MS_STAGE_DOOR && ms_stage_answer(&st, 3, 1) == MS_ACT_LANDED,
       "door: a late yes takes the door down and lands");
    ms_stage_reset(&st);
    OK(ms_stage_answer(&st, 1, 1) == MS_ACT_NONE && ms_stage_compact(&st) == MS_ACT_NONE,
       "insert: nothing answers or wakes a stage that has not started");

    /* ERRORS HAVE THEIR OWN BUDGET (bug 1). An error is asked again after a
     * beat; the third reverts; and silences never spend it. */
    ms_stage_first(&st);
    OK(ms_stage_answer(&st, 1, 0) == MS_ACT_INSERT_LATER && st.state == MS_STAGE_DELAY,
       "error: a first error retries after a beat");
    OK(ms_stage_silence(&st, 1, 1) == MS_ACT_NONE, "error: the errored try's watchdog is moot");
    OK(ms_stage_due(&st, 1) == MS_ACT_INSERT && st.try_no == 2, "error: the beat's end inserts try 2");
    OK(ms_stage_due(&st, 1) == MS_ACT_NONE, "error: a beat that already ended is nothing");
    OK(ms_stage_answer(&st, 2, 0) == MS_ACT_INSERT_LATER && st.try_no == 2, "error: a second error waits a beat too");
    OK(ms_stage_due(&st, 1) == MS_ACT_NONE && st.state == MS_STAGE_DELAY,
       "error: an older try's beat cannot end a newer try's");
    OK(ms_stage_due(&st, 2) == MS_ACT_INSERT, "error: a second error retries too");
    OK(ms_stage_answer(&st, 3, 0) == MS_ACT_REVERT && st.state == MS_STAGE_DONE,
       "error: the third error reverts the draft");
    OK(ms_stage_answer(&st, 3, 1) == MS_ACT_NONE && ms_stage_due(&st, 3) == MS_ACT_NONE,
       "error: nothing of a reverted stage goes again");
    OK(MS_INSERT_ERROR_MS < MS_INSERT_SILENCE_MS, "error: the beat is shorter than the silence");

    ms_stage_first(&st);
    ms_stage_silence(&st, 1, 1);
    ms_stage_silence(&st, 2, 1);
    OK(ms_stage_answer(&st, 3, 0) == MS_ACT_INSERT_LATER,
       "error: two silences do not spend the error budget");
    ms_stage_due(&st, 3);
    ms_stage_answer(&st, 4, 0);
    ms_stage_due(&st, 4);
    OK(st.try_no == 5 && ms_stage_answer(&st, 5, 0) == MS_ACT_REVERT,
       "error: the third error after two silences still reverts");
    ms_stage_first(&st);
    ms_stage_answer(&st, 1, 0);
    ms_stage_due(&st, 1);
    ms_stage_answer(&st, 2, 0);
    ms_stage_due(&st, 2);
    int retried = 1;
    for (int t = 3; t < MS_INSERT_ATTEMPTS + 2; t++)
        retried &= ms_stage_silence(&st, t, 1) == MS_ACT_INSERT;
    OK(retried && ms_stage_silence(&st, MS_INSERT_ATTEMPTS + 2, 1) == MS_ACT_DOOR,
       "error: two errors do not spend the silent budget");
    ms_stage_first(&st);
    ms_stage_silence(&st, 1, 1);
    OK(ms_stage_answer(&st, 1, 0) == MS_ACT_NONE && st.state == MS_STAGE_WAITING,
       "error: an overtaken try's late error is nothing");

    /* THE EXPANDED DRAWER PARKS (bug 2). A silence while not compact is
     * neither counted nor asked again and arms no timer; the drawer going
     * compact arms the watchdog once more, and the loop goes on from there
     * to the door as it would have. */
    ms_stage_first(&st);
    OK(ms_stage_silence(&st, 1, 0) == MS_ACT_PARK && st.state == MS_STAGE_PARKED && st.silent == 0,
       "expanded: a silence parks the stage and counts nothing");
    OK(ms_stage_silence(&st, 1, 0) == MS_ACT_NONE && ms_stage_silence(&st, 1, 1) == MS_ACT_NONE,
       "expanded: a parked stage has no watchdog to hear");
    OK(ms_stage_compact(&st) == MS_ACT_ARM && st.state == MS_STAGE_WAITING && st.try_no == 1,
       "expanded: the drawer going compact arms the same try's watchdog");
    OK(ms_stage_compact(&st) == MS_ACT_NONE, "expanded: compact again while waiting is nothing");
    OK(ms_stage_silence(&st, 1, 1) == MS_ACT_INSERT && st.try_no == 2,
       "expanded: the silence after the collapse is a refusal and retries");
    OK(ms_stage_silence(&st, 2, 0) == MS_ACT_PARK && ms_stage_compact(&st) == MS_ACT_ARM,
       "expanded: a try can park more than once");
    int door = MS_ACT_NONE;
    for (int t = 2; t <= MS_INSERT_ATTEMPTS; t++) door = ms_stage_silence(&st, t, 1);
    OK(door == MS_ACT_DOOR, "expanded: after the collapse the door still comes at the tenth silence");
    ms_stage_first(&st);
    ms_stage_silence(&st, 1, 0);
    OK(ms_stage_answer(&st, 1, 1) == MS_ACT_LANDED, "expanded: a parked yes lands");
    ms_stage_first(&st);
    ms_stage_silence(&st, 1, 0);
    OK(ms_stage_answer(&st, 1, 0) == MS_ACT_INSERT_LATER, "expanded: a parked try's error retries after a beat");

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
