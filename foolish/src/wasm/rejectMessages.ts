// What the action path's refusal of a move says, for a console line. Nothing
// renders these: the client logs one and reverts its optimistic state.
//
// The codes and the sentences are both the kernel's (c/errtext/error_text.h,
// MOVE_REJECT_TEXT, which tools/datagen writes out as the table read here): the
// rules' ENGINE_REJECT_*, the edge's stale-round refusal, and a wire that is
// not a move. Only the fallback for a code that table does not hold is this
// file's, and e2e/validation/error_text_validation.test.ts holds every
// ENGINE_REJECT_* to a row, so it is reached by a code no build here knows.
import { MoveRejectText } from '@sdk/ts/gen/errtext/move_reject.ts';

export function rejectMessage(code: number): string {
    return MoveRejectText.find((row) => row.code === code)?.text ?? `move rejected (code ${code})`;
}
