/* The kernel's sin and cos: shared/c/wasm/libm.c's series, compiled into this
 * kernel under cn_libm_ names (cn_libm.c) so the native build, the browser
 * module and the tests all run the one series and no platform libm is in
 * cn_roll.c's result. */
#ifndef CN_LIBM_H
#define CN_LIBM_H

double cn_libm_sin(double x);
double cn_libm_cos(double x);

#endif
