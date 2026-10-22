'use client';

// The bot league: every bot's rating after every rated game (one canvas, 49 lines), the standings
// and the strategy families. Every number is the game server's (wire.h be_w_bot, be_w_result: the
// finished game's rating stamp and the leaderboard); this file only draws them.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { ago, int, type WBot, type WResult } from '../api';

// ---- the families --------------------------------------------------------------------------------
// One hue per strategy family (bot_seed.c: seven of each), in the server's ladder order.
export const FAMILY_ORDER = ['random', 'handwritten', 'robusta', 'firecracker', 'blackpowder', 'cordite', 'octogen'];
const FAMILY_COLOR: Record<string, string> = {
  random: '#9aa3b2',
  handwritten: '#2f6fe4',
  robusta: '#9a6230',
  firecracker: '#e8590c',
  blackpowder: '#374151',
  cordite: '#1a9a5a',
  octogen: '#8b3fc9',
};
export function familyColor(key: string): string {
  return FAMILY_COLOR[key] ?? '#6b7280';
}
export function botName(b: WBot): string {
  return b.name.replace(/^%/, '');
}

const FLASH_MS = 6000;

// ---- the series ----------------------------------------------------------------------------------
// Each bot's line: its rating before its first league game at x 0, then after each of its games at
// that game's sequence number, then held to the latest game.
export interface Series {
  pts: Float64Array; // x0, y0, x1, y1, ...
  n: number;
}

export function buildSeries(bots: WBot[], results: WResult[]): Series[] {
  const out: number[][] = bots.map((b) => [0, b.startRating]);
  for (const r of results) {
    for (let s = 0; s < r.bot.length; s++) {
      const line = out[r.bot[s]];
      if (line) line.push(r.seq, r.after[s]);
    }
  }
  return out.map((a) => ({ pts: Float64Array.from(a), n: a.length / 2 }));
}

// ---- the chart -----------------------------------------------------------------------------------
interface ChartProps {
  bots: WBot[];
  results: WResult[];
  series: Series[];
  flash: Map<number, number>; // bot index -> when its last game landed (performance.now)
  focus: number | null; // a bot picked in the standings
  hidden: Set<string>; // families switched off
  onHover: (bot: number | null) => void;
}

const PAD = { l: 44, r: 150, t: 12, b: 26 };

function niceStep(span: number, target: number): number {
  const raw = span / Math.max(1, target);
  const p = Math.pow(10, Math.floor(Math.log10(raw)));
  const m = raw / p;
  return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p;
}

