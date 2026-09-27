/* =============================================================================
 * SegmentText — a small "15-segment" alphanumeric LED-display glyph renderer
 * (14 line segments + a dedicated decimal-point dot = 15), styled after the
 * amber digital readouts on vintage car-stereo faceplates. Digits, A-Z and a
 * few symbols render as lit/unlit segment glyphs; anything else (lowercase,
 * non-Latin scripts, punctuation outside the font) falls back to a plain
 * glowing text run so localized strings never break.
 * ========================================================================== */

import React from 'react';

type Seg = 'T' | 'B' | 'TL' | 'TR' | 'BL' | 'BR' | 'ML' | 'MR' | 'DTL' | 'DTR' | 'DBL' | 'DBR' | 'VT' | 'VB' | 'BRH';

const W = 30;
const H = 50;
const PAD = 4;
type Pt = [number, number];
const P: Record<string, Pt> = {
    TL: [PAD, PAD], TR: [W - PAD, PAD], BL: [PAD, H - PAD], BR: [W - PAD, H - PAD],
    ML: [PAD, H / 2], MR: [W - PAD, H / 2], C: [W / 2, H / 2], TC: [W / 2, PAD], BC: [W / 2, H - PAD],
};
const LINES: Record<Seg, [string, string]> = {
    T: ['TL', 'TR'], B: ['BL', 'BR'], TL: ['TL', 'ML'], TR: ['TR', 'MR'],
    BL: ['ML', 'BL'], BR: ['MR', 'BR'], ML: ['ML', 'C'], MR: ['C', 'MR'],
    DTL: ['TL', 'C'], DTR: ['TR', 'C'], DBL: ['C', 'BL'], DBR: ['C', 'BR'],
    VT: ['TC', 'C'], VB: ['C', 'BC'],
    // right half of the bottom edge (BC-to-BR) — only the "→" arrowhead uses
    // this 15th segment; every other glyph just shows it dim, same as any
    // other segment it doesn't light.
    BRH: ['BC', 'BR'],
};
const ALL_SEGS = Object.keys(LINES) as Seg[];

// Straight-line approximations of each glyph. A handful of letters
// deliberately reuse a digit's shape (B~8, D/O~0, S~5) — real segment
// alphanumerics do the same; context (surrounded by letters or digits,
// never both) disambiguates. V is drawn as an asymmetric checkmark
// (straight left edge + one diagonal) rather than mirrored diagonals,
// so it stays structurally distinct from Y's fork at small sizes —
// a shared-shape-minus-one-segment V/Y pair reads identically once
// dim ghost segments are in the mix.
const FONT: Record<string, Seg[]> = {
    '0': ['T', 'TL', 'TR', 'BL', 'BR', 'B'],
    '1': ['TR', 'BR'],
    '2': ['T', 'TR', 'ML', 'MR', 'BL', 'B'],
    '3': ['T', 'TR', 'ML', 'MR', 'BR', 'B'],
    '4': ['TL', 'TR', 'ML', 'MR', 'BR'],
    '5': ['T', 'TL', 'ML', 'MR', 'BR', 'B'],
    '6': ['T', 'TL', 'ML', 'MR', 'BL', 'BR', 'B'],
    '7': ['T', 'TR', 'BR'],
    '8': ['T', 'TL', 'TR', 'ML', 'MR', 'BL', 'BR', 'B'],
    '9': ['T', 'TL', 'TR', 'ML', 'MR', 'BR', 'B'],
    A: ['T', 'TL', 'TR', 'ML', 'MR', 'BL', 'BR'],
    B: ['T', 'TL', 'TR', 'ML', 'MR', 'BL', 'BR', 'B'],
    C: ['T', 'TL', 'BL', 'B'],
    D: ['T', 'TL', 'TR', 'BL', 'BR', 'B'],
    E: ['T', 'TL', 'BL', 'B', 'ML', 'MR'],
    F: ['T', 'TL', 'BL', 'ML', 'MR'],
    G: ['T', 'TL', 'BL', 'B', 'BR', 'MR'],
    H: ['TL', 'BL', 'TR', 'BR', 'ML', 'MR'],
    I: ['T', 'B', 'VT', 'VB'],
    J: ['TR', 'BR', 'BL', 'B'],
    K: ['TL', 'BL', 'DTR', 'DBR'],
    L: ['TL', 'BL', 'B'],
    M: ['TL', 'BL', 'TR', 'BR', 'DTL', 'DTR'],
    N: ['TL', 'BL', 'TR', 'BR', 'DTL', 'DBR'],
    O: ['T', 'TL', 'TR', 'BL', 'BR', 'B'],
    P: ['T', 'TL', 'TR', 'ML', 'MR', 'BL'],
    Q: ['T', 'TL', 'TR', 'BL', 'BR', 'B', 'DBR'],
    R: ['T', 'TL', 'TR', 'ML', 'MR', 'BL', 'DBR'],
    S: ['T', 'TL', 'ML', 'MR', 'BR', 'B'],
    T: ['T', 'VT', 'VB'],
    U: ['TL', 'BL', 'TR', 'BR', 'B'],
    V: ['TL', 'BL', 'DTR', 'DBL'],
    W: ['TL', 'BL', 'TR', 'BR', 'DBL', 'DBR'],
    X: ['DTL', 'DTR', 'DBL', 'DBR'],
    Y: ['DTL', 'DTR', 'VT', 'VB'],
    Z: ['T', 'B', 'DTR', 'DBL'],
    '-': ['ML', 'MR'],
    '+': ['ML', 'MR', 'VT', 'VB'],
    '±': ['VT', 'ML', 'MR', 'VB', 'B'],
    '/': ['DTR', 'DBL'],
    // "covers" arrow: a diagonal shaft (DTL+DBR, top-left corner straight
    // through to bottom-right) tipped with a two-pronged arrowhead (BR, the
    // right side's lower half; BRH, the bottom edge's right half) meeting it
    // at the bottom-right corner.
    '→': ['DTL', 'DBR', 'BR', 'BRH'],
};

