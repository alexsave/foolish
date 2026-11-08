# Sound

Every animation beat the game plays makes a sound, struck in time with the motion.
This file says who decides what, how to hear each sound, how to replace one, and what is still open.

Written 2026-10-13, with the first set of sounds, all of which were placeholders.
Since 2026-11-07 every card sound is a model Alex approved by ear, built from his own recordings and ported to C (see "The card sounds").
Out, good, hand-off, finish and fool are still the first placeholders: nobody has recorded or modelled them yet.

## Who decides what

There are three layers, and each one knows nothing about the next one's business.

| Layer | Decides | Lives in |
| --- | --- | --- |
| The kernel | WHICH cue a beat makes, WHEN, and how a move of several cards is spread | `c/src/anim_plan.h` ("sound cues"), `c/src/anim_plan.c` |
| The sounds | what a cue's moment SOUNDS like: the takes, and how much of each gesture comes before its moment (the anchor) | `tools/soundgen/` (the models), writing `src/audio/gen/cueTakes.ts` |
| The cue table | which takes a cue plays, how loud, how much it may vary | `src/audio/cueSounds.ts` |
| The engine | turning "cue K in D ms" into a sound on the audio clock | `src/audio/cueAudio.ts` |

The kernel's answer is a rule, so it is asked once and every host gets the same one.
A host never looks at an event type to choose a sound: two hosts that did would be two sound designs.
The engine has no idea what an attack is, and must not learn.

### Where a cue comes from

A sequence's sounds are not all in its steps, so there are three sources.

**The plan.**
`AnimPlan.cues` lists everything a step's own motion makes: `{at_ms, kind, step, hits, n_cards, undoes, room_ms}`, twelve bytes a cue.
`at_ms` is on the same clock as every step's `start_ms`.
A host reads it through the generated layout (`sdk/ts/gen/anim.bots.ts`, `sdk/swift/gen/kernel.ios.swift`) like the rest of the plan.

**The roles.**
A good being said, and the shield or the opening sword changing seats, are not steps - a plain good emits no event at all.
The host asks `anim_role_cue(shown, next)` at the one place it turns the marks.
On the web that is `syncRoles` in `src/state/useRoleMotion.ts`.

