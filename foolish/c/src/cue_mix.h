// cue_mix.h - the takes a host has scheduled, summed into its output, to the
// sample.
//
// cue_voice.h says which take a strike plays and how long from now its file
// starts. This is the last step: a host hands over the take's samples (already
// at the output's rate) with the output frame it starts on and its level, and
// its audio callback asks for each block of output. A take starts on exactly
// the frame it was given and plays whole, overlapping whatever else is
// sounding; nothing here resamples, pans or decides.
//
// WHY A MIXER OF ITS OWN and not the platform's player nodes: on iOS, a take
// scheduled on an AVAudioPlayerNode at a sample time came out 25 samples (and,
// through one more node, 60 ms) away from it and 3 dB down, measured on the
// engine's own offline render (ios/FoolishTests/CuePlayerTests). Summing the
// takes here makes the start frame the frame heard, which a test can hold to
// the sample.
//
// TWO THREADS, NO LOCK. One producer (the host's main thread: cue_mix_add,
// cue_mix_clear) and one consumer (the audio callback: cue_mix_render) share
// a fixed table of slots and a bit per slot. The producer fills a free slot and
// then sets its bit (release); the consumer reads a slot whose bit it sees
// (acquire), and clears the bit once the take has played out. A slot's samples
// belong to the host, which keeps them alive until it sees the bit clear
// (cue_mix_live). The callback allocates nothing and never waits.
//
// THE CLOCK is the output's: frames rendered so far, and the host time the
// last block was stamped with (when its first frame reaches the speaker, as an
// audio callback's timestamp says), so a host can turn "heard at host time H"
// into a frame (cue_mix_frame_at).

#ifndef CUE_MIX_H
#define CUE_MIX_H

#include <stdatomic.h>
#include <stdint.h>

#define CUE_MIX_SLOTS 32

typedef struct {
    const float *x;
    int32_t n;
    int64_t start;      // the output frame its first sample plays on
    float gain;
} CueMixSlot;

typedef struct {
    CueMixSlot slot[CUE_MIX_SLOTS];
    _Atomic uint32_t live;        // bit k: slot k holds a take not yet played out
    _Atomic uint32_t clear;       // bits the producer asks the callback to drop now
    _Atomic int64_t frames;       // output frames rendered so far: the next block's first
    _Atomic uint64_t host;        // the host time stamped on the last block (0: none)
    _Atomic int64_t stamped;      // ...and the frame that stamp is of: that block's first
    double rate;                  // the output's frames a second
} CueMixer;

void cue_mix_init(CueMixer *m, double rate);

// Schedule `n` samples at `x` to start on output frame `start` at `gain`.
// Answers the slot, or -1 when every slot is busy or an argument is bad. A start
// already rendered past plays from the next block, whole.
int cue_mix_add(CueMixer *m, const float *x, int n, int64_t start, float gain);

// The slots still holding a take (the host frees a take's samples once its bit
// is gone), and a request to drop every take now (a mute).
uint32_t cue_mix_live(const CueMixer *m);
void cue_mix_clear(CueMixer *m);

// The audio callback: `frames` samples of the sum into `out`, then the clock
// moves on. `host` is the block's timestamp, 0 when there is none (an offline
// render).
void cue_mix_render(CueMixer *m, float *out, int frames, uint64_t host);

// The next frame to be rendered, and the output frame heard at host time `at`
// by the last block's stamp (`ticks_per_second` of the host clock); with no
// stamp, the next frame to be rendered.
int64_t cue_mix_now(const CueMixer *m);
int64_t cue_mix_frame_at(const CueMixer *m, uint64_t at, double ticks_per_second);

#endif
