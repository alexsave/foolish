# The night kernel

How the night phase works, what it hides, and the evidence that it hides it.

Werewolf lives entirely inside an iMessage thread, 5 to 10 players, with no server.
The category is open because of one technical barrier, and the barrier is not the rules.

## The problem

**In a message thread, the act of sending is visible.**

Every previous attempt at a social-deduction game in Messages died on that sentence.
A wolf who sends at night has announced himself, because nobody else had a reason to send.
The night is where the game is, and the night is exactly where a message thread leaks.

## The answer: make the act universal

Six decisions, and they only work together.

1. **Everyone must send.**
   The night does not end until every living player has sent one record.
   A wolf is not behaving oddly by sending, because sending is the only thing anyone does.

2. **Everyone chooses.**
   Every player picks another player on the same screen with the same taps.
   A wolf's pick is a kill vote, the seer's is a question, a villager's is who they dreamt about and does nothing.

3. **A ten-second floor.**
   Send is disabled for ten seconds so nobody can answer instantly.
   That removes the only real latency tell left: an instant answer means you had no decision to make, and the only players with no decision to make are the villagers.
   Ten is a guess, and it wants a real table to settle it.

4. **One wolf line each.**
   The wolves get a channel only they can read, one message per wolf per night.
   The last wolf in the rotation makes the kill call, and the rotation advances each night, so the same voice is never the one deciding.
   Last rather than first, because the lines ride in the records and the records arrive in rotation order - the decider is the wolf who has read everyone.

5. **The wolf line rides INSIDE the night record.**
   Never as a second message.
   A separate message is a countable bubble, and three extra bubbles on a five-player night is three wolves.
   **One bubble per player per night, always.**

6. **The skip.**
   No timer can exist, because nobody has to be awake.
   A player who opens after a minute has passed may act, and their message carries the auto-passes for everyone late, forward into the chain.

## What hiding means here, precisely

The hiding is **social, not cryptographic** - the same trust level as passing the phone around a table.
Every device receives the same envelope bytes; what is defended is the *reading*, because that is what a player actually does.

So the contract is not about the wire.
It is about what `ww_view_put` will hand a given seat, and it has four parts, written out in `c/src/ww_view.h`:

1. A third seat's view of a night record is **the same bytes** whatever the sender's role is.
   Not "the role field is blank" - byte-identical.
2. The wolf line does not appear in a non-wolf's view **at all** - not truncated, not zeroed to its length, absent, so there is no length to read either.
3. The seer's readings are the seer's, not even as a count.
4. A dead seat's role is public. That is the genre's own rule and the day has nothing to argue about without it.

**The payload is not padded to a constant size.**
It was considered - a longer bubble on a night when a wolf spoke is in principle countable - and rejected.
Anybody willing to measure bytes has the whole envelope in front of them and could read the roles out of it instead of counting.
Padding would cost every real player URL budget to defend against an attacker who does not exist.

## The race, and why it was already solved

Two players can act for the same seat's turn: one really, one carried as an auto-pass by a later player.

