/* =============================================================================
 * planFlights, held to the flights the overlay's own effect built
 * =============================================================================
 * src/components/GameDisplay/flightGeometry.ts planFlights is the body of
 * AnimationOverlay's layout effect, lifted out of the component: one step, the
 * board and the row on screen in, the flights to draw out. Every case here was
 * first played through the component AS IT WAS before the lift - the overlay
 * mounted over the same page, its contexts answering with the same step and
 * board - and the AnimatedCard[] it handed FlightCard was written down below.
 * The lifted function has to give the same list, card for card, to the pixel.
 *
 * The page is a hand-written one: only the elements a flight looks up, each
 * carrying the rect it answers with (data-rect="left,top,width,height"). jsdom
 * has no layout, so an element with no rect of its own - the placeholders
 * planFlights inserts to measure a slot - answers by its place among its
 * siblings, which is exactly what a placeholder is measured for.
 *
 * THE CASES RUN IN ORDER AND SHARE THE MODULE'S MEMORY, as the page does: the
 * revert near the end leaves from the spot an earlier case's flight landed on
 * (tableLandings), and that is the point of it.
 *
 * e2e/ui_animation_trace.test.ts holds the same code through the whole page,
 * frame by frame; this file is the short one that says WHICH flight changed.
 * ========================================================================== */

