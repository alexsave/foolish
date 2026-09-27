// Who sits where. See msg_seat_tag.h.
#include "msg_seat_tag.h"
#include <string.h>

int msg_seat_utf8_chars(const uint8_t *s, int len)
{
    int chars = 0;
    for (int i = 0; i < len;) {
        uint32_t b = s[i], cp;
        int k;
        if (b < 0x80)      { cp = b; k = 1; }
        else if (b < 0xC2) return -1;
        else if (b < 0xE0) { cp = b & 0x1F; k = 2; }
        else if (b < 0xF0) { cp = b & 0x0F; k = 3; }
        else if (b < 0xF5) { cp = b & 0x07; k = 4; }
        else return -1;
        if (i + k > len) return -1;
        for (int j = 1; j < k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (s[i + j] & 0x3F);
        }
        if ((k == 3 && cp < 0x800) || (k == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return -1;
        if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) return -1;
        i += k;
        chars++;
    }
    return chars;
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
    return row->name_len == len && len > 0 && memcmp(row->name, name, (size_t)len) == 0;
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
