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

function Glyph({ ch, height, color, dim }: { ch: string; height: number; color: string; dim: string }) {
    const on = new Set(FONT[ch] ?? []);
    const w = height * (W / H);
    return (
        <svg width={w} height={height} viewBox={`0 0 ${W} ${H}`} style={{ flex: 'none', overflow: 'visible' }}>
            {ALL_SEGS.map((seg) => {
                const [p1, p2] = LINES[seg].map((k) => P[k]) as [Pt, Pt];
                const [a, b] = inset(p1, p2, 0.12);
                const lit = on.has(seg);
                return (
                    <line
                        key={seg}
                        x1={a[0]} y1={a[1]} x2={b[0]} y2={b[1]}
                        stroke={lit ? color : dim}
                        strokeWidth={lit ? 2.6 : 2}
                        strokeLinecap="round"
                        style={lit ? { filter: `drop-shadow(0 0 2px ${color}99)` } : undefined}
                    />
                );
            })}
        </svg>
    );
}

function Dot({ height, color }: { height: number; color: string }) {
    const w = height * 0.32;
    const r = height * 0.09;
    return (
        <svg width={w} height={height} viewBox={`0 0 ${W * 0.32} ${H}`} style={{ flex: 'none', overflow: 'visible' }}>
            <circle cx={W * 0.16} cy={H - PAD} r={r * (H / height)} fill={color} style={{ filter: `drop-shadow(0 0 2px ${color}99)` }} />
        </svg>
    );
}

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
}

/** Renders `text` as 15-segment LED glyphs where the font has a mapping,
 * falling back to plain glowing text (space-separated runs) otherwise —
 * so lowercase, punctuation and non-Latin scripts stay legible. */
export function SegmentText({
    text, color, height = 12, gap = 2, dim = 'rgba(255,255,255,0.09)', style, length,
}: SegmentTextProps) {
    const chars = text.toUpperCase().split('');
    const nodes: React.ReactNode[] = [];
    let plainBuf = '';
    const flushPlain = (key: string) => {
        if (!plainBuf) return;
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
    let cells = 0;
    chars.forEach((ch, i) => {
        if (ch === ' ') { flushPlain(`p${i}`); nodes.push(<span key={`sp${i}`} style={{ display: 'inline-block', width: height * 0.32 }} />); return; }
        if (ch === '.') { flushPlain(`p${i}`); nodes.push(<Dot key={i} height={height} color={color} />); return; }
        if (FONT[ch]) { flushPlain(`p${i}`); nodes.push(<Glyph key={i} ch={ch} height={height} color={color} dim={dim} />); cells += 1; return; }
        plainBuf += ch;
    });
    flushPlain('pEnd');
    for (let i = cells; length != null && i < length; i += 1) {
        nodes.push(<Glyph key={`blank${i}`} ch="" height={height} color={color} dim={dim} />);
    }
    return (
        <span style={{ display: 'inline-flex', alignItems: 'center', gap, ...style }}>
            {nodes}
        </span>
    );
}