export function EloChart({ bots, results, series, flash, focus, hidden, onHover }: ChartProps) {
  const wrap = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  const [size, setSize] = useState({ w: 800, h: 420 });
  const [hover, setHover] = useState<{
    bot: number;
    x: number;
    y: number;
  } | null>(null);
  const [tick, setTick] = useState(0);

  useEffect(() => {
    const el = wrap.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setSize({ w: el.clientWidth, h: el.clientHeight }));
    ro.observe(el);
    setSize({ w: el.clientWidth, h: el.clientHeight });
    return () => ro.disconnect();
  }, []);

  // While a game's lines are lit, redraw each frame so the light fades.
  useEffect(() => {
    let raf = 0;
    const loop = () => {
      const now = performance.now();
      let live = false;
      for (const at of flash.values()) if (now - at < FLASH_MS) live = true;
      setTick((t) => t + 1);
      if (live) raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(raf);
  }, [flash]);

  const lastSeq = results.length ? results[results.length - 1].seq : 0;
  const view = useMemo(() => {
    let lo = Infinity,
      hi = -Infinity;
    for (let i = 0; i < series.length; i++) {
      if (hidden.has(bots[i]?.key ?? '')) continue;
      const p = series[i].pts;
      for (let k = 1; k < p.length; k += 2) {
        if (p[k] < lo) lo = p[k];
        if (p[k] > hi) hi = p[k];
      }
    }
    if (!isFinite(lo)) {
      lo = 900;
      hi = 1100;
    }
    if (hi - lo < 40) {
      const mid = (hi + lo) / 2;
      lo = mid - 20;
      hi = mid + 20;
    }
    const pad = (hi - lo) * 0.06;
    return { x1: Math.max(1, lastSeq), y0: lo - pad, y1: hi + pad };
  }, [series, bots, hidden, lastSeq]);

  const toX = useCallback((x: number) => PAD.l + (x / view.x1) * (size.w - PAD.l - PAD.r), [view, size]);
  const toY = useCallback((y: number) => PAD.t + (1 - (y - view.y0) / (view.y1 - view.y0)) * (size.h - PAD.t - PAD.b), [view, size]);

  useEffect(() => {
    const c = canvas.current;
    if (!c) return;
    const dpr = window.devicePixelRatio || 1;
    if (c.width !== Math.round(size.w * dpr) || c.height !== Math.round(size.h * dpr)) {
      c.width = Math.round(size.w * dpr);
      c.height = Math.round(size.h * dpr);
    }
    const g = c.getContext('2d');
    if (!g) return;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, size.w, size.h);
    const css = getComputedStyle(c);
    const gridColor = css.getPropertyValue('--chart-grid').trim() || '#eef0f3';
    const axisText = css.getPropertyValue('--chart-axis').trim() || '#6b7280';
    const right = size.w - PAD.r;
    // The grid: ratings every nice step, games along the bottom.
    g.font = '11px ui-monospace, SFMono-Regular, Menlo, monospace';
    g.textBaseline = 'middle';
    const ys = niceStep(view.y1 - view.y0, 6);
    g.strokeStyle = gridColor;
    g.lineWidth = 1;
    g.fillStyle = axisText;
    g.textAlign = 'right';
    for (let y = Math.ceil(view.y0 / ys) * ys; y <= view.y1; y += ys) {
      const py = Math.round(toY(y)) + 0.5;
      g.beginPath();
      g.moveTo(PAD.l, py);
      g.lineTo(right, py);
      g.stroke();
      g.fillText(String(Math.round(y)), PAD.l - 8, py);
    }
    const xs = niceStep(view.x1, 8);
    g.textAlign = 'center';
    g.textBaseline = 'top';
    for (let x = 0; x <= view.x1; x += xs) g.fillText(String(x), toX(x), size.h - PAD.b + 8);
    // The lines: dim ones first, the lit and focused last, so they sit on top.
    const now = performance.now();
    const order: number[] = [];
    const strong: number[] = [];
    for (let i = 0; i < series.length; i++) {
      if (hidden.has(bots[i]?.key ?? '')) continue;
      const lit = (flash.get(i) ?? -1e9) > now - FLASH_MS;
      if (lit || i === focus || i === hover?.bot) strong.push(i);
      else order.push(i);
    }
    const anyFocus = focus !== null || hover !== null;
    const drawLine = (i: number, alpha: number, width: number) => {
      const s = series[i];
      const b = bots[i];
      if (!s || !b) return;
      g.globalAlpha = alpha;
      g.strokeStyle = familyColor(b.key);
      g.lineWidth = width;
      g.lineJoin = 'round';
      g.beginPath();
      g.moveTo(toX(s.pts[0]), toY(s.pts[1]));
      for (let k = 2; k < s.pts.length; k += 2) g.lineTo(toX(s.pts[k]), toY(s.pts[k + 1]));
      g.lineTo(toX(view.x1), toY(s.pts[s.pts.length - 1]));
      g.stroke();
    };
    for (const i of order) drawLine(i, anyFocus ? 0.18 : 0.55, 1.25);
    for (const i of strong) {
      const at = flash.get(i) ?? -1e9;
      const fade = Math.max(0, 1 - (now - at) / FLASH_MS);
      const sel = i === focus || i === hover?.bot;
      drawLine(i, 1, sel ? 2.6 : 1.4 + 1.4 * fade);
    }
    g.globalAlpha = 1;
    // Each line's end: a dot at the right edge, a halo while its last game is fresh.
    for (let i = 0; i < series.length; i++) {
      if (hidden.has(bots[i]?.key ?? '')) continue;
      const s = series[i];
      const y = toY(s.pts[s.pts.length - 1]);
      const at = flash.get(i) ?? -1e9;
      const fade = Math.max(0, 1 - (now - at) / FLASH_MS);
      const sel = i === focus || i === hover?.bot;
      if (fade > 0) {
        g.globalAlpha = 0.25 * fade;
        g.fillStyle = familyColor(bots[i].key);
        g.beginPath();
        g.arc(right, y, 4 + 8 * (1 - fade), 0, Math.PI * 2);
        g.fill();
      }
      g.globalAlpha = anyFocus && !sel && fade === 0 ? 0.3 : 1;
      g.fillStyle = familyColor(bots[i].key);
      g.beginPath();
      g.arc(right, y, sel || fade > 0 ? 3.2 : 2, 0, Math.PI * 2);
      g.fill();
    }
    g.globalAlpha = 1;
    // Labels at the right edge for the lit and focused lines, kept apart vertically.
    const labels = strong.map((i) => ({ i, y: toY(series[i].pts[series[i].pts.length - 1]) })).sort((a, b) => a.y - b.y);
    let lastY = -1e9;
    g.textAlign = 'left';
    g.textBaseline = 'middle';
    g.font = '600 11px -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif';
    for (const l of labels) {
      const y = Math.max(l.y, lastY + 13);
      lastY = y;
      const b = bots[l.i];
      const d = b.lastDelta;
      g.fillStyle = familyColor(b.key);
      g.fillText(`${botName(b)} ${b.rating}`, right + 8, y);
      if (d !== 0 && (flash.get(l.i) ?? -1e9) > now - FLASH_MS) {
        const w = g.measureText(`${botName(b)} ${b.rating} `).width;
        g.fillStyle = d > 0 ? '#107a46' : '#b42318';
        g.fillText(d > 0 ? `+${d}` : String(d), right + 8 + w, y);
      }
    }
  }, [size, view, series, bots, flash, focus, hover, hidden, toX, toY, tick]);

  const onMove = (e: React.MouseEvent) => {
    const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
    const mx = e.clientX - rect.left,
      my = e.clientY - rect.top;
    if (mx < PAD.l || mx > size.w - PAD.r + 4) {
      setHover(null);
      onHover(null);
      return;
    }
    const x = ((mx - PAD.l) / (size.w - PAD.l - PAD.r)) * view.x1;
    // The line nearest the pointer at this x: each bot's rating there (its last game at or before x).
    let best = -1,
      bestD = 14;
    for (let i = 0; i < series.length; i++) {
      if (hidden.has(bots[i]?.key ?? '')) continue;
      const p = series[i].pts;
      let y = p[1];
      for (let k = 0; k < p.length && p[k] <= x; k += 2) y = p[k + 1];
      const d = Math.abs(toY(y) - my);
      if (d < bestD) {
        bestD = d;
        best = i;
      }
    }
    const h = best >= 0 ? { bot: best, x: mx, y: my } : null;
    setHover(h);
    onHover(h ? h.bot : null);
  };

  const hb = hover ? bots[hover.bot] : null;
  return (
    <div
      className="chart-wrap"
      ref={wrap}
      onMouseMove={onMove}
      onMouseLeave={() => {
        setHover(null);
        onHover(null);
      }}
    >
      <canvas ref={canvas} style={{ width: size.w, height: size.h }} aria-label="every bot's rating after every rated league game" />
      {hb && hover && (
        <div
          className="chart-tip"
          style={{
            left: Math.min(hover.x + 12, size.w - 190),
            top: Math.max(4, hover.y - 44),
          }}
        >
          <span className="fam-dot" style={{ background: familyColor(hb.key) }} />
          <b>{botName(hb)}</b> <span className="muted">{hb.key}</span>
          <div className="num">
            {hb.rating} now &middot; {hb.leagueGames} games &middot; fool {hb.fools}
          </div>
        </div>
      )}
      {results.length === 0 && <div className="chart-empty">Waiting for the first rated game...</div>}
    </div>
  );
}

