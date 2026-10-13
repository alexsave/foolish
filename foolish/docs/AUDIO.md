# Sound

Every animation beat the game plays makes a sound, struck in time with the motion.
This file says who decides what, how to hear each sound, how to replace one, and what is still open.

Written 2026-10-13, with the first set of sounds, all of which are placeholders.

## Who decides what

There are three layers, and each one knows nothing about the next one's business.

| Layer | Decides | Lives in |
| --- | --- | --- |
| The kernel | WHICH cue a beat makes, WHEN, and how a move of several cards is spread | `c/src/anim_plan.h` ("sound cues"), `c/src/anim_plan.c` |
| The cue table | which FILE a cue plays, how loud, how much it may vary | `src/audio/cueSounds.ts` |
| The engine | turning "cue K in D ms" into a sound on the audio clock | `src/audio/cueAudio.ts` |

The kernel's answer is a rule, so it is asked once and every host gets the same one.
A host never looks at an event type to choose a sound: two hosts that did would be two sound designs.
The engine has no idea what an attack is, and must not learn.

### Where a cue comes from

A sequence's sounds are not all in its steps, so there are three sources.

**The plan.**
`AnimPlan.cues` lists everything a step's own motion makes: `{at_ms, kind, step, hits, n_cards}`, eight bytes a cue.
`at_ms` is on the same clock as every step's `start_ms`.
A host reads it through the generated layout (`sdk/ts/gen/anim.bots.ts`, `sdk/swift/gen/kernel.ios.swift`) like the rest of the plan.

**The roles.**
A good being said, and the shield or the opening sword changing seats, are not steps - a plain good emits no event at all.
The host asks `anim_role_cue(shown, next)` at the one place it turns the marks.
On the web that is `syncRoles` in `src/state/useRoleMotion.ts`.

**The end.**
`anim_finish_cue(game_over, my_seat)` answers the fool's sound, everybody else's, or none while the game runs.
The web asks it as the results screen comes up (`src/audio/useFinishCue.ts`, called from `GameView`).

## The cues

The ids are stable and append-only: a host's table is keyed by them.

| Id | Cue | Made by | Struck | Strikes |
| --- | --- | --- | --- | --- |
| 1 | `DEAL` | each card of the opening deal; each card of a refill | as the card leaves the stock | one a card, up to 6 |
| 2 | `FLIP` | the trump turned face up | as it lands in its slot | 1 |
| 3 | `LIFT` | cards leaving a hand for the table (attack, throw-in, transfer, cover) | as the flight opens | 1 a beat |
| 4 | `PLACE` | a card landing on the table uncovered (attack, throw-in, transfer) | at the landing | one a card, up to 6 |
| 5 | `COVER` | a cover landing on its attack | at the landing | one a card, up to 6 a beat |
| 6 | `PICKUP` | the table taken into a hand | as the cards lift | 1, for n cards |
| 7 | `SWEEP` | the table swept to the discard | as the sweep opens | 1, for n cards |
| 8 | `PILE` | the swept cards coming to rest | as the sweep lands | 1, for n cards |
| 9 | `OUT` | a seat going out | 90 ms behind the landing of the move that put them out | 1 |
| 10 | `REVERT` | a refused card flying home | as it sets off | 1 |
| 11 | `GOOD` | a good said | as the badge turns | 1 |
| 12 | `HAND_OFF` | the shield or opening sword going from one seat to another | as the mark leaves | 1 |
| 13 | `FINISH` | the game over, viewer not the fool (or a spectator) | as the results come up | 1 |
| 14 | `FOOL` | the game over, viewer is the fool | as the results come up | 1 |

Steps that move no card make no sound: a magic transition and a defender move are silent.
A good being CLEARED is silent, because the throw-in that cleared it is already making a sound.
A mark that appears where there was none (the opening) or goes away altogether (a rematch) is not a hand-off.

### The rules that keep it from being noise

**Several cards are several strikes, but not a chord.**
A move of n cards lands as one movement, and n copies of one sample started in the same instant sum into one louder, harsher sample.
So the strikes of a beat are spread `ANIM_SOUND_STAGGER_MS` (45 ms) apart, from the landing onwards.
A sound that trails its picture by a few frames reads as its consequence; one that leads it reads as a mistake.

**No beat strikes more than `ANIM_SOUND_MAX_HITS` (6) times.**
A table of twelve cards swept away is one sweep, which is why the sweeps are a kind of their own: one sound that says how many cards it stands for.

**The spread runs across the whole beat.**
The kernel spends one COVER event per card, so a two-card cover is two steps and one beat: one `LIFT`, and the second `COVER` is the beat's second strike.

**An out is struck with the move that caused it.**
Its own step sits a bout-end hold (1.5 s) later when it follows a last defence, which is long after the badge collapsed.

**A cue never moves once it is in a plan.**
The web appends to a run in flight and re-plans every frame.
Every cue rule reads only a step's own beat and the steps before it, so the longer plan has the shorter one's cues at the same milliseconds.
A host can therefore remember what it has scheduled by `(step, kind)`, which the kernel keeps unique.

