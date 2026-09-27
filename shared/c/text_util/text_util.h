// THE LOW-LEVEL TEXT UNDER A SAY LAYER: stepping through UTF-8, counting the
// columns a line takes, writing a number, copying a string and filling a
// {placeholder} template, all into a caller's fixed buffer.
//
// NO snprintf: a kernel that composes every sentence through these runs the
// same code in a -nostdlib wasm32 build as on a phone. Freestanding: no
// allocation; the only libc is strlen, memcpy and strncmp, which
// shared/c/wasm provides.
//
// EVERY WRITER RETURNS the count written, not counting the NUL, or -1 when the
// result and its NUL would not fit in `cap` (or `out` is NULL). A refused write
// may leave `out` partly written; the caller throws it away.
#ifndef SHARED_TEXT_UTIL_H
#define SHARED_TEXT_UTIL_H

// Decode one UTF-8 character at `s` (which must not point at the NUL) and put
// its length in bytes in `*len`. A sequence cut short by a byte that is not a
// continuation byte (the NUL included) decodes as U+FFFD with `*len` the bytes
// read before it, so a walk never steps over the terminator.
unsigned text_next_cp(const unsigned char *s, int *len);

// The columns one code point takes: 0 for a combining acute-to-tilde mark
// (U+0300-036F), the zero-width space, joiner and direction marks and the
// variation selectors; 2 for the East Asian wide blocks (Hangul jamo, CJK,
// Hangul syllables, compatibility ideographs, fullwidth forms) and everything
// from U+1F300 up (the emoji); 1 for the rest.
//
// A LINE LIMIT IS COUNTED WITH THIS TABLE, and the limits in a string table
// were checked against it: changing a width moves which sentences fit.
// A consumer that needs a different table keeps its own and still steps with
// text_next_cp.
int text_cp_cols(unsigned c);

// The columns of a NUL-terminated string by text_cp_cols. NULL is 0.
int text_cols(const char *s);

// `v` in decimal, a leading '-' when negative, INT_MIN included.
int text_itoa(int v, char *out, int cap);

// Copy `s` whole, or nothing.
int text_put(char *out, int cap, const char *s);

// Copy template `t`, replacing each `{name}` by its value. `kv` and `fallback`
// are each a NULL-terminated list of name/value pairs (either may be NULL); a
// name is looked up in `kv` first and in `fallback` only when `kv` has no such
// name. A NULL value writes nothing. A `{name}` neither list has, a `{` with no
// `}` before the next `{` or the end, stays in the output as written.
int text_fill(char *out, int cap, const char *t, const char *const *kv, const char *const *fallback);

#endif
