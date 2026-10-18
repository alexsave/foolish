// The wasm half of the fm_exp bit-identity check (`make wasm-fm-exp-test`):
// src/fm_exp.c compiled with the shipped bot module's flags, hashing the same
// inputs the native suite hashes (tests.c, test_fm_exp), held to the same
// committed digest. Native and wasm agreeing with one constant is native and
// wasm agreeing with each other.
#include "fm_exp_digest.h"

__attribute__((export_name("fm_exp_digest_value")))
uint64_t fm_exp_digest_value(void) { return fm_exp_digest(FM_EXP_DIGEST_N); }

__attribute__((export_name("fm_exp_digest_expected")))
uint64_t fm_exp_digest_expected(void) { return FM_EXP_DIGEST; }

__attribute__((export_name("fm_exp_bits")))
uint64_t fm_exp_bits(int i) { return fmd_bits(fm_exp(fm_exp_digest_input(i))); }