function inset(a: Pt, b: Pt, m: number): [Pt, Pt] {
    const dx = b[0] - a[0]; const dy = b[1] - a[1];
    return [[a[0] + dx * m, a[1] + dy * m], [b[0] - dx * m, b[1] - dy * m]];
}

/* ---------------------------- the cell run -------------------------------
 * ONE <svg> AND ONE FILTER PER RUN, not per segment. The glow is a CSS
 * `drop-shadow`, and Chrome gives every filtered element its own render
 * surface: with the filter on each lit <line> the Oracle panel carried 912 of
 * them, and any single change inside the panel re-ran all 912 filter passes.
 * Measured on the replay screen with the oracle deliberating, production
 * builds, same decision, 6 s samples: 1,163-1,289 filtered elements gave
 * 15-31 fps, a p95 frame of 50-350 ms and 9-14 frames over 100 ms; 47 of them
 * give 48-55 fps, a p95 of 19-50 ms and NONE over 100 ms. The element count is
 * untouched either way - the same 6,098 <line>s are on screen - so the whole
 * difference is how many render surfaces Chrome has to re-run. An isolated
 * bench of the three shapes agrees: per-line 12.7 fps / p95 433 ms, per-glyph
 * 53.7 / 98 ms, per-run 60.3 / 18.6 ms, the last identical to no filter at all.
 *
 * The glow is UNCHANGED, not cheapened. Every cell still lives in the same
 * 30x50-per-cell user space at the same scale, so `2px` of blur is the same
 * 2 user units it always was; all that moves is WHERE the filter is attached.
 * Lit segments are grouped by colour because `colorAt` may tint a cell.
 */

/** One position on the readout. `gap` is a space: it advances and draws nothing. */
type Cell =
    | { k: 'glyph'; ch: string; color: string }
    | { k: 'suit'; suit: number; color: string }
    | { k: 'dot'; color: string }
    | { k: 'gap' };

/** Cell advance in viewBox units. A glyph cell is W wide; a space or a decimal
 *  point is the narrow cell, which is `height * 0.32` PX - and px converts to
 *  user units through the cell scale (height / H), so it is H * 0.32 here, not
 *  W * 0.32. Getting that wrong shifts every cell after a space by ~1.3 px. */
