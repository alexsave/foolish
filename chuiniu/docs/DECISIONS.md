# Chui Niu - decisions

The one decisions doc for the proof of concept.
The owner asked for no full design phase: decide, write one paragraph here, move on.
Each section has one owner; a worker writes only inside its own section.
The owner can veto any decision individually.

## Rules (owner: the kernel worker; R1 onward)

R1: 2 to 6 seats, five six-sided dice each at the start, one die lost per lost challenge, out at zero dice, last seat with dice wins.

R2: a bid is (quantity, face) with face 2 to 6; 1s are wild and count toward every face.
A bid is higher than the last when its quantity is greater, or its quantity is equal and its face is higher.
There is no bid on 1s and no "spot on" call in the proof of concept.

R3: instead of raising, the player on turn calls the last bid.
Every die is revealed.
If the count of the bid's face plus the wild 1s is at least the bid's quantity the caller loses a die, otherwise the bidder loses a die.

R4: the loser of the call bids first in the next round; if the loser is out, the next live seat after it in seat order does.
Round 1 opens with the host (seat 0).
Turn order is seat order among the seats that still hold dice.

R5: the opening bid of a round is any (quantity, face) with quantity at least 1.

R6: every round's dice are derived, never rolled and never sent (see K2), so cancelling a staged bubble and reopening the extension produces the same dice.

## Kernel and wire (owner: the kernel worker; K1 onward)

K1: C prefix `cn_`, kernel in `chuiniu/c/src/cn_*.{c,h}`, tests in `chuiniu/c/tests/`, bridge in `chuiniu/c/ios/` with the one header Swift sees (`cn_api.h`, module `CChuiniu`), Makefile targets `run`, `asan`, `ios-smoke`, `structgen`, `datagen`, `swift-smoke`, `ios-lib` as in `pickemup/c/Makefile`.

K2: dice derivation goes through `shared/c/deal_rng` only, seeded from a SHA-256 over already-sent state (the game seed, the round index and the digest of the log as it stood when the round opened) plus the seat, so no player can pick a roll by cancelling and re-staging.

## iOS and rendering (owner: the iOS worker; I1 onward)

I1: SwiftUI, in `pickemup/`'s shape: `ChuiniuKit`, `ChuiniuMessages`, `ChuiniuMessagesApp`, project `chuiniu/ios/Chuiniu.xcodeproj` from `chuiniu/ios/project.yml`, module `CChuiniu`, `chuiniu/ios/vendor/Chuiniu.xcframework` from `make -C c ios-lib`.

## Orchestration (owner: the orchestrator; O1 onward)

O1: the proof of concept is built in three parallel packages (kernel and wire; iOS scaffold with the dice and cup primitive; legal, README and CI) and one tie-together package that wires the screens to the bridge and proves it on a simulator inside Messages.

O2: the working name is Chui Niu (吹牛), the owner's choice and the game's own generic Chinese name; it is threaded through one `GAME_NAME` string like Pick 'Em Up's.
The names Perudo and Dudo, pirate theming and any published product's cup or box art are avoided (`LEGAL.md`).

## BLOCKED (things only the owner can decide)

- A real trademark search on "Chui Niu" and on "Liar's Dice" as a store name.
- App Store Connect, signing, TestFlight and store metadata: skipped for the proof of concept on purpose.
