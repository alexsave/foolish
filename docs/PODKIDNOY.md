# Podkidnoy: the game without the transfer

*The first rules VARIANT this engine has ever had. Until now there was one
Durak, played one way, and every layer could assume it. A table can now choose
between perevodnoy - the transfer game, the default, and what every game before
this played - and podkidnoy, the throw-in game where the defender covers or
picks up and there is no transfer at all.*

## The rule

The defender may not hand the attack on by laying a card of the same rank.
That move (`handle_pass`, `MOVE_PASS`, the perevod) does not exist at such a
table: the defender covers, or picks up.
Everything else is untouched - the same deck, the same deal, the same throw-in
limit, the same fool.

## Where the rule lives

In the kernel, once.
`Game.rules` is a bitmask of `GAME_RULE_*` and its **zero is the classic
game**, so every Game in this tree - hundreds of construction sites, all of
them born from a `memset` - is the game it always was without a single edit.
The one bit defined today is `GAME_RULE_NO_PASS`, and the one question anybody
asks is `game_pass_allowed(g)`.

Three places read it, and nothing else has to:

| where | what changes |
| --- | --- |
| `game.c handle_pass` | refuses with `ENGINE_REJECT_PASS_DISABLED`, before any card rule |
| `legal.c calc_pass_moves` | enumerates nothing, so the transfer is not in any menu |
| `cordite_sim.c` | the Monte-Carlo world's own movegen and rollout policy, likewise |

The gate sits inside `calc_pass_moves` rather than at its two call sites, so
the enumerator a bot searches with and the one a human's menu is built from
cannot come to disagree - a menu that offers what the validator refuses is how
a phantom "invalid move" reaches a board.

**The UI needed no gate at all.**
The Pass button, the drag-to-empty-table, and the drag hint are all the
kernel's own answer over its legal menu (`fio_play_probe`), so they disappear on
their own.
That is the whole payoff of keeping the rules in C: a variant is added in one
place and every surface follows.
The one thing the menu cannot say is which rules to TEACH, so the rulebook is
told explicitly (`RulesView(passing:)`) and a podkidnoy table's help page does
not mention passing anywhere - not a section on it, not the "(if allowed)"
aside the defending section used to carry.

## Why the codec had to know

A v6-family replay code does not store moves.
It stores, for each step, an INDEX into the legal-move menu of that state -
which is what makes a whole 8-player game fit in ~68 bytes.
The menu is therefore the codec's probability model, and two devices that
disagree about the rules do not merely play differently: they read the same
bytes as different moves.

So the mode is pinned by the code itself.
The inline-reveal line has carried a pass-mode bit since format 7 (1 =
perevodnoy, 0 = podkidnoy); it was written as 1 and ignored for two releases,
and it is live now - it gates the PASS block in `build_top_menu`, and the
decoder reads it before a single atom is decoded.

**Spliced, not appended.**
An earlier note in `replay.h` planned to move the PASS block to the END of the
menu first, so that every non-pass index would be identical across the two
modes.
That was reversed when the variant was built, and re-examined in 1.0(17) when
the owner asked whether the append was the more elegant answer after all.

It is not, and the reason is that **index identity is not an observable property
of this format**.
A code is a single mixed-radix rANS integer: each decision multiplies it by that
state's TOTAL menu weight `M` and adds a digit derived from the chosen option's
`cum` and `w` (`coder_code` / `coder_finish`).
`M` is a sum, so permuting a menu cannot change it, and the code's LENGTH is
`sum of log2(M/w)` - in which `cum`, the index, does not appear at all.
Two orderings of the same menu therefore produce a different integer of the same
size that decodes to the same game.

Worked on one decision, a defender picking up out of a menu of
`[cover 6, cover 3, pass 16, pickup 2, attack 2, good 1]`:

| | `cum` of pickup | code | bits |
| --- | --- | --- | --- |
| perevodnoy, spliced | 25 | 55 | 6 |
| perevodnoy, appended | 9 | 39 | 6 |
| podkidnoy (either) | 9 | 23 | 5 |

The append saves nothing (55 and 39 are the same length), changes nothing for
podkidnoy (23 either way - the saving comes from `M` dropping 30 to 14, i.e.
from the block being ABSENT, not from where it used to sit), and re-points every
perevodnoy code ever written (55 becomes 39 for the same game).
The fresh-pass option is offered whenever the defender holds an unknown card of
a matching rank, so the block is non-empty at very nearly every first defender
decision of every bout - which is how many states shift.
That is a second format renumber within a week (the deal-order fix already spent
that break, `docs/DEAL_ORDER.md`) or, far worse, old codes decoding silently as
different moves.

It does not buy a better failure mode either, which was the last argument for
it.
Feed a podkidnoy code to a decoder that wrongly built the perevodnoy menu and
both orderings read a PASS that never happened, silently: the divisor `M` is the
same 30 in both, and the remainder lands in a pass range either way.
Append merely tends to diverge a few moves later, which puts the damage further
from its cause.
In practice neither can happen: the mode bit rides inside the same integer,
written before any atom and read before any atom, and a corrupt header fails
`msg_replay`'s header-vs-body cross-check first.