const advanceOf = (c: Cell): number => (c.k === 'dot' || c.k === 'gap' ? H * 0.32 : W);

/** The 15 ghost segments every cell shows, as the dim background of the array. */
function segsOf(x: number, keyPrefix: string, onSegs: Set<Seg> | null, color: string,
                dim: string, litInto: React.ReactNode[], dimInto: React.ReactNode[]): void {
    for (const seg of ALL_SEGS) {
        const [p1, p2] = LINES[seg].map((k) => P[k]) as [Pt, Pt];
        const [a, b] = inset(p1, p2, 0.12);
        const on = onSegs?.has(seg) ?? false;
        (on ? litInto : dimInto).push(
            <line
                key={`${keyPrefix}${seg}`}
                x1={a[0] + x} y1={a[1]} x2={b[0] + x} y2={b[1]}
                stroke={on ? color : dim}
                strokeWidth={on ? 2.6 : 2}
                strokeLinecap="round"
            />,
        );
    }
}

/** The suit's own art (♠♥♣♦), drawn over its cell's ghost segments. */
function suitArt(suit: number, color: string): React.ReactNode {
    const stroke = { stroke: color, strokeWidth: 1.3, strokeLinecap: 'round' as const, strokeLinejoin: 'round' as const, fill: 'none' };
    if (suit === 0) { // spade — triangle + stem
        return (
            <>
                <path d="M6 1 L11 9.5 L1 9.5 Z" {...stroke} />
                <line x1={6} y1={9.5} x2={6} y2={11.5} stroke={color} strokeWidth={1.3} strokeLinecap="round" />
            </>
        );
    }
    if (suit === 1) { // heart — two dots + converging V
        return (
            <>
                <circle cx={3.4} cy={3.6} r={1.6} fill={color} />
                <circle cx={8.6} cy={3.6} r={1.6} fill={color} />
                <path d="M1.8 5.2 L6 11.5 L10.2 5.2" {...stroke} />
            </>
        );
    }
    if (suit === 2) { // club — trefoil dots + stem
        return (
            <>
                <circle cx={6} cy={3.2} r={1.9} fill={color} />
                <circle cx={2.6} cy={7} r={1.9} fill={color} />
                <circle cx={9.4} cy={7} r={1.9} fill={color} />
                <line x1={6} y1={8} x2={6} y2={11.5} stroke={color} strokeWidth={1.3} strokeLinecap="round" />
            </>
        );
    }
    return <path d="M6 1 L11 6 L6 11 L1 6 Z" {...stroke} />; // diamond — rotated square
}

/** A contiguous stretch of readout cells as ONE <svg>: the ghost segments in a
 *  single unfiltered group, and the lit ones in one filtered group per colour. */
function CellRun({ cells, height, dim, gap }: { cells: Cell[]; height: number; dim: string; gap: number }) {
    const scale = height / H;
    // The flex gap used to sit between sibling cells; inside one <svg> it has to
    // be spent as advance, in the same user units as everything else.
    const gapVB = scale > 0 ? gap / scale : 0;

    const dimSegs: React.ReactNode[] = [];
    const litByColor = new Map<string, React.ReactNode[]>();
    const litInto = (color: string): React.ReactNode[] => {
        const a = litByColor.get(color);
        if (a) return a;
        const made: React.ReactNode[] = [];
        litByColor.set(color, made);
        return made;
    };

    let x = 0;
    cells.forEach((c, i) => {
        if (c.k === 'glyph') {
            segsOf(x, `g${i}`, new Set(FONT[c.ch] ?? []), c.color, dim, litInto(c.color), dimSegs);
        } else if (c.k === 'suit') {
            segsOf(x, `s${i}`, null, c.color, dim, litInto(c.color), dimSegs);
            litInto(c.color).push(
                <g key={`a${i}`} transform={`translate(${x + 1.8},11.8) scale(2.2)`}>{suitArt(c.suit, c.color)}</g>,
            );
        } else if (c.k === 'dot') {
            litInto(c.color).push(
                <circle key={`d${i}`} cx={x + H * 0.16} cy={H - PAD} r={H * 0.09} fill={c.color} />,
            );
        }
        x += advanceOf(c) + gapVB;
    });
    const totalVB = Math.max(0, x - gapVB);

    return (
        <svg
            width={totalVB * scale} height={height} viewBox={`0 0 ${totalVB} ${H}`}
            style={{ flex: 'none', overflow: 'visible' }}
        >
            <g>{dimSegs}</g>
            {[...litByColor].map(([color, nodes]) => (
                <g key={color} style={{ filter: `drop-shadow(0 0 2px ${color}99)` }}>{nodes}</g>
            ))}
        </svg>
    );
}

