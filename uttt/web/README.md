# uttt.live

Ultimate Tic-Tac-Toe's replay site. The end screen's **Copy code** puts
`https://uttt.live/<code>` on the pasteboard (`uttt_replay_url`,
`uttt/c/src/uttt_code.h`), and this site plays that game back: back a move,
play or pause, forward a move. No accounts and no database: the whole game,
and the look its napkin was drawn with, is in the code: base32 of a version
byte (2), the look and the moves. A link 1.0(8) wrote (no version) is dead
and shows the error page.

**The page draws nothing itself.** The board, the motion and the line under
it are the uttt kernel compiled to wasm (`uttt/c/wasm/uttt_web.c`,
`make -C uttt/c wasm-web`), the same polygons the app fills. `lib/kernel.ts`
knows the kernel's function names and nothing else; `components/Replay.tsx`
owns the clock, the canvas and three buttons.

```
cd uttt/web
npm install
npm run dev        # builds public/uttt.wasm first (needs clang with wasm32 + wasm-ld)
open http://localhost:3000/AJG7NYNQG4RFXXGVPGD7QR2ETSOYTWMCREAQ
```

`public/uttt.wasm` is a build output and is never committed. CI builds it
with the repo's pinned clang, and every push to `main` that touches the site
deploys it with `.github/workflows/web_aws.yml`, as a static export (`out/`)
to S3 behind CloudFront (`c_base/ops/web.yaml`), which serves `uttt.live` and
`www.uttt.live`.

## /243: two bots on the 243 x 243 board

`uttt.live/243` (and `/234`, redirected) plays the recursive game at depth 5
between two greedy lookahead bots (`uttt/c/src/uttt_big_bot.h`), in the
visitor's browser, as fast as it goes. One seed per page load, from
`crypto.getRandomValues`, drives both bots; `/243?seed=<16 hex digits>`
replays a named game move for move. The bots are their own wasm module
(`uttt/c/wasm/uttt_243_web.c`, `make -C uttt/c wasm-243`, written to
`public/uttt243.wasm`); the structs the page reads come through readers
`shared/tools/structgen` writes into `lib/gen/` (a libclang program: Homebrew's
`llvm` on a Mac). Both are build outputs of `npm run wasm`. `npm test` loads
`lib/arena.ts` against the built module.

## Hosting

The site's AWS side (the `cbase-web` stack's bucket prefix `uttt/`, its own
CloudFront distribution, the certificate and the Squarespace DNS records) is
set up once by the owner, as `c_base/RUNBOOK.md` "Web hosting: foolish.cards
and uttt.live off Vercel" records; after that a push to `main` is the deploy.
