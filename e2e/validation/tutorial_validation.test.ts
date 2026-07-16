// Fast validation runner for the tutorial's frozen game. The scenarios live in
// the domain file (e2e/tutorial_game.test.ts); this runner imports and executes
// them. Pure — no Postgres.
//
// The tutorial ships a replay code baked into the client, and a code is only
// readable by the kernel that cut it: the coder's probability model IS the
// legal-move menu, so any menu change renumbers every choice and orphans it.
// That has happened twice. A menu change that strands the tutorial fails HERE,
// in seconds — instead of shipping a tutorial that no longer opens.
import { registerTutorialValidation } from '../tutorial_game.test.ts';

registerTutorialValidation();
