// table_wire.c - the action request and response (table.h), both halves.
//
// Apart from table.c because they are bytes and nothing else: a client that
// links no table (the iOS core library, netclient.c) writes the request and
// reads the response through these, and table.c reaches every bot.
#include "table.h"
#include "../../../shared/c/le_bytes.h"
#include <string.h>

int table_request_decode(const uint8_t *p, int len, TableRequest *out) {
    memset(out, 0, sizeof(*out));
    if (!p || len < 2) return TABLE_E_WIRE;
    const int fmt = p[0], gid_len = p[1];
    int at = 2 + gid_len;
    if (fmt == TABLE_REQ_FORMAT_V2) {
        if (len < at + 4) return TABLE_E_WIRE;
        out->has_intent = true;
        out->intent = le_get_u32(p + at);
        at += 4;
    } else if (fmt != TABLE_REQ_FORMAT_V1) {
        return TABLE_E_WIRE;
    }
    if (len < at + 2) return TABLE_E_WIRE;   // the shortest wire is two bytes
    out->fmt = (int8_t)fmt;
    out->gid_len = (uint8_t)gid_len;
    out->gid_off = 2;
    out->wire_off = at;
    out->wire_len = len - at;
    return 0;
}

int table_action_response(int result, int reject, uint32_t version, uint8_t *out, int cap) {
    if (!out || cap < TABLE_RESP_BYTES) return TABLE_E_CAP;
    int status = TABLE_STATUS_APPLIED, code = 0;
    if (result == TABLE_REJECTED) { status = TABLE_STATUS_REJECTED; code = reject; }
    else if (result == TABLE_STALE_ROUND) { status = TABLE_STATUS_REJECTED; code = TABLE_REJECT_STALE_ROUND; }
    else if (result == TABLE_MOOT) status = TABLE_STATUS_MOOT;
    out[0] = TABLE_RESP_FORMAT;
    out[1] = (uint8_t)status;
    out[2] = (uint8_t)code;
    le_put_u32(out + 3, version);
    return TABLE_RESP_BYTES;
}

int table_request_encode(uint8_t *buf, int cap, int gid_len, int wire_len, bool has_intent, uint32_t intent) {
    if (!buf || gid_len < 0 || gid_len > 255 || wire_len < 0) return TABLE_E_WIRE;
    const int head = 2, at = head + gid_len + (has_intent ? 4 : 0);
    if (wire_len > cap - at) return TABLE_E_CAP;
    // Back to front, so nothing is written over bytes not yet moved.
    memmove(buf + at, buf + gid_len, (size_t)wire_len);
    memmove(buf + head, buf, (size_t)gid_len);
    if (has_intent) le_put_u32(buf + head + gid_len, intent);
    buf[0] = has_intent ? TABLE_REQ_FORMAT_V2 : TABLE_REQ_FORMAT_V1;
    buf[1] = (uint8_t)gid_len;
    return at + wire_len;
}

int table_response_decode(const uint8_t *p, int len, TableResponse *out) {
    memset(out, 0, sizeof(*out));
    if (!p || len < TABLE_RESP_BYTES || p[0] != TABLE_RESP_FORMAT) return TABLE_E_WIRE;
    out->status = p[1];
    out->reject_code = p[2];
    out->version = le_get_u32(p + 3);
    return 0;
}

