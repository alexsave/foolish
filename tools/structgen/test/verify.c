// structgen verification module (wasm32). Independent of the generator: C code
// fills and checks structs through the COMPILER's own field access and offsetof,
// and the TS test reads/writes the same bytes through the generated accessors.
#include "kinds.h"
#include "anim_plan.h"
#include "legal.h"

static Kinds k = {
    .tag = -5, .big = -1234567890123LL, .ubig = 0xF123456789ABCDEFull, .f = 1.5f, .d = -2.25,
    .e = K_NEG, .p = KP_B, .name = (const char *)0x1234,
    .cards = { { { -1, -1 }, { 3, 13 } }, { { 0, 1 }, { 2, 7 } }, { { -4, 15 }, { 1, -16 } } },
    .n = { -300, 3.5 }, .narr = { { 1, 0.5 }, { -2, 9.75 } },
    .u = { .i = -42 }, .w = 0x7abc, .pt = { { 1, 2 }, { 250, 251 } },
    .flag = 1, .bits = 5, .wide = 0xABCDE, .bflag = 1, .text = "abcd", .u16s = { 1, 65535, 300 }, .i32 = -99,
    .packed = { 4000, -1000, 1, 200 },
};
Kinds *k_ptr(void) { return &k; }

// After TS wrote the "second" values, report each mismatching field as a bit.
int k_check(void) {
    int bad = 0, b = 0;
#define CHK(cond) do { if (!(cond)) bad |= 1 << b; b++; } while (0)
    CHK(k.tag == 17);            CHK(k.big == 77LL);          CHK(k.ubig == 0x8000000000000001ull);
    CHK(k.f == -0.125f);         CHK(k.d == 1e300);           CHK(k.e == K_POS);
    CHK(k.cards[2][1].s == -3 && k.cards[2][1].v == 9 && k.cards[2][0].s == -4 && k.cards[2][0].v == 15);
    CHK(k.narr[1].a == 12345 && k.narr[1].d == -0.5);         CHK(k.u.b[3] == 0x80);
    CHK(k.pt[1].y == 7 && k.pt[1].x == 250);                  CHK(k.flag == 0);
    CHK(k.bits == 2 && k.wide == 0xFFFFF && k.bflag == 0);    CHK(k.text[4] == 'z');
    CHK(k.u16s[2] == 40000);     CHK(k.i32 == -2147483647 - 1);
    CHK(k.packed.lo == 1 && k.packed.mid == -1024 && k.packed.on == 0 && k.packed.top == 255);
#undef CHK
    return bad;
}

// Constants as C sees them, for the --const check.
static const long long consts[] = { K_NEG, K_POS, KFLAG_LOW, KFLAG_HIGH, KFLAG_NEG };
long long const_at(int i) { return consts[i]; }
// A string C wrote, for the char[N] reader.
void k_set_text_utf8(void) { const char t[5] = { (char)0xc3, (char)0xa9, 't', 0, 'x' }; for (int i = 0; i < 5; i++) k.text[i] = t[i]; }
int k_text_byte(int i) { return (unsigned char)k.text[i]; }

// Independent offsetof probe for the real anim_plan.h / legal.h structs.
static const unsigned offs[] = {
    sizeof(AnimPlan), __builtin_offsetof(AnimPlan, steps[3].hand[2]), __builtin_offsetof(AnimPlan, pre.flipped),
    __builtin_offsetof(AnimPlan, veil_ids[7]), __builtin_offsetof(AnimPlan, total_ms),
    sizeof(AnimBeats), __builtin_offsetof(AnimBeats, beats[5].placed_ids), __builtin_offsetof(AnimBeats, first_good_mask),
    __builtin_offsetof(AnimEvent, cards), __builtin_offsetof(AnimEvent, mask_cards),
    sizeof(LegalMoves), __builtin_offsetof(LegalMoves, moves[4095].attack_cards[27]), __builtin_offsetof(LegalMoves, moves[1].cards[3]),
};
unsigned off_at(int i) { return offs[i]; }

static AnimBeats beats;
AnimBeats *beats_fill(void) {
    beats.beats[5].placed_ids = 0x0123456789ABCDEFull;
    beats.beats[127].outs_mask = 0x80000001u;
    beats.first_good_mask = -7;
    return &beats;
}
static AnimPlan plan;
AnimPlan *plan_fill(void) {
    plan.pre.flipped.suit = -1; plan.pre.flipped.value = 12;
    plan.steps[3].hand[2] = -123456;
    return &plan;
}
