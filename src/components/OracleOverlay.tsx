/* =============================================================================
 * Infinite Oracle — the replay overlay panel (docs/INFINITE_ORACLE_DESIGN.md §9)
 * Right-anchored panel mounted through GameBoard's chrome slot. Shows octogen's
 * read of every option at the paused decision: candidate rows with relative
 * bars, expected-finish ± SE, a chess.com-style classification chip, and the
 * "come into focus" animation as the standard errors shrink. Switches to verdict
 * mode when the exact endgame solver proves win/loss.
 *
 * Styled as a vintage car-stereo graphic-equalizer faceplate: brushed-metal
 * bezel, amber LED digital readouts, teal illuminated hardware buttons, and
 * segmented EQ-style bargraphs standing in for the plain progress bars.
 * ========================================================================== */

import React, { useMemo } from 'react';
import { Card } from '@api/core/types.ts';
import { useLocalization } from '../contexts/LocalizationContext';
import {
    OracleSnapshot, OracleCandidate, oracleClassify, OracleClass,
} from '../oracle/types';

// Inverse of oracleCardToken (types.ts): recover the decoded Card so the real
// CardFace renders it. Rank strings are OG_EX_VAL; suit is "SHCD"; trailing '*'
// (trump marker) is decorative.
const RANK_TO_VALUE: Record<string, number> = {
    '2': 1, '3': 2, '4': 3, '5': 4, '6': 5, '7': 6, '8': 7, '9': 8,
    '10': 9, J: 10, Q: 11, K: 12, A: 13,
};
function tokenToCard(token: string): Card | null {
    const body = token.endsWith('*') ? token.slice(0, -1) : token;
    if (body.length < 2) return null;
    const suit = 'SHCD'.indexOf(body[body.length - 1]);
    const value = RANK_TO_VALUE[body.slice(0, -1)];
    if (suit < 0 || value == null) return null;
    return { suit, value };
}

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
    renderCard: (card: Card, w?: number) => React.ReactNode;
}

// Filled "hardware label" chip — mimics the amber/colored solid indicator
// tags stenciled onto old car-stereo faceplates (e.g. "PLAYED", "WIN").
const solidChipStyle = (bg: string, fg = '#fff'): React.CSSProperties => ({
    fontFamily: LCD_MONO,
    fontSize: '0.6rem',
    fontWeight: 700,
    letterSpacing: '0.03em',
    padding: '1px 5px',
    borderRadius: 4,
    background: bg,
    color: fg,
    whiteSpace: 'nowrap',
});

// Outline "LCD segment" chip — dark display glass with glowing colored text,
// like the small backlit labels (MONO / TRCL / SEEK) on the reference units.
const ledChipStyle = (color: string): React.CSSProperties => ({
    ...ledText(color),
    fontSize: '0.58rem',
    textTransform: 'uppercase',
    padding: '1px 6px',
    borderRadius: 4,
    background: 'rgba(0,0,0,0.5)',
    border: `1px solid ${color}55`,
    whiteSpace: 'nowrap',
});

function CardTokens({ tokens, renderCard }: { tokens: string[]; renderCard: Props['renderCard'] }) {
    return (
        <span style={{ display: 'inline-flex', gap: 1, alignItems: 'center' }}>
            {tokens.map((tk, i) => {
                const c = tokenToCard(tk);
                return c ? <React.Fragment key={i}>{renderCard(c, 17)}</React.Fragment>
                    : <span key={i} style={{ fontSize: '0.66rem' }}>{tk}</span>;
            })}
        </span>
    );
}

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

