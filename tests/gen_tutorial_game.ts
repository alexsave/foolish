/* =============================================================================
 * Cut the tutorial's scripted game (src/components/tutorialGame.ts)
 * =============================================================================
 *   npx tsx tests/gen_tutorial_game.ts [--seeds N]
 *
 * The tutorial is a real 3-player game played by the real engine and frozen as a
 * replay code. It has to be a particular KIND of real game: one where the
 * learner (seat 0) personally performs every move the tutorial teaches — lead,
 * throw in, cover, trump-cover, pass, pick up, say good — and where the table
 * shows the rest: a bout closing, refills, the stock running out, a player going
 * out, and a fool who is not the learner.
 *
 * So this searches seeds for such a game. That is all a "generator" can do here:
 * the elements have to co-occur naturally, because the game is played by the
 * engine, not scripted.
 *
 * It scores the REPLAY'S STEPS — what the tutorial will actually walk the
 * learner through — and not the played game's log stream, which is a different
 * thing and lies about exactly the elements that matter. Two of them, concretely:
 *
 *   - The log stream records a `good` for every attacker at every bout end. The
 *     replay does not: v6 keeps only a trailing good, and a good that CLOSES a
 *     bout is folded into a seat-less round end (replay.c apply_round_end emits
 *     seat -1, because the transition belongs to all of them). So a game whose
 *     logs are full of the learner's goods can be one where the learner is never
 *     once asked to say good — which is what the first cut of this file picked.
 *
 *   - `game.first_attacker` is the CURRENT attacker of a finished game, not the
 *     opening one. Reading it here reported "the learner leads" about a game
 *     that opens on seat 1.
 *
 * The predecessor of this file was lost, and the tutorial's code was left as a
 * base32 constant nobody could regenerate — which matters more than it sounds,
 * because a replay code is only readable by the kernel that cut it (the coder's
 * probability model IS the legal-move menu, so any menu change renumbers every
 * choice and orphans the code). When that happens, this is the way back in.
 * ========================================================================== */

import { PLAYER_STATUS } from '../server/api/core/types.ts';
import { playSeededV6 } from '../e2e/helpers/seeded_game.ts';
import { base32Encode, bytesToBigint } from '../server/api/common/replay/codec.ts';
import { decodeReplay } from '../server/api/common/replay/decode.ts';
import { buildReplayFrames, REPLAY_STEP, ReplayFrame } from '../src/replay/frames.ts';
import { TUTORIAL_NAMES } from '../src/components/tutorialGame.ts';

const LEARNER = 0;
const SELF_ID = 'seat-0';

/* Mirrors Tutorial.tsx: a bout-closing good is seat-less, so the learner's own
 * good is recognised from the board — they were in, not defending, and had not
 * spoken, so the good the table waits on is theirs. */
const learnerOwesGood = (prev: ReplayFrame | undefined): boolean => {
    if (!prev) return false;
    const me = prev.game.players[LEARNER];
    return !!me && me.status !== PLAYER_STATUS.OUT
        && prev.game.defender !== LEARNER
        && !prev.game.good_players.includes(SELF_ID);
};

interface Elements {
    leads: boolean;        // the learner opens the game (lowest trump)
    attack: boolean;
    throwIn: boolean;      // the learner attacks onto a non-empty table
    cover: boolean;
    trumpCover: boolean;   // the learner covers a plain card with a trump
    pass: boolean;
    pickup: boolean;
    good: boolean;         // the learner is ASKED to say good
    roundEnd: boolean;
    draw: boolean;
    deckEmpty: boolean;
    someoneOut: boolean;
    foolIsNotLearner: boolean;
}

const REQUIRED: (keyof Elements)[] = [
    'leads', 'attack', 'throwIn', 'cover', 'trumpCover', 'pass', 'pickup',
    'good', 'roundEnd', 'draw', 'deckEmpty', 'someoneOut', 'foolIsNotLearner',
];

