/* Chui Niu - the throw, baked. See cn_roll.h.
 *
 * TWO HALVES. The first is product-neutral and could lift into shared/c: a
 * rigid cube (position, velocity, a rotation matrix whose columns are the
 * cube's axes in the world, an angular velocity; a cube's inertia is the
 * same about every axis, m d^2 / 6, which is what keeps it short), one
 * contact function (an impulse along the normal with restitution, then
 * Coulomb friction against the slip, taken against the surface's own
 * velocity), the step (gravity; integrate; every corner against the table,
 * the cup's domed floor and the cup's cone wall in the cup's own frame; the
 * dice against each other by bounding spheres; damping), rest and snap.
 * The second half is this game's two throws: the cup's pose as a function
 * of the clock, and the table roll's pour, sit, slide and cover.
 *
 * DETERMINISM. Everything is IEEE double in one fixed order; the Makefile
 * compiles with -ffp-contract=off so no platform fuses a multiply-add; and
 * sin, cos and sqrt are the kernel's own below, so no platform libm is in
 * the result. tests/cn_roll_test.c holds a pinned golden. */
#include "cn_roll.h"
#include <string.h>

/* ---- the kernel's own maths ------------------------------------------------ */

#define PI 3.14159265358979323846

/* sqrt is one instruction everywhere (f64.sqrt on wasm, sqrtsd / fsqrt
 * natively) and correctly rounded by IEEE, so the builtin is deterministic;
 * -fno-math-errno is not needed because the argument is never negative. */
static double rsqrt(double x) { return __builtin_sqrt(x); }

/* sin and cos as shared/c/wasm/libm.c spells them: a series on
 * [-pi/4, pi/4], then quadrant by quadrant. Copied rather than linked so
 * the native build uses the same one and not its platform's. */
static double sin_k(double x)
{
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156))))));
}
static double cos_k(double x)
{
    double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132)))));
}
static double quad(double x, int *q)
{
    double k = __builtin_floor(x / (PI / 2) + .5);
    *q = (int)((long long)k & 3);
    return x - k * (PI / 2);
}
static double rsin(double x)
{
    int q; double r = quad(x, &q);
    switch (q) { case 0: return sin_k(r); case 1: return cos_k(r); case 2: return -sin_k(r); default: return -cos_k(r); }
}
static double rcos(double x)
{
    int q; double r = quad(x, &q);
    switch (q) { case 0: return cos_k(r); case 1: return -sin_k(r); case 2: return -cos_k(r); default: return sin_k(r); }
}

/* ---- vectors and rotations ----------------------------------------------------- */

typedef struct { double x, y, z; } V3;
typedef struct { V3 c[3]; } M3;          /* columns: the body's local x, y, z in the world */

static V3 v3(double x, double y, double z) { V3 v = { x, y, z }; return v; }
static V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 mul(V3 a, double k) { return v3(a.x * k, a.y * k, a.z * k); }
static double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static double len(V3 a) { return rsqrt(dot(a, a)); }
static V3 norm(V3 a) { double l = len(a); return l > 0 ? mul(a, 1 / l) : a; }
static V3 apply(const M3 *R, V3 p) { return add(add(mul(R->c[0], p.x), mul(R->c[1], p.y)), mul(R->c[2], p.z)); }
static V3 applyT(const M3 *R, V3 p) { return v3(dot(R->c[0], p), dot(R->c[1], p), dot(R->c[2], p)); }
static M3 mulM(const M3 *A, const M3 *B) { M3 m; for (int i = 0; i < 3; i++) m.c[i] = apply(A, B->c[i]); return m; }
static M3 ident(void) { M3 m = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } }; return m; }
static M3 rotX(double a) { double c = rcos(a), s = rsin(a); M3 m = { { { 1, 0, 0 }, { 0, c, s }, { 0, -s, c } } }; return m; }
static M3 rotY(double a) { double c = rcos(a), s = rsin(a); M3 m = { { { c, 0, -s }, { 0, 1, 0 }, { s, 0, c } } }; return m; }
static M3 rotZ(double a) { double c = rcos(a), s = rsin(a); M3 m = { { { c, s, 0 }, { -s, c, 0 }, { 0, 0, 1 } } }; return m; }
/* re-orthonormalise after an integration step, treating the three axes alike: one Newton step
 * toward R^T R = I (R - R (R^T R - I) / 2), then each column to unit length. Gram-Schmidt from the
 * x axis would hand all the step's error to the z axis, and over a throw that favoured the x and y
 * faces (measured: 285 / 286 against 208 / 212 of 250 expected over 1,500 dice). */
