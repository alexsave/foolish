'use client';

// THE REPLAY: one finished game, move by move, on the napkin it was played
// on. Three controls and nothing else - back a move, play or pause, forward a
// move. Every stroke is the kernel's (lib/kernel.ts): this component owns the
// clock, the canvas and the buttons, and asks the kernel for the board once a
// display frame while anything is moving. It schedules nothing: the next move
// starts when the loop sees the last one has settled and rested.

import { useCallback, useEffect, useRef, useState, type ReactNode } from 'react';
import { openReplay, type Replay as Kernel } from '../lib/kernel';
import styles from './Replay.module.css';

/** The board's lines run past it (the pen's overshoot); this much paper is
 *  kept round the board so they are not cut at the canvas edge. */
const MARGIN = 0.06;

/** How many rests a settled move holds before autoplay takes the next one:
 *  the pace is the page's, the rest is the kernel's. */
const RESTS_BETWEEN_MOVES = 2;

type Status = 'loading' | 'ready' | 'bad';

export function Replay({ code }: { code: string }) {
    const canvas = useRef<HTMLCanvasElement>(null);
    const kernel = useRef<Kernel | null>(null);
    const [status, setStatus] = useState<Status>('loading');
    const [ply, setPly] = useState(0);
    const [plies, setPlies] = useState(0);
    const [playing, setPlaying] = useState(false);
    const [caption, setCaption] = useState('');

    // what the frame loop reads without re-subscribing
    const loop = useRef({ start: 0, dirty: true, settledAt: -1, ply: 0, playing: false });
    loop.current.ply = ply;
    loop.current.playing = playing;

    const goTo = useCallback((k: number, animate: boolean) => {
        const kr = kernel.current;
        if (!kr) return;
        const to = Math.max(0, Math.min(k, kr.plies));
        kr.seek(to);
        if (animate) kr.animate();
        Object.assign(loop.current, { start: performance.now(), dirty: true, settledAt: -1, ply: to });
        setPly(to);
        setCaption(kr.caption());
    }, []);

    // open the code
    useEffect(() => {
        let gone = false;
        openReplay(code)
            .then((kr) => {
                if (gone) return;
                if (!kr) return setStatus('bad');
                kernel.current = kr;
                setPlies(kr.plies);
                paintPaper(kr);
                goTo(0, false);
                setStatus('ready');
                setPlaying(true);
            })
            .catch(() => !gone && setStatus('bad'));
        return () => {
            gone = true;
        };
    }, [code, goTo]);

    // one animation-frame loop: draw while moving, advance while playing
    useEffect(() => {
        if (status !== 'ready') return;
        let raf = 0;
        const tick = (now: number) => {
            const kr = kernel.current, cv = canvas.current, st = loop.current;
            if (kr && cv) {
                if (st.dirty) {
                    const f = kr.frame(now - st.start);
                    draw(cv, f);
                    st.dirty = f.running;
                    if (!f.running) st.settledAt = now;
                }
                if (st.playing && !st.dirty && st.settledAt >= 0
                    && now - st.settledAt >= kr.restMs * RESTS_BETWEEN_MOVES) {
                    if (st.ply < kr.plies) goTo(st.ply + 1, true);
                    else setPlaying(false);
                }
            }
            raf = requestAnimationFrame(tick);
        };
        raf = requestAnimationFrame(tick);
        const redraw = () => (loop.current.dirty = true);
        window.addEventListener('resize', redraw);
        return () => {
            cancelAnimationFrame(raf);
            window.removeEventListener('resize', redraw);
        };
    }, [status, goTo]);

    const back = useCallback(() => {
        setPlaying(false);
        goTo(loop.current.ply - 1, false);
    }, [goTo]);
    const forward = useCallback(() => {
        setPlaying(false);
        if (loop.current.ply < plies) goTo(loop.current.ply + 1, true);
    }, [goTo, plies]);
    const toggle = useCallback(() => {
        if (loop.current.playing) return setPlaying(false);
        // play from a finished board starts the game over
        if (loop.current.ply >= plies) goTo(0, false);
        setPlaying(true);
    }, [goTo, plies]);

    // the keyboard: space plays and pauses, the arrows step
    useEffect(() => {
        const key = (e: KeyboardEvent) => {
            if (e.target instanceof HTMLButtonElement && e.key === ' ') return; // the button's own
            if (e.key === ' ') { e.preventDefault(); toggle(); }
            else if (e.key === 'ArrowLeft') back();
            else if (e.key === 'ArrowRight') forward();
        };
        window.addEventListener('keydown', key);
        return () => window.removeEventListener('keydown', key);
    }, [toggle, back, forward]);

    if (status === 'bad') {
        return (
            <main className={styles.page}>
                <h1 className={styles.title}>Ultimate Tic-Tac-Toe</h1>
                <p className={styles.line}>This link does not hold a game. Copy it again from the end of the game in Messages.</p>
            </main>
        );
    }

    return (
        <main className={styles.page}>
            <header className={styles.head}>
                <h1 className={styles.title}>Ultimate Tic-Tac-Toe</h1>
                <span className={styles.label}>
                    Replay{plies ? <> &middot; move <span className={styles.num}>{ply}</span> of <span className={styles.num}>{plies}</span></> : null}
                </span>
            </header>

            <canvas ref={canvas} className={styles.board} role="img" aria-label={caption || 'The board'} />

            <p className={styles.line} aria-live="polite">{status === 'loading' ? ' ' : caption}</p>

            <nav className={styles.deck} aria-label="Replay controls">
                <button type="button" className={styles.knob} onClick={back} disabled={status !== 'ready' || ply === 0} aria-label="Back a move" title="Back a move (Left arrow)">
                    <IconBack />
                </button>
                <button type="button" className={`${styles.knob} ${styles.main}`} onClick={toggle} disabled={status !== 'ready'} aria-label={playing ? 'Pause' : 'Play'} title={playing ? 'Pause (Space)' : 'Play (Space)'}>
                    {playing ? <IconPause /> : <IconPlay />}
                </button>
                <button type="button" className={styles.knob} onClick={forward} disabled={status !== 'ready' || ply >= plies} aria-label="Forward a move" title="Forward a move (Right arrow)">
                    <IconForward />
                </button>
            </nav>
        </main>
    );
}

