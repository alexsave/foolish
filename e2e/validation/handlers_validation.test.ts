// Fast validation runner for the pure server-action handlers. The scenarios live
// in their domain files (cover.test.ts, rearrange.test.ts, fuzz.test.ts); this
// runner imports and executes the handpicked deterministic cases. Run with
// VALIDATION_ONLY=1 so importing those files does NOT register their heavy/DB tests.
import { registerCoverValidation } from '../cover.test.ts';
import { registerRearrangeValidation } from '../rearrange.test.ts';
import { registerAttackValidation } from '../fuzz.test.ts';

registerCoverValidation();
registerRearrangeValidation();
registerAttackValidation();
