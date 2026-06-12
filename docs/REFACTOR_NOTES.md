# Pending work / notes

## (Later — after cordite) Unify the game-state → render pipeline

Requested architecture (paraphrased from the user):

1. **One game-state consumer/interface** — the same shape for every source of
   game state.
2. **Multiple implementations of that interface** — `live` (server channel),
   `replay` (decoded URL), `tutorial` (scripted replay). Today these are three
   separate `ServerProvider` variants + ad-hoc wiring.
3. **A consumer-of-the-consumer = the renderer** — the actual buttons,
   positions, cards, hands, deck, discard, etc., reading only from the
   interface (not from a specific source).
4. **The final React layout chooses which pieces to show per screen** — e.g.
   replay reveals opponents' hands; the live game does not. But the layout
   should be *mostly shared*: tutorial ≈ live ≈ replay. There are more
   similarities than differences.

Current state (starting point for the refactor):
- `ServerContext` has `ServerProvider` (live) and `ReplayServerProvider`
  (replay + now tutorial). Both expose the same `useServer()` surface, so that
  is effectively the "interface" already — but it carries server-action methods
  that only the live impl implements.
- Display components (`GameDisplay/*`) already read only `useServer().game` +
  `useAnimation()` + `useAuth()` + `useGame()` + `useDrag()`. They are the
  shared renderer. `GameDisplay`, `ReplayScreen`'s `ReplayStage`, and
  `Tutorial`'s `TutorialBoard` each re-compose those pieces differently — this
  is the duplication to collapse.
- The tutorial added: `AnimationContext` override (action methods drive a
  scripted step), `TutorialHintContext` (optional green hints the shared
  `ActionButtons`/`TableBattles` read), and `localHandOrder` in
  `ReplayServerProvider`.

Refactor sketch:
- Define a single `GameStateSource` interface (the read surface the renderer
  needs) + an optional `GameActions` surface (attack/cover/.../advance) that
  each impl provides (server call, or scripted-step advance, or no-op).
- One `GameBoard` layout component parameterized by capabilities/flags
  (e.g. `revealAllHands`, `interactive`, `selfSeat`, `hint`) instead of three
  bespoke boards.
- Live / replay / tutorial become thin wrappers that pick the source impl and
  the flags.