/** Fill the kernel's polygons, board and margin fitted to the canvas's box. */
function draw(cv: HTMLCanvasElement, f: ReturnType<Kernel['frame']>) {
    const css = cv.getBoundingClientRect().width;
    if (!css) return;
    const dpr = Math.min(window.devicePixelRatio || 1, 3);
    // capped: a canvas with no stylesheet is as wide as its backing store,
    // so sizing one from the other would grow it every frame
    const px = Math.min(Math.round(css * dpr), 2048);
    if (cv.width !== px || cv.height !== px) cv.width = cv.height = px;
    const ctx = cv.getContext('2d');
    if (!ctx) return;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, px, px);
    const side = px * (1 - 2 * MARGIN), off = px * MARGIN;
    const p = f.points;
    for (const poly of f.polys) {
        if (poly.n < 3) continue;
        ctx.beginPath();
        let k = poly.first * 2;
        ctx.moveTo(off + p[k] * side, off + p[k + 1] * side);
        for (let j = 1; j < poly.n; j++) {
            k += 2;
            ctx.lineTo(off + p[k] * side, off + p[k + 1] * side);
        }
        ctx.closePath();
        ctx.fillStyle = poly.fill;
        ctx.fill();
    }
}

/** The kernel's napkin under the whole page, stretched as the app stretches
 *  it over its sheet. */
function paintPaper(kr: Kernel) {
    const side = 420;
    const c = document.createElement('canvas');
    c.width = c.height = side;
    c.getContext('2d')?.putImageData(kr.paper(side), 0, 0);
    document.body.style.backgroundImage = `url(${c.toDataURL('image/png')})`;
}

const Svg = ({ children }: { children: ReactNode }) => (
    <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        {children}
    </svg>
);
const IconBack = () => <Svg><path d="M15 5 8 12l7 7" /></Svg>;
const IconForward = () => <Svg><path d="m9 5 7 7-7 7" /></Svg>;
const IconPlay = () => <Svg><path d="M8 5.5v13l10.5-6.5z" fill="currentColor" /></Svg>;
const IconPause = () => <Svg><path d="M9 5.5v13M15 5.5v13" /></Svg>;