static M3 orth(const M3 *R)
{
    double e[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) e[i][j] = dot(R->c[i], R->c[j]) - (i == j ? 1 : 0);
    M3 m;
    for (int j = 0; j < 3; j++) {
        V3 v = R->c[j];
        for (int i = 0; i < 3; i++) v = sub(v, mul(R->c[i], e[i][j] / 2));
        m.c[j] = norm(v);
    }
    return m;
}
/* a rotation drawn evenly over every orientation, from three numbers in [0, 1) (Shoemake) */
static M3 randRot(double u1, double u2, double u3)
{
    double a = rsqrt(1 - u1), b = rsqrt(u1);
    double x = a * rsin(2 * PI * u2), y = a * rcos(2 * PI * u2), z = b * rsin(2 * PI * u3), w = b * rcos(2 * PI * u3);
    M3 m = { { { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w) },
               { 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w) },
               { 2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) } } };
    return m;
}
/* the matrix as a unit quaternion (x, y, z, w), for the frames */
static void toQuat(const M3 *R, float q[4])
{
    double m00 = R->c[0].x, m01 = R->c[1].x, m02 = R->c[2].x;
    double m10 = R->c[0].y, m11 = R->c[1].y, m12 = R->c[2].y;
    double m20 = R->c[0].z, m21 = R->c[1].z, m22 = R->c[2].z;
    double tr = m00 + m11 + m22, x, y, z, w;
    if (tr > 0) { double s = rsqrt(tr + 1) * 2; w = .25 * s; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s; }
    else if (m00 > m11 && m00 > m22) { double s = rsqrt(1 + m00 - m11 - m22) * 2; w = (m21 - m12) / s; x = .25 * s; y = (m01 + m10) / s; z = (m02 + m20) / s; }
    else if (m11 > m22) { double s = rsqrt(1 + m11 - m00 - m22) * 2; w = (m02 - m20) / s; x = (m01 + m10) / s; y = .25 * s; z = (m12 + m21) / s; }
    else { double s = rsqrt(1 + m22 - m00 - m11) * 2; w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = .25 * s; }
    q[0] = (float)x; q[1] = (float)y; q[2] = (float)z; q[3] = (float)w;
}

/* ---- the seed's numbers: splitmix64 ------------------------------------------------ */