import { test, mock, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';
import { fakeBackend } from './helpers/fake_backend.ts';
import { NO_CARD, type TableView, type ViewBattle, type ViewCard } from '../src/state/view.ts';
import type { ClientAnimationEvent } from '../src/state/animationStep.ts';
import type { AnimatedCard } from '../src/components/GameDisplay/FlightCard.tsx';

// ---- jsdom -------------------------------------------------------------------------
const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
for (const k of ['window', 'document', 'HTMLElement', 'Node', 'Element', 'getComputedStyle']) {
    try { g[k] = (dom.window as any)[k]; } catch { /* a getter already there */ }
}
Object.defineProperty(dom.window, 'innerWidth', { value: 1280, configurable: true });
Object.defineProperty(dom.window, 'innerHeight', { value: 800, configurable: true });
// An element's own rect, or - for one that carries none - a 60x80 box 70px
// along per sibling before it.
(dom.window.Element.prototype as any).getBoundingClientRect = function rect(this: Element) {
    const own = this.getAttribute('data-rect');
    const index = this.parentElement ? Array.prototype.indexOf.call(this.parentElement.children, this) : 0;
    const [left, top, width, height] = own ? own.split(',').map(Number) : [500 + 70 * index, 300, 60, 80];
    return { x: left, y: top, left, top, width, height, right: left + width, bottom: top + height };
};

// The page's network door: nothing here uses it, but the grid's module (whose
// cover angle and glide planFlights reads) reaches it through its contexts.
mock.module('../src/common/backend.ts', { namedExports: fakeBackend().module });

// The overlay's own word that a place was missing (stockPlace). No case here
// takes a place away, so none may be said.
const misses: string[] = [];
const realError = console.error;
console.error = (...a: unknown[]) => {
    if (typeof a[0] === 'string' && a[0].startsWith('AnimationOverlay: no ')) misses.push(a[0]);
    else realError(...a);
};
after(() => { console.error = realError; });

// A flight's id carries the clock; the clock stands still.
const NOW = 1000;
const realNow = Date.now;
Date.now = () => NOW;
after(() => { Date.now = realNow; });

// The kernel: a hand slot (hand_layout) and the cover guess (can_cover) are its.
before(async () => {
    const { ensureBotsAsync } = await import('../sdk/ts/wasm/bots.ts');
    await ensureBotsAsync();
});

// ---- the page ----------------------------------------------------------------------
const ME = 'u-me', OPP = 'u-opp';
const c = (suit: number, value: number): ViewCard => ({ suit, value });
const open = (attack: ViewCard): ViewBattle => ({ attack, defense: NO_CARD });
/** The board on screen: two seats, hearts-as-2 trump, and the row given. */
const board = (battles: ViewBattle[]): TableView =>
    ({ seats: [{ id: ME }, { id: OPP }], powerSuit: 2, battles }) as unknown as TableView;

const DECK = '<div data-location="deck" data-rect="40,90,50,70"></div>';
const FLIPPED = '<div data-location="flipped" data-rect="40,170,70,50"></div>';
const DISCARD = '<div data-location="discard" data-rect="1100,90,50,70"></div>';
const OPP_HAND = `<div data-location="hand" data-player-id="${OPP}" data-rect="600,20,120,40"></div>`;
/** My hand as the page draws it: a plane the kernel's slots are measured in, and my cards on it. */
const myHand = (cards: ViewCard[]): string =>
    `<div data-hand-container data-player-id="${ME}"><div data-hand-plane data-rect="340,700,600,70">${cards.map((k, i) =>
        `<div data-location="hand" data-player-id="${ME}" data-card="${k.suit}-${k.value}" data-card-index="${i}" data-rect="${400 + 52 * i},700,50,70"></div>`).join('')}</div></div>`;
/** One battle cell of the grid, at its slot, with the cards lying in it. */
const cell = (index: number, attack: ViewCard, defense?: ViewCard, style = ''): string =>
    `<div data-location="table" data-rect="${500 + 70 * index},310,60,80"${style ? ` style="${style}"` : ''}>`
    + `<div data-battle-index="${index}" data-card="${attack.suit}-${attack.value}" data-rect="${505 + 70 * index},315,50,70"></div>`
    + (defense ? `<div data-card="${defense.suit}-${defense.value}" data-rect="${512 + 70 * index},312,54,74"></div>` : '')
    + '</div>';
const table = (...cells: string[]): string => `<div data-table-container data-rect="490,300,300,200">${cells.join('')}</div>`;

// ---- the flights -------------------------------------------------------------------
/** A flight less the step it flies for, which is held by identity instead. */
type Pinned = Omit<AnimatedCard, 'flight'>;

async function plan(step: ClientAnimationEvent, game: TableView, shown: readonly ViewBattle[]): Promise<AnimatedCard[] | null> {
    const { planFlights } = await import('../src/components/GameDisplay/flightGeometry.ts');
    return planFlights(step, game, shown);
}

async function holds(html: string, step: ClientAnimationEvent, game: TableView, shown: readonly ViewBattle[], want: Pinned[] | null): Promise<void> {
    dom.window.document.body.innerHTML = html;
    const got = await plan(step, game, shown);
    if (want === null || got === null) {
        assert.equal(got, want, 'a step that flies nothing answers null');
        return;
    }
    assert.deepEqual(got.map(({ flight: _flight, ...rest }) => rest), want, 'the flights are the ones the overlay built before the lift');
    for (const f of got) assert.equal(f.flight, step, 'every flight is tied to the step it flies for, by identity');
    assert.equal(dom.window.document.querySelectorAll('[data-placeholder]').length, 0, 'no measuring placeholder is left in the page');
    assert.deepEqual(misses, [], 'no place a card leaves or lands on was missing');
}

test('my attack lands on the cell the grid has already laid for it', async () => {
    const row = [open(c(1, 9))];
    await holds(myHand([c(1, 9), c(0, 6)]) + table(cell(0, c(1, 9))),
        { type: 'attack_pass', seat: 0, cards: [c(1, 9)], from_location: 'hand', to_location: 'table' },
        board([]), row, WANT.attackLaid);
});

test('an attack the grid has no cell for yet is aimed at measured slots, after the piles already down', async () => {
    const row = [open(c(0, 6))];
    await holds(OPP_HAND + table(cell(0, c(0, 6))),
        { type: 'attack_pass', seat: 1, cards: [c(0, 7), c(3, 7)], from_location: 'hand', to_location: 'table' },
        board(row), row, WANT.attackMeasured);
});

test('a cover the kernel names a slot for comes down laid across that attack, where its gliding cell will rest', async () => {
    const row = [open(c(1, 9))];
    await holds(myHand([c(2, 10)]) + table(cell(0, c(1, 9), undefined, 'transform: translate(12px, 4px)')),
        { type: 'cover', seat: 0, cards: [c(2, 10)], from_location: 'hand', to_location: 'table', target_card: c(1, 9), battle_index: 0 },
        board(row), row, WANT.coverNamed);
});

test('covers that name no attack take the first open pile each can beat, and never the same one twice', async () => {
    const row = [open(c(0, 9)), open(c(1, 9)), { attack: c(3, 9), defense: c(3, 10) }];
    await holds(OPP_HAND + table(cell(0, c(0, 9)), cell(1, c(1, 9)), cell(2, c(3, 9), c(3, 10))),
        { type: 'cover', seat: 1, cards: [c(2, 6), c(2, 7)], from_location: 'hand', to_location: 'table' },
        board(row), row, WANT.coverGuessed);
});

test('a cover with nothing on screen to aim at fans out from the table', async () => {
    const row = [open(c(0, 9))];
    await holds(OPP_HAND + '<div data-location="table" data-rect="500,310,60,80"></div>',
        { type: 'cover', seat: 1, cards: [c(0, 6), c(1, 6)], from_location: 'hand', to_location: 'table' },
        board(row), row, WANT.coverUnplaced);
});

test("another seat's draw is one stack of backs out of the stock and the trump, face up, out of its slot", async () => {
    await holds(DECK + FLIPPED + OPP_HAND,
        { type: 'refill', seat: 1, cards: [c(-1, -1), c(-1, -1), c(-1, -1)], from_location: 'deck', to_location: 'hand', trump_out: c(2, 14), trump_from: 'flipped' },
        board([]), [], WANT.drawBacks);
});

test('my own draw flies each card to its slot in my hand, the trump last and from its slot', async () => {
    await holds(DECK + FLIPPED + myHand([c(0, 6)]),
        { type: 'refill', seat: 0, cards: [c(1, 8), c(2, 14)], from_location: 'deck', to_location: 'hand', trump_out: c(2, 14), trump_from: 'flipped' },
        board([]), [], WANT.drawMine);
});

test('a pickup lifts each card off at the tilt the grid drew it with and lands it on its hand slot', async () => {
    const row = [{ attack: c(1, 9), defense: c(2, 10) }, open(c(0, 7))];
    await holds(myHand([c(0, 6), c(3, 8)]) + table(cell(0, c(1, 9), c(2, 10)), cell(1, c(0, 7))),
        { type: 'pickup', seat: 0, cards: [c(1, 9), c(2, 10), c(0, 7)], from_location: 'table', to_location: 'hand' },
        board(row), row, WANT.pickup);
});

test("another seat's pickup lands on its mini-hand, each card a little off the last", async () => {
    const row = [open(c(0, 7)), open(c(1, 7))];
    await holds(OPP_HAND + table(cell(0, c(0, 7)), cell(1, c(1, 7))),
        { type: 'pickup', seat: 1, cards: [c(0, 7), c(1, 7)], from_location: 'table', to_location: 'hand' },
        board(row), row, WANT.pickupTheirs);
});

test('a discard leaves the table for the pile', async () => {
    const row = [{ attack: c(1, 9), defense: c(2, 10) }];
    await holds(DISCARD + table(cell(0, c(1, 9), c(2, 10))),
        { type: 'discard', cards: [c(1, 9), c(2, 10)], from_location: 'table', to_location: 'discard' },
        board(row), row, WANT.discard);
});

test('a refused card the board never laid comes home from where its own flight left it', async () => {
    await holds(`<div data-location="hand" data-player-id="${OPP}" data-rect="600,20,120,40"></div>` + table(),
        { type: 'revert', seat: 1, cards: [c(3, 7)], from_location: 'table', to_location: 'hand', is_revert: true },
        board([]), [], WANT.revert);
});

test('a card turned up as the trump lands in the trump slot', async () => {
    await holds(DECK + FLIPPED,
        { type: 'flipped', cards: [c(2, 14)], from_location: 'deck', to_location: 'flipped' },
        board([]), [], WANT.flipped);
});

test('a message, and a step with no cards, fly nothing', async () => {
    await holds(DECK, { type: 'magic_transition', message: 'x', cards: [c(1, 9)] }, board([]), [], null);
    await holds(DECK, { type: 'pickup', seat: 0, cards: [], from_location: 'table', to_location: 'hand' }, board([]), [], null);
});

// ---- what the overlay built, before the lift ------------------------------------------
const WANT: Record<string, Pinned[]> = {
    attackLaid: [
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0,
            id: '1-9-u-me-1000-0',
            card: { suit: 1, value: 9 },
            startPosition: { x: 425, y: 735 },
            endPosition: { x: 530, y: 350 },
            progress: 0,
            animationType: 'attack_pass',
            playerId: 'u-me',
            isRevert: undefined,
        },
    ],
    attackMeasured: [
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0,
            id: '0-7-u-opp-1000-0',
            card: { suit: 0, value: 7 },
            startPosition: { x: 660, y: 40 },
            endPosition: { x: 600, y: 340 },
            progress: 0,
            animationType: 'attack_pass',
            playerId: 'u-opp',
            isRevert: undefined,
        },
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0,
            id: '3-7-u-opp-1000-1',
            card: { suit: 3, value: 7 },
            startPosition: { x: 660, y: 40 },
            endPosition: { x: 670, y: 340 },
            progress: 0,
            animationType: 'attack_pass',
            playerId: 'u-opp',
            isRevert: undefined,
        },
    ],
    coverNamed: [
        {
            fromLanding: false,
            angle: 0.19634954084936207,
            fromAngle: 0,
            id: '2-10-u-me-1000-0',
            card: { suit: 2, value: 10 },
            startPosition: { x: 425, y: 735 },
            endPosition: { x: 524.8281612705645, y: 346.67251518588694 },
            progress: 0,
            animationType: 'cover',
            playerId: 'u-me',
            isRevert: undefined,
        },
    ],
    coverGuessed: [
        {
            fromLanding: false,
            angle: 0.19634954084936207,
            fromAngle: 0,
            id: '2-6-u-opp-1000-0',
            card: { suit: 2, value: 6 },
            startPosition: { x: 660, y: 40 },
            endPosition: { x: 536.8281612705645, y: 350.67251518588694 },
            progress: 0,
            animationType: 'cover',
            playerId: 'u-opp',
            isRevert: undefined,
        },
        {
            fromLanding: false,
            angle: 0.19634954084936207,
            fromAngle: 0,
            id: '2-7-u-opp-1000-1',
            card: { suit: 2, value: 7 },
            startPosition: { x: 660, y: 40 },
            endPosition: { x: 609.8281612705645, y: 353.67251518588694 },
            progress: 0,
            animationType: 'cover',
            playerId: 'u-opp',
            isRevert: undefined,
        },
    ],
    coverUnplaced: [
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0,
            id: '0-6-u-opp-1000-0',
            card: { suit: 0, value: 6 },
            startPosition: { x: 660, y: 40 },
            endPosition: { x: 530, y: 350 },
            progress: 0,
            animationType: 'cover',
            playerId: 'u-opp',
            isRevert: undefined,
        },
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0,
            id: '1-6-u-opp-1000-1',
            card: { suit: 1, value: 6 },
            startPosition: { x: 660, y: 40 },
            endPosition: { x: 603, y: 353 },
            progress: 0,
            animationType: 'cover',
            playerId: 'u-opp',
            isRevert: undefined,
        },
    ],
    drawBacks: [
        {
            id: 'sanitized-refill-u-opp-1000',
            card: { suit: -1, value: -1 },
            startPosition: { x: 65, y: 125 },
            endPosition: { x: 660, y: 40 },
            progress: 0,
            animationType: 'refill',
            playerId: 'u-opp',
            isSanitizedRefill: true,
            cardCount: 2,
            isRevert: undefined,
        },
        {
            id: '2-14-u-opp-1000-trump',
            card: { suit: 2, value: 14 },
            startPosition: { x: 75, y: 195 },
            endPosition: { x: 660, y: 40 },
            progress: 0,
            animationType: 'refill',
            playerId: 'u-opp',
            isRevert: undefined,
        },
    ],
    drawMine: [
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0,
            id: '1-8-u-me-1000-0',
            card: { suit: 1, value: 8 },
            startPosition: { x: 65, y: 125 },
            endPosition: { x: 640, y: 735 },
            progress: 0,
            animationType: 'refill',
            playerId: 'u-me',
            isRevert: undefined,
        },
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0,
            id: '2-14-u-me-1000-1',
            card: { suit: 2, value: 14 },
            startPosition: { x: 75, y: 195 },
            endPosition: { x: 692, y: 735 },
            progress: 0,
            animationType: 'refill',
            playerId: 'u-me',
            isRevert: undefined,
        },
    ],
    pickup: [
        {
            fromLanding: false,
            angle: 0,
            fromAngle: -0.19634954084936207,
            id: '1-9-u-me-1000-0',
            card: { suit: 1, value: 9 },
            startPosition: { x: 530, y: 350 },
            endPosition: { x: 640, y: 735 },
            progress: 0,
            animationType: 'pickup',
            playerId: 'u-me',
            isRevert: undefined,
        },
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0.19634954084936207,
            id: '2-10-u-me-1000-1',
            card: { suit: 2, value: 10 },
            startPosition: { x: 539, y: 349 },
            endPosition: { x: 692, y: 735 },
            progress: 0,
            animationType: 'pickup',
            playerId: 'u-me',
            isRevert: undefined,
        },
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0,
            id: '0-7-u-me-1000-2',
            card: { suit: 0, value: 7 },
            startPosition: { x: 600, y: 350 },
            endPosition: { x: 744, y: 735 },
            progress: 0,
            animationType: 'pickup',
            playerId: 'u-me',
            isRevert: undefined,
        },
    ],
    pickupTheirs: [
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0,
            id: '0-7-u-opp-1000-0',
            card: { suit: 0, value: 7 },
            startPosition: { x: 530, y: 350 },
            endPosition: { x: 660, y: 40 },
            progress: 0,
            animationType: 'pickup',
            playerId: 'u-opp',
            isRevert: undefined,
        },
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0,
            id: '1-7-u-opp-1000-1',
            card: { suit: 1, value: 7 },
            startPosition: { x: 600, y: 350 },
            endPosition: { x: 663, y: 43 },
            progress: 0,
            animationType: 'pickup',
            playerId: 'u-opp',
            isRevert: undefined,
        },
    ],
    discard: [
        {
            fromLanding: false,
            angle: 0,
            fromAngle: -0.19634954084936207,
            id: '1-9-undefined-1000-0',
            card: { suit: 1, value: 9 },
            startPosition: { x: 530, y: 350 },
            endPosition: { x: 1125, y: 125 },
            progress: 0,
            animationType: 'discard',
            playerId: undefined,
            isRevert: undefined,
        },
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0.19634954084936207,
            id: '2-10-undefined-1000-1',
            card: { suit: 2, value: 10 },
            startPosition: { x: 539, y: 349 },
            endPosition: { x: 1128, y: 128 },
            progress: 0,
            animationType: 'discard',
            playerId: undefined,
            isRevert: undefined,
        },
    ],
    revert: [
        {
            fromLanding: true,
            angle: 0,
            fromAngle: 0,
            id: '3-7-u-opp-1000-0',
            card: { suit: 3, value: 7 },
            startPosition: { x: 670, y: 340 },
            endPosition: { x: 660, y: 40 },
            progress: 0,
            animationType: 'revert',
            playerId: 'u-opp',
            isRevert: true,
        },
    ],
    flipped: [
        {
            fromLanding: false,
            angle: 0,
            fromAngle: 0,
            id: '2-14-undefined-1000-0',
            card: { suit: 2, value: 14 },
            startPosition: { x: 65, y: 125 },
            endPosition: { x: 75, y: 195 },
            progress: 0,
            animationType: 'flipped',
            playerId: undefined,
            isRevert: undefined,
        },
    ],
};
