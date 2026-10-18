// The native half of `make wasm-fm-exp-test`: prints fm_exp's result bits at
// every digest input, one hex word per line, for tests/fm_exp_wasm_test.mjs to
// compare against the wasm module input by input. Built with the native CFLAGS
// (-ffast-math included), which is the point.
#include <inttypes.h>
#include <stdio.h>
#include "fm_exp_digest.h"

int main(void) {
    for (int i = 0; i < FM_EXP_DIGEST_N; i++)
        printf("%016" PRIx64 "\n", fmd_bits(fm_exp(fm_exp_digest_input(i))));
    return fm_exp_digest(FM_EXP_DIGEST_N) == FM_EXP_DIGEST ? 0 : 1;
}
