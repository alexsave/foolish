/* cn_glass_shot: a stage frame on the glass, as the iOS host shows it, with the HUD drawn over it (package S).
 * The kernel's picture is flat (the canvas's points) and the host turns it by the HUD's homography; this tool does
 * the turn (each glass pixel through the inverse homography into the frame), lays it over a checker that shows the
 * turned board, and draws what the host lays over the glass: each counting die's up face (die_q) in the glow, the
 * plate and the outcome line's rects. The table is cn_stage_test's: the study's hands, three 3s called.
 *
 *   ./build/cn_glass_shot PACK W H SEATS KIND OUT.png [LIFT]     KIND 1 the table, 2 the reveal
 *   make -C chuiniu/c glass-shot     2, 4 and 6 seats, 390 by 718 and 390 by 340, the reveal: build/glass_*.png
 *
 * Not a test: the pictures a reader looks at. cn_stage_test asserts the same geometry. */
#define main cn_texgen_main
#include "cn_texgen.c"
#undef main
#include "../src/cn_stage.h"
#include <math.h>

static CnStage ST;

static void inv3(const double m[9], double o[9])
{
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g, det = a * A + b * B + c * C;
    o[0] = A / det; o[1] = -(b * i - c * h) / det; o[2] = (b * f - c * e) / det;
    o[3] = B / det; o[4] = (a * i - c * g) / det; o[5] = -(a * f - c * d) / det;
    o[6] = C / det; o[7] = -(a * h - b * g) / det; o[8] = (a * e - b * d) / det;
}

static uint8_t *IMG;
static int IW, IH;
static void dot(double x, double y, const uint8_t c[3])
{
    const int X = (int)x, Y = (int)y;
    if (X < 0 || Y < 0 || X >= IW || Y >= IH) return;
    memcpy(&IMG[((size_t)Y * IW + X) * 3], c, 3);
}
static void line(double x0, double y0, double x1, double y1, const uint8_t c[3])
{
    const int n = (int)(fmax(fabs(x1 - x0), fabs(y1 - y0)) * 2) + 1;
    for (int k = 0; k <= n; k++) dot(x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n, c);
}
static void rect(const float *r, double S, const uint8_t c[3])
{
    line(r[0] * S, r[1] * S, (r[0] + r[2]) * S, r[1] * S, c); line(r[0] * S, (r[1] + r[3]) * S, (r[0] + r[2]) * S, (r[1] + r[3]) * S, c);
    line(r[0] * S, r[1] * S, r[0] * S, (r[1] + r[3]) * S, c); line((r[0] + r[2]) * S, r[1] * S, (r[0] + r[2]) * S, (r[1] + r[3]) * S, c);
}

int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "cn_glass_shot PACK W H SEATS KIND OUT.png [LIFT]\n"); return 2; }
    const int W = atoi(argv[2]), H = atoi(argv[3]), n = atoi(argv[4]), kind = atoi(argv[5]);
    const float lift = argc > 7 ? (float)atof(argv[7]) : 1;
    size_t pn;
    uint8_t *pack = read_file(argv[1], &pn);
    void *arena = malloc(CN_STAGE_ARENA);
    if (!pack || !arena || cn_stage_init(&ST, pack, pn) || cn_stage_attach(&ST, arena, CN_STAGE_ARENA)) { fprintf(stderr, "cn_glass_shot: the stage\n"); return 1; }
    static const uint8_t COUNTS[6] = { 5, 4, 5, 3, 5, 2 };
    static const uint8_t FACES[6][5] = { { 1, 1, 3, 5, 6 }, { 2, 3, 4, 4 }, { 1, 2, 3, 3, 6 }, { 3, 5, 6 }, { 2, 2, 3, 4, 5 }, { 1, 3 } };
    CnStageIn in;
    memset(&in, 0, sizeof in);
    in.kind = (uint8_t)kind; in.seats = (uint8_t)n; in.me = 0; in.turn = 0;
    for (int s = 0; s < n; s++) {
        in.dice[s] = COUNTS[s]; memcpy(&in.faces[s * 5], FACES[s], COUNTS[s]);
        for (int k = 0; k < COUNTS[s]; k++) if (FACES[s][k] == 3 || FACES[s][k] == 1) in.count_mask |= 1u << (s * 5 + k);
    }
    in.known_mask = kind == CN_STAGE_REVEAL ? (uint8_t)((1 << n) - 1) : 1;
    in.w = (float)W; in.h = (float)H; in.scale = 2; in.seed = 7; in.roll_at_ms = CN_STAGE_NO_ROLL;
    const CnStageHud *h = cn_stage_begin(&ST, &in);
    if (!h) { fprintf(stderr, "cn_glass_shot: no table at %dx%d n %d\n", W, H, n); return 1; }
    int fw, fh;
    const uint8_t *fb = cn_stage_frame(&ST, 0, 0, lift, &fw, &fh);
    if (!fb) { fprintf(stderr, "cn_glass_shot: no frame\n"); return 1; }
    const CnStageShot *sh = cn_stage_shot(&ST);
    double hm[9], hi[9];
    for (int i = 0; i < 9; i++) hm[i] = h->hom[i];
    inv3(hm, hi);
    const double S = 2;
    IW = W * 2; IH = H * 2;
    IMG = malloc((size_t)IW * IH * 3);
    for (int y = 0; y < IH; y++) for (int x = 0; x < IW; x++) {
        const double gx = (x + .5) / S, gy = (y + .5) / S;
        const double X = hi[0] * gx + hi[1] * gy + hi[2], Y = hi[3] * gx + hi[4] * gy + hi[5], Wd = hi[6] * gx + hi[7] * gy + hi[8];
        const double fx = X / Wd, fy = Y / Wd;
        const int px = (int)floor((fx - sh->canvas[0]) * sh->scale), py = (int)floor((fy - sh->canvas[1]) * sh->scale);
        uint8_t *q = &IMG[((size_t)y * IW + x) * 3];
        const int ck = ((int)floor(fx / 20) + (int)floor(fy / 20)) & 1;
        q[0] = q[1] = q[2] = (uint8_t)(ck ? 28 : 40);
        if (px >= 0 && py >= 0 && px < fw && py < fh) {
            const uint8_t *c = &fb[((size_t)py * fw + px) * 4];
            for (int k = 0; k < 3; k++) q[k] = (uint8_t)((c[k] * c[3] + q[k] * (255 - c[3]) + 127) / 255);
        }
    }
    static const uint8_t GLOW[3] = { 0x8f, 0xfb, 0xe0 }, PLATE[3] = { 200, 160, 90 }, OUT[3] = { 192, 74, 51 };
    for (int i = 0; i < CN_STAGE_ALL_DICE; i++) {
        if (!(in.count_mask >> i & 1) || kind != CN_STAGE_REVEAL) continue;
        const float *q = &h->die_q[i * 8];
        for (int k = 0; k < 4; k++) line(q[2 * k] * S, q[2 * k + 1] * S, q[(2 * k + 2) % 8] * S, q[(2 * k + 3) % 8] * S, GLOW);
    }
    if (h->has_plate) rect(h->plate, S, PLATE);
    if (h->outcome[3] > 0) rect(h->outcome, S, OUT);
    if (!write_png(argv[6], IMG, IW, IH)) return 1;
    printf("%dx%d n %d kind %d: short %d, canvas %.0f %.0f %.0f %.0f at %.1fx\n", W, H, n, kind, h->short_board,
           sh->canvas[0], sh->canvas[1], sh->canvas[2], sh->canvas[3], sh->scale);
    return 0;
}
