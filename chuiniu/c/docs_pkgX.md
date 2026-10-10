# Package X report: the throw plays once a phone

The owner's bug: the dice were thrown again mid-round.
The table threw whenever `TableModel.rollID != host.playedRoll`, and `playedRoll` lived in the extension's memory, so every fresh launch of the extension (the drawer opened again) threw the running round again.

## The rule (proposed decision, for DECISIONS.md)

Each phone sees each round's throw at most once, ever: when the game starts and when a new round starts, the first time this phone shows that round's table.
It is never thrown again for that round, however many times the drawer is closed and reopened, the extension is killed, a bid arrives, the table collapses or expands, or the bubble is drawn.

- The kernel decides it: `cn_api_stage_begin` takes no `roll` any more, and a table throws while `cn_api_roll_pending()` says the round is pending on this phone.
- The host reports the end: `cn_api_roll_seen(round)` when the director's throw ran to its end (`StageDirector.onRollDone`), with the round the table was begun for (`TableModel.rollID`, which the bridge maps back to `CnView.round`), so a report that lands after the resident moved on cannot mark the new round.
- A throw cut off before its end (the drawer closed mid-throw, the extension killed) was not watched: it plays again once, from its start (from the plan's SHAKE beat, as any first look).
- Reduce Motion is the end at once: the director starts the clock at the throw's total and reports it from the begin, so the round is watched without a frame of motion. The host's own second report for Reduce Motion in `TableScreen` was a double Band-Aid over that and is deleted.
- A reveal throws nothing and its dice are untouched (I22: the reveal shows the called round where its throw left them). After a call the next round is pending, and its table throws the first time this phone shows it (the looked-ahead table on a reveal, or the opener's bid).
- A finished game, a lobby and a phone with no seat have nothing pending.
- A phone that first looks in a later round (a late arrival, or a store lost) throws the running round once; earlier rounds are never thrown.
- Two people on one simulator (`dev.seat`) each keep their own, because each person has their own records.

## The record (I14)

The seat record of a game grows one byte: the newest round whose throw this phone watched to its end, stored as round + 1 (0 for none), raised only (never lowered).
`cn_rec_put` keeps the byte when the game is recorded again (a rejoin under a new tag is the same phone that watched).

The stored form now starts with an 8-byte mark, `"cnrec\x02\x00\xa5"`, followed by 18-byte records (8 id, 9 tag, 1 seen), the newest 256 games: `CN_API_REC_BYTES` is 4616 (was 4352).
The first form had no version, so the mark is the version: a store without it is read as the first form (17-byte records), every seat kept and every round unwatched, and is written back in the new form at the next flush.
A first-form store is mistaken for the new one only if its newest game's id (a SHA-256 prefix) equals the mark: chance 2^-64.
Either form's trailing partial record is dropped, as before.
The defaults key stays `chuiniu.seats.v1` (the bytes carry their own version; the host hands them over unread).
`cn_rec_load` and `cn_rec_save` in `cn_msg.c` own the form; `cn_api_seats_load` and `cn_api_seats_save` call them.

`cn_api_roll_seen` dirties the records, and `BridgeKernel.rollSeen` flushes at once (I14: flushed after every call that can dirty them), so a launch that ends right after the throw still remembers.

## Swift

- `TableModel.rollPending` (the kernel's `cn_api_roll_pending`), `Kernel.rollSeen(rollID:)`, `ChuiniuHost.rollSeen(_:)` (reports, then refreshes the model).
- `ChuiniuHost.playedRoll` is deleted. `TableScreen` asks with `roll: t.rollPending` and reports through `host.rollSeen`.
- `TableStage.begin` takes no `roll`; `StageView.swift` changed in one line (the call) and one doc comment (`StageRequest.roll` is now the model's `rollPending`, kept only as when to ask again and for the director's same-throw clock).
- Once the round is watched the model's `rollPending` turns false, the request changes, and the table is begun again still. The still table's picture is the throw's last frame byte for byte (checked in the C smoke), so the re-begin cannot flicker.

## Tests, each seen red

C (`make -C chuiniu/c run asan`):
- `cn_msg_test` `test_records`: the seen byte (new record 0, raised, never lowered, the same again no change, per game, no record nothing kept), kept by a re-record, the stored form's round trip, the mark, a short buffer, a cut store, an empty store, a full first-form store of 256 games loading every seat with nothing watched, a first-form partial record.
- `cn_twophone_test` `throw_once`: a group of three through the bridge with `relaunch()` (the bridge's static state zeroed, only the phone's stored records kept): pending at the start, a cut throw pending again, a round not dealt refused, watched (and dirty), kept across a relaunch and an adopt, each phone its own, a bid in a new launch throws nothing, a call makes the next round pending, a relaunch on the reveal keeps it, a late arrival in round 1 pending exactly once, the first record form loaded with the seat by record and nothing watched, written back in the new form; and a finished game has nothing pending.
- `ios_smoke`: a watched round's table begins still (`rolls` 0), and its frame is the throw's last frame byte for byte.

Mutations (each restored by re-editing, the file compared with a copy):
- `cn_rec_put` drops the seen byte: `cn_msg_test` "recorded again (a new seat): the seen round stays", "loaded: the seats and the seen round".
- The first form not converted: `cn_msg_test` "a full first-form store loads every game" (and two more), `cn_twophone_test` "the first form: Alex's seat by its record".
- `roll_pending` always 1: 11 `cn_twophone_test` assertions ("and the round is no longer pending", "a new launch of the extension: still watched", ...).
- `roll_pending` only when nothing was ever seen: `cn_twophone_test` "round 1's throw is pending on Alex's phone".
- `cn_api_roll_seen` leaves the records clean: "watched to its end: the records are dirty".
- `cn_rec_see` may lower: "never lowered", "watched again changes nothing", "once: a relaunch throws nothing, and round 0 is past".
- `cn_api_roll_seen` accepts a round not dealt: "a round not dealt yet cannot be watched".
- `cn_rec_save` without the mark: "saved behind the mark", "a round trip", and the relaunch assertions.
- `roll_pending` without the finished-game rule: "and no throw to play".
- `cn_api_stage_begin` always throws: `ios_smoke` "and its table begins still".
- The still table seeded from the next round: `ios_smoke` "the still table is the throw's last frame, byte for byte".

Swift (`chuiniu/ios/scripts/mac_tests.sh unit`):
- `BridgeKernelTests.testARoundsThrowPlaysOncePerPhoneAcrossLaunches`: a new `BridgeKernel` on the phone's store is a launch; the same steps as the C test through the seam, the stage's `rolls` read from `begin`, `ChuiniuHost.rollSeen` read back through the model, and the first store form written by hand.
- `StageViewTests.testUnderReduceMotionTheThrowIsReportedWatchedAtTheBegin`.
- The still-table tests (`testAViewWithNoSize...`, `testMyCupsEllipse...`, `testAFrameAskedBeforeABeginIsDropped`) now watch the round first, since the kernel, not the test, decides the throw.

Swift mutations are in `chuiniu/ios/TESTS_MUTATED.md` ("The throw once a phone"): `rollSeen` without its flush, `rollSeen` passing the wrong round, the model's `rollPending` always true, and the director's Reduce Motion jump removed, each red on its named assertions.

Not tested by a unit test: `TableScreen` wiring `onRollDone` to `host.rollSeen` (a view); the coordinator's simulator pass should close and reopen the drawer mid-round and see no throw.
A note on `make`: restoring a mutated file within the same second as its build leaves the old binary (the smoke failed once that way); `touch` the file and rebuild.
