/* bubble_data.h - a board of three-state cells, drawn as a picture, and back.
 *
 * WHAT IT IS FOR. A Messages app message carries a URL (about 3 KB) and a
 * picture. A game whose state is a grid of cells - empty, one mark, the other -
 * can be its own picture: one cell, one pixel (or a few). tools/layout_probe
 * measured what a real send does to such a picture; the numbers this file is
 * built to survive are in its README:
 *
 *   - the picture is re-encoded as a JPEG (4:2:0, quality 0.50 and then 0.89),
 *   - a picture over 1200 px on a side is cut down to 1200,
 *   - three LUMINANCE levels (255, 128, 0) decode with no error at 1 pixel per
 *     cell, with the worst channel error 37 against a margin of 64. Hue does
 *     not survive (4:2:0 halves the colour), so there is no colour here.
 *
 * THE PICTURE is n cells wide and n + 1 cells tall, p pixels per cell:
 *
 *     row 0         the header (BD_HEADER_CELLS cells, the rest of the row 0)
 *     rows 1 .. n   n * n payload cells, row by row, left to right
 *
 * The header row is OUTSIDE the n x n board, so a board of n x n cells uses its
 * whole grid (243 x 243 = 59,049 cells) and loses none to framing.
 *
 * TWO LAYERS. The SYMBOL layer carries cells as they are (0, 1 or 2): a game
 * board goes in as its cells. The BYTE layer packs bytes into cells, 3 bytes
 * (24 bits) into 16 cells (3^16 > 2^24, 94% of the 1.585 bits a cell can
 * hold), for anything that is not a grid. Both are framed the same way and
 * both are refused, never misread, when the picture does not check out.
 *
 * THE HEADER, 12 bytes carried as four 24-bit groups of 16 cells (64 cells):
 *
 *     [0..1]   'B' 'D'
 *     [2]      BD_VERSION
 *     [3]      kind: BD_KIND_SYMBOLS or BD_KIND_BYTES
 *     [4]      pad: bytes missing from the last group (BD_KIND_BYTES; else 0)
 *     [5..7]   payload cell count, little-endian
 *     [8..11]  CRC-32 over bytes 0..7 and the payload cells, little-endian
 *
 * NO ALLOCATION, no libc beyond the fixed-width integers; the CRC is here so
 * that the Swift package has no source outside its own directory.
 */
#ifndef BUBBLE_DATA_H
#define BUBBLE_DATA_H

#include <stdint.h>

#define BD_VERSION       1
#define BD_LEVELS        3
#define BD_HEADER_CELLS  64      /* 4 groups of 16 cells */
#define BD_MIN_SIDE      BD_HEADER_CELLS
#define BD_MAX_PIXELS    1200    /* the longest side a real send keeps */

#define BD_KIND_SYMBOLS  0
#define BD_KIND_BYTES    1

#define BD_EOK        0
#define BD_EGEOMETRY -1   /* a side under 64, a cell under 1 px, or over BD_MAX_PIXELS */
#define BD_ECAP      -2   /* the payload does not fit the grid, or a buffer is too small */
#define BD_EMAGIC    -3   /* not one of ours                                           */
#define BD_EVERSION  -4   /* a version this build does not know                        */
#define BD_EKIND     -5   /* the wrong layer was asked for                             */
#define BD_ELENGTH   -6   /* the header's count does not fit the grid                  */
#define BD_ECHECK    -7   /* the checksum does not match: a cell was misread           */
#define BD_ESYMBOL   -8   /* a cell value over 2, or a group that is not 24 bits       */

/* Is an n-cell, p-pixel picture one a send keeps whole? */
int bd_geometry_ok(int n, int p);

/* Pixels: the picture is n*p wide and (n+1)*p tall. */
static inline int bd_width(int n, int p)  { return n * p; }
static inline int bd_height(int n, int p) { return (n + 1) * p; }

/* How many cells a grid carries, and how many bytes the byte layer can. */
int bd_symbol_capacity(int n);
int bd_byte_capacity(int n);

/* ------------------------------------------------------------- the cells */

/* Frame `nsym` symbols (each 0..2) into `cells`, n * (n + 1) of them: the
 * header row, then the symbols, then zeros to the end. Returns BD_EOK or a
 * negative BD_E*. `pad` is the byte layer's, 0 for a plain board. */
int bd_frame(const uint8_t *sym, int nsym, int kind, int pad, int n, uint8_t *cells);

/* Check the header and the checksum of n * (n + 1) cells and copy the
 * payload symbols out. Returns the symbol count (>= 0) or a negative BD_E*.
 * `kind` and `pad` may be NULL. */
int bd_unframe(const uint8_t *cells, int n, uint8_t *sym, int cap, int *kind, int *pad);

/* The byte layer: 3 bytes into 16 cells. */
int bd_pack_bytes(const uint8_t *in, int nbytes, uint8_t *sym, int cap, int *pad);
int bd_unpack_bytes(const uint8_t *sym, int nsym, int pad, uint8_t *out, int cap);

/* Both layers in one call. bd_encode_bytes returns BD_EOK; bd_decode_bytes
 * returns the byte count or a negative BD_E*. */
int bd_encode_bytes(const uint8_t *bytes, int nbytes, int n, uint8_t *cells);
int bd_decode_bytes(const uint8_t *cells, int n, uint8_t *out, int cap);

/* ----------------------------------------------------------- the pixels */

/* Paint cells as grey: 0 is 255, 1 is 128, 2 is 0. `rgba` holds
 * bd_width * bd_height * 4 bytes; alpha 255. */
int bd_paint(const uint8_t *cells, int n, int p, uint8_t *rgba);

/* What reading a picture was like: how many cells it read, how many sat close
 * to a decision threshold (a picture that read right with a lot of these is
 * one more recompression from reading wrong), and the smallest distance to a
 * threshold of any cell, 0..64. */
typedef struct {
    int cells;
    int risky;
    int min_margin;
} BdReading;

#define BD_RISKY_MARGIN 16

/* The longest side bd_sample accepts. A picture comes back at most
 * BD_MAX_PIXELS from a send, but a reader may be handed anything; this bound
 * keeps w * h * 4 far inside an int and lets a caller refuse a picture before
 * it allocates the pixels to read it into. */
#define BD_MAX_READ_SIDE 16384

/* Read a w x h picture as n x (n + 1) cells, one sample at each cell's centre
 * wherever the picture's own size puts it, so a picture that came back
 * smaller or larger is read at the size it came back at. */
int bd_sample(const uint8_t *rgba, int w, int h, int n, uint8_t *cells, BdReading *reading);

/* CRC-32 (IEEE 802.3), exposed because the test holds its check value. */
uint32_t bd_crc32(uint32_t crc, const uint8_t *p, int n);

#endif
