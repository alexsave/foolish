// The replay code's EXTRAS blob - the seat nicknames and per-move timing that
// ride behind the dash in `foolish.cards/<base32 moves>-<base32 extras>`.
//
// The moves half is a game; this half is what a reader needs to say who played
// it and how long they took. It is version 2 and has been on the wire since the
// channel was invented, so the layout below is a description of shipped bytes,
// not a design that is free to move:
//
//   byte 0        version (2)
//   byte 1        flags: bit0 = names section, bit1 = times section
//   names         exactly `player_count` NUL-terminated UTF-8 names, seat
//                 ordered and dense. The count is NOT in the blob - a reader
//                 takes it from the decoded moves - so a short roster
//                 desynchronizes the parse and a long one buries the times.
//   times         1 byte scale exponent e (the unit is 2^(e-64) seconds),
//                 5 bytes big-endian unix start time, then one byte per
//                 information-bearing move: the gap since the previous move on
//                 a log curve, gap(v) = unit * (B^v - 1), B = 1.072.
//
// WHY THE CURVE. Move gaps run from a simulation's nanoseconds to a
// correspondence game's weeks, so the unit is stored per blob and auto-fitted
// to the game's largest gap: ~7.7 decades at <= ~7% relative error, one byte
// per move whatever the game's tempo.
//
// A MALFORMED BLOB MUST NEVER BREAK THE REPLAY. Every failure here is a
// negative return, never a partial write past what it reports; the hosts treat
// extras as decoration around the moves and fall back to "P1"/"P2".
#ifndef CNITRO_REPLAY_EXTRAS_H
#define CNITRO_REPLAY_EXTRAS_H

#define REPLAY_EXTRAS_VERSION     2
#define REPLAY_EXTRAS_FLAG_NAMES  1
#define REPLAY_EXTRAS_FLAG_TIMES  2

// A name is capped at 48 UTF-8 bytes, trimmed by whole CODE POINTS - never
// bytes (that would sever a sequence) and never grapheme clusters (that would
// drop 8 bytes where the format drops 4, and the two would disagree). This is
// the smallest of the three name budgets in the tree: FMSG allows 64
// (MSG_MAX_NAME) and the nickname field 16 characters, and 16 characters of
// emoji is 64 bytes, so the trim is reachable in practice.
#define REPLAY_EXTRAS_MAX_NAME    48

// Errors, returned negated. The hosts map these to the messages the format has
// always thrown, so a reader that catches by message keeps working.
#define REPLAY_EXTRAS_EOK       0
#define REPLAY_EXTRAS_EHEADER   1   // fewer than 2 bytes
#define REPLAY_EXTRAS_EVERSION  2   // version byte is not 2
#define REPLAY_EXTRAS_ENAME     3   // a name ran off the end with no NUL
#define REPLAY_EXTRAS_ETIMES    4   // times flagged, header does not fit
#define REPLAY_EXTRAS_EGAPS     5   // fewer gaps than the moves they describe
#define REPLAY_EXTRAS_EINPUT    6   // the packed argument blob is malformed
#define REPLAY_EXTRAS_ECAP      7   // `cap` too small for the answer

// ---------- the packed argument/answer blob ---------------------------------
//
// Both directions cross as ONE little-endian byte blob, so a host marshals a
// roster and a gap list without an array-of-pointers ABI:
//
//   u8  flags                       (bit0 names, bit1 times)
//   if names:  u8 n_names, then n_names x [u16 len][len UTF-8 bytes]
//   if times:  f64 start_time, u16 n_gaps, then n_gaps x f64 seconds
//
// On the way IN the names are raw: untrimmed, and free to contain a NUL, which
// the encoder strips rather than escapes (NUL is the field terminator, and it
// can never appear inside a UTF-8 multi-byte sequence - that is what makes a
// NUL-terminated list safe for arbitrary Unicode at all). On the way OUT they
// are what the blob holds.

// Roster + timing -> the extras blob. Returns bytes written, or -REPLAY_EXTRAS_E*.
int replay_extras_encode(const unsigned char *in, int in_len,
                         unsigned char *out, int cap);

// The extras blob -> roster + timing. `player_count` and `move_count` come from
// the DECODED MOVES: the blob carries neither, and reading it needs both.
// Returns bytes written, or -REPLAY_EXTRAS_E*.
int replay_extras_decode(const unsigned char *blob, int blob_len,
                         int player_count, int move_count,
                         unsigned char *out, int cap);

// Does this roster say anything worth a segment? An all-empty roster decodes to
// the same "P1"/"P2" a reader already shows, so the bytes would buy nothing and
// the link stays exactly what every build before names emitted. Takes the same
// packed blob as `replay_extras_encode`.
int replay_extras_roster_speaks(const unsigned char *in, int in_len);

// ---------- the whole shareable link ----------------------------------------
//
// `https://foolish.cards/<base32 moves>` and, when the roster says anything,
// `-<base32 extras>` after it. This is the string a client puts in front of a
// person, built here rather than concatenated per platform: the prefix is a
// constant, the dash is already this codec's (replay_b32_decode stops at it),
// and gluing them would give the same answer on a watch, a phone and a browser.
// What stays platform-side is the URL *type*, not the string that goes into it.
//
// `moves` is a base32 code (ASCII, NUL-free, so a C string is the right shape
// for one). `names` is a dense SEAT-ORDERED roster, `n_names` entries of
// [u16 len][len UTF-8 bytes] - never C strings, because a nickname is arbitrary
// Unicode and a NUL is not its terminator. It must be as wide as the table: a
// reader takes the seat count from the decoded moves and then reads exactly
// that many names, so a short roster desynchronizes its parse. Unnamed seats
// are zero-length.
//
// A roster the codec cannot encode costs the NAMES, never the link. Returns
// the length written (NUL-terminated), or -REPLAY_EXTRAS_ECAP.
#define REPLAY_LINK_PREFIX "https://foolish.cards/"

// The same link in QR ALPHANUMERIC form. A QR packs an uppercase-alphanumeric
// payload at 5.5 bits per character against byte mode's 8, so a lowercase
// letter anywhere in the string costs a whole QR version. The moves code is
// already RFC 4648 upper-case; the prefix is the only thing that would drop it
// into byte mode, so this one is uppercase and drops the scheme. It is the same
// link - a reader that resolves foolish.cards gets there either way.
//
// Only a QR should use it. Anything a person copies, clicks or pastes wants
// REPLAY_LINK_PREFIX, which browsers and chat clients auto-link.
#define REPLAY_LINK_PREFIX_QR "WWW.FOOLISH.CARDS/"

// `style` picks between them: 0 = the https link, 1 = the QR form.
#define REPLAY_LINK_STYLE_URL 0
#define REPLAY_LINK_STYLE_QR  1

int replay_extras_link_styled(const char *moves,
                              const unsigned char *names, int names_len, int n_names,
                              int style, char *out, int cap);

// REPLAY_LINK_STYLE_URL, for the callers that never wanted a choice.
int replay_extras_link(const char *moves,
                       const unsigned char *names, int names_len, int n_names,
                       char *out, int cap);

#endif
