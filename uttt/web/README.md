# uttt.live

Ultimate Tic-Tac-Toe's replay site. The end screen's **Copy code** puts
`https://uttt.live/<code>` on the pasteboard (`uttt_replay_url`,
`uttt/c/src/uttt_code.h`), and this site plays that game back: back a move,
play or pause, forward a move. No accounts and no database: the whole game,
and the seed its napkin was drawn with, is in the code.

**The page draws nothing itself.** The board, the motion and the line under
it are the uttt kernel compiled to wasm (`uttt/c/wasm/uttt_web.c`,
`make -C uttt/c wasm-web`), the same polygons the app fills. `lib/kernel.ts`
knows the kernel's function names and nothing else; `components/Replay.tsx`
owns the clock, the canvas and three buttons.

```
cd uttt/web
npm install
npm run dev        # builds public/uttt.wasm first (needs clang with wasm32 + wasm-ld)
open http://localhost:3000/AAA6ER7HDME5QRDFPW6GBSAGZCB22ST4AQ
```

`public/uttt.wasm` is a build output and is never committed. CI builds it
with the repo's pinned clang (`.github/workflows/uttt-web.yml`) and deploys
the site to its own Vercel project on every push to `main` that touches it.

## One-time setup (the owner)

1. **Create the Vercel project**, from a checkout, not from the dashboard's
   Git import (a Git-connected project would build on Vercel, which has no
   clang):

   ```
   cd uttt/web
   npx vercel@latest link      # "Link to existing project?" No -> name it uttt, directory ./
   cat .vercel/project.json    # the projectId
   ```

   Use the same team as the foolish project, so the existing `VERCEL_TOKEN`
   and `VERCEL_ORG_ID` secrets cover it.

2. **Add the secret**: GitHub, the repo, Settings, Secrets and variables,
   Actions, New repository secret: `UTTT_VERCEL_PROJECT_ID` = that projectId.

3. **Deploy**: merge to `main`, or Actions, uttt-web, Run workflow on `main`.

4. **The domain, in Vercel**: the uttt project, Settings, Domains, add
   `uttt.live`, and add `www.uttt.live` set to redirect to `uttt.live`.
   Vercel then lists the DNS records it wants.

5. **The domain, in Squarespace**: Domains, `uttt.live`, DNS (DNS Settings).
   If the domain is connected to a Squarespace site, disconnect it first.
   Delete the **Squarespace Defaults** records, then add the records Vercel
   listed - usually an `A` record for `@` to `76.76.21.21` and a `CNAME` for
   `www` to `cname.vercel-dns.com`, but use Vercel's values if they differ.
   Vercel's Domains page turns green once DNS has propagated (minutes to a
   few hours).
