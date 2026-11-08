# Sound

Every animation beat the game plays makes a sound, struck in time with the motion.
This file says who decides what, how to hear each sound, how to replace one, and what is still open.

Written 2026-10-13, with the first set of sounds, all of which were placeholders.
Since 2026-11-07 every card sound is a model Alex approved by ear, built from his own recordings and ported to C (see "The card sounds").
Out, good, hand-off, finish and fool are still the first placeholders: nobody has recorded or modelled them yet.

## Who decides what

There are four layers, and each one knows nothing about the next one's business.

| Layer | Decides | Lives in |
| --- | --- | --- |
| The plan | WHICH cue a beat makes, WHEN, and how a move of several cards is spread | `c/src/anim_plan.h` ("sound cues"), `c/src/anim_plan.c` |
| The sounds | what a cue's moment SOUNDS like: the takes, how much of each gesture comes before its moment (the anchor), and how loud each cue sits (the mix) | `tools/soundgen/` (the models, `RECIPES`, `tuning.tsv` for every part's time and level and every cue's mix, the recordings), writing the files and the take table `c/build/gen/cue_takes.c` |
| The voice | what a strike PLAYS: which take, when its file starts, how loud and how fast, reversed or not, the strokes before it | `c/src/cue_voice.h`, `cue_voice.c`, and `cue_voice_table.c` (the one voice over the take table) |
| The engine | fetching the files, and starting what the voice answers on the host's audio clock | `src/audio/cueAudio.ts` on the web, `ios/FoolishKit/Sound/CuePlayer.swift` on iOS |

The kernel's answers are rules, so they are asked once and every host gets the same ones.
A host never looks at an event type to choose a sound, and never chooses a take: two hosts that did would be two sound designs.
The engine has no idea what an attack is, and must not learn.

### The rules are C, for both hosts

Every decision between "cue K, its moment D from now, for N cards" and a file starting is `cue_strike` in `c/src/cue_voice.c`.
Both hosts link `c/src/cue_voice_table.c` beside soundgen's take table and export it under their own prefix, one line each: the web as `wasm_cue_*` (`c/wasm/wasm_bots_api.c`, read in `sdk/ts/wasm/bots.ts`), iOS as `fio_cue_*` (`c/ios/ios_api_sound.c`).

| Export | Answers |
| --- | --- |
| `cue_n_takes`, `cue_take(i)` | the take table: each file's name (served as `<name>.m4a`), rate, length to the sample, anchor in microseconds, cue, role and stroke count (a `CueTakeRow`) |
| `cue_lead_us(kind)` | how long before its moment a cue's earliest take starts: when a host must hand it over |
| `cue_seed(seed)` | seeds the voice's generator, once a page or a board |
| `cue_strike(kind, lead_us, n_cards, undoes, gain_pm)` | a `CueStrike`: the take (or -1), when its file starts, its level and speed in thousandths, whether it plays backwards, and its strokes (a take, when, how loud, how fast) |
| `wasm_cue_mix(kind)` | a cue's mix (a `CueMix`), for `/sounds` only, which shows the level and gives a cue with strokes a button per stroke count |

The structs cross through generated readers (`tools/structgen/specs/view_layout.args` for the web, `ios_layout.args` for iOS), so no host reads a byte of them by hand.

**The voice's state lives in the module**, not in the host: the take each cue played last and its generator are one `CueVoiceState` in static storage, like the one resident game, because a page or an iMessage extension has one board.
The web's engine seeds it on its first strike, from `Math.random`.

**The web's engine made these decisions in TypeScript first.**
Before that copy was deleted it was driven over 1839 strikes (the real games of `test_plan_cues_over_real_games`, every `/sounds` button and move, and 700 strikes of every kind), with the C voice's generator in place of `Math.random`, and what it started was written down: `c/tests/cue_voice_parity.h`, with the take table it ran against frozen beside it.
`test_cue_voice_matches_the_web` in `c/tests/anim_plan_test.c` plays the same strikes through `cue_strike`, under the shipped mix and under one that wanders and is heavier per card.
Every take, way round and prelude matches; levels and speeds agree within the C's thousandths, and starts within the 0.1 ms the web's table rounded anchors to.
The one difference found was that the C drew from its generator for a cue with one take and the web did not, so the two fell out of step after the first placeholder; the C now draws only when there is a choice.
`e2e/audio_cues.test.ts` holds the web's engine to the kernel's answer field for field (take, start, level, speed, reversal, strokes) over every cue, lead, card count and seat gain.

