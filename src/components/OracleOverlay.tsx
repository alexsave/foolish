/* =============================================================================
 * Infinite Oracle — the replay overlay panel (docs/INFINITE_ORACLE_DESIGN.md §9)
 * Right-anchored panel mounted through GameBoard's chrome slot. Shows octogen's
 * read of every option at the paused decision: candidate rows with relative
 * bars, expected-finish ± SE, a chess.com-style classification chip, and the
 * "come into focus" animation as the standard errors shrink. Switches to verdict
 * mode when the exact endgame solver proves win/loss.
 *
 * Styled as a vintage car-stereo graphic-equalizer faceplate: brushed-metal
 * bezel, amber 15-segment LED digital readouts, teal illuminated hardware
 * buttons, and segmented EQ-style bargraphs standing in for the plain
 * progress bars.
 * ========================================================================== */

import React, { useMemo, useState } from 'react';
import { Card } from '@api/core/types.ts';
import { useLocalization } from '../contexts/LocalizationContext';
import { SegmentText } from './SegmentDisplay';
import {
    OracleSnapshot, OracleCandidate, oracleClassify, OracleClass,
} from '../oracle/types';
import { explainCandidate, WhyTreeNode, symPhrase, TFn } from '../oracle/explain';

// Inverse of oracleCardToken (types.ts): recover the decoded Card so it can
// be re-rendered as segment glyphs. Rank strings are OG_EX_VAL; suit is
// "SHCD"; trailing '*' (trump marker) is decorative.
const RANK_TO_VALUE: Record<string, number> = {
    '2': 1, '3': 2, '4': 3, '5': 4, '6': 5, '7': 6, '8': 7, '9': 8,
    '10': 9, J: 10, Q: 11, K: 12, A: 13,
};
const VALUE_TO_RANK = ['?', '2', '3', '4', '5', '6', '7', '8', '9', '10', 'J', 'Q', 'K', 'A'];
function tokenToCard(token: string): Card | null {
    const body = token.endsWith('*') ? token.slice(0, -1) : token;
    if (body.length < 2) return null;
    const suit = 'SHCD'.indexOf(body[body.length - 1]);
    const value = RANK_TO_VALUE[body.slice(0, -1)];
    if (suit < 0 || value == null) return null;
    return { suit, value };
}
// The whole move — attacking cards, an optional "→" and the covered/target
// cards, or the bare move type for card-less moves (pass/pickup/good/wait) —
// as one string for a single fixed-width segment display per row. Suits are
// the ♠♥♣♦ characters SegmentText renders as small icons (see SuitGlyph in
// SegmentDisplay.tsx) rather than letters — still one array element each,
// so the fixed-length budget and blank padding are unaffected. Also tracks
// which indices are red-suit (♥/♦) so the display can tint just those.
function moveTitleText(c: OracleCandidate): { text: string; redAt: Set<number> } {
    const redAt = new Set<number>();
    let text = '';
    const pushCard = (token: string) => {
        const card = tokenToCard(token);
        if (!card) { text += token; return; }
        text += VALUE_TO_RANK[card.value];
        if (card.suit === 1 || card.suit === 3) redAt.add(text.length);
        text += '♠♥♣♦'[card.suit];
    };
    if (!c.cards.length) { text = c.type === 'pass' ? 'PASS' : c.type; return { text, redAt }; }
    c.cards.forEach(pushCard);
    if (c.target?.length) { text += '→'; c.target.forEach(pushCard); }
    return { text, redAt };
}
// Fixed character budget for the move-title strip: worst realistic case is
// three attacking tens covering three cards — 3×"10S" (9) + "→" (1) +
// 3×"XS" (6) = 16.
const MOVE_TITLE_LEN = 16;
const SUIT_RED = '#E8674F';

const CLASS_COLOR: Record<OracleClass, string> = {
    best: '#4CAF7D',
    excellent: '#6FBF73',
    good: '#A7C957',
    inaccuracy: '#E0B341',
    mistake: '#E08A3C',
    blunder: '#D45B4E',
};

const VERDICT_BAR: Record<string, number> = { win: 100, draw: 55, unknown: 45, loss: 16 };
const VERDICT_COLOR: Record<string, string> = {
    win: '#4CAF7D', draw: '#E0B341', unknown: 'rgba(200,200,210,0.5)',
    loss: '#D45B4E', none: 'rgba(200,200,210,0.35)', illegal: 'rgba(120,120,130,0.4)',
};

