// gfx.c's char_off finds a space, A..Z and 0..9 by arithmetic; this is the
// check that the art's font order (ART_FONT_CHARS in art_tiles.h) still is
// that order. It compiles or it does not: `make test` builds it.
#include "art_tiles.h"

_Static_assert(ART_FONT_CHARS[0] == ' ', "the font starts with a space");
_Static_assert(ART_FONT_CHARS[1] == 'A' && ART_FONT_CHARS[26] == 'Z', "then A..Z");
_Static_assert(ART_FONT_CHARS[27] == '0' && ART_FONT_CHARS[36] == '9', "then 0..9");
_Static_assert(sizeof ART_FONT_CHARS > 37, "then the punctuation");

int main(void) { return 0; }