Two smaller things follow from splicing:

* A podkidnoy code is slightly **smaller** - the transfers it never had are not
  in the model either. `c/tests/msg_wire_test.c` asserts exactly this, and it
  is what would catch the mode being stored and then ignored.
* This menu keeps the same convention as `legal.c`, which gates the pass inside
  `calc_pass_moves` rather than reordering around it, so the codec's menu and
  the one the bots and buttons read cannot drift apart in shape.

The **retrodiction line** (v9, which carries no bit) can still encode a
podkidnoy game faithfully, because every atom such a game can play is in the
perevodnoy menu too - which is why the iOS replay share link needed no change.
Note that this is true under EITHER scheme; it is a consequence of the podkidnoy
menu being a sub-selection of the perevodnoy one, not of where the pass block
sits. An earlier draft of this doc claimed it as a benefit of splicing, and that
was an overclaim.

## Why the wire needed a new format

The FMSG envelope has carried a reserved `variant` byte since the first
version, and it had to be 0.
It is now the RULES byte - and by the owner's call it reads **0 = podkidnoy,
1 = passing**, which is the opposite reading of the same byte.
Every bubble already sitting in a transcript carries 0.

That is what a version number is for.
Formats 2, 3 and 4 keep the old reading (variant 0, and the passing game by
definition); **formats 5 and 6** are formats 3 and 4 with the byte respent, and
they add no bytes at all - 5 is 3's 62-byte header, 6 is 4's 69.
Every seal this build makes writes 5 or 6, because the rules must never again
be a byte whose meaning depends on who is reading.
A build that predates them refuses a format it does not know (`MSG_EFORMAT`)
instead of quietly dealing a different game.

**The header carries the rules even though the body already does.**
A v10 body names its own pass mode, and for a LIVE bubble that would be enough.
A WAITING lobby has no body at all - the deal alone is its state - so the one
place a lobby's rules can live is the header, and the lobby is exactly where
they are chosen.
Carrying it on every phase keeps one answer rather than two, and `msg_replay`
checks the two against each other: a chain whose header says one game and whose
body was cut against the other does not replay (`MSG_EBODY`).

## The lobby

A wooden checkbox (`FCheckbox`, the first in this app - a `WoodFill` plank with
the same hand-drawn `FCheck` the seat badges wear), labelled "Passing
(perevodnoy)" and ticked by default, sitting under the player list.
A spectator sees it and cannot move it: the rules are as much a part of "what
game is this" as the player list, and moving it takes a seat, because a reseal
has to name an actor.

Ticking it **reseals the lobby** and stages that bubble, exactly as a join
does. The change is not a local preference; it is a fact about the table that
everyone must see before anyone starts.

**Whoever changes it cannot start the game** (the owner's rule, "similar to how
last joined cannot start the game").
It is round 5's M9 authorship gate without the full-lobby exemption, and the
exemption's own reasoning is why: that exemption exists so a full lobby is
never stranded with no way forward, and a rules change strands nothing - the
reseal is sendable, and whoever opens it may start at once.
What it would otherwise allow is exactly what the rule forbids: in a
two-player DM (full the moment both are in) the changer could flip the rules
and start in the same breath, and their opponent would first learn of it from a
board that refuses their transfer.

"Did I change them" is answered from the CHAIN, not from a memory of a tap
(`LobbyControls.rulesChanged`): the baseline is the passing value of the last
bubble somebody else put on the chain, compared with what the lobby says now.
That makes it self-cancelling - tick the box back and there is nothing left to
withhold Start for - and it survives the extension being closed mid-lobby,
which a flag would not.

## What is NOT wired

**Online (supabase) games are always perevodnoy.**
The rules would have to survive `games.state`, and that durable blob has no
room for them today.
Nothing about the online path changed: its games are the classic game, its
stored replay codes decode as they always did.
`src/components/Lobby.tsx` carries a note saying so, and what the web checkbox
would need.

**Local games against bots are perevodnoy**, for the same reason there is
nothing to decide: the app's offline setup has no lobby, and a fresh deal is
always the classic game (`fio_new_game` resets the rules - without that, one
podkidnoy lobby would leave every later game on the device podkidnoy, in a
process that never restarts).

## Where it is pinned

| test | what it would catch |
| --- | --- |
| `c/tests/tests.c test_podkidnoy` | the handler and both legal menus, played both ways from one position |
| `c/tests/msg_wire_test.c test_podkidnoy_wire` | the seal, the wire, the replay, the rebuilt game, header/body disagreement, and the code SIZE (the menu gate) |
| `c/ios/ios_api_smoke.c lobby_rules_check` | the lobby flow through the API the app really calls, including Start's re-deal |
| `ios/FoolishTests/PodkidnoyTests.swift` | the checkbox → wire → Start → board chain, the Start gate, and the rulebook's silence |

Each was mutation-checked against the change it guards.
