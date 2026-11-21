// gen_tiles - turns the placeholder art below (ASCII, '.'=white 0, '-'=light 1,
// '+'=dark 2, '#'=black 3) into Game Boy 2bpp tile data, written as a C header
// (build/tiles.h) the ROM includes. Build-time only. The real art comes from
// gameboy/art/ (another branch) and replaces this.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t out[512][16];
static int n_out;

static int px_of(char c) { return c == '#' ? 3 : c == '+' ? 2 : c == '-' ? 1 : 0; }

static void tile_from(const char rows[8][9]) {
    for (int y = 0; y < 8; y++) {
        uint8_t lo = 0, hi = 0;
        for (int x = 0; x < 8; x++) {
            int p = px_of(rows[y][x]);
            lo |= (uint8_t)((p & 1) << (7 - x));
            hi |= (uint8_t)(((p >> 1) & 1) << (7 - x));
        }
        out[n_out][2 * y] = lo;
        out[n_out][2 * y + 1] = hi;
    }
    n_out++;
}

static void fill(char rows[8][9], char c) {
    for (int y = 0; y < 8; y++) { memset(rows[y], c, 8); rows[y][8] = 0; }
}

// Paint a glyph (rows of '#'/'.') at (x0, y0) in colour `fg`.
static void put(char rows[8][9], const char *const *g, int h, int x0, int y0, char fg) {
    for (int y = 0; y < h; y++)
        for (int x = 0; g[y][x]; x++)
            if (g[y][x] == '#' && y0 + y < 8 && x0 + x < 8) rows[y0 + y][x0 + x] = fg;
}

