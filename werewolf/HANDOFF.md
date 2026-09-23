# HANDOFF - Werewolf, paused 2026-09-20

Werewolf is **on ice**, and not because anything went wrong with it.

It is paused for a reason that has nothing to do with the code: **there is no
iMessage group chat with five or more people to test it in.**
Werewolf needs a crowd before it needs another commit.
Foolish is 2 to 8 and most of its real games are two-handed, so it could be
built and shipped from one device with a simulator; werewolf cannot.
That is the whole blocker, and no amount of kernel work moves it.

The design in `docs/UI.html` is solid and the kernel under it is green.
Pick this up when there is a thread to play it in.

## Where it actually stands

- **The kernel is done enough to play.** 2,390 assertions plus 115 on the
  Swift-visible bridge, green under ASan and UBSan, and a CI lane on every push
  that also proves `shared/` compiles for a second product under `-Werror`.
- **Roles, the night, masking, the envelope, seats and the lobby all exist** -
  `ww_game`, `ww_view`, `ww_wire`, `ww_seat`, `ww_lobby`. `ww_view` is the role
  hiding, and it is the file to be careful in.
- **The surface is designed and not built.** `docs/UI.html` (512KB, ~40 commits)
  is a real study, drawn at device size: the bubble timeline, the shield roster
  at 32 seats in twelve scripts, day and night palettes, the wolf's flow, the
  villager's flow, and both endings. It was reviewed screen by screen with the
  owner. **Do not redesign it from scratch; it is further along than the code.**
- `docs/check_ui_doc.py` is a structural checker for that file. Run it after any
  edit. It exists because the same scoped-CSS bug shipped three times.
- **`docs/THREE_SIZES.html` redraws the bubble, the collapsed drawer and the
  expanded screen as a shadow theatre** - cut paper, one lamp, and the shadows
  it throws; no grain, no stone. One stylesheet, one 32-player game seen from
  one phone, every element tagged with the edge it holds during a collapse, and
  a filled-in channel grid. `docs/THREE_SIZES_STONE.html` is the same page in
  the stone-and-tallow materials of `UI.html`, kept as the backup. Read both
  after `UI.html`; they are where the three sizes are current.

## What the design settled, that the code does not know yet

These are decisions, not ideas. They came out of the UI study and the kernel
has not caught up.

- **32 players, not 10.** `README.md` still says 5 to 10. The cap is the
  iMessage group limit, and the roster, the shields and the bubble were all
  drawn and measured at 32.
- **A wolf opens into the pack chat**, not the table. The kill is a door at the
  bottom of the conversation; the table is a sheet over it. Tap a shield and
  the sheet drops.
- **Ten seconds of silence stages for you** - `[WOLF] suggests [VICTIM]`. The
  send floor and the message are one mechanism.
- **The day vote is a per-player record.** `ww_day_lynch(g, target)` models the
  RESULT of a vote, not the vote. There is no per-player vote to fold into a
  lynch, and the day screens are built entirely around public tallies. **This is
  the one real kernel gap and it is the first thing to write.**
- **Dead players cannot stage anything.** The screen says so by having nothing
  to press - there is no line explaining it.
- **The night screen shows no progress count.** At night, a count of who has yet
  to move is a count of living wolves.
- Transcripts older than the last day and night get dropped to fit the ~1,000
  character body budget; a 32-player night is 1,323 base32 characters otherwise.

## Briefed and never started

- **The DEBUG seat picker** (`SeatPicker(nPlayers:joins:)` plus `addSoloSeat`),
  ported from foolish behind `MessageDevBoard.flag(key:shipping:)`. Werewolf
  kept `HarnessUI` instead, which is backwards - the fake transcript is the
  weaker of the two tools. `ios/scripts/release_gate.sh` already refuses to let
  a seat picker ship, because choosing a seat here is choosing to be the wolf.
- **Lobby v3's cheating hole matters most in this game.** Roles come from the
  seed, so a creator who can re-create is a creator who can decide whether they
  are the wolf. `ww_lobby` exists; the iOS side of it does not.
- **A motion vocabulary.** Nothing in `docs/UI.html` says how any of it moves.
  Read `docs/MOTION_BEFORE_FLOW.md` in the parent repo BEFORE drawing another
  screen - it exists because foolish found this out at build 24.
- werewolf's `e2e/` is orphaned; it runs in no lane. See issue #207.

## The thing worth carrying to the next game

All of `docs/UI.html` cost nothing to iterate on because it is one HTML file
opened in a browser. Foolish reached build 74 discovering the same class of
thing through Xcode, a version bump, an upload, a TestFlight download and a
play-through. Roughly one round trip against twenty seconds.
See `docs/MOTION_BEFORE_FLOW.md` in the parent repo.
