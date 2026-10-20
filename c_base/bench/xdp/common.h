/* common.h : the spike's wire format, shared by the server (xsrv) and the load client (xload).
 *
 * A stream of length-prefixed messages over TCP, little-endian, every message starts with struct hdr.
 *   client -> server  HELLO  once per connection: which group it is in, its slot, the group size K, the frame size
 *   client -> server  MOVE   one move: the server spins the handler work, then sends one FRAME to every member of
 *                            the mover's group (the mover included), so a move is one recv plus K sends
 *   server -> client  FRAME  `size` bytes: the header below, then a body whose every byte is a function of
 *                            (mover, mseq, offset), so the client can check the payload byte by byte
 * dseq is the server's per-destination-connection counter: the client checks it is contiguous (no gap, no dup).
 */
#ifndef XDP_SPIKE_COMMON_H
#define XDP_SPIKE_COMMON_H
#include <stdint.h>

enum { MSG_HELLO = 1, MSG_MOVE = 2, MSG_FRAME = 3 };

struct hdr { uint32_t len, type; };
struct hello { struct hdr h; uint32_t group, slot, k, size; };
struct move { struct hdr h; uint32_t mseq, pad; uint64_t t_send; };
struct frame { struct hdr h; uint32_t group, mover, mseq, dseq; uint64_t t_send; };

#define FRAME_MIN ((uint32_t)sizeof(struct frame))
#define FRAME_MAX 4096u

static inline uint8_t body_byte(uint32_t mover, uint32_t mseq, uint32_t off) {
  return (uint8_t)(mover * 131u + mseq * 7u + off);
}

#endif
