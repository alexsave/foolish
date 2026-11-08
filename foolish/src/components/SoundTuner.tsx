import { useCallback, useEffect, useRef, useState } from 'react';
import { botsReady, cueMix, cueTakes } from '@sdk/ts/wasm/bots.ts';
import { ANIM_CUE_NAMES, ANIM_N_CUES, ANIM_CUE_REVERT } from '@sdk/ts/gen/kernel_consts.bots.ts';

// THE TUNER on /sounds: the timing and level of every part of every sound, by ear.
//
// DEV ONLY. SoundAudition loads it only when NODE_ENV is development, so a
// production build never has it. It edits tools/soundgen/tuning.tsv through the
// local endpoint `npm run tune` starts (tools/soundgen/tune.c), which rebuilds
// every take, the take table and the kernel links from that file; the page then
// reloads to play them. It holds no value of its own: what it shows is the
// file's, "changed" is against the file in git HEAD, and when Alex is happy the
// file is what gets committed (docs/AUDIO.md "How to tune").

const TUNE_URL = 'http://127.0.0.1:3101';
const RELOAD_KEY = 'foolish_tuner_reload';

/** How a row of the tuner plays its cue: as a strike with time for its take, as
 *  a run's opening strike (a tight take), or with strokes before it. */
export type TunerPlay = (kind: number, how: 'main' | 'tight' | number) => void;

// ---- tuning.tsv, as lines with tab-separated fields ---------------------------------
// The file is edited in place: a value is one field of one line, and every other
// line (the comments) goes back as it came.
interface PartRow { kind: 'part'; line: number; cue: string; role: string; part: string; at: number; atSpread: number; db: number; dbSpread: number; what: string }
interface MixRow { kind: 'mix'; line: number; cue: string; gain: number }
type Row = PartRow | MixRow;

const parse = (text: string): Row[] => text.split('\n').flatMap((l, line): Row[] => {
    const f = l.split('\t');
    if (f[0] === 'part' && f.length === 9) {
        return [{ kind: 'part', line, cue: f[1], role: f[2], part: f[3], at: Number(f[4]), atSpread: Number(f[5]),
                  db: Number(f[6]), dbSpread: Number(f[7]), what: f[8] }];
    }
    if (f[0] === 'mix' && f.length === 6) return [{ kind: 'mix', line, cue: f[1], gain: Number(f[2]) }];
    return [];
});
const same = (a: Row | undefined, b: Row): boolean =>
    !!a && (a.kind === 'part' && b.kind === 'part'
        ? a.at === b.at && a.atSpread === b.atSpread && a.db === b.db && a.dbSpread === b.dbSpread
        : a.kind === 'mix' && b.kind === 'mix' && a.gain === b.gain);
const keyOf = (r: Row) => (r.kind === 'part' ? `part ${r.cue} ${r.role} ${r.part}` : `mix ${r.cue}`);
/** A number as the file writes it: no exponent, no float noise. */
const num = (v: number, places: number) => String(Number(v.toFixed(places)));
const setField = (text: string, line: number, field: number, value: string): string => {
    const lines = text.split('\n');
    const f = lines[line].split('\t');
    f[field] = value;
    lines[line] = f.join('\t');
    return lines.join('\n');
};

const dbOfGain = (pm: number) => (pm > 0 ? 20 * Math.log10(pm / 1000) : -Infinity);
const gainOfDb = (db: number) => Math.max(0, Math.min(8000, Math.round(1000 * 10 ** (db / 20))));

/** What tune.sh's "table" line sums, from the table this page's kernel carries:
 *  the same number means the reload has the new kernel link. */
const tableFingerprint = (): number => {
    let s = 0;
    for (const t of cueTakes()) s += t.nSamples + t.anchorUs;
    for (let k = 1; k < ANIM_N_CUES; k++) {
        const m = cueMix(k);
        if (m) s += m.gainPm + m.pitchPm + m.levelPm + m.perCardPm;
    }
    return s;
};

/** A reload the tuner asked for: the table it expects, where the page was, what to say. Kept
 *  for this tab only, as "fp scroll tries said". */
/** The kernel links the dev server serves now, by their hashed file names, read from
 *  the chunk that names them. A reload before the dev server has seen a relink would
 *  load the old link and then be hot-patched under the page, so the tuner reloads
 *  only once these change. */