Rule P (`c/src/ww_wire.h`, inherited intact from the fork's `msg_wire.h` §7.2) is a total preference order every device computes identically, and it deliberately does **not** trust delivery order.

```
ancestry first: a chain's own DIRECT CHILD beats the parent it names
  0. a STARTED chain (phase >= NIGHT) beats a lobby
  1. higher round wins            (a resolved night is settled history)
  2. else higher turn wins        (MORE ACCEPTED RECORDS)
  3. else more joins wins
  4. else lexicographically smaller SHA-256(envelope bytes) wins
```

**Clause 2 is the one this product leans on.**
A player who carried a skip *plus* their own move has a longer chain, so they win, whichever bubble landed last.
Consequence: the night always moves forward and racing it cannot stall it.

`turn` is read off the header, before anything is replayed - that is what makes adoption cheap, and it is also why a bubble could simply *claim* turn 9000 and win every race in the thread forever.
The line that makes the claim worthless is in `ww_msg_replay`: a chain wins a race on its header, but it is only adopted after it replays, and it only replays if the header was telling the truth.

### When the skipped player was the deciding wolf

You cannot hold the night open for him.
A night waiting on exactly one player has named that player to everybody watching, and the player it names is a wolf.

So the kill falls to **the most recent wolf who did choose** - the living wolf whose record landed latest in the chain.
Deterministic, invisible from outside (the night ended the way every night ends, with one record per living seat), and it keeps the kill under wolf control rather than handing it to the dice or dropping it.

## What is in the kernel that a client might expect to own

Both night clocks are kernel rules, not view opinions:

- `WW_SEND_FLOOR_S` (10) and `ww_send_floor_remaining`
- `WW_CARRY_AFTER_S` (60) and `ww_may_carry`

Neither is an input to replay.
A kernel that refused a carry by the clock would make replay depend on when it ran, and two devices replaying the same chain at different times would disagree about the game.

## Evidence

`make -C c tests` - **2282 assertions, 0 failures.**
`make -C c tests-asan` - the same suite clean under ASan + UBSan, which is what proves the hostile-bytes decoder never reads past a truncated buffer.

### The mutation ledger

Every mutation below was applied **alone**, the suite rebuilt and run, and the source restored.
A mutation that does not compile proves nothing, so the four that first failed to compile were rewritten until they did.

**29 mutations, 29 caught, 0 survivors.**

| # | mutation | first assertion that died |
| --- | --- | --- |
| 1 | view: publish the wolf channel to everyone | a villager's view does not carry the line (11 assertions) |
| 2 | view: a third seat's row leaks the target | the rows say the same thing |
| 3 | view: every wolf's role is public | the third seat's view is byte-identical with the roles exchanged |
| 4 | view: the decider is published to everyone | a villager is not told who decides |
| 5 | view: a dead wolf keeps the channel | the dead wolf cannot read the channel |
| 6 | view: the seer's readings go to everyone | and is told nothing |
| 7 | rule P: clause 2 removed | the carried skip wins |
| 8 | rule P: clause 2 inverted | the carried skip wins |
| 9 | rule P: ancestry clause removed | it outranks a parent claiming anything |
| 10 | rule P: clause 0 removed | a dealt game outranks the invite |
| 11 | kill: no fallback when the decider was skipped | the kill fell to the most recent wolf who chose |
| 12 | kill: the fallback takes the FIRST wolf who chose | not to the first one who spoke |
| 13 | carry: reaches forward as well as back | one player cannot end the night alone (19 assertions) |
| 14 | chat: a non-wolf may carry a line | a villager cannot |
| 15 | decider: the FIRST wolf in the rotation decides | the decider is the rotation's last wolf |
| 16 | decider: the rotation does not advance | the rotation hands the call to a different wolf |
| 17 | winner: parity is not a wolf win | parity ends it |
| 18 | clock: the wrap is computed in int, not uint16 | across the wrap |
| 19 | deal: the shuffle is skipped | a different seed deals a different table |
| 20 | deal: the last seat is never shuffled | every seat can be dealt a wolf |
| 21 | night: a second record from one seat is accepted | one record per seat per night |
| 22 | decode: the body length is not proven exactly | a trailing byte is refused |
| 23 | decode: an auto-pass may name a target | a carried pass names nobody |
| 24 | decode: an all-zero seed is accepted | an all-zero seed is never a deal |
| 25 | replay: a claimed turn is not cross-checked | it cannot be replayed, so it is never adopted |
| 26 | replay: the header's phase and night are not cross-checked | a claimed ending is refused |
| 27 | replay: a record may claim a night it did not land in | refused, so the digest is not malleable |
| 28 | seal: past nights' lines are not pruned | last night's lines are gone |
| 29 | view: measure and put disagree | measure agrees with put (207 assertions) |

Two of these were **survivors on the first pass**, and both were real findings rather than missing tests:

- **#25** compared the rebuilt `turn` against `e->n_records`, which the decoder already guarantees, so the check could never fire.
  It now compares against `e->turn`, the header field an attacker would forge, and that is the check that makes a forged Rule P win worthless.
- **#27** had no test at all.
  A frame that lies about its night does not corrupt the state - the night comes from the game, not the frame - which is exactly why it has to be refused: two byte strings replaying to one game make the digest malleable, and the digest is Rule P's last tiebreak and every envelope's parent link.

### The three headline tests

In `c/tests/tests.c`:

- `test_a_wolfs_record_is_byte_identical_to_a_villagers`
  Builds the same deal twice with one wolf's role and one villager's role **exchanged**, plays an identical night into both, and asserts a third seat's view is byte-identical.
  It also asserts the wolf's *own* view differs between the two, so the test cannot pass by comparing a game with itself.
- `test_the_wolf_line_is_absent_from_a_non_wolfs_view`
  Scans the whole masked blob for the line's bytes - absent for a villager, the seer and a spectator, **present** for both wolves.
  It fails in both directions, so a masking bug that blanked the channel for everyone is caught too.
- `test_a_carried_skip_beats_the_skipped_players_late_move`
  Searches for a fork where the digest tiebreak would have picked the *shorter* chain, so the test proves clause 2 overrode clause 4 rather than a coin flip landing right.
  Then it replays the winner and confirms it carries exactly one skip.

## Size

A ten-player first night with three wolves and every line at the cap seals to **286 bytes, 464 base32 characters**, against the ~1000-character URL budget the fork measured (`test_a_full_night_seals_inside_the_url_budget` asserts both).

By night ten the record chain has grown and the total goes over.
The answer when that day comes is the one the fork already found for its own body - entropy-code the chain against the state's own menu of choices - and it is deliberately not built yet: it needs real games to calibrate, and the first night is what has to work first.

Past nights' wolf lines are pruned at seal.
No rule reads them, the prune is a deterministic function of the sealing game, and replay is unaffected - so carrying them would grow every bubble for the rest of the game to show a channel that closed.
It is also a privacy win: a wolf's phone stops carrying a transcript of the pack.
