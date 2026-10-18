/* shared/c/wasm/libm.c under cn_libm_ names. That file defines sin, cos, exp,
 * log and the rest as the freestanding wasm kernel's own; natively the
 * platform has the same names, so every one is renamed here before the file
 * is included, and only cn_libm_sin and cn_libm_cos are used (cn_libm.h). */
#define sin     cn_libm_sin
#define cos     cn_libm_cos
#define sincos  cn_libm_sincos
#define exp     cn_libm_exp
#define log     cn_libm_log
#define log2    cn_libm_log2
#define sinf    cn_libm_sinf
#define cosf    cn_libm_cosf
#define sincosf cn_libm_sincosf
#define logf    cn_libm_logf
#define powf    cn_libm_powf
#define acosf   cn_libm_acosf
#define lroundf cn_libm_lroundf
#define roundf  cn_libm_roundf
#define fminf   cn_libm_fminf
#define fmaxf   cn_libm_fmaxf
#include "../../../shared/c/wasm/libm.c"
