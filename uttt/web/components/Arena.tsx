'use client';

// THE 243 ARENA: two greedy bots playing the 243 x 243 game, as fast as the
// browser lets them. Every move, score and square is the kernel's (lib/
// arena.ts); this component owns the seed, the clock, the canvas, the bots'
// control and one button.
//
// ONE SEED A PAGE LOAD. Drawn once from the browser's secure random when the
// page loads (or read from ?seed=, to replay a game someone named), and
// everything on the page runs from it: both bots' dice, every tie-break, the
// whole game. Another game is a reload.
//
// THE BOARD IS PAINTED, NOT BUILT: no element per cell. Cells and decided
// nodes are forever, so they are painted once, when they appear, onto a layer
// canvas (found by comparing the kernel's bytes with last frame's copy); the
// grid lines are another layer painted once per size. A frame is those two
// layers, the region's wash under them and a ring round the last move.
//
// THE PACE: each animation frame plays moves until STEP_MS has gone, then
// paints. The rest of the frame is the browser's, so the page stays live. A
// kernel call plays `chunk` moves, and the chunk follows how long a call
// takes, so a fast setting is not one call a move and a slow one does not
// spend a whole chunk past the frame.
//
// THE CONTROL: how far the bots look (N plies) and how much work a move may
// spend. A change goes to the kernel at once and the next move plays by it;
// the game goes on.

import { useCallback, useEffect, useRef, useState, type ReactNode } from 'react';
import {
    GameClock, gameTime, loadArena, startError, parseSeed, randomSeed, UA_DRAW, UA_O, UA_X,
    UA_PLIES_MIN, UA_PLIES_MAX, UA_BUDGET_SMALL, UA_BUDGET_MED, UA_BUDGET_LARGE, UA_BUDGET_HUGE,
    type Arena as Kernel, type Config, type Status,
} from '../lib/arena';
import styles from './Arena.module.css';

/** How long a frame spends playing moves before it paints. */
const STEP_MS = 11;
/** A kernel call's moves: the first, the most, and the time a call aims at. */
const CHUNK0 = 8;
const CHUNK_MAX = 256;
const CALL_MS = 2;

/** The work a move may spend, as the control offers it. */
const BUDGETS: readonly { name: string; units: number }[] = [
    { name: 'Small', units: UA_BUDGET_SMALL },
    { name: 'Medium', units: UA_BUDGET_MED },
    { name: 'Large', units: UA_BUDGET_LARGE },
    { name: 'Huge', units: UA_BUDGET_HUGE },
];
/** How often the numbers in the header are refreshed. */
const STATS_MS = 120;

/* The kernel's inks (uttt_draw.c): X blue, O red, the page's ink, the
 * highlighter. */
const INK = '29, 27, 22';
const X_INK = '37, 55, 107';
const O_INK = '168, 50, 31';
const GOLD = '214, 168, 54';

type Phase = 'loading' | 'running' | 'over' | 'bad';

interface Stats { status: Status; rate: number | null; elapsedMs: number }

