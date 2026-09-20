// Fast validation runner for the web's pass rules (the optimistic pass's shield and
// the pass gate). The scenarios live in the domain file
// (e2e/client_pass_rules.test.ts); this runner imports and executes them.
// Pure - no Postgres.
import { registerClientRulesValidation } from '../client_pass_rules.test.ts';

registerClientRulesValidation();
