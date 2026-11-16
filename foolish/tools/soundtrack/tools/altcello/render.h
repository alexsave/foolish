/* altcello render NOTES.txt OUT.wav [--style expert|novice] [--seed N] [--vib on|off] [--room off|RT60] [--dist M] [--trace PREFIX]
 *                                   [--fs HZ] [--damp PRESS] [--acc LO HI] [--vb MIN SPAN] [--frac MIN SPAN]
 * Plays the note list (player.h) on the cello (all four strings on the bridge, modal body, physical radiation), takes the radiated
 * pressure decimated to 48 kHz and, with --room, applies the image-source plus FDN room and cardioid microphone (room.h; default RT60
 * 0.6 s, 1.5 m). Writes a float32 mono wav (peak -1 dBFS) and prints the real-time factor. */
#ifndef ALTCELLO_RENDER_H
#define ALTCELLO_RENDER_H
#include "player.h"

typedef struct {
    PlayStyle style;
    uint64_t seed;
    int vib_off;
    double rt60;     /* <= 0: no room */
    double dist;
    double fs;       /* simulation rate, 96000 */
    double damp;     /* < 0: the player default */
    double rock; /* < 0: default */
    double acc[2]; /* 0 0: the default */
    double beta0; /* 0: default */
    double vb[2], frac[2]; /* player speed and band-position laws (player.h), 0 0: the defaults */
    const char *trace;
} RenderOpts;
void render_opts_default(RenderOpts *o);
/* renders the notes; *pcm48 is the radiated pressure at 48 kHz (malloc'ed, before the room and the peak normalisation), *played gets the
   contours (caller plays_free); returns 0 on success; *rtf = audio seconds per cpu second */
int render_notes(const Note *notes, int n, const RenderOpts *o, float **pcm48, long *n48, Played *played, double *rtf);
int render_cli(int argc, char **argv);
#endif