export function Arena() {
    const canvas = useRef<HTMLCanvasElement>(null);
    const kernel = useRef<Kernel | null>(null);
    const [phase, setPhase] = useState<Phase>('loading');
    const [seed, setSeed] = useState('');
    const [config, setConfig] = useState<Config | null>(null);
    const [stats, setStats] = useState<Stats | null>(null);
    const [replay, setReplay] = useState(false);
    const [failure, setFailure] = useState<{ lead: string; detail: string | null } | null>(null);
    const clock = useRef(new GameClock());

    // open the kernel and start the one game this load plays
    useEffect(() => {
        let gone = false;
        const named = parseSeed(new URLSearchParams(window.location.search).get('seed'));
        const s = named ?? randomSeed();
        setSeed(s);
        setReplay(named !== null);
        loadArena()
            .then((k) => {
                if (gone) return;
                k.start(s, 5);
                kernel.current = k;
                setConfig(k.config());
                setPhase('running');
            })
            .catch((e: unknown) => {
                if (gone) return;
                setFailure(startError(e));
                setPhase('bad');
            });
        return () => { gone = true; };
    }, []);

    // the loop: play, paint, count
    useEffect(() => {
        if (phase !== 'running') return;
        const k = kernel.current, cv = canvas.current;
        if (!k || !cv) return;
        const gc = clock.current;
        const first = k.status();
        // a loop started again over a game that already ended (a hidden page
        // shown again) has nothing to play and nothing to time
        if (first.over) { setPhase('over'); return; }
        const cfg = k.config();
        const painter = new Painter(cv, k, cfg);
        const view = viewControls(cv, painter);
        gc.frame(performance.now(), first.plies, false);
        let lastStats = 0, raf = 0, chunk = CHUNK0;

        const tick = (now: number) => {
            const until = performance.now() + STEP_MS;
            for (;;) {
                const a = performance.now(), asked = chunk;
                const n = k.step(asked);
                const dt = performance.now() - a;
                chunk = dt < CALL_MS / 2 ? Math.min(chunk * 2, CHUNK_MAX) : dt > CALL_MS * 2 ? Math.max(1, chunk >> 1) : chunk;
                if (n < asked || performance.now() >= until) break;
            }
            const st = k.status();
            gc.frame(performance.now(), st.plies, st.over !== 0);
            painter.frame(st);
            if (now - lastStats >= STATS_MS || gc.over) {
                lastStats = now;
                setStats({ status: st, rate: gc.rate(), elapsedMs: gc.ms });
                if (gc.over) { setPhase('over'); return; }
            }
            raf = requestAnimationFrame(tick);
        };
        raf = requestAnimationFrame(tick);
        return () => { cancelAnimationFrame(raf); gc.pause(); view.detach(); painter.detach(); };
    }, [phase]);

    // once the game is over the loop is gone; the board still answers to
    // resizing and zooming
    useEffect(() => {
        if (phase !== 'over') return;
        const k = kernel.current, cv = canvas.current;
        if (!k || !cv) return;
        const painter = new Painter(cv, k, k.config());
        const view = viewControls(cv, painter);
        painter.frame(k.status());
        return () => { view.detach(); painter.detach(); };
    }, [phase]);

    const another = useCallback(() => { window.location.assign('/243'); }, []);

    // the control: to the kernel now, read back from it (it clamps)
    const setBots = useCallback((plies: number, budget: number) => {
        const k = kernel.current;
        if (!k) return;
        k.setBots(plies, budget);
        setConfig(k.config());
    }, []);

    if (phase === 'bad') {
        return (
            <main className={styles.page}>
                <h1 className={styles.title}>Ultimate Tic-Tac-Toe, 243 x 243</h1>
                <p className={styles.note}>{failure?.lead ?? 'This browser could not start the game.'}</p>
                {failure?.detail ? <p className={styles.note}><code className={styles.seed}>{failure.detail}</code></p> : null}
            </main>
        );
    }

    const st = stats?.status;
    const w = config?.weight, t = config?.threat;
    return (
        <main className={styles.page}>
            <header className={styles.head}>
                <div className={styles.titles}>
                    <h1 className={styles.title}>Ultimate Tic-Tac-Toe, 243 x 243</h1>
                    <span className={styles.label}>Two bots, playing in your browser</span>
                </div>
                <dl className={styles.stats}>
                    <Stat name="Move" value={st ? fmt(st.plies) : '0'} />
                    <Stat name="Moves / s" value={stats?.rate != null ? fmt(Math.round(stats.rate)) : '-'} />
                    <Stat name={st?.over ? 'Result' : 'To play'} value={<Turn st={st} />} />
                    <Stat name="Time" value={gameTime(stats?.elapsedMs ?? 0, !!st?.over)} />
                    <Stat name="Seed" value={<span className={styles.seed}>{seed || ' '}</span>} />
                </dl>
                <Control config={config} disabled={phase !== 'running'} onChange={setBots} />
                <div className={styles.bots}>
                    <Seat side={UA_X} st={st} gameWeight={w?.[4]} />
                    <Seat side={UA_O} st={st} gameWeight={w?.[4]} />
                </div>
            </header>

            <canvas ref={canvas} className={styles.board} role="img"
                aria-label={st ? `The 243 by 243 board after ${st.plies} moves` : 'The 243 by 243 board'} />

            <div className={styles.foot}>
                {phase === 'over' && st ? (
                    <p className={styles.result} aria-live="polite">
                        {st.over === UA_DRAW ? `Drawn after ${fmt(st.plies)} moves.` : `${st.over === UA_X ? 'X' : 'O'} won in ${fmt(st.plies)} moves.`}
                        <button type="button" className={styles.again} onClick={another}>Another game</button>
                    </p>
                ) : null}
                <p className={styles.note}>
                    Each bot looks {config?.plies ?? 6} moves ahead, one more after a move that decides a grid, and plays
                    the move that gains the most, minus what it hands the other side: a 3 x 3 is worth {w?.[0] ?? 9}, a
                    9 x 9 {w?.[1] ?? 81}, a 27 x 27 {w?.[2] ?? 729}, an 81 x 81 {w?.[3] ?? 6561}, and the game more than
                    the rest of the board together; two in a row with the third still open is worth two ninths of the
                    grid it threatens ({`${t?.[0] ?? 2} in a 3 x 3, ${t?.[1] ?? 18} in a 9 x 9`}). &ldquo;Look ahead&rdquo; and
                    &ldquo;Work a move&rdquo; change both bots from their next move. &ldquo;Sees&rdquo; is
                    the value of the move it just chose; &ldquo;holds&rdquo; is everything that side has won. Ties go to
                    each bot&rsquo;s own dice, which come from the seed: the same seed and settings play the same game
                    {seed ? <> (<a href={`/243?seed=${seed}`}>{replay ? 'this link' : 'replay this one'}</a>)</> : null}.
                    Scroll or pinch to zoom, drag to pan, double-click to see the whole board.
                </p>
            </div>
        </main>
    );
}