static uint64_t mix(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
/* in [0, 1), the top 53 bits */
static double unit(uint64_t *s) { return (double)(mix(s) >> 11) * (1.0 / 9007199254740992.0); }

/* ---- the rigid cube ------------------------------------------------------------- */

#define G         (-12000.0)   /* points a second squared: near 9.8 m/s^2 at 24 points a 16 mm die */
#define E_TABLE   .38
#define E_CUP     .22
#define E_DIE     .3
#define MU        .6
#define LIN_DAMP  .996
#define ANG_DAMP  .988
#define DT        (1.0 / CN_ROLL_SIM_HZ)

typedef struct {
    V3     p, v, w;
    M3     R;
    double s, m, I;           /* half the side, mass, inertia */
    int    settled;
} Body;

/* the cup as the contacts see it: its pose (R, and o the mouth's centre),
 * its velocity and spin, the inner radius at the mouth and at the floor,
 * its height, the floor's depth and its dome, the wall's slope */
typedef struct {
    M3     R;
    V3     o, v, w;
    double ri0, ric, h, hf, dome, nz;
} Cup;

static Body make(double d, V3 p, V3 v, V3 w, M3 R)
{
    Body b; b.p = p; b.v = v; b.w = w; b.R = R; b.s = d / 2; b.m = 1; b.I = d * d / 6; b.settled = 0;
    return b;
}

static V3 corner(const Body *b, int i) { return v3(i & 1 ? b->s : -b->s, i & 2 ? b->s : -b->s, i & 4 ? b->s : -b->s); }

/* one contact at a corner: r the corner from the body's centre, n the
 * surface's normal (into the body), uc the surface's own velocity at that
 * point, e the restitution. If the corner is closing on the surface, an
 * impulse along n, then Coulomb friction against the slip, capped by mu
 * times the impulse. */
static void contact(Body *b, V3 r, V3 n, V3 uc, double e)
{
    V3 u = sub(add(b->v, cross(b->w, r)), uc);
    double un = dot(u, n);
    if (un >= 0) return;
    V3 rn = cross(r, n);
    double k = 1 / b->m + dot(cross(mul(rn, 1 / b->I), r), n), j = -(1 + e) * un / k;
    b->v = add(b->v, mul(n, j / b->m)); b->w = add(b->w, mul(cross(r, mul(n, j)), 1 / b->I));
    V3 u2 = sub(add(b->v, cross(b->w, r)), uc), t = sub(u2, mul(n, dot(u2, n)));
    double tl = len(t);
    if (tl > 1e-3) {
        V3 td = mul(t, 1 / tl), rt = cross(r, td);
        double kt = 1 / b->m + dot(cross(mul(rt, 1 / b->I), r), td), jt = tl / kt;
        if (jt > MU * j) jt = MU * j;
        b->v = sub(b->v, mul(td, jt / b->m)); b->w = sub(b->w, mul(cross(r, mul(td, jt)), 1 / b->I));
    }
}

/* tick: the step's count, which varies the order the corners are taken in. The
 * contacts are resolved one corner after another, and a corner taken first
 * takes the most of a landing; taken in one fixed order, that favoured the +x
 * and +y faces by a seventh over thousands of throws. Flipping the order's
 * axes with the step and the die (an xor over the corner's bits) takes every
 * order as often. */
static void step(Body *bodies, int n, const Cup *cup, const double *walls, double g, int tick)
{
    for (int bi = 0; bi < n; bi++) {
        Body *b = &bodies[bi];
        int mask = (tick * 5 + bi * 3) & 7;
        /* a settled die keeps its rest, but a nudge from another die (below) can have moved it into the
         * wall; it is pushed back out along the table, and nothing else happens to it */
        if (b->settled) {
            if (cup) {
                double deep = 0; V3 pushv = v3(0, 0, 0);
                for (int ci = 0; ci < 8; ci++) {
                    V3 q = applyT(&cup->R, sub(add(b->p, apply(&b->R, corner(b, ci))), cup->o));
                    double rr = rsqrt(q.x * q.x + q.y * q.y);
                    double zz = q.z < 0 ? 0 : q.z > cup->h ? cup->h : q.z, ri = cup->ri0 + (cup->ric - cup->ri0) * zz / cup->h;
                    if (q.z > -b->s * .6 && q.z < cup->hf + b->s && rr > ri && rr > 1e-6) {
                        V3 nl = norm(v3(-q.x / rr, -q.y / rr, -cup->nz)), nw = apply(&cup->R, nl);
                        double dp = (rr - ri) * rsqrt(nl.x * nl.x + nl.y * nl.y);
                        if (dp > deep) { deep = dp; pushv = nw; }
                    }
                }
                if (deep > 0) { b->p = add(b->p, mul(v3(pushv.x, pushv.y, 0), deep)); }
            }
            continue;
        }
        b->v = add(b->v, v3(0, 0, g * DT));
        b->p = add(b->p, mul(b->v, DT));
        { M3 R; for (int i = 0; i < 3; i++) R.c[i] = add(b->R.c[i], mul(cross(b->w, b->R.c[i]), DT)); b->R = orth(&R); }
        double deepest = 0, cupDeep = 0; V3 cupPush = v3(0, 0, 0);
        for (int cj = 0; cj < 8; cj++) {
            int ci = cj ^ mask;
            V3 r = apply(&b->R, corner(b, ci)), pw = add(b->p, r);
            /* the table: every corner below it */
            if (pw.z < 0) { if (pw.z < deepest) deepest = pw.z; contact(b, r, v3(0, 0, 1), v3(0, 0, 0), E_TABLE); }
            if (cup) {
                /* the corner in the cup's frame: z runs from the mouth (0) to the floor (hf); the wall is the cone between */
                V3 q = applyT(&cup->R, sub(pw, cup->o));
                if (q.z > -b->s * .6 && q.z < cup->hf + b->s) {
                    V3 uc = add(cup->v, cross(cup->w, sub(pw, cup->o)));
                    double rr = rsqrt(q.x * q.x + q.y * q.y), f = 1 - rr * rr / (cup->ric * cup->ric);
                    /* the floor is a shallow dome, as a cup's bottom is, so a die cannot lie flat on it and is tipped as it leaves */
                    double zs = cup->hf - cup->dome * (f > 0 ? f : 0);
                    if (q.z > zs) {
                        double sl = 2 * cup->dome * rr / (cup->ric * cup->ric);
                        V3 nl = norm(v3(rr > 1e-6 ? sl * q.x / rr : 0, rr > 1e-6 ? sl * q.y / rr : 0, -1)), nw = apply(&cup->R, nl);
                        double dp = (q.z - zs) * -nl.z;
                        contact(b, r, nw, uc, E_CUP);
                        if (dp > cupDeep) { cupDeep = dp; cupPush = nw; }
                    }
                    double zz = q.z < 0 ? 0 : q.z > cup->h ? cup->h : q.z, ri = cup->ri0 + (cup->ric - cup->ri0) * zz / cup->h;
                    if (rr > ri && rr > 1e-6) {
                        V3 nl = norm(v3(-q.x / rr, -q.y / rr, -cup->nz)), nw = apply(&cup->R, nl);
                        double dp = (rr - ri) * rsqrt(nl.x * nl.x + nl.y * nl.y);
                        contact(b, r, nw, uc, E_CUP);
                        if (dp > cupDeep) { cupDeep = dp; cupPush = nw; }
                    }
                }
            }
        }
        if (deepest < 0) b->p.z -= deepest;
        if (cupDeep > 0) b->p = add(b->p, mul(cupPush, cupDeep));
        /* soft walls round the band: a bounce */
        if (walls) {
            if (b->p.x < walls[0] + b->s) { b->p.x = walls[0] + b->s; b->v.x = (b->v.x < 0 ? -b->v.x : b->v.x) * .5; }
            if (b->p.x > walls[1] - b->s) { b->p.x = walls[1] - b->s; b->v.x = -(b->v.x < 0 ? -b->v.x : b->v.x) * .5; }
            if (b->p.y < walls[2] + b->s) { b->p.y = walls[2] + b->s; b->v.y = (b->v.y < 0 ? -b->v.y : b->v.y) * .5; }
            if (b->p.y > walls[3] - b->s) { b->p.y = walls[3] - b->s; b->v.y = -(b->v.y < 0 ? -b->v.y : b->v.y) * .5; }
        }
        b->v = mul(b->v, LIN_DAMP); b->w = mul(b->w, ANG_DAMP);
    }
    /* die against die: bounding spheres (a cube's corner reaches s * 1.73; its face s), an impulse along the line between them */
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
        Body *a = &bodies[i], *c = &bodies[j];
        double rad = (a->s + c->s) * 1.25; V3 dv = sub(c->p, a->p); double dist = len(dv);
        if (dist < rad && dist > 1e-6) {
            V3 nn = mul(dv, 1 / dist); double push = (rad - dist) / 2;
            /* both are pushed apart; a settled one only along the table, so it stays flat and at rest */
            a->p = sub(a->p, mul(a->settled ? v3(nn.x, nn.y, 0) : nn, push));
            c->p = add(c->p, mul(c->settled ? v3(nn.x, nn.y, 0) : nn, push));
            V3 ra = mul(nn, a->s), rc = mul(nn, -c->s);
            V3 u = sub(add(c->v, cross(c->w, rc)), add(a->v, cross(a->w, ra)));
            double un = dot(u, nn);
            if (un < 0) {
                double k = 1 / a->m + 1 / c->m + dot(cross(mul(cross(ra, nn), 1 / a->I), ra), nn) + dot(cross(mul(cross(rc, nn), 1 / c->I), rc), nn);
                double jn = -(1 + E_DIE) * un / k;
                if (!a->settled) { a->v = sub(a->v, mul(nn, jn / a->m)); a->w = sub(a->w, mul(cross(ra, mul(nn, jn)), 1 / a->I)); }
                if (!c->settled) { c->v = add(c->v, mul(nn, jn / c->m)); c->w = add(c->w, mul(cross(rc, mul(nn, jn)), 1 / c->I)); }
            }
        }
    }
}

