// fm_exp: the one exp the bots' decision paths may call (fulminate's posterior
// softmax today). It returns the same bits on every target and under every
// compiler flag, because it does no floating-point arithmetic at all: the
// double comes in as bits, the work is 64-bit integer fixed point, and the
// result goes out as bits.
//
// Why not libm, and why not plain double arithmetic:
//   - libm's exp is not one function. glibc (the Lambda server), Apple's libm
//     (iOS, this Mac) and the freestanding wasm modules (no libm at all) each
//     round some inputs differently, so the same position could weigh a
//     posterior differently, and at a near-tie pick a different move, on two
//     hosts.
//   - Plain double arithmetic is not one function either here: the native
//     builds compile with -ffast-math (foolish/c/Makefile CFLAGS, and the
//     c_base kernel build), which licenses the compiler to fuse, reassociate
//     and swap a divide for a reciprocal multiply, while the wasm builds are
//     strict IEEE. Integer arithmetic has no such licence to take.
//
// Accuracy: within 1 ulp of the correctly rounded exp over the domain below
// (tests.c checks the bound against libm over a seeded grid, and holds the
// bits to a committed digest, tests/fm_exp_digest.h, that the wasm build is
// held to as well by `make wasm-fm-exp-test`).
//
// Domain: x <= -708 (and -inf) returns +0.0, x >= 709 (and +inf) returns
// +inf, NaN returns its input. The softmax only ever passes x <= 0.
#ifndef CNITRO_FM_EXP_H
#define CNITRO_FM_EXP_H

#include <stdint.h>

double fm_exp(double x);

#endif
