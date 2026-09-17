// structgen fixture: every field kind the generator claims to handle.
#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef enum { K_NEG = -1, K_POS = 7 } KEnum;
#define KFLAG_LOW  3
#define KFLAG_HIGH (1 << 30)
#define KFLAG_NEG  (-KFLAG_HIGH - 5)
enum KPos { KP_A, KP_B };
typedef struct { int8_t s : 3; int8_t v : 5; } KCard;
#define KBAD_CARD ((KCard){ .s = 1, .v = 2 })   // not an integer: --const KBAD_ must fail
typedef struct { unsigned lo : 12; int mid : 11; bool on : 1; unsigned top : 8; } KPacked;   // all bitfields, 4 bytes
struct KNamed { int16_t a; double d; };
typedef struct {
    char tag;
    int64_t big;
    uint64_t ubig;
    float f;
    double d;
    KEnum e;
    enum KPos p;
    const char *name;
    KCard cards[3][2];
    struct KNamed n;
    struct KNamed narr[2];
    union { int32_t i; float f; uint8_t b[4]; } u;
    union { int16_t w; char c; };
    struct { uint8_t x, y; } pt[2];
    bool flag;
    unsigned bits : 3;
    unsigned wide : 20;
    bool bflag : 1;
    char text[5];
    uint16_t u16s[3];
    int32_t i32;
    KPacked packed;
} Kinds;
