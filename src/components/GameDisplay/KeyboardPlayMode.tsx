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
 * overlay (a red cursor ring + a cover/pass arrow). Mounted by GameBoard
 * wherever the board is `interactive` (live + tutorial). The replay screen is
 * NOT interactive, so its arrow-key transport is never hijacked.
 *
 * CONTROLS
 *   left/right (nothing selected) -> select the leftmost card
 *   left/right (card selected)    -> move the red cursor between hand cards
 *   down  (defender)              -> pick up (if the table is non-empty)
 *   down  (attacker)              -> good   (if the table is fully covered)
 *   up    (attacker)              -> attack with the cursor card (if legal)
 *   up    (defender):
 *       can cover exactly one & cannot pass  -> cover it immediately
 *       can pass but cannot cover            -> pass immediately
 *       can cover several (or cover AND pass)-> TARGET mode: an arrow points the
 *           cursor card at an attack; left/right moves it between the coverable
 *           attacks; if the card can also pass, one step right past the last
 *           attack points the arrow at an empty space (= pass); up executes.
 *   down / escape (in target mode) -> cancel back to card selection
 * ========================================================================== */

import React, { useCallback, useEffect, useRef, useState } from 'react';
import { Card, PersonalGame } from '../../common/types';
import { useServer } from '../../contexts/ServerContext';
import { useAnimation } from '../../contexts/AnimationContext';
import { useAuth } from '../../contexts/AuthContext';
import { canCover } from '../../common/common_utils';
import { canAttack, canPass } from '../../utils/gameValidation';

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
    const { game: rawGame, localHandOrder } = useServer();
    const game = rawGame as PersonalGame | null;
    const { attack, cover, pass, pickup, good } = useAnimation();
    const { user_id } = useAuth();

    const hand: Card[] = localHandOrder && localHandOrder.length ? localHandOrder : (game?.self?.hand ?? []);

    const [selIdx, setSelIdx] = useState<number | null>(null);
    const [target, setTarget] = useState<{ targets: Target[]; idx: number } | null>(null);

    // latest of everything the key handler needs, so the listener stays
    // installed once instead of re-subscribing on every state change
    const ref = useRef<any>({});
    ref.current = { game, hand, user_id, selIdx, target, attack, cover, pass, pickup, good };

    const reset = useCallback(() => { setSelIdx(null); setTarget(null); }, []);

    // clamp / clear the cursor when the hand changes under it
    useEffect(() => {
        if (selIdx !== null && selIdx >= hand.length) setSelIdx(hand.length ? hand.length - 1 : null);
    }, [hand.length, selIdx]);

    const run = useCallback((p: Promise<unknown>) => {
        p.then(reset).catch((e) => { console.error('[kbd] move failed:', e?.message ?? e); reset(); });
    }, [reset]);

    /* ------------------------------- key logic ----------------------------- */
    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            const k = e.key;
            if (k !== 'ArrowLeft' && k !== 'ArrowRight' && k !== 'ArrowUp' && k !== 'ArrowDown' && k !== 'Escape') return;
            if (isTypingTarget() || e.ctrlKey || e.metaKey || e.altKey) return;

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
                if (k === 'ArrowDown' || k === 'Escape') { setTarget(null); return; }
                if (k === 'ArrowUp') {
                    if (sel == null || !h[sel]) { setTarget(null); return; }
                    const card = h[sel];
                    const t = cur.targets[cur.idx];
                    if (t.kind === 'pass') run(s.pass([card]));
                    else run(s.cover([card], [t.attack]));
                    return;
                }
                return;
            }

            // ---------- card-selection mode ------------------------------------
            if (k === 'Escape') { reset(); return; }

            if (sel == null) {
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
                if (isDefender) { if ((g.table_battles?.length ?? 0) > 0) run(s.pickup()); }
                else { if (tableFullyCovered(g) && !alreadyGood(g, s.user_id)) run(s.good()); }
                return;
            }

            if (k === 'ArrowUp') {
                if (!isDefender) {
                    if (canAttack(g, [card])) run(s.attack([card]));
                    return;
                }
                // defender: decide cover vs pass vs target-selection
                const coverable: CoverTarget[] = (g.table_battles || [])
                    .map((b, i) => ({ b, i }))
                    .filter(({ b }) => !b.defense && canCover(b.attack, card, g.power_suit))
                    .map(({ b, i }) => ({ kind: 'cover', attack: b.attack, battleIndex: i }));
                const passOK = canPass(g, [card]);

                if (coverable.length === 0 && passOK) { run(s.pass([card])); return; }
                if (coverable.length === 1 && !passOK) { run(s.cover([card], [coverable[0].attack])); return; }
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
    }, [reset, run]);

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
            {/* red cursor ring on the selected hand card */}
            {geom.sel && (
                <div
                    style={{
                        position: 'fixed', left: geom.sel.x - 3, top: geom.sel.y - 3,
                        width: geom.sel.w + 6, height: geom.sel.h + 6, borderRadius: 8,
                        border: '3px solid #ff2d2d', boxShadow: '0 0 12px 2px rgba(255,45,45,0.8)',
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