/** How far the bots look, and how much a move may spend. */
function Control({ config, disabled, onChange }: { config: Config | null; disabled: boolean; onChange: (plies: number, budget: number) => void }) {
    const plies = config?.plies ?? 0, budget = config?.budget ?? 0;
    const off = disabled || !config;
    return (
        <div className={styles.control}>
            <div className={styles.knob}>
                <span className={styles.label} id="arena-plies">Look ahead</span>
                <div className={styles.stepper} role="group" aria-labelledby="arena-plies">
                    <button type="button" className={styles.step} aria-label="Look one move less"
                        disabled={off || plies <= UA_PLIES_MIN} onClick={() => onChange(plies - 1, budget)}>&minus;</button>
                    <output className={styles.value} aria-live="polite" data-plies={plies}>{config ? `${plies} ${plies === 1 ? 'move' : 'moves'}` : '-'}</output>
                    <button type="button" className={styles.step} aria-label="Look one move more"
                        disabled={off || plies >= UA_PLIES_MAX} onClick={() => onChange(plies + 1, budget)}>+</button>
                </div>
            </div>
            <label className={styles.knob}>
                <span className={styles.label}>Work a move</span>
                <select className={styles.select} value={budget} disabled={off} data-budget={budget}
                    onChange={(e) => onChange(plies, Number(e.target.value))}>
                    {BUDGETS.map((b) => <option key={b.units} value={b.units}>{b.name}, {fmt(b.units)}</option>)}
                </select>
            </label>
            <span className={styles.label}>{config ? `${config.capNode} replies a node, ${config.capRoot} at the root` : ''}</span>
        </div>
    );
}

function Stat({ name, value }: { name: string; value: ReactNode }) {
    return (
        <div className={styles.stat}>
            <dt className={styles.label}>{name}</dt>
            <dd className={styles.value}>{value}</dd>
        </div>
    );
}

function Turn({ st }: { st?: Status }) {
    if (!st) return <>X</>;
    if (st.over === UA_DRAW) return <>Drawn</>;
    if (st.over) return <span className={st.over === UA_X ? styles.x : styles.o}>{st.over === UA_X ? 'X won' : 'O won'}</span>;
    return <span className={st.turn === UA_X ? styles.x : styles.o}>{st.turn === UA_X ? 'X' : 'O'}</span>;
}

function Seat({ side, st, gameWeight }: { side: number; st?: Status; gameWeight?: number }) {
    const s = st ? (side === UA_X ? st.x : st.o) : null;
    const big = gameWeight ?? Infinity;
    const sees = !s || !s.searched ? '-' : Math.abs(s.value) * 2 >= big ? (s.value > 0 ? 'the game' : 'a lost game') : signed(s.value);
    return (
        <p className={styles.seat}>
            <span className={side === UA_X ? styles.x : styles.o}>{side === UA_X ? 'X' : 'O'}</span>
            {' '}sees <span className={styles.num}>{sees}</span>
            {' '}&middot; holds <span className={styles.num}>{s ? fmt(s.material >= big ? s.material - big : s.material) : '0'}</span>
            {s && s.material >= big ? ' and the game' : ''}
        </p>
    );
}

const fmt = (n: number) => n.toLocaleString('en-US');
const signed = (n: number) => (n > 0 ? '+' : n < 0 ? '-' : '') + fmt(Math.abs(n));

/* ------------------------------------------------------------------ paint */

interface View { z: number; tx: number; ty: number }

