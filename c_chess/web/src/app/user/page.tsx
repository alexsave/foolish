'use client';
// A profile: the rating, the record, the rating's history as a graph, and the recent games.
import { Suspense, useEffect, useState } from 'react';
import Link from 'next/link';
import { useSearchParams } from 'next/navigation';
import type { CvGames_Snap, CvProfile_Snap } from '@/gen/web_layout';
import { net } from '@/lib/net';
import { tc } from '@/lib/use';
import { score } from '@/lib/words';

function Graph({ p }: { p: CvProfile_Snap }) {
  const pts = p.points.slice(0, p.n);
  if (pts.length < 2) return <p className="muted">The graph starts after two rated games.</p>;
  const W = 520, H = 220, L = 40, B = 20;
  const rs = pts.map((x) => x.rating);
  const lo = Math.floor((Math.min(...rs) - 20) / 50) * 50, hi = Math.ceil((Math.max(...rs) + 20) / 50) * 50;
  const t0 = pts[0].at, t1 = pts[pts.length - 1].at;
  const x = (t: number) => L + ((W - L - 8) * (t - t0)) / Math.max(1, t1 - t0);
  const y = (r: number) => 8 + ((H - B - 8) * (hi - r)) / Math.max(1, hi - lo);
  const d = pts.map((q, i) => `${i ? 'L' : 'M'}${x(q.at).toFixed(1)},${y(q.rating).toFixed(1)}`).join('');
  const ticks = [];
  for (let r = lo; r <= hi; r += Math.max(50, Math.ceil((hi - lo) / 4 / 50) * 50)) ticks.push(r);
  return (
    <svg className="graph" viewBox={`0 0 ${W} ${H}`} role="img" aria-label="rating history">
      {ticks.map((r) => (
        <g key={r}><line className="grid" x1={L} x2={W} y1={y(r)} y2={y(r)} /><text x={4} y={y(r) + 4}>{r}</text></g>
      ))}
      <path d={d} />
    </svg>
  );
}

function User() {
  const name = useSearchParams().get('name') ?? '';
  const [p, setP] = useState<CvProfile_Snap | null>(null);
  const [g, setG] = useState<CvGames_Snap | null>(null);
  const [missing, setMissing] = useState(false);
  useEffect(() => {
    if (!name) return;
    let gone = false;
    (async () => {
      const a = await net.read(`/c/user/${encodeURIComponent(name)}`);
      if (gone) return;
      if (a.status !== 200) { setMissing(true); return; }
      setP(net.k!.profile(a.body));
      const b = await net.read(`/c/games/${encodeURIComponent(name)}`);
      if (!gone && b.status === 200) setG(net.k!.games(b.body));
    })();
    return () => { gone = true; };
  }, [name]);
  if (missing) return <p className="muted">No player called {name}.</p>;
  if (!p) return <p className="muted">Loading...</p>;
  return (
    <div className="prof">
      <section className="panel" style={{ padding: 20 }}>
        <h1>{p.name}</h1>
        <div className="row" style={{ gap: 28, alignItems: 'baseline' }}>
          <span className="big">{p.rating}{p.rd >= 110 ? '?' : ''}</span>
          <span className="muted">{p.games} games: {p.wins} won, {p.losses} lost, {p.draws} drawn</span>
        </div>
        <Graph p={p} />
      </section>
      <section className="panel">
        <table className="seeks">
          <thead><tr><th>Opponent</th><th>Time</th><th>Result</th><th>Rating</th></tr></thead>
          <tbody>
            {(g?.rows ?? []).slice(0, g?.n ?? 0).map((r) => {
              const won = r.winner === r.color, drawn = r.winner === 2;
              return (
                <tr key={r.game} className="click" onClick={() => (location.href = `/game?id=${r.game}`)}>
                  <td><Link prefetch={false} href={`/user?name=${encodeURIComponent(r.opponent)}`} onClick={(e) => e.stopPropagation()}>{r.opponent}</Link></td>
                  <td>{tc(r.baseS, r.incS)}{r.rated ? '' : ' casual'}</td>
                  <td className={drawn ? '' : won ? 'up' : 'down'}>{score(r.status, r.winner)}</td>
                  <td className={r.diff > 0 ? 'up' : r.diff < 0 ? 'down' : 'muted'}>{r.diff > 0 ? '+' : ''}{r.rated ? r.diff : ''}</td>
                </tr>
              );
            })}
            {!g?.n && <tr><td colSpan={4} className="muted" style={{ padding: 24, textAlign: 'center' }}>No finished games yet.</td></tr>}
          </tbody>
        </table>
      </section>
    </div>
  );
}

export default function Page() { return <Suspense><User /></Suspense>; }
