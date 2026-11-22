# Game Boy Durak: sound design

Work in progress; the full document replaces this file.

## Hypotheses (written before measuring)

H1. The website's slides, scrapes and grabs (DEAL, LIFT, TURN, PASS, the strokes, INTO_HAND) have at least 30% of their energy above 4 kHz, so 4-bit PCM at 8192 Hz (Nyquist 4096 Hz) loses most of what makes them; the landings (PLACE, COVER, DEAL_LAND, FLIP, PILE) have under 15% there.
H2. Fitted noise synthesis driven at 241 Hz matches the takes' 5 ms envelopes within 3 dB on average (frames within 30 dB of the peak); driven at 60 Hz it is at least 2 dB worse on the landings, whose fall to -20 dB takes about 25 ms.
H3. 4-bit PCM at 8192 Hz matches the landings' envelopes better than synthesis (within 2 dB) and their spectra within 3 dB below 4 kHz, but misses the slides' spectra by more than 6 dB in the bands above 4 kHz.
H4. ROM: the synthesis tables for every cue, three takes a card cue, are under 6 KB; PCM of the same takes at 8192 Hz, 4 bits, is about 4 KB a second and 25 to 40 KB in all.
H5. RAM: the driver's working state is under 64 bytes, and with it overlaid on the search bots' scratch it costs no work RAM at all.
H6. CPU: the synthesis driver costs under 150 M-cycles a tick while sounding (about 3.5% of the CPU at 241 Hz) and nothing during a bot's think, because every card cue is over before the think starts; CH3 PCM costs 8% to 15% of the CPU while it plays (a 256 Hz reload interrupt through GBDK's dispatcher).