/** The board on a canvas: two layers painted as things appear, composed
 *  under the view's zoom once a frame. */
class Painter {
    view: View = { z: 1, tx: 0, ty: 0 };
    private marks = document.createElement('canvas');
    private lines = document.createElement('canvas');
    private prevGrid: Uint8Array;
    private prevNodes: Uint8Array;
    private px = 0;          // the canvas's backing side
    private cp = 1;          // pixels a cell
    private off = 0;         // the board's corner in the canvas
    private last: Status | null = null;
    private onResize = () => { this.resized = true; };
    private resized = true;

    constructor(private cv: HTMLCanvasElement, private k: Kernel, private cfg: Config) {
        this.prevGrid = new Uint8Array(cfg.leaves);
        this.prevNodes = new Uint8Array(cfg.nodes);
        window.addEventListener('resize', this.onResize);
    }

    detach() { window.removeEventListener('resize', this.onResize); }

    /** Device pixels of the canvas per board cell, and where the board starts. */
    geometry() { return { cp: this.cp, off: this.off, px: this.px, side: this.cfg.side }; }

    private fit(): boolean {
        const css = this.cv.getBoundingClientRect().width;
        if (!css) return false;
        const dpr = Math.min(window.devicePixelRatio || 1, 3);
        const px = Math.min(Math.round(css * dpr), 2916);   // 12 px a cell; two layers of this are 68 MB
        if (px === this.px && !this.resized) return true;
        this.resized = false;
        this.px = px;
        this.cv.width = this.cv.height = px;
        const side = this.cfg.side;
        this.cp = Math.max(1, Math.floor((px * 0.985) / side));
        this.off = Math.floor((px - this.cp * side) / 2);
        for (const c of [this.marks, this.lines]) c.width = c.height = px;
        this.paintLines();
        // repaint everything already on the board onto the new layer
        this.prevGrid.fill(0);
        this.prevNodes.fill(0);
        return true;
    }

    private paintLines() {
        const ctx = this.lines.getContext('2d')!;
        const { cp, off } = this, side = this.cfg.side, n = side * cp;
        ctx.clearRect(0, 0, this.px, this.px);
        // the five levels, finest first, each heavier than the one inside it
        const levels = [
            { every: 1, w: cp >= 6 ? 1 : 0, a: 0.07 },
            { every: 3, w: 1, a: cp >= 3 ? 0.22 : 0.12 },
            { every: 9, w: Math.max(1, cp * 0.22), a: 0.42 },
            { every: 27, w: Math.max(1.5, cp * 0.4), a: 0.66 },
            { every: 81, w: Math.max(2, cp * 0.65), a: 0.88 },
        ];
        for (const L of levels) {
            if (!L.w) continue;
            ctx.strokeStyle = `rgba(${INK}, ${L.a})`;
            ctx.lineWidth = L.w;
            ctx.beginPath();
            for (let i = L.every; i < side; i += L.every) {
                if (L.every < 81 && i % (L.every * 3) === 0) continue;   // a heavier line is there
                const p = off + i * cp;
                ctx.moveTo(p, off); ctx.lineTo(p, off + n);
                ctx.moveTo(off, p); ctx.lineTo(off + n, p);
            }
            ctx.stroke();
        }
        const edge = Math.max(2, cp * 0.65);
        ctx.strokeStyle = `rgba(${INK}, 0.88)`;
        ctx.lineWidth = edge;
        ctx.strokeRect(off, off, n, n);
    }