// ---- faceplate palette -----------------------------------------------------
const AMBER = '#FFA53C';
const TEAL = '#5EEAD4';
const CARD_COLOR = '#E8E3D2';
const LCD_MONO = "'Consolas', 'Menlo', 'SFMono-Regular', monospace";

const ledText = (color: string): React.CSSProperties => ({
    fontFamily: LCD_MONO,
    fontWeight: 700,
    letterSpacing: '0.05em',
    color,
    textShadow: `0 0 4px ${color}99, 0 0 9px ${color}44`,
});

interface Props {
    snapshot: OracleSnapshot | null;
    onClose: () => void;
    onToggleMemory: () => void;
    onRetry: () => void;
}

// Filled "hardware label" chip — mimics the amber/colored solid indicator
// tags stenciled onto old car-stereo faceplates (e.g. "PLAYED", "WIN").
// Unlit segments read as embossed dark strokes against the solid fill.
const SolidChip = ({ bg, fg = '#fff', text }: { bg: string; fg?: string; text: string }) => (
    <span style={{
        display: 'inline-flex', padding: '2px 5px', borderRadius: 4, background: bg, whiteSpace: 'nowrap',
    }}>
        <SegmentText text={text} color={fg} height={8} gap={1.5} dim="rgba(0,0,0,0.18)" />
    </span>
);

// Outline "LCD segment" chip — dark display glass with glowing colored text,
// like the small backlit labels (MONO / TRCL / SEEK) on the reference units.
// `width`, when given, fixes the chip's footprint (and centers its text) so
// a column of these — e.g. the classification tag on every candidate row —
// lines up instead of jittering with "BEST" vs "INACCURACY".
const LedChip = ({ color, text, width }: { color: string; text: string; width?: number }) => (
    <span style={{
        display: 'inline-flex', justifyContent: width ? 'center' : 'flex-start',
        padding: '2px 6px', borderRadius: 4, boxSizing: 'border-box',
        background: 'rgba(0,0,0,0.5)', border: `1px solid ${color}55`, whiteSpace: 'nowrap',
        ...(width ? { width, flex: '0 0 auto' } : null),
    }}>
        <SegmentText text={text} color={color} height={8} gap={1.5} />
    </span>
);

// "EF 5.60 ±0.08" as one fixed-length segment array (like the move title).
// EF and its error are always exactly "D.DD" (single digit, two decimals),
// and the error is never omitted, so the format — not just the footprint —
// is fixed: "EF " (3) + "D.DD" (4) + " ±" (2) + "D.DD" (4) = 13.
const EF_TITLE_LEN = 13;

// Fixed footprint for the classification chip (best/excellent/.../blunder)
// — sized to the longest label, "INACCURACY".
const CLASS_CHIP_W = 80;

// Segmented LED bargraph — a row of discrete lit/unlit blocks standing in for
// a plain progress bar, echoing the graphic-equalizer displays in the refs.
const EQ_SEGMENTS = 20;
function EqBar({ pct, color }: { pct: number; color: string }) {
    const lit = Math.round((Math.max(0, Math.min(100, pct)) / 100) * EQ_SEGMENTS);
    return (
        <div style={{ display: 'flex', gap: 1.5, flex: '1 1 auto', height: 7 }}>
            {Array.from({ length: EQ_SEGMENTS }).map((_, i) => (
                <div
                    key={i}
                    style={{
                        flex: 1,
                        borderRadius: 1,
                        background: i < lit ? color : 'rgba(255,255,255,0.07)',
                        boxShadow: i < lit ? `0 0 3px ${color}aa` : 'none',
                        transition: 'background 160ms ease, box-shadow 160ms ease',
                    }}
                />
            ))}
        </div>
    );
}

/* ------------------- the click-to-open "why" detail panel ------------------ */

// Colored card-token strip ("10H*" grammar) as plain monospace text — the
// proof panel is prose-heavy, so tokens render inline rather than as segment
// glyphs. Red suits tinted like the row titles.
const TOKEN_SUIT = { S: '♠', H: '♥', C: '♣', D: '♦' } as Record<string, string>;
function TokenList({ tokens }: { tokens: string[] }) {
    return (
        <>
            {tokens.map((tok, i) => {
                const star = tok.endsWith('*');
                const body = star ? tok.slice(0, -1) : tok;
                const suit = body[body.length - 1];
                const red = suit === 'H' || suit === 'D';
                return (
                    <span key={i} style={{
                        fontFamily: LCD_MONO, fontWeight: 700,
                        color: red ? SUIT_RED : CARD_COLOR,
                        marginRight: 4, whiteSpace: 'nowrap',
                    }}>
                        {body.slice(0, -1)}{TOKEN_SUIT[suit] ?? suit}{star ? '·' : ''}
                    </span>
                );
            })}
        </>
    );
}