// ---- the standings -------------------------------------------------------------------------------
interface StandingsProps {
  bots: WBot[];
  flash: Map<number, number>;
  focus: number | null;
  hover: number | null;
  onFocus: (bot: number | null) => void;
  now: number;
}

export function Standings({ bots, flash, focus, hover, onFocus, now }: StandingsProps) {
  const rows = useMemo(() => bots.map((b, i) => ({ b, i })).sort((x, y) => y.b.rating - x.b.rating || x.i - y.i), [bots]);
  const t = performance.now();
  return (
    <section className="card standings-card">
      <div className="standings-head">
        <h2 className="card-title">Standings</h2>
        <span className="faint num">{bots.length} bots</span>
      </div>
      <div className="standings-cols label">
        <span>#</span>
        <span>Bot</span>
        <span className="r">Rating</span>
        <span className="r">Last</span>
        <span className="r">Games</span>
        <span className="r" title="share of its league games it was the fool of">
          Fool
        </span>
      </div>
      <div className="standings-list">
        {rows.map(({ b, i }, rank) => {
          const lit = (flash.get(i) ?? -1e9) > t - FLASH_MS;
          const cls = `standing${lit ? (b.lastDelta >= 0 ? ' up' : ' down') : ''}${focus === i || hover === i ? ' on' : ''}`;
          const foolPct = b.leagueGames ? Math.round((100 * b.fools) / b.leagueGames) : null;
          return (
            <button
              type="button"
              key={b.id}
              className={cls}
              onClick={() => onFocus(focus === i ? null : i)}
              title={`${botName(b)} (${b.key}): ${b.games} rated games on the server, ${b.firsts} times first out in the league`}
            >
              <span className="num faint">{rank + 1}</span>
              <span className="standing-name">
                <span className="fam-dot" style={{ background: familyColor(b.key) }} />
                <span className="standing-text">{botName(b)}</span>
                {b.seated ? <span className="playing-dot" title="at a table now" /> : null}
              </span>
              <span className="num r b">{b.rating}</span>
              <span
                className={`num r ${b.lastDelta > 0 ? 'pos' : b.lastDelta < 0 ? 'neg' : 'faint'}`}
                title={b.lastMs ? `${ago(Number(b.lastMs), now)} ago` : ''}
              >
                {b.leagueGames ? (b.lastDelta > 0 ? `+${b.lastDelta}` : String(b.lastDelta)) : '-'}
              </span>
              <span className="num r">{int(b.leagueGames)}</span>
              <span className="num r muted">{foolPct === null ? '-' : `${foolPct}%`}</span>
            </button>
          );
        })}
      </div>
    </section>
  );
}