let linkChunk: string | null = null;
const LINKS = /(?:web|promo)\.wasm\.[\w~-]+\.gz/g;
const servedLinks = async (): Promise<string> => {
    const chunks = linkChunk ? [linkChunk] : performance.getEntriesByType('resource').map((e) => e.name)
        .filter((n) => n.includes('/_next/static/chunks/') && n.endsWith('.js'));
    for (const url of chunks) {
        const names = (await (await fetch(url, { cache: 'no-store' })).text()).match(LINKS);
        if (names) { linkChunk = url; return [...new Set(names)].sort().join(' '); }
    }
    return '';
};
const until = async (changed: () => Promise<boolean>, ms: number): Promise<void> => {
    for (const end = performance.now() + ms; performance.now() < end; await new Promise((r) => setTimeout(r, 200))) {
        if (await changed()) return;
    }
};

interface Pending { fp: number; scroll: number; said: string; tries: number }
const readPending = (): Pending | null => {
    try {
        const v = sessionStorage.getItem(RELOAD_KEY);
        const m = v ? /^(\d+) (\d+) (\d+) (.*)$/s.exec(v) : null;
        return m ? { fp: Number(m[1]), scroll: Number(m[2]), tries: Number(m[3]), said: m[4] } : null;
    } catch { return null; }
};
const writePending = (p: Pending | null): void => {
    try {
        if (p) sessionStorage.setItem(RELOAD_KEY, `${p.fp} ${Math.round(p.scroll)} ${p.tries} ${p.said}`);
        else sessionStorage.removeItem(RELOAD_KEY);
    } catch { /* private mode */ }
};

const cueKind = (name: string): number => {
    for (let k = 1; k < ANIM_N_CUES; k++) if ((ANIM_CUE_NAMES[k] ?? '').toLowerCase() === name) return k;
    return -1;
};

// ---- look ---------------------------------------------------------------------------
const small: React.CSSProperties = {
    background: '#3a2c22', color: '#f5e6c8', border: '1px solid #6b5443', borderRadius: 6,
    padding: '2px 8px', font: 'inherit', fontSize: 13, cursor: 'pointer', minWidth: 26,
};
const playBtn: React.CSSProperties = { ...small, background: '#5b7a3a', borderColor: '#7d9c58', minWidth: 54 };
const input: React.CSSProperties = {
    width: 58, background: '#1b130e', color: '#f5e6c8', border: '1px solid #6b5443', borderRadius: 6,
    padding: '2px 4px', font: 'inherit', fontSize: 13, textAlign: 'right',
};
const td: React.CSSProperties = { padding: '3px 6px', verticalAlign: 'middle', whiteSpace: 'nowrap' };
const changedColor = '#f0b429';

/** A number with a step down, a field and a step up. Typing commits on Enter or leaving the field. */
const Stepper = ({ value, step, places, onChange, label, changed }: {
    value: number; step: number; places: number; onChange: (v: number) => void; label: string; changed: boolean;
}) => {
    const [text, setText] = useState(num(value, places));
    useEffect(() => setText(num(value, places)), [value, places]);
    const commit = () => { const v = Number(text); if (Number.isFinite(v) && v !== value) onChange(v); else setText(num(value, places)); };
    return (
        <span style={{ display: 'inline-flex', gap: 3, alignItems: 'center' }}>
            <button type="button" style={small} aria-label={`${label} down`} onClick={() => onChange(value - step)}>-</button>
            <input style={{ ...input, borderColor: changed ? changedColor : input.borderColor }} value={text} aria-label={label}
                   onChange={(e) => setText(e.target.value)} onBlur={commit}
                   onKeyDown={(e) => { if (e.key === 'Enter') commit(); }} />
            <button type="button" style={small} aria-label={`${label} up`} onClick={() => onChange(value + step)}>+</button>
        </span>
    );
};