// One storyline-tree node row: indented, share bargraph, clause, and the
// expected finish on leaves. The tree is tiny (≤ ~10 nodes) — render it all.
function TreeRows({ nodes, depth, t }: { nodes: WhyTreeNode[]; depth: number; t: TFn }) {
    return (
        <>
            {nodes.map((nd, i) => (
                <React.Fragment key={`${depth}:${i}`}>
                    <div style={{
                        display: 'flex', alignItems: 'center', gap: 6,
                        paddingLeft: 6 + depth * 14, marginTop: 2,
                    }}>
                        <span style={{ ...ledText(TEAL), fontSize: '0.62rem', flex: '0 0 34px', textAlign: 'right' }}>
                            {Math.round(nd.share * 100)}%
                        </span>
                        <div style={{ flex: '0 0 46px' }}>
                            <EqBarMini pct={nd.share * 100} color={nd.sym === 2 ? '#D45B4E' : nd.sym === 4 ? '#4CAF7D' : TEAL} />
                        </div>
                        <span style={{ fontSize: '0.66rem', color: 'var(--color-text-primary)', opacity: 0.92 }}>
                            {nd.sym == null ? t('oracle_ev_resolved') : symPhrase(t, nd.sym)}
                            {nd.fin != null && (
                                <span style={{ ...ledText(AMBER), marginLeft: 5, fontSize: '0.6rem' }}>
                                    EF {nd.fin.toFixed(2)}
                                </span>
                            )}
                        </span>
                    </div>
                    {nd.children.length > 0 && <TreeRows nodes={nd.children} depth={depth + 1} t={t} />}
                </React.Fragment>
            ))}
        </>
    );
}

const EQ_MINI = 8;
function EqBarMini({ pct, color }: { pct: number; color: string }) {
    const lit = Math.round((Math.max(0, Math.min(100, pct)) / 100) * EQ_MINI);
    return (
        <div style={{ display: 'flex', gap: 1, height: 5 }}>
            {Array.from({ length: EQ_MINI }).map((_, i) => (
                <div key={i} style={{
                    flex: 1, borderRadius: 1,
                    background: i < lit ? color : 'rgba(255,255,255,0.07)',
                }} />
            ))}
        </div>
    );
}

const DetailTitle = ({ text }: { text: string }) => (
    <div style={{ marginTop: 7, marginBottom: 2 }}>
        <SegmentText text={text} color={TEAL} height={7} gap={1.2} />
    </div>
);

