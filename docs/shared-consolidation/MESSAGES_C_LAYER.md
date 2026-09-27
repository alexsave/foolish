# The C side of the Messages plumbing - consolidation pass over five products

Investigated in the `shared-consolidation` worktree, read-only.
Products: foolish/c (msg_wire.c/h, the shipped kernel), uttt/c (uttt_msg.c/h, in App Store review), pickemup/c (pk_msg.c/h, pk_lobby.c/h), chuiniu/c (cn_msg.c/h, cn_lobby.c/h), tallybones/c (tb_msg.c/h, tb_lobby.c/h).
werewolf/c/src/ww_wire.c, ww_seat.c and ww_lobby.c are cited as background only, per `werewolf/COMMON.md`.
This document is the C-side sibling of `docs/shared-consolidation/IOS_MESSAGES_LAYER.md`, which already covers `shared/swift/MessagesKit` (InsertStaging, DevFlags, SendHint, CollapseSlide, the `MessagesViewController` lifecycle) at the quality bar this document tries to match.
Nothing about the Swift face is repeated here.
Every claim below is a diff, a grep result, or a test run in this worktree; no source file was edited to produce it (`git status --short` was clean before and after).

## Verdict

`shared/c/msg_stage/msg_stage.h` is reached by exactly one path in the whole monorepo: `shared/swift/MessagesKit/InsertStaging.swift:11` (`import CMsgStage`).
No product's C bridge - not `foolish/c/ios/ios_api_msg.c`, not `uttt/c/src/uttt_msg.c`, not `pickemup/c/src/pk_msg.c`, not `chuiniu/c/src/cn_msg.c`, not `tallybones/c/src/tb_msg.c`, and not any of their `ios/include/*.h` bridge headers - contains a `#include` of `msg_stage.h`, and none of them reimplements `ms_drawer_up`, `ms_stage_*` or `ms_receive` in C: a repo-wide grep for `drawer`, `silence`, `echo`, `MS_ACT`, `MS_STAGE` and `insert` inside every product's message-envelope C file turns up nothing but two comments (`uttt/c/src/uttt_msg.h:281-283`, `uttt/c/ios/include/uttt_api.h:122`) that point at `msg_stage.h`/`CMsgStage` rather than restate it.
`foolish/c/ios/ios_api_msg.c:220`'s `fio_msg_staged_atoms_before` is not a second implementation of any `msg_stage` decision: it answers "where does THIS DEVICE's own staged run start in the resident game's atom log", an animation-timeline bookkeeping question, never a Messages-insert-timing or echo question, and it is read by nothing that also touches `ms_stage`/`ms_receive`.
So question 1's headline finding is negative in the useful sense: there is no C-side duplication of `msg_stage.h` to clean up, in any of the five products.

Question 2 is where the real duplication lives, and it splits into two families that do not merge into each other cleanly.
`pickemup/c/src/pk_lobby.c`, `chuiniu/c/src/cn_lobby.c` and `tallybones/c/src/tb_lobby.c` are 85-89% byte-identical after mechanical prefix renaming (`pk_`/`Pk`/`PK_` to `x_`/`X`/`X_`), and `pk_msg_resolve`, `cn_msg_resolve` and `tb_msg_resolve` (the seat-identity priority chain) are 100% byte-identical the same way, as are `*_name_verdict`, `same_name` and the `utf8_chars` UTF-8 validator.
That three-way family descends from `uttt/c/src/uttt_msg.c`'s `utm_resolve` (the `UTM_BY_RECORD` / `UTM_BY_TAG` / `UTM_BY_SENDER` priority chain), extended with a fourth `BY_NAME` tier because pickemup (and its two children) have named, more-than-two-seat rosters where uttt has only X and O.
It is a *different* lineage from `foolish/c/src/msg_wire.c`'s `msg_seat_resolve*` family (cached-seat plus sender-is-local plus name, no cryptographic tag, no persisted "record"), which `werewolf/c/src/ww_seat.c` ported "unchanged in behaviour" per its own header comment.
So there are now two independent, internally-consistent seat-identity algorithms in the monorepo, not one algorithm with knobs: foolish+werewolf resolve identity from a name string and a cached seat index; uttt+pickemup+chuiniu+tallybones resolve it from a salted SHA-256 tag plus a locally persisted per-game "record" seat, falling back to name only as a last resort.
`pickemup/docs/REUSE_AUDIT.md` S8 scoped its lift "over a `{seat, name}` row" - that scoping only fits the foolish/werewolf family; the pk/cn/tb/uttt family needs a `{seat, tag, record}` row instead, and REUSE_AUDIT had only one data point (uttt) for that family when it was written.
The three-way evidence here does NOT make a single five-product unification cleaner; it proves there are two, and it makes lifting the pk/cn/tb-only sub-family trivial and low risk, independent of foolish and werewolf entirely.

