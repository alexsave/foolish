// Fast validation runner for the client hand-rolled-logic fixes (next-defender
// rotation and keyboard pass parity). The scenarios live in the domain file
// (e2e/client_rules_parity.test.ts); this runner imports and executes them.
// Pure — no Postgres.
import { registerClientRulesValidation } from '../client_rules_parity.test.ts';

registerClientRulesValidation();
