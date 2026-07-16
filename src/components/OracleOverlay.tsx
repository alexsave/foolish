/* =============================================================================
 * Infinite Oracle — the replay overlay panel (docs/INFINITE_ORACLE_DESIGN.md §9)
 * Right-anchored panel mounted through GameBoard's chrome slot. Shows octogen's
 * read of every option at the paused decision: candidate rows with relative
 * bars, expected-finish ± SE, a chess.com-style classification chip, and the
 * "come into focus" animation as the standard errors shrink. Switches to verdict
 * mode when the exact endgame solver proves win/loss.
 * ========================================================================== */

import React, { useMemo } from 'react';
import { Card } from '@shared/core/types.ts';
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

interface Props {
    snapshot: OracleSnapshot | null;
    onClose: () => void;
    onToggleMemory: () => void;
    onRetry: () => void;
    renderCard: (card: Card, w?: number) => React.ReactNode;
}

const chipStyle = (bg: string, fg = '#fff'): React.CSSProperties => ({
    fontSize: '0.6rem',
    fontWeight: 700,
    letterSpacing: '0.02em',
    padding: '1px 5px',
    borderRadius: 6,
    background: bg,
    color: fg,
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

    const cards = c.cards.length
        ? <CardTokens tokens={c.cards} renderCard={renderCard} />
        : <span style={{ fontSize: '0.68rem', opacity: 0.8, textTransform: 'capitalize' }}>{c.type}</span>;

    return (
        <div
            style={{
                display: 'flex', flexDirection: 'column', gap: 2,
                padding: '5px 7px', borderRadius: 7,
                border: c.played ? '1px solid #E79743' : '1px solid transparent',
                background: c.played ? 'rgba(231,151,67,0.08)' : 'transparent',
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
                {c.played && <span style={chipStyle('#E79743', '#231404')}>{t('oracle_played')}</span>}
                {isBest && <span style={chipStyle(CLASS_COLOR.best)}>{t('oracle_best')}</span>}
                {!isBest && cls && <span style={chipStyle(CLASS_COLOR[cls])}>{t(`oracle_class_${cls}`)}</span>}
                {c.pruned && !scored && <span style={chipStyle('rgba(120,120,130,0.45)')}>{t('oracle_pruned')}</span>}
                {c.forcedLoss && <span style={chipStyle(VERDICT_COLOR.loss)}>{t('oracle_forced_loss')}</span>}
            </div>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <div style={{
                    flex: '1 1 auto', height: 7, borderRadius: 4,
                    background: 'rgba(255,255,255,0.08)', overflow: 'hidden',
                }}>
                    {scored && (
                        <div style={{
                            width: `${barW}%`, height: '100%', borderRadius: 4,
                            background: isBest ? CLASS_COLOR.best : (cls ? CLASS_COLOR[cls] : '#8a8a92'),
                            transition: 'width 220ms ease, background 220ms ease',
                        }} />
                    )}
                </div>
                <span
                    title={t('oracle_ef_tip')}
                    style={{
                        fontVariantNumeric: 'tabular-nums', fontSize: '0.68rem',
                        color: 'var(--color-text-muted)', minWidth: 66, textAlign: 'right',
                    }}
                >
                    {scored
                        ? <>EF {c.mean!.toFixed(decimals)}{c.se < 0.5 && c.se !== Infinity
                            ? <span style={{ opacity: 0.6 }}> ±{c.se.toFixed(2)}</span> : null}</>
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
            display: 'flex', flexDirection: 'column', gap: 2, padding: '5px 7px', borderRadius: 7,
            border: c.played ? '1px solid #E79743' : '1px solid transparent',
            background: c.played ? 'rgba(231,151,67,0.08)' : 'transparent',
        }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <div style={{ flex: '1 1 auto', display: 'flex', alignItems: 'center', gap: 4, minWidth: 0 }}>{cards}</div>
                {c.played && <span style={chipStyle('#E79743', '#231404')}>{t('oracle_played')}</span>}
                {badge && <span style={chipStyle(color, c.verdict === 'unknown' ? '#222' : '#fff')}>{badge}</span>}
            </div>
            <div style={{ height: 7, borderRadius: 4, background: 'rgba(255,255,255,0.08)', overflow: 'hidden' }}>
                <div style={{ width: `${barW}%`, height: '100%', background: color, borderRadius: 4 }} />
            </div>
        </div>
    );
}

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
        if (s.status === 'exact') return <span style={chipStyle('#4CAF7D')}>{t('oracle_exact')}</span>;
        if (s.status === 'converged') return <span style={chipStyle('#5b7fb0')}>{t('oracle_converged')}</span>;
        if (s.status === 'forced') return <span style={chipStyle('rgba(150,150,160,0.5)')}>{t('oracle_forced_move')}</span>;
        if (running) return (
            <span style={{ display: 'inline-flex', alignItems: 'center', gap: 5, fontSize: '0.68rem', color: 'var(--color-text-muted)' }}>
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
                borderRadius: 12,
                border: '1px solid rgba(255,255,255,0.14)',
                background: 'linear-gradient(180deg, rgba(26,26,30,0.97) 0%, rgba(18,18,20,0.98) 100%)',
                boxShadow: '0 8px 28px rgba(0,0,0,0.55)',
                color: 'var(--color-text-primary)',
                overflow: 'hidden',
            }}
        >
            <style>{'@keyframes oracle-spin{to{transform:rotate(360deg)}}'}</style>
            {/* header */}
            <div style={{ padding: '8px 10px', borderBottom: '1px solid rgba(255,255,255,0.08)' }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                    <span style={{ fontSize: '0.78rem', fontWeight: 800, letterSpacing: '0.04em' }}>
                        🔮 {t('oracle_panel_title')}
                    </span>
                    <span style={{ flex: '1 1 auto' }} />
                    {statusChip}
                    <button onClick={onClose} aria-label="close" style={{
                        width: 20, height: 20, borderRadius: '50%', border: 'none', cursor: 'pointer',
                        background: 'rgba(255,255,255,0.08)', color: 'var(--color-text-primary)', fontSize: '0.8rem', lineHeight: 1,
                    }}>×</button>
                </div>
                {s && s.status !== 'error' && (
                    <div style={{ marginTop: 4, fontSize: '0.72rem', color: 'var(--color-text-muted)' }}>
                        <span style={{ fontWeight: 700, color: 'var(--color-text-primary)' }}>P{s.seat + 1}</span>
                        {' · '}<span style={{ textTransform: 'capitalize' }}>{s.recordedLabel}</span>
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
                            padding: '2px 8px', borderRadius: 999, cursor: 'pointer',
                            border: '1px solid rgba(255,255,255,0.16)',
                            background: s.memoryOn ? 'rgba(231,151,67,0.18)' : 'rgba(255,255,255,0.05)',
                            color: 'var(--color-text-primary)', fontSize: '0.66rem', fontWeight: 700,
                        }}
                    >
                        {t('oracle_memory')}: {s.memoryOn ? 'on' : 'off'}
                    </button>
                )}
            </div>

            {/* body */}
            <div style={{ overflowY: 'auto', padding: '6px 6px 8px' }}>
                {!s || s.status === 'loading' ? (
                    <div style={{ padding: 16, textAlign: 'center', color: 'var(--color-text-muted)', fontSize: '0.74rem' }}>
                        <Spinner /> …
                    </div>
                ) : s.status === 'error' ? (
                    <div style={{ padding: 14, textAlign: 'center' }}>
                        <div style={{ fontSize: '0.78rem', marginBottom: 8 }}>{t('oracle_unavailable')}</div>
                        <button onClick={onRetry} style={{
                            padding: '4px 12px', borderRadius: 8, cursor: 'pointer',
                            border: '1px solid rgba(255,255,255,0.2)', background: 'rgba(255,255,255,0.06)',
                            color: 'var(--color-text-primary)', fontSize: '0.72rem', fontWeight: 700,
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
                            <div style={{ padding: '6px 7px', fontSize: '0.66rem', color: 'var(--color-text-muted)', fontStyle: 'italic' }}
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
    <div style={{ fontSize: '0.62rem', color: 'var(--color-text-muted)', opacity: muted ? 0.6 : 0.85 }}>{text}</div>
);

const Spinner = () => (
    <span
        aria-hidden
        style={{
            display: 'inline-block', width: 10, height: 10, borderRadius: '50%',
            border: '1.6px solid rgba(255,255,255,0.25)', borderTopColor: '#E79743',
            animation: 'oracle-spin 0.7s linear infinite',
        }}
    />
);
