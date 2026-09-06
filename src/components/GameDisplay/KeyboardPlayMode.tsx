/* =============================================================================
 * KeyboardPlayMode — arrow-key driver for playing moves
 * =============================================================================
 * A second input path alongside taps/drag and the letter/number shortcuts in
 * KeyboardInputHandler. It depends only on the shared read surface (useServer
 * for the game + hand, useAnimation for the action methods, useAuth for "me")
 * and on the data-attributes the render pieces already expose:
 *   - hand cards:    [data-location="hand"][data-player-id=<me>][data-card="s-v"]
 *   - table attacks: [data-battle-index=i]
 * so it needs no changes to ActionButtons / TableBattles — it draws its own
 * overlay (a red cursor underline + a cover/pass arrow). Mounted by GameBoard
 * wherever the board is `interactive` (live + tutorial). The replay screen is
 * NOT interactive, so its arrow-key transport is never hijacked.
 *
 * CONTROLS
 *   left/right (nothing selected) -> select the leftmost card
 *   left/right (card selected)    -> move the cursor between hand cards
 *   down  (defender)              -> pick up (if the table is non-empty)
 *   down  (attacker)              -> good   (if the table is fully covered)
 *       down works with or without an active cursor (neither needs a card).
 *   up    (attacker)              -> attack with the cursor card (if legal)
 *   up    (defender):
 *       can cover exactly one & cannot pass  -> cover it immediately
 *       can pass but cannot cover            -> pass immediately
 *       can cover several (or cover AND pass)-> TARGET mode: an arrow points the
 *           cursor card at an attack; left/right moves it between the coverable
 *           attacks; if the card can also pass, one step right past the last
 *           attack points the arrow at an empty space (= pass); up executes.
 *   down (in target mode)         -> cancel back to card selection
 *
 *   shift (press)                 -> toggle the cursor card in/out of the shared
 *       selectedCards set (identical to clicking it; reuses selected styling).
 *       The cursor itself is unaffected, so you keep navigating after selecting.
 *   shift + left/right (HOLD)     -> sweep-select: move the cursor and toggle
 *       each card it lands on (already-selected -> deselected), matching the OS
 *       "Shift extends the selection" gesture. Combined with the press-toggle
 *       above, holding Shift over a card then arrowing right selects that card
 *       AND each one swept onto.
 *   meta/cmd + left/right         -> REARRANGE: drag the cursor card sideways in
 *       the hand (cursor follows it). Reserved for reordering — never plays.
 *   up (selectedCards non-empty)  -> commit a move from the SET, like the
 *       attack/pass/cover buttons: attacker -> attack; defender -> unambiguous
 *       cover else pass. Cleared on success; ambiguous covers are a no-op.
 * ========================================================================== */

import React, { useCallback, useEffect, useRef, useState } from 'react';
import { Card, PersonalGame } from '@api/core/types.ts';
import { useServer } from '../../contexts/ServerContext';
import { useAnimation } from '../../contexts/AnimationContext';
import { useAuth } from '../../contexts/AuthContext';
import { useGame } from '../../contexts/GameContext';
import { canCoverPair } from '../../wasm/clientGuards';
import { canAttack, canPass, canCoverCards } from '../../utils/gameValidation';
import { kernelUnambiguousCover } from '@sdk/ts/wasm/bots.ts';

type CoverTarget = { kind: 'cover'; attack: Card; battleIndex: number };
type Target = CoverTarget | { kind: 'pass' };

interface Rect { x: number; y: number; w: number; h: number; }

const cardKey = (c: Card) => `${c.suit}-${c.value}`;

const rectOf = (el: Element | null): Rect | null => {
    if (!el) return null;
    const r = el.getBoundingClientRect();
    if (r.width === 0 && r.height === 0) return null;
    return { x: r.left, y: r.top, w: r.width, h: r.height };
};

const isTypingTarget = () => {
    const a = document.activeElement;
    if (!a) return false;
    return (
        a.tagName === 'INPUT' ||
        a.tagName === 'TEXTAREA' ||
        a.getAttribute('contenteditable') === 'true' ||
        a.closest('[data-chat-scrollable]') !== null
    );
};