function elementsOf(frames: ReplayFrame[], trump: number, firstAttacker: number, fool: number): Elements {
    const e: Elements = {
        leads: firstAttacker === LEARNER, attack: false, throwIn: false, cover: false,
        trumpCover: false, pass: false, pickup: false, good: false, roundEnd: false,
        draw: false, deckEmpty: false, someoneOut: false, foolIsNotLearner: fool !== LEARNER,
    };
    frames.forEach((f, i) => {
        const prev = frames[i - 1];
        const mine = f.seat === LEARNER;
        switch (f.kind) {
            case REPLAY_STEP.ATTACK:
                if (mine) {
                    e.attack = true;
                    if (prev && prev.game.table_battles.length > 0) e.throwIn = true;
                }
                break;
            case REPLAY_STEP.COVER:
                if (mine) {
                    e.cover = true;
                    if (f.cards[0]?.suit === trump && f.target && f.target.suit !== trump) {
                        e.trumpCover = true;
                    }
                }
                break;
            case REPLAY_STEP.PASS: if (mine) e.pass = true; break;
            case REPLAY_STEP.PICKUP: if (mine) e.pickup = true; break;
            case REPLAY_STEP.GOOD: if (mine) e.good = true; break;
            case REPLAY_STEP.ROUND_END:
                e.roundEnd = true;
                if (learnerOwesGood(prev)) e.good = true;
                break;
        }
        if (f.seq.events.some((ev) => ev.type === 'refill')) e.draw = true;
        if (f.seq.events.some((ev) => ev.type === 'out')) e.someoneOut = true;
        if (f.game.deck_length === 0 && f.game.flipped === null) e.deckEmpty = true;
    });
    return e;
}

const score = (e: Elements) => REQUIRED.filter((k) => e[k]).length;

async function main() {
    const nArg = process.argv.indexOf('--seeds');
    const SEEDS = nArg > 0 ? Number(process.argv[nArg + 1]) : 1500;

    let best: { s: number; e: Elements; code: Uint8Array; frames: ReplayFrame[] } | null = null;
    let complete = 0;

    for (let s = 0; s < SEEDS; s++) {
        const played = await playSeededV6(3, s);
        if (!played) continue;
        let frames: ReplayFrame[];
        let decoded;
        try {
            decoded = await decodeReplay(bytesToBigint(played.code));
            frames = buildReplayFrames(played.code, 'tutorial', TUTORIAL_NAMES, { viewer: LEARNER });
        } catch {
            continue;   // a code the tutorial could not replay is no use to it
        }
        const e = elementsOf(frames, decoded.powerSuit, decoded.firstAttacker, decoded.fool);
        const sc = score(e);
        if (sc === REQUIRED.length) complete++;
        if (!best || sc > score(best.e)
            // Among equally complete games, prefer the shorter one: a tutorial
            // that teaches everything in fewer moves is a better tutorial.
            || (sc === score(best.e) && frames.length < best.frames.length)) {
            best = { s, e, code: played.code, frames };
        }
    }

    if (!best) { console.error('no game finished'); process.exit(1); }
    const missing = REQUIRED.filter((k) => !best!.e[k]);
    const learnerSteps = best.frames.filter((f, i) =>
        (f.seat === LEARNER && f.kind !== REPLAY_STEP.DEAL)
        || (f.kind === REPLAY_STEP.ROUND_END && learnerOwesGood(best!.frames[i - 1]))).length;

    console.log(`searched ${SEEDS} seeds; ${complete} taught every element`);
    console.log(`best: seed ${best.s} — ${score(best.e)}/${REQUIRED.length} elements, ${best.frames.length} steps`);
    if (missing.length) console.log(`MISSING: ${missing.join(', ')}`);
    console.log(`the learner acts on ${learnerSteps} steps`);
    console.log(`\ncode: ${base32Encode(best.code)}`);
}

main();