/* the face nearest to flat (0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z) and how far from flat it is: the cosine, 1 = flat */
static int upFace(const Body *b, double *cosv)
{
    double best = -2; int bi = 0;
    for (int i = 0; i < 6; i++) {
        double zc = (i & 1 ? -1 : 1) * (i >> 1 == 0 ? b->R.c[0].z : i >> 1 == 1 ? b->R.c[1].z : b->R.c[2].z);
        if (zc > best) { best = zc; bi = i; }
    }
    if (cosv) *cosv = best;
    return bi;
}

/* sc: the throw's scale; a speed scales with it, a spin does not */
static int atRest(const Body *b, double sc)
{
    double c; upFace(b, &c);
    return len(b->v) < 40 * sc && len(b->w) < 3 && c > .9759 /* within 12.6 degrees */ && b->p.z < b->s * 1.08;
}
/* is a die clear of every other's bounding sphere? (the sphere's push moves
 * a pair apart a few hundredths of a point a step until it is) */
static int clear(const Body *bodies, int n, int i)
{
    for (int j = 0; j < n; j++) if (j != i && len(sub(bodies[j].p, bodies[i].p)) < (bodies[i].s + bodies[j].s) * 1.25 - 1e-6) return 0;
    return 1;
}

/* snap flat: the up face exactly up, the yaw kept, the handedness kept */
static void snap(Body *b)
{
    int uf = upFace(b, 0), ax = uf >> 1; double sg = uf & 1 ? -1 : 1;
    V3 other = b->R.c[(ax + 1) % 3], flat = norm(v3(other.x, other.y, 0));
    M3 R;
    R.c[ax] = v3(0, 0, sg); R.c[(ax + 1) % 3] = flat; R.c[(ax + 2) % 3] = cross(R.c[ax], R.c[(ax + 1) % 3]);
    double det = dot(cross(b->R.c[0], b->R.c[1]), b->R.c[2]), det2 = dot(cross(R.c[0], R.c[1]), R.c[2]);
    if (det * det2 < 0) R.c[(ax + 2) % 3] = mul(R.c[(ax + 2) % 3], -1);
    b->R = R; b->p.z = b->s; b->v = v3(0, 0, 0); b->w = v3(0, 0, 0); b->settled = 1;
}

/* ---- the frames ----------------------------------------------------------------- */

typedef struct {
    float   *frames;
    uint8_t *phase;
    int      cap, n, last;    /* last: the phase of the newest frame */
    CnRollInfo *info;
} Out;

static void pose(float *f, V3 p, const M3 *R)
{
    f[0] = (float)p.x; f[1] = (float)p.y; f[2] = (float)p.z;
    toQuat(R, f + 3);
}

/* write one frame: the cup's pose and the dice's. 0 when the cap is full. */
static int emit(Out *o, V3 cupO, const M3 *cupR, const Body *b, int nd, int ph)
{
    if (o->n >= o->cap) return 0;
    float *f = o->frames + o->n * CN_ROLL_FRAME_FLOATS;
    pose(f, cupO, cupR);
    for (int i = 0; i < CN_ROLL_DICE; i++) {
        if (i < nd) pose(f + (1 + i) * CN_ROLL_POSE_FLOATS, b[i].p, &b[i].R);
        else { M3 I = ident(); pose(f + (1 + i) * CN_ROLL_POSE_FLOATS, v3(0, 0, -1e4), &I); }
    }
    if (o->phase) o->phase[o->n] = (uint8_t)ph;
    o->n++; o->last = ph;
    return 1;
}

/* ---- the cup roll ----------------------------------------------------------------- */

/* The cup's pose is a function of the clock: held mouth up for HOLD, shaken
 * for SHAKE (a jiggle, a twist, a tilt, and up and down, each its own
 * rhythm), then the flip, half a turn about the grip in FLIP seconds
 * gathering speed as k^FLIP_POW and stopping dead, and the cup is driven
 * down at DROP_G g, timed so the mouth meets the planks the moment the turn
 * ends. Every number here was searched (the #ifndefs are for that: compile
 * the test with -DFLIP=.6 and read its margin): the slowest turn and the
 * hardest shake that never put a die corner past the mouth over 300 seeds.
 * A slow turn sets the dice down on the face they rode on, so the shake,
 * not the slam, is what randomises them: the test checks that the six
 * faces come up evenly, and reports how often the pre-flip face recurs. */
#define HOLD     .5     /* the first LIFT of it the cup rises from low over the table to where it is held */
#define LIFT     .35
#define LOW      60     /* how far below the held height the cup starts (times the reach) */
#ifndef SHAKE
#define SHAKE    2.0
#endif
#ifndef FLIP
#define FLIP     .4     /* the half turn; the slowest that keeps the dice in is about .5 (.55 leaves a 3-point margin, .6 spills) */
#endif
#ifndef FLIP_POW
#define FLIP_POW 2      /* the turn gathers speed as k^2: a wrist, not a whip */
#endif
#ifndef DROP_G
#define DROP_G   1.8
#endif
#define GRIP     .15    /* the pivot, a fraction of the height in from the mouth */
#define DOME     .35    /* the floor's dome, a fraction of a die */
/* the shake's reach: sideways, and up and down (into and out of the screen) */
#ifndef SHAKE_XY
#define SHAKE_XY 1.6
#endif
#ifndef SHAKE_Z
#define SHAKE_Z  3.0    /* 21 points up and down: the most that keeps the dice in at this beat */
#endif
#ifndef SHAKE_ZF
#define SHAKE_ZF 4.0    /* the up-and-down's beat, a second */
#endif
#ifndef SHAKE_TILT
#define SHAKE_TILT 1.5
#endif
#ifndef HOLD_TILT
#define HOLD_TILT .38   /* radians the held cup leans toward my face, so I see in from a seat; the turn starts from it */
#endif