function McRow({ c, best, worst, bestAdj, renderCard, t }: {
    c: OracleCandidate; best: number; worst: number; bestAdj: number | null;
    renderCard: Props['renderCard']; t: (id: any, p?: any) => string;
}) {
    // Bar, sort, classification and the displayed number ALL key off the true
    // expected finish (mean) — the 0.04/trump tie-break tax never distorts what
    // the user sees, so the bars always agree with the EF numbers.
    const eff = c.mean;
    const scored = eff != null;
    // Bar width replicates the X-ray formula (gen_html.py / multi_render.py).
    const span = Math.max(worst - best, 0.4);
    const barW = scored ? Math.max(12, Math.min(100, 96 - ((eff! - best) / span) * 84)) : 0;
    const decimals = c.se < 0.15 ? 2 : 0;
    const isBest = scored && bestAdj != null && Math.abs(eff! - bestAdj) < 1e-9;
    const delta = scored && bestAdj != null ? eff! - bestAdj : 0;
    const cls = scored ? oracleClassify(delta) : null;
    const barColor = isBest ? CLASS_COLOR.best : (cls ? CLASS_COLOR[cls] : '#8a8a92');

    const cards = c.cards.length
        ? <CardTokens tokens={c.cards} renderCard={renderCard} />
        : <span style={{ fontSize: '0.68rem', opacity: 0.8, textTransform: 'capitalize' }}>{c.type}</span>;

    return (
        <div
            style={{
                display: 'flex', flexDirection: 'column', gap: 3,
                padding: '5px 7px', borderRadius: 5,
                border: c.played ? '1px solid rgba(255,165,60,0.5)' : '1px solid rgba(255,255,255,0.04)',
                background: c.played ? 'rgba(255,165,60,0.07)' : 'rgba(0,0,0,0.28)',
                opacity: scored ? 1 : 0.7,
            }}
        >
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <div style={{ flex: '1 1 auto', display: 'flex', alignItems: 'center', gap: 4, minWidth: 0 }}>
                    {c.target?.length ? (
                        <>{cards}<span style={{ opacity: 0.55, fontSize: '0.66rem' }}>→</span>
                            <CardTokens tokens={c.target} renderCard={renderCard} /></>
                    ) : cards}
                </div>
                {c.played && <span style={solidChipStyle(AMBER, '#231404')}>{t('oracle_played')}</span>}
                {isBest && <span style={ledChipStyle(CLASS_COLOR.best)}>{t('oracle_best')}</span>}
                {!isBest && cls && <span style={ledChipStyle(CLASS_COLOR[cls])}>{t(`oracle_class_${cls}`)}</span>}
                {c.pruned && !scored && <span style={ledChipStyle('rgba(180,180,190,0.7)')}>{t('oracle_pruned')}</span>}
                {c.forcedLoss && <span style={solidChipStyle(VERDICT_COLOR.loss)}>{t('oracle_forced_loss')}</span>}
            </div>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                {scored ? <EqBar pct={barW} color={barColor} />
                    : <div style={{ flex: '1 1 auto', height: 7 }} />}
                <span
                    title={t('oracle_ef_tip')}
                    style={{
                        ...ledText(AMBER),
                        fontVariantNumeric: 'tabular-nums', fontSize: '0.66rem',
                        minWidth: 66, textAlign: 'right',
                    }}
                >
                    {scored
                        ? <>EF {c.mean!.toFixed(decimals)}{c.se < 0.5 && c.se !== Infinity
                            ? <span style={{ opacity: 0.65 }}> ±{c.se.toFixed(2)}</span> : null}</>
                        : (c.pruned ? '—' : '…')}
                </span>
            </div>
        </div>
    );
}