    /** Paint what appeared since the last frame onto the marks layer. */
    private paintNew() {
        const ctx = this.marks.getContext('2d')!;
        const { cp, off } = this, side = this.cfg.side;
        const grid = this.k.grid(), prev = this.prevGrid;
        const xs = new Path2D(), os = new Path2D();
        const glyph = cp >= 6;
        const pad = glyph ? cp * 0.24 : cp >= 3 ? 0.5 : 0;
        for (let i = 0; i < grid.length; i++) {
            const c = grid[i];
            if (c === prev[i]) continue;
            prev[i] = c;
            const x = off + (i % side) * cp, y = off + ((i / side) | 0) * cp;
            const path = c === UA_X ? xs : os;
            if (!glyph) { path.rect(x + pad, y + pad, cp - 2 * pad, cp - 2 * pad); continue; }
            if (c === UA_X) {
                path.moveTo(x + pad, y + pad); path.lineTo(x + cp - pad, y + cp - pad);
                path.moveTo(x + cp - pad, y + pad); path.lineTo(x + pad, y + cp - pad);
            } else {
                path.moveTo(x + cp - pad, y + cp / 2);
                path.arc(x + cp / 2, y + cp / 2, cp / 2 - pad, 0, Math.PI * 2);
            }
        }
        if (glyph) {
            ctx.lineWidth = Math.max(1, cp * 0.14);
            ctx.lineCap = 'round';
            ctx.strokeStyle = `rgba(${X_INK}, 0.9)`; ctx.stroke(xs);
            ctx.strokeStyle = `rgba(${O_INK}, 0.9)`; ctx.stroke(os);
        } else {
            ctx.fillStyle = `rgba(${X_INK}, 0.85)`; ctx.fill(xs);
            ctx.fillStyle = `rgba(${O_INK}, 0.85)`; ctx.fill(os);
        }

        // the nodes decided since: deepest first, so a bigger mark lands on
        // top of the smaller ones decided in the same frame
        const nodes = this.k.nodes(), pn = this.prevNodes, fresh: number[] = [];
        for (let id = 0; id < nodes.length; id++) {
            if (nodes[id] !== pn[id]) { pn[id] = nodes[id]; if (nodes[id]) fresh.push(id); }
        }
        fresh.sort((a, b) => b - a);    // ids are level-major: a larger id is never shallower
        for (const id of fresh) this.paintNode(ctx, id, nodes[id]);
    }

    private paintNode(ctx: CanvasRenderingContext2D, id: number, s: number) {
        const b = this.k.box(id), level = this.k.level(id), depth = this.cfg.depth;
        const { cp, off } = this;
        const x = off + b.x * cp, y = off + b.y * cp, n = b.size * cp;
        const ink = s === UA_X ? X_INK : s === UA_O ? O_INK : INK;
        // the tint: every won node adds its own, so a block inside a bigger
        // win reads darker - the board's history in one wash
        ctx.fillStyle = `rgba(${ink}, ${s === UA_DRAW ? 0.07 : 0.1})`;
        ctx.fillRect(x, y, n, n);
        // the big mark, thinner and fainter the larger it is, so the board
        // under an 81 x 81 still reads
        const size = depth - level;                      // 1 for a 3 x 3 ... depth for the game
        // a draw is a thin faint dash, not a mark: it won nothing; and the
        // game's own draw is the header's to say, not a bar across the board
        if (s === UA_DRAW && level === 0) return;
        const alpha = s === UA_DRAW ? 0.3 : [0, 0.85, 0.7, 0.55, 0.45, 0.6][size] ?? 0.5;
        ctx.lineWidth = Math.max(1, n * (s === UA_DRAW ? 0.015 : size === 1 ? 0.08 : 0.05));
        ctx.lineCap = s === UA_DRAW ? 'butt' : 'round';
        ctx.strokeStyle = `rgba(${ink}, ${alpha})`;
        const p = n * 0.16;
        ctx.beginPath();
        if (s === UA_X) {
            ctx.moveTo(x + p, y + p); ctx.lineTo(x + n - p, y + n - p);
            ctx.moveTo(x + n - p, y + p); ctx.lineTo(x + p, y + n - p);
        } else if (s === UA_O) {
            ctx.arc(x + n / 2, y + n / 2, n / 2 - p, 0, Math.PI * 2);
        } else {
            ctx.moveTo(x + p * 1.4, y + n / 2); ctx.lineTo(x + n - p * 1.4, y + n / 2);   // a draw: a dash
        }
        ctx.stroke();
    }

    /** One frame: anything new onto the layers, then wash, layers and ring
     *  under the view. Pass null to repaint the last status (a zoom). */
    frame(st: Status | null) {
        if (st) this.last = st;
        st = this.last;
        if (!st || !this.fit()) return;
        this.paintNew();
        const ctx = this.cv.getContext('2d')!;
        const { cp, off, px } = this, { z, tx, ty } = this.view;
        ctx.setTransform(1, 0, 0, 1, 0, 0);
        ctx.clearRect(0, 0, px, px);
        ctx.setTransform(z, 0, 0, z, tx, ty);
        ctx.imageSmoothingEnabled = z < 2;
        const r = st.regionBox;
        if (r.size) {
            ctx.fillStyle = `rgba(${GOLD}, 0.32)`;
            ctx.fillRect(off + r.x * cp, off + r.y * cp, r.size * cp, r.size * cp);
        }
        ctx.drawImage(this.marks, 0, 0);
        ctx.drawImage(this.lines, 0, 0);
        if (r.size) {
            ctx.strokeStyle = `rgba(${GOLD}, 0.95)`;
            ctx.lineWidth = Math.max(1.5, cp * 0.35) / Math.sqrt(z);
            ctx.strokeRect(off + r.x * cp, off + r.y * cp, r.size * cp, r.size * cp);
        }
        const l = st.lastBox;
        if (l.size && !st.over) {
            // a ring the size of a 3 x 3 at least, so the last move is findable
            const ring = Math.max(cp * 2.2, 9 / z);
            const cx = off + (l.x + 0.5) * cp, cy = off + (l.y + 0.5) * cp;
            ctx.strokeStyle = `rgba(${st.turn === UA_X ? O_INK : X_INK}, 0.95)`;
            ctx.lineWidth = Math.max(1.5, cp * 0.3) / Math.sqrt(z);
            ctx.beginPath();
            ctx.arc(cx, cy, ring / 2, 0, Math.PI * 2);
            ctx.stroke();
        }
    }
}

