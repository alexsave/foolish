# Sample licences

Every sample library used by the city themes, with its licence and source.
The files are read only as data by our own C code; nothing inside a download is ever executed.

| Library | Used for | Licence | Source |
|---|---|---|---|
| VSCO 2 Community Edition (Versilian Studios, Sam Gossner) | string sections (violin, viola, cello), solo contrabass, horn, trumpet, tenor trombone, tuba, flute, oboe, clarinet, bassoon, harp, glockenspiel, marimba, timpani, snares, bass drums, congas, bongos | CC0 1.0 (public domain dedication) | https://github.com/sgossner/VSCO-2-CE (LICENSE file in the repo, https://raw.githubusercontent.com/sgossner/VSCO-2-CE/master/LICENSE) |
| Salamander Grand Piano V3 (Alexander Holm), 48 kHz / 24-bit | piano | CC-BY 3.0: attribution "Salamander Grand Piano by Alexander Holm" is required | https://archive.org/details/SalamanderGrandPianoV3 (licence stated in the archive metadata and the README inside the archive) |
| Versilian Community Sample Library (VCSL, Versilian Studios) | vibraphone, TX81Z FM piano, tubular bells, pipe organ, tenor saxophone, rope-tension field drum, clash and suspended cymbals, timpani, congas, bongos, cowbells, claves, guiro, shakers, tambourines, woodblock, glockenspiel, concert harp | CC0 1.0 | https://github.com/sgossner/VCSL (README: "This collection is under a Creative Commons 0 license") |
| Karoryfer Samples, Big Rusty Drums (via sfzinstruments) | kick, snare, hi-hats, crash and ride for New York, Seoul and Miami | CC0 1.0 (LICENSE file in the repo) | https://github.com/sfzinstruments/karoryfer.big-rusty-drums |
| D. Smolken double bass (via sfzinstruments) | upright bass pizzicato (New York, Madrid) | CC0 1.0 (LICENSE file in the repo) | https://github.com/sfzinstruments/dsmolken.double-bass |
| Musopen, Mozart: The Marriage of Figaro overture, multitrack session (premix_02 L/R only) | measuring reference only, never in a theme: a real orchestra's fullness, band balance and stereo width to compare our strings against | CC0 1.0 (licenseurl in the archive metadata) | https://archive.org/details/musopen-mozart-marriage-of-figaro-compressed (two files fetched from inside the zip, merged to one 48 kHz stereo file with ffmpeg) |
| Apple General MIDI DLS (gs_instruments.dls, ships with macOS) | choir "aah" only, where no free choir library could be verified | Apple system component; used only as rendered sound inside the game, as before | /System/Library/Components/CoreAudio.component |

Downloads: VSCO 2 CE 2.1 GB, Salamander 1.9 GB (extracted), Big Rusty Drums 0.26 GB (converted from FLAC with macOS `afconvert`), VCSL subset 1.0 GB, double bass 0.12 GB. Total about 5.4 GB under `samples/`.
Libraries considered and not used: Sonatina Symphonic Orchestra (CC Sampling Plus, unclear for redistribution), Philharmonia samples (conflicting terms), Virtual Playing Orchestra (mixed licences), jRhodes (CC BY-NC, no commercial use), FreePats Rhodes (GPL).