export default function SoundTuner({ play, perStroke }: { play: TunerPlay; perStroke: (kind: number) => number }) {
    const [text, setText] = useState<string | null>(null);
    const [committed, setCommitted] = useState<Map<string, Row>>(new Map());
    const [status, setStatus] = useState<string>('');
    const [log, setLog] = useState<string>('');
    const [busy, setBusy] = useState(false);
    const [off, setOff] = useState(false);
    const [strokes, setStrokes] = useState<Record<string, number>>({});
    // The text the endpoint has been sent, and the newest the page holds: a
    // change made while a rebuild runs is sent when it answers.
    const sent = useRef<string | null>(null);
    const latest = useRef<string | null>(null);

    // After a rebuild the page reloads; once the kernel is here it checks the
    // table is the new one, and reloads again (briefly) if the dev server had
    // not caught up.
    useEffect(() => {
        const p = readPending();
        if (!p) return;
        window.scrollTo(0, p.scroll);
        let stop = false;
        const check = () => {
            if (stop) return;
            if (!botsReady()) { setTimeout(check, 100); return; }
            if (tableFingerprint() === p.fp || p.tries >= 12) {
                writePending(null);
                setStatus(p.tries >= 12 ? `${p.said} (but this page's kernel does not have the new table: reload)` : p.said);
                requestAnimationFrame(() => window.scrollTo(0, p.scroll));
            } else {
                writePending({ ...p, tries: p.tries + 1 });
                setTimeout(() => window.location.reload(), 400);
            }
        };
        check();
        return () => { stop = true; };
    }, []);

    useEffect(() => {
        Promise.all([
            fetch(`${TUNE_URL}/tuning`, { cache: 'no-store' }).then((r) => (r.ok ? r.text() : Promise.reject(new Error()))),
            fetch(`${TUNE_URL}/tuning/committed`, { cache: 'no-store' }).then((r) => (r.ok ? r.text() : '')),
        ]).then(([now, head]) => {
            setText(now);
            sent.current = now;
            latest.current = now;
            setCommitted(new Map(parse(head).map((r) => [keyOf(r), r])));
        }, () => setOff(true));
    }, []);

    const send = useCallback((body: string) => {
        sent.current = body;
        setBusy(true);
        setStatus('Rebuilding the takes...');
        const t0 = performance.now();
        let links = '';
        servedLinks().catch(() => '').then((l) => { links = l; })
            .then(() => fetch(`${TUNE_URL}/tuning`, { method: 'POST', body, headers: { 'Content-Type': 'text/plain' } }))
            .then(async (r) => {
                const out = await r.text();
                if (r.ok && out.includes('relinked')) {
                    setStatus('Waiting for the dev server to serve the new kernel...');
                    await until(async () => (await servedLinks().catch(() => links)) !== links, 10000);
                }
                return { ok: r.ok, out };
            })
            .then(({ ok, out }) => {
                if (latest.current !== body) { send(latest.current ?? body); return; }
                setBusy(false);
                if (!ok) {
                    setStatus('soundgen refused it; tuning.tsv is as it was');
                    setLog(out);
                    return;
                }
                const fp = /tune\.sh: table (\d+)/.exec(out);
                const gain = /common gain ([+-][\d.]+ dB)/.exec(out);
                const said = `Rebuilt in ${((performance.now() - t0) / 1000).toFixed(1)} s${gain ? `; the common gain is ${gain[1]}` : ''}`;
                if (!fp) { setStatus(said); return; }
                writePending({ fp: Number(fp[1]), scroll: window.scrollY, said, tries: 0 });
                window.location.reload();
            }, () => { setBusy(false); setOff(true); });
    }, []);

    const edit = (next: string) => {
        setText(next);
        latest.current = next;
        if (!busy) send(next);
    };

    if (off) {
        return (
            <p style={{ border: `1px solid ${changedColor}`, borderRadius: 8, padding: 10 }}>
                The tuner needs its endpoint: run <code>npm run tune</code> beside <code>npm run dev</code>, then reload.
            </p>
        );
    }
    if (text === null) return <p>Loading the tuning...</p>;

    const rows = parse(text);
    const parts = rows.filter((r): r is PartRow => r.kind === 'part');
    const mixes = rows.filter((r): r is MixRow => r.kind === 'mix');
    // The cues in the file's order of parts, then the cues with a mix line only (the stand-ins).
    const order = [...new Set([...parts.map((r) => r.cue), ...mixes.map((r) => r.cue)])]
        .filter((c) => cueKind(c) > 0 && cueKind(c) !== ANIM_CUE_REVERT);
    const changedCount = rows.filter((r) => !same(committed.get(keyOf(r)), r)).length;

    const playRow = (cue: string, how: 'main' | 'tight' | number) => play(cueKind(cue), how);

    return (
        <section data-tuner style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
            <h2 className="title">Tune</h2>
            <p style={{ margin: 0, opacity: 0.85 }}>
                Every part&apos;s level (dB, on the deal&apos;s scale, before the common gain that puts the loudest take
                at -3 dBFS, so raising the loudest lowers the rest) and time (ms from the cue&apos;s moment, negative
                before it). A change rebuilds the takes it touches and reloads the page, in a second or two; then
                press Play, or play a game at <a href="/ws" style={{ color: 'inherit' }}>/ws</a> (reload it after a change). Values come from <code>tools/soundgen/tuning.tsv</code> and are
                marked against git HEAD: when it sounds right, commit that file.
            </p>
            <div style={{ display: 'flex', gap: 12, alignItems: 'center', flexWrap: 'wrap', minHeight: 26 }}>
                <span data-tuner-status style={{ color: busy ? changedColor : undefined }}>{status || 'Ready'}</span>
                <span style={{ opacity: 0.8 }}>{changedCount ? `${changedCount} changed against HEAD` : 'nothing changed against HEAD'}</span>
                {changedCount > 0 && (
                    <button type="button" style={small} onClick={() => {
                        let next = text;
                        for (const r of rows) {
                            const c = committed.get(keyOf(r));
                            if (!c) continue;
                            if (r.kind === 'part' && c.kind === 'part') {
                                next = setField(next, r.line, 4, num(c.at, 1));
                                next = setField(next, r.line, 5, num(c.atSpread, 1));
                                next = setField(next, r.line, 6, num(c.db, 2));
                                next = setField(next, r.line, 7, num(c.dbSpread, 2));
                            } else if (r.kind === 'mix' && c.kind === 'mix') next = setField(next, r.line, 2, String(c.gain));
                        }
                        edit(next);
                    }}>Reset all</button>
                )}
            </div>
            {log && <pre style={{ whiteSpace: 'pre-wrap', fontSize: 12, background: '#1b130e', padding: 8, borderRadius: 6 }}>{log}</pre>}
            <div style={{ overflowX: 'auto' }}>
            <table style={{ borderCollapse: 'collapse', width: '100%', fontSize: 14 }}>
                <thead>
                    <tr style={{ textAlign: 'left', opacity: 0.7 }}>
                        <th style={td}>Sound</th><th style={td}>Level dB</th><th style={td}>Time ms</th><th style={td} /><th style={td} />
                    </tr>
                </thead>
                <tbody>
                    {order.map((cue) => {
                        const kind = cueKind(cue);
                        const own = parts.filter((r) => r.cue === cue);
                        const mix = mixes.find((r) => r.cue === cue);
                        const mixHead = mix ? committed.get(keyOf(mix)) : undefined;
                        const mixChanged = !!mix && (!mixHead || (mixHead.kind === 'mix' && mixHead.gain !== mix.gain));
                        const shift = (ms: number) => {
                            let next = text;
                            for (const r of own) if (r.role !== 'prelude') next = setField(next, r.line, 4, num(r.at + ms, 1));
                            edit(next);
                        };
                        return [
                            mix && (
                                <tr key={`mix ${cue}`} data-tuner-row={`mix ${cue}`} style={{ borderTop: '1px solid #4a382c' }}>
                                    <td style={td}>
                                        <strong>{cue.replace('_', ' ')}</strong>: whole cue
                                        {mixChanged && <span title="changed against HEAD" style={{ color: changedColor }}> ●</span>}
                                    </td>
                                    <td style={td}>
                                        <Stepper value={Number(dbOfGain(mix.gain).toFixed(1))} step={1} places={1} label={`${cue} whole cue dB`}
                                                 changed={mixChanged}
                                                 onChange={(v) => edit(setField(text, mix.line, 2, String(gainOfDb(v))))} />
                                    </td>
                                    <td style={td}>
                                        {own.some((r) => r.role !== 'prelude') && (
                                            <span style={{ display: 'inline-flex', gap: 3, alignItems: 'center' }}
                                                  title="moves every part of the cue (not its strokes) against the animation">
                                                <button type="button" style={small} onClick={() => shift(-10)}>-10</button>
                                                <span style={{ opacity: 0.7, fontSize: 12 }}>all parts</span>
                                                <button type="button" style={small} onClick={() => shift(10)}>+10</button>
                                            </span>
                                        )}
                                    </td>
                                    <td style={td}><button type="button" style={playBtn} onClick={() => playRow(cue, 'main')}>Play</button></td>
                                    <td style={td}>
                                        <button type="button" style={{ ...small, visibility: mixChanged && mixHead?.kind === 'mix' ? 'visible' : 'hidden' }}
                                                onClick={() => { if (mixHead?.kind === 'mix') edit(setField(text, mix.line, 2, String(mixHead.gain))); }}>
                                            Reset
                                        </button>
                                    </td>
                                </tr>
                            ),
                            ...own.map((r) => {
                                const head = committed.get(keyOf(r));
                                const h = head?.kind === 'part' ? head : undefined;
                                const dbChanged = !h || h.db !== r.db || h.dbSpread !== r.dbSpread;
                                const atChanged = !h || h.at !== r.at || h.atSpread !== r.atSpread;
                                const how = r.role === 'tight' ? 'tight' : r.role === 'prelude' ? (strokes[cue] ?? 3) : 'main';
                                return (
                                    <tr key={keyOf(r)} data-tuner-row={keyOf(r)}>
                                        <td style={{ ...td, paddingLeft: 18 }}>
                                            {r.what}
                                            {(dbChanged || atChanged) && <span title="changed against HEAD" style={{ color: changedColor }}> ●</span>}
                                        </td>
                                        <td style={td}>
                                            <Stepper value={r.db} step={1} places={2} label={`${r.what} dB`} changed={dbChanged}
                                                     onChange={(v) => edit(setField(text, r.line, 6, num(v, 2)))} />
                                            {r.dbSpread > 0 && <span style={{ opacity: 0.6, fontSize: 12 }}> to {num(r.db - r.dbSpread, 2)}</span>}
                                        </td>
                                        <td style={td}>
                                            <Stepper value={r.at} step={10} places={1} label={`${r.what} ms`} changed={atChanged}
                                                     onChange={(v) => edit(setField(text, r.line, 4, num(v, 1)))} />
                                            {r.atSpread > 0 && <span style={{ opacity: 0.6, fontSize: 12 }}> to {num(r.at - r.atSpread, 1)}</span>}
                                        </td>
                                        <td style={td}>
                                            <span style={{ display: 'inline-flex', gap: 4, alignItems: 'center' }}>
                                                <button type="button" style={playBtn} onClick={() => playRow(cue, how)}
                                                        title={r.role === 'tight' ? 'as a move that opens its run plays it: at once' : undefined}>
                                                    {r.role === 'tight' ? 'Now' : 'Play'}
                                                </button>
                                                {r.role === 'prelude' && perStroke(kind) > 0 && (
                                                    <select style={{ ...small, minWidth: 0 }} value={strokes[cue] ?? 3} aria-label="strokes"
                                                            onChange={(e) => setStrokes({ ...strokes, [cue]: Number(e.target.value) })}>
                                                        {[1, 2, 3, 4, 5].map((n) => <option key={n} value={n}>{n} strokes</option>)}
                                                    </select>
                                                )}
                                            </span>
                                        </td>
                                        <td style={td}>
                                            <button type="button" style={{ ...small, visibility: h && (dbChanged || atChanged) ? 'visible' : 'hidden' }}
                                                    onClick={() => {
                                                        if (!h) return;
                                                        let next = setField(text, r.line, 4, num(h.at, 1));
                                                        next = setField(next, r.line, 5, num(h.atSpread, 1));
                                                        next = setField(next, r.line, 6, num(h.db, 2));
                                                        edit(setField(next, r.line, 7, num(h.dbSpread, 2)));
                                                    }}>
                                                Reset
                                            </button>
                                        </td>
                                    </tr>
                                );
                            }),
                        ];
                    })}
                </tbody>
            </table>
            </div>
        </section>
    );
}