// The expanded panel under a clicked candidate row: the hard-coded proof
// (headline, likely reply, storyline tree, path clauses, counterfactual,
// marginals) plus octogen's belief block. All template strings + measured
// MC probabilities — no AI call anywhere near this (product requirement).
function WhyPanel({ c, bestCand, s, t }: {
    c: OracleCandidate; bestCand: OracleCandidate | null; s: OracleSnapshot; t: TFn;
}) {
    const why = useMemo(() => explainCandidate(t, c, bestCand, s), [t, c, bestCand, s]);
    const b = s.belief;
    const prose: React.CSSProperties = {
        fontSize: '0.68rem', lineHeight: 1.45, color: 'var(--color-text-primary)', opacity: 0.94,
    };
    return (
        <div data-testid="oracle-why" style={{
            margin: '2px 0 6px', padding: '6px 8px', borderRadius: 5,
            border: '1px solid rgba(94,234,212,0.25)', background: 'rgba(0,0,0,0.4)',
        }}>
            {why ? (
                <>
                    <div style={{ ...ledText(AMBER), fontSize: '0.7rem' }}>{why.headline}</div>
                    {why.reply && <div style={{ ...prose, marginTop: 4 }}>{why.reply}</div>}
                    {why.tree.length > 0 && (
                        <>
                            <DetailTitle text={t('oracle_tree_title')} />
                            <TreeRows nodes={why.tree} depth={0} t={t} />
                        </>
                    )}
                    {why.proof.length > 0 && (
                        <>
                            <DetailTitle text={t('oracle_why_title')} />
                            {why.proof.map((p, i) => (
                                <div key={i} style={{ ...prose, marginTop: i ? 3 : 0 }}>{p}</div>
                            ))}
                        </>
                    )}
                    {why.versusBest && (
                        <div style={{ ...prose, marginTop: 4, color: '#E0B341', opacity: 1 }}>{why.versusBest}</div>
                    )}
                    {why.marginals && (
                        <div style={{ ...prose, marginTop: 4, fontSize: '0.6rem', opacity: 0.62 }}>{why.marginals}</div>
                    )}
                </>
            ) : (
                <div style={{ ...prose, opacity: 0.6 }}>{t('oracle_why_nodata')}</div>
            )}
            {b && (
                <>
                    <DetailTitle text={t('oracle_belief_title')} />
                    <div style={prose}>
                        {(() => {
                            const rows: React.ReactNode[] = [];
                            for (let p = 0; p < (b.pinned?.length ?? 0); p++) {
                                if (p === s.seat) continue;
                                if (b.pinned[p]?.length) {
                                    rows.push(
                                        <div key={`pin${p}`}>
                                            {t('oracle_belief_pinned', { n: p + 1, cards: '' })}
                                            <TokenList tokens={b.pinned[p]} />
                                        </div>,
                                    );
                                }
                                if (b.voids[p]?.length) {
                                    rows.push(
                                        <div key={`void${p}`}>
                                            {t('oracle_belief_voids', { n: p + 1, cards: '' })}
                                            <TokenList tokens={b.voids[p]} />
                                        </div>,
                                    );
                                }
                                if (b.floor[p] > 0) {
                                    rows.push(
                                        <div key={`floor${p}`}>
                                            {t('oracle_belief_floor', { n: p + 1, v: VALUE_TO_RANK[b.floor[p]] ?? b.floor[p] })}
                                        </div>,
                                    );
                                }
                            }
                            if (rows.length === 0) rows.push(<div key="none">{t('oracle_belief_none')}</div>);
                            rows.push(<div key="pool" style={{ opacity: 0.62, marginTop: 2 }}>{t('oracle_belief_pool', { n: b.poolCount })}</div>);
                            return rows;
                        })()}
                    </div>
                </>
            )}
        </div>
    );
}

function McRow({ c, best, worst, bestAdj, t, open, onClick }: {
    c: OracleCandidate; best: number; worst: number; bestAdj: number | null;
    t: (id: any, p?: any) => string; open: boolean; onClick: () => void;
}) {
    // Bar, sort, classification and the displayed number ALL key off the true
    // expected finish (mean) — the 0.04/trump tie-break tax never distorts what
    // the user sees, so the bars always agree with the EF numbers.
    const eff = c.mean;
    const scored = eff != null;
    // Bar width replicates the X-ray formula (gen_html.py / multi_render.py).
    const span = Math.max(worst - best, 0.4);
    const barW = scored ? Math.max(12, Math.min(100, 96 - ((eff! - best) / span) * 84)) : 0;
    const isBest = scored && bestAdj != null && Math.abs(eff! - bestAdj) < 1e-9;
    const delta = scored && bestAdj != null ? eff! - bestAdj : 0;
    const cls = scored ? oracleClassify(delta) : null;
    const barColor = isBest ? CLASS_COLOR.best : (cls ? CLASS_COLOR[cls] : '#8a8a92');
    // EF and its error are always exactly "D.DD" and never omitted.
    const efText = scored ? `EF ${eff!.toFixed(2)} ±${c.se.toFixed(2)}` : (c.pruned ? '—' : '…');

    const { text: title, redAt } = moveTitleText(c);

    return (
        <div
            data-testid="oracle-row"
            onClick={onClick}
            style={{
                display: 'flex', flexDirection: 'column', gap: 3,
                padding: '5px 7px', borderRadius: 5, cursor: 'pointer',
                border: open ? `1px solid ${TEAL}88`
                    : c.played ? '1px solid rgba(255,165,60,0.5)' : '1px solid rgba(255,255,255,0.04)',
                background: c.played ? 'rgba(255,165,60,0.07)' : 'rgba(0,0,0,0.28)',
                opacity: scored ? 1 : 0.7,
                boxShadow: open ? `0 0 6px ${TEAL}33` : 'none',
            }}
        >
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <div style={{ flex: '1 1 auto', display: 'flex', alignItems: 'center', minWidth: 0 }}>
                    <SegmentText
                        text={title} color={CARD_COLOR} height={10} gap={1.5} length={MOVE_TITLE_LEN}
                        colorAt={(i) => (redAt.has(i) ? SUIT_RED : undefined)}
                    />
                </div>
                {c.played && <SolidChip bg={AMBER} fg="#231404" text={t('oracle_played')} />}
                {isBest && <LedChip color={CLASS_COLOR.best} text={t('oracle_best')} width={CLASS_CHIP_W} />}
                {!isBest && cls && <LedChip color={CLASS_COLOR[cls]} text={t(`oracle_class_${cls}`)} width={CLASS_CHIP_W} />}
                {c.pruned && !scored && <LedChip color="rgba(180,180,190,0.7)" text={t('oracle_pruned')} width={CLASS_CHIP_W} />}
                {c.forcedLoss && <SolidChip bg={VERDICT_COLOR.loss} text={t('oracle_forced_loss')} />}
            </div>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                {scored ? <EqBar pct={barW} color={barColor} />
                    : <div style={{ flex: '1 1 auto', height: 7 }} />}
                <span title={t('oracle_ef_tip')} style={{ display: 'inline-flex', alignItems: 'center', flex: '0 0 auto' }}>
                    <SegmentText text={efText} color={AMBER} height={10} gap={1.5} length={EF_TITLE_LEN} />
                </span>
            </div>
        </div>
    );
}

