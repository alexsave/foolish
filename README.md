# foolish

This repository holds several products side by side, each in its own folder, with the code they share kept once.
Each product that is built follows the same rule: one kernel written in C owns the logic, the drawing, the motion and the words, and everything else is a way of reaching it.
`docs/ARCHITECTURE_AS_A_PATTERN.md` is that idea written down on its own.

| Folder | What it is |
| --- | --- |
| [`foolish/`](foolish) | Foolish, a Durak card game: the website at foolish.cards, the iMessage app, the Supabase server, the native server, and the C kernel under all of them. Start at [`foolish/README.md`](foolish/README.md). |
| [`uttt/`](uttt) | Ultimate Tic-Tac-Toe: an iMessage app, and the replay site at uttt.live that plays back a finished game. See [`uttt/README.md`](uttt/README.md). |
| [`werewolf/`](werewolf) | A social-deduction game that lives inside an iMessage thread. Paused. See [`werewolf/README.md`](werewolf/README.md). |
| [`pickemup/`](pickemup) | Pick 'Em Up (a working title), a shedding card game for 2 to 8 in an iMessage thread: the C kernel, its wire and bridge are built and tested, the Messages extension is built and unit-tested but not yet seen inside Messages. See [`pickemup/README.md`](pickemup/README.md). |
| [`chuiniu/`](chuiniu) | Chui Niu (吹牛), Liar's Dice for 2 to 6 in an iMessage thread: a proof of concept with a C kernel and a Messages extension, no App Store Connect and no TestFlight. See [`chuiniu/README.md`](chuiniu/README.md). |
| [`shared/`](shared) | The code more than one of them builds: the generators, the toolchain, the checksum and RNG, and the Messages helpers. It may not name a product, and a test enforces that. See [`shared/README.md`](shared/README.md). |
| [`docs/`](docs) | The two method documents that apply to every product. Each product keeps its own docs in its own folder. |
| [`.github/workflows/`](.github/workflows) | One workflow per lane. Each runs from its product's folder and triggers only on that folder and `shared/`. |

## Working in it

Run commands from inside the product's folder, for example `cd foolish && npm run test:validate`.
Every path in a product's own documentation and comments is written from that folder.
Inside `foolish/`, a path that starts with `shared/` means the folder beside it, reached as `../shared/`.

The wasm and generator toolchain is pinned in [`shared/scripts/ci_llvm.sh`](shared/scripts/ci_llvm.sh), and every product's CI installs it from there.
