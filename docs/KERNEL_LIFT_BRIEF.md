# The Swift-to-C lift: the brief every stage works from

This is the working brief for the campaign that moves logic out of
`ios/FoolishKit` and into the C kernel under `c/`.
It exists so each stage can be handed to a fresh pair of hands without
re-deriving the ground rules.

## Why

The iMessage extension grew a rich animation and interaction model in Swift while
the C animation core (`c/src/anim_plan.h`) stood still.
More clients are coming - the iOS app proper, a watch, whatever follows - and
every rule left in Swift is a rule each of them re-derives and gets subtly wrong.

The owner's standing instruction: **when in doubt, move code to C.**
Where a thing genuinely cannot leave Swift, it should at least leave the view
and land in `FoolishKit`, so the iOS app can reuse it.

## The direction of travel

**iMessage is the spec.**
Not the web.
The extension's behaviour has been refined by hand over dozens of device rounds
and the owner prefers it.
When an iMessage rule lands in C it **replaces** whatever rule it meets there;
the web becomes the client that re-derives.
Do not reconcile an incoming rule with the web's version - that is backwards.

## The boundary

Only **rendering** is irreducibly per-platform: interpolation and springs, view
updates, screen coordinates, gesture previews, anything typed in `CGRect` /
`CGPoint` / `Angle`.

Everything above that line is pure data transformation and belongs in C:
ordering, grouping, which counts freeze until when, which cards are veiled,
which seats change role and when, what a gesture resolves to, what a conflict
means.

A useful test: if the function would give the same answer on a watch with a
different screen, it belongs in C.

## No JSON

**Every new kernel entry this campaign adds crosses as PACKED BYTES.**
Not JSON.
This is the owner's standing position and it is not a preference about taste -
task #17 spent a whole round taking production Swift off JSON decode, and a new
JSON entry hands that ground straight back.

So:

- A new `fio_*` entry gets a fixed-layout byte blob and a Swift decoder beside
  the other wire decoders in `sdk/swift/`, in the shape `fio_state_packed` /
  `fio_legal_packed` / `fio_bot_drive_packed` already use.
- Where a stage meets an existing `*_json` entry that it is replacing, the JSON
  one goes.
  `fio_anim_plan_json` is the case in point: its only caller in the entire repo
  is the C smoke test, so Stage 4 replaces it with a packed twin and deletes it
  rather than leaving two plans that can disagree.
- `c/src/json_out.c` is the ONE exception and stays.
  It is how non-Swift hosts (the web, through wasm) read the kernel's formats,
  and it is a reader of packed bytes rather than a second format.
  Refactoring it is fine; growing it for a Swift caller is not.
- `docs/ANIMATION_CORE_C.md`'s "Mac session" checklist opens by telling you to
  call `fio_anim_plan_json` and decode it with `Codable`.
  That step is stale and this rule overrides it.

If a packed layout feels like too much ceremony for what you are moving, that is
usually a sign the thing should cross as a handful of ints rather than as a blob
at all.

## Comments

The Swift these rules are moving out of carries very long archaeological comment
blocks - the history of a bug, the owner's words, which round it was found in.

**Do not copy those blocks into C.**
Compress each to a short statement of what the rule *is* and why it is not the
obvious thing.
A sentence or two.
The history stays in git and in the Swift file's own history; the C header is a
specification, not a scrapbook.

Where a Swift site is deleted outright, its comment goes with it.
Where a Swift site becomes a call into C, leave one line saying which C function
now answers.

## House rules that apply to every stage

- **Mutation-check every test.**
  A test that passes against the broken version of the thing it guards is not a
  test.
  Break the rule deliberately, watch the test fail, put it back.
  Say in the test file which mutation was run.
- **Prefer delete.**
  Code that is dead in production and kept alive only by its tests goes, and the
  tests go with it.
- **No em dashes** anywhere - in code, comments, commit messages or prose.
  Plain `-`.
- **Never freeze replay codes as fixtures.**
- One sentence per line in long Markdown.
- Commit messages: lower-case conventional prefix, a sentence that says what is
  now true rather than what was done.

## Verifying

    cd c && make tests && ./build/cnitro_tests      # 3200+ cases, must be 0 failed
    cd c && make ios-lib                            # rebuilds the xcframework
    ios/scripts/mac_tests.sh                        # FoolishTests + HarnessTests + shipping build
    npm run test:swift-parity                       # the Swift/TS codec gate

Editing anything under `c/` makes `ios/vendor/Foolish.xcframework` stale and a
FoolishKit build phase will refuse to link until `make ios-lib` has run.
That guard is doing its job; run the rebuild, do not disable it.

`ios/scripts/mac_tests.sh` handles the `xcodegen`-blanks-the-entitlements
landmine on its own.
Do not run `xcodegen generate` by hand without restoring
`ios/FoolishApp/Foolish.entitlements` with `cp -p` afterwards.

## Where things are

| what | where |
|---|---|
| the animation core | `c/src/anim_plan.{c,h}` |
| the event wire (writer, and now the reader) | `c/src/evwire.{c,h}` |
| legality and the move menu | `c/src/legal.{c,h}` |
| JSON emission for hosts | `c/src/json_out.{c,h}` |
| the iOS bridge | `c/ios/ios_api.c`, `c/ios/include/ios_api.h` |
| C tests | `c/tests/tests.c` |
| the Swift board | `ios/FoolishKit/Boards/MessageTableView.swift` |
| the staging/send controller | `ios/FoolishKit/Messages/MessageTurnController.swift` |
| the Swift kernel bindings | `sdk/swift/` |

## Not in scope

Do not bump the version and do not archive or upload.
The owner runs those builds by hand for this campaign.