The lobby-rule family has the same two-shape split.
`pk_lobby_offered`/`cn_lobby_offered`/`tb_lobby_offered` operate on a `struct { n_seats, dm, newest, started, rev, who[] }` roster and are 85-89% byte-identical to each other.
`foolish/c/src/msg_wire.c:1124-1146`'s `msg_lobby_offered` and `werewolf/c/src/ww_lobby.c`'s `ww_lobby_offered` both instead take five scalar arguments (`my_seat, joined, capacity, i_sent_the_newest[, i_changed_the_rules]`) with no roster struct at all, and are semantically identical to the struct family (same "the newest sender stands aside while there is room, a full table has nobody left to stand aside for" rule, the same "M9" comment lineage) but 0% byte-similar in shape.
`ww_lobby.c` additionally has a `WW_LOBBY_TOO_FEW` verdict pk/cn/tb do not need (no chat shape they support falls under the game's minimum), and foolish's version alone carries the passing-rule-toggle clause (`i_changed_the_rules`) pk/cn/tb do not have at all.
Again: the pk/cn/tb sub-family is a clean, low-risk, self-contained lift; folding foolish's and werewolf's scalar-arg shape into the same header is the harder problem REUSE_AUDIT S9 already rated medium-to-high, and this pass finds nothing that lowers that rating.

The message envelope itself (question 2's header/bounds/digest/Rule-P/tie-break request) is the one place where "lift now" is the wrong call even inside the pk/cn/tb family.
The envelope *skeleton* - magic byte, format byte, phase byte, flags byte, 32-byte seed, `lobby_rev` u16, a bounded per-seat roster loop with `ESHORT` bounds checks, a 2-byte SHA-256 check, and the body - is structurally identical across pk/cn/tb and is explicitly stated to be modeled on uttt's (`pickemup/c/src/pk_msg.h:15`: "THE SHAPE IS UTTT'S ... with foolish's lobby and seat rules"), but the actual byte offsets differ per product because each game bakes different derived counters into the header (`PK_HEAD_LEN` 44 with a `bubbles`+`turns` pair and a `TIP_SAID` flag; `CN_HEAD_LEN` 42 with one `moves` counter and no `TIP_SAID`; `TB_HEAD_LEN` 44 with its own fields) - a genuine per-product wire decision, not a naming difference.
More significantly: uttt, pickemup, chuiniu and tallybones have **no Rule-P ancestry check at all**.
`foolish/c/src/msg_wire.c`'s `msg_rule_p` and its documented port `werewolf/c/src/ww_wire.c`'s `ww_rule_p` both open with a "does `parent8` name the other's `digest`" ancestry test (`names_parent`) before falling back to counters, because those two products encode a bubble as a *delta against a resident chain*.
uttt/pickemup/chuiniu/tallybones instead encode the whole game in every bubble (`pk_msg.h`'s own words: "AN EXTENSION IS HANDED EXACTLY ONE MESSAGE ... so every bubble carries the whole game"), so their `*_msg_prefer`/`utm_prefer` tie-breaks are pure counter-then-digest comparisons with no ancestry step - there is nothing missing, the mechanism these four products need is genuinely simpler than foolish's.
That confirms REUSE_AUDIT S15's "high" risk rating rather than softening it: a shared `msg_env` would have to support two envelope *philosophies* (delta-plus-ancestry-chain vs whole-game-per-bubble), not just parameterise one body codec.

Question 3's two-phone harnesses are genuinely a shared *pattern*, not shared *content*.
`pickemup/c/tests/pk_twophone_test.c`, `chuiniu/c/tests/cn_twophone_test.c` and `tallybones/c/tests/tb_twophone_test.c` all use the identical `STEP`/`OK` assertion-counting macro pair (byte-identical between chuiniu's and tallybones' copies), the identical "TWO PHONES, ONE RESIDENT SLOT" design (save the departing phone's seat records, load the arriving phone's, drop the sender fact, fabricate 16 bytes of device identity, set a nickname, then adopt the newest bubble exactly as a tap would), and tallybones' identity-byte formula (`p * 53 + k * 7 + 1`) is copied verbatim from pickemup's, while chuiniu's differs (`i * 29 + k + 3`) - independently reinvented rather than copied, but the same idea.
None of that extends to `uttt/c/tests/uttt_msg_test.c` (1023 lines, no `STEP` macro, no phone-switch harness at all) or to `foolish/c/tests/msg_wire_test.c` (4427 lines, same: foolish's two-device races are simulated at the TypeScript e2e layer, e.g. `foolish/e2e/msg_concurrency.test.ts`, not in C).
So the harness skeleton is a three-way pattern specific to pickemup and its two children, worth a small shared header; the steering logic that drives each game toward a specific rules state and the oracle that predicts captions and counts is inherently product-specific and does not belong in `shared/`.

All four message-test suites and the `msg_stage_test.c` standalone test pass unmodified in this pass: `msg_stage_test` 60/60, `pk_msg_test` 298,141/298,141 assertions, `pk_twophone_test` 2,420/2,420, `cn_msg_test` 89,178/89,178, `cn_twophone_test` 9,909/9,909, `tb_msg_test` 14,921/14,921, `tb_twophone_test` 458/458 - all 0 failed, run from this worktree with no source changes.

---

## 1. Does any product reach `msg_stage.h` from C, and does anything reimplement it?

**Direct includes.** `grep -rn "msg_stage.h"` across `foolish/`, `uttt/`, `pickemup/`, `chuiniu/`, `tallybones/` and `shared/` finds three hits: `shared/c/msg_stage/msg_stage.h:1` (itself), `shared/c/msg_stage/msg_stage_test.c:7` (its own test), and `uttt/c/src/uttt_msg.h:283`, which is a *comment*, not an include:

```
uttt/c/src/uttt_msg.h:281-283
/* GETTING THE BUBBLE INTO THE FIELD - is the drawer up, what a silent insert
 * means, and whether a bubble handed to didReceive is my own coming back - is
 * shared with the sister product: shared/c/msg_stage/msg_stage.h, read from
 * Swift as CMsgStage (MessagesKit/InsertStaging.swift). */
```

`uttt/c/ios/include/uttt_api.h:122` has the same kind of pointer-comment ("may go and what its silence means are shared/c/msg_stage (CMsgStage)").
No other product's `ios/include/*.h` mentions `msg_stage` at all.

**Swift face (confirmed, not re-audited).** `grep -rln "CMsgStage" --include="*.swift"` returns exactly one file: `shared/swift/MessagesKit/InsertStaging.swift:11` (`import CMsgStage`).
This is the path `IOS_MESSAGES_LAYER.md` already covers end to end (its "MessagesKit adoption table"): uttt, pickemup, chuiniu and tallybones all call `InsertStaging.Loop`/`.drawerUp`/`.receive`; foolish compiles `CMsgStage` in but calls none of it (its own D4 gap, already documented there).
Nothing new to add on the Swift side.

**Reimplementation check.** `grep -n "drawer\|silence\|echo\|MS_ACT\|MS_STAGE\|insert"` against every product's message-envelope C file:

| File | Hits |
| --- | --- |
| `foolish/c/src/msg_wire.c`, `.h` | comments about `conversation.insert` replacing a draft and about a live-drawer report; no decision logic |
| `uttt/c/src/uttt_msg.c`, `.h` | the two pointer-comments above; no decision logic |
| `pickemup/c/src/pk_msg.c`, `.h` | nothing |
| `chuiniu/c/src/cn_msg.c`, `.h` | nothing |
| `tallybones/c/src/tb_msg.c`, `.h` | nothing |

None of the five products' `c/ios/` bridge directories (`foolish/c/ios/`, `uttt/c/ios/`, `pickemup/c/ios/`, `chuiniu/c/ios/`, `tallybones/c/ios/`) contain a `drawer`/`watchdog`/`silence`/`MS_ACT`/`insert_attempt` match either, except for drawer-*geometry* references (collapse height numbers, unrelated to insert gating and already covered under `IOS_MESSAGES_LAYER.md`'s S7/`CollapseSlide` discussion).

**`fio_msg_staged_atoms_before` (the task's explicit question).** `foolish/c/ios/ios_api_msg.c:220-227`:

```c
// Where THIS DEVICE's own staged run starts in the resident game's atom stream
// - the same question msg_seal answers for the bubble delta, asked for the
// animation instead of for the wire, and answered from the same log mark.
...
int fio_msg_staged_atoms_before(void) {
    const FioSession *s = fio_session();
    if (s->msg_base_logs < 0) return -1;
    return replay_atoms_before_log(s->game.logs, s->game.num_logs,
                                   s->msg_base_logs);
}
```

This is **not** the same decision `msg_stage` makes.
It answers "how many atoms of the resident game's replay log belong to this device's own most recent staged run" - an animation-timeline bookkeeping question consumed by `fio_msg_turn_publish` (`ios_api_msg.c:429`, `out_anim_atoms_before`) to decide which of the board's animated events are "mine" for veiling/timing purposes.
It has no connection to whether an `MSConversation.insert` call may go, what a silent insert answer means, or whether a `didReceive` bubble is an echo - those are exactly and only `ms_drawer_up`, `ms_stage_*` and `ms_receive`, called from Swift, never from this function or its callers.

**A related-but-distinct mechanism, for context (not a duplication of `msg_stage`).** `foolish/c/src/msg_expand.h` and `foolish/c/ios/include/ios_api.h:1279-1298` implement a second "measured, not reasoned" silent-discard-and-retry gate, but for a different Apple API: `requestPresentationStyle(.expanded)` on the name-entry screen, not `conversation.insert`.
Its header documents its own field measurement (8 cold opens on a real iPhone 17, iOS 26.3: 0/4 landed from `onAppear`, 4/4 landed from the host's `willTransition` callback) and states plainly "The extension is the only host with this screen, so this pair is not in any wasm module."
`grep -rn "msg_expand"` confirms it: the only hits are inside `foolish/c/src/`, `foolish/c/ios/` and `foolish/c/tests/` - no other product has this file or anything like it.
This is the same *design pattern* as `msg_stage` (Apple silently drops a call before some host-side installation event; treat silence as refusal; retry on the event that proves installation happened) independently discovered a second time for a different call, which is a point in favor of the pattern rather than evidence of duplication - but it is single-user code today and not a lift candidate.

---

## 2. The message envelope family: seat identity, lobby rules, envelope skeleton

### 2.1 Seat identity - two families, not one

**Family A (tag + record): uttt, pickemup, chuiniu, tallybones.**

`uttt/c/src/uttt_msg.h:189-202` defines the priority order:

```c
#define UTM_BY_NONE     0
#define UTM_BY_RECORD   1
#define UTM_BY_TAG      2
#define UTM_BY_SENDER   3
...
int  utm_resolve(const UtmMsg *m, int record, int tag_seat, int is_dm, int i_sent, int *by);
```

`pickemup/c/src/pk_msg.c:515-547`'s `pk_msg_resolve` generalizes it to a named, N-seat roster with a fourth tier, `PK_BY_NAME`:

```c
int pk_msg_resolve(const PkMsg *m, int record, int tag_seat, int is_dm, int i_sent,
                   const uint8_t *name, int name_len, int *by)
{
    const int n = m->n_seats;
    int b = PK_BY_NONE, seat = -1;
    if (record >= 0 && record < n) {
        b = PK_BY_RECORD; seat = record;
    } else if (tag_seat >= 0 && tag_seat < n) {
        b = PK_BY_TAG; seat = tag_seat;
    } else if (record != PK_REC_GONE) {
        int s = -1, snd = pk_msg_sender(m);
        if (i_sent == 1) s = snd;
        else if (i_sent == 0 && is_dm && n == 2 && snd >= 0) s = 1 - snd;
        if (s >= 0 && !started(m) && name && name_len > 0 && !same_name(&m->seat[s], name, name_len)) s = -1;
        if (s >= 0) { b = PK_BY_SENDER; seat = s; }
        else if (name && name_len > 0) {
            for (int t = 0; t < n; t++) if (same_name(&m->seat[t], name, name_len)) { b = PK_BY_NAME; seat = t; break; }
        }
    }
    if (by) *by = b;
    return seat;
}
```

`chuiniu/c/src/cn_msg.c:483-511`'s `cn_msg_resolve` and `tallybones/c/src/tb_msg.c:528-556`'s `tb_msg_resolve` are **byte-identical** to this after mechanically renaming `pk_`/`Pk`/`PK_` to `x_`/`X`/`X_`: `diff` of the two normalized functions against pickemup's is 0 lines for tallybones and a single one-word comment difference for chuiniu ("not me (D51)" vs "not me (pickemup D51)").
Only the small helper each calls, `*_msg_sender` (which seat sent the most recent bubble), differs, because it reads each product's own history representation (pickemup walks a `hist[]` array looking for a sealed `PK_A_BUBBLE` record; chuiniu reads a `last_seat` field directly; tallybones reads the seat off the last move via its own `move_at()` accessor) - that part is genuinely product-specific and not liftable without also lifting each product's game-history layout.

The nickname/name utilities are likewise identical: `pk_name_verdict`/`cn_name_verdict`/`tb_name_verdict` (`pickemup/c/src/pk_msg.c:67-74`, `chuiniu/c/src/cn_msg.c:67-74`, `tallybones/c/src/tb_msg.c:67-74`), the `same_name` helper immediately below each, and the 15-line strict-UTF-8 validator `utf8_chars` immediately above each, are **byte-for-byte identical** (mod the `Pk`/`Cn`/`Tb` prefix) across all three files - confirmed with `diff` on the three functions with no remaining hunks at all.

**Family B (name + cached seat): foolish, werewolf.**

`foolish/c/src/msg_wire.c:1009 (msg_seat_resolve family)` resolves identity from `(cached_seat, sender_is_local, n_players, last_actor_seat, chat_is_dm, name)` - no cryptographic tag, no persisted "record" concept.
`werewolf/c/src/ww_seat.h`'s own header comment states this plainly: "PORTED FROM THE FORK UNCHANGED IN BEHAVIOUR (msg_wire.c's msg_seat_resolve* family...)".
`ww_seat.c` is 94 lines against `msg_wire.c`'s ~95-line seat-resolve section (`:989-1104`), a direct, acknowledged port.

**The two families do not share a data model.** Family A carries a 9-byte salted SHA-256 tag per device (`pk_tag`, `PK_TAG_LEN 9`, "UTTT's, utm_tag" per `pk_msg.h:112`) and a locally-persisted "record" seat that survives app relaunch; family B carries no tag at all and instead trusts `sender_is_local` (a fact Messages itself reports) plus a name string.
REUSE_AUDIT S8 scoped its lift "over a `{seat, name}` row" - that is family B's shape.
This pass's evidence is that family A (four products: uttt plus the three newest) is a second, independently-converged, internally-consistent design that REUSE_AUDIT had only one data point for (uttt) when S8 was scoped.
**That makes a same-family lift (pk/cn/tb, and arguably uttt too) a clean, low-risk step available today, and it does NOT make the cross-family unification S8 originally imagined any easier** - if anything it raises the bar, because a shared abstraction now has to support two different seat-identity philosophies rather than one algorithm with different field names.

### 2.2 Lobby rules - same two-shape split

`pickemup/c/src/pk_lobby.c` (100 lines) vs `chuiniu/c/src/cn_lobby.c` (98) vs `tallybones/c/src/tb_lobby.c` (99), diffed after the same mechanical renaming:

- pk vs cn: 82 diff lines (in the whole `pk_msg.c`/`cn_msg.c` normalized diff; isolating just `pk_lobby.c`/`cn_lobby.c` the diff is 4 hunks / ~15 lines out of ~95, about 84% identical). Differences: the game-start call signature (`pk__new(g, seed, n_seats, seat, 0)` vs `cn_new(g, seed, n_seats)` - chuiniu's game has no explicit starter/round-0 argument the way pickemup's does), the event struct has a `card` field only in pickemup's `PkEvent` (dice and tally games have no card to record), and one comment reword.
- pk vs tb: 4 hunks / ~10 lines out of ~95, about 89% identical. Differences: the header comment crediting the copy ("Pick 'Em Up's copied (T2)"), and `tb_new`'s call signature (`tb_new(g, seed, n_seats, seat)`, closer to pickemup's than chuiniu's but still not identical).

Functions confirmed byte-identical across all three (0 diff lines): `*_lobby_cap`, `*_lobby_new`, `*_lobby_seat_of`, `*_lobby_join`, `*_lobby_can_exit`, `*_lobby_leave`, `*_lobby_can_join_and_start`, `*_plan_lobby`.
The one verdict function with real logic, `*_lobby_offered`, is also byte-identical across all three (same "M9" comment, same two-clause structure: newest-sender-stands-aside while there is room, full table exempt).

`foolish/c/src/msg_wire.c:1124-1146`'s `msg_lobby_offered` and `werewolf/c/src/ww_lobby.c`'s `ww_lobby_offered` implement the identical rule in words but over five scalar arguments and no roster struct:

```c
// foolish, msg_wire.c:1124
int msg_lobby_offered(int my_seat, int joined, int capacity,
                      int i_sent_the_newest, int i_changed_the_rules) {
    if (my_seat >= 0) {
        if (joined >= 2) {
            if (i_changed_the_rules) return MSG_LOBBY_WAITING;
            // M9: the newest sender stands aside WHILE THERE IS STILL ROOM...
            if (i_sent_the_newest && joined < capacity) return MSG_LOBBY_WAITING;
            return MSG_LOBBY_START;
        }
        return i_sent_the_newest ? MSG_LOBBY_WAITING : MSG_LOBBY_INVITE;
    }
    return joined < capacity ? MSG_LOBBY_JOIN : MSG_LOBBY_FULL;
}
```

```c
// werewolf, ww_lobby.c:29
int ww_lobby_offered(int my_seat, int joined, int capacity, int i_sent_the_newest) {
    if (my_seat < 0) return joined < capacity ? WW_LOBBY_JOIN : WW_LOBBY_FULL;
    if (ww_lobby_impossible(capacity)) return WW_LOBBY_TOO_FEW;
    if (ww_lobby_can_start(joined, capacity)) {
        if (i_sent_the_newest && joined < capacity) return WW_LOBBY_WAITING;
        return WW_LOBBY_START;
    }
    return i_sent_the_newest ? WW_LOBBY_WAITING : WW_LOBBY_INVITE;
}
```

Same parameter names (`my_seat`, `joined`, `capacity`, `i_sent_the_newest`), same rule, 0% byte overlap with the struct-based family, and each carries one clause the other family lacks: `msg_lobby_offered` has the passing-rules-toggle clause (`i_changed_the_rules`) pk/cn/tb do not need; `ww_lobby_offered` has the `WW_LOBBY_TOO_FEW` verdict pk/cn/tb do not need (every chat shape they support can reach their minimum of 2).
Neither pk/cn/tb's `n_seats >= 2` (hardcoded, e.g. `pk_lobby_can_exit`) nor foolish's `joined >= 2` (`msg_wire.c:1124`, `:1146`) is parameterised today - REUSE_AUDIT S9's proposed `(min_players, max_players, has_rules_toggle)` signature would need a `TOO_FEW` verdict bolted on for werewolf and a struct-vs-scalar bridging decision neither REUSE_AUDIT nor this pass resolves for free.

### 2.3 The envelope skeleton - shape shared, bytes not

`pickemup/c/src/pk_msg.h:1-16` states its own provenance: "THE SHAPE IS UTTT'S (uttt/c/src/uttt_msg.h) with foolish's lobby and seat rules (foolish/c/src/msg_wire.h)."
Confirmed structurally: `pk_msg_decode`/`cn_msg_decode`/`tb_msg_decode` (not separately renamed above, part of the whole-file diff) share, byte-for-byte, the magic/format check, the phase/flags parse with an `EFLAGS` refusal for unknown bits, the `seed`/`lobby_rev` read, the bounded per-seat roster loop (`tag[N]`, `name_len`, `name`, each step guarded by an `ESHORT` bounds check before the read), the SHA-256 check-digest verification, and the "the header must say what the replay says or the whole message is refused" discipline.
What differs, function by function, in the pk-vs-cn normalized diff (82 line-diffs out of 595 lines total in `pk_msg.c`):

| Region | Same? | Why |
| --- | --- | --- |
| `*_tag`, `*_game_id` salts | different by one string literal | intentional per-product domain separation (`"pickemup.seat.1\|"` vs `"chuiniu.seat.1\|"`) |
| `*_name_verdict`, `same_name`, `utf8_chars` | identical | pure string/roster arithmetic, no game state |
| `roster_ok`, `lobby_rev_ok`, `cap_of`, `started` | identical | pure roster/lobby bookkeeping |
| `*_msg_encode` header-byte layout | different | pickemup bakes `bubbles` + `turns` + a `TIP_SAID` flag (44-byte head); chuiniu bakes one `moves` counter, no `TIP_SAID` (42-byte head); tallybones bakes its own pair at 44 bytes - each is what that game's rules need in the header to self-verify against its replay |
| `*_msg_decode` bounds/roster loop | identical | same bounds-checked loop shape |
| `*_msg_decode` per-game-counter cross-check | different | each checks its own header fields against its own replay's derived fields |
| `*_msg_prefer` tie-break | mostly identical shape, different counters | started-first, then per-game progress counters, then `lobby_rev`, then `n_seats`, then a SHA-256 digest fallback (`digest_of`/`memcmp`) - no ancestry step in any of the three |
| `*_common_moves`/`*_common_bubbles` | different | walks each product's own history representation |
| `*_msg_sender` | different | reads each product's own "who sent the last bubble" fact, by a different route each time (see 2.1) |
| `*_msg_resolve` | identical | see 2.1 |

No product in this family (uttt, pickemup, chuiniu, tallybones) implements Rule P's ancestry-first check.
`foolish/c/src/msg_wire.c:707`'s `msg_rule_p` and `werewolf/c/src/ww_wire.c:310`'s `ww_rule_p` both open with a parent-link test before any counter comparison:

```c
// foolish, msg_wire.c: msg_chain_key carries parent8 + a SHA-256 digest of the
// whole envelope (msg_digest); msg_rule_p's first clause (not shown, see the
// file) asks names_parent(a->parent8, b->digest) / names_parent(b->parent8, a->digest)
// - "a chain's own direct child outranks it, whatever the other fields say."
```

```c
// werewolf, ww_wire.c:320-322
const int a_is_child = names_parent(a->parent8, b->digest);
const int b_is_child = names_parent(b->parent8, a->digest);
if (a_is_child != b_is_child) return a_is_child ? -1 : 1;
```

This is possible only because foolish's and werewolf's envelopes carry a delta against a resident chain (so a bubble's parent digest is meaningful) and both games can accumulate arbitrarily long histories across many bubbles.
uttt, pickemup, chuiniu and tallybones each re-encode the entire game from the deal in every bubble (`pk_msg.h`'s own words, quoted in the Verdict), so there is no delta to arbitrate and no ancestry chain to check - `utm_prefer`/`pk_msg_prefer`/`cn_msg_resolve`/`tb_msg_resolve`'s tie-break logic is complete without one.
A shared `msg_env` (REUSE_AUDIT S15) would have to support both shapes - "envelope carries a delta plus a parent digest" and "envelope carries the whole state, no parent" - which is a bigger fork in the abstraction than a single body-codec vtable, and this pass finds no evidence that narrows it.

---

## 3. Two-phone test harnesses

| File | Lines | `STEP`/`OK` macros | Phone-switch pattern |
| --- | --- | --- | --- |
| `pickemup/c/tests/pk_twophone_test.c` | 951 | yes, with an extra `g_quiet` gate not in the other two | `pick_up(p)`: save departing phone's seat records, load arriving phone's, `*_sender(NULL,0,-1)`, fabricate 16 identity bytes `p*53+k*7+1`, set nickname |
| `chuiniu/c/tests/cn_twophone_test.c` | 313 | yes, byte-identical macro block to tallybones' | `be(i)`: same shape, identity bytes `i*29+k+3` (independently written, not copied from pickemup) |
| `tallybones/c/tests/tb_twophone_test.c` | 276 | yes, byte-identical macro block to chuiniu's | `pick_up(p)`: same shape, identity bytes `p*53+k*7+1` - copied verbatim from pickemup's formula |
| `uttt/c/tests/uttt_msg_test.c` | 1023 | no `STEP` macro anywhere; no phone-switch harness | none - envelope tests are direct unit assertions |
| `foolish/c/tests/msg_wire_test.c` | 4427 | no `STEP` macro; no phone-switch harness | none - two-device races are simulated at the TypeScript e2e layer (`foolish/e2e/msg_concurrency.test.ts`, `msg_full_game.test.ts`), not in C |

What is duplicated: the `STEP(name)`/`OK(cond, ...)` assertion-counting macro pair, and the "one resident slot, switch phones by saving/loading seat records and dropping the sender fact" harness idea, which is a real design decision worth stating once (a device really does keep exactly one thing resident between bubbles, which is why the test switches by save/load rather than by holding two independent state trees).
What is not duplicated, and should not be: the steering logic that walks each product's own RNG/seed space toward a specific game state ("a wild with a suit", "a KEEP SCORE turn", a specific call/raise sequence), and the oracle that predicts captions, counts and turn order from each product's own rules document - that logic is the test, not the harness around it, and it is exactly as product-specific as the game itself.
This is a lift candidate for the harness skeleton only (a small shared header with the macro pair and, at most, a documented pattern for the phone-switch shape - the actual save/load calls go through each product's own generated `*_api_seats_save`/`_load`, which cannot be shared without sharing the generated bridge itself), not for the test files as wholes.

---

## Ranked list

**Lift now:**

- `shared/c/msg_seat_tag.{c,h}` (name to be chosen; not `msg_seat`, which REUSE_AUDIT S8 already reserved for the foolish/werewolf name-based family) for pickemup, chuiniu and tallybones only: `*_name_verdict`, `same_name`, `utf8_chars`, and `*_msg_resolve` (the record/tag/sender/name priority chain).
  These four are byte-identical across all three products today, per the diffs in section 2.1, and touching only pk/cn/tb means zero risk to foolish, uttt or werewolf.
  Proof: `make -C pickemup/c build/pk_msg_test build/pk_twophone_test && ./build/pk_msg_test && ./build/pk_twophone_test`, same for chuiniu and tallybones, assertion counts unchanged (298,141 / 2,420 for pickemup; 89,178 / 9,909 for chuiniu; 14,921 / 458 for tallybones, all 0 failed, as run in this pass) before and after; each product's Makefile `SRC`/`HDR` list gets the new file added explicitly (these Makefiles use explicit source lists, not wildcards over `shared/c`, confirmed in `pickemup/c/Makefile:19-25`); `msg_seat_tag.h` gets added to `SHARED_HEADERS` in `foolish/e2e/validation/shared_headers_reachable_validation.test.ts` (currently only `['sha256.h', 'deal_rng.h']` - note this list is already stale relative to `shared/README.md`'s inventory, which lists `b32.h`, `i18n/languages.h`, `motion_ruler` and `msg_stage.h` as shared too; that staleness is a pre-existing gap this pass noticed but is out of scope to fix here).
  Whether uttt also adopts the same header is a separate, slightly higher-risk decision (uttt's `utm_resolve` is the three-tier ancestor, not byte-identical to the four-tier pk/cn/tb version, and uttt is in App Store review) - worth doing, but as its own step with its own proof, not bundled into the pk/cn/tb move.

- `shared/c/msg_lobby_roster.{c,h}` for pickemup, chuiniu and tallybones only: the roster-struct lobby family (`*_lobby_cap/_new/_seat_of/_join/_can_exit/_leave/_can_join_and_start/_offered/_plan_lobby`), parameterised by `(dm_cap, group_cap)` and a game-start callback or function pointer to cover the `pk__new`/`cn_new`/`tb_new` signature difference.
  85-89% byte-identical today per section 2.2; same low-risk profile as the seat-tag lift (pk/cn/tb only, explicit Makefile `SRC` edits, `SHARED_HEADERS` addition, before/after assertion-count proof).

- A small shared C test-harness header (e.g. `shared/c/test_twophone.h`) carrying just the `STEP`/`OK` macro pair, for pickemup, chuiniu and tallybones' `*_twophone_test.c` files.
  Zero product risk since it only touches test binaries that never ship; proof is the same "assertion count and pass/fail status unchanged" check as above.
  Do not try to lift the phone-switch pattern itself (the save/load calls are generated per product and the steering/oracle logic is the point of each test).

**Lift later:**

- Unifying the tag-family (uttt + pickemup + chuiniu + tallybones) seat-identity header with the name-family (foolish + werewolf) one, as REUSE_AUDIT S8 originally scoped.
  This pass's evidence does not make it easier: it confirms two incompatible data models (`{tag, record}` vs `{name, cached_seat}`) rather than one algorithm with parameters.
  If it is ever done, it should be done as two headers behind one small dispatch, not one header with a mode flag, because the fields genuinely differ (one family has no tag at all; the other has no plain cached-seat-index concept).

- Unifying the struct-roster lobby family (pk/cn/tb) with the scalar-argument lobby family (foolish/werewolf), as REUSE_AUDIT S9 scoped.
  Same reasoning: two shapes, not parameters of one shape.
  The pk/cn/tb-only lift above is the useful subset of S9 available today; the cross-family piece keeps its medium-to-high risk rating.

- `shared/c/msg_env.{c,h}` (REUSE_AUDIT S15), the envelope header parse plus Rule P plus tie-break, as a single shared abstraction.
  This pass raises rather than lowers that risk rating: a shared envelope now has to support "delta plus ancestry chain" (foolish, werewolf) and "whole game, no ancestry" (uttt, pickemup, chuiniu, tallybones) as two genuinely different philosophies, not a single body-codec vtable as `werewolf/COMMON.md` item 3 proposed for the foolish/werewolf pair alone.
  A `shared/c/msg_env_whole.{c,h}` scoped only to the whole-game-per-bubble family (uttt + pickemup + chuiniu + tallybones) - covering just the shared skeleton in section 2.3's table (magic/format/phase/flags/seed/lobby_rev parse, the bounded roster loop, the check-digest verify, the counter-then-digest tie-break with no ancestry step) - is a smaller, lower-risk version of S15 worth scoping as its own step; this pass did not attempt to design it, only to show the skeleton that would go in it.

- `shared/c/msg_expand.h` style "silent discard, retry on transition" pattern: currently single-user (foolish only).
  Nothing to lift until a second product needs `requestPresentationStyle(.expanded)` timing help; noted here only so the pattern is not reinvented a third time without checking this file first.

**Do not lift:**

- The envelope's per-product header byte layout (`PK_HEAD_LEN`/`CN_HEAD_LEN`/`TB_HEAD_LEN` and what each bakes into it) - genuinely different wire bytes driven by each game's own derived state, not a naming difference (section 2.3).
- Each product's `*_msg_sender`, `*_common_moves`/`*_common_bubbles`, and `*_msg_encode`'s per-game-counter block - each reads or writes that product's own history representation directly and has no product-neutral core left once the shared roster/check/bounds pieces above are subtracted out.
- The two-phone tests' steering and oracle logic (section 3) - by construction as product-specific as the rules documents they are transcribing.
- `fio_msg_staged_atoms_before` (`foolish/c/ios/ios_api_msg.c:220`) - confirmed not a duplicate of any `msg_stage` decision; it is animation atom-bookkeeping with a different job entirely, so there is nothing here to consolidate with `msg_stage.h`.

## Lift attempt: the roster-struct lobby (M8, done)

`pk_lobby.c`, `cn_lobby.c` and `tb_lobby.c` were re-diffed function by function after renaming the prefixes.

| Function | Verdict | Difference |
| --- | --- | --- |
| `*_lobby_cap` | identical | the group cap constant only (8 / 6 / 8); the DM cap is 2 in all three |
| `*_lobby_new` | identical | none |
| `*_lobby_seat_of` | identical | none |
| `*_lobby_join` | identical | none |
| `*_lobby_can_exit` | identical | none |
| `*_lobby_leave` | identical | one comment |
| `*_lobby_offered` | identical | comment wording only; the rule (newest sender stands aside while there is room, full table exempt) is the same |
| `*_lobby_can_join_and_start` | identical | none |
| `*_lobby_start` | differs by the constructor only | `pk__new(g, seed, n, seat, 0)` / `cn_new(g, seed, n)` / `tb_new(g, seed, n, seat)`; the gate (offered START) and "started only when the constructor succeeds" are the same |
| `*_plan_lobby` and its `ev` helper | differs by the event type only | the product's event struct (pickemup's has a `card` field set to `PK_CARD_NONE`); the rule (leaves at old seats, then joins at new seats, by handle) is the same |
| the struct | differs by one bound | `who[MAX_SEATS]` is 8 / 6 / 8 wide; nothing outside the kernel sees it (not in any structgen root) |

No function differs by a rule, so everything moved.
The rules live in `shared/c/msg_lobby_roster` over `MsgLobbyRoster` (which carries `group_cap`), with the game constructor as the one callback of `msg_lobby_roster_start` and the roster's changes as a product-neutral list from `msg_lobby_roster_plan`.
Each product keeps a lobby adapter of about 27 lines: its group cap, its `*_LOBBY_*` constants as aliases (structgen reads them), `typedef MsgLobbyRoster *Lobby`, `*_lobby_start` (binds its constructor) and `*_plan_lobby` (builds its events).
The three lobby files went from 297 lines of `.c` to 81.
