// Fast validation runner for PASS. The scenarios themselves live in the pass
// domain file (e2e/pass_parity.test.ts), alongside the fuzzer; this runner just
// imports and executes the handpicked deterministic cases. Run with
// VALIDATION_ONLY=1 so importing the domain file does NOT register its fuzzer.
import { registerPassValidation } from '../pass_parity.test.ts';

registerPassValidation();