**The end.**
`anim_finish_cue(AnimFinishAsk)` answers the fool's sound, everybody else's, or none while the game runs.
It is asked the end screen's own question (the elimination, a fool's seat, the seat count, the viewer), so the sound and the results list cannot disagree about who the fool is: one helper in `c/src/anim_plan.c` answers both.
The web asks it as the results screen comes up (`src/audio/useFinishCue.ts`, called from `GameView` online and from `PromoGame` for a promo code's local game).

**A repetition draw has several fools.**
A game drawn on the third repetition (`TableView.drawn`, `game.h game_fools`) makes every seat still in a fool, and `game_over` names only the lowest of them.
The rule: the viewer hears `FOOL` if they are one of the fools, and `FINISH` otherwise - a seat that went out before the draw, or a spectator.
The fools are every seat the elimination does not name, exactly as the results list ranks them, so a fool the board's `fool` field does not name still hears `FOOL`.
`test_plan_sound_cues` in `c/tests/anim_plan_test.c` holds the draw case and that a seat hears `FOOL` exactly when the list puts it last.

## The cues

The ids are stable and append-only: a host's table is keyed by them.

| Id | Cue | Made by | Struck | Strikes | Its sound |
| --- | --- | --- | --- | --- | --- |
| 1 | `DEAL` | each card of the opening deal; each card of a refill (the trump drawn last included) | as the card leaves the stock | one a card, up to 6 | the dealer's thumb slide, then the grab |
| 15 | `DEAL_LAND` | the same cards | as each lands at its seat | one a card, up to 6 | the card on the cloth, quieter the further its seat is from the viewer |
| 16 | `TURN` | the trump turned up | as it leaves the stock | 1 | the card pushed off and snapped over |
| 2 | `FLIP` | the same trump | as it lands in its slot | 1 | a throw-in's landing, and the stock set down on it 350 ms later |
| 3 | `LIFT` | cards leaving a hand for the table (attack, throw-in, transfer, cover) | as the flight opens | 1 a beat | two fingers take the card, the thumb slides it out |
| 4 | `PLACE` | a card landing on the table uncovered (attack, throw-in, transfer) | at the landing | one a card, up to 6 | the throw-in's landing |
| 17 | `PASS` | a transfer (the event's msg is `EVW_MSG_PASSED`) | at its landing, beside its `PLACE` | 1 a step | the "shfff" of the card slid on, from 150 ms after the landing |
| 5 | `COVER` | a cover landing on its attack | at the landing | one a card, up to 6 a beat | a card landing on a card |
| 6 | `PICKUP` | the table taken into a hand | as the cards lift | 1, for n cards | the pile lifted with one soft knock |
| 18 | `INTO_HAND` | the same cards | as they land in the hand | 1, for n cards | one slide into the fanned hand |
| 7 | `SWEEP` | the table swept to the discard | as the sweep opens | 1, for n cards | strokes gathering the table (one per ~2.5 cards, as many as fit the hold before it), then the pile lifted and turned |
| 8 | `PILE` | the swept cards coming to rest | as the sweep lands | 1, for n cards | the pile set softly on the discard pile |
| 9 | `OUT` | a seat going out | 90 ms behind the landing of the move that put them out | 1 | placeholder |
| 10 | `REVERT` | a refused move flying home | as it sets off | 1, of the cue it undoes, backwards | the undone cue's take, reversed |
| 11 | `GOOD` | a good said | as the badge turns | 1 | placeholder |
| 12 | `HAND_OFF` | the shield or opening sword going from one seat to another | as the mark leaves | 1 | placeholder |
| 13 | `FINISH` | the game over, viewer not the fool (or a spectator) | as the results come up | 1 | placeholder |
| 14 | `FOOL` | the game over, viewer is the fool | as the results come up | 1 | placeholder |

The rows are in the order a gesture happens; the ids are the order they were added.
Every row with a later part on screen has a cue of its own at that moment (a dealt card landing, the trump landing, the picked-up cards reaching the hand, the swept pile reaching the discard), so the kernel says when each part is struck.
Two later parts have no moment on screen, and are baked into their files instead: the stock set down 350 ms after the trump lands (Alex: "350 ms after trump lands is fine"; nothing moves), and the transfer's slide from 150 ms after its landing (nothing moves either).

Steps that move no card make no sound: a magic transition and a defender move are silent.
A good being CLEARED is silent, because the throw-in that cleared it is already making a sound.
A mark that appears where there was none (the opening) or goes away altogether (a rematch) is not a hand-off.

### A revert is the move it undoes, backwards

Alex: "the revert audio should be the audio reversed of whatever the move was".
So `REVERT` is not a sound of its own and no host keeps an asset for it.
The cue carries `undoes`, the cue of the move being taken back, and a host plays that cue's sound reversed: a refused attack is `PLACE` backwards, a refused cover `COVER` backwards, a refused pickup `PICKUP` backwards.
Replacing a move's sound with a recording therefore replaces its revert too, with nothing to keep in step.

The kernel reads which move it was off the stream.
Cards going back to the table undo a pickup.
Cards going home to a hand were laid, and the nearest step before the revert that laid one of them says whether it was a cover.
A revert whose move is not in the stream, because the refusal arrived after that run had ended, is read as `PLACE`.
That is wrong for a cover refused more than half a second late, and fixing it needs the host to say what it is undoing on the step (see the open questions).

On the web the engine reverses a decoded buffer the first time a revert needs it and keeps the result.
`e2e/audio_cues.test.ts` refuses a `REVERT` row in the table and a revert file from soundgen.

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

**The quiet before a cue (`room_ms`).**
A gesture can start well before its moment: the strokes that gather a table come before it is swept.
`room_ms` is how long before `at_ms` the plan has nothing else to strike: back to the end of the latest cue struck before it (its last strike), plus `ANIM_SOUND_ROOM_GUARD_MS` (150 ms) for that cue's own tail, or `at_ms` itself when nothing comes before it.
A host fits such a gesture into it, so no cue is ever moved for a sound and a gathering never plays over the beat before.
Measured over real games (`test_plan_cues_over_real_games` prints it): every sweep has at least 400 ms, and 29 of 85 have a second or more (the bout-end hold behind a last cover); no pickup has 400 ms, because a pickup opens its run or follows the attack it answers by one gap.

**A cue never moves once it is in a plan.**
The web appends to a run in flight and re-plans every frame.
Every cue rule reads only a step's own beat and the steps before it, so the longer plan has the shorter one's cues at the same milliseconds.
A host can therefore remember what it has scheduled by `(step, kind)`, which the kernel keeps unique.

Both properties, and that a room never reaches back past the cue before it, are held over 3913 plans of real games by `test_plan_cues_over_real_games` in `c/tests/tests.c`.

## How the web plays one

`src/state/useAnimationRun.ts` is the frame loop that already asks the kernel where the run stands.
Each frame it hands over the cues coming due (`src/audio/cueSchedule.ts`) with how far off each one is.
The engine aims each take's anchor at `AudioContext.currentTime + delay`.

**A take starts its anchor early.**
Every file is a whole gesture, and its anchor is how far into the file the cue's moment is (`src/audio/gen/cueTakes.ts`, written by soundgen).
A `LIFT` take is the grab and the thumb slide, so it starts 280-440 ms before the card leaves; a landing starts a few tens of ms before its peak; the transfer's slide has a negative anchor and starts 50 ms after its landing.
A cue is therefore handed over its longest anchor plus the look-ahead before its moment (`cueLeadMs`), so the longest gathering (a sweep's, 1.25 s) is scheduled on time whenever the plan knew about it that early.

**When there is less time than the gesture.**
The engine picks a take that fits both the plan's `room_ms` and the time left before the moment: a sweep plays the most strokes its cards ask for (one per 2.5 cards, 1-5) that fit, down to the lift alone.
A take that still starts in the past is started now, that far into the file: the moment stays where the kernel put it and the part of the gesture that can no longer be heard is not.
That is the case for every move that OPENS a run: a run starts as its push arrives, so its first lift is at 0 ms and only the tail of the thumb slide is heard (see the open questions).

**A landing at a far seat is quieter.**
`DEAL_LAND` is struck at `anim_seat_gain_pm(viewer, seat, n_seats)`, the kernel's inverse-distance level from Alex's table geometry (`deal_game.py`): at four seats a neighbour's card lands 4.7 dB under one's own and the seat across 6.8 dB under.
The web asks it with the step's seat and the board's `mySeat`; a spectator hears every seat at full level.

**A modelled sound does not wander.**
The stand-ins are played a little off pitch and level each time; a model's takes already vary as the recordings did, so its row in `cueSounds.ts` wanders by nothing and is not made heavier per card (a sweep's strokes say how many cards it is).

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
The route mounts it, not the board, so it is there for the lobby, the board and the results (`src/app/[game_id]/GameRoute.tsx` for a live game, a replay and a promo code's local game, and `src/app/tutorial/page.tsx`).
The mute and the master volume are remembered in this browser as two plain keys (`foolish_sound_muted`, `foolish_sound_volume`).
The volume slider lives on `/sounds` for now.

## What is loaded first

Every sound is loaded, but the ones the next move could make are loaded first.

**Which those are is the kernel's answer.**
`anim_cues_next(AnimCueAsk)` returns one bit per cue from seven scalars of the board a viewer holds: the status, their seat, the seat count, the piles on the table and how many are covered, the stock, and whether the trump is turned.
"Next" is one move by any seat, with everything that move brings with it.

| The board | Could sound next |
| --- | --- |
| a lobby | `DEAL`, `DEAL_LAND`, `TURN`, `FLIP` |
| a finished game | nothing |
| in play, always | `LIFT`, `PLACE`, `PASS` |
| no trump turned yet (the opening is still being dealt) | `DEAL`, `DEAL_LAND`, `TURN`, `FLIP` |
| an uncovered attack | `COVER`, `PICKUP`, `INTO_HAND` |
| cards on the table | `SWEEP`, `PILE`, `GOOD`, `HAND_OFF`, and `DEAL`, `DEAL_LAND` while there is stock |
| the stock is empty, or the bout can close and its refills (six a seat at most) could empty it | `OUT`, `FINISH`, and `FOOL` for a seated viewer |

`REVERT` is never named: the cue it plays backwards is already named wherever that move can be made.

**It is a superset, deliberately, and never less.**
A cue named and not played costs a place in a queue.
A cue played and not named is a sound that may not have arrived, which is the one failure the rule must not have.
`test_next_cues_cover_every_move` (`c/tests/tests.c`) asks the rule about 17,976 boards of real games and holds every sound each move then made to the answer: none was missed.
It names 12.0 of the 18 cues on average, so it is a strong hint at a lobby (4) and on an empty table (3), and a weak one mid-bout.

**How the queue behaves** (`src/audio/cueAudio.ts`, `src/audio/useCuePriority.ts`).

- The page asks the kernel whenever one of the seven scalars changes and passes the answer to the engine (`want`).
- Wanted files are fetched first, in the table's order, then everything else.
- Three fetches run at a time, so the order of the queue decides what arrives first.
- A fetch on its way is never restarted, and no file is asked for twice.
- Before the first gesture only the wanted files are fetched, and only as bytes: there is no audio context yet, so they are decoded by the gesture itself.
  This is the one change to "nothing before a gesture", and the reason is that the first sound is usually caused by the very tap that unlocks audio.
- A muted player is sent nothing, before or after a gesture.
- A cue that is struck jumps the queue, whatever the board said.
- A strike whose file is still on its way waits for it: it starts as the file decodes if that is no more than 80 ms behind the moment it was aimed at, and is dropped otherwise.

**Measured with the placeholders** (2026-10-13) in Chrome on the tutorial, where Start is the first gesture and the deal follows it, from a cold checkout started with `npm run dev`, cache off.
The modelled set is 35 times the bytes (see "The 4.4 MB itself"), so these numbers are history until they are measured again.
The network was throttled with Chrome's own presets after the page had loaded.

| | Before | After |
| --- | --- | --- |
| First deal sound decodable after the gesture, local | 137 ms | 108 ms |
| ...on "Fast 4G" (9 Mbit/s, 60 ms) | 84 ms | 7 ms |
| ...on "Slow 4G" (1.6 Mbit/s, 150 ms) | 257 ms | 7 ms |
| Every sound decoded after the gesture, Slow 4G | 1073 ms | 839 ms |
| Requests | 17 | 16 |
| Bytes on the wire | 160,210 | 127,240 |

Six files (33 KB) were fetched before the gesture: both deals, the flip, the lift and both places.
The local row is the cost of creating the audio context, which nothing here shortens.

**A correction.**
An earlier version of this file said the first card or two of the tutorial's deal were silent.
That was a measurement that stopped after seven seconds: the deal runs 6.75 s and its first card leaves about a second after Start.
In both columns above all eighteen dealt cards were heard.
What the change buys is margin: the sound is ready 7 ms after the tap where it used to be 257 ms on a slow connection, and a slower one than that would have lost cards.

### The 4.4 MB itself

111 files, 4,362,854 bytes, all 16-bit mono WAV (the placeholders were 16 files, 122 KB).

| Cue | Takes | Bytes | Rate |
| --- | --- | --- | --- |
| `SWEEP` | 18 (3 for each of 0-5 strokes) | 1,384,254 | 32 kHz |
| `FLIP` | 8 | 496,466 | 32 kHz |
| `LIFT` | 8 | 349,976 | 48 kHz |
| `DEAL` | 8 | 341,240 | 48 kHz |
| `INTO_HAND` | 8 | 313,864 | 32 kHz |
| `PASS` | 8 | 310,066 | 32 kHz |
| `TURN` | 8 | 222,618 | 32 kHz |
| `PILE` | 8 | 208,224 | 32 kHz |
| `PICKUP` | 8 | 196,192 | 32 kHz |
| `PLACE` | 8 | 187,840 | 32 kHz |
| `COVER` | 8 | 175,088 | 32 kHz |
| `DEAL_LAND` | 8 | 136,450 | 32 kHz |
| the five placeholders | 5 | 40,576 | 8-12 kHz |

| Option | Chosen | Why |
| --- | --- | --- |
| 32 kHz where a model has nothing above 15 kHz | yes | every model but the deal's thumb slide cuts at 15 kHz, so 32 kHz (a 15.6 kHz windowed-sinc low pass, -0.8 dB at 15 kHz, -77 dB of alias) holds all of it at two thirds of the bytes |
| 48 kHz for `DEAL` and `LIFT` | yes | they carry the thumb slide, whose recordings reach 20 kHz: 38% of its energy is above 16 kHz |
| 24 kHz | no | it would cut the 12-15 kHz the models carry |
| Trimming | yes | each take is cut to 60 dB under its peak at both ends |
| Pickup strokes | no | no real game leaves a pickup any room, so its 0-5 stroke takes (1.4 MB) would never be heard; `RECIPES` gives it levels again the day a plan does |
| One packed file for all sounds | no | it defeats loading by priority (the first cue would wait for the whole pack), and a pack needs an index, which is a second layout to keep or a manifest |
| A compressed format (AAC, Opus) | not yet: the biggest open item | it would cut this about ten times; it needs an encoder in the build, Opus does not decode in every Safari, and both add a codec's start-up padding, which the anchors would then have to include |
| Fewer takes | not by my judgment | 8 a cue (3 a stroke count) is what the models were heard as; `takes` in `RECIPES` is one number a cue |

## How to hear each cue

**In the browser: `/sounds`.**
Run `npm run dev` and open `http://localhost:3000/sounds`.
Every cue has a Play button and an x4 button (four strikes at the kernel's stagger), with its takes, their anchors and its gain beside it.
A cue's moment is set far enough off that every take is heard whole, the part before its moment included.
The sweep has a button per stroke count instead (the lift alone, then 1-5 strokes).
Under them, "In context" asks the kernel for the plan of a small move and plays its cues at the plan's own times, exactly as the board does: an attack, a three-card attack, a transfer, an attack and its cover, a bout ending, a pickup, a refused attack, a last card, the opening deal.
Like the board it starts the plan now, so the grab and the slide before the first lift are cut to what is left of them (see "When there is less time than the gesture").
It plays through the same engine and table the board uses, so what you hear there is what ships.

**In a terminal: `make -C tools/soundgen audition`.**
Plays every file in turn with `afplay`, printing each name.

**Measured: `make -C tools/soundgen stats`.**
Prints each file's rate, length, bytes, anchor, peak and RMS.

## How to replace a sound with a recording

1. Put the recording in `public/audio/`, for example `public/audio/place_a.wav`.
   Anything every target browser's `decodeAudioData` reads will do: WAV, m4a/AAC or mp3 (not ogg, which Safari will not decode).
2. In `src/audio/cueSounds.ts`, name it in the cue's row with its anchor, the ms into the file of the cue's moment (0 when the file starts on it):
   `[ANIM_CUE_PLACE]: { ...model(ANIM_CUE_PLACE), takes: [{ file: 'place_a.wav', anchorMs: 12, level: 0 }] },`
   Several takes in a row are takes of one gesture and are played in turn, never the same one twice running.
3. Set the row's `gain` by ear on `/sounds`.
4. In `tools/soundgen/soundgen.c`, set the replaced cue's `make` to `NULL` in `RECIPES` so its takes stop being generated.

A refused move needs no step of its own: it is the same take, reversed by the engine, with its anchor measured from the other end.

Nothing else changes.
`e2e/audio_cues.test.ts` fails if a row names a generated file that is not written, or with an anchor soundgen did not write it with, a recording that is not committed, or if a generated file is left with no row.
To silence a cue altogether, map it to `null`.

The same recordings can serve iOS, since they are plain files under `public/audio/`.

## The card sounds

Every card sound is a model Alex approved by ear (2026-11-01 to 11-07), each built from his own recordings, measured part by part and synthesised so that every take varies the way the real ones do.
The measurement, the reasoning and his feedback live beside each model, in Python: `tools/soundgen/{deal,flip,throwin,cover,pass,discard}/` (their `.md` notes say what each part is and why).
The C port is what ships: `tools/soundgen/c/models.c` follows each Python synth line for line, `c/dsp.c` is the handful of numpy it used (an FFT, the template eq, brick-wall band passes, moving RMS, two-pole resonators), and `c/tables.h` holds the measured tables, generated once from the models' `.npy` files by `c/tables.py`.
The build runs no Python and no numpy.

| Model | Python (approved) | In the game |
| --- | --- | --- |
| thumb slide | `deal/an/thumb2.py` thumb9 | `DEAL` (130 ms before the grab), `LIFT` (130 ms before the card leaves) |
| deal grab | `deal/an/grab2.py` | `DEAL`, as the card leaves |
| landing on felt, 229 Hz table | `deal/an/land_syn.py land229 2` | `DEAL_LAND` |
| trump flip | `flip/an/flip_syn.py flipE 3` | `TURN`, its snap as the trump leaves |
| deck set down, 229 Hz table | `flip/an/deck_syn.py deck229c 5` | `FLIP`, 350 ms after the trump lands |
| throw-in landing | `throwin/an/throw_syn.py throw9 7` | `PLACE`, and the trump's landing in `FLIP` |
| two-finger grab | `throwin/an/tgrab_syn.py tgrab2 9` | `LIFT`, 240 ms before the card leaves |
| cover landing | `cover/an/cov_syn.py cover3 7` | `COVER` |
| pass "shfff" | `pass/an/pass_syn.py pass7 4` | `PASS`, from 150 ms after the landing |
| strokes (SHFF = 1, the pass's timbre) | `discard/an/disc_syn.py` scrape | `SWEEP` (and `PICKUP`, not shipped: no room) |
| discard lift (two knocks) | `disc_syn.py` lift | `SWEEP`, 100-180 ms after the last stroke |
| placement on the pile | `disc_syn.py` place | `PILE` |
| pickup lift (one soft knock) | `disc_syn.py` plift | `PICKUP` |
| slide into the hand | `disc_syn.py` insert | `INTO_HAND` |

**A file is a gesture at the game's timing.**
`soundgen.c` lays the parts of each cue's gesture out as its `*_game.py` does, against the cue's moment, and writes the take with its anchor.
The levels are the games' own, on the deal's scale (g = -34 to -32 dBFS a take): the grab at g-5 and the slide at g-6 to g-9, a dealt landing at g-13 to g-16, the flip at g-5, the trump's landing at g-9 to g-12 and the deck at g-3 to g-6, the throw-in's grab at g-14 and its landing at g-3 to g-5, the cover's at g-4 to g-6, the shfff at g-11 to g-13, and the discard parts by 50 ms RMS against the strokes at g-30.5 (lift +5, placement +3, pickup lift -10, slide into the hand -11).
The whole set is then lifted by one constant (`LIFT_DB`, 32 dB) so the loudest take peaks at -3.3 dBFS: the deal's scale was a listening level, not a file level.
Balancing the cues against each other is still open (Alex: the cover is "quiet as fuck", the two-finger grab too quiet), and is the `gain` column of `src/audio/cueSounds.ts` (1 for every model, which is the model's level), set by ear on `/sounds`.

**The strokes are fitted, never moved.**
A sweep's takes come in six levels, 0-5 strokes, the strokes 0.8 times a recorded one's length and 60-200 ms apart as `disc_game.py` draws them, a little louder as the pile grows.
No take starts more than 1.25 s before its moment (the hold behind a last cover, less the cover's tail and an out's note): the strokes and their gaps are compressed together to fit, never under 150 ms a stroke or 30 ms a gap (Alex: "trim/compress the strokes to fit rather than moving any cue").
The engine plays the most strokes the cue's cards ask for that fit the cue's `room_ms`.

**How each was checked.**
`make -C tools/soundgen parts` writes each model alone, 12 takes at 48 kHz, named as its Python names them.
Each set was measured with `throwin/an/measure.py` (22 measures, the spectrum, the envelopes) three ways: the recordings against the Python takes, the recordings against the C takes, and the Python takes against the C takes.

| Model | C outside the Python takes' range | C outside the recordings' range (Python's own, for comparison) |
| --- | --- | --- |
| thumb9 | none | rise -20->0 (also Python), micro-rough 4-12k, rough 1/20 ms (also Python), flatness (also Python) |
| grab2 | none | crest, click width, kurtosis 1-4k, crest 1-4k, micro-rough 4-12k, flatness (all also Python); crest 150-1k, rise, micro-rough 1-4k |
| land229 | none | dur > -20 dB, click span, flatness (also Python); crest 150-1k |
| throw9 | none | dur > -10 dB (17.5 against 18), rise (10 against 11), micro-rough 4-12k (4.26 against 4.23); Python had none |
| cover3 | none | click width (1.40 against 1.47), rough 1/20 ms (5.89 against 6.08); Python's kurtosis 1-4k is inside in C |
| tgrab2 | none | micro-rough 1-4k (also Python), kurtosis 4-12k (10.4 against 11.6) |
| flipE | none | rise (51 against 57), centroid, click width; Python's kurtosis 1-4k is inside in C |
| deck229c | none | fall to -20 dB, centroid (both also Python), kurtosis 4-12k |
| pass7 | none | flatness (also Python), kurtosis 1-4k (3.77 against 3.79), crest 4-12k |
| strokes | none | crest, peak lo/full, low-high balance, centroid (all also Python: the pass's timbre, by design) |
| discard lift | none | the reps are two; the same eight as Python, and two durations |
| placement | none | the reps are two; the same as Python's, and crest 150-1k, crest 4-12k |
| pickup lift | none | the reps are three; the same as Python's |
| slide into hand | crest (15.58 against a maximum of 15.57) | rise, centroid, click width (also Python), dur > -10 dB |

The spectra of the C and the Python takes agree within 3 dB in every band but three: the cover's lowest (150 Hz, -7 dB), the two-finger grab's (150 Hz, -5 dB) and the throw-in's 600 Hz (+4 dB), each inside the Python takes' own spread.
Bit identity with numpy was never the bar (the draws come in another order, and an FFT here is zero-padded to a power of two where numpy's is circular); measuring like the approved takes is.

**Reproduce.**
From `tools/soundgen`: make the Python takes as each model's doc says (`synth/` is ignored), `make parts`, then for each model `python3 -I throwin/an/measure.py NAME 'PYTHON_GLOB' 'build/parts/NAME_*.wav'` (and the recordings' glob in place of the first).
`python3 -I c/tables.py > c/tables.h` after an analysis rewrites a table.

### The placeholders

Out, good, hand-off, finish and fool keep the first synthesised stand-ins (`c/placeholders.c`): filtered noise, a short falling sine and soft mallet notes, at 8 or 12 kHz.
They were designed by reasoning, not from recordings.

| Cue | What it is trying to be |
| --- | --- |
| `hand_off` | air, then something set down |
| `out` | two soft notes stepping down (D5, G4) |
| `good` | two small bright notes, a fifth apart (C6, G6) |
| `finish` | four notes up a major chord |
| `fool` | four notes down by semitones, the last one sagging |

Everything soundgen writes is a build artifact (`npm run sounds`, hooked onto `predev`, `prebuild`, `pretypecheck` and the test lanes' pre-hooks, since the web now imports the take table), written to `public/audio/gen/` and `src/audio/gen/` and never committed.
It takes about 12 s of one core.

## Cost

| What | Size | When it is paid |
| --- | --- | --- |
| Engine, table and scheduler | about 2.3 KB gzipped (esbuild, minified) | with the game routes' JavaScript |
| Sound button, finish hook and priority hook | about 1.2 KB gzipped | the same |
| `web.wasm.gz` | +858 B (36,974 to 37,832) | with the kernel |
| `bots.wasm.gz` | +735 B (87,992 to 88,727) | server side |
| The sound files | 4.4 MB, 111 requests (was 122 KB, 16) | wanted ones first; all of them after the first tap; none when muted |
| The take table (`src/audio/gen/cueTakes.ts`) | about 10 KB of source, a few KB gzipped | with the game routes' JavaScript |
| `/sounds` | about 2.5 KB gzipped | that route only |

The wasm numbers are measured against a build of main as of the rating stamp (PR #284).
The modelled cues (the four new ids, `room_ms`, the transfer flag and `anim_seat_gain_pm`) took `web.wasm.gz` from 49,544 to 49,924 B (+380 B, measured against this branch before them, 2026-11-07).
The JavaScript numbers are an estimate from bundling the modules alone, not a diff of two `next build` outputs.
Neither wasm module grew a page of linear memory (`e2e/mem/wasm_memory.test.ts`).

## iOS and iMessage

Nothing plays sound on iOS yet; the architecture is ready for it.
The plan wire already carries each event's `msg` (`FIO_PLAN_VERSION` 4), so an iOS plan hears a transfer as one.

**What is already there.**
`AnimPlanSnap.cues` is in the generated Swift layout with `ANIM_CUE_*`, `ANIM_N_CUES` and `ANIM_SOUND_STAGGER_MS` as constants.
`fio_role_cue`, `fio_finish_cue` and `fio_cues_next` are in `c/ios/include/ios_api.h` and held by the host smoke test.

**What a host has to add.**

1. Expose the cues on `AnimPlan` in `sdk/swift/AnimPlanWire.swift`, beside `steps` (a `Cue` struct copied from `pl.cues`, `room_ms` included).
2. A `CuePlayer` in FoolishKit: an `AVAudioEngine` with one `AVAudioPlayerNode` pool, the files loaded once as `AVAudioPCMBuffer`, and a table keyed by cue id of takes with their anchors and levels - the twin of `cueSounds.ts`.
   soundgen's take table would then also be written as Swift (or read through the kernel), the anchors being soundgen's.
   A take is started its anchor before its moment, as on the web, and a `DEAL_LAND` at the seat's gain (an `fio_` export of `anim_seat_gain_pm` is still to add).
   Schedule with `scheduleBuffer(_:at:)` against the engine's clock, never `DispatchQueue.asyncAfter`.
3. In `MessageTableView+Sequence.swift`, where `playStep` builds a beat's flights, schedule the cues whose `step` falls in that beat at `cue.atMs - step.startMs` from the beat's start, each strike `ANIM_SOUND_STAGGER_MS` after the last.
   iOS walks beats and sleeps between them, so the offset is taken from the beat, not from the sequence.
4. Ask `fio_role_cue` in `syncRoles` (`MessageTableView+Roles.swift`) and `fio_finish_cue` (with the arguments `fio_finish_rows` already takes) where `gameOverHold` ends.
5. Use the `.ambient` audio session category, so the ringer switch silences it and it mixes with the player's music.
6. Play a `REVERT` as the buffer of `cue.undoes`, reversed (copy the samples back to front once and keep the buffer).
7. Load the buffers `fio_cues_next` names first.

An iMessage extension has its own constraints (memory, and it is torn down between bubbles), so the buffers should be loaded lazily and the engine stopped when the board goes away.

## Open questions for Alex

1. **On by default?**
   Sound is on until muted, at 70% master volume.
   Many web games start muted; say if you want that.
2. **A transfer is its own sound now** (`PASS`, beside its `PLACE`): the plan reads the event's `msg` (`EVW_MSG_PASSED`) through `ClientPlanStep.msg` and the iOS plan wire, and my own transfer is heard as one before the server confirms it.
   An attack and a throw-in are still one sound, which is what Alex modelled.
   A refused transfer reverts as `PLACE` backwards, without its slide.
3. **The grab and thumb slide of a move that opens its run are not heard.**
   A run starts as its push arrives, so its first `LIFT` is at 0 ms, and the 280-440 ms of grab and slide before the card leaves are skipped: only the slide's tail is heard.
   Most moves open their run (one push a move), so the lift's gesture is mostly heard where a move follows another inside one push (a cover after an attack, a refill's deal).
   The alternative is to play it whole and late, starting as the card leaves, which puts the grab under the flight; say if you prefer that.
   The same applies to a pickup's strokes, and more so: no plan ever leaves a pickup room for them, so they are not shipped at all.
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
9. **A cover refused late reverts as `PLACE`, not `COVER`.**
   The kernel reads the undone move off the stream, and a refusal that arrives after the move's run has ended finds nothing there.
   Carrying it exactly means one more field on `ClientPlanStep` (`c/src/client_table.h`) for the host to say what it is undoing.
10. **A replay has no finish sound**, because it has no results screen of its own.
11. **Seat position.**
    A cue carries its step, so a host can pan a sound towards the seat that made it.
    The web plays everything centred; only a dealt card's landing follows its seat, in level.
12. **The balance between the cues.**
    Every model is written at its `*_game.py` level and the mix column is 1 throughout: the cover is still "quiet as fuck" and the two-finger grab too quiet, by Alex's ear, and that is his to set on `/sounds`.
    The five placeholders keep their old gains against a much quieter card set; they will want the same pass.
13. **4.4 MB of WAV.**
    A compressed format would cut it about ten times, and is the one real cost left (see "The 4.4 MB itself").
14. **The strokes before a sweep are compressed** to fit the hold before it (at most 1.25 s, strokes down to 150 ms), and fewer are played when the hold is shorter.
    A sweep with no hold before it (an attacker's "good" in a later push) is the lift alone.
