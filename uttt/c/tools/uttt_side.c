/* uttt_side - the board's side for every whole drawer height, for the ruler's
 * scorer (shared/tools/motion: `motion score --side FILE`), which scores a
 * board mark off the centre against the board's own scale.
 *
 *   uttt_side KIND WORDS WIDTH H0 H1 > side.txt     ("h side" per line)
 *
 * KIND is UTTT_SHEET_* (0 play, 1 watch, 2 wait); WORDS as UtttSheetIn. */
#include "uttt_anim.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: uttt_side KIND WORDS WIDTH H0 H1\n");
        return 2;
    }
    int kind = atoi(argv[1]), words = atoi(argv[2]);
    float w = (float)atof(argv[3]);
    for (int h = atoi(argv[4]); h <= atoi(argv[5]); h++) {
        UtttSheet o;
        uttt_sheet(&(UtttSheetIn){.w = w, .h = (float)h, .kind = kind, .words = words}, &o);
        printf("%d %.3f\n", h, o.board[2]);
    }
    return 0;
}