typedef struct {
    double held[3], grip0[3], home[3], pz, a, tDrop, dropAt, tFlip, tSlam, h, sc, shake;
} CupPath;


/* k^FLIP_POW for the turn's profile, by repeated multiplication (no pow); FLIP_POW is 1, 2 or 3 */
static double powk(double k) { double r = k; for (int i = 1; i < FLIP_POW; i++) r *= k; return r; }
static void cupPose(const CupPath *c, double T, M3 *R, V3 *o)
{
    /* the shake: a jiggle, a twist, a tilt and a bob, each its own rhythm, in from the hold, and carried
     * on through the turn until the drop, over which it fades, so the cup lands flat */
    double u = T - HOLD; if (u < 0) u = 0;
    double env;
    if (T < c->tFlip) { env = u / .15; if (env > 1) env = 1; }
    else { double td = T - c->tFlip - c->dropAt; env = td <= 0 ? 1 : 1 - td / c->tDrop; if (env < 0) env = 0; }
    double dx = 13 * SHAKE_XY * c->sc * rsin(2 * PI * 4.6 * u) * env, dy = 8 * SHAKE_XY * c->sc * rsin(2 * PI * 3.1 * u + 1) * env, dz = 7 * SHAKE_Z * c->sc * rsin(2 * PI * SHAKE_ZF * u) * env;
    double yaw = .55 * rsin(2 * PI * 2.3 * u + .7) * env, tx = .2 * SHAKE_TILT * rsin(2 * PI * 3.7 * u) * env, ty = .2 * SHAKE_TILT * rsin(2 * PI * 2.9 * u + 2) * env;
    M3 b = rotY(ty), d = rotX(tx), e = rotZ(yaw), de = mulM(&d, &e), S = mulM(&b, &de);
    V3 piv; M3 M; double pv;
    /* ONE PIVOT, the grip, for the hold, the shake and the turn: a pivot that moved at the turn's start
     * would move the cup by a tilt's worth in one step, and that jolt alone throws the dice out */
    if (T < c->tFlip) {
        /* the lift: from low over the table to the held height, easing in and out, in the hold's first LIFT seconds */
        double lk = T / LIFT; if (lk > 1) lk = 1; if (lk < 0) lk = 0;
        double low = LOW * c->sc * (1 - lk * lk * (3 - 2 * lk));   /* smooth: no jolt to the dice at the start or the top */
        piv = v3(c->grip0[0] + dx, c->grip0[1] + dy, c->grip0[2] + dz - low); pv = c->pz;
        M3 a = rotX(PI - HOLD_TILT); M = mulM(&a, &S);   /* mouth up, leaned toward me */
    } else {
        double w = T - c->tFlip, k = w / FLIP; if (k > 1) k = 1;
        double td = w - c->dropAt; if (td < 0) td = 0;
        double z = c->grip0[2] - .5 * c->a * td * td; if (z < c->home[2]) z = c->home[2];
        /* the cup comes toward me as it comes down, fastest at the end, and stops dead at the slam: the dice do not */
        double kx = w / (c->tSlam - c->tFlip); if (kx > 1) kx = 1;
        double ex = kx * rsqrt(kx);   /* kx^1.5: slow at first, fastest at the end */
        piv = v3(c->grip0[0] + (c->home[0] - c->grip0[0]) * ex + dx, c->grip0[1] + (c->home[1] - c->grip0[1]) * ex + dy, z + dz); pv = c->pz;
        /* the wrist: the turn gathers speed the whole way and stops dead at the end of its travel */
        if (k < 1) { M3 a = rotX(PI - HOLD_TILT + (PI + HOLD_TILT) * powk(k)); M = mulM(&a, &S); } else M = ident();
        /* the slam's shiver: the cup sits a hair up and settles */
        if (T > c->tSlam && T < c->tSlam + .14) { double sw = (T - c->tSlam) / .14; piv.z += 2.5 * c->sc * rsin(sw * PI) * (1 - sw); }
    }
    *R = M; *o = sub(piv, apply(&M, v3(0, 0, pv)));
}