export const KeyboardPlayMode = () => {
    const { game: rawGame, localHandOrder, setLocalHandOrder } = useServer();
    const game = rawGame as PersonalGame | null;
    const { attack, cover, pass, pickup, good } = useAnimation();
    const { user_id } = useAuth();
    const { selectedCards, setSelectedCards, handleCardSelection, setActionPressed } = useGame();

    const hand: Card[] = localHandOrder && localHandOrder.length ? localHandOrder : (game?.self?.hand ?? []);

    const [selIdx, setSelIdx] = useState<number | null>(null);
    const [target, setTarget] = useState<{ targets: Target[]; idx: number } | null>(null);

    // latest of everything the key handler needs, so the listener stays
    // installed once instead of re-subscribing on every state change
    const ref = useRef<any>({});

    // clamp / clear the cursor when the hand changes under it: keep the same
    // index (so the cursor stays put across a move), shift it left to the last
    // card if what it sat on got played off the end, and only turn it off once
    // the hand is empty.
    useEffect(() => {
        if (selIdx !== null && selIdx >= hand.length) setSelIdx(hand.length ? hand.length - 1 : null);
    }, [hand.length, selIdx]);

    // After a move we only leave the cover/pass sub-mode; the CURSOR is kept
    // (the clamp effect above shifts/clears it as the hand shrinks) so you don't
    // have to re-aim from scratch after every play.
    const afterMove = useCallback(() => { setTarget(null); }, []);

    // Fire a move and mirror the on-screen buttons: optimistically hide the
    // action's button (shared pressedActions flag) the instant the key is hit,
    // optionally clear the multi-card selection on success, and un-hide the
    // button if the move fails. Replaces the old run()/withClear()/runGood().
    const fire = useCallback((action: string, p: Promise<unknown>, clearSel = false) => {
        setActionPressed(action, true);
        const done = clearSel ? p.then((v) => { setSelectedCards([]); return v; }) : p;
        done.then(afterMove).catch((e) => {
            console.error(`[kbd] ${action} failed:`, e?.message ?? e);
            setActionPressed(action, false);
            afterMove();
        });
    }, [afterMove, setActionPressed, setSelectedCards]);

    ref.current = {
        game, hand, user_id, selIdx, target,
        attack, cover, pass, pickup, good, fire,
        selectedCards, setSelectedCards, handleCardSelection,
        setLocalHandOrder,
    };

    /* ------------------------------- key logic ----------------------------- */
    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            const k = e.key;
            if (isTypingTarget()) return;

            // Shift down: immediately toggle the cursored card into/out of the
            // shared selection (like shift-clicking to anchor a range). With no
            // active cursor it's a no-op; holding Shift and arrowing (below)
            // extends the selection from here, so the card under the cursor when
            // Shift was pressed is included.
            if (k === 'Shift') {
                if (e.ctrlKey || e.altKey || e.metaKey) return;
                const s = ref.current;
                const g: PersonalGame | null = s.game;
                if (!g || !g.self) return;
                const h: Card[] = s.hand;
                const sel: number | null = s.selIdx;
                if (sel == null || !h[sel] || s.target) return;
                e.preventDefault();
                s.handleCardSelection(h[sel]);
                return;
            }

            if (k !== 'ArrowLeft' && k !== 'ArrowRight' && k !== 'ArrowUp' && k !== 'ArrowDown') return;

            // Shift + left/right: SWEEP-SELECT — walk the cursor and toggle each
            // card it lands on into the shared selection (already-selected ->
            // deselected). Matches the OS "Shift extends the selection" gesture:
            // hold Shift, right, right, right grabs three cards in one motion.
            if (e.shiftKey && !e.ctrlKey && !e.altKey && !e.metaKey && (k === 'ArrowLeft' || k === 'ArrowRight')) {
                e.preventDefault();
                const s = ref.current;
                const g: PersonalGame | null = s.game;
                if (!g || !g.self) return;
                const h: Card[] = s.hand;
                if (!h.length) return;
                if (s.target) return; // not while in the cover/pass sub-mode
                const sel: number | null = s.selIdx;
                const ni = sel == null
                    ? 0
                    : k === 'ArrowLeft' ? (sel - 1 + h.length) % h.length : (sel + 1) % h.length;
                setSelIdx(ni);
                if (h[ni]) s.handleCardSelection(h[ni]);
                return;
            }

            // Cmd/Meta + left/right: REARRANGE — drag the cursored card sideways
            // in the hand (cursor follows it), persisted via the same
            // localHandOrder the mouse drag uses. Never plays a card; Cmd+up/down
            // are inert.
            if (e.metaKey && !e.ctrlKey && !e.altKey && (k === 'ArrowLeft' || k === 'ArrowRight')) {
                e.preventDefault();
                const s = ref.current;
                const h: Card[] = s.hand;
                const sel: number | null = s.selIdx;
                if (sel == null || !h[sel]) return;
                const to = k === 'ArrowLeft' ? sel - 1 : sel + 1;
                if (to < 0 || to >= h.length) return; // clamp at the ends
                const next = h.slice();
                [next[sel], next[to]] = [next[to], next[sel]];
                s.setLocalHandOrder(next);
                setSelIdx(to);
                return;
            }

            // every other modifier+arrow combo (Shift/Cmd+up/down, Ctrl, Alt) is
            // inert so a held modifier can never trigger a play.
            if (e.ctrlKey || e.metaKey || e.altKey || e.shiftKey) return;

            const s = ref.current;
            const g: PersonalGame | null = s.game;
            if (!g || !g.self) return;
            const meIdx = g.players.findIndex((p: any) => p.player_id === s.user_id);
            if (meIdx < 0) return;
            const isDefender = g.defender === meIdx;
            const h: Card[] = s.hand;
            const cur: { targets: Target[]; idx: number } | null = s.target;
            const sel: number | null = s.selIdx;

            e.preventDefault();

            // ---------- TARGET (cover/pass) mode -------------------------------
            if (cur) {
                const len = cur.targets.length;
                if (k === 'ArrowLeft') { setTarget({ ...cur, idx: (cur.idx - 1 + len) % len }); return; }
                if (k === 'ArrowRight') { setTarget({ ...cur, idx: (cur.idx + 1) % len }); return; }
                if (k === 'ArrowDown') { setTarget(null); return; }
                if (k === 'ArrowUp') {
                    if (sel == null || !h[sel]) { setTarget(null); return; }
                    const card = h[sel];
                    const t = cur.targets[cur.idx];
                    if (t.kind === 'pass') s.fire('pass', s.pass([card]));
                    else s.fire('cover', s.cover([card], [t.attack]));
                    return;
                }
                return;
            }

            // ---------- card-selection mode ------------------------------------
            if (sel == null) {
                // Down picks up (defender) / goods (attacker) without needing a
                // cursor first — neither move depends on a selected card.
                if (k === 'ArrowDown') {
                    if (isDefender) { if ((g.table_battles?.length ?? 0) > 0) s.fire('pickup', s.pickup()); }
                    else { if (tableFullyCovered(g) && !alreadyGood(g, s.user_id)) s.fire('good', s.good()); }
                    return;
                }
                if (k === 'ArrowLeft' || k === 'ArrowRight') { if (h.length) setSelIdx(0); }
                return;
            }
            if (!h.length) { setSelIdx(null); return; }
            // wrap around at the ends rather than clamping
            if (k === 'ArrowLeft') { setSelIdx((sel - 1 + h.length) % h.length); return; }
            if (k === 'ArrowRight') { setSelIdx((sel + 1) % h.length); return; }

            const card = h[sel];
            if (!card) return;

            if (k === 'ArrowDown') {
                if (isDefender) { if ((g.table_battles?.length ?? 0) > 0) s.fire('pickup', s.pickup()); }
                else { if (tableFullyCovered(g) && !alreadyGood(g, s.user_id)) s.fire('good', s.good()); }
                return;
            }

            if (k === 'ArrowUp') {
                // ----- multi-select commit (Feature 2) -------------------------
                // When the shared selectedCards set (built via Meta taps, same as
                // clicking) is non-empty, Up commits a move from the SET, exactly
                // like the ActionButtons attack/pass/cover buttons. When the set
                // is empty, fall through to the single-cursor-card behaviour.
                const selected: Card[] = s.selectedCards || [];
                if (selected.length > 0) {
                    if (!isDefender) {
                        if (canAttack(g, selected)) {
                            s.fire('attack', s.attack(selected), true);
                        }
                        return;
                    }
                    // defender: cover via the unambiguous mapping, else pass.
                    if (canCoverCards(g, selected)) {
                        const mapping = kernelUnambiguousCover(selected, g.table_battles || [], g.power_suit);
                        if (mapping) {
                            s.fire('cover', s.cover(mapping.coverCards, mapping.attackCards), true);
                            return;
                        }
                    }
                    if (canPass(g, selected)) {
                        s.fire('pass', s.pass(selected), true);
                    }
                    // ambiguous / illegal multi-card cover: no-op, keep selection
                    return;
                }

                if (!isDefender) {
                    if (canAttack(g, [card])) s.fire('attack', s.attack([card]));
                    return;
                }
                // defender: decide cover vs pass vs target-selection
                const coverable: CoverTarget[] = (g.table_battles || [])
                    .map((b, i) => ({ b, i }))
                    .filter(({ b }) => !b.defense && canCoverPair(b.attack, card, g.power_suit))
                    .map(({ b, i }) => ({ kind: 'cover', attack: b.attack, battleIndex: i }));
                const passOK = canPass(g, [card]);

                if (coverable.length === 0 && passOK) { s.fire('pass', s.pass([card])); return; }
                if (coverable.length === 1 && !passOK) { s.fire('cover', s.cover([card], [coverable[0].attack])); return; }
                if (coverable.length === 0 && !passOK) return; // illegal

                // ambiguous cover, or cover-AND-pass: assume cover first, append
                // the pass "empty space" as the right-most target if allowed.
                const targets: Target[] = passOK ? [...coverable, { kind: 'pass' }] : coverable;
                setTarget({ targets, idx: 0 });
                return;
            }
        };

        document.addEventListener('keydown', onKey);
        return () => document.removeEventListener('keydown', onKey);
        // everything the handler needs is read through ref.current, so the
        // listener installs once.
    }, []);

    /* ------------------------------- overlay geometry ---------------------- */
    const [geom, setGeom] = useState<{ sel: Rect | null; arrowTo: Rect | { point: { x: number; y: number } } | null; pass: boolean }>(
        { sel: null, arrowTo: null, pass: false },
    );
    const geomSig = useRef('');

    useEffect(() => {
        if (selIdx == null && !target) {
            if (geomSig.current !== '') { geomSig.current = ''; setGeom({ sel: null, arrowTo: null, pass: false }); }
            return;
        }
        let raf = 0;
        const tick = () => {
            const card = selIdx != null ? hand[selIdx] : null;
            const selRect = card
                ? rectOf(document.querySelector(`[data-location="hand"][data-player-id="${user_id}"][data-card="${cardKey(card)}"]`))
                : null;

            let arrowTo: Rect | { point: { x: number; y: number } } | null = null;
            let pass = false;
            if (target) {
                const t = target.targets[target.idx];
                if (t.kind === 'cover') {
                    arrowTo = rectOf(document.querySelector(`[data-battle-index="${t.battleIndex}"]`));
                } else {
                    pass = true;
                    arrowTo = { point: passPoint() };
                }
            }
            const sig = JSON.stringify({ selRect, arrowTo, pass });
            if (sig !== geomSig.current) { geomSig.current = sig; setGeom({ sel: selRect, arrowTo, pass }); }
            raf = requestAnimationFrame(tick);
        };
        raf = requestAnimationFrame(tick);
        return () => cancelAnimationFrame(raf);
    }, [selIdx, target, hand, user_id]);

    if (!game?.self) return null;

    const selCard = selIdx != null ? hand[selIdx] : null;
    const targetT = target ? target.targets[target.idx] : null;
    const stateEl = (
        <div
            data-testid="kbd-state"
            data-mode={target ? 'target' : selIdx != null ? 'cards' : 'none'}
            data-sel={selCard ? cardKey(selCard) : ''}
            data-target={targetT ? (targetT.kind === 'pass' ? 'pass' : cardKey(targetT.attack)) : ''}
            style={{ display: 'none' }}
        />
    );

    if (!geom.sel && !geom.arrowTo) return stateEl;

    const selCenter = geom.sel ? { x: geom.sel.x + geom.sel.w / 2, y: geom.sel.y } : null;
    const toCenter = geom.arrowTo
        ? ('point' in geom.arrowTo
            ? geom.arrowTo.point
            : { x: geom.arrowTo.x + geom.arrowTo.w / 2, y: geom.arrowTo.y + geom.arrowTo.h / 2 })
        : null;

    return (
        <>
            {stateEl}
            {/* arrow-key cursor: a short near-black underline a little below the
                card (black reads far better than red over the wool table). A soft
                white halo keeps it visible on dark wood too.
                Deliberately NOT a border ring — the full-border highlight is
                reserved for SELECTED cards (Cmd/click), so the two indicators
                never collide on a card that is both cursored and selected. */}
            {geom.sel && (
                <div
                    style={{
                        position: 'fixed',
                        left: geom.sel.x + geom.sel.w * 0.15,
                        top: geom.sel.y + geom.sel.h + 4,
                        width: geom.sel.w * 0.7, height: 3,
                        background: '#111', borderRadius: 2,
                        boxShadow: '0 0 5px 1px rgba(255,255,255,0.7)',
                        pointerEvents: 'none', zIndex: 1500,
                    }}
                />
            )}

            {/* cover/pass arrow + target marker */}
            {selCenter && toCenter && (
                <svg style={{ position: 'fixed', inset: 0, width: '100%', height: '100%', pointerEvents: 'none', zIndex: 1500 }}>
                    <defs>
                        <marker id="kbd-arrow" markerWidth="11" markerHeight="8" refX="9" refY="4" orient="auto">
                            <polygon points="0 0, 11 4, 0 8" fill="#ff2d2d" />
                        </marker>
                    </defs>
                    <line
                        x1={selCenter.x} y1={selCenter.y} x2={toCenter.x} y2={toCenter.y}
                        stroke="#ff2d2d" strokeWidth={3} strokeDasharray={geom.pass ? '7 5' : undefined}
                        markerEnd="url(#kbd-arrow)"
                    />
                    {!geom.pass && geom.arrowTo && 'w' in geom.arrowTo && (
                        <rect
                            x={geom.arrowTo.x - 3} y={geom.arrowTo.y - 3}
                            width={geom.arrowTo.w + 6} height={geom.arrowTo.h + 6}
                            rx={8} fill="none" stroke="#ff2d2d" strokeWidth={3}
                        />
                    )}
                    {geom.pass && (
                        <g>
                            <rect x={toCenter.x - 26} y={toCenter.y - 36} width={52} height={72} rx={8}
                                fill="none" stroke="#ff2d2d" strokeWidth={3} strokeDasharray="6 4" />
                            <text x={toCenter.x} y={toCenter.y + 54} fill="#ff2d2d" fontSize={13} fontWeight={700}
                                textAnchor="middle">PASS</text>
                        </g>
                    )}
                </svg>
            )}
        </>
    );
};

/* ------------------------------- helpers ----------------------------------- */
// Cover->attack mapping resolution lives in ONE place — the kernel
// (kernelUnambiguousCover -> legal.c unambiguous_cover). The local copy that
// used to sit here, and the TS coverCombinations.ts that replaced it, are both
// gone (A7/F9): one resolver for web/phone/watch/iMessage.

function tableFullyCovered(g: PersonalGame): boolean {
    const b = g.table_battles || [];
    return b.length > 0 && b.every((x) => x.defense);
}
function alreadyGood(g: PersonalGame, myId?: string | null): boolean {
    return !!myId && (g.good_players ?? []).includes(myId);
}
// the "empty space" the pass arrow points at: just right of the table battles
function passPoint(): { x: number; y: number } {
    const cells = Array.from(document.querySelectorAll('[data-location="table"]'));
    if (cells.length === 0) return { x: window.innerWidth / 2, y: window.innerHeight / 2 };
    let right = -Infinity, top = Infinity, bottom = -Infinity;
    for (const c of cells) {
        const r = c.getBoundingClientRect();
        right = Math.max(right, r.right);
        top = Math.min(top, r.top);
        bottom = Math.max(bottom, r.bottom);
    }
    return { x: right + 46, y: (top + bottom) / 2 };
}
