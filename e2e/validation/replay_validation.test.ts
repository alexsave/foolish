// Fast validation runner for the replay codec. The scenarios live in the domain
// file (replay_codec.test.ts); this runner imports and executes them. Pure — no
// Postgres. Run with VALIDATION_ONLY=1 so the 140-game sweep is not registered.
import { registerReplayValidation } from '../replay_codec.test.ts';

registerReplayValidation();