static int bakeCup(const CnThrow *t, uint64_t seed, Out *o)
{
    const double R = t->cup_r, rc = t->cup_rc, h = t->cup_h, wt = t->cup_t, hf = h - wt, d = t->die, s = d / 2, dome = DOME * d;
    const int nd = t->dice;
    /* the reach: a small cup (a far seat's) is held lower and shaken less, in proportion; the clock and gravity are not scaled */
    const double sc = t->scale > 0 ? t->scale : t->cup_r / CN_THROW_REF_R;
    /* THE TURN STARTS ON THE BOB'S BEAT. Whether the dice stay in depends on where the up-and-down is in its
     * cycle when the turn begins: a shake of 2.0 s keeps a 12-point margin, 1.6 s or 1.9 s spills (measured
     * over 60 seeds a length). So a shake length rounds to a whole number of bobs (1/SHAKE_ZF), and every
     * length is as safe as the one that was searched. */
    double shakeS = t->shake_s > 0 ? t->shake_s : SHAKE;
    shakeS = __builtin_floor(shakeS * SHAKE_ZF + .5) / SHAKE_ZF;
    if (shakeS < .5) shakeS = .5;
    CupPath c;
    c.h = h; c.pz = h * GRIP; c.sc = sc; c.shake = shakeS;
    c.held[0] = t->cup_x; c.held[1] = t->cup_y - 60 * sc; c.held[2] = 110 * sc;
    c.grip0[0] = c.held[0]; c.grip0[1] = c.held[1]; c.grip0[2] = c.held[2] + h / 2 - c.pz;
    c.home[0] = t->cup_x; c.home[1] = t->cup_y; c.home[2] = c.pz;
    /* the drop is timed to end as the turn does, so the mouth meets the planks the moment it faces them */
    /* GRAVITY SCALES WITH THE THROW: lengths and g by sc, the clock as it is, so a far seat's small cup throws
     * exactly my cup's throw in miniature (dynamic similarity), and every margin measured for mine holds for it */
    const double g = G * sc;
    c.a = DROP_G * -g; c.tDrop = rsqrt(2 * (c.grip0[2] - c.home[2]) / c.a);
    c.dropAt = FLIP > c.tDrop ? FLIP - c.tDrop : 0;
    c.tFlip = HOLD + shakeS; c.tSlam = c.tFlip + c.dropAt + c.tDrop;

    /* five dice in the held cup: three on its floor, two on top of them, every one turned its own way */
    uint64_t rs = seed ^ 0x636e2e726f6c6c01ull;        /* "cn.roll" + the recipe's version */
    M3 R0; V3 o0; cupPose(&c, 0, &R0, &o0);
    Body b[CN_ROLL_DICE];
    for (int k = 0; k < nd; k++) {
        double u1 = unit(&rs), u2 = unit(&rs), u3 = unit(&rs), ua = unit(&rs), w1 = unit(&rs), w2 = unit(&rs), w3 = unit(&rs);
        double an = k * 2 * PI / 3 + ua * .5, rr = (k < 3 ? 17 : 10) * sc;
        /* on the cup's floor in the cup's own frame (the held cup leans): three down, two on top */
        V3 lp = v3(rr * rcos(an), rr * rsin(an), hf - dome - s - 1 - (k < 3 ? 0 : d + 2));
        b[k] = make(d, add(o0, apply(&R0, lp)), v3(0, 0, 0), v3((w1 - .5) * 6, (w2 - .5) * 6, (w3 - .5) * 6), randRot(u1, u2, u3));
    }
    double stx[CN_ROLL_DICE], sty[CN_ROLL_DICE];
    for (int k = 0; k < CN_ROLL_DICE; k++) { double an = -PI / 2 + k * 2 * PI / 5; stx[k] = t->cup_x + t->ring * rcos(an); sty[k] = t->cup_y + t->ring * rsin(an); }

    double T = 0, kickT = 0; int ph = CN_RP_HOLD, steps = 0, forced = 0;
    M3 prevR = R0; V3 prevO = o0;
    o->info->slam = 0;
    /* the dice settle on the floor before the first frame (half a second unseen, the cup still), so a
     * throw that waits its turn shows dice at rest, as gravity has them, not dice in the air */
    { Cup still; still.R = R0; still.o = o0; still.v = v3(0, 0, 0); still.w = v3(0, 0, 0); still.ri0 = R - wt; still.ric = rc - wt; still.h = h; still.hf = hf; still.dome = dome; still.nz = (R - rc) / h;
      for (int i = 0; i < CN_ROLL_SIM_HZ / 2; i++) step(b, nd, &still, 0, g, i); }
    emit(o, o0, &R0, b, nd, ph);
    while (ph != CN_RP_IDLE) {
        T += DT; steps++;
        M3 curR; V3 curO; cupPose(&c, T, &curR, &curO);
        Cup cup;
        cup.R = curR; cup.o = curO; cup.v = mul(sub(curO, prevO), 1 / DT);
        { V3 w = v3(0, 0, 0); for (int i = 0; i < 3; i++) w = add(w, cross(prevR.c[i], mul(sub(curR.c[i], prevR.c[i]), 1 / DT))); cup.w = mul(w, .5); }
        cup.ri0 = R - wt; cup.ric = rc - wt; cup.h = h; cup.hf = hf; cup.dome = dome; cup.nz = (R - rc) / h;
        step(b, nd, &cup, 0, g, steps);
        prevR = curR; prevO = curO;
        if (T < HOLD) ph = CN_RP_HOLD; else if (T < c.tFlip) ph = CN_RP_SHAKE; else if (T < c.tSlam) ph = CN_RP_FLIP;
        else {
            if (ph != CN_RP_SETTLE && !o->info->slam) o->info->slam = (uint16_t)o->n;
            ph = CN_RP_SETTLE;
            double u = T - c.tSlam;
            if (u > .45) for (int k = 0; k < nd; k++) if (!b[k].settled && atRest(&b[k], sc)) snap(&b[k]);
            /* a die that lies on another, or stands on an edge, is shivered loose, as a knuckle on the cup would */
            if (u > .9 && T - kickT > .25) {
                kickT = T;
                for (int k = 0; k < nd; k++) if (!b[k].settled) {
                    /* one draw per statement: C leaves the order of two calls in one expression to the
                     * compiler, and gcc and clang chose differently (seen as a native/wasm split) */
                    double kx = unit(&rs), ky = unit(&rs), wx = unit(&rs), wy = unit(&rs);
                    b[k].v = add(b[k].v, v3((kx - .5) * 480 * sc, (ky - .5) * 480 * sc, 180 * sc));
                    b[k].w = add(b[k].w, v3((wx - .5) * 40, (wy - .5) * 40, 0));
                }
            }
            /* the last resort, never seen under the cup: the station farthest from the settled dice */
            if (u > 3) for (int k = 0; k < nd; k++) if (!b[k].settled) {
                int best = 0; double bestd = -1;
                for (int st = 0; st < CN_ROLL_DICE; st++) {
                    double m = 1e9;
                    for (int j = 0; j < nd; j++) if (b[j].settled) { double dx = b[j].p.x - stx[st], dy = b[j].p.y - sty[st], dd = rsqrt(dx * dx + dy * dy); if (dd < m) m = dd; }
                    if (m > bestd) { bestd = m; best = st; }
                }
                b[k].p = v3(stx[best], sty[best], b[k].s); snap(&b[k]); forced++;
            }
            /* idle once every die is settled and clear of the others (a settled die is still nudged apart
             * from a neighbour, a few hundredths of a point a step), or four seconds on whatever happened */
            int all = 1; for (int k = 0; k < nd; k++) if (!b[k].settled || !clear(b, nd, k)) all = 0;
            if (all || u > 4) ph = CN_RP_IDLE;
        }
        /* a frame every fourth step, and the last one whenever the throw ends: the host's final pose is the rest */
        if (steps % (CN_ROLL_SIM_HZ / CN_ROLL_HZ) == 0 || ph == CN_RP_IDLE) {
            if (ph == CN_RP_IDLE) { curO = v3(t->cup_x, t->cup_y, 0); curR = ident(); }
            if (!emit(o, curO, &curR, b, nd, ph)) { o->info->complete = 0; o->info->bake_s = (float)T; return o->n; }
        }
    }
    o->info->complete = 1; o->info->forced = (uint8_t)forced; o->info->bake_s = (float)T;
    for (int k = 0; k < nd; k++) o->info->up[k] = (uint8_t)upFace(&b[k], 0);
    return o->n;
}