function VerdictRow({ c, t, open, onClick }: {
    c: OracleCandidate; t: (id: any, p?: any) => string; open: boolean; onClick: () => void;
}) {
    const barW = VERDICT_BAR[c.verdict] ?? 30;
    const color = VERDICT_COLOR[c.verdict] ?? VERDICT_COLOR.none;
    const depth = c.verdictVal != null ? 1000 - Math.abs(c.verdictVal) : null;
    const { text: title, redAt } = moveTitleText(c);
    const badge = c.verdict === 'win' ? `WIN${depth != null ? ` in ${depth}` : ''}`
        : c.verdict === 'loss' ? `LOSS${depth != null ? ` in ${depth}` : ''}`
        : c.verdict === 'draw' ? 'DRAW' : c.verdict === 'unknown' ? '?' : '';
    return (
        <div data-testid="oracle-row" onClick={onClick} style={{
            display: 'flex', flexDirection: 'column', gap: 3, padding: '5px 7px', borderRadius: 5,
            cursor: 'pointer',
            border: open ? `1px solid ${TEAL}88`
                : c.played ? '1px solid rgba(255,165,60,0.5)' : '1px solid rgba(255,255,255,0.04)',
            background: c.played ? 'rgba(255,165,60,0.07)' : 'rgba(0,0,0,0.28)',
            boxShadow: open ? `0 0 6px ${TEAL}33` : 'none',
        }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <div style={{ flex: '1 1 auto', display: 'flex', alignItems: 'center', minWidth: 0 }}>
                    <SegmentText
                        text={title} color={CARD_COLOR} height={10} gap={1.5} length={MOVE_TITLE_LEN}
                        colorAt={(i) => (redAt.has(i) ? SUIT_RED : undefined)}
                    />
                </div>
                {c.played && <SolidChip bg={AMBER} fg="#231404" text={t('oracle_played')} />}
                {badge && <SolidChip bg={color} fg={c.verdict === 'unknown' ? '#222' : '#fff'} text={badge} />}
            </div>
            <EqBar pct={barW} color={color} />
        </div>
    );
}

// Corner rivet — the tiny mounting screws visible on every reference unit.
const Rivet = ({ style }: { style: React.CSSProperties }) => (
    <div style={{
        position: 'absolute', width: 4, height: 4, borderRadius: '50%',
        background: 'radial-gradient(circle at 35% 30%, #8a8a90, #222226)',
        boxShadow: '0 0 1px rgba(0,0,0,0.8)',
        ...style,
    }} />
);

export const OracleOverlay = ({ snapshot, onClose, onToggleMemory, onRetry }: Props) => {
    const { t } = useLocalization();
    // The clicked candidate whose "why" panel is open. Keyed by canonical move
    // key so it survives re-sorting as estimates sharpen; explanation text is
    // computed on click only (§ product requirement), never while streaming.
    const [openKey, setOpenKey] = useState<string | null>(null);

    const scored = useMemo(
        () => (snapshot?.candidates ?? []).filter((c) => c.mean != null).map((c) => c.mean!),
        [snapshot],
    );
    const best = scored.length ? Math.min(...scored) : 0;
    const worst = scored.length ? Math.max(...scored) : 0;
    const bestAdj = scored.length ? best : null;

    const s = snapshot;
    const exact = s?.regime === 'exact';
    const running = s?.status === 'running' || s?.status === 'loading';

    const statusChip = (() => {
        if (!s) return null;
        if (s.status === 'exact') return <LedChip color={CLASS_COLOR.best} text={t('oracle_exact')} />;
        if (s.status === 'converged') return <LedChip color="#7FB6E8" text={t('oracle_converged')} />;
        if (s.status === 'forced') return <span style={{ ...ledText('rgba(190,190,200,0.75)'), fontSize: '0.6rem' }}>{t('oracle_forced_move')}</span>;
        if (running) return (
            <span style={{ display: 'inline-flex', alignItems: 'center', gap: 5, fontSize: '0.66rem', ...ledText(AMBER) }}>
                <Spinner />
                {t('oracle_analyzing', { n: (s.totalWorlds || 0).toLocaleString(), rate: (s.worldsPerSec || 0).toLocaleString() })}
            </span>
        );
        return null;
    })();

    return (
        <div
            data-testid="oracle-panel"
            style={{
                position: 'absolute',
                top: 'calc(44px + max(8px, env(safe-area-inset-top)))',
                right: 'max(10px, env(safe-area-inset-right))',
                zIndex: 1050,
                width: 'min(88vw, 340px)',
                maxHeight: '70vh',
                display: 'flex', flexDirection: 'column',
                borderRadius: 10,
                border: '1px solid rgba(255,255,255,0.16)',
                background: 'linear-gradient(180deg, #2b2b2f 0%, #17171a 8%, #101012 100%)',
                boxShadow: 'inset 0 1px 0 rgba(255,255,255,0.12), inset 0 -1px 0 rgba(0,0,0,0.7), 0 10px 30px rgba(0,0,0,0.6)',
                color: 'var(--color-text-primary)',
                overflow: 'hidden',
            }}
        >
            <style>{'@keyframes oracle-spin{to{transform:rotate(360deg)}}'}</style>
            {/* faint brushed-metal texture */}
            <div style={{
                position: 'absolute', inset: 0, pointerEvents: 'none', opacity: 0.5,
                background: 'repeating-linear-gradient(115deg, rgba(255,255,255,0.025) 0px, rgba(255,255,255,0.025) 1px, transparent 1px, transparent 3px)',
            }} />
            <Rivet style={{ top: 4, left: 4 }} />
            <Rivet style={{ top: 4, right: 4 }} />
            <Rivet style={{ bottom: 4, left: 4 }} />
            <Rivet style={{ bottom: 4, right: 4 }} />

            {/* header — dark LCD readout strip */}
            <div style={{
                position: 'relative', padding: '5px 12px 7px',
                borderBottom: '1px solid rgba(255,255,255,0.09)',
                background: 'linear-gradient(180deg, rgba(0,0,0,0.35), rgba(0,0,0,0.15))',
            }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                    <span style={{ flex: '1 1 auto' }} />
                    {statusChip}
                    <button onClick={onClose} aria-label="close" style={{
                        width: 18, height: 18, borderRadius: '50%', border: '1px solid rgba(0,0,0,0.5)', cursor: 'pointer',
                        background: `radial-gradient(circle at 35% 30%, ${TEAL}, #0f3a35)`,
                        boxShadow: `0 0 5px ${TEAL}88, inset 0 1px 1px rgba(255,255,255,0.5)`,
                        color: '#062421', fontSize: '0.72rem', fontWeight: 900, lineHeight: 1, padding: 0,
                    }}>×</button>
                </div>
                {s && s.status !== 'error' && (
                    <div style={{ marginTop: 1 }}>
                        <SegmentText text={`P${s.seat + 1} · ${s.recordedLabel}`} color="#7CE38C" height={10} gap={1.5} />
                    </div>
                )}
                {/* memory toggle */}
                {s && s.status !== 'error' && (
                    <button
                        data-testid="oracle-memory"
                        onClick={onToggleMemory}
                        title={s.memoryOn ? t('oracle_memory_on') : t('oracle_memory_off')}
                        style={{
                            marginTop: 6, display: 'inline-flex', alignItems: 'center', gap: 6,
                            padding: '3px 9px', borderRadius: 3, cursor: 'pointer',
                            border: s.memoryOn ? `1px solid ${TEAL}88` : '1px solid rgba(255,255,255,0.14)',
                            background: s.memoryOn ? 'rgba(94,234,212,0.14)' : 'rgba(255,255,255,0.04)',
                            boxShadow: s.memoryOn ? `0 0 6px ${TEAL}44` : 'none',
                        }}
                    >
                        <SegmentText
                            text={`${t('oracle_memory')}: ${s.memoryOn ? 'ON' : 'OFF'}`}
                            color={s.memoryOn ? TEAL : 'rgba(220,220,225,0.55)'}
                            height={9} gap={1.5}
                        />
                    </button>
                )}
            </div>

            {/* body */}
            <div style={{ position: 'relative', overflowY: 'auto', padding: '6px 6px 8px' }}>
                {!s || s.status === 'loading' ? (
                    <div style={{ padding: 16, textAlign: 'center', fontSize: '0.74rem', ...ledText(AMBER) }}>
                        <Spinner /> …
                    </div>
                ) : s.status === 'error' ? (
                    <div style={{ padding: 14, textAlign: 'center' }}>
                        <div style={{ fontSize: '0.78rem', marginBottom: 8, color: 'var(--color-text-primary)' }}>{t('oracle_unavailable')}</div>
                        <button onClick={onRetry} style={{
                            padding: '4px 14px', borderRadius: 4, cursor: 'pointer',
                            border: `1px solid ${TEAL}88`, background: 'rgba(94,234,212,0.12)',
                            boxShadow: `0 0 6px ${TEAL}33`,
                        }}>
                            <SegmentText text={t('oracle_retry')} color={TEAL} height={9} gap={1.5} />
                        </button>
                    </div>
                ) : (
                    <>
                        {s.candidates.map((c) => {
                            const open = openKey === c.key;
                            const onClick = () => setOpenKey(open ? null : c.key);
                            const bestCand = s.candidates.find((x) => x.mean != null) ?? null;
                            return (
                                <React.Fragment key={c.key}>
                                    {exact
                                        ? <VerdictRow c={c} t={t} open={open} onClick={onClick} />
                                        : <McRow c={c} best={best} worst={worst} bestAdj={bestAdj} t={t} open={open} onClick={onClick} />}
                                    {open && <WhyPanel c={c} bestCand={bestCand} s={s} t={t as unknown as TFn} />}
                                </React.Fragment>
                            );
                        })}
                        {openKey == null && s.status !== 'forced' && (
                            <div style={{ padding: '3px 7px 0', fontSize: '0.58rem', fontStyle: 'italic', ...ledText('rgba(200,200,210,0.55)') }}>
                                {t('oracle_detail_hint')}
                            </div>
                        )}
                        {!s.recordedPresent && s.status !== 'forced' && (
                            <div style={{ padding: '6px 7px', fontSize: '0.64rem', fontStyle: 'italic', ...ledText('rgba(210,210,216,0.6)') }}
                                title={t('oracle_pruned_tip')}>
                                <span style={{ textTransform: 'capitalize' }}>{s.recordedLabel}</span> — {t('oracle_pruned')}
                            </div>
                        )}
                        {/* footnotes */}
                        <div style={{ marginTop: 6, padding: '0 4px', display: 'flex', flexDirection: 'column', gap: 3 }}>
                            {s.approx && <Footnote text={t('oracle_approx')} />}
                            {!s.approx && s.deckAlive === false && !s.memoryOn && exact === false && s.regime === 'mc' && (
                                <Footnote text={t('oracle_memory_off_endgame')} />
                            )}
                            <Footnote text={t('oracle_basis')} muted />
                        </div>
                    </>
                )}
            </div>
        </div>
    );
};

const Footnote = ({ text, muted }: { text: string; muted?: boolean }) => (
    <div style={{ fontSize: '0.6rem', ...ledText('rgba(200,200,210,0.7)'), fontWeight: 500, opacity: muted ? 0.6 : 0.9 }}>{text}</div>
);

const Spinner = () => (
    <span
        aria-hidden
        style={{
            display: 'inline-block', width: 10, height: 10, borderRadius: '50%',
            border: `1.6px solid rgba(255,255,255,0.2)`, borderTopColor: TEAL,
            animation: 'oracle-spin 0.7s linear infinite',
        }}
    />
);