Both properties are held over 4122 plans of real games by `test_plan_cues_over_real_games` in `c/tests/tests.c`.

## How the web plays one

`src/state/useAnimationRun.ts` is the frame loop that already asks the kernel where the run stands.
Each frame it hands over the cues coming due (`src/audio/cueSchedule.ts`) with how far off each one is.
The engine starts a buffer source at `AudioContext.currentTime + delay`.

**The audio clock, not a timer.**
A cue is handed over up to 120 ms early (`CUE_LOOK_AHEAD_MS`), so a late frame does not make a late sound.
Measured in Chrome against a live game: a card's landing was aimed 499 to 500 ms after its lift, with the frames that scheduled them 380 to 393 ms apart.

**When it is silent, on purpose.**

- Before the first tap or key: browsers refuse audio nobody asked for, so the context is not created until a gesture.
- While muted: nothing is fetched or decoded for a player who has it off.
- While the tab is hidden.
- Where there is no Web Audio (Node, jsdom, old browsers): every call is a no-op.
- When a file did not load: that cue is silent and the rest play.
- After a tab was away: a strike more than 80 ms behind the clock is dropped, so the run it missed is not replayed in a burst.

**Not tied to `prefers-reduced-motion`.**
That setting is about vestibular triggers, and a sound is not one.
A player who has motion turned down is leaning on the other channels to follow the table, so muting them too would be backwards.
The control for sound is the mute.

**The control.**
`src/components/SoundButton.tsx` is one icon button: tap to mute or unmute.
The route mounts it, not the board, so it is there for the lobby, the board and the results (`src/app/[game_id]/page.tsx`, `src/app/tutorial/page.tsx`).
The mute and the master volume are remembered in this browser as two plain keys (`foolish_sound_muted`, `foolish_sound_volume`).
The volume slider lives on `/sounds` for now.

## How to hear each cue

