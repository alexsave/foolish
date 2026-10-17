// Kernel doors only tests open, on the test build (bots_test.wasm, through the
// fixtures' instance in e2e/helpers/table_fixture.ts).
//
// They lived in sdk/ts/wasm/bots.ts while that file could load a whole-kernel
// module in Node (bots.wasm, the old server link). It loads web.wasm everywhere
// now, which does not export these, and no browser path calls them: the browser
// reaches the same C through client_push_settle and the client slot. What the
// tests hold to the kernel here is the C entry itself, so they keep it, on the
// build that exports it.

import * as V from '../../sdk/ts/gen/view_layout.bots.ts';
import type { Card } from '../../sdk/ts/wasm/bots.ts';
import { fixtureExports } from './table_fixture.ts';

const mem = () => V.memOf(fixtureExports().memory.buffer);

function ok(rc: number, refusal: (code: number) => Error): number {
    if (rc < 0) throw refusal(rc);
    return rc;
}
const animError = (entry: string) => (code: number) => new Error(`${entry} error ${code}`);

/**
 * The replay code out of whatever a person pasted - the kernel's
 * replay_link_parse. Building a link and reading one back are two halves of the
 * same format, and this is the half that has to REFUSE: replay_b32_decode is
 * deliberately tolerant, so an unstripped "https" prefix would decode to a
 * different game rather than fail. Throws when the input is not a code.
 */
export function kernelReplayLinkParse(url: string): string {
    const ex = fixtureExports();
    const text = new TextEncoder().encode(url);
    let at = ex.wasm_replay_io_ptr();
    if (text.length + 1 > ex.wasm_replay_io_cap()) throw new Error('link: does not fit the kernel IO buffer');
    const u8 = new Uint8Array(ex.memory.buffer);
    u8.set(text, at);
    u8[at + text.length] = 0;
    const n = ok(ex.wasm_replay_link_parse(text.length), () => new Error(`not a replay code: "${url}"`));
    at = ex.wasm_io_ptr();
    return new TextDecoder().decode(new Uint8Array(ex.memory.buffer).slice(at, at + n));
}

/** anim_plan.h anim_stale_optimistic_ask (the browser asks it inside client_push_settle). Returns the INDICES
 *  into `optCards` to release. */
export function animStaleOptimisticOnTable(optCards: Card[], tableCards: Card[], namedCards: Card[]): number[] {
    const ex = fixtureExports();
    V.writeAnimStaleAsk(mem(), ex.wasm_anim_stale_ask_ptr(), { opt: optCards, table: tableCards, named: namedCards });
    ok(ex.wasm_anim_stale_optimistic(), animError('anim_stale_optimistic'));
    return [...V.readAnimStaleRelease(mem(), ex.wasm_anim_stale_release_ptr()).release];
}

/**
 * THE ORDER A DOOMED SEQUENCE FLIES HOME IN (anim_plan.h anim_reversal_order),
 * for a caller that already holds its verdicts.
 *
 * `verdicts` are ANIM_CONFLICT_* per motion in the order the motions flew, and
 * `groupSizes` slices them into the parallel steps they flew as. The answer is
 * the steps to play, each a list of motion indices, in REVERSE group order: the
 * cards travel back the way they came, last group first, and a group nothing
 * reverts is dropped rather than played as a beat of silence.
 */
export function animReversalOrder(
    verdicts: readonly number[], groupSizes: readonly number[],
): number[][] {
    const ex = fixtureExports();
    V.writeAnimReversalAsk(mem(), ex.wasm_anim_reversal_ask_ptr(), { verdicts, groupSizes });
    ok(ex.wasm_anim_reversal_order(), animError('anim_reversal_order'));
    const plan = V.readAnimConflictPlan(mem(), ex.wasm_anim_reversal_plan_ptr());
    const steps: number[][] = [];
    let at = 0;
    for (const c of plan.stepCount) steps.push(plan.order.slice(at, at += c));
    return steps;
}