/* ---- the table roll ----------------------------------------------------------------- */

/* The dice pour out of the raised cup's mouth onto the planks, bounce,
 * tumble and come to rest wherever they land, sit a second as they fell,
 * then slide into the ring and the cup comes down over them. */
#define POUR    .7      /* how far the cup is tipped, radians */
#define SIT     1.0
#define SLIDE   .4
#define COVER   .4

static int bakeTable(const CnThrow *t, uint64_t seed, Out *o)
{
    const double d = t->die, h = t->cup_h, cupLift = 150;
    const int nd = t->dice;
    const double cupUp[2] = { t->cup_x + 96, t->cup_y - 50 };
    M3 cupRot = rotY(POUR);
    uint64_t rs = seed ^ 0x636e2e726f6c6c02ull;
    Body b[CN_ROLL_DICE];
    for (int k = 0; k < nd; k++) {
        double u1 = unit(&rs), u2 = unit(&rs), u4 = unit(&rs), u5 = unit(&rs), u6 = unit(&rs), u7 = unit(&rs), u8 = unit(&rs), u9 = unit(&rs);
        double an = u1 * PI * 2, spread = 4 + u2 * 14;
        V3 m = apply(&cupRot, v3(rcos(an) * spread, rsin(an) * spread, -d / 2 - 2 - k * 6));
        b[k] = make(d, v3(cupUp[0] + m.x, cupUp[1] + m.y, cupLift + m.z),
                    v3(-300 + (u4 - .5) * 120, 60 + (u5 - .5) * 120, -140), v3((u6 - .5) * 40, (u7 - .5) * 40, (u8 - .5) * 40), rotZ(u9 * 6.28));
    }
    double walls[4] = { t->band_x0, t->band_x1, t->band_y0, t->band_y1 };
    /* the stations: the ring of five, each a little off its station, in angle order */
    double sta[CN_ROLL_DICE], stx[CN_ROLL_DICE], sty[CN_ROLL_DICE];
    {
        uint64_t ss = 0x737461746e6f6e73ull;  /* fixed: the stations are the table's, not the throw's */
        for (int k = 0; k < CN_ROLL_DICE; k++) {
            double ua = unit(&ss), ur = unit(&ss);
            double a = -PI / 2 + k * PI * 2 / 5 + (ua - .5) * .18, rr = t->ring * (1 + (ur - .5) * .1);
            sta[k] = a; stx[k] = t->cup_x + rr * rcos(a); sty[k] = t->cup_y + rr * rsin(a);
        }
    }
    int slot[CN_ROLL_DICE] = { 0, 0, 0, 0, 0 };
    double from[CN_ROLL_DICE][2];
    double T = 0, phaseT = 0; int ph = CN_RP_FALL, steps = 0;
    V3 cupO = v3(cupUp[0], cupUp[1], cupLift); M3 cupR = cupRot;
    o->info->slam = 0;
    emit(o, cupO, &cupR, b, nd, ph);
    while (ph != CN_RP_IDLE) {
        T += DT; phaseT += DT; steps++;
        if (ph == CN_RP_FALL) {
            step(b, nd, 0, walls, G, steps);
            if (T > .35) for (int k = 0; k < nd; k++) if (!b[k].settled && atRest(&b[k], 1)) snap(&b[k]);
            if (T > 2.6) for (int k = 0; k < nd; k++) if (!b[k].settled) snap(&b[k]);
            int all = 1; for (int k = 0; k < nd; k++) if (!b[k].settled || !clear(b, nd, k)) all = 0;
            if (all || T > 3) { ph = CN_RP_SIT; phaseT = 0; }
        } else if (ph == CN_RP_SIT) {
            if (phaseT >= SIT) {
                ph = CN_RP_SLIDE; phaseT = 0; o->info->slam = (uint16_t)o->n;
                /* each die takes the station nearest its angle round the group, in order, so none cross */
                double cx = 0, cy = 0; for (int k = 0; k < nd; k++) { cx += b[k].p.x; cy += b[k].p.y; } cx /= nd; cy /= nd;
                int order[CN_ROLL_DICE]; double ang[CN_ROLL_DICE];
                for (int k = 0; k < nd; k++) { order[k] = k; V3 dv = sub(b[k].p, v3(cx, cy, 0)); ang[k] = dv.x == 0 && dv.y == 0 ? 0 : (dv.y >= 0 ? 1 : -1) * (1 - dv.x / (rsqrt(dv.x * dv.x + dv.y * dv.y))); }
                for (int i = 1; i < nd; i++) for (int j = i; j > 0 && ang[order[j]] < ang[order[j - 1]]; j--) { int tmp = order[j]; order[j] = order[j - 1]; order[j - 1] = tmp; }
                int sorder[CN_ROLL_DICE]; for (int k = 0; k < CN_ROLL_DICE; k++) sorder[k] = k;
                for (int i = 1; i < CN_ROLL_DICE; i++) for (int j = i; j > 0 && sta[sorder[j]] < sta[sorder[j - 1]]; j--) { int tmp = sorder[j]; sorder[j] = sorder[j - 1]; sorder[j - 1] = tmp; }
                int best = 0; double bd = 1e18;
                for (int k = 0; k < CN_ROLL_DICE; k++) { double dx = stx[sorder[k]] - b[order[0]].p.x, dy = sty[sorder[k]] - b[order[0]].p.y, dd = dx * dx + dy * dy; if (dd < bd) { bd = dd; best = k; } }
                for (int k = 0; k < nd; k++) { slot[order[k]] = sorder[(best + k) % CN_ROLL_DICE]; from[order[k]][0] = b[order[k]].p.x; from[order[k]][1] = b[order[k]].p.y; }
            }
        } else if (ph == CN_RP_SLIDE) {
            double k = phaseT / SLIDE; if (k > 1) k = 1;
            double e = 1 - (1 - k) * (1 - k) * (1 - k);
            for (int i = 0; i < nd; i++) b[i].p = v3(from[i][0] + (stx[slot[i]] - from[i][0]) * e, from[i][1] + (sty[slot[i]] - from[i][1]) * e, b[i].s);
            if (k >= 1) { ph = CN_RP_COVER; phaseT = 0; }
        } else if (ph == CN_RP_COVER) {
            double k = phaseT / COVER; if (k > 1) k = 1;
            double e = 1 - (1 - k) * (1 - k);
            cupO = v3(cupUp[0] + (t->cup_x - cupUp[0]) * e, cupUp[1] + (t->cup_y - cupUp[1]) * e, cupLift * (1 - e)); cupR = rotY(POUR * (1 - e));
            if (k >= 1) { ph = CN_RP_IDLE; cupO = v3(t->cup_x, t->cup_y, 0); cupR = ident(); }
        }
        if (steps % (CN_ROLL_SIM_HZ / CN_ROLL_HZ) == 0 || ph == CN_RP_IDLE)
            if (!emit(o, cupO, &cupR, b, nd, ph)) { o->info->complete = 0; o->info->bake_s = (float)T; return o->n; }
    }
    (void)h;
    o->info->complete = 1; o->info->bake_s = (float)T;
    for (int k = 0; k < nd; k++) o->info->up[k] = (uint8_t)upFace(&b[k], 0);
    return o->n;
}