// Suit icons (♠♥♣♦, index-matched to "SHCD") for the move-title strip.
// Not straight-segment glyphs — hearts and spades read as blobs when forced
// onto 14 straight lines — but drawn on the *same* dim ghost-segment
// background as every other cell, at the same cell size and glow, so a
// suit still reads as one more position on the LED array, not a pasted-in
// icon that breaks the strip.
const SUIT_CHARS = ['♠', '♥', '♣', '♦'];

interface SegmentTextProps {
    text: string;
    color: string;
    height?: number;
    gap?: number;
    dim?: string;
    style?: React.CSSProperties;
    /** Pad the display out to this many character cells with blank (fully
     * unlit) glyphs, like an unused position on a fixed-width LED readout,
     * so the whole run — not just the text — occupies a constant footprint. */
    length?: number;
    /** Per-character color override (index into `text`, pre-uppercasing) —
     * e.g. tinting just the suit letters in a card readout red. Falls back
     * to `color` wherever it returns undefined. */
    colorAt?: (index: number) => string | undefined;
}

/** Renders `text` as 15-segment LED glyphs where the font has a mapping,
 * falling back to plain glowing text (space-separated runs) otherwise —
 * so lowercase, punctuation and non-Latin scripts stay legible. */
export function SegmentText({
    text, color, height = 12, gap = 2, dim = 'rgba(255,255,255,0.09)', style, length, colorAt,
}: SegmentTextProps) {
    const chars = text.toUpperCase().split('');
    const nodes: React.ReactNode[] = [];
    // Cells accumulate into a run; a plain-text fallback span closes it, because
    // that text is HTML and cannot live inside the run's <svg>.
    let run: Cell[] = [];
    let plainBuf = '';
    const flushRun = (key: string) => {
        if (run.length === 0) return;
        nodes.push(<CellRun key={`r${key}`} cells={run} height={height} dim={dim} gap={gap} />);
        run = [];
    };
    const flushPlain = (key: string) => {
        if (!plainBuf) return;
        flushRun(key);
        nodes.push(
            <span
                key={key}
                style={{
                    fontFamily: "'Consolas','Menlo',monospace", fontWeight: 700,
                    color, textShadow: `0 0 4px ${color}99, 0 0 9px ${color}44`,
                    fontSize: height * 0.85,
                }}
            >
                {plainBuf}
            </span>,
        );
        plainBuf = '';
    };
    chars.forEach((ch, i) => {
        if (ch === ' ') { flushPlain(`p${i}`); run.push({ k: 'gap' }); return; }
        if (ch === '.') { flushPlain(`p${i}`); run.push({ k: 'dot', color }); return; }
        const suit = SUIT_CHARS.indexOf(ch);
        if (suit >= 0) { flushPlain(`p${i}`); run.push({ k: 'suit', suit, color: colorAt?.(i) ?? color }); return; }
        if (FONT[ch]) { flushPlain(`p${i}`); run.push({ k: 'glyph', ch, color: colorAt?.(i) ?? color }); return; }
        plainBuf += ch;
    });
    flushPlain('pEnd');
    // Pad by total character count, not just glyph cells — text with spaces
    // or decimal points (e.g. "EF 5.60 ±0.08") renders those as their own
    // (narrower) nodes above, so they still count toward the fixed length.
    for (let i = chars.length; length != null && i < length; i += 1) {
        run.push({ k: 'glyph', ch: '', color });
    }
    flushRun('End');
    return (
        <span style={{ display: 'inline-flex', alignItems: 'center', gap, ...style }}>
            {nodes}
        </span>
    );
}
