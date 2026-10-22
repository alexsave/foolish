// unit_board.c - which refused moves are races (board.h be_move_raced): the sim counts every other
// refusal as a finding, so a race counted there is a false alarm and a finding counted as a race
// is a server bug hidden.
#include "t.h"
#include "board.h"

static TableResponse resp(int status, int reject, uint32_t version) {
    TableResponse r = { (uint8_t)status, (uint8_t)reject, version };
    return r;
}

static void races(void) {
    TableResponse r;
    r = resp(TABLE_STATUS_MOOT, 0, 10);
    T_CHECK(be_move_raced(&r, 10));
    r = resp(TABLE_STATUS_REJECTED, TABLE_REJECT_STALE_ROUND, 10);
    T_CHECK(be_move_raced(&r, 10));
    // A pass made the mover the defender while its attack was on the way: refused at v55, made on v54.
    r = resp(TABLE_STATUS_REJECTED, ENGINE_REJECT_IS_DEFENDER, 55);
    T_CHECK(be_move_raced(&r, 54));
}

static void findings(void) {
    TableResponse r;
    // Refused on the very board it was made on: the sim or the server is wrong.
    r = resp(TABLE_STATUS_REJECTED, ENGINE_REJECT_IS_DEFENDER, 54);
    T_CHECK(!be_move_raced(&r, 54));
    r = resp(TABLE_STATUS_REJECTED, ENGINE_REJECT_NOT_IN_HAND, 54);
    T_CHECK(!be_move_raced(&r, 54));
    r = resp(TABLE_STATUS_APPLIED, 0, 55);
    T_CHECK(!be_move_raced(&r, 54));
}

int main(void) {
    T_RUN(races);
    T_RUN(findings);
    return T_DONE();
}