// ---- 5x7 glyphs ------------------------------------------------------------------------
typedef struct { char ch; const char *r[7]; } Glyph;
static const Glyph font[] = {
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
    {':', {".....", "..#..", "..#..", ".....", "..#..", "..#..", "....."}},
    {'-', {".....", ".....", ".....", ".###.", ".....", ".....", "....."}},
    {'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
    {'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
    {'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {'>', {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."}},
    {'<', {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."}},
    {'^', {"..#..", ".###.", "#.#.#", "..#..", "..#..", "..#..", "..#.."}},
    {'v', {"..#..", "..#..", "..#..", "..#..", "#.#.#", ".###.", "..#.."}},
    {'x', {".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#"}},
};
#define NFONT ((int)(sizeof font / sizeof font[0]))

static const Glyph *glyph(char c) {
    for (int i = 0; i < NFONT; i++) if (font[i].ch == c) return &font[i];
    return 0;
}

static const char *ranks = "6789TJQKA";
static const char *ten[7] = { "#.###", "#.#.#", "#.#.#", "#.#.#", "#.#.#", "#.#.#", "#.###" };

static const char *suits[4][6] = {
    { "...#...", "..###..", ".#####.", "#######", ".#.#.#.", "...#..." },   // spades
    { ".##.##.", "#######", "#######", ".#####.", "..###..", "...#..." },   // hearts
    { "..###..", "..###..", "##.#.##", "#######", "##.#.##", "...#..." },   // clubs
    { "...#...", "..###..", ".#####.", ".#####.", "..###..", "...#..." },   // diamonds
};

int main(void) {
    char t[8][9];
    int base_font, base_rank, base_rank_inv, base_suit, base_suit_inv;

    // 0: felt
    fill(t, '-'); tile_from(t);
    // 1: blank white (unused margin)
    fill(t, '.'); tile_from(t);
    // 2: card back, one tile tall (the bot's hand)
    fill(t, '+');
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) if (((x + y) & 3) == 0) t[y][x] = '-';
    for (int x = 0; x < 8; x++) { t[0][x] = '#'; t[7][x] = '#'; }
    for (int y = 0; y < 8; y++) t[y][0] = '#';
    tile_from(t);
    // 3, 4: card back, two tiles tall (the stock), top and bottom
    for (int half = 0; half < 2; half++) {
        fill(t, '+');
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) if (((x + y) & 3) == 0) t[y][x] = '-';
        for (int y = 0; y < 8; y++) { t[y][0] = '#'; t[y][7] = '#'; }
        if (half == 0) for (int x = 0; x < 8; x++) t[0][x] = '#';
        else for (int x = 0; x < 8; x++) t[7][x] = '#';
        tile_from(t);
    }
    // 5, 6: an empty slot (the transfer target), dashed, top and bottom
    for (int half = 0; half < 2; half++) {
        fill(t, '-');
        for (int y = 0; y < 8; y += 2) { t[y][0] = '#'; t[y][7] = '#'; }
        if (half == 0) for (int x = 0; x < 8; x += 2) t[0][x] = '#';
        else for (int x = 0; x < 8; x += 2) t[7][x] = '#';
        tile_from(t);
    }
    // font on felt: black glyphs
    base_font = n_out;
    for (int i = 0; i < NFONT; i++) { fill(t, '-'); put(t, font[i].r, 7, 1, 0, '#'); tile_from(t); }
    // card tops: rank glyphs, normal then selected (inverted)
    base_rank = n_out;
    for (int inv = 0; inv < 2; inv++) {
        if (inv) base_rank_inv = n_out;
        for (int r = 0; r < 9; r++) {
            fill(t, inv ? '#' : '.');
            for (int x = 0; x < 8; x++) t[0][x] = '#';
            for (int y = 0; y < 8; y++) t[y][0] = '#';
            put(t, ranks[r] == 'T' ? ten : glyph(ranks[r])->r, 7, 2, 1, inv ? '.' : '#');
            tile_from(t);
        }
    }
    // card bottoms: suit glyphs, normal then selected. Hearts and diamonds are
    // dark grey, spades and clubs black; selected cards are black with the
    // suits white and light grey.
    base_suit = n_out;
    for (int inv = 0; inv < 2; inv++) {
        if (inv) base_suit_inv = n_out;
        for (int s = 0; s < 4; s++) {
            const int red = s == 1 || s == 3;
            fill(t, inv ? '#' : '.');
            for (int x = 0; x < 8; x++) t[7][x] = '#';
            for (int y = 0; y < 8; y++) t[y][0] = '#';
            put(t, suits[s], 6, 1, 0, inv ? (red ? '-' : '.') : (red ? '+' : '#'));
            tile_from(t);
        }
    }
    const int bg_count = n_out;
    // sprites: the cursor underline, the target frame top and bottom
    const int spr_base = n_out;
    fill(t, '.'); for (int x = 1; x < 7; x++) { t[1][x] = '#'; t[2][x] = '#'; } tile_from(t);
    fill(t, '.'); for (int x = 0; x < 8; x++) { t[0][x] = '#'; t[1][x] = '#'; }
    for (int y = 0; y < 8; y++) { t[y][0] = '#'; t[y][7] = '#'; } tile_from(t);
    fill(t, '.'); for (int x = 0; x < 8; x++) { t[6][x] = '#'; t[7][x] = '#'; }
    for (int y = 0; y < 8; y++) { t[y][0] = '#'; t[y][7] = '#'; } tile_from(t);

    printf("// Generated by tools/gen_tiles.c. Do not edit.\n");
    printf("#define T_FELT 0\n#define T_WHITE 1\n#define T_BACK 2\n#define T_STOCK_TOP 3\n#define T_STOCK_BOT 4\n");
    printf("#define T_SLOT_TOP 5\n#define T_SLOT_BOT 6\n");
    printf("#define T_FONT %d\n#define T_RANK %d\n#define T_RANK_SEL %d\n#define T_SUIT %d\n#define T_SUIT_SEL %d\n",
           base_font, base_rank, base_rank_inv, base_suit, base_suit_inv);
    printf("#define BG_TILES %d\n", bg_count);
    printf("#define S_UNDERLINE 0\n#define S_FRAME_TOP 1\n#define S_FRAME_BOT 2\n#define SPR_TILES 3\n");
    printf("static const char font_chars[] = \"");
    for (int i = 0; i < NFONT; i++) putchar(font[i].ch);
    printf("\";\n");
    printf("static const unsigned char bg_tile_data[] = {\n");
    for (int i = 0; i < bg_count; i++) {
        printf("   ");
        for (int b = 0; b < 16; b++) printf(" 0x%02X,", out[i][b]);
        printf("\n");
    }
    printf("};\nstatic const unsigned char spr_tile_data[] = {\n");
    for (int i = spr_base; i < n_out; i++) {
        printf("   ");
        for (int b = 0; b < 16; b++) printf(" 0x%02X,", out[i][b]);
        printf("\n");
    }
    printf("};\n");
    return 0;
}
