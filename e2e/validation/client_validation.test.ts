// Fast validation runner for the client logic. The scenarios live in their domain
// files (client.test.ts, optimistic_animation.test.ts); this runner imports and
// executes them. Pure — no Postgres.
import { registerClientValidation } from '../client.test.ts';
import { registerOptimisticValidation } from '../optimistic_animation.test.ts';

registerClientValidation();
registerOptimisticValidation();
