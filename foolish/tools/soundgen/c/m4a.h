// m4a.h - the shipped files are AAC in MP4 (.m4a), encoded by ffmpeg at build
// time, and this is what soundgen knows about them.
//
// THE PRIMING. An AAC encoder starts its output with priming samples (ffmpeg's
// native encoder: 1024) before the first one it was given, and a take's anchor
// counts samples from the take's first sample. The file says so in its edit
// list (moov/trak/edts/elst): the media starts `edit_media_time` samples in and
// lasts `edit_duration` (in the movie's timescale). Every decoder the game meets
// honours it: Chrome's (FFmpeg), Safari's (Core Audio's AudioFile, the API
// WebKit decodes with) and Firefox's on macOS, measured in docs/AUDIO.md
// ("Shipped as AAC"). So a decoded take begins on the WAV's first sample, and
// `soundgen check` holds every written file to that declaration.
#ifndef SOUNDGEN_M4A_H
#define SOUNDGEN_M4A_H

#include <stdint.h>

// What ffmpeg's native AAC encoder declares as its priming, in samples.
#define AAC_PRIMING 1024

typedef struct {
    uint32_t movie_timescale, media_timescale;
    int64_t  media_duration;     // samples in the track, priming and padding included
    int      n_edits;
    int64_t  edit_media_time;    // where the edit starts in the track: the priming
    int64_t  edit_duration;      // its length, in movie_timescale units
    long     bytes;
} M4aInfo;

// Read a file's headers. 0, or -1 for a file that is not one.
int m4a_info(const char *path, M4aInfo *m);

// Start ffmpeg encoding `wav` (mono, `rate` Hz) into `m4a` at `kbps`: its pid,
// or -1. m4a_wait collects it: 0 when it succeeded.
int m4a_encode(const char *wav, const char *m4a, int rate, int kbps);
int m4a_wait(int pid);

#endif