function VerdictRow({ c, renderCard, t }: {
    c: OracleCandidate; renderCard: Props['renderCard']; t: (id: any, p?: any) => string;
}) {
    const barW = VERDICT_BAR[c.verdict] ?? 30;
    const color = VERDICT_COLOR[c.verdict] ?? VERDICT_COLOR.none;
    const depth = c.verdictVal != null ? 1000 - Math.abs(c.verdictVal) : null;
    const cards = c.cards.length
        ? (c.target?.length
            ? <><CardTokens tokens={c.cards} renderCard={renderCard} /><span style={{ opacity: 0.55 }}>→</span><CardTokens tokens={c.target} renderCard={renderCard} /></>
            : <CardTokens tokens={c.cards} renderCard={renderCard} />)
        : <span style={{ fontSize: '0.68rem', opacity: 0.8, textTransform: 'capitalize' }}>{c.type}</span>;
    const badge = c.verdict === 'win' ? `WIN${depth != null ? ` in ${depth}` : ''}`
        : c.verdict === 'loss' ? `LOSS${depth != null ? ` in ${depth}` : ''}`
        : c.verdict === 'draw' ? 'DRAW' : c.verdict === 'unknown' ? '?' : '';
    return (
        <div style={{
            display: 'flex', flexDirection: 'column', gap: 3, padding: '5px 7px', borderRadius: 5,
            border: c.played ? '1px solid rgba(255,165,60,0.5)' : '1px solid rgba(255,255,255,0.04)',
            background: c.played ? 'rgba(255,165,60,0.07)' : 'rgba(0,0,0,0.28)',
        }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <div style={{ flex: '1 1 auto', display: 'flex', alignItems: 'center', gap: 4, minWidth: 0 }}>{cards}</div>
                {c.played && <span style={solidChipStyle(AMBER, '#231404')}>{t('oracle_played')}</span>}
                {badge && <span style={solidChipStyle(color, c.verdict === 'unknown' ? '#222' : '#fff')}>{badge}</span>}
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

export const OracleOverlay = ({ snapshot, onClose, onToggleMemory, onRetry, renderCard }: Props) => {
    const { t } = useLocalization();

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
        if (s.status === 'exact') return <span style={ledChipStyle(CLASS_COLOR.best)}>{t('oracle_exact')}</span>;
        if (s.status === 'converged') return <span style={ledChipStyle('#7FB6E8')}>{t('oracle_converged')}</span>;
        if (s.status === 'forced') return <span style={ledChipStyle('rgba(190,190,200,0.75)')}>{t('oracle_forced_move')}</span>;
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
                position: 'relative', padding: '8px 12px 7px',
                borderBottom: '1px solid rgba(255,255,255,0.09)',
                background: 'linear-gradient(180deg, rgba(0,0,0,0.35), rgba(0,0,0,0.15))',
            }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                    <span style={{ ...ledText(AMBER), fontSize: '0.8rem', textTransform: 'uppercase' }}>
                        🔮 {t('oracle_panel_title')}
                    </span>
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
                    <div style={{ marginTop: 4, fontSize: '0.68rem', ...ledText('#7CE38C') }}>
                        <span>P{s.seat + 1}</span>
                        {' · '}<span style={{ textTransform: 'uppercase' }}>{s.recordedLabel}</span>
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
                            padding: '2px 9px', borderRadius: 3, cursor: 'pointer',
                            border: s.memoryOn ? `1px solid ${TEAL}88` : '1px solid rgba(255,255,255,0.14)',
                            background: s.memoryOn ? 'rgba(94,234,212,0.14)' : 'rgba(255,255,255,0.04)',
                            boxShadow: s.memoryOn ? `0 0 6px ${TEAL}44` : 'none',
                            ...ledText(s.memoryOn ? TEAL : 'rgba(220,220,225,0.55)'),
                            fontSize: '0.62rem', textTransform: 'uppercase',
                        }}
                    >
                        {t('oracle_memory')}: {s.memoryOn ? 'on' : 'off'}
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
                            ...ledText(TEAL), fontSize: '0.7rem', textTransform: 'uppercase',
                        }}>{t('oracle_retry')}</button>
                    </div>
                ) : (
                    <>
                        {s.candidates.map((c) => (
                            exact
                                ? <VerdictRow key={c.key} c={c} renderCard={renderCard} t={t} />
                                : <McRow key={c.key} c={c} best={best} worst={worst} bestAdj={bestAdj} renderCard={renderCard} t={t} />
                        ))}
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
