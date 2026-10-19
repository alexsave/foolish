// Who sits where. See msg_seat_tag.h.
#include "msg_seat_tag.h"
#include "../text_util/utf8.h"
#include <string.h>

int msg_seat_utf8_chars(const uint8_t *s, int len)
{
    int chars;
    return text_utf8_scan(s, (size_t)len, TEXT_UTF8_NO_CONTROLS, &chars, 0) == 0 ? chars : -1;
}

int msg_seat_name_verdict(const uint8_t *name, int len)
{
    if (!name || len <= 0) return MSG_SEAT_NAME_EMPTY;
    if (len > MSG_SEAT_NAME_MAX_BYTES) return MSG_SEAT_NAME_TOO_LONG;
    int chars = msg_seat_utf8_chars(name, len);
    if (chars < 0) return MSG_SEAT_NAME_BAD;
    return chars > MSG_SEAT_NAME_MAX_CHARS ? MSG_SEAT_NAME_TOO_LONG : MSG_SEAT_NAME_OK;
}

int msg_seat_same_name(const MsgSeat *row, const uint8_t *name, int len)
{
    // len at most the row's buffer: a row whose name_len says more than the
    // buffer holds (a caller that trusted a length off the wire) is no match,
    // never a read past the row
    return row->name_len == len && len > 0 && len <= MSG_SEAT_NAME_MAX_BYTES
        && memcmp(row->name, name, (size_t)len) == 0;
}

int msg_seat_resolve(const MsgSeat *rows, int n, int started, int sender,
                     int record, int tag_seat, int is_dm, int i_sent,
                     const uint8_t *name, int name_len, int *by)
{
    int b = MSG_SEAT_BY_NONE, seat = -1;
    if (record >= 0 && record < n) {
        b = MSG_SEAT_BY_RECORD;
        seat = record;
    } else if (tag_seat >= 0 && tag_seat < n) {
        b = MSG_SEAT_BY_TAG;
        seat = tag_seat;
    } else if (record != MSG_SEAT_REC_GONE) {
        // THE INFERENCES, only for a device with no word on this game: a
        // record whose tag has no row says "not me", and a namesake who took
        // the freed name, or the next joiner's bubble, cannot overrule it
        int s = -1;
        if (i_sent == 1) s = sender;
        else if (i_sent == 0 && is_dm && n == 2 && sender >= 0) s = 1 - sender;
        // a sender that is not a row is no witness: the lobby gate below reads
        // rows[s], and the answer must be a seat of this roster
        if (s >= n) s = -1;
        // THE LOBBY GATE: a named device gets a lobby seat by its name or not
        // by inference at all
        if (s >= 0 && !started && name && name_len > 0 && !msg_seat_same_name(&rows[s], name, name_len))
            s = -1;
        if (s >= 0) {
            b = MSG_SEAT_BY_SENDER;
            seat = s;
        } else if (name && name_len > 0) {
            for (int t = 0; t < n; t++)
                if (msg_seat_same_name(&rows[t], name, name_len)) { b = MSG_SEAT_BY_NAME; seat = t; break; }
        }
    }
    if (by) *by = b;
    return seat;
}