### Where a cue comes from

A sequence's sounds are not all in its steps, so there are three sources.

**The plan.**
`AnimPlan.cues` lists everything a step's own motion makes: `{at_ms, kind, step, hits, n_cards, undoes}`, twelve bytes a cue.
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

The voice answers the undone cue's take with `backwards` set and its start measured from the file's other end; on the web the engine reverses the decoded buffer the first time a revert needs it and keeps the result.
`e2e/audio_cues.test.ts` refuses a `REVERT` take in the table and a revert file from soundgen.

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

Both properties are held over 3913 plans of real games by `test_plan_cues_over_real_games` in `c/tests/tests.c`.

## How the web plays one

`src/state/useAnimationRun.ts` is the frame loop that already asks the kernel where the run stands.
Each frame it hands over the cues coming due (`src/audio/cueSchedule.ts`) with how far off each one is.
The engine asks the voice what the strike plays and starts each file at `AudioContext.currentTime` plus the start it is given.
The rules below are the voice's (`c/src/cue_voice.c`), and iOS plays by the same ones.

**A take starts its anchor early.**
Every file is a whole gesture, and its anchor is how far into the file the cue's moment is (the take table, written by soundgen).
A `LIFT` take is the grab and the thumb slide, so it starts 280-440 ms before the card leaves; a landing starts a few tens of ms before its peak; the transfer's slide has a negative anchor and starts 50 ms after its landing.
A cue is therefore handed over its longest anchor plus the look-ahead before its moment (`cue_lead_us`), so the longest gathering (five strokes before a sweep, 3.5 s) is scheduled on time whenever the plan knew about it that early.

**A take is always played whole** (Alex: "you should definitely play them ... and play them in full").
When the time left before a moment is shorter than a take's anchor, the take starts now, from its first sample, and its moment comes late; nothing is ever started part way into its file.
That is the case for every move that OPENS a run: a run starts as its push arrives, so its first `LIFT` is at 0 ms.
For it the deal and the lift have TIGHT takes, the grab and the thumb slide moved closer together and to the moment (Alex: "maybe put them closer to each other"): the lift's grab 60 ms before the slide's peak and the peak 30 ms before the card leaves, where they were 110 and 130 ms; the deal's slide peak 50 ms before the grab, where it was 130.
The voice plays an ordinary take when one fits the time it has, and a tight one when none does, so a run's first lift is heard whole and late by 180-290 ms (the slide's own swell before its peak) where an ordinary take would be 280-440 ms late.

**The strokes before a sweep or a pickup are a prelude.**
They are a take of their own, one set per stroke count (one stroke per 2.5 cards of the cue, 1-5, as `disc_game.py` counts them), each stroke as long as the recordings' (0.8 times one, 0.25-0.7 s) and never compressed.
The voice starts them their anchor before the cue (the last stroke ends 100-180 ms before a sweep's lift, 50-120 ms before a pickup's), over whatever is sounding then (Alex: "just play the pickup over the other sounds then"), and the lift's own take still lands on its cue.
With no time before the cue at all (a pickup opens its run) the strokes start at once, in full, and the lift and the cards reaching the hand keep their own cues.

**A landing at a far seat is quieter.**
`DEAL_LAND` is struck at `anim_seat_gain_pm(viewer, seat, n_seats)`, the kernel's inverse-distance level from Alex's table geometry (`deal_game.py`): at four seats a neighbour's card lands 4.7 dB under one's own and the seat across 6.8 dB under.
The web asks it with the step's seat and the board's `mySeat`; a spectator hears every seat at full level.

