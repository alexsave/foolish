# Werewolf

A social-deduction game that lives entirely inside an iMessage thread, 5 to 10
players, with no server. Forked from `foolish` at `689630eb`.

## Read this first

- **[docs/NIGHT_KERNEL.md](docs/NIGHT_KERNEL.md)** - how the night works, what it
  hides, and the evidence that it hides it. Start here.
- **[COMMON.md](COMMON.md)** - what was inherited untouched, and what had to be
  touched that should never have been one game's business.

## The shape

```
c/src/       the kernel. Every rule in this product is here and nowhere else.
             ww_game    roles from a seed, the night, the kill
             ww_view    per-seat masking - THIS is the role hiding
             ww_wire    the WMSG envelope and Rule P
             ww_seat    which seat this device is, on a bubble's roster
             ww_lobby   when a locked seed is allowed to become roles
             deal_rng   inherited: a crypto-grade seeded shuffle
             sha256     inherited: the chain link and the tiebreak
c/ios/       the Swift-visible bridge. Flat accessors that read the masked blob.
sdk/swift/   the Swift face of the bridge. Thin, and answers no rule.
ios/         the iMessage extension and its one night screen.
```

## The gate

```
make -C c tests          the kernel (2390 assertions) + the bridge (115)
make -C c tests-asan     the same suite under ASan + UBSan
ios/scripts/mac_tests.sh needs Xcode: the release gate, the Swift tests,
                         and a build of the shipping target
```

`ios/scripts/release_gate.sh` refuses to let the debug seat picker ship. In this
game, choosing a seat is choosing to be the wolf.

## Looking at a night

A werewolf night is a claim about what several people can see, and it is only
observable where the bubbles really are. You cannot add participants to Messages,
so one operator becomes every player:

```
export WW_SIM=<your own simulator's udid>      # not somebody else's
ios/Tools/rig/night.sh build
ios/Tools/rig/night.sh stage dark
ios/Tools/rig/night.sh open                    # creates the App Group container
ios/Tools/rig/night.sh flag                    # switch the solo rig on
ios/Tools/rig/night.sh open                    # flags are read once per process
```
