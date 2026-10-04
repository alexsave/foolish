/* The throw for a browser: cn_roll.c behind scalar exports and static
 * buffers, so a page never learns a byte layout. The page calls
 * cn_roll_run with the throw's numbers, then reads cn_roll_frames_ptr()
 * as a Float32Array of frames x CN_ROLL_FRAME_FLOATS (cn_roll_frame_floats()),
 * cn_roll_phase_ptr() as a Uint8Array of frames, and the rest as ints. */
#include "cn_roll.h"

#define EXPORT(name) __attribute__((export_name(#name)))

static float      frames[CN_ROLL_MAX_FRAMES * CN_ROLL_FRAME_FLOATS];
static uint8_t    phase[CN_ROLL_MAX_FRAMES];
static CnRollInfo info;

EXPORT(cn_roll_run) int cn_roll_run(int kind, int dice, float cup_x, float cup_y, float cup_r, float die, float ring,
                                     float band_x0, float band_x1, float band_y0, float band_y1, float shake_s, uint32_t seed_lo, uint32_t seed_hi)
{
    CnThrow t;
    cn_throw_default(&t, kind, cup_x, cup_y, cup_r, die, ring);
    t.dice = (uint8_t)dice; t.shake_s = shake_s;
    t.band_x0 = band_x0; t.band_x1 = band_x1; t.band_y0 = band_y0; t.band_y1 = band_y1;
    return cn_roll_bake(&t, ((uint64_t)seed_hi << 32) | seed_lo, frames, phase, CN_ROLL_MAX_FRAMES, &info);
}
EXPORT(cn_roll_frames_ptr)   const float   *cn_roll_frames_ptr(void)   { return frames; }
EXPORT(cn_roll_phase_ptr)    const uint8_t *cn_roll_phase_ptr(void)    { return phase; }
EXPORT(cn_roll_frame_floats) int cn_roll_frame_floats(void) { return CN_ROLL_FRAME_FLOATS; }
EXPORT(cn_roll_hz)           int cn_roll_hz(void)           { return CN_ROLL_HZ; }
EXPORT(cn_roll_max_frames)   int cn_roll_max_frames(void)   { return CN_ROLL_MAX_FRAMES; }
EXPORT(cn_roll_slam)         int cn_roll_slam(void)         { return info.slam; }
EXPORT(cn_roll_complete)     int cn_roll_complete(void)     { return info.complete; }
EXPORT(cn_roll_forced)       int cn_roll_forced(void)       { return info.forced; }
EXPORT(cn_roll_up)           int cn_roll_up(int die)        { return die >= 0 && die < CN_ROLL_DICE ? info.up[die] : -1; }