**A modelled sound does not wander.**
The stand-ins are played a little off pitch and level each time; a model's takes already vary as the recordings did, so its mix line in soundgen's `tuning.tsv` wanders by nothing and is not made heavier per card (a sweep's strokes say how many cards it is).
On the web a speed is the source's `playbackRate`; iOS keeps a player per speed and resamples with `fio_cue_resample`.

**The audio clock, not a timer.**
A cue is handed over up to 120 ms early (`CUE_LOOK_AHEAD_MS`), so a late frame does not make a late sound.
Measured in Chrome against a live game: a card's landing was aimed 499 to 500 ms after its lift, with the frames that scheduled them 380 to 393 ms apart.
Measured again after the rules moved to C (2026-11-07), in headless Chrome with its audio muted, by wrapping `AudioBufferSourceNode.start` on `/sounds` and pressing every button: 173 takes started, every one whole (its buffer the table's length), and each take's moment (its start plus its anchor, from the other end for a revert) on the plan's time to 0.1 ms in every "In context" move - the landings at 500, 545 and 590 ms, the cover at 1025, the sweep and its strokes at 2025, the pile at 2525, the opening deal's cards 375 ms apart with their landings 350 ms after each.
The only moments off by more (5 ms) were a take's first strike while its file was still decoding, which the engine starts late by the rule above.

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
The modelled set is 15 times the bytes (see "Shipped as AAC"), so these numbers are history until they are measured again.
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

### Shipped as AAC

139 files, 1,783,626 bytes of AAC in MP4 (`.m4a`), from 6,468,802 bytes of 16-bit mono WAV (the placeholders were 16 files, 122 KB).
soundgen renders the WAVs into `tools/soundgen/build/wav/`, ffmpeg's native AAC encoder (mono, `-aac_coder fast`) encodes each into `public/audio/gen/`, and `soundgen check` holds every file to its take.

**The bitrates are measured, not chosen** (`tools/soundgen/aac/bitrate.py`).
Each set of takes is encoded at 48-192 kbps exactly as the build does, decoded, and held against the WAVs: the median 1/12-octave spectrum within 0.5 dB in every band within 30 dB of the loudest, and every `measure.py` measure moved by no more than a quarter of the takes' own spread.
The lowest that passes ships (the `kbps` of each set in `RECIPES`): 96 kbps for the flip, the throw-in and cover landings, the pile and the dealt landing; 128 for the deal, the lift, the turn, the pass, the pickup, the sweep and its strokes and the slide into the hand; 160 for the tight lift and the pickup's strokes; 192 for the tight deal (four takes, so its median moves most).
At 8 and 12 kHz the encoder gives no more than about 64 kbps whatever it is asked; the five placeholders ship at 64 and measure within 0.8 dB, where the one measure each still misses (a 54 dB low-high balance, a centroid) is the same at every bitrate.

**The priming, measured.**
An AAC encoder starts its stream with priming samples (ffmpeg's: 1024), and an anchor counts samples from a take's first sample.
The file says where the take starts in its edit list (`elst`, media time 1024), and with the movie's timescale set to the sample rate (`-movie_timescale`) it says how long the take is to the sample (at ffmpeg's default of 1000 the end is rounded to the millisecond).
Decoded, one take of every set (and every lift) against its WAV, by cross-correlation and length:

| Decoder | Lag | Length |
| --- | --- | --- |
| headless Chrome's `decodeAudioData` (FFmpeg) | 0 samples, every take | equal, every take |
| headless Firefox's `decodeAudioData` (driven over WebDriver BiDi) | 0 | equal |
| Core Audio (`afconvert`, the AudioFile and ExtAudioFile API WebKit's `AudioFileReaderCocoa` decodes with on macOS and iOS) | 0 | equal |
| the ffmpeg CLI | 0 | 1024-frame padding not trimmed at the end (it honours the edit's start only) |

Safari itself was not driven: there is no headless Safari, and `safaridriver` opens a window.
Its decoder is Core Audio's, measured above, and Apple's TN2258 documents the edit list as how Core Audio reads AAC priming.
So no anchor is corrected: every browser starts the WAV's first sample on the cue.
`e2e/audio_cues.test.ts` holds it: `make check` (every file one edit, starting at the priming, as long as its take, at its rate) and ffmpeg's decode of one take of every set lined up with its WAV to the sample; both halves were mutation-checked (no movie timescale; a 5 ms delay).

| Option | Chosen | Why |
| --- | --- | --- |
| AAC in MP4 | yes | decodes in every browser the game targets, with its priming declared |
| Opus | no | Safari's `decodeAudioData` support is too recent to rely on |
| Apple's AAC encoder (`aac_at`) | no | macOS only; the CI lanes that build the site are Linux, and one encoder writes one set of files |
| 32 kHz where a model has nothing above 15 kHz, 48 kHz for `DEAL` and `LIFT` | yes | as for the WAVs: the thumb slide has 38% of its energy above 16 kHz |
| One packed file for all sounds | no | it defeats loading by priority, and a pack needs an index |

**ffmpeg is a build dependency** of `npm run sounds` (Homebrew on macOS, the distribution's package on Linux): the CI lanes that build the site and run the full e2e suite install it.
The take table needs none: `c/Makefile` runs `soundgen ctable` for web.wasm and the iOS kernel whenever soundgen or a recording changed, so any lane that builds the wasm has it.

## How to hear each cue

**In the browser: `/sounds`.**
Run `npm run dev` and open `http://localhost:3000/sounds`.
Every cue has a Play button and an x4 button (four strikes at the kernel's stagger), with its takes, their anchors and its gain beside it.
A cue's moment is set far enough off that every take is heard whole, the part before its moment included.
The sweep has a button per stroke count instead (the lift alone, then 1-5 strokes).
Under them, "In context" asks the kernel for the plan of a small move and plays its cues at the plan's own times, exactly as the board does: an attack, a three-card attack, a transfer, an attack and its cover, a bout ending, a pickup, a refused attack, a last card, the opening deal.
Like the board it starts the plan now, so the grab and the slide before the first lift are cut to what is left of them (see "When there is less time than the gesture").
It plays through the same engine and kernel the board uses, so what you hear there is what ships.

**In a terminal: `make -C tools/soundgen audition`.**
Plays every file in turn with `afplay`, printing each name.

**Measured: `make -C tools/soundgen stats`.**
Prints each file's rate, length, bytes, anchor, peak and RMS.

## How to tune

Every part of every card sound (a grab, a thumb slide, a landing, the strokes) has a line in `tools/soundgen/tuning.tsv`: where it sits from its cue's moment, how loud it is, and how far each may wander a take at a time.
Every cue has a `mix` line there too: its whole level, for every host.
That file is the one owner of these numbers; soundgen reads it at build time and nothing else holds a copy.

**By ear, in the browser.**
Run `npm run dev` and `npm run tune` side by side, and open `http://localhost:3000/sounds`.
`npm run tune` starts `tools/soundgen/tune.c`, a small endpoint on `127.0.0.1:3101` that answers only pages on this machine and is never part of a build.
The "Tune" table at the top of `/sounds` has one row a part ("deal: thumb slide", "pickup: slide into hand", "trump: deck set down on it"), grouped under each cue's whole-cue row.
The level is in dB on the deal's scale, before the common gain that lifts every card take so the loudest peaks at -3 dBFS: raising the loudest part therefore lowers everything else, and what a level says is its balance against the rest.
The time is in ms from the cue's moment, negative before it; for strokes it is where the last one ends before the lift.
A grey "to" beside a value is the far end of its random spread, which the tuner shows and leaves alone.
The whole-cue row sets the cue's level for every host (its `mix` gain), and its "all parts" buttons move every part of the cue 10 ms against the animation.
Every change writes `tuning.tsv`, rebuilds the takes it touched, the take table, `web.wasm` and `promo.wasm` (`tools/soundgen/tune.sh`, 1.5 to 4 s), and reloads the page, which checks its kernel carries the new table before it says "Rebuilt".
Then press Play: every Play stops whatever was sounding first, so one sound is heard at a time.
A part's row plays that part alone: soundgen renders the take again with every other part silent, at the whole set's common gain (`soundgen solo`, served by the endpoint as `GET /solo/<cue>/<role>/<part>/<take>/<strokes>`), and it plays at the cue's mix gain, so it is as loud as in the game.
Each press plays the set's next take, and a strokes row plays the number of strokes chosen beside it.
The whole-cue row's Play plays the cue, every part together.
On the replay screen (development only) the Oracle's button opens the same tuner as a panel, in tabs by what happens at the table, with Again to hear the move on the board once more.
The "In context" moves below play the new takes at the plan's own times.
A value that differs from git HEAD is marked with a dot and gets a Reset; "Reset all" puts the whole file back.
When it sounds right, commit `tools/soundgen/tuning.tsv`: that is the shipped value, for the web and iOS alike.

**In a real game.**
`http://localhost:3000/ws` is a game against a bot, played entirely in the tab (a promo code's local game, no server or sign-in), with the sounds on after the first tap; reload it after a change in the tuner to hear the new takes.
The tutorial (`/tutorial`) plays the same engine too.

**By hand.**
Edit `tuning.tsv` in an editor (tab-separated; its header says what each column is) and rebuild (`npm run dev` does it).
A line soundgen cannot read, a part with no line or two, or a cue with no `mix` line stops the build: `e2e/audio_cues.test.ts` holds it, and that moving a part moves its takes and nothing else.

**Rendering.**
soundgen renders the takes in one forked worker per core (`SOUNDGEN_JOBS` sets how many; `1` renders in its own process), about 2.3 s for all 139 where one core takes 11 s, and the files are byte for byte the same whatever the number.
The tuner's rebuild also keeps each take as rendered (`SOUNDGEN_CACHE`, set by `tune.sh` only) under a key of its own `tuning.tsv` lines, so a change re-renders only the takes of the part's cue and role, rewrites only the WAVs whose bytes changed and re-encodes only those: about 1.5 s for one part, the relink of both kernel links included.

## How to replace a sound with a recording

A recording enters where a model does: as a take soundgen lists, so it is in the one table both hosts play by and among the files they serve and bundle, and nothing in either host changes.

1. Put it in `tools/soundgen/recordings/` as a 16-bit mono PCM WAV at its own rate (8-48 kHz), named for its cue: `place_rec_0.wav`.
2. Add a line for it to `tools/soundgen/recordings/recordings.tsv`: the cue as `RECIPES` names it, the role (`main`, `tight`, or `prelude` with its stroke count as the level, `0` otherwise), the file, its anchor (the ms into the file of the cue's moment, 0 when the file starts on it) and its AAC bitrate.
   `place main 0 place_rec_0.wav 12.5 96`
   Several lines for one cue are takes of one gesture, played in turn and never the same one twice running.
3. Rebuild (`npm run dev` does it): soundgen writes and encodes it with the rest, and the wasm is relinked with the new table.
   A cue with a recording plays its recordings only: its synthesised takes are no longer written.
4. Set the cue's level by ear with the tuner on `/sounds` (its whole-cue row), which edits its `mix` line in `tools/soundgen/tuning.tsv`.

A refused move needs nothing of its own: it is the same take, reversed by the engine, with its anchor measured from the other end.
A line soundgen cannot read, or a file that is not such a WAV, stops the build rather than leaving a cue silent.
`e2e/audio_cues.test.ts` holds it (a recording listed is its cue's one take, at its rate, length and anchor), and fails if the kernel's table and the files soundgen writes ever differ.

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
| strokes (SHFF = 1, the pass's timbre) | `discard/an/disc_syn.py` scrape | the preludes of `SWEEP` and `PICKUP` |
| discard lift (two knocks) | `disc_syn.py` lift | `SWEEP`, 100-180 ms after the last stroke |
| placement on the pile | `disc_syn.py` place | `PILE` |
| pickup lift (one soft knock) | `disc_syn.py` plift | `PICKUP` |
| slide into the hand | `disc_syn.py` insert | `INTO_HAND` |

**A file is a gesture at the game's timing.**
`soundgen.c` lays the parts of each cue's gesture out as its `*_game.py` does, against the cue's moment, at the times and levels `tuning.tsv` gives, and writes the take with its anchor.
The levels are the games' own, on the deal's scale (g = -34 to -32 dBFS a take): the grab at g-5 and the slide at g-6 to g-9, a dealt landing at g-13 to g-16, the flip at g-5, the trump's landing at g-9 to g-12 and the deck at g-3 to g-6, the throw-in's grab at g-14 and its landing at g-3 to g-5, the cover's at g-4 to g-6, the shfff at g-11 to g-13, and the discard parts by 50 ms RMS against the strokes at g-30.5 (lift +5, placement +3, pickup lift -10, slide into the hand -11).
**The balance between the parts** (folded into each part's level in `tuning.tsv`, set 2026-11-07 from a levels study, Alex: "do it yourself").
Within one video a card's landing is about 25 dB over the grab and slide before it, where the games had it about 4 dB over; and the discard and pickup videos were shot about twice as far from the phone as the pass's, so their "6.5 dB under a pass" was distance.
So on top of each game's level: the throw-in landing +18 dB (also the trump's and the pass's landing, the same model), the cover landing +19, the trump flip +17, the deck set down +14, the discard's lift +18, the strokes +7, the placement on the pile +8, the pickup's lift +8 and the slide into the hand +8; the deal's slide, grab and landing (the landing Alex set by ear), the two-finger grab, the throw-in's slide and the pass's shfff unchanged.
Then one common gain scales every card take so the loudest peaks at -3 dBFS at full master volume (+14.3 dB from the deal's scale), checked against the mixes a plan really strikes together (six landings a stagger apart, six covers, a refill's landings, the trump turned and landed, a transfer's landing and slide, a run-opening pickup's strokes under its lift and over the last landing, a sweep's strokes over the last cover): the loudest of them peaks at -3.0 dBFS too, so nothing clips and the gain was not lowered (soundgen lowers it, never a part, if a mix would pass -1 dBFS).
The gain in each model's `mix` line is 1000: the levels are the files'.

| Cue | Peak (dBFS) | Loudest 50 ms RMS (dBFS) |
| --- | --- | --- |
| `PLACE` (throw-in landing) | -6.7 to -3.0 | -24.4 to -17.7 |
| `COVER` | -6.0 to -3.6 | -27.9 to -23.3 |
| `TURN` (the trump flip) | -8.3 to -6.0 | -24.4 to -17.1 |
| `FLIP` (trump landing and deck) | -10.9 to -8.2 | -27.0 to -18.7 |
| `SWEEP` (the discard's lift) | -10.9 to -5.9 | -27.1 to -25.2 |
| `PILE` | -23.9 to -18.0 | -38.8 to -37.2 |
| `DEAL` (slide and grab), tight | -24.6 to -22.2 | -41.2 to -36.8 |
| `LIFT` (two-finger grab and slide), tight | -28.0 to -24.8 | -41.2 to -37.0 |
| sweep strokes (1-5) | -27.3 to -21.5 | -44.9 to -39.8 |
| pickup strokes (1-5) | -32.3 to -23.9 | -45.3 to -40.1 |
| `PASS` (shfff) | -31.4 to -29.0 | -44.2 to -39.6 |
| `DEAL_LAND` (own seat) | -34.0 to -30.9 | -49.5 to -47.6 |
| `PICKUP` (its lift) | -38.7 to -33.7 | -52.1 to -50.3 |
| `INTO_HAND` | -39.4 to -32.5 | -52.8 to -51.4 |
| the five placeholders (before their gains of 0.044-0.1, which bring them to -38 to -26 by 50 ms RMS) | -1.0 | -11.4 to -5.6 |


**The strokes are heard in full.**
Before a sweep or a pickup come 1-5 strokes (one per 2.5 cards), each 0.8 times a recorded stroke's length and 60-200 ms apart as `disc_game.py` draws them, a little louder as the pile grows, written as preludes of their own and played before the lift over whatever else is sounding.

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

Everything soundgen writes is a build artifact (`npm run sounds`, hooked onto `predev`, `prebuild` and the full e2e's pre-hook, and `soundgen ctable`, run by `c/Makefile` for the wasm and iOS), written to `public/audio/gen/` and `c/build/gen/` and never committed.
Each takes about 12 s of one core, and runs only when soundgen changed.

## Cost

| What | Size | When it is paid |
| --- | --- | --- |
| Engine and scheduler | 5.1 KB minified, 2.2 KB gzipped (esbuild), and 1.6 KB, 0.6 KB gzipped, of bridge in `sdk/ts/wasm/bots.ts` | with the game routes' JavaScript |
| Sound button, finish hook and priority hook | about 1.2 KB gzipped | the same |
| `web.wasm.gz` | +858 B (36,974 to 37,832); then +2,864 B (49,898 to 52,762) for the voice and the take table | with the kernel |
| `bots.wasm.gz` | +735 B (87,992 to 88,727) | server side |
| The sound files | 1.8 MB of AAC, 139 requests (was 122 KB, 16) | wanted ones first; all of them after the first tap; none when muted |
| `/sounds` | about 2.5 KB gzipped | that route only |
| The iMessage app's sounds | 2.0 MB on disk in `FoolishKit.framework/sounds/` (the same 1.8 MB of AAC) | with every install |
| The iMessage app's decoded samples | 2.6 MB for a deal, 5.3 to 7.2 MB mid-game, 9.3 MB at most | while a board is up |

The wasm numbers are measured against a build of main as of the rating stamp (PR #284).
The modelled cues (the four new ids, the transfer flag and `anim_seat_gain_pm`) took `web.wasm.gz` from 49,544 to 49,924 B (+380 B, measured against this branch before them, 2026-11-07).
The JavaScript numbers are an estimate from bundling the modules alone, not a diff of two `next build` outputs.
Moving the rules and the take table into the kernel (2026-11-07) took the engine's JavaScript from 15,669 to 5,107 B minified (3,841 to 2,218 gzipped) and added 1,619 B (613 gzipped) of bridge; `web.wasm.gz` grew 2,864 B, of which the table is about 1.5 KB (139 rows of 48 B, 6,672 B raw), and `promo.wasm.gz` 2,908 B.
Neither wasm module grew a page of linear memory (`e2e/mem/wasm_memory.test.ts`).

## iOS and iMessage

The iMessage board plays the same cues as the web, from the same C: which cue and when is the plan's (`AnimPlan.cues` in `sdk/swift/AnimPlanWire.swift`), and which take, when its file starts, how loud and how fast is `fio_cue_strike` (see "The rules are C, for both hosts").
Nothing in Swift restates a take, an anchor, a level or a rule.

**The files.**
`ios/scripts/bundle_sounds.sh` is a build phase of FoolishKit: it runs `make -C tools/soundgen bundle`, which renders and encodes only when soundgen changed since the last time (about 1 s when nothing did), and copies the 139 `.m4a` files soundgen's manifest names into `FoolishKit.framework/sounds/`.
A file the table names and the build lacks fails the build.
The take table iOS plays them by is `c/build/gen/cue_takes.c`, which `c/Makefile` writes with `soundgen ctable` from the same sources, so the two cannot disagree.
They add 2.0 MB on disk to the app (1,783,626 bytes of AAC, which a download does not compress further); Alex accepted the size.

**The engine** (`ios/FoolishKit/Sound/CuePlayer.swift`).
One `AVAudioEngine` with one `AVAudioSourceNode` at the output's own rate and width, whose callback sums the scheduled takes through the kernel's output mix (`c/src/cue_mix.h`, `fio_mix_*`): a take is handed over with the output frame it starts on and its level, and starts on that frame, whole, overlapping whatever else sounds.
The same mono sum goes to every channel, so a take is as loud on each side as Web Audio's up-mix plays it.
"Now" is the frame heard at this host time, read off the last rendered block's timestamp; a take whose start was already rendered starts in the next block, whole.
It was first written on `AVAudioPlayerNode`s, scheduled at sample times; rendered offline, a take came out 25 samples late and 3 dB down through one mixer, and 60 ms late through one more node, which is why the mix is the kernel's.
Each file is converted once, as it is decoded, to the output's rate (`AVAudioConverter`, mastering quality, priming `.normal`): a 32 kHz cover in a 48 kHz mix lands its loudest sample on the frame it was aimed at.
A revert plays its take reversed; a stand-in's wander in speed is the kernel's resampler (`fio_cue_resample`).
The session is `.ambient`: the ringer switch silences the game, and it mixes with the player's music.

**What is decoded.**
Only the cues `fio_cues_next` names for the board on screen are decoded ahead and kept, in the table's order; what the next answer no longer names is let go.
A struck take that is not decoded is decoded off the main thread, ahead of the loader's queue, and then follows the web's rule: started if no more than 80 ms behind its aim, dropped otherwise.
The strokes before a sweep or a pickup (30 files, 5.7 MB as samples, the largest part) are never kept: decoded for their strike and let go once played.
Everything is dropped when the board goes away, when the extension resigns, and on a memory warning (`purge`, which keeps the engine).

**Measured** (iPhone 17e simulator, iOS 27, 48 kHz output).

| | |
| --- | --- |
| The real extension in Messages, a lobby's deal | footprint 37.3 MB as the engine came up, 43.4 MB with the deal's 31 takes decoded (2.6 MB of samples); peak 49.9 MB over the deal |
| The harness, a 3-seat game, the decoded set mid-game | 69 to 93 takes, 5.3 to 7.2 MB of samples |
| Every cue's own takes at once (the most `want` can keep), FoolishTests | 109 takes, 9.3 MB of samples, footprint +8.5 MB |

**Placing a beat's cues** (`ios/FoolishKit/Sound/CueSequence.swift`).
The board walks beats and sleeps between them (`MessageTableView+Sequence.swift`), so the clock is re-read at every beat: `playStep` says when a beat's flight opens, and the beat's own cues are placed from that moment, which puts them on the picture to well under a millisecond (the trace's "late" for an own-beat cue is 0.2 to 0.8 ms).
A cue whose take starts before its beat opens (the grab and the thumb slide before the next dealt card, the strokes under the last cover) is placed from the beat before, against where that beat says the next will open.
The walk is not the plan: measured, a beat that flies nothing (the magic transition before a deal) takes no time where the plan gives it 500 ms, and a flown beat runs about 30 ms over its plan.
So the forecast is the walk's own: the next beat opens at once after a beat that flies nothing, and otherwise a plan's length later plus the overrun the walk has shown in this stream.
A take placed ahead whose beat opens elsewhere before it has sounded is taken back and placed again (`cue_mix_drop`).
Over a 3-seat harness game, 36 forecasts landed within -32 to +37 ms of the beat's real opening (median +4, 28 of 36 within 15 ms), and in the real extension's deal within -36 to +33 ms; that is the error of a take placed ahead that was already sounding when its beat opened.
A move that opens its run hears its lift late by the tight take's swell (158 to 267 ms), as on the web.

**The role and the end.**
`syncRoles` (`MessageTableView+Roles.swift`) asks `fio_role_cue` where the marks turn, and the results screen's `settleResults` asks `fio_finish_cue` with `fio_finish_rows`' arguments; a game opened already finished makes no sound.

**The setting.**
Settings has a Sound row, On or Off (`FPrefs.soundOn`, default on, three keys in all 25 languages); Off is the web's mute, nothing loaded or played.
There is no volume: the level is the web's 0.7, and the phone's buttons do the rest.
The feature is behind `CuePlayer.onByDefault` (shipping on); in a debug build `sound=0` in `dev.flags` turns it off, and `sound.silent=1` (or `HARNESS_SOUND_SILENT` in the harness) keeps it running and scheduling at zero output, which is how a simulator run measures it.

**Tests.**
`ios/FoolishTests/CuePlayerTests.swift` renders the engine offline and holds the output to the take the kernel chose, to the sample: a take starting its anchor before its moment, a tight take from now, a revert, a dealt card at its seat's level, a sweep's strokes, two overlapping strikes, a stereo mix, a 32 kHz take in a 48 kHz mix, mute, detach, what stays decoded, the 80 ms rule and the memory; every one was mutation-checked.
`c/tests/anim_plan_test.c` holds the mix (`test_cue_mix`), and `c/ios/ios_api_smoke.c` the exports.

**Not yet.**
A revert is silent on iOS: an iMessage stream never carries a refused move, and the undo and conflict flights are not planned by the kernel, so no cue says what they undo.
A transfer's `PASS` is in the plan but no harness game played one.

## Open questions for Alex

1. **On by default?**
   Sound is on until muted, at 70% master volume.
   Many web games start muted; say if you want that.
2. **A transfer is its own sound now** (`PASS`, beside its `PLACE`): the plan reads the event's `msg` (`EVW_MSG_PASSED`) through `ClientPlanStep.msg` and the iOS plan wire, and my own transfer is heard as one before the server confirms it.
   An attack and a throw-in are still one sound, which is what Alex modelled.
   A refused transfer reverts as `PLACE` backwards, without its slide.
3. **A move that opens its run hears its grab and slide late.**
   Played whole (Alex's call), a tight take 180-290 ms behind the card leaving, where the slide's swell before its peak is most of it; a shorter swell would be a change to the model.
   A run-opening pickup's lift lands 60-80 ms late the same way, under its strokes.
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
12. **The placeholders were turned down to sit with the card set** (Alex: "turn them down").
    Their gains in soundgen's `tuning.tsv` mix lines put them, by loudest 50 ms RMS, at -26 for the game over (once a game, just under a card landing at about -22), -30 for a seat going out, -34 for a good and -38 for the hand-off (heard every bout).
    They still want designing; these levels are a starting point for tuning by ear on `/sounds`.
13. **1.8 MB of AAC** (1,783,626 bytes, from 6.5 MB of WAV), loaded after the first tap, wanted cues first.
14. **The strokes play over the beat before** (Alex: "just play the pickup over the other sounds"): five strokes start 3.5 s before their sweep, over the last cover and its hold.