/* ---- the entry points ------------------------------------------------------------- */

int cn_roll_bake(const CnThrow *t, uint64_t seed, float *frames, uint8_t *phase, int cap, CnRollInfo *info)
{
    CnRollInfo local;
    if (!info) info = &local;
    memset(info, 0, sizeof *info);
    if (!t || !frames || cap < 1) return 0;
    if ((t->kind != CN_THROW_CUP && t->kind != CN_THROW_TABLE) || t->dice < 1 || t->dice > CN_ROLL_DICE) return 0;
    if (!(t->cup_r > 0) || !(t->die > 0) || !(t->cup_h > t->cup_t) || !(t->cup_rc > t->cup_t)) return 0;
    Out o; o.frames = frames; o.phase = phase; o.cap = cap; o.n = 0; o.last = 0; o.info = info;
    int n = t->kind == CN_THROW_CUP ? bakeCup(t, seed, &o) : bakeTable(t, seed, &o);
    info->frames = (uint16_t)n;
    return n;
}

void cn_throw_default(CnThrow *t, int kind, float cup_x, float cup_y, float cup_r, float die, float ring)
{
    memset(t, 0, sizeof *t);
    t->kind = (uint8_t)kind; t->dice = CN_ROLL_DICE;
    t->cup_x = cup_x; t->cup_y = cup_y; t->cup_r = cup_r; t->cup_rc = cup_r * .72f; t->cup_h = cup_r * CN_CUP_TALL; t->cup_t = cup_r * .06f;
    t->die = die; t->ring = ring; t->shake_s = 0; t->scale = cup_r / CN_THROW_REF_R;
    t->band_x0 = cup_x - 160; t->band_x1 = cup_x + 160; t->band_y0 = cup_y - 120; t->band_y1 = cup_y + 60;
}