**In the browser: `/sounds`.**
Run `npm run dev` and open `http://localhost:3000/sounds`.
Every cue has a Play button and an x4 button (four strikes at the kernel's stagger), with its files and gain beside it.
Under them, "In context" asks the kernel for the plan of a small move and plays its cues at the plan's own times: an attack, a three-card attack, a bout ending, a pickup, a last card, the opening deal.
It plays through the same engine and table the board uses, so what you hear there is what ships.

**In a terminal: `make -C tools/soundgen audition`.**
Plays every placeholder file in turn with `afplay`, printing each name.

**Measured: `make -C tools/soundgen stats`.**
Prints each file's length, bytes, peak, RMS and spectral centroid.

## How to replace a sound with a recording

1. Put the recording in `public/audio/`, for example `public/audio/place_a.wav`.
   Anything every target browser's `decodeAudioData` reads will do: WAV, m4a/AAC or mp3 (not ogg, which Safari will not decode).
   Trim the silence off the front, because the engine starts the file at the cue's millisecond.
2. In `src/audio/cueSounds.ts`, name it in the cue's row:
   `[ANIM_CUE_PLACE]: card(['place_a.wav', 'place_b.wav'], 0.7),`
   Several files in a row are takes of one gesture and are played in turn, never the same one twice running.
3. Set the row's `gain` by ear on `/sounds`.
   The placeholders are all written at one peak level, so that column is the whole mix.
4. In `tools/soundgen/soundgen.c`, set the replaced cue's `make` to `NULL` in `RECIPES` so its placeholder stops being generated.

Nothing else changes.
`e2e/audio_cues.test.ts` fails if a row names a placeholder that is not generated, a recording that is not committed, or if a generated file is left with no row.
To silence a cue altogether, map it to `null`.

The same recordings can serve iOS, since they are plain files under `public/audio/`.

## The placeholders

`tools/soundgen/soundgen.c` synthesises them: 17 WAV files, 155,110 bytes, 16-bit mono, 24 kHz for the card sounds and 12 kHz for the four that are notes.
Each is filtered noise, a short falling sine and a click, added together; the header of that file describes the four layers.
They are a build artifact (`npm run sounds`, hooked onto `predev` and `prebuild`), written to `public/audio/gen/` and never committed.

They were designed by reasoning about what a card sounds like and checked by measurement (peak, RMS, centroid, length), in a browser that the signal reaches the output.
**Nobody has listened to them.**
The agent that wrote them cannot hear, so treat the whole set as a first guess at shape and timing, not at timbre.

| Cue | What it is trying to be |
| --- | --- |
| `deal` | a card's edge dragging off the card under it: a bright 85 ms burst rising in pitch |
| `flip` | a turn, then the card laid flat: a short swell and a snap |
| `lift` | a card drawn out of a held fan: soft, no contact |
| `place` | a snap on felt with the table under it: click, noise burst, a 190 Hz thump |
| `cover` | card on card, then the pair on the table: two contacts 7 ms apart, brighter, less table |
| `pickup` | one slide with cards catching along it, squared up at the end |
| `sweep` | a long slide over felt, nothing struck |
| `pile` | a soft low thud with a little paper on top |
| `revert` | the lift played backwards |
| `hand_off` | air, then something set down |
| `out` | two soft notes stepping down (D5, G4) |
| `good` | two small bright notes, a fifth apart (C6, G6) |
| `finish` | four notes up a major chord |
| `fool` | four notes down by semitones, the last one sagging |

## Cost

| What | Size | When it is paid |
| --- | --- | --- |
| Engine, table and scheduler | about 1.75 KB gzipped (esbuild, minified) | with the game routes' JavaScript |
| Sound button and finish hook | about 0.95 KB gzipped | the same |
| `web.wasm.gz` | +592 B (36,708 to 37,300) | with the kernel |
| `bots.wasm.gz` | +553 B (87,138 to 87,691) | server side |
| The placeholder files | 155 KB, 17 requests | after the first tap, and only when not muted |
| `/sounds` | about 2.5 KB gzipped | that route only |

The wasm numbers are measured against a build of the base commit.
The JavaScript numbers are an estimate from bundling the modules alone, not a diff of two `next build` outputs.
Neither wasm module grew a page of linear memory (`e2e/mem/wasm_memory.test.ts`).

## iOS and iMessage

Nothing plays sound on iOS yet; the architecture is ready for it.

**What is already there.**
`AnimPlanSnap.cues` is in the generated Swift layout with `ANIM_CUE_*`, `ANIM_N_CUES` and `ANIM_SOUND_STAGGER_MS` as constants.
`fio_role_cue` and `fio_finish_cue` are in `c/ios/include/ios_api.h` and held by the host smoke test.

**What a host has to add.**

1. Expose the cues on `AnimPlan` in `sdk/swift/AnimPlanWire.swift`, beside `steps` (a `Cue` struct copied from `pl.cues`).
2. A `CuePlayer` in FoolishKit: an `AVAudioEngine` with one `AVAudioPlayerNode` pool, the files loaded once as `AVAudioPCMBuffer`, and a `[Int: [AVAudioPCMBuffer]]` table keyed by cue id - the twin of `cueSounds.ts`.
   Schedule with `scheduleBuffer(_:at:)` against the engine's clock, never `DispatchQueue.asyncAfter`.
3. In `MessageTableView+Sequence.swift`, where `playStep` builds a beat's flights, schedule the cues whose `step` falls in that beat at `cue.atMs - step.startMs` from the beat's start, each strike `ANIM_SOUND_STAGGER_MS` after the last.
   iOS walks beats and sleeps between them, so the offset is taken from the beat, not from the sequence.
4. Ask `fio_role_cue` in `syncRoles` (`MessageTableView+Roles.swift`) and `fio_finish_cue` where `gameOverHold` ends.
5. Use the `.ambient` audio session category, so the ringer switch silences it and it mixes with the player's music.

An iMessage extension has its own constraints (memory, and it is torn down between bubbles), so the buffers should be loaded lazily and the engine stopped when the board goes away.

## Open questions for Alex

1. **On by default?**
   Sound is on until muted, at 70% master volume.
   Many web games start muted; say if you want that.
2. **Attack, throw-in and transfer are one cue (`PLACE`).**
   The wire names all three `ANIM_EVT_ATTACK_PASS`, and the plan is not told which it was.
   A transfer is still heard as its own thing, because the shield it sends on makes `HAND_OFF`.
   Telling them apart inside the plan needs the event's `msg` (`EVW_MSG_ATTACKED` / `EVW_MSG_PASSED`) carried through `client_plan_step` and iOS's `fio_anim_plan`; a throw-in can be told from an opening attack by the row.
   Say if you want three sounds and I will carry it through.
3. **The `LIFT` at the start of a flight.**
   Every card played is heard twice, leaving and landing.
   It is quiet (gain 0.2) and may still be one sound too many; mapping it to `null` in the table removes it.
4. **`HAND_OFF` at every bout end.**
   The roles change hands after every bout, so this is heard often.
   It may want to be only the transfer's sound.
5. **My own transfer sounds its hand-off one frame after the card lands, not as it leaves.**
   That is where the mark turns today for a predicted pass (it has no settled board to read the next defender from).
   A transfer that arrives hands on as it opens.
   This is the existing visual timing, and the sound follows it.
6. **Where should the volume control live?**
   The board has a mute button; the slider is only on `/sounds`.
7. **The button's label is English** (`Turn sound off`), like the sign-out button's.
   A key in `c/i18n` has to arrive in all twenty-five languages at once.
8. **No sound for the game starting, for a card being selected, or for a button press.**
   Only animation beats were asked for.
9. **The first card or two of the tutorial's deal are silent.**
   Its Start button is the first gesture, the deal begins at once, and the files take about 200 ms to arrive and decode.
   In a live game the gesture happened long before, on the dashboard or in the lobby.
10. **A replay has no finish sound**, because it has no results screen of its own.
11. **Seat position.**
    A cue carries its step, so a host can pan a sound towards the seat that made it.
    The web plays everything centred.