/** Wheel or pinch to zoom about the pointer, drag to pan, double-click to see
 *  the whole board. The view is a scale and a translation in canvas pixels,
 *  kept so the board always covers the canvas once zoomed. */
function viewControls(cv: HTMLCanvasElement, painter: Painter) {
    const scale = () => cv.width / Math.max(cv.getBoundingClientRect().width, 1);
    const clamp = () => {
        const v = painter.view, px = cv.width;
        v.z = Math.min(Math.max(v.z, 1), 16);
        v.tx = Math.min(0, Math.max(px - px * v.z, v.tx));
        v.ty = Math.min(0, Math.max(px - px * v.z, v.ty));
    };
    const zoomAt = (cx: number, cy: number, f: number) => {
        const v = painter.view, z = Math.min(Math.max(v.z * f, 1), 16);
        v.tx = cx - ((cx - v.tx) * z) / v.z;
        v.ty = cy - ((cy - v.ty) * z) / v.z;
        v.z = z;
        clamp();
        painter.frame(null);
    };
    const at = (e: { clientX: number; clientY: number }) => {
        const r = cv.getBoundingClientRect(), s = scale();
        return [(e.clientX - r.left) * s, (e.clientY - r.top) * s];
    };
    const wheel = (e: WheelEvent) => {
        e.preventDefault();
        const [x, y] = at(e);
        zoomAt(x, y, Math.exp(-e.deltaY * (e.ctrlKey ? 0.01 : 0.002)));
    };
    const pointers = new Map<number, [number, number]>();
    let pinch = 0;
    const down = (e: PointerEvent) => {
        cv.setPointerCapture(e.pointerId);
        pointers.set(e.pointerId, [e.clientX, e.clientY]);
        if (pointers.size === 2) { const [a, b] = [...pointers.values()]; pinch = Math.hypot(a[0] - b[0], a[1] - b[1]); }
    };
    const move = (e: PointerEvent) => {
        const was = pointers.get(e.pointerId);
        if (!was) return;
        pointers.set(e.pointerId, [e.clientX, e.clientY]);
        if (pointers.size === 2) {
            const [a, b] = [...pointers.values()], d = Math.hypot(a[0] - b[0], a[1] - b[1]);
            if (pinch) { const [x, y] = at({ clientX: (a[0] + b[0]) / 2, clientY: (a[1] + b[1]) / 2 }); zoomAt(x, y, d / pinch); }
            pinch = d;
            return;
        }
        const s = scale(), v = painter.view;
        v.tx += (e.clientX - was[0]) * s;
        v.ty += (e.clientY - was[1]) * s;
        clamp();
        painter.frame(null);
    };
    const up = (e: PointerEvent) => { pointers.delete(e.pointerId); if (pointers.size < 2) pinch = 0; };
    const reset = () => { Object.assign(painter.view, { z: 1, tx: 0, ty: 0 }); painter.frame(null); };
    cv.addEventListener('wheel', wheel, { passive: false });
    cv.addEventListener('pointerdown', down);
    cv.addEventListener('pointermove', move);
    cv.addEventListener('pointerup', up);
    cv.addEventListener('pointercancel', up);
    cv.addEventListener('dblclick', reset);
    return {
        detach() {
            cv.removeEventListener('wheel', wheel);
            cv.removeEventListener('pointerdown', down);
            cv.removeEventListener('pointermove', move);
            cv.removeEventListener('pointerup', up);
            cv.removeEventListener('pointercancel', up);
            cv.removeEventListener('dblclick', reset);
        },
    };
}
