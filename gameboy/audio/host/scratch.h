// scratch.h - host/snd_test.c's stand-in for the bots' scratch the driver's
// state is overlaid on (snd.c SND_OVERLAY): the test writes over it as a
// think would.
#include <stdint.h>
typedef union { void *align; uint8_t b[256]; } snd_test_scratch;
extern snd_test_scratch snd_test_ram;
#define SND_RAM       (snd_test_ram.b)
#define SND_RAM_BYTES 256u