// ---- the families --------------------------------------------------------------------------------
interface FamiliesProps {
  bots: WBot[];
  hidden: Set<string>;
  onToggle: (key: string) => void;
}

export function Families({ bots, hidden, onToggle }: FamiliesProps) {
  const fams = useMemo(() => {
    const m = new Map<string, { n: number; sum: number; games: number; fools: number; best: number }>();
    for (const b of bots) {
      const f = m.get(b.key) ?? { n: 0, sum: 0, games: 0, fools: 0, best: 0 };
      f.n++;
      f.sum += b.rating;
      f.games += b.leagueGames;
      f.fools += b.fools;
      f.best = Math.max(f.best, b.rating);
      m.set(b.key, f);
    }
    const keys = [...m.keys()].sort((a, b) => {
      const ia = FAMILY_ORDER.indexOf(a),
        ib = FAMILY_ORDER.indexOf(b);
      return (ia < 0 ? 99 : ia) - (ib < 0 ? 99 : ib) || a.localeCompare(b);
    });
    return keys.map((k) => ({ key: k, ...m.get(k)! }));
  }, [bots]);
  return (
    <div className="families">
      {fams.map((f) => {
        const avg = f.sum / f.n;
        const off = hidden.has(f.key);
        return (
          <button
            type="button"
            key={f.key}
            className={`family${off ? ' off' : ''}`}
            onClick={() => onToggle(f.key)}
            title={off ? 'show this family on the chart' : `${f.key}: average rating of its ${f.n} bots (click to hide its lines)`}
          >
            <span className="family-name">
              <span className="fam-dot" style={{ background: familyColor(f.key) }} />
              {f.key}
            </span>
            <span className="num family-avg">{Math.round(avg)}</span>
            <span className="num faint family-games" title="the share of the family's league games one of its bots was the fool of">
              {f.games ? `${Math.round((100 * f.fools) / f.games)}% fool` : ''}
            </span>
          </button>
        );
      })}
    </div>
  );
}
